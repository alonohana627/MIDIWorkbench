// Playback snapshot, scheduler and transport tests (specification sections 13, 20.3, 34.3, 34.5).
// All timing runs on a synthetic frame clock; nothing sleeps.

#include "test_support.hpp"

#include "mwb/core/change_set.hpp"
#include "mwb/core/model.hpp"
#include "mwb/core/random.hpp"
#include "mwb/playback/handoff.hpp"
#include "mwb/playback/scheduler.hpp"
#include "mwb/playback/snapshot.hpp"
#include "mwb/playback/transport.hpp"

#include <algorithm>
#include <map>
#include <cmath>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

using namespace mwb;
using namespace mwb::playback;

namespace {

/// One event as received by the recording sink, with its absolute frame.
struct Received {
    std::int64_t absolute = 0;
    RealtimeMidiEvent event;
};

/// Sink that records events with their absolute frame (block start + offset).
class RecordingSink final : public IEventSink {
public:
    void on_event(const RealtimeMidiEvent& event) noexcept override {
        events.push_back(Received{block_start + event.frame_offset, event});
    }
    std::int64_t block_start = 0;
    std::vector<Received> events;
};

/// Sink that discards events.
class NullSink final : public IEventSink {
public:
    void on_event(const RealtimeMidiEvent&) noexcept override {}
};

/// Sink that tracks sounding notes per (route, channel, pitch) and fails on negative counts.
class BalanceSink final : public IEventSink {
public:
    void on_event(const RealtimeMidiEvent& event) noexcept override {
        if (event.kind != RealtimeEventKind::midi)
            return;
        const auto key = std::make_tuple(event.route, event.channel(), event.data1());
        if (event.message() == 0x90 && event.data2() > 0)
            ++sounding[key];
        else if (event.message() == 0x80 || (event.message() == 0x90 && event.data2() == 0)) {
            if (--sounding[key] < 0)
                negative = true;
        }
        if (event.status() == 0xFF)
            system_reset = true;
    }
    [[nodiscard]] long total() const {
        long sum = 0;
        for (const auto& [key, count] : sounding)
            sum += count;
        return sum;
    }
    std::map<std::tuple<std::uint32_t, std::uint8_t, std::uint8_t>, long> sounding;
    bool negative = false;
    bool system_reset = false;
};

/// Runs `blocks` blocks of `block_size` frames, keeping the sink's block start current.
void run_blocks(Scheduler& scheduler, RecordingSink& sink, std::uint32_t block_size, std::size_t blocks) {
    for (std::size_t index = 0; index < blocks; ++index) {
        sink.block_start = scheduler.absolute_frame();
        std::ignore = scheduler.process_block(block_size, sink); // Results are not needed here.
    }
}

/// Compiles with default options plus a sample rate.
std::unique_ptr<const PlaybackSnapshot> compile(const DocumentData& document, PlaybackOptions options = {}) {
    auto snapshot = compile_playback(document, options);
    REQUIRE(snapshot);
    return std::move(*snapshot);
}

TrackId first_track(const DocumentData& document) { return document.sequences.front().tracks.front().id; }

/// Adds a track to the first sequence and returns its identity.
TrackId add_track(DocumentData& document, const std::string& name) {
    Track track;
    track.id = TrackId::generate();
    track.name = name;
    track.end_tick = document.sequences.front().end_tick;
    document.sequences.front().tracks.push_back(track);
    return track.id;
}

/// Inserts an event built by `make` into `track`.
void insert_event(DocumentData& document, TrackId track, Event event) {
    ChangeBuilder builder(document, "insert");
    builder.insert(track, std::move(event));
    REQUIRE(apply_change_set(document, builder.finish()));
}

void add_note(DocumentData& document, TrackId track, Tick start, Tick end, std::uint8_t pitch,
              std::uint8_t channel = 0, std::uint8_t velocity = 100) {
    ChangeBuilder builder(document, "note");
    builder.add_note(track, start, end, channel, pitch, velocity);
    REQUIRE(apply_change_set(document, builder.finish()));
}

std::size_t count_notes_on(const std::vector<Received>& events) {
    return static_cast<std::size_t>(std::ranges::count_if(events, [](const Received& received) {
        return received.event.kind == RealtimeEventKind::midi && received.event.message() == 0x90;
    }));
}

} // namespace

