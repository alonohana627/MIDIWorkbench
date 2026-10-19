#include "test_support.hpp"

#include "mwb/core/change_set.hpp"
#include "mwb/core/checked.hpp"
#include "mwb/core/hashing.hpp"
#include "mwb/core/note_edit.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/core/pitch.hpp"
#include "mwb/core/random.hpp"
#include "mwb/core/serialization.hpp"
#include "mwb/core/tempo_map.hpp"
#include "mwb/core/text.hpp"
#include "mwb/core/validation.hpp"

#include <set>
#include <tuple>

using namespace mwb;

namespace {

Sequence ppq_sequence(std::int32_t ppq) {
    Sequence sequence;
    sequence.id = SequenceId::generate();
    sequence.division = Division::make_ppq(ppq);
    Track track;
    track.id = TrackId::generate();
    sequence.tracks.push_back(track);
    return sequence;
}

void add_event(Track& track, Event event) {
    event.order_key = (track.events.size() + 1) * order_key_spacing;
    track.events.push_back(std::move(event));
    track.end_tick = std::max(track.end_tick, track.events.back().tick);
}

} // namespace

TEST_SUITE("time") {
    TEST_CASE("golden 1: PPQ 960 at 120 BPM maps tick 960 to 0.5 s and sample 24000 at 48 kHz") {
        const auto map = TempoMap::constant(Division::make_ppq(960), 500000);
        CHECK(map.tick_to_microseconds(960) == 500000);
        CHECK(map.tick_to_frame(960, 48000) == 24000);
    }

    TEST_CASE("golden 2: tempo switch 120 -> 60 BPM at tick 960 puts tick 1920 at 1.5 s / frame 72000") {
        auto sequence = ppq_sequence(960);
        add_event(sequence.tracks[0], events::tempo(0, 500000));
        add_event(sequence.tracks[0], events::tempo(960, 1000000));
        const auto map = TempoMap::build(sequence);
        CHECK(map.tick_to_microseconds(1920) == 1500000);
        CHECK(map.tick_to_frame(1920, 48000) == 72000);
        CHECK(map.has_explicit_initial_tempo());
        CHECK(map.microseconds_to_tick(1500000) == 1920);
        CHECK(map.frame_to_tick(72000, 48000) == 1920);
    }

    TEST_CASE("golden 3: 6/8 at PPQ 960 spans 2880 ticks per bar") {
        const auto meter = MeterMap::constant(960, 6, 8);
        CHECK(meter.ticks_per_bar(meter.segments().front()) == 2880);
        CHECK(meter.bar_start(2) == 5760);
        const auto position = meter.position(2880 + 480 + 5);
        CHECK(position.bar == 1);
        CHECK(position.beat == 1);
        CHECK(position.tick_in_beat == 5);
    }

    TEST_CASE("golden 4: SMPTE -29 with 100 ticks/frame spans 1.001 s in 3000 ticks") {
        const auto map = TempoMap::constant(Division::make_smpte(SmpteFormat::fps29_97_drop, 100), 500000);
        CHECK(map.tick_to_microseconds(3000) == 1001000);
    }

    TEST_CASE("default tempo is assumed and reported when no tempo event exists") {
        const auto sequence = ppq_sequence(480);
        const auto map = TempoMap::build(sequence);
        CHECK_FALSE(map.has_explicit_initial_tempo());
        CHECK(map.tick_to_microseconds(480) == 500000);
    }

    TEST_CASE("time conversion is monotone across tempo changes") {
        auto sequence = ppq_sequence(96);
        add_event(sequence.tracks[0], events::tempo(0, 400000));
        add_event(sequence.tracks[0], events::tempo(100, 700001));
        add_event(sequence.tracks[0], events::tempo(333, 250000));
        const auto map = TempoMap::build(sequence);
        std::int64_t previous = -1;
        for (Tick tick = 0; tick < 2000; ++tick) {
            const auto frame = map.tick_to_frame(tick, 44100);
            CHECK(frame >= previous);
            previous = frame;
        }
    }

    TEST_CASE("an equal-tick tempo back to the previous value is not a tempo change (code-core-24)") {
        auto sequence = ppq_sequence(960);
        add_event(sequence.tracks[0], events::tempo(0, 500000));
        add_event(sequence.tracks[0], events::tempo(960, 400000));
        add_event(sequence.tracks[0], events::tempo(960, 500000));
        const auto map = TempoMap::build(sequence);
        CHECK(map.change_count() == 0);
        CHECK(map.tempo_at(2000) == 500000);
    }

    TEST_CASE("pitch names with absurd octaves are rejected, not wrapped (code-core-22)") {
        CHECK_FALSE(parse_pitch("C357913941").has_value());
        CHECK_FALSE(parse_pitch("C-21").has_value());
        CHECK(parse_pitch("C4") == std::optional<std::uint8_t>(60));
    }

    TEST_CASE("equal-tick tempo conflict keeps the last event and is flagged") {
        auto sequence = ppq_sequence(960);
        add_event(sequence.tracks[0], events::tempo(0, 500000));
        add_event(sequence.tracks[0], events::tempo(0, 600000));
        const auto map = TempoMap::build(sequence);
        CHECK(map.tempo_at(0) == 600000);
        CHECK(map.has_conflicts());
    }

    TEST_CASE("mid-bar meter change starts a new bar and marks the previous one partial") {
        auto sequence = ppq_sequence(960);
        add_event(sequence.tracks[0], events::time_signature(0, 4, 2));
        add_event(sequence.tracks[0], events::time_signature(3840 + 960, 3, 2));
        const auto meter = MeterMap::build(sequence);
        REQUIRE(meter.segments().size() == 2);
        CHECK(meter.segments()[1].previous_bar_partial);
        CHECK(meter.segments()[1].first_bar == 2);
        CHECK(meter.position(3840 + 960).bar == 2);
    }

    TEST_CASE("bars that are not a whole number of ticks are counted exactly (code-core-4)") {
        // PPQ 1 in 7/8: a bar is 3.5 ticks. Tick 70 starts bar 21 (20 bars of 3.5 ticks).
        const auto meter = MeterMap::constant(1, 7, 8);
        const auto position = meter.position(70);
        CHECK(position.bar == 20);
        CHECK(position.beat == 0);
        CHECK(position.tick_in_beat == 0);
        CHECK(meter.format(70) == "21.1.000");
        for (Tick tick = 0; tick < 400; ++tick) {
            const auto at = meter.position(tick);
            CAPTURE(tick);
            CHECK(at.beat >= 0);
            CHECK(at.tick_in_beat >= 0);
            CHECK(meter.bar_start(at.bar) <= tick);
            CHECK(meter.bar_start(at.bar + 1) > tick); // the tick belongs to exactly that bar
        }
        // A meter change after 5 such bars plus one tick: 5 whole bars and a partial one.
        auto sequence = ppq_sequence(1);
        add_event(sequence.tracks[0], events::time_signature(0, 7, 3));
        add_event(sequence.tracks[0], events::time_signature(18, 4, 2));
        const auto changed = MeterMap::build(sequence);
        REQUIRE(changed.segments().size() == 2);
        CHECK(changed.segments()[1].first_bar == 6);
        CHECK(changed.segments()[1].previous_bar_partial);
    }

    TEST_CASE("checked arithmetic rounds half away from zero and detects overflow") {
        CHECK(mul_div_round(3, 1, 2) == 2);
        CHECK(mul_div_round(-3, 1, 2) == -2);
        CHECK(mul_div_floor(-3, 1, 2) == -2);
        CHECK_FALSE(mul_div_floor(INT64_MAX, 4, 1).has_value());
        CHECK_FALSE(checked_add(INT64_MAX, 1).has_value());
    }

    TEST_CASE("rational parsing normalizes") {
        auto value = Rational::parse("6/8");
        REQUIRE(value);
        CHECK(value->numerator == 3);
        CHECK(value->denominator == 4);
        CHECK_FALSE(Rational::parse("1/0"));
    }
}

