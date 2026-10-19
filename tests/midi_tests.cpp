#include "test_support.hpp"

#include "mwb/core/change_set.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/core/tempo_map.hpp"
#include "mwb/midi/codec.hpp"

#include <map>
#include <MidiFile.h>

#include <sstream>

using namespace mwb;
using Bytes = std::vector<std::uint8_t>;

namespace {

/// Builds Standard MIDI File bytes by hand so fixtures do not depend on the writer under test.
struct SmfBuilder {
    int format = 1;
    std::uint16_t division = 960;
    std::vector<Bytes> tracks;
    std::vector<Bytes> extra_chunks_before_tracks;
    Bytes header_extension;

    static void be16(Bytes& out, std::uint32_t value) {
        out.push_back(static_cast<std::uint8_t>(value >> 8));
        out.push_back(static_cast<std::uint8_t>(value));
    }
    static void be32(Bytes& out, std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    [[nodiscard]] Bytes build(int declared_tracks = -1) const {
        Bytes out = {'M', 'T', 'h', 'd'};
        be32(out, static_cast<std::uint32_t>(6 + header_extension.size()));
        be16(out, static_cast<std::uint32_t>(format));
        be16(out, static_cast<std::uint32_t>(declared_tracks >= 0 ? declared_tracks : static_cast<int>(tracks.size())));
        be16(out, division);
        out.insert(out.end(), header_extension.begin(), header_extension.end());
        for (const auto& chunk : extra_chunks_before_tracks)
            out.insert(out.end(), chunk.begin(), chunk.end());
        for (const auto& track : tracks) {
            out.insert(out.end(), {'M', 'T', 'r', 'k'});
            be32(out, static_cast<std::uint32_t>(track.size()));
            out.insert(out.end(), track.begin(), track.end());
        }
        return out;
    }
};

const Bytes end_of_track = {0x00, 0xFF, 0x2F, 0x00};

Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& part : parts)
        out.insert(out.end(), part.begin(), part.end());
    return out;
}

Result<midi::ImportResult> import(const Bytes& bytes) { return midi::import_midi(bytes, "fixture.mid"); }

/// Musical content of a document (per sequence, per track: tick, category, code, payload).
std::vector<std::tuple<std::size_t, std::size_t, Tick, int, int, Bytes>> content(const DocumentData& document) {
    std::vector<std::tuple<std::size_t, std::size_t, Tick, int, int, Bytes>> rows;
    for (std::size_t s = 0; s < document.sequences.size(); ++s)
        for (std::size_t t = 0; t < document.sequences[s].tracks.size(); ++t)
            for (const auto& event : document.sequences[s].tracks[t].events)
                rows.emplace_back(s, t, event.tick, static_cast<int>(event.category), event.code,
                                  event.payload.to_vector());
    return rows;
}

Bytes simple_type1() {
    SmfBuilder builder;
    builder.tracks.push_back(concat({{0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}, // 120 BPM
                                     {0x00, 0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08},
                                     end_of_track}));
    builder.tracks.push_back(concat({{0x00, 0xFF, 0x03, 0x04, 'L', 'e', 'a', 'd'},
                                     {0x00, 0xC0, 0x05},
                                     {0x00, 0x90, 0x3C, 0x64},
                                     {0x83, 0x60, 0x80, 0x3C, 0x40}, // delta 480
                                     {0x00, 0x90, 0x40, 0x64},
                                     {0x83, 0x60, 0x90, 0x40, 0x00}, // velocity-zero end
                                     {0x00, 0xF0, 0x03, 0x7E, 0x09, 0xF7},
                                     {0x00, 0xF7, 0x02, 0x01, 0x02},
                                     {0x00, 0xFF, 0x7E, 0x02, 0xAB, 0xCD}, // unknown meta type
                                     end_of_track}));
    return builder.build();
}

} // namespace