TEST_SUITE("playback") {
    TEST_CASE("solo and mute follow the section 12 rule") {
        DocumentData document = make_blank_document();
        const TrackId melody = first_track(document);
        const TrackId bass = add_track(document, "Bass");
        const TrackId drums = add_track(document, "Drums");
        add_note(document, melody, 0, 480, 60, 0);
        add_note(document, bass, 0, 480, 40, 1);
        add_note(document, drums, 0, 480, 36, 9);
        Sequence& sequence = document.sequences.front();

        CHECK(audible_tracks(sequence, false) == std::vector<bool>{true, true, true});
        sequence.tracks[1].muted = true;
        CHECK(audible_tracks(sequence, false) == std::vector<bool>{true, false, true});
        sequence.tracks[2].solo = true;
        CHECK(audible_tracks(sequence, false) == std::vector<bool>{false, false, true});
        sequence.tracks[2].muted = true; // A muted solo track stays silent; others remain unsoloed.
        CHECK(audible_tracks(sequence, false) == std::vector<bool>{false, false, false});
        sequence.tracks[2].muted = false;
        sequence.tracks[1].solo = true; // Soloed but muted: still silent.
        CHECK(audible_tracks(sequence, false) == std::vector<bool>{false, false, true});
        CHECK(audible_tracks(sequence, true) == std::vector<bool>{false, false, false});

        const auto snapshot = compile(document);
        CHECK(snapshot->statistics.audible_tracks == 1);
        REQUIRE(snapshot->notes.size() == 1);
        CHECK(snapshot->notes.front().pitch == 36);
        CHECK(snapshot->routes[snapshot->notes.front().route].drum);

        PlaybackOptions render_all;
        render_all.include_muted_tracks = true;
        CHECK(compile(document, render_all)->notes.size() == 3);
    }

    TEST_CASE("routing applies channel override and playback-only transposition") {
        DocumentData document = make_blank_document();
        const TrackId track = first_track(document);
        add_note(document, track, 0, 480, 60, 2);
        document.sequences.front().tracks.front().routing.channel_override = 5;
        document.sequences.front().tracks.front().routing.transpose = 3;
        PlaybackOptions options;
        options.session.preview_transpose = 2;
        const auto snapshot = compile(document, options);
        REQUIRE(snapshot->notes.size() == 1);
        CHECK(snapshot->notes[0].pitch == 65);
        CHECK(snapshot->notes[0].channel == 5);
        CHECK(snapshot->routes.size() == 1);
        CHECK(snapshot->routes[0].channel == 5);
        // The document itself is unchanged.
        const auto& events = document.sequences.front().tracks.front().events;
        CHECK(std::ranges::any_of(events, [](const Event& event) { return event.is_note_start() && event.data1() == 60; }));

        options.session.preview_transpose = 100;
        const auto out_of_range = compile(document, options);
        CHECK(out_of_range->notes.empty());
        CHECK(out_of_range->statistics.notes_out_of_range == 1);
    }

    TEST_CASE("tempo override multiplies PPQ tempo") {
        DocumentData document = make_blank_document();
        add_note(document, first_track(document), 960, 1920, 60);
        PlaybackOptions options;
        options.sample_rate = 48000;
        options.session.tempo_multiplier = Rational{2, 1};
        const auto snapshot = compile(document, options);
        CHECK(snapshot->mapping.frame_at(960) == 12000);
        options.session.tempo_multiplier = Rational{0, 1};
        CHECK_FALSE(compile_playback(document, options));
    }

    TEST_CASE("real-time preflight refuses pathological density with an explanation") {
        DocumentData document = make_blank_document();
        const TrackId track = first_track(document);
        ChangeBuilder builder(document, "dense");
        for (int index = 0; index < 40; ++index)
            builder.insert(track, events::control_change(0, 0, 1, static_cast<std::uint8_t>(index)));
        REQUIRE(apply_change_set(document, builder.finish()));
        PlaybackOptions options;
        options.max_events_per_block = 16;
        const auto refused = compile(document, options);
        CHECK_FALSE(refused->live_playback_allowed);
        CHECK(refused->live_refusal.find("refused") != std::string::npos);
        CHECK(refused->statistics.peak_events_per_block >= 40);
        CHECK(compile(document)->live_playback_allowed);
    }

    TEST_CASE("snapshot handoff never destroys on the render thread") {
        RetiringHandoff<int> handoff;
        auto first = std::make_shared<const int>(1);
        std::weak_ptr<const int> first_watch = first;
        REQUIRE(handoff.publish(std::move(first)));
        const int* current = handoff.acquire_pending();
        REQUIRE(current != nullptr);
        CHECK(*current == 1);
        CHECK(handoff.acquire_pending() == nullptr);

        auto second = std::make_shared<const int>(2);
        REQUIRE(handoff.publish(std::move(second)));
        const int* next = handoff.acquire_pending();
        REQUIRE(next != nullptr);
        CHECK(*next == 2);
        CHECK_FALSE(first_watch.expired()); // Previous object still valid for the render thread.
        handoff.retire_previous();
        CHECK_FALSE(first_watch.expired()); // Retired, but only the owner destroys it.
        CHECK(handoff.collect_retired() == 1);
        CHECK(first_watch.expired());
        CHECK(handoff.retired_count() == 1);

        // A publication the render thread never picked up is replaced on the owner thread.
        auto skipped = std::make_shared<const int>(3);
        std::weak_ptr<const int> skipped_watch = skipped;
        REQUIRE(handoff.publish(std::move(skipped)));
        REQUIRE(handoff.publish(std::make_shared<const int>(4)));
        CHECK(skipped_watch.expired());
        CHECK(*handoff.acquire_pending() == 4);
        handoff.retire_previous();
        handoff.reset();
    }
}

