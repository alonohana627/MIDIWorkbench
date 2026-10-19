// Release-only performance checks against specification section 33.3 on the E100K corpus
// (32 tracks, 100,000 notes, controllers and tempo changes). Registered only for optimized
// builds; the medians are printed so a run records the measured values.
#include "test_support.hpp"

#include "mwb/core/change_set.hpp"
#include "mwb/core/serialization.hpp"
#include "mwb/core/validation.hpp"
#include "mwb/core/note_edit.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/core/random.hpp"
#include "mwb/core/tempo_map.hpp"
#include "mwb/library/bench.hpp"
#include "mwb/document/recovery.hpp"
#include "mwb/document/session.hpp"

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

TEST_SUITE("perf") {
    TEST_CASE("E100K semantic hash, computed per journaled edit and per playback snapshot (code-core-12)") {
        const auto document = make_e100k();
        Sha256Digest digest{};
        const double hash = median_ms(5, [&] { digest = semantic_hash(document); });
        std::printf("E100K: semantic_hash %.1f ms\n", hash);
        CHECK(digest != Sha256Digest{});
        // The cost is SHA-256 itself (picosha2, ~220 MB/s over ~8 MB of canonical bytes), not the
        // call pattern: staging the small updates measured the same. A regression guard only.
        CHECK(hash <= 60.0);
    }
}

TEST_SUITE("perf_library") {
    // Section 33.3 on the synthetic L100K catalog with M10K melody streams: first pages and
    // selective filters (SQL and the browse index) <= 150 ms p95 warm, exact motif search
    // <= 500 ms p95, approximate <= 2 s p95 (code-library-13).
    TEST_CASE("L100K/M10K: search pages and melody searches stay within the section 33.3 targets") {
        test::TemporaryDirectory temporary;
        library::BenchOptions options;
        options.rows = 100'000;
        options.motif_rows = 10'000;
        options.repetitions = 10;
        auto report = library::run_bench(temporary.path() / "bench", options);
        REQUIRE(report.has_value());
        CHECK(report->plan_problems.empty());
        std::printf("L100K populate %.1f s\n", report->populate_seconds);
        for (const auto& timing : report->timings) {
            std::printf("%-24s p50 %8.2f ms  p95 %8.2f ms\n", timing.name.c_str(), timing.p50_ms, timing.p95_ms);
            CAPTURE(timing.name);
            const std::string& name = timing.name;
            if (name.starts_with("motif_exact"))
                CHECK(timing.p95_ms <= 500.0);
            else if (name.starts_with("motif_similar"))
                CHECK(timing.p95_ms <= 2000.0);
            else if (name == "browse_index_build")
                CHECK(timing.max_ms <= 30'000.0); // background work, bounded so it cannot silently grow
            else
                CHECK(timing.p95_ms <= 150.0);
        }
    }
}

TEST_SUITE("perf_large") {
    // Spec-sized documents that only optimized builds handle in reasonable time.
    TEST_CASE("a document above the editable event limit still recovers, and edits cannot grow past it (code-document-3)") {
        DocumentData document = make_blank_document("Huge");
        auto& track = document.sequences.front().tracks.front();
        const std::size_t count = DocumentLimits{}.max_events_per_document + 1;
        track.events.clear();
        track.events.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            auto event = events::control_change(static_cast<Tick>(index / 4), 0, 1, static_cast<std::uint8_t>(index % 128));
            event.order_key = (index + 1) * order_key_spacing;
            track.events.push_back(std::move(event));
        }
        track.end_tick = track.events.back().tick;
        document.sequences.front().end_tick = track.end_tick;
        const auto expected = semantic_hash(document);

        test::TemporaryDirectory directory;
        document::RecoveryPolicy policy;
        policy.checkpoint_command_count = 1;
        auto created = document::RecoveryJournal::create(directory.path(), document,
                                                         document::SourceReference{"Huge", {}, {}, {}}, policy);
        REQUIRE(created);
        std::shared_ptr<document::RecoveryJournal> journal = std::move(*created);
        REQUIRE(journal->flush(true));
        const Uuid session_id = journal->session_id();
        journal.reset(); // crash
        const auto recovered = document::recover_session(directory.path(), session_id);
        REQUIRE_MESSAGE(recovered, (recovered ? "" : recovered.error().describe()));
        CHECK(recovered->document.sequences.front().tracks.front().events.size() == count);
        CHECK(semantic_hash(recovered->document) == expected);

        // Editing cannot push a document past the limit (it would only open read-only later).
        document::WorkingSession session(std::move(document), document::SessionOrigin::recovered);
        ChangeBuilder builder(session.document(), "one more");
        builder.insert(session.document().sequences.front().tracks.front().id, events::control_change(0, 0, 7, 100));
        const auto grown = session.apply(builder.finish());
        REQUIRE_FALSE(grown);
        CHECK(grown.error().code == ErrorCode::limit_exceeded);
    }
}