TEST_SUITE("import") {
    TEST_CASE("type 1 import keeps events, names, channels and preserved encodings") {
        auto result = import(simple_type1());
        REQUIRE(result);
        const auto& document = result->document;
        CHECK(result->report.format == 1);
        CHECK(result->report.reader_result == "validated");
        REQUIRE(document.sequences.size() == 1);
        const auto& sequence = document.sequences[0];
        CHECK(sequence.division.ppq == 960);
        REQUIRE(sequence.tracks.size() == 2);
        CHECK(sequence.tracks[1].name == "Lead");
        const auto pairing = pair_notes(sequence);
        REQUIRE(pairing.notes.size() == 2);
        CHECK(pairing.notes[0].end == 480);
        CHECK(pairing.notes[0].release_velocity == 64);
        CHECK(pairing.notes[1].end == 960);
        // Velocity-zero note end encoding is preserved as a note-on.
        bool found_velocity_zero = false;
        for (const auto& event : sequence.tracks[1].events)
            if (event.is_channel() && event.message() == ChannelMessage::note_on && event.data2() == 0)
                found_velocity_zero = true;
        CHECK(found_velocity_zero);
        int f0 = 0;
        int f7 = 0;
        int unknown_meta = 0;
        for (const auto& event : sequence.tracks[1].events) {
            f0 += event.category == EventCategory::sysex_f0;
            f7 += event.category == EventCategory::sysex_f7;
            unknown_meta += event.is_meta(0x7E);
        }
        CHECK(f0 == 1);
        CHECK(f7 == 1);
        CHECK(unknown_meta == 1);
        CHECK(document.sources.size() == 1);
        CHECK(*document.sources[0].bytes == simple_type1());
        CHECK(TempoMap::build(sequence).tempo_at(0) == 500000);
    }

    TEST_CASE("type 0 is one track; type 2 tracks become independent sequences") {
        SmfBuilder type0;
        type0.format = 0;
        type0.tracks.push_back(concat({{0x00, 0x90, 0x3C, 0x64}, {0x60, 0x80, 0x3C, 0x00}, end_of_track}));
        auto zero = import(type0.build());
        REQUIRE(zero);
        CHECK(zero->document.sequences.size() == 1);
        CHECK(zero->document.sequences[0].tracks.size() == 1);

        SmfBuilder type2;
        type2.format = 2;
        type2.tracks.push_back(concat({{0x00, 0xFF, 0x51, 0x03, 0x06, 0x1A, 0x80}, end_of_track}));
        type2.tracks.push_back(concat({{0x00, 0xFF, 0x59, 0x02, 0xFC, 0x01}, end_of_track}));
        auto two = import(type2.build());
        REQUIRE(two);
        CHECK(two->document.sequences.size() == 2);
    }

    TEST_CASE("PPQ values are preserved exactly") {
        for (const int ppq_value : {1, 24, 96, 480, 960, 32767}) {
            SmfBuilder builder;
            const auto ppq = static_cast<std::uint16_t>(ppq_value);
            builder.division = ppq;
            builder.tracks.push_back(end_of_track);
            auto result = import(builder.build());
            REQUIRE(result);
            CHECK(result->document.sequences[0].division.ppq == ppq);
        }
    }

    TEST_CASE("every SMPTE frame code imports with absolute timing") {
        for (const int code : {-24, -25, -29, -30}) {
            SmfBuilder builder;
            builder.division = static_cast<std::uint16_t>((static_cast<std::uint8_t>(static_cast<std::int8_t>(code)) << 8) | 100);
            builder.tracks.push_back(concat({{0x00, 0x90, 0x3C, 0x64}, {0x97, 0x38, 0x80, 0x3C, 0x00}, end_of_track}));
            auto result = import(builder.build());
            REQUIRE(result);
            const auto& division = result->document.sequences[0].division;
            CHECK_FALSE(division.is_ppq());
            CHECK(static_cast<int>(division.smpte_format) == code);
            CHECK(division.ticks_per_frame == 100);
        }
        SmfBuilder golden;
        golden.division = static_cast<std::uint16_t>((0xE3 << 8) | 100); // -29, 100 ticks/frame
        golden.tracks.push_back(end_of_track);
        auto result = import(golden.build());
        REQUIRE(result);
        const auto map = TempoMap::build(result->document.sequences[0]);
        CHECK(map.tick_to_microseconds(3000) == 1001000);
    }

    TEST_CASE("unknown chunks and header extensions are skipped and reported") {
        SmfBuilder builder;
        builder.header_extension = {0xAA, 0xBB};
        builder.extra_chunks_before_tracks.push_back({'X', 'Y', 'Z', 'W', 0, 0, 0, 3, 1, 2, 3});
        builder.tracks.push_back(concat({{0x00, 0x90, 0x3C, 0x64}, {0x10, 0x80, 0x3C, 0x00}, end_of_track}));
        auto result = import(builder.build());
        REQUIRE(result);
        CHECK(result->report.skipped_chunks == 1);
        CHECK(result->report.header_extension);
        CHECK(pair_notes(result->document.sequences[0]).notes.size() == 1);
    }

    TEST_CASE("RMID wrapper with padding is unwrapped and the wrapper kept for Original export") {
        SmfBuilder inner;
        inner.format = 0;
        inner.tracks.push_back(concat({{0x00, 0x90, 0x3C, 0x64}, {0x10, 0x80, 0x3C, 0x00}, end_of_track}));
        Bytes smf = inner.build();
        Bytes riff = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'R', 'M', 'I', 'D'};
        // An odd-sized leading chunk forces a RIFF pad byte.
        riff.insert(riff.end(), {'I', 'N', 'F', 'O', 3, 0, 0, 0, 'a', 'b', 'c', 0});
        riff.insert(riff.end(), {'d', 'a', 't', 'a'});
        const auto size = static_cast<std::uint32_t>(smf.size());
        for (int shift = 0; shift < 32; shift += 8)
            riff.push_back(static_cast<std::uint8_t>(size >> shift));
        riff.insert(riff.end(), smf.begin(), smf.end());
        const auto total = static_cast<std::uint32_t>(riff.size() - 8);
        for (int shift = 0; shift < 4; ++shift)
            riff[4 + static_cast<std::size_t>(shift)] = static_cast<std::uint8_t>(total >> (8 * shift));
        auto result = midi::import_midi(riff, "song.rmi");
        REQUIRE(result);
        CHECK(result->report.container == midi::Container::rmid);
        CHECK(result->document.sources[0].media_type == "rmid");
        midi::ExportOptions original;
        original.mode = midi::ExportMode::original;
        auto bytes = midi::export_midi(result->document, original);
        REQUIRE(bytes);
        CHECK(*bytes == riff);
    }

    TEST_CASE("invalid inputs are rejected or labeled, never called lossless") {
        CHECK_FALSE(import(Bytes{'n', 'o', 'p', 'e'}));
        SmfBuilder zero_tracks;
        CHECK_FALSE(import(zero_tracks.build()));
        // Truncated track: declared length longer than the data.
        Bytes truncated = simple_type1();
        truncated.resize(truncated.size() - 5);
        auto partial = import(truncated);
        if (partial) {
            CHECK(partial->report.reader_result == "incomplete");
            CHECK_FALSE(partial->report.semantic_model_complete);
        }
        // Overlong variable-length delta (5 bytes) makes the track unreadable from there.
        SmfBuilder overlong;
        overlong.tracks.push_back(concat({{0x81, 0x81, 0x81, 0x81, 0x01, 0x90, 0x3C, 0x64}, end_of_track}));
        auto bad = import(overlong.build());
        if (bad)
            CHECK(bad->report.reader_result != "validated");
        midi::ParseLimits limits;
        limits.max_bytes = 10;
        CHECK(midi::import_midi(simple_type1(), "x.mid", limits).error().code == ErrorCode::limit_exceeded);
        // The event limit is enforced before events are materialized (running status: 2 bytes each).
        Bytes packed{0x00, 0xC0, 0x01};
        for (int index = 0; index < 5000; ++index) {
            packed.push_back(0x00);
            packed.push_back(0x02);
        }
        SmfBuilder dense;
        dense.tracks.push_back(concat({packed, end_of_track}));
        midi::ParseLimits event_limits;
        event_limits.max_events = 1000;
        CHECK(midi::import_midi(dense.build(), "x.mid", event_limits).error().code == ErrorCode::limit_exceeded);
        CHECK(import(dense.build()).has_value());
    }

    TEST_CASE("malformed tempo payload is preserved, flagged and ignored for timing") {
        SmfBuilder builder;
        builder.tracks.push_back(concat({{0x00, 0xFF, 0x51, 0x02, 0x07, 0xA1}, {0x00, 0x90, 0x3C, 0x64},
                                         {0x83, 0x60, 0x80, 0x3C, 0x00}, end_of_track}));
        auto result = import(builder.build());
        REQUIRE(result);
        const auto& events = result->document.sequences[0].tracks[0].events;
        REQUIRE_FALSE(events.empty());
        CHECK((events[0].flags & event_flag::malformed) != 0);
        CHECK(TempoMap::build(result->document.sequences[0]).tempo_at(0) == 500000);
        CHECK(pair_notes(result->document.sequences[0]).notes.size() == 1);
    }

    TEST_CASE("running status is decoded") {
        SmfBuilder builder;
        builder.tracks.push_back(concat({{0x00, 0x90, 0x3C, 0x64}, {0x10, 0x3E, 0x64}, {0x10, 0x3C, 0x00},
                                         {0x10, 0x3E, 0x00}, end_of_track}));
        auto result = import(builder.build());
        REQUIRE(result);
        CHECK(pair_notes(result->document.sequences[0]).notes.size() == 2);
    }

    TEST_CASE("track end after the last event comes from end-of-track") {
        SmfBuilder builder;
        builder.tracks.push_back(concat({{0x00, 0x90, 0x3C, 0x64}, {0x10, 0x80, 0x3C, 0x00}, {0x83, 0x60, 0xFF, 0x2F, 0x00}}));
        auto result = import(builder.build());
        REQUIRE(result);
        CHECK(result->document.sequences[0].tracks[0].end_tick == 16 + 480);
    }
}