TEST_SUITE("scheduler") {
    TEST_CASE("golden case 8: a tempo event at an exact block boundary applies to the next block once") {
        DocumentData document = make_blank_document();
        const TrackId track = first_track(document);
        insert_event(document, track, events::tempo(960, 1'000'000));
        add_note(document, track, 1920, 2400, 64);
        PlaybackOptions options;
        options.sample_rate = 48000;
        const auto snapshot = compile(document, options);
        CHECK(snapshot->mapping.frame_at(960) == 24000);
        CHECK(snapshot->mapping.frame_at(1920) == 72000);

        Scheduler scheduler;
        NullSink ignore;
        REQUIRE(scheduler.set_snapshot(snapshot.get(), ignore));
        RecordingSink sink;
        scheduler.start(0, sink);
        sink.events.clear();
        run_blocks(scheduler, sink, 1000, 100);

        std::vector<Received> tempos;
        for (const Received& received : sink.events)
            if (received.event.kind == RealtimeEventKind::tempo)
                tempos.push_back(received);
        REQUIRE(tempos.size() == 2);
        CHECK(tempos[0].absolute == 0);
        CHECK(tempos[1].absolute == 24000);
        CHECK(tempos[1].event.frame_offset == 0); // First frame of the next block, not the end of the previous.
        CHECK(tempos[1].event.value == 1'000'000);
        const auto note_on = std::ranges::find_if(sink.events, [](const Received& received) {
            return received.event.kind == RealtimeEventKind::midi && received.event.message() == 0x90;
        });
        REQUIRE(note_on != sink.events.end());
        CHECK(note_on->absolute == 72000);
    }

    TEST_CASE("every event falls in exactly one block across random block sizes") {
        DocumentData document = make_blank_document();
        const TrackId track = first_track(document);
        DeterministicRandom random(42);
        ChangeBuilder builder(document, "random");
        for (int index = 0; index < 300; ++index) {
            const Tick start = static_cast<Tick>(random.bounded(15000));
            const Tick length = 1 + static_cast<Tick>(random.bounded(900));
            builder.add_note(track, start, start + length, static_cast<std::uint8_t>(random.bounded(16)),
                             static_cast<std::uint8_t>(30 + random.bounded(60)), 90);
            builder.insert(track, events::control_change(static_cast<Tick>(random.bounded(15000)), 0, 74,
                                                         static_cast<std::uint8_t>(random.bounded(128))));
        }
        builder.insert(track, events::tempo(3000, 412345));
        builder.insert(track, events::tempo(7777, 733333));
        REQUIRE(apply_change_set(document, builder.finish()));
        PlaybackOptions options;
        options.sample_rate = 44100;
        const auto snapshot = compile(document, options);

        // Expected stream: every MIDI/tempo event at its snapshot frame (playback starts at 0).
        std::vector<std::pair<std::int64_t, std::array<std::uint8_t, 3>>> expected;
        for (const ScheduledEvent& event : snapshot->events)
            if (event.kind != RealtimeEventKind::metronome_click)
                expected.emplace_back(event.frame, event.kind == RealtimeEventKind::midi
                                                       ? event.bytes
                                                       : std::array<std::uint8_t, 3>{0xFF, 0x51, 0});
        for (int trial = 0; trial < 20; ++trial) {
            Scheduler scheduler;
            NullSink ignore;
            REQUIRE(scheduler.set_snapshot(snapshot.get(), ignore));
            scheduler.start(0, ignore);
            RecordingSink sink;
            std::size_t blocks = 0;
            while (scheduler.absolute_frame() <= snapshot->end_frame + 10) {
                const auto block = static_cast<std::uint32_t>(1 + random.bounded(trial % 2 == 0 ? 64 : 4096));
                sink.block_start = scheduler.absolute_frame();
                const std::size_t before = sink.events.size();
                std::ignore = scheduler.process_block(block, sink); // Counted through the sink.
                for (std::size_t index = before; index < sink.events.size(); ++index) {
                    CHECK(sink.events[index].event.frame_offset < block);
                    if (index > before)
                        CHECK(sink.events[index].event.frame_offset >= sink.events[index - 1].event.frame_offset);
                }
                ++blocks;
            }
            std::vector<std::pair<std::int64_t, std::array<std::uint8_t, 3>>> actual;
            for (const Received& received : sink.events)
                actual.emplace_back(received.absolute, received.event.kind == RealtimeEventKind::midi
                                                           ? received.event.bytes
                                                           : std::array<std::uint8_t, 3>{0xFF, 0x51, 0});
            REQUIRE(actual.size() == expected.size());
            CHECK(actual == expected);
            CHECK(scheduler.owned_notes().empty());
            CHECK(blocks > 0);
        }
    }

    TEST_CASE("loop long-run drift is zero frames") {
        DocumentData document = make_blank_document();
        const TrackId track = first_track(document);
        add_note(document, track, 0, 100, 60);
        PlaybackOptions options;
        options.sample_rate = 44100;
        options.session.loop = TimeRange{0, 1000}; // 1000 ticks = 22968.75 frames at 120 BPM.
        const auto snapshot = compile(document, options);
        REQUIRE(snapshot->loop);
        CHECK(static_cast<double>(snapshot->loop->length_numerator) /
                  static_cast<double>(snapshot->loop->length_denominator) ==
              doctest::Approx(22968.75));

        Scheduler scheduler;
        NullSink ignore;
        REQUIRE(scheduler.set_snapshot(snapshot.get(), ignore));
        RecordingSink sink;
        scheduler.start(0, sink);
        sink.events.clear();
        constexpr std::int64_t iterations = 4000;
        while (scheduler.loop_iteration() < iterations) {
            sink.block_start = scheduler.absolute_frame();
            std::ignore = scheduler.process_block(512, sink); // Loop wraps are checked through the sink.
            sink.events.erase(std::remove_if(sink.events.begin(), sink.events.end(),
                                             [](const Received& received) {
                                                 return received.event.kind != RealtimeEventKind::midi ||
                                                        received.event.message() != 0x90;
                                             }),
                              sink.events.end());
        }
        // Note-ons at loop start: iteration k must start at exactly round(k * 22968.75).
        REQUIRE(sink.events.size() >= static_cast<std::size_t>(iterations));
        for (std::size_t k = 0; k < sink.events.size(); ++k) {
            const auto ideal = static_cast<std::int64_t>((static_cast<__int128>(k) * 9187500 * 2 + 400) / 800);
            if (sink.events[k].absolute != ideal) {
                FAIL("iteration " << k << " started at " << sink.events[k].absolute << " instead of " << ideal);
            }
        }
        const std::int64_t last = sink.events.back().absolute;
        const auto k = static_cast<std::int64_t>(sink.events.size() - 1);
        CHECK(last - (k * 22968 + (k * 3 + 2) / 4) == 0);
        CHECK(scheduler.owned_notes().size() <= 1);
    }

    TEST_CASE("loop end cuts owned notes, clears sustain and restores state at loop start") {
        DocumentData document = make_blank_document();
        const TrackId track = first_track(document);
        add_note(document, track, 500, 5000, 72); // Crosses the loop end.
        insert_event(document, track, events::control_change(100, 0, 7, 40));
        insert_event(document, track, events::control_change(600, 0, 64, 127));
        insert_event(document, track, events::control_change(700, 0, 7, 90));
        PlaybackOptions options;
        options.session.loop = TimeRange{0, 960};
        const auto snapshot = compile(document, options);
        Scheduler scheduler;
        BalanceSink balance;
        REQUIRE(scheduler.set_snapshot(snapshot.get(), balance));
        RecordingSink sink;
        scheduler.start(0, sink);
        sink.events.clear();
        const std::int64_t wrap = snapshot->mapping.frame_at(960);
        while (scheduler.loop_iteration() < 1) {
            sink.block_start = scheduler.absolute_frame();
            std::ignore = scheduler.process_block(256, sink); // Checked via recorded events.
        }
        std::vector<Received> at_wrap;
        for (const Received& received : sink.events)
            if (received.absolute == wrap)
                at_wrap.push_back(received);
        auto has = [&](std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
            return std::ranges::any_of(at_wrap, [&](const Received& received) {
                return received.event.status() == status && received.event.data1() == data1 &&
                       received.event.data2() == data2;
            });
        };
        CHECK(has(0x80, 72, 0));   // Owned note cut.
        CHECK(has(0xB0, 64, 0));   // Sustain cleared.
        CHECK(has(0xB0, 7, 100));  // State at loop start: volume back to its default.
        CHECK(scheduler.owned_notes().empty());
    }

    TEST_CASE("state chase restores program, controllers and bend at a seek target") {
        DocumentData document = make_blank_document();
        const TrackId track = first_track(document);
        insert_event(document, track, events::program_change(0, 0, 5));
        insert_event(document, track, events::control_change(100, 0, 7, 30));
        insert_event(document, track, events::pitch_bend(200, 0, 12000));
        insert_event(document, track, events::control_change(1000, 0, 10, 20));
        insert_event(document, track, events::control_change(1000, 0, 7, 90));
        add_note(document, track, 300, 2000, 60);
        const auto snapshot = compile(document);
        Scheduler scheduler;
        NullSink ignore;
        REQUIRE(scheduler.set_snapshot(snapshot.get(), ignore));

        RecordingSink sink;
        scheduler.start(500, sink);
        auto find = [&](std::uint8_t status, std::uint8_t data1) -> const Received* {
            for (const Received& received : sink.events)
                if (received.event.status() == status && received.event.data1() == data1)
                    return &received;
            return nullptr;
        };
        const auto* program = find(0xC0, 5);
        REQUIRE(program != nullptr);
        const auto* volume = find(0xB0, 7);
        REQUIRE(volume != nullptr);
        CHECK(volume->event.data2() == 30);
        CHECK(std::ranges::any_of(sink.events, [](const Received& received) {
            return received.event.message() == 0xE0 && (received.event.data1() | (received.event.data2() << 7)) == 12000;
        }));
        // State chase does not restart the note that began at tick 300.
        CHECK(count_notes_on(sink.events) == 0);
        CHECK(find(0xB0, 10) == nullptr); // Never set before tick 500.

        // Play past tick 1000, then seek back: CC7 returns to 30 and CC10 to its default.
        run_blocks(scheduler, sink, 4800, 10);
        sink.events.clear();
        const std::uint64_t epoch = scheduler.epoch();
        scheduler.seek(500, sink);
        CHECK(scheduler.epoch() == epoch + 1);
        const auto* restored_volume = find(0xB0, 7);
        REQUIRE(restored_volume != nullptr);
        CHECK(restored_volume->event.data2() == 30);
        const auto* restored_pan = find(0xB0, 10);
        REQUIRE(restored_pan != nullptr);
        CHECK(restored_pan->event.data2() == 64);
    }

    TEST_CASE("held-note chase restarts notes crossing the target and is labeled") {
        DocumentData document = make_blank_document();
        add_note(document, first_track(document), 300, 2000, 60);
        const auto snapshot = compile(document);
        SchedulerSettings settings;
        settings.chase = ChaseMode::state_and_held_notes;
        Scheduler scheduler(settings);
        NullSink ignore;
        REQUIRE(scheduler.set_snapshot(snapshot.get(), ignore));
        RecordingSink sink;
        scheduler.start(500, sink);
        CHECK(count_notes_on(sink.events) == 1);
        CHECK(scheduler.owned_notes().size() == 1);
        CHECK(std::string(chase_mode_label(ChaseMode::state_and_held_notes)).find("envelope") != std::string::npos);
        scheduler.stop(sink);
        CHECK(scheduler.owned_notes().empty());
    }

    TEST_CASE("seek, stop, loop, panic and snapshot swap stress leaves no owned notes") {
        DocumentData document = make_blank_document();
        const TrackId track = first_track(document);
        DeterministicRandom random(7);
        ChangeBuilder builder(document, "stress");
        for (int index = 0; index < 400; ++index) {
            const Tick start = static_cast<Tick>(random.bounded(15000));
            builder.add_note(track, start, start + 1 + static_cast<Tick>(random.bounded(3000)),
                             static_cast<std::uint8_t>(random.bounded(4)),
                             static_cast<std::uint8_t>(36 + random.bounded(48)), 100);
            if (index % 10 == 0)
                builder.insert(track, events::control_change(start, 0, 64, index % 20 == 0 ? 127 : 0));
        }
        REQUIRE(apply_change_set(document, builder.finish()));
        PlaybackOptions looped;
        looped.session.loop = TimeRange{1000, 9000};
        const auto plain = compile(document);
        const auto with_loop = compile(document, looped);
        document.sequences.front().tracks.front().routing.transpose = 1; // Changes every owner's pitch.
        const auto transposed_snapshot = compile(document);

        Scheduler scheduler;
        BalanceSink balance;
        REQUIRE(scheduler.set_snapshot(plain.get(), balance));
        std::size_t transitions = 0;
        for (int step = 0; step < 5000; ++step) {
            switch (random.bounded(9)) {
            case 0:
                scheduler.start(static_cast<Tick>(random.bounded(16000)), balance, random.bounded(64));
                break;
            case 1:
                scheduler.seek(static_cast<Tick>(random.bounded(16000)), balance);
                break;
            case 2:
                scheduler.stop(balance);
                break;
            case 3: {
                const PlaybackSnapshot* choices[] = {plain.get(), with_loop.get(), transposed_snapshot.get()};
                REQUIRE(scheduler.set_snapshot(choices[random.bounded(3)], balance));
                break;
            }
            case 4:
                if (random.bounded(10) == 0)
                    scheduler.panic(balance);
                break;
            default:
                std::ignore = scheduler.process_block(1 + random.bounded(2048), balance); // Balance checked below.
                break;
            }
            ++transitions;
            CHECK(scheduler.owned_notes().size() <= scheduler.owned_notes().capacity());
            REQUIRE_FALSE(balance.negative);
            CHECK(balance.total() == static_cast<long>(scheduler.owned_notes().size()));
        }
        scheduler.stop(balance);
        CHECK(transitions == 5000);
        CHECK(scheduler.owned_notes().empty());
        CHECK(balance.total() == 0);
        CHECK_FALSE(balance.system_reset);
    }

    TEST_CASE("snapshot swap keeps surviving notes and releases removed ones") {
        DocumentData document = make_blank_document();
        const TrackId track = first_track(document);
        add_note(document, track, 0, 10000, 60);
        add_note(document, track, 0, 10000, 64);
        const auto before = compile(document);
        // Remove the second note.
        ChangeBuilder builder(document, "remove");
        const auto& events = document.sequences.front().tracks.front().events;
        for (const Event& event : events)
            if (event.is_channel() && event.data1() == 64)
                REQUIRE(builder.remove(track, event.id));
        REQUIRE(apply_change_set(document, builder.finish()));
        const auto after = compile(document);

        Scheduler scheduler;
        RecordingSink sink;
        REQUIRE(scheduler.set_snapshot(before.get(), sink));
        scheduler.start(0, sink);
        run_blocks(scheduler, sink, 1024, 4);
        CHECK(scheduler.owned_notes().size() == 2);
        sink.events.clear();
        REQUIRE(scheduler.set_snapshot(after.get(), sink));
        CHECK(scheduler.owned_notes().size() == 1);
        REQUIRE(sink.events.size() == 1);
        CHECK(sink.events[0].event.message() == 0x80);
        CHECK(sink.events[0].event.data1() == 64);
        // The surviving note ends normally under the new snapshot.
        sink.events.clear();
        run_blocks(scheduler, sink, 4096, 100);
        CHECK(scheduler.owned_notes().empty());
        CHECK(std::ranges::count_if(sink.events, [](const Received& received) {
                  return received.event.message() == 0x80 && received.event.data1() == 60;
              }) == 1);
    }

    TEST_CASE("panic adds all-notes-off, all-sound-off and bend reset but never System Reset") {
        DocumentData document = make_blank_document();
        add_note(document, first_track(document), 0, 5000, 60, 3);
        const auto snapshot = compile(document);
        Scheduler scheduler;
        BalanceSink balance;
        REQUIRE(scheduler.set_snapshot(snapshot.get(), balance));
        scheduler.start(0, balance);
        std::ignore = scheduler.process_block(256, balance); // Starts the note.
        RecordingSink sink;
        scheduler.panic(sink);
        auto has = [&](std::uint8_t status, std::uint8_t data1) {
            return std::ranges::any_of(sink.events, [&](const Received& received) {
                return received.event.status() == status && received.event.data1() == data1;
            });
        };
        CHECK(has(0x83, 60));
        CHECK(has(0xB3, 123));
        CHECK(has(0xB3, 120));
        CHECK(has(0xB3, 64));
        CHECK(has(0xE3, 0));
        CHECK_FALSE(std::ranges::any_of(sink.events, [](const Received& received) { return received.event.status() == 0xFF; }));
        CHECK(scheduler.owned_notes().empty());
        CHECK_FALSE(scheduler.playing());
    }

    TEST_CASE("non-looping playback reports its end and the registry overflow never starts unowned notes") {
        DocumentData document = make_blank_document();
        const TrackId track = first_track(document);
        ChangeBuilder builder(document, "chord");
        for (std::uint8_t pitch = 40; pitch < 60; ++pitch)
            builder.add_note(track, 0, 960, 0, pitch, 100);
        REQUIRE(apply_change_set(document, builder.finish()));
        const auto snapshot = compile(document);
        SchedulerSettings settings;
        settings.owned_note_capacity = 8;
        Scheduler scheduler(settings);
        BalanceSink balance;
        REQUIRE(scheduler.set_snapshot(snapshot.get(), balance));
        scheduler.start(0, balance);
        BlockResult result;
        while (!result.reached_end)
            result = scheduler.process_block(4096, balance);
        CHECK(scheduler.dropped_note_ons() == 12);
        CHECK(balance.total() == 0);
        CHECK(scheduler.owned_notes().empty());
    }
}

