#include "test_support.hpp"

#include "mwb/app/document_io.hpp"
#include "mwb/app/recording.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/core/serialization.hpp"
#include "mwb/document/command_codec.hpp"
#include "mwb/document/native_format.hpp"

using namespace mwb;
using namespace mwb::app;

namespace {

CapturedMessage message(std::int64_t ms, std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
    CapturedMessage captured;
    captured.time_ns = ms * 1'000'000;
    captured.size = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 2 : 3;
    captured.bytes = {status, data1, data2};
    return captured;
}

/// 1 ms = 1 tick for readable tests.
Tick millisecond_ticks(std::int64_t nanoseconds) { return nanoseconds / 1'000'000; }

std::vector<NoteSpan> track_notes(const DocumentData& document) {
    return pair_track_notes(document.sequences[0].tracks[0]).notes;
}

} // namespace

TEST_SUITE("recording") {
    TEST_CASE("take chunk codec round-trips and rejects corrupt input") {
        std::vector<CapturedMessage> messages = {message(5, 0x90, 60, 100), message(9, 0x80, 60, 0), message(9, 0xC1, 5, 0)};
        const auto bytes = encode_take_chunk(messages);
        auto decoded = decode_take_chunk(bytes);
        REQUIRE(decoded);
        REQUIRE(decoded->size() == 3);
        CHECK((*decoded)[2].size == 2);
        CHECK((*decoded)[1].time_ns == 9'000'000);
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            auto damaged = bytes;
            damaged[index] ^= 0xFF;
            std::ignore = decode_take_chunk(damaged); // must never crash; many flips are detected
        }
        auto truncated = bytes;
        truncated.pop_back();
        CHECK_FALSE(decode_take_chunk(truncated));
    }

    TEST_CASE("a take maps timestamps, keeps controllers and closes held notes at stop") {
        auto document = make_blank_document();
        const TrackId track = document.sequences[0].tracks[0].id;
        RecordingOptions options;
        options.latency_compensation_ns = 2'000'000; // applied once
        TakeRecorder recorder(track, options, millisecond_ticks);
        recorder.add(message(102, 0x90, 60, 90));
        recorder.add(message(150, 0xB0, 64, 127)); // sustain stays a controller event
        recorder.add(message(202, 0x80, 60, 40));  // physical note-off independent of sustain
        recorder.add(message(302, 0x90, 64, 80));  // still held at stop
        auto take = recorder.finish(document, 500, "Take 1");
        REQUIRE(take);
        CHECK(take->notes == 2);
        CHECK(take->controllers == 1);
        CHECK(take->synthetic_closures == 1);
        CHECK(take->complete);
        REQUIRE(apply_change_set(document, take->changes));
        const auto notes = track_notes(document);
        REQUIRE(notes.size() == 2);
        CHECK(notes[0].start == 100);
        CHECK(notes[0].end == 200);
        CHECK(notes[0].release_velocity == 40);
        CHECK(notes[1].start == 300);
        CHECK(notes[1].end == 500);
        REQUIRE(document.takes.size() == 1);
        CHECK(document.takes[0].status == TakeStatus::accepted);
        auto raw = decode_take_chunk(document.takes[0].chunks.at(0).payload);
        REQUIRE(raw);
        CHECK(raw->size() == 4); // the raw evidence is retained exactly
    }

    TEST_CASE("a recorded take is one undo entry, survives native save and the journal codec") {
        auto document = make_blank_document();
        const auto before = semantic_hash(document);
        TakeRecorder recorder(document.sequences[0].tracks[0].id, {}, millisecond_ticks);
        recorder.add(message(0, 0x90, 60, 90));
        recorder.add(message(100, 0x80, 60, 0));
        auto take = recorder.finish(document, 200, "Take");
        REQUIRE(take);
        REQUIRE(apply_change_set(document, take->changes));
        auto bytes = document::native_to_bytes(document);
        REQUIRE(bytes);
        auto reopened = document::native_from_bytes(*bytes);
        REQUIRE(reopened);
        CHECK(semantic_hash(*reopened) == semantic_hash(document));
        document::CommandEnvelope envelope;
        envelope.changes = take->changes;
        envelope.base_generation = 0;
        envelope.result_generation = 1;
        auto encoded = document::encode_command(envelope);
        REQUIRE(encoded);
        auto decoded = document::decode_command(*encoded);
        REQUIRE(decoded);
        CHECK(decoded->changes.operations.size() == take->changes.operations.size());
        REQUIRE(apply_change_set(document, invert(take->changes)));
        CHECK(semantic_hash(document) == before);
    }

    TEST_CASE("punch boundaries and held-note capture") {
        auto document = make_blank_document();
        RecordingOptions options;
        options.punch = TimeRange{100, 300};
        options.capture_held_notes = true;
        TakeRecorder recorder(document.sequences[0].tracks[0].id, options, millisecond_ticks);
        recorder.add(message(50, 0x90, 60, 90));  // begun before punch-in, held across it
        recorder.add(message(150, 0x80, 60, 0));
        recorder.add(message(200, 0x90, 62, 90));
        recorder.add(message(400, 0x80, 62, 0));  // ends after punch-out
        auto take = recorder.finish(document, 500, "Punch");
        REQUIRE(take);
        REQUIRE(apply_change_set(document, take->changes));
        const auto notes = track_notes(document);
        REQUIRE(notes.size() == 2);
        CHECK(notes[0].start == 100);
        CHECK(notes[0].end == 150);
        CHECK(notes[1].start == 200);
        CHECK(notes[1].end == 300);
    }

    TEST_CASE("replace-range removes old events only inside the punch after acceptance") {
        auto document = make_blank_document();
        const TrackId track = document.sequences[0].tracks[0].id;
        ChangeBuilder existing(document, "existing");
        existing.add_note(track, 0, 50, 0, 40, 100);
        existing.add_note(track, 150, 200, 0, 41, 100);
        REQUIRE(apply_change_set(document, existing.finish()));
        RecordingOptions options;
        options.punch = TimeRange{100, 300};
        options.replace_range = true;
        TakeRecorder recorder(track, options, millisecond_ticks);
        recorder.add(message(120, 0x90, 70, 90));
        recorder.add(message(180, 0x80, 70, 0));
        auto take = recorder.finish(document, 400, "Replace");
        REQUIRE(take);
        REQUIRE(apply_change_set(document, take->changes));
        const auto notes = track_notes(document);
        REQUIRE(notes.size() == 2);
        CHECK(notes[0].pitch == 40); // outside the punch: kept
        CHECK(notes[1].pitch == 70);
    }

    TEST_CASE("a note held across the loop end is split with synthetic boundary events") {
        auto document = make_blank_document();
        RecordingOptions options;
        options.loop = TimeRange{0, 1000};
        TakeRecorder recorder(document.sequences[0].tracks[0].id, options, millisecond_ticks);
        recorder.add(message(1900, 0x90, 60, 90)); // pass 1
        recorder.add(message(2300, 0x80, 60, 0));  // pass 2
        auto take = recorder.finish(document, 2500, "Loop");
        REQUIRE(take);
        CHECK(take->synthetic_closures >= 1);
        REQUIRE(apply_change_set(document, take->changes));
        const auto notes = track_notes(document);
        // Stopped during pass 3: pass 2 (the last complete one) is composed, ending at the loop end.
        REQUIRE(notes.size() == 1);
        CHECK(notes[0].start == 900);
        CHECK(notes[0].end == 1000);
        auto raw = decode_take_chunk(document.takes[0].chunks.at(0).payload);
        REQUIRE(raw);
        CHECK(raw->size() == 2);
    }

    TEST_CASE("lost input and interruptions make the take incomplete") {
        auto document = make_blank_document();
        TakeRecorder recorder(document.sequences[0].tracks[0].id, {}, millisecond_ticks);
        recorder.add(message(0, 0x90, 60, 90));
        recorder.report_lost(3);
        recorder.interrupt("device unplugged");
        auto take = recorder.finish(document, 100, "Broken");
        REQUIRE(take);
        CHECK_FALSE(take->complete);
        CHECK(take->lost_events == 3);
        CHECK(take->take.status == TakeStatus::interrupted);
    }
}

