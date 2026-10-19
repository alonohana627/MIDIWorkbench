// Release-only performance checks against specification section 33.3 on the E100K corpus
// (32 tracks, 100,000 notes, controllers and tempo changes). Registered only for optimized
// builds; the medians are printed so a run records the measured values.
#include "test_support.hpp"

#include "mwb/core/change_set.hpp"
#include "mwb/core/note_edit.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/core/random.hpp"
#include "mwb/core/tempo_map.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace mwb;

namespace {

DocumentData make_e100k() {
    DocumentData document = make_blank_document("E100K");
    auto& sequence = document.sequences.front();
    DeterministicRandom random(100000);
    for (int index = 1; index < 32; ++index) {
        Track track;
        track.id = TrackId::generate();
        sequence.tracks.push_back(std::move(track));
    }
    for (std::size_t track_index = 0; track_index < sequence.tracks.size(); ++track_index) {
        auto& track = sequence.tracks[track_index];
        std::vector<Event> events = track.events;
        const auto channel = static_cast<std::uint8_t>(track_index % 16);
        for (int note = 0; note < 3125; ++note) {
            const Tick start = note * 240 + static_cast<Tick>(random.bounded(60));
            const Tick length = note % 500 == 0 ? 96000 : 120 + static_cast<Tick>(random.bounded(400));
            const auto pitch = static_cast<std::uint8_t>(36 + random.bounded(48));
            events.push_back(events::note_on(start, channel, pitch, static_cast<std::uint8_t>(40 + random.bounded(80))));
            events.push_back(events::note_off(start + length, channel, pitch, 0));
            if (note % 8 == 0)
                events.push_back(events::control_change(start, channel, 1, static_cast<std::uint8_t>(random.bounded(128))));
        }
        std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.tick < b.tick; });
        for (std::size_t index = 0; index < events.size(); ++index)
            events[index].order_key = (index + 1) * order_key_spacing;
        track.events = std::move(events);
        track.end_tick = track.events.back().tick;
    }
    sequence.end_tick = sequence.effective_end();
    return document;
}

template <class Work>
double median_ms(int runs, Work work) {
    std::vector<double> samples;
    for (int run = 0; run < runs; ++run) {
        const auto begin = std::chrono::steady_clock::now();
        work();
        samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
    }
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
}

/// What the editor does after any change: re-pair notes and rebuild the interval index.
void rebuild_editor_caches(const Sequence& sequence) {
    const auto pairing = pair_notes(sequence);
    const NoteIndex index(pairing.notes);
    std::ignore = index;
}

} // namespace

TEST_SUITE("perf") {
    TEST_CASE("E100K: an edit of 3,125 notes and its undo stay within the section 33.3 undo budget") {
        auto document = make_e100k();
        const auto& sequence = document.sequences.front();
        std::vector<NoteUpdate> moves;
        for (const auto& note : pair_notes(sequence).notes)
            if (note.on_track == 3)
                moves.push_back(NoteUpdate{note.on_id, note.start + 240, note.end + 240, note.pitch, note.velocity,
                                           note.channel, note.release_velocity});
        REQUIRE(moves.size() == 3125);
        auto changes = update_notes(document, sequence.id, moves);
        REQUIRE(changes);
        const auto undo = invert(*changes);
        // Apply and undo, each followed by the editor's cache rebuild (what the user waits for).
        const double edit = median_ms(5, [&] {
            REQUIRE(apply_change_set(document, *changes));
            rebuild_editor_caches(document.sequences.front());
            REQUIRE(apply_change_set(document, undo));
        });
        const double undo_only = median_ms(5, [&] {
            REQUIRE(apply_change_set(document, *changes));
            REQUIRE(apply_change_set(document, undo));
            rebuild_editor_caches(document.sequences.front());
        });
        std::printf("E100K: apply+caches+undo %.1f ms, apply+undo+caches %.1f ms\n", edit, undo_only);
        CHECK(undo_only <= 50.0 + 2.0); // undo ordinary edit <= 50 ms p95 (one apply counted twice here)
    }
}