TEST_SUITE("transport") {
    TEST_CASE("transport follows the section 13.3 state machine exactly") {
        using State = TransportState;
        using Command = TransportCommand;
        const std::vector<std::tuple<State, Command, State>> edges = {
            {State::stopped, Command::play, State::playing},
            {State::stopped, Command::record_with_count_in, State::counting_in},
            {State::stopped, Command::record, State::recording},
            {State::counting_in, Command::count_in_complete, State::recording},
            {State::playing, Command::pause, State::paused},
            {State::paused, Command::resume, State::playing},
            {State::playing, Command::punch_in, State::recording},
            {State::recording, Command::punch_out, State::playing},
            {State::playing, Command::stop, State::stopped},
            {State::recording, Command::stop, State::stopped},
            {State::counting_in, Command::cancel, State::stopped},
            {State::paused, Command::stop, State::stopped},
            {State::playing, Command::device_failure, State::device_lost},
            {State::recording, Command::device_failure, State::device_lost},
            {State::device_lost, Command::reconfigure, State::stopped},
        };
        const std::vector<State> states = {State::stopped, State::playing, State::paused,
                                           State::counting_in, State::recording, State::device_lost};
        const std::vector<Command> commands = {
            Command::play,     Command::record,         Command::record_with_count_in, Command::count_in_complete,
            Command::pause,    Command::resume,         Command::punch_in,             Command::punch_out,
            Command::stop,     Command::cancel,         Command::device_failure,       Command::reconfigure};
        std::size_t allowed = 0;
        for (State state : states) {
            for (Command command : commands) {
                const auto edge = std::ranges::find_if(edges, [&](const auto& candidate) {
                    return std::get<0>(candidate) == state && std::get<1>(candidate) == command;
                });
                const auto result = Transport::transition(state, command);
                if (edge != edges.end()) {
                    REQUIRE(result);
                    CHECK(*result == std::get<2>(*edge));
                    ++allowed;
                } else {
                    REQUIRE_FALSE(result);
                    CHECK(result.error().code == ErrorCode::conflict);
                }
            }
        }
        CHECK(allowed == edges.size());
    }

    TEST_CASE("seek is accepted in Stopped, Paused and Playing and rejected while recording") {
        Transport transport;
        REQUIRE(transport.seek(100));
        CHECK(transport.epoch() == 1);
        REQUIRE(transport.apply(TransportCommand::play));
        REQUIRE(transport.seek(200));
        REQUIRE(transport.apply(TransportCommand::pause));
        REQUIRE(transport.seek(300));
        CHECK(transport.epoch() == 3);
        REQUIRE(transport.apply(TransportCommand::resume));
        REQUIRE(transport.apply(TransportCommand::punch_in));
        CHECK(transport.state() == TransportState::recording);
        const auto rejected = transport.seek(400);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == ErrorCode::conflict);
        CHECK(transport.position() == 300);
        CHECK(transport.epoch() == 3);
        REQUIRE(transport.apply(TransportCommand::stop));
        REQUIRE(transport.apply(TransportCommand::record_with_count_in));
        CHECK_FALSE(transport.seek(0));
        REQUIRE(transport.apply(TransportCommand::cancel));
        REQUIRE(transport.apply(TransportCommand::play));
        REQUIRE(transport.apply(TransportCommand::device_failure));
        CHECK_FALSE(transport.seek(0));
        CHECK_FALSE(transport.apply(TransportCommand::play));
        REQUIRE(transport.apply(TransportCommand::reconfigure));
        CHECK(transport.state() == TransportState::stopped);
        CHECK_FALSE(transport.seek(-5));
    }
}