TEST_SUITE("step_recording") {
    TEST_CASE("chords commit when every key is released; rest, tie and back") {
        auto document = make_blank_document();
        const TrackId track = document.sequences[0].tracks[0].id;
        StepRecorder::Settings settings;
        settings.note_length = 200;
        settings.advance = 240;
        StepRecorder recorder(track, 0, settings);
        recorder.key_down(60, 90);
        recorder.key_down(64, 80);
        recorder.key_down(64, 80); // auto-repeat is ignored
        CHECK_FALSE(recorder.key_up(document, 60));
        auto chord = recorder.key_up(document, 64);
        REQUIRE(chord);
        REQUIRE(apply_change_set(document, *chord));
        CHECK(track_notes(document).size() == 2);
        CHECK(recorder.cursor() == 240);
        recorder.rest();
        CHECK(recorder.cursor() == 480);
        recorder.key_down(67, 100);
        auto single = recorder.key_up(document, 67);
        REQUIRE(single);
        REQUIRE(apply_change_set(document, *single));
        auto tie = recorder.tie(document);
        REQUIRE(tie);
        REQUIRE(apply_change_set(document, *tie));
        auto notes = track_notes(document);
        REQUIRE(notes.size() == 3);
        CHECK(notes[2].start == 480);
        CHECK(notes[2].end == 480 + 200 + 240);
        auto back = recorder.back(document);
        REQUIRE(back);
        REQUIRE(apply_change_set(document, *back));
        CHECK(track_notes(document).size() == 2);
        CHECK(recorder.cursor() == 480);
    }
}