TEST_SUITE("time") {
    TEST_CASE("the incremental SHA-256 builder gives the digest of the concatenated bytes") {
        std::vector<std::uint8_t> all;
        Sha256Builder builder;
        for (std::uint32_t index = 0; index < 50'000; ++index) { // ~400 KiB of small updates
            builder.update_u32(index);
            builder.update_u8(static_cast<std::uint8_t>(index));
            for (int shift = 0; shift < 32; shift += 8)
                all.push_back(static_cast<std::uint8_t>(index >> shift));
            all.push_back(static_cast<std::uint8_t>(index));
        }
        std::vector<std::uint8_t> large(200'000, 0x5A); // larger than the stage: passed through
        builder.update(large);
        all.insert(all.end(), large.begin(), large.end());
        builder.update_string("tail");
        all.insert(all.end(), {std::uint8_t{4}, 0, 0, 0, 0, 0, 0, 0}); // little-endian length
        for (const char character : std::string("tail"))
            all.push_back(static_cast<std::uint8_t>(character));
        CHECK(builder.finish() == sha256(all));
    }
}

TEST_SUITE("notes") {
    TEST_CASE("FIFO pairing, velocity-zero endings, orphans and unterminated notes") {
        auto sequence = ppq_sequence(960);
        auto& track = sequence.tracks[0];
        add_event(track, events::note_on(0, 0, 60, 100));
        add_event(track, events::note_on(10, 0, 60, 90)); // overlapping same key
        add_event(track, events::note_on(20, 0, 60, 0)); // velocity-zero end closes the oldest
        add_event(track, events::note_off(30, 0, 60, 64));
        add_event(track, events::note_off(40, 0, 61, 0)); // orphan
        add_event(track, events::note_on(50, 0, 62, 80)); // unterminated
        sequence.end_tick = 100;
        const auto pairing = pair_notes(sequence);
        REQUIRE(pairing.notes.size() == 3);
        CHECK(pairing.notes[0].start == 0);
        CHECK(pairing.notes[0].end == 20);
        CHECK(pairing.notes[1].end == 30);
        CHECK(pairing.notes[1].release_velocity == 64);
        CHECK((pairing.notes[0].flags & note_flag::ambiguous) != 0);
        CHECK((pairing.notes[2].flags & note_flag::unterminated) != 0);
        CHECK(pairing.notes[2].end == 100);
        CHECK(pairing.orphan_ends.size() == 1);
    }

    TEST_CASE("note ends may live in another track") {
        auto sequence = ppq_sequence(960);
        Track second;
        second.id = TrackId::generate();
        sequence.tracks.push_back(second);
        add_event(sequence.tracks[0], events::note_on(0, 3, 64, 100));
        add_event(sequence.tracks[1], events::note_off(480, 3, 64, 0));
        const auto pairing = pair_notes(sequence);
        REQUIRE(pairing.notes.size() == 1);
        CHECK(pairing.notes[0].end == 480);
        CHECK((pairing.notes[0].flags & note_flag::cross_track) != 0);
    }

    TEST_CASE("zero-length pairs are retained and flagged") {
        auto sequence = ppq_sequence(960);
        add_event(sequence.tracks[0], events::note_on(5, 0, 60, 100));
        add_event(sequence.tracks[0], events::note_off(5, 0, 60, 0));
        const auto pairing = pair_notes(sequence);
        REQUIRE(pairing.notes.size() == 1);
        CHECK((pairing.notes[0].flags & note_flag::zero_length) != 0);
    }

    TEST_CASE("note index finds long notes that start before the window") {
        std::vector<NoteSpan> notes;
        for (int index = 0; index < 1000; ++index) {
            NoteSpan span;
            span.start = index * 10;
            span.end = index * 10 + 5;
            span.pitch = static_cast<std::uint8_t>(index % 128);
            notes.push_back(span);
        }
        NoteSpan long_note;
        long_note.start = 0;
        long_note.end = 100000;
        long_note.pitch = 64;
        notes.push_back(long_note);
        const NoteIndex index(notes);
        const auto found = index.query(TimeRange{50000, 50010});
        bool has_long = false;
        for (const auto position : found)
            if (index.notes()[position].end == 100000)
                has_long = true;
        CHECK(has_long);
    }
}

TEST_SUITE("changes") {
    TEST_CASE("apply then undo restores the exact semantic hash") {
        auto document = make_blank_document("Test");
        const auto before = semantic_hash(document);
        const TrackId track = document.sequences[0].tracks[0].id;
        ChangeBuilder builder(document, "Add notes");
        builder.add_note(track, 0, 240, 0, 60, 100);
        builder.add_note(track, 240, 480, 0, 60, 100); // meets the previous same-pitch note
        builder.add_note(track, 100000, 100480, 0, 72, 100); // extends the track end
        const ChangeSet changes = builder.finish();
        REQUIRE(apply_change_set(document, changes));
        CHECK(semantic_hash(document) != before);
        CHECK(document.sequences[0].tracks[0].end_tick == 100480);
        CHECK_FALSE(has_errors(validate_document(document)));

        const auto pairing = pair_notes(document.sequences[0]);
        REQUIRE(pairing.notes.size() == 3);
        CHECK(pairing.notes[0].end == 240);
        CHECK(pairing.notes[1].start == 240);
        CHECK(pairing.notes[1].end == 480);

        REQUIRE(apply_change_set(document, invert(changes)));
        CHECK(semantic_hash(document) == before);
    }

    TEST_CASE("a failing operation leaves the document unchanged") {
        auto document = make_blank_document();
        const auto before = semantic_hash(document);
        const TrackId track = document.sequences[0].tracks[0].id;
        ChangeSet changes;
        changes.operations.push_back(EventInsert{track, events::note_on(0, 0, 60, 100)});
        std::get<EventInsert>(changes.operations.back()).event.order_key = 99999;
        changes.operations.push_back(TrackPropertiesChange{TrackId::generate(), Track{}, Track{}});
        CHECK_FALSE(apply_change_set(document, changes));
        CHECK(semantic_hash(document) == before);
    }

    TEST_CASE("moving an event re-sorts it and stays invertible") {
        auto document = make_blank_document();
        const TrackId track = document.sequences[0].tracks[0].id;
        ChangeBuilder first(document, "add");
        const EventId note = first.add_note(track, 960, 1920, 0, 64, 90);
        REQUIRE(apply_change_set(document, first.finish()));
        const auto before = semantic_hash(document);

        ChangeBuilder move(document, "move");
        const Track* working = move.working_track(track);
        const auto position = find_event(*working, note);
        REQUIRE(position);
        Event moved = working->events[*position];
        moved.tick = 0;
        REQUIRE(move.replace(track, note, moved));
        const ChangeSet changes = move.finish();
        REQUIRE(apply_change_set(document, changes));
        CHECK(document.sequences[0].tracks[0].events.front().tick == 0);
        CHECK_FALSE(has_errors(validate_document(document)));
        REQUIRE(apply_change_set(document, invert(changes)));
        CHECK(semantic_hash(document) == before);
    }

    TEST_CASE("split, update and duplicate notes as single valid commands") {
        auto document = make_blank_document();
        const auto sequence_id = document.sequences[0].id;
        const TrackId track = document.sequences[0].tracks[0].id;
        const NewNote first{track, 0, 960, 60, 100, 0, 0};
        const NewNote touching{track, 960, 1920, 60, 90, 0, 0}; // same pitch starts where the first ends
        const NewNote both[] = {first, touching};
        REQUIRE(apply_change_set(document, insert_notes(document, both).value()));
        auto notes = pair_notes(document.sequences[0]).notes;
        REQUIRE(notes.size() == 2);

        // Split at a tick with no other event (the case that used to collide) and at a shared tick.
        auto split = split_note(document, sequence_id, notes[0].on_id, 480);
        REQUIRE(split.has_value());
        const auto before_split = semantic_hash(document);
        REQUIRE(apply_change_set(document, split->changes));
        CHECK_FALSE(has_errors(validate_document(document)));
        notes = pair_notes(document.sequences[0]).notes;
        REQUIRE(notes.size() == 3);
        CHECK(notes[0].start == 0);
        CHECK(notes[0].end == 480);
        CHECK(notes[1].start == 480);
        CHECK(notes[1].end == 960);
        CHECK(notes[1].on_id == split->created);
        CHECK(notes[2].start == 960);
        CHECK_FALSE(split_note(document, sequence_id, notes[0].on_id, 0).has_value());
        REQUIRE(apply_change_set(document, invert(split->changes)));
        CHECK(semantic_hash(document) == before_split);

        // Move every note right by a beat and up a tone in one command; then undo.
        notes = pair_notes(document.sequences[0]).notes;
        std::vector<NoteUpdate> moves;
        for (const auto& note : notes)
            moves.push_back(NoteUpdate{note.on_id, note.start + 240, note.end + 240,
                                       static_cast<std::uint8_t>(note.pitch + 2), note.velocity, note.channel, 0});
        auto moved = update_notes(document, sequence_id, moves);
        REQUIRE(moved.has_value());
        REQUIRE(apply_change_set(document, *moved));
        CHECK_FALSE(has_errors(validate_document(document)));
        const auto after_move = pair_notes(document.sequences[0]).notes;
        REQUIRE(after_move.size() == 2);
        CHECK(after_move[0].start == 240);
        CHECK(after_move[0].pitch == 62);
        CHECK(after_move[1].start == 1200);
        CHECK(after_move[0].on_id == notes[0].on_id); // identity survives a move
        REQUIRE(apply_change_set(document, invert(*moved)));
        CHECK(pair_notes(document.sequences[0]).notes[0].start == 0);

        // Duplicate both notes one bar later; a missing destination track is an error.
        const EventId ids[] = {notes[0].on_id, notes[1].on_id};
        auto duplicated = duplicate_notes(document, sequence_id, ids, 3840);
        REQUIRE(duplicated.has_value());
        CHECK(duplicated->created.size() == 2);
        REQUIRE(apply_change_set(document, duplicated->changes));
        CHECK(pair_notes(document.sequences[0]).notes.size() == 4);
        CHECK_FALSE(has_errors(validate_document(document)));
        CHECK_FALSE(duplicate_notes(document, sequence_id, ids, 0, 0, TrackId::generate()).has_value());
    }

    TEST_CASE("bulk track edits keep order invariants on random batches") {
        for (std::uint64_t seed = 1; seed <= 40; ++seed) {
            DeterministicRandom random(seed);
            auto document = make_blank_document();
            const TrackId track = document.sequences[0].tracks[0].id;
            ChangeBuilder setup(document, "setup");
            for (int index = 0; index < 60; ++index)
                setup.add_note(track, random.bounded(8) * 120, random.bounded(8) * 120 + 120, 0,
                               static_cast<std::uint8_t>(60 + random.bounded(4)), 100);
            REQUIRE(apply_change_set(document, setup.finish()));
            const auto& events = document.sequences[0].tracks[0].events;

            BulkTrackEdit bulk;
            std::vector<EventId> kept_order;
            std::set<EventId> note_end_insertions;
            for (const auto& event : events) {
                const auto choice = random.bounded(6);
                if (choice == 0 && event.is_channel()) {
                    Event changed = event;
                    changed.payload = Payload{event.data1(), 77};
                    bulk.replace_in_place.insert_or_assign(event.id, changed);
                }
                if (choice == 1 && event.is_channel())
                    bulk.remove.insert(event.id);
                else
                    kept_order.push_back(event.id);
            }
            for (int index = 0; index < 30; ++index) {
                const bool start = random.bounded(2) != 0;
                Event added = start ? events::note_on(random.bounded(8) * 120, 0, 70, 90)
                                    : events::note_off(random.bounded(8) * 120, 0, 70, 0);
                added.id = EventId::generate();
                if (!start)
                    note_end_insertions.insert(added.id);
                bulk.insert.emplace_back(added, start ? OrderPlacement::after_existing : OrderPlacement::before_note_starts);
            }
            ChangeBuilder batched(document, "batched");
            REQUIRE(batched.edit_track(track, std::move(bulk)));
            const auto& result = batched.working_track(track)->events;
            CHECK(result.size() == kept_order.size() + 30);
            for (std::size_t index = 1; index < result.size(); ++index)
                CHECK(std::tie(result[index - 1].tick, result[index - 1].order_key) <
                      std::tie(result[index].tick, result[index].order_key));
            // Existing events keep their relative order.
            const std::set<EventId> kept_set(kept_order.begin(), kept_order.end());
            std::vector<EventId> seen;
            for (const auto& event : result)
                if (kept_set.contains(event.id))
                    seen.push_back(event.id);
            CHECK(seen == kept_order);
            // An inserted note end never follows a note start at its tick.
            for (std::size_t index = 1; index < result.size(); ++index)
                if (note_end_insertions.contains(result[index].id))
                    CHECK_FALSE((result[index - 1].tick == result[index].tick && result[index - 1].is_note_start()));
            ChangeSet changes = batched.finish();
            const auto before = semantic_hash(document);
            REQUIRE(apply_change_set(document, changes));
            for (const auto& diagnostic : validate_document(document))
                if (diagnostic.severity == Severity::error)
                    FAIL_CHECK(diagnostic.code << ": " << diagnostic.message);
            REQUIRE(apply_change_set(document, invert(changes)));
            CHECK(semantic_hash(document) == before);
        }
    }
}

TEST_SUITE("core") {
    TEST_CASE("identities are random version-4 values with canonical text") {
        const auto first = Uuid::generate();
        const auto second = Uuid::generate();
        CHECK(first != second);
        auto parsed = Uuid::parse(first.to_string());
        REQUIRE(parsed);
        CHECK(*parsed == first);
        CHECK((first.bytes()[6] >> 4) == 4);
    }

    TEST_CASE("PCG32 sequences are reproducible for a seed") {
        DeterministicRandom a(42);
        DeterministicRandom b(42);
        for (int index = 0; index < 100; ++index)
            CHECK(a.bounded(1000) == b.bounded(1000));
        DeterministicRandom c(43);
        bool differs = false;
        DeterministicRandom d(42);
        for (int index = 0; index < 10; ++index)
            differs = differs || c.next_u32() != d.next_u32();
        CHECK(differs);
    }

    TEST_CASE("sha256 known answer") {
        CHECK(to_hex(sha256(std::string_view("abc"))) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }

    TEST_CASE("text decoding keeps UTF-8 and falls back visibly") {
        const std::uint8_t hebrew[] = {0xD7, 0xA9, 0xD7, 0x9C, 0xD7, 0x95, 0xD7, 0x9D};
        bool fallback = true;
        CHECK(decode_midi_text(hebrew, &fallback) == "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D");
        CHECK_FALSE(fallback);
        const std::uint8_t latin[] = {'C', 'a', 'f', 0xE9};
        CHECK(decode_midi_text(latin, &fallback) == "Caf\xC3\xA9");
        CHECK(fallback);
        CHECK(fold_for_search("Stra\xC3\x9F" "E") == "strasse");
    }

    TEST_CASE("pitch names use C4 = 60 by default") {
        CHECK(pitch_name(60) == "C4");
        CHECK(pitch_name(61, -1, true) == "Db4");
        CHECK(parse_pitch("F#3") == 54);
        CHECK(parse_pitch("B#3") == 60);
        CHECK(parse_pitch("127") == 127);
        CHECK_FALSE(parse_pitch("H2"));
    }

    TEST_CASE("settings JSON round-trips with versions") {
        SequenceSettings settings;
        settings.snap = Rational{1, 12};
        settings.loop_region = RegionId::generate();
        settings.input_policy = InputScalePolicy::nearest_member;
        auto parsed = sequence_settings_from_json(sequence_settings_to_json(settings));
        REQUIRE(parsed);
        CHECK(*parsed == settings);
        CHECK_FALSE(sequence_settings_from_json("{\"version\":2}"));
    }

    TEST_CASE("blank document follows the section 5.1 defaults") {
        const auto document = make_blank_document();
        REQUIRE(document.sequences.size() == 1);
        const auto& sequence = document.sequences[0];
        CHECK(sequence.division.ppq == 960);
        CHECK(sequence.tracks.size() == 1);
        CHECK(sequence.settings.snap == Rational{1, 16});
        CHECK(sequence.settings.loop_region.has_value());
        CHECK(TempoMap::build(sequence).tempo_at(0) == 500000);
        CHECK(sequence.regions[0].end_tick == 4 * 4 * 960);
        CHECK_FALSE(has_errors(validate_document(document)));
    }
}

TEST_SUITE("changes") {
    TEST_CASE("delete_notes removes each note-on with its own note-off and undoes exactly (code-core-19)") {
        DocumentData document = make_blank_document();
        const SequenceId sequence = document.sequences.front().id;
        const TrackId track = document.sequences.front().tracks.front().id;
        ChangeBuilder add(document, "add");
        const EventId first = add.add_note(track, 0, 480, 0, 60, 100);
        add.add_note(track, 240, 720, 0, 60, 90); // overlapping same key: FIFO pairing
        const EventId third = add.add_note(track, 960, 1440, 1, 64, 80);
        REQUIRE(apply_change_set(document, add.finish()));
        const auto before = semantic_hash(document);
        const std::vector<EventId> doomed{first, third};
        auto changes = delete_notes(document, sequence, doomed);
        REQUIRE(changes);
        DocumentData after = document;
        REQUIRE(apply_change_set(after, *changes));
        const auto pairing = pair_notes(after.sequences.front());
        REQUIRE(pairing.notes.size() == 1);
        CHECK(pairing.notes[0].start == 240);
        CHECK(pairing.notes[0].end == 720);
        CHECK(pairing.orphan_ends.empty());
        CHECK(after.sequences.front().tracks.front().events.size() ==
              document.sequences.front().tracks.front().events.size() - 4); // two note-ons, two note-offs
        REQUIRE(apply_change_set(after, invert(*changes)));
        CHECK(semantic_hash(after) == before);
        // Unknown identities delete nothing (never another note).
        const std::vector<EventId> unknown{EventId::generate()};
        auto nothing = delete_notes(document, sequence, unknown);
        CHECK((!nothing || nothing->operations.empty()));
    }

    // code-core-1: the end of an edited zero-length note must stay after its own start, or the
    // note-on becomes unterminated and the note-off an orphan.
    TEST_CASE("zero-length notes keep their note-off after the note-on through every edit") {
        DocumentData document = make_blank_document();
        const SequenceId sequence = document.sequences.front().id;
        const TrackId track = document.sequences.front().tracks.front().id;
        ChangeBuilder add(document, "add");
        const EventId note = add.add_note(track, 500, 500, 0, 60, 100);
        REQUIRE(apply_change_set(document, add.finish()));
        const auto check_single_zero_length = [&](const DocumentData& doc, Tick at, int pitch, std::size_t count) {
            const auto pairing = pair_notes(doc.sequences.front());
            CHECK(pairing.orphan_ends.empty());
            REQUIRE(pairing.notes.size() == count);
            bool found = false;
            for (const auto& span : pairing.notes)
                if (span.start == at && span.pitch == pitch) {
                    found = true;
                    CHECK(span.off_id.has_value());
                    CHECK(span.end == at);
                }
            CHECK(found);
        };
        check_single_zero_length(document, 500, 60, 1);

        // Move, transpose and velocity edit in one update.
        const NoteUpdate update{note, 900, 900, 64, 80, 0, 0};
        auto moved = update_notes(document, sequence, std::span(&update, 1));
        REQUIRE(moved);
        REQUIRE(apply_change_set(document, *moved));
        check_single_zero_length(document, 900, 64, 1);

        // Moving it onto a tick that already holds another note's events.
        ChangeBuilder other(document, "other");
        std::ignore = other.add_note(track, 100, 300, 0, 64, 90);
        REQUIRE(apply_change_set(document, other.finish()));
        const NoteUpdate onto{note, 300, 300, 64, 80, 0, 0};
        auto onto_changes = update_notes(document, sequence, std::span(&onto, 1));
        REQUIRE(onto_changes);
        REQUIRE(apply_change_set(document, *onto_changes));
        check_single_zero_length(document, 300, 64, 2);

        // Duplicating it keeps the copy's end after its start too.
        auto copy = duplicate_notes(document, sequence, std::span(&note, 1), 960);
        REQUIRE(copy);
        REQUIRE(apply_change_set(document, copy->changes));
        check_single_zero_length(document, 1260, 64, 3);
    }
}

TEST_SUITE("changes") {
    TEST_CASE("a take with a negative or reversed range is refused, as the native format would") {
        DocumentData document = make_blank_document();
        RecordingTake take;
        take.id = TakeId::generate();
        take.track = document.sequences.front().tracks.front().id;
        take.start_tick = -1000;
        take.end_tick = 0;
        ChangeBuilder negative(document, "take");
        negative.append(TakeInsert{0, take});
        CHECK_FALSE(apply_change_set(document, negative.finish()));
        take.start_tick = 500;
        take.end_tick = 100;
        ChangeBuilder reversed(document, "take");
        reversed.append(TakeInsert{0, take});
        CHECK_FALSE(apply_change_set(document, reversed.finish()));
        CHECK(document.takes.empty());
    }
}