TEST_SUITE("scheduler") {
    TEST_CASE("metronome clicks are generated while playing, so a huge time span stays small") {
        DocumentData document = make_blank_document();
        const TrackId track = first_track(document);
        add_note(document, track, 0, 100, 60);
        // A crafted file can place an event billions of ticks later; the snapshot must not hold one
        // click per beat of that span.
        add_note(document, track, Tick{960} * 4 * 1'000'000'000, Tick{960} * 4 * 1'000'000'000 + 10, 62);
        PlaybackOptions options;
        options.sample_rate = 48000;
        const auto snapshot = compile(document, options);
        CHECK(snapshot->events.size() <= 5); // two notes plus the tempo
        CHECK(snapshot->click_segments.size() == 1);

        SchedulerSettings settings;
        settings.metronome = true;
        Scheduler scheduler(settings);
        NullSink ignore;
        REQUIRE(scheduler.set_snapshot(snapshot.get(), ignore));
        RecordingSink sink;
        scheduler.start(0, sink);
        run_blocks(scheduler, sink, 512, 96000 / 512 + 1); // just over two seconds at 120 BPM
        std::vector<Received> clicks;
        for (const Received& received : sink.events)
            if (received.event.kind == RealtimeEventKind::metronome_click)
                clicks.push_back(received);
        REQUIRE(clicks.size() == 5);
        CHECK(clicks[0].absolute == 0);
        CHECK(clicks[1].absolute == 24000);
        CHECK(clicks[4].absolute == 96000);
        CHECK(clicks[0].event.value == 2); // bar start
        CHECK(clicks[1].event.value == 0);
        CHECK(clicks[4].event.value == 2);
    }

    TEST_CASE("metronome clicks restart inside the loop on every iteration") {
        DocumentData document = make_blank_document();
        add_note(document, first_track(document), 0, 100, 60);
        PlaybackOptions options;
        options.sample_rate = 48000;
        options.session.loop = TimeRange{960, 2880}; // beats 2 and 3 of bar 1
        const auto snapshot = compile(document, options);
        SchedulerSettings settings;
        settings.metronome = true;
        Scheduler scheduler(settings);
        RecordingSink sink;
        REQUIRE(scheduler.set_snapshot(snapshot.get(), sink));
        scheduler.start(960, sink);
        run_blocks(scheduler, sink, 480, 200); // two seconds = two loop iterations
        std::vector<std::int64_t> frames;
        for (const Received& received : sink.events)
            if (received.event.kind == RealtimeEventKind::metronome_click)
                frames.push_back(received.absolute);
        REQUIRE(frames.size() >= 4);
        CHECK(frames[0] == 0);
        CHECK(frames[1] == 24000);
        CHECK(frames[2] == 48000); // next iteration starts with the loop's first beat again
        CHECK(frames[3] == 72000);
    }

    TEST_CASE("a count-in plays bar clicks before the song, also from tick 0") {
        DocumentData document = make_blank_document();
        add_note(document, first_track(document), 0, 480, 60);
        PlaybackOptions options;
        options.sample_rate = 48000;
        const auto snapshot = compile(document, options);
        Scheduler scheduler; // metronome off: count-in clicks still sound
        RecordingSink sink;
        REQUIRE(scheduler.set_snapshot(snapshot.get(), sink));
        scheduler.start(0, sink, 0, 1);
        CHECK(scheduler.in_count_in());
        CHECK(scheduler.position_tick() == 0);
        // Before the song starts the clock maps to ticks before 0 at the tempo of tick 0.
        CHECK(linear_tick_at(*snapshot, scheduler.clock(), 0) == -3840);
        CHECK(linear_tick_at(*snapshot, scheduler.clock(), 96000) == 0);
        run_blocks(scheduler, sink, 512, 200);
        std::vector<Received> clicks;
        std::optional<std::int64_t> note_on;
        for (const Received& received : sink.events) {
            if (received.event.kind == RealtimeEventKind::metronome_click)
                clicks.push_back(received);
            else if (received.event.kind == RealtimeEventKind::midi && received.event.message() == 0x90 && !note_on)
                note_on = received.absolute;
        }
        REQUIRE(clicks.size() == 4);
        CHECK(clicks[0].event.value == 2);
        CHECK(clicks[3].absolute == 72000);
        REQUIRE(note_on);
        CHECK(*note_on == 96000);
        CHECK_FALSE(scheduler.in_count_in());
    }

    TEST_CASE("a snapshot swap while playing sends an edited program, and nothing when unchanged") {
        DocumentData document = make_blank_document();
        const TrackId track = first_track(document);
        add_note(document, track, 0, 100000, 60);
        insert_event(document, track, events::program_change(0, 0, 5));
        const auto before = compile(document);
        for (Event& event : document.sequences.front().tracks.front().events)
            if (event.is_channel() && event.message() == ChannelMessage::program_change)
                event.payload = Payload{40};
        const auto after = compile(document);

        Scheduler scheduler;
        RecordingSink sink;
        REQUIRE(scheduler.set_snapshot(before.get(), sink));
        scheduler.start(0, sink);
        run_blocks(scheduler, sink, 1024, 8);
        sink.events.clear();
        REQUIRE(scheduler.set_snapshot(after.get(), sink));
        CHECK(std::ranges::any_of(sink.events, [](const Received& received) {
            return received.event.message() == 0xC0 && received.event.data1() == 40;
        }));
        CHECK_FALSE(std::ranges::any_of(sink.events, [](const Received& received) {
            return received.event.message() == 0xB0 && received.event.data1() == 7; // unchanged volume
        }));
        CHECK(scheduler.owned_notes().size() == 1);
    }

    TEST_CASE("the transport clock unfolds loop passes, also across a snapshot swap") {
        DocumentData document = make_blank_document();
        add_note(document, first_track(document), 0, 100, 60);
        PlaybackOptions options;
        options.sample_rate = 48000;
        options.session.loop = TimeRange{0, 1920};
        const auto snapshot = compile(document, options);
        const auto recompiled = compile(document, options);
        Scheduler scheduler;
        NullSink ignore;
        REQUIRE(scheduler.set_snapshot(snapshot.get(), ignore));
        scheduler.start(0, ignore);
        while (scheduler.loop_iteration() < 2)
            std::ignore = scheduler.process_block(480, ignore); // Position is checked below.
        for (int index = 0; index < 20; ++index)
            std::ignore = scheduler.process_block(480, ignore); // into pass 3
        const Tick expected = 2 * 1920 + scheduler.position_tick();
        CHECK(std::abs(linear_tick_at(*snapshot, scheduler.clock(), scheduler.absolute_frame()) - expected) <= 1);
        REQUIRE(scheduler.set_snapshot(recompiled.get(), ignore)); // re-anchors inside the pass
        CHECK(std::abs(linear_tick_at(*recompiled, scheduler.clock(), scheduler.absolute_frame()) - expected) <= 1);
        // Half a second earlier is still in the same pass.
        CHECK(std::abs(linear_tick_at(*recompiled, scheduler.clock(), scheduler.absolute_frame() - 24000) -
                       (expected - 960)) <= 1);
    }
}

