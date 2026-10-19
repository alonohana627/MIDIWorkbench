// Editor and file benchmarks on the E100K corpus of section 33.2: one PPQ sequence with 100,000
// notes on 32 tracks, controllers, tempo changes and long crossing notes, generated
// deterministically. Results are measured with nanobench and printed as JSON (never estimated).
#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include "mwb/core/change_set.hpp"
#include "mwb/core/hashing.hpp"
#include "mwb/core/serialization.hpp"
#include "mwb/core/note_edit.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/core/random.hpp"
#include "mwb/core/tempo_map.hpp"
#include "mwb/document/native_format.hpp"
#include "mwb/midi/codec.hpp"
#include "mwb/music/transform.hpp"

#include <cstdio>
#include <iostream>

using namespace mwb;

namespace {

DocumentData make_e100k() {
    DocumentData document = make_blank_document("E100K");
    auto& sequence = document.sequences.front();
    DeterministicRandom random(100000);
    for (int index = 1; index < 32; ++index) {
        Track track;
        track.id = TrackId::generate();
        track.name = "Track " + std::to_string(index + 1);
        sequence.tracks.push_back(std::move(track));
    }
    for (std::size_t track_index = 0; track_index < sequence.tracks.size(); ++track_index) {
        auto& track = sequence.tracks[track_index];
        std::vector<Event> events = track.events;
        const auto channel = static_cast<std::uint8_t>(track_index % 16);
        for (int note = 0; note < 3125; ++note) {
            const Tick start = note * 240 + static_cast<Tick>(random.bounded(60));
            const bool long_note = note % 500 == 0; // long crossing notes
            const Tick length = long_note ? 96000 : 120 + static_cast<Tick>(random.bounded(400));
            const auto pitch = static_cast<std::uint8_t>(36 + random.bounded(48));
            events.push_back(events::note_on(start, channel, pitch, static_cast<std::uint8_t>(40 + random.bounded(80))));
            events.push_back(events::note_off(start + length, channel, pitch, 0));
            if (note % 8 == 0)
                events.push_back(events::control_change(start, channel, 1, static_cast<std::uint8_t>(random.bounded(128))));
        }
        if (track_index == 0)
            for (int change = 1; change < 50; ++change)
                events.push_back(events::tempo(change * 15000, static_cast<std::uint32_t>(400000 + change * 1000)));
        std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.tick < b.tick; });
        for (std::size_t index = 0; index < events.size(); ++index)
            events[index].order_key = (index + 1) * order_key_spacing;
        track.events = std::move(events);
        track.end_tick = track.events.back().tick;
    }
    sequence.end_tick = sequence.effective_end();
    return document;
}

} // namespace

int main() {
    const auto document = make_e100k();
    const auto& sequence = document.sequences.front();
    std::size_t events = 0;
    for (const auto& track : sequence.tracks)
        events += track.events.size();
    std::fprintf(stderr, "E100K: %zu tracks, %zu events\n", sequence.tracks.size(), events);

    ankerl::nanobench::Bench bench;
    bench.title("E100K editor and file paths").unit("op").warmup(1).minEpochIterations(3).relative(false);
    NotePairing pairing;
    bench.run("pair_notes (full sequence)", [&] { pairing = pair_notes(sequence); });
    NoteIndex index;
    bench.run("NoteIndex build", [&] { index = NoteIndex(pairing.notes); });
    std::size_t visible = 0;
    bench.run("viewport query (8 bars, all pitches)", [&] { visible = index.query(TimeRange{400000, 400000 + 8 * 3840}).size(); });
    ankerl::nanobench::doNotOptimizeAway(visible);

    // Move 10,000 notes on one track as one command, then undo it (section 33.3: undo <= 50 ms).
    music::TargetSelection selection;
    selection.sequence = sequence.id;
    selection.tracks = {sequence.tracks[3].id};
    music::MoveParameters move;
    move.ticks = 240;
    move.semitones = 1;
    DocumentData working = document;
    std::optional<ChangeSet> changes;
    bench.run("prepare move of one track's notes (3,125 notes)", [&] {
        auto preview = music::move_notes(working, selection, move);
        changes = preview->changes;
    });
    // The piano roll's drag path: core update_notes on every note of the busiest track and
    // insert_notes for a 3,125-note paste (one merge per track).
    std::vector<NoteUpdate> drag;
    std::vector<NewNote> paste;
    for (const auto& note : pairing.notes)
        if (note.on_track == 3) {
            drag.push_back(NoteUpdate{note.on_id, note.start + 240, note.end + 240, note.pitch, note.velocity,
                                      note.channel, note.release_velocity});
            paste.push_back(NewNote{sequence.tracks[3].id, note.start + 120, note.end + 120, note.pitch,
                                    note.velocity, note.channel, note.release_velocity});
        }
    bench.run("update_notes drag (3,125 notes)", [&] {
        ankerl::nanobench::doNotOptimizeAway(update_notes(document, sequence.id, drag, "drag"));
    });
    bench.run("insert_notes paste (3,125 notes)", [&] {
        ankerl::nanobench::doNotOptimizeAway(insert_notes(document, paste, "paste"));
    });
    bench.run("apply move", [&] {
        if (!apply_change_set(working, *changes))
            std::abort();
        if (!apply_change_set(working, invert(*changes)))
            std::abort();
    });

    bench.run("semantic_hash (per journaled edit)", [&] { ankerl::nanobench::doNotOptimizeAway(semantic_hash(document)); });
    bench.run("document copy (playback snapshot)", [&] { ankerl::nanobench::doNotOptimizeAway(DocumentData(document)); });

    std::vector<std::uint8_t> smf;
    bench.run("SMF export (preserving)", [&] { smf = *midi::export_midi(document, {}); });
    bench.run("SMF import", [&] { ankerl::nanobench::doNotOptimizeAway(midi::import_midi(smf, "e100k.mid")); });
    std::vector<std::uint8_t> native;
    bench.run("native .mwb serialize", [&] { native = *document::native_to_bytes(document); });
    bench.run("native .mwb load + validate", [&] { ankerl::nanobench::doNotOptimizeAway(document::native_from_bytes(native)); });

    bench.render(ankerl::nanobench::templates::json(), std::cout);
    return 0;
}