#ifdef MWB_WORKER_PATH
TEST_SUITE("app") {
    TEST_CASE("the worker parser isolates MIDI parsing and matches the in-process result") {
        test::TemporaryDirectory directory;
        DocumentData document = make_blank_document("Worker");
        ChangeBuilder builder(document, "notes");
        builder.add_note(document.sequences[0].tracks[0].id, 0, 480, 0, 60, 100);
        REQUIRE(apply_change_set(document, builder.finish()));
        const auto bytes = midi::export_midi(document, {});
        REQUIRE(bytes);
        const auto path = directory.path() / "song.mid";
        REQUIRE(write_file_atomically(path, *bytes, true));

        WorkerParser worker(MWB_WORKER_PATH);
        CHECK(worker.isolated());
        auto isolated = open_document(path, {}, {}, worker.parser());
        REQUIRE_MESSAGE(isolated, (isolated ? "" : isolated.error().describe()));
        auto local = open_document(path);
        REQUIRE(local);
        CHECK(pair_notes(isolated->document.sequences[0]).notes.size() == 1);
        CHECK(isolated->import_report->reader_result == local->import_report->reader_result);
        CHECK(isolated->import_report->event_count == local->import_report->event_count);
        // A file the worker rejects fails only that file; the next parse restarts as needed.
        const std::vector<std::uint8_t> garbage = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1};
        CHECK_FALSE(worker.parse(garbage, "bad.mid", {}, {}).has_value());
        CHECK(worker.parse(*bytes, "again.mid", {}, {}).has_value());

        WorkerParser missing(directory.path() / "no-worker.exe");
        CHECK_FALSE(missing.isolated());
        CHECK(missing.parse(*bytes, "local.mid", {}, {}).has_value()); // falls back to in-process
    }
}
#endif