TEST_SUITE("export") {
    TEST_CASE("golden 9 part: unedited Original export equals imported bytes") {
        auto result = import(simple_type1());
        REQUIRE(result);
        midi::ExportOptions options;
        options.mode = midi::ExportMode::original;
        auto bytes = midi::export_midi(result->document, options);
        REQUIRE(bytes);
        CHECK(*bytes == simple_type1());
    }

    TEST_CASE("preserving export round-trips every event class and is deterministic") {
        auto result = import(simple_type1());
        REQUIRE(result);
        midi::ExportReport report;
        auto first = midi::export_midi(result->document, {}, &report);
        REQUIRE(first);
        auto second = midi::export_midi(result->document, {});
        REQUIRE(second);
        CHECK(*first == *second);
        CHECK(report.format == 1);
        auto reread = import(*first);
        REQUIRE(reread);
        CHECK(content(reread->document) == content(result->document));
        CHECK(reread->document.sequences[0].tracks[1].end_tick == result->document.sequences[0].tracks[1].end_tick);
    }

    TEST_CASE("SMPTE division round-trips") {
        SmfBuilder builder;
        builder.division = static_cast<std::uint16_t>((0xE7 << 8) | 40); // -25 fps, 40 ticks/frame
        builder.tracks.push_back(concat({{0x00, 0x90, 0x3C, 0x64}, {0x50, 0x80, 0x3C, 0x00}, end_of_track}));
        auto result = import(builder.build());
        REQUIRE(result);
        auto bytes = midi::export_midi(result->document, {});
        REQUIRE(bytes);
        auto reread = import(*bytes);
        REQUIRE(reread);
        CHECK(reread->document.sequences[0].division == result->document.sequences[0].division);
    }

    TEST_CASE("type 2 export keeps independent sequences") {
        SmfBuilder type2;
        type2.format = 2;
        type2.tracks.push_back(concat({{0x00, 0x90, 0x3C, 0x64}, {0x10, 0x80, 0x3C, 0x00}, end_of_track}));
        type2.tracks.push_back(concat({{0x00, 0x90, 0x40, 0x64}, {0x20, 0x80, 0x40, 0x00}, end_of_track}));
        auto result = import(type2.build());
        REQUIRE(result);
        midi::ExportReport report;
        auto bytes = midi::export_midi(result->document, {}, &report);
        REQUIRE(bytes);
        CHECK(report.format == 2);
        auto reread = import(*bytes);
        REQUIRE(reread);
        CHECK(reread->document.sequences.size() == 2);
        CHECK(content(reread->document) == content(result->document));
    }

    TEST_CASE("canonical type 1 moves tempo and meter to a conductor track") {
        SmfBuilder type0;
        type0.format = 0;
        type0.tracks.push_back(concat({{0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}, {0x00, 0x90, 0x3C, 0x64},
                                       {0x10, 0x80, 0x3C, 0x00}, end_of_track}));
        auto result = import(type0.build());
        REQUIRE(result);
        midi::ExportOptions options;
        options.mode = midi::ExportMode::compatibility;
        auto bytes = midi::export_midi(result->document, options);
        REQUIRE(bytes);
        auto reread = import(*bytes);
        REQUIRE(reread);
        REQUIRE(reread->document.sequences[0].tracks.size() == 2);
        CHECK(reread->document.sequences[0].tracks[0].events.size() == 1);
        CHECK(reread->document.sequences[0].tracks[0].events[0].is_meta(meta_type::tempo));
    }

    TEST_CASE("type 0 compatibility export merges tracks in stable order") {
        auto result = import(simple_type1());
        REQUIRE(result);
        midi::ExportOptions options;
        options.mode = midi::ExportMode::compatibility;
        options.profile = midi::CompatibilityProfile::type0;
        midi::ExportReport report;
        auto bytes = midi::export_midi(result->document, options, &report);
        REQUIRE(bytes);
        CHECK(report.format == 0);
        auto reread = import(*bytes);
        REQUIRE(reread);
        CHECK(reread->document.sequences[0].tracks.size() == 1);
        CHECK(pair_notes(reread->document.sequences[0]).notes.size() == 2);
    }

    TEST_CASE("preserving export refuses malformed events; compatibility repairs and reports") {
        SmfBuilder builder;
        builder.tracks.push_back(concat({{0x00, 0xFF, 0x51, 0x02, 0x07, 0xA1}, {0x00, 0x90, 0x3C, 0x64}, end_of_track}));
        auto result = import(builder.build());
        REQUIRE(result);
        CHECK_FALSE(midi::export_midi(result->document, {}));
        midi::ExportOptions options;
        options.mode = midi::ExportMode::compatibility;
        midi::ExportReport report;
        auto bytes = midi::export_midi(result->document, options, &report);
        REQUIRE(bytes);
        CHECK(report.events_omitted == 1);
        bool closed = false;
        for (const auto& diagnostic : report.diagnostics)
            closed = closed || diagnostic.code == "repair.closed_notes";
        CHECK(closed);
    }

    TEST_CASE("export-selected-range trims crossing notes, initializes state and rebases") {
        auto document = make_blank_document();
        const TrackId track = document.sequences[0].tracks[0].id;
        ChangeBuilder builder(document, "fixture");
        builder.insert(track, events::program_change(0, 0, 33));
        builder.insert(track, events::control_change(10, 0, 7, 90));
        builder.add_note(track, 0, 2000, 0, 60, 100);   // crosses the start
        builder.add_note(track, 1100, 1200, 0, 62, 100); // inside
        builder.add_note(track, 1900, 2500, 0, 64, 100); // crosses the end
        REQUIRE(apply_change_set(document, builder.finish()));
        midi::ExportOptions options;
        options.range = TimeRange{1000, 2000};
        midi::ExportReport report;
        auto bytes = midi::export_midi(document, options, &report);
        REQUIRE(bytes);
        CHECK(report.notes_trimmed == 2);
        auto reread = import(*bytes);
        REQUIRE(reread);
        const auto& sequence = reread->document.sequences[0];
        const auto pairing = pair_notes(sequence);
        REQUIRE(pairing.notes.size() == 3);
        CHECK(pairing.notes[0].start == 0);
        CHECK(pairing.notes[0].end == 1000);
        CHECK(pairing.notes[2].end == 1000);
        CHECK(sequence.tracks[0].end_tick == 1000);
        bool program = false;
        bool volume = false;
        for (const auto& event : sequence.tracks[0].events) {
            program = program || (event.is_channel() && event.message() == ChannelMessage::program_change && event.tick == 0);
            volume = volume || (event.is_channel() && event.message() == ChannelMessage::control_change && event.tick == 0);
        }
        CHECK(program);
        CHECK(volume);
    }

    TEST_CASE("export-selected-range replays RPN and NRPN state as select-then-data blocks (code-core-3)") {
        auto document = make_blank_document();
        const TrackId track = document.sequences[0].tracks[0].id;
        ChangeBuilder builder(document, "fixture");
        Tick tick = 0;
        const auto cc = [&](std::uint8_t controller, std::uint8_t value) {
            builder.insert(track, events::control_change(tick++, 0, controller, value));
        };
        // Pitch-bend range (RPN 0,0) first set to 2, then to 12; fine tuning (RPN 0,1) 64/0;
        // then an NRPN (1,8) = 50, which stays the selected parameter at the range start.
        cc(101, 0), cc(100, 0), cc(6, 2), cc(38, 0), cc(6, 12);
        cc(101, 0), cc(100, 1), cc(6, 64), cc(38, 0);
        cc(99, 1), cc(98, 8), cc(6, 50);
        builder.add_note(track, 1000, 1200, 0, 60, 100);
        REQUIRE(apply_change_set(document, builder.finish()));
        midi::ExportOptions options;
        options.range = TimeRange{1000, 2000};
        auto bytes = midi::export_midi(document, options);
        REQUIRE(bytes);
        auto reread = import(*bytes);
        REQUIRE(reread);

        // Replays the controllers in file order through the data-entry protocol.
        struct Replay {
            int active = 0; // 1 registered, 2 non-registered
            int rpn_msb = -1, rpn_lsb = -1, nrpn_msb = -1, nrpn_lsb = -1;
            std::map<std::tuple<int, int, int>, std::pair<int, int>> values;
            int orphan_data = 0; // data entry before any parameter was selected
            void apply(int controller, int value) {
                if (controller == 101) { rpn_msb = value; active = 1; }
                else if (controller == 100) { rpn_lsb = value; active = 1; }
                else if (controller == 99) { nrpn_msb = value; active = 2; }
                else if (controller == 98) { nrpn_lsb = value; active = 2; }
                else if (controller == 6 || controller == 38) {
                    if (active == 0) { ++orphan_data; return; }
                    const auto key = active == 1 ? std::tuple{1, rpn_msb, rpn_lsb} : std::tuple{2, nrpn_msb, nrpn_lsb};
                    auto& stored = values.try_emplace(key, -1, -1).first->second;
                    (controller == 6 ? stored.first : stored.second) = value;
                }
            }
        };
        Replay replay;
        for (const auto& event : reread->document.sequences[0].tracks[0].events)
            if (event.is_channel() && event.message() == ChannelMessage::control_change && event.tick == 0)
                replay.apply(event.data1(), event.data2());
        CHECK(replay.orphan_data == 0); // never CC 6/38 before its parameter select
        CHECK(replay.values[std::tuple{1, 0, 0}] == std::pair{12, 0}); // the last value, not the first
        CHECK(replay.values[std::tuple{1, 0, 1}] == std::pair{64, 0});
        CHECK(replay.values[std::tuple{2, 1, 8}].first == 50);
        // The parameter selected at the range start is still selected after the replay.
        CHECK(replay.active == 2);
        CHECK(replay.nrpn_msb == 1);
        CHECK(replay.nrpn_lsb == 8);
    }

    TEST_CASE("the craigsapp/midifile oracle agrees with exported files") {
        auto document = make_blank_document();
        const TrackId track = document.sequences[0].tracks[0].id;
        ChangeBuilder builder(document, "fixture");
        for (int index = 0; index < 32; ++index)
            builder.add_note(track, index * 240, index * 240 + 200, static_cast<std::uint8_t>(index % 4),
                             static_cast<std::uint8_t>(48 + index), 90);
        REQUIRE(apply_change_set(document, builder.finish()));
        auto bytes = midi::export_midi(document, {});
        REQUIRE(bytes);
        std::stringstream stream(std::string(bytes->begin(), bytes->end()));
        smf::MidiFile oracle;
        REQUIRE(oracle.read(stream));
        CHECK(oracle.getTicksPerQuarterNote() == 960);
        oracle.joinTracks();
        oracle.linkNotePairs();
        int notes = 0;
        for (int index = 0; index < oracle.getEventCount(0); ++index)
            if (oracle[0][index].isNoteOn())
                ++notes;
        CHECK(notes == 32);
    }
}