TEST_SUITE("scheduler") {
    TEST_CASE("a tempo edit while playing keeps the musical position and continues at the new tempo") {
        DocumentData document = make_blank_document();
        add_note(document, first_track(document), 0, 100000, 60);
        PlaybackOptions options;
        options.sample_rate = 48000;
        const auto before = compile(document, options);
        bool replaced = false;
        for (Event& event : document.sequences.front().tracks.front().events)
            if (event.is_meta(meta_type::tempo) && event.tick == 0) {
                event.payload = events::tempo(0, 1'000'000).payload; // 120 -> 60 BPM
                replaced = true;
            }
        if (!replaced)
            insert_event(document, first_track(document), events::tempo(0, 1'000'000));
        const auto after = compile(document, options);
        Scheduler scheduler;
        NullSink ignore;
        REQUIRE(scheduler.set_snapshot(before.get(), ignore));
        scheduler.start(0, ignore);
        for (int block = 0; block < 100; ++block)
            std::ignore = scheduler.process_block(480, ignore); // 1 s at 120 BPM
        const Tick at_swap = scheduler.position_tick();
        CHECK(std::abs(at_swap - 1920) <= 1);
        REQUIRE(scheduler.set_snapshot(after.get(), ignore));
        CHECK(std::abs(scheduler.position_tick() - at_swap) <= 1); // no jump, no restart
        CHECK(scheduler.playing());
        for (int block = 0; block < 100; ++block)
            std::ignore = scheduler.process_block(480, ignore); // 1 s at 60 BPM: one beat
        CHECK(std::abs(scheduler.position_tick() - (at_swap + 960)) <= 2);
    }

    TEST_CASE("a take may run past the song end: minimum_end_tick extends the snapshot") {
        DocumentData document = make_blank_document();
        PlaybackOptions options;
        options.minimum_end_tick = 1'000'000;
        const auto snapshot = compile(document, options);
        CHECK(snapshot->end_tick >= 1'000'000);
    }

    TEST_CASE("the transport clock follows the tempo multiplier") {
        DocumentData document = make_blank_document();
        add_note(document, first_track(document), 0, 100000, 60);
        PlaybackOptions options;
        options.sample_rate = 48000;
        options.session.tempo_multiplier = Rational{2, 1};
        const auto snapshot = compile(document, options);
        Scheduler scheduler;
        NullSink ignore;
        REQUIRE(scheduler.set_snapshot(snapshot.get(), ignore));
        scheduler.start(0, ignore);
        // One second at 120 BPM and double speed is four beats.
        CHECK(std::abs(linear_tick_at(*snapshot, scheduler.clock(), 48000) - 3840) <= 1);
    }
}
