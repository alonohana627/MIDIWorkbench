#include "test_support.hpp"

#include "mwb/core/change_set.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/core/serialization.hpp"
#include "mwb/music/analysis.hpp"
#include "mwb/music/chord.hpp"
#include "mwb/music/fingerprint.hpp"
#include "mwb/music/general_midi.hpp"
#include "mwb/music/grid.hpp"
#include "mwb/music/json_fields.hpp"
#include "mwb/music/motif.hpp"
#include "mwb/music/ppq.hpp"
#include "mwb/music/progression.hpp"
#include "mwb/music/scale.hpp"
#include "mwb/music/transform.hpp"
#include "mwb/music/voice.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <tuple>

using namespace mwb;
using namespace mwb::music;

namespace {

struct NoteSpec {
    Tick start = 0;
    Tick end = 0;
    int pitch = 60;
    int velocity = 100;
    std::uint8_t channel = 0;
};

/// One sequence with one track holding the given notes (note ends ordered before note starts).
DocumentData make_document(const std::vector<NoteSpec>& notes, std::int32_t ppq = 960, bool drum = false) {
    DocumentData document;
    document.id = DocumentId::generate();
    Sequence sequence;
    sequence.id = SequenceId::generate();
    sequence.division = Division::make_ppq(ppq);
    Track track;
    track.id = TrackId::generate();
    track.is_drum = drum;
    std::vector<Event> list;
    for (const auto& note : notes) {
        list.push_back(events::note_on(note.start, note.channel, static_cast<std::uint8_t>(note.pitch),
                                       static_cast<std::uint8_t>(note.velocity)));
        list.push_back(events::note_off(note.end, note.channel, static_cast<std::uint8_t>(note.pitch)));
    }
    std::ranges::stable_sort(list, [](const Event& a, const Event& b) {
        return std::make_tuple(a.tick, a.is_note_start()) < std::make_tuple(b.tick, b.is_note_start());
    });
    for (std::size_t index = 0; index < list.size(); ++index)
        list[index].order_key = (index + 1) * order_key_spacing;
    track.end_tick = list.empty() ? 0 : list.back().tick;
    track.events = std::move(list);
    sequence.end_tick = track.end_tick;
    sequence.tracks.push_back(std::move(track));
    document.sequences.push_back(std::move(sequence));
    return document;
}

TargetSelection select_all(const DocumentData& document) {
    TargetSelection selection;
    selection.sequence = document.sequences.front().id;
    return selection;
}

DocumentData applied(const DocumentData& document, const ChangeSet& changes) {
    DocumentData copy = document;
    const auto status = apply_change_set(copy, changes);
    REQUIRE(status.has_value());
    return copy;
}

using SimpleNote = std::tuple<Tick, Tick, int, int>; // start, end, pitch, velocity

std::vector<SimpleNote> notes_of(const DocumentData& document) {
    std::vector<SimpleNote> result;
    for (const auto& note : pair_notes(document.sequences.front()).notes)
        result.emplace_back(note.start, note.end, note.pitch, note.velocity);
    std::ranges::sort(result);
    return result;
}

DocumentData run(const DocumentData& document, const Result<TransformPreview>& preview) {
    REQUIRE(preview.has_value());
    return applied(document, preview->changes);
}

std::vector<int> offsets(std::initializer_list<int> values) { return std::vector<int>(values); }

ProgressionStep chord_step(int root, ChordType type, Tick duration = 3840) {
    ProgressionStep step;
    step.root_pitch_class = root;
    step.type = type;
    step.duration = duration;
    return step;
}

} // namespace

TEST_SUITE("scales") {
    TEST_CASE("the nineteen shipped scales have exactly the specified masks") {
        const std::vector<std::pair<std::string_view, std::vector<int>>> expected{
            {"major", offsets({0, 2, 4, 5, 7, 9, 11})},
            {"natural_minor", offsets({0, 2, 3, 5, 7, 8, 10})},
            {"harmonic_minor", offsets({0, 2, 3, 5, 7, 8, 11})},
            {"melodic_minor", offsets({0, 2, 3, 5, 7, 9, 11})},
            {"dorian", offsets({0, 2, 3, 5, 7, 9, 10})},
            {"phrygian", offsets({0, 1, 3, 5, 7, 8, 10})},
            {"lydian", offsets({0, 2, 4, 6, 7, 9, 11})},
            {"mixolydian", offsets({0, 2, 4, 5, 7, 9, 10})},
            {"locrian", offsets({0, 1, 3, 5, 6, 8, 10})},
            {"phrygian_dominant", offsets({0, 1, 4, 5, 7, 8, 10})},
            {"double_harmonic_major", offsets({0, 1, 4, 5, 7, 8, 11})},
            {"hungarian_minor", offsets({0, 2, 3, 6, 7, 8, 11})},
            {"major_pentatonic", offsets({0, 2, 4, 7, 9})},
            {"minor_pentatonic", offsets({0, 3, 5, 7, 10})},
            {"blues_hexatonic", offsets({0, 3, 5, 6, 7, 10})},
            {"whole_tone", offsets({0, 2, 4, 6, 8, 10})},
            {"diminished_half_whole", offsets({0, 1, 3, 4, 6, 7, 9, 10})},
            {"diminished_whole_half", offsets({0, 2, 3, 5, 6, 8, 9, 11})},
            {"chromatic", offsets({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11})},
        };
        REQUIRE(builtin_scales().size() == expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            const auto& definition = builtin_scales()[index];
            CAPTURE(definition.id);
            CHECK(definition.id == expected[index].first);
            const auto mask = interval_mask_from_offsets(expected[index].second);
            REQUIRE(mask.has_value());
            CHECK(definition.interval_mask == *mask);
            const auto scale = Scale::builtin(definition.id, 0);
            REQUIRE(scale.has_value());
            CHECK(scale->offsets() == expected[index].second);
        }
        CHECK(builtin_scales()[0].interval_mask == 0x0AB5);
    }

    TEST_CASE("golden 5: C major accepts 60, 62, 64 and rejects 61") {
        const auto major = Scale::builtin("major", 0);
        REQUIRE(major.has_value());
        CHECK(major->contains(60));
        CHECK(major->contains(62));
        CHECK(major->contains(64));
        CHECK_FALSE(major->contains(61));
        const auto e_phrygian_dominant = Scale::builtin("phrygian_dominant", 4);
        REQUIRE(e_phrygian_dominant.has_value());
        CHECK(e_phrygian_dominant->contains(64));
        CHECK(e_phrygian_dominant->contains(65));
        CHECK(e_phrygian_dominant->contains(68));
        CHECK_FALSE(e_phrygian_dominant->contains(66));
    }

    TEST_CASE("golden 5: selecting and highlighting a scale leaves every event unchanged") {
        const auto document = make_document({{0, 480, 61}, {480, 960, 62}});
        const auto before_events = fingerprint_document(document, FingerprintProfile::exact_event_content);
        const auto major = Scale::builtin("major", 0);
        REQUIRE(major.has_value());
        const auto changes = set_sequence_scale(document, document.sequences.front().id, *major, true, true);
        REQUIRE(changes.has_value());
        REQUIRE(changes->operations.size() == 1);
        CHECK(std::holds_alternative<SequencePropertiesChange>(changes->operations.front()));
        const auto after = applied(document, *changes);
        CHECK(after.sequences.front().tracks == document.sequences.front().tracks);
        CHECK(fingerprint_document(after, FingerprintProfile::exact_event_content) == before_events);
        CHECK(after.sequences.front().settings.highlight_scale);
        CHECK(after.sequences.front().scale_mask == 0x0AB5);
        const auto restored = applied(after, invert(*changes));
        CHECK(semantic_hash(restored) == semantic_hash(document));
    }

    TEST_CASE("nearest member ties go downward by default and upward on request") {
        const auto major = *Scale::builtin("major", 0);
        CHECK(major.nearest_member(61) == 60);
        CHECK(major.nearest_member(61, NearestTie::upward) == 62);
        CHECK(major.nearest_member(66) == 65);
        CHECK(major.nearest_member(66, NearestTie::upward) == 67);
        CHECK(major.nearest_member(64) == 64);
        const auto pentatonic = *Scale::builtin("major_pentatonic", 0);
        CHECK(pentatonic.nearest_member(65) == 64);
        CHECK(pentatonic.nearest_member(65, NearestTie::upward) == 64);
        CHECK(pentatonic.nearest_member(66, NearestTie::downward) == 67);
    }

    TEST_CASE("diatonic transpose moves by ordered degrees across octaves with an out-of-scale policy") {
        const auto major = *Scale::builtin("major", 0);
        CHECK(major.transpose_by_degrees(60, 2, OutOfScalePolicy::preserve_chromatic_offset) == 64);
        CHECK(major.transpose_by_degrees(71, 1, OutOfScalePolicy::preserve_chromatic_offset) == 72);
        CHECK(major.transpose_by_degrees(60, -1, OutOfScalePolicy::preserve_chromatic_offset) == 59);
        CHECK(major.transpose_by_degrees(60, 7, OutOfScalePolicy::preserve_chromatic_offset) == 72);
        CHECK(major.transpose_by_degrees(61, 1, OutOfScalePolicy::preserve_chromatic_offset) == 63);
        CHECK(major.transpose_by_degrees(61, 1, OutOfScalePolicy::conform_first) == 62);
        for (int pitch = 0; pitch < 128; ++pitch) {
            if (const auto ordinal = major.member_ordinal(pitch))
                CHECK(major.pitch_of_ordinal(*ordinal) == pitch);
        }
    }

    TEST_CASE("custom scales need one to twelve pitch classes including the root") {
        CHECK_FALSE(Scale::custom(0, 0).has_value());
        CHECK_FALSE(Scale::custom(0, 0b10).has_value());
        CHECK_FALSE(Scale::custom(12, 1).has_value());
        CHECK_FALSE(Scale::custom(0, 0x1001).has_value());
        const auto single = Scale::custom(2, 1);
        REQUIRE(single.has_value());
        CHECK(single->size() == 1);
        CHECK(single->contains(62));
        CHECK(single->nearest_member(68) == 62);
        CHECK(Scale::custom(0, 0x0FFF)->size() == 12);
    }

    TEST_CASE("diatonic chords stack every other degree; numerals only for heptatonic scales") {
        const auto triads = diatonic_chords(*Scale::builtin("major", 0), 3);
        REQUIRE(triads.has_value());
        REQUIRE(triads->size() == 7);
        CHECK((*triads)[0].roman_numeral == "I");
        CHECK((*triads)[1].roman_numeral == "ii");
        CHECK((*triads)[4].roman_numeral == "V");
        CHECK((*triads)[6].roman_numeral == "vii°");
        CHECK((*triads)[1].pitch_classes == std::vector<int>{2, 5, 9});
        const auto sevenths = diatonic_chords(*Scale::builtin("major", 0), 4);
        REQUIRE(sevenths.has_value());
        CHECK((*sevenths)[0].roman_numeral == "Imaj7");
        CHECK((*sevenths)[4].roman_numeral == "V7");
        CHECK((*sevenths)[6].roman_numeral == "viiø7");
        const auto pentatonic = diatonic_chords(*Scale::builtin("major_pentatonic", 0), 3);
        REQUIRE(pentatonic.has_value());
        CHECK(pentatonic->front().roman_numeral.empty());
        CHECK(pentatonic->front().label.find("stack every other scale degree") != std::string::npos);
    }
}

TEST_SUITE("chords") {
    TEST_CASE("every chord type produces its actual pitches") {
        const std::vector<std::pair<ChordType, std::vector<int>>> expected{
            {ChordType::major, {60, 64, 67}},
            {ChordType::minor, {60, 63, 67}},
            {ChordType::diminished, {60, 63, 66}},
            {ChordType::augmented, {60, 64, 68}},
            {ChordType::suspended_second, {60, 62, 67}},
            {ChordType::suspended_fourth, {60, 65, 67}},
            {ChordType::power_fifth, {60, 67}},
            {ChordType::major_sixth, {60, 64, 67, 69}},
            {ChordType::minor_sixth, {60, 63, 67, 69}},
            {ChordType::dominant_seventh, {60, 64, 67, 70}},
            {ChordType::major_seventh, {60, 64, 67, 71}},
            {ChordType::minor_seventh, {60, 63, 67, 70}},
            {ChordType::minor_major_seventh, {60, 63, 67, 71}},
            {ChordType::half_diminished_seventh, {60, 63, 66, 70}},
            {ChordType::diminished_seventh, {60, 63, 66, 69}},
            {ChordType::added_ninth, {60, 64, 67, 74}},
            {ChordType::minor_added_ninth, {60, 63, 67, 74}},
        };
        REQUIRE(chord_types().size() == expected.size());
        for (const auto& [type, pitches] : expected) {
            ChordSpec spec;
            spec.type = type;
            const auto voicing = voice_chord(spec);
            REQUIRE(voicing.has_value());
            CAPTURE(chord_type_info(type).id);
            CHECK(voicing->pitches == pitches);
            CHECK(voicing->out_of_range.empty());
        }
    }

    TEST_CASE("extensions, inversion, spread, bass and note count") {
        ChordSpec ninth;
        ninth.type = ChordType::dominant_seventh;
        ninth.extensions = chord_extension::ninth;
        CHECK(voice_chord(ninth)->pitches == std::vector<int>{60, 64, 67, 70, 74});
        CHECK(voice_chord(ninth)->symbol == "C9");
        ChordSpec thirteenth = ninth;
        thirteenth.type = ChordType::minor_seventh;
        thirteenth.extensions = chord_extension::ninth | chord_extension::thirteenth;
        CHECK(voice_chord(thirteenth)->symbol == "Cm13");

        ChordSpec first_inversion;
        first_inversion.inversion = 1;
        CHECK(voice_chord(first_inversion)->pitches == std::vector<int>{64, 67, 72});
        CHECK(voice_chord(first_inversion)->symbol == "C/E");

        ChordSpec drop_two;
        drop_two.type = ChordType::major_seventh;
        drop_two.spread = ChordSpread::drop_two;
        CHECK(voice_chord(drop_two)->pitches == std::vector<int>{55, 60, 64, 71});

        ChordSpec slash;
        slash.root_pitch = 57;
        slash.type = ChordType::minor_seventh;
        slash.bass_pitch_class = 7;
        CHECK(voice_chord(slash)->pitches == std::vector<int>{55, 57, 60, 64, 67});
        CHECK(voice_chord(slash)->symbol == "Am7/G");

        ChordSpec reduced = ninth;
        reduced.note_count = 3;
        CHECK(voice_chord(reduced)->pitches == std::vector<int>{60, 64, 70});
        ChordSpec doubled;
        doubled.note_count = 5;
        CHECK(voice_chord(doubled)->pitches == std::vector<int>{60, 64, 67, 72, 76});

        ChordSpec invalid;
        invalid.inversion = 3;
        CHECK_FALSE(voice_chord(invalid).has_value());
    }

    TEST_CASE("out-of-range chord tones are reported before commit") {
        ChordSpec high;
        high.root_pitch = 125;
        const auto voicing = voice_chord(high);
        REQUIRE(voicing.has_value());
        CHECK(voicing->out_of_range == std::vector<int>{129, 132});
        ChordSpec register_limited;
        register_limited.high_limit = 65;
        CHECK(voice_chord(register_limited)->out_of_range == std::vector<int>{67});

        const auto document = make_document({});
        const auto rejected = insert_chord(document, document.sequences.front().tracks.front().id, high, {});
        CHECK_FALSE(rejected.has_value());
        const auto skipped = insert_chord(document, document.sequences.front().tracks.front().id, high, {},
                                          OutOfRangeAction::skip);
        REQUIRE(skipped.has_value());
        CHECK(skipped->report.created == 1);
        CHECK(skipped->report.skipped == 2);
    }

    TEST_CASE("rendering applies strum offsets and the velocity profile") {
        ChordRenderSpec render;
        render.start = 0;
        render.duration = 480;
        render.strum = 60;
        render.velocity = 80;
        render.accent = 20;
        render.profile = VelocityProfile::ramp_up;
        const std::vector<int> pitches{60, 64, 67};
        const auto notes = render_chord(pitches, render);
        REQUIRE(notes.has_value());
        CHECK(*notes == std::vector<GeneratedNote>{{0, 480, 60, 80}, {30, 480, 64, 90}, {60, 480, 67, 100}});
        render.direction = StrumDirection::down;
        const auto down = render_chord(pitches, render);
        CHECK((*down)[0] == GeneratedNote{0, 480, 67, 100});
    }

    TEST_CASE("recognition is advisory and ranks alternatives such as C6 / Am7") {
        const std::vector<int> pitches{60, 64, 67, 69};
        const auto candidates = recognize_chord(pitch_class_mask(pitches), 0);
        REQUIRE(candidates.size() >= 2);
        CHECK(candidates[0].symbol == "C6");
        CHECK(candidates[1].symbol == "Am7/C");
        const auto bass_a = recognize_chord(pitch_class_mask(pitches), 9);
        CHECK(bass_a.front().symbol == "Am7");
        CHECK(recognize_chord(pitch_class_mask(std::vector<int>{60, 64, 67}), 0).front().symbol == "C");
        CHECK(recognize_chord(pitch_class_mask(std::vector<int>{60, 63, 67, 70}), 0).front().symbol == "Cm7");
        // Omitted fifth is allowed: C E Bb reads as C7.
        CHECK(recognize_chord(pitch_class_mask(std::vector<int>{60, 64, 70}), 0).front().symbol == "C7");
        CHECK(recognize_chord(pitch_class_mask(std::vector<int>{60}), 0).empty());
    }

    TEST_CASE("chord analysis segments onsets and offers alternatives") {
        const auto document = make_document({{0, 960, 60}, {0, 960, 64}, {0, 960, 67}, {960, 1920, 65}, {960, 1920, 69}, {960, 1920, 72}});
        const auto notes = pair_notes(document.sequences.front()).notes;
        const auto segments = analyze_chords(notes, 10);
        REQUIRE(segments.size() == 2);
        CHECK(segments[0].alternatives.front().symbol == "C");
        CHECK(segments[1].alternatives.front().symbol == "F");
        CHECK(segments[1].start == 960);
        CHECK(segments[1].end == 1920);
    }
}

TEST_SUITE("progression") {
    TEST_CASE("voice-leading dynamic program keeps common tones and reports its weights") {
        const std::vector<ProgressionStep> steps{chord_step(0, ChordType::major), chord_step(9, ChordType::minor)};
        VoiceLeadingOptions options;
        options.low_pitch = 60;
        options.high_pitch = 72;
        options.register_tolerance = 0;
        options.voice_count = 3;
        const auto result = voice_lead(steps, options);
        REQUIRE(result.has_value());
        REQUIRE(result->voicings.size() == 2);
        const auto& first = result->voicings[0];
        const auto& second = result->voicings[1];
        int movement = 0;
        int held = 0;
        for (std::size_t voice = 0; voice < 3; ++voice) {
            movement += std::abs(second[voice] - first[voice]);
            held += second[voice] == first[voice] ? 1 : 0;
        }
        CHECK(movement == 2);
        CHECK(held == 2);
        CHECK(result->total_cost == -2);
        CHECK(result->step_costs[0] + result->step_costs[1] == result->total_cost);
        CHECK(result->weights == VoiceLeadingWeights{});
        CHECK(result->candidate_limit == 64);
        CHECK(pitch_class_mask(second) == pitch_class_mask(std::vector<int>{57, 60, 64}));
    }

    TEST_CASE("progression with sevenths, inversion and register constraints") {
        std::vector<ProgressionStep> steps{chord_step(0, ChordType::major), chord_step(5, ChordType::major),
                                           chord_step(7, ChordType::dominant_seventh), chord_step(0, ChordType::major)};
        steps[1].inversion = 2; // F/C
        VoiceLeadingOptions options;
        options.low_pitch = 48;
        options.high_pitch = 72;
        const auto result = voice_lead(steps, options);
        REQUIRE(result.has_value());
        for (const auto& voicing : result->voicings) {
            CHECK(voicing.size() == 4);
            CHECK(std::ranges::is_sorted(voicing));
        }
        CHECK(result->voicings[1].front() % 12 == 0);
        const auto mask = pitch_class_mask(result->voicings[2]);
        for (const int pitch_class : {7, 11, 2, 5})
            CHECK((mask & (1u << pitch_class)) != 0);
        const auto rerun = voice_lead(steps, options);
        CHECK(rerun->voicings == result->voicings);
        const auto json = progression_to_json(steps, options);
        const auto parsed = progression_from_json(json);
        REQUIRE(parsed.has_value());
        CHECK(parsed->first == steps);
    }

    TEST_CASE("inserting a progression materializes notes and stores its recipe") {
        const auto document = make_document({});
        const std::vector<ProgressionStep> steps{chord_step(0, ChordType::major, 960), chord_step(7, ChordType::major, 960)};
        const auto preview = insert_progression(document, document.sequences.front().tracks.front().id, steps, {}, 0, 0);
        REQUIRE(preview.has_value());
        const auto after = run(document, preview);
        CHECK(after.sequences.front().recipes.size() == 1);
        CHECK(after.sequences.front().recipes.front().tool_id == "progression");
        CHECK(notes_of(after).size() == preview->report.created);
        CHECK(semantic_hash(applied(after, invert(preview->changes))) == semantic_hash(document));
        // The stored recipe regenerates the same notes (code-core-15): on the source it was
        // recorded from it replays exactly; on the result the source changed, so it needs consent.
        const auto& recipe = after.sequences.front().recipes.front();
        const auto replayed = replay_recipe(document, recipe, false);
        REQUIRE_MESSAGE(replayed, (replayed ? "" : replayed.error().describe()));
        CHECK(notes_of(run(document, *replayed)) == notes_of(after));
        CHECK(replay_recipe(after, recipe, false).error().code == ErrorCode::conflict);
        CHECK(replay_recipe(after, recipe, true).has_value());
    }
}

TEST_SUITE("transform") {
    TEST_CASE("quantize golden numbers: nearest grid, earlier tie, preserve duration") {
        const auto document = make_document({{100, 300, 60}, {120, 200, 62}, {130, 200, 64}, {365, 400, 65}});
        QuantizeParameters parameters;
        parameters.grid.step = {1, 16}; // 240 ticks at PPQ 960
        const auto preview = quantize(document, select_all(document), parameters);
        const auto after = run(document, preview);
        CHECK(notes_of(after) == std::vector<SimpleNote>{{0, 80, 62, 100}, {0, 200, 60, 100}, {240, 310, 64, 100},
                                                         {480, 515, 65, 100}});
        CHECK(preview->report.changed == 4);
        CHECK(preview->report.max_movement == 120);
    }

    TEST_CASE("quantize strength uses exact rationals and rounds half ticks away from zero") {
        const auto document = make_document({{100, 300, 60}, {101, 300, 62}});
        QuantizeParameters parameters;
        parameters.strength = {1, 2};
        parameters.preserve_duration = false;
        const auto preview = quantize(document, select_all(document), parameters);
        const auto after = run(document, preview);
        CHECK(notes_of(after) == std::vector<SimpleNote>{{50, 300, 60, 100}, {51, 300, 62, 100}});
        CHECK(preview->report.max_rounding_error == Rational{1, 2});
    }

    TEST_CASE("quantize swing pairs are anchored to the grid origin") {
        const auto document = make_document({{600, 700, 60}, {800, 900, 62}, {1500, 1600, 64}});
        QuantizeParameters parameters;
        parameters.grid.step = {1, 8};   // 480 ticks, pair span 960
        parameters.grid.swing = {2, 3}; // second point at 640
        const auto after = run(document, quantize(document, select_all(document), parameters));
        CHECK(notes_of(after) == std::vector<SimpleNote>{{640, 740, 60, 100}, {640, 740, 62, 100}, {1600, 1700, 64, 100}});
        auto grid = Grid::make(960, GridSpec{{1, 8}, {2, 3}, 120});
        REQUIRE(grid.has_value());
        CHECK(grid->point_tick(1) == 760);
        CHECK(grid->point_tick(2) == 1080);
        CHECK(grid->nearest_index(700) == 1);
        CHECK(grid->nearest_index(400) == 0);
    }

    TEST_CASE("quantize maximum movement, ends mode, and unselected notes") {
        const auto document = make_document({{100, 300, 60}, {230, 400, 62}, {10, 20, 64}});
        QuantizeParameters limited;
        limited.max_movement = 50;
        const auto preview = quantize(document, select_all(document), limited);
        REQUIRE(preview.has_value());
        CHECK(preview->report.skipped == 1);
        const auto after = run(document, preview);
        CHECK(notes_of(after) == std::vector<SimpleNote>{{0, 10, 64, 100}, {100, 300, 60, 100}, {240, 410, 62, 100}});

        QuantizeParameters both;
        both.target = QuantizeTarget::starts_and_ends;
        const auto clamped = quantize(document, select_all(document), both);
        REQUIRE(clamped.has_value());
        CHECK(clamped->report.clipped == 1);
        CHECK(std::get<1>(notes_of(run(document, clamped))[0]) == 1);

        const auto ids = pair_notes(document.sequences.front()).notes;
        auto only_one = select_all(document);
        only_one.notes = {ids.front().on_id};
        const auto single = run(document, quantize(document, only_one, QuantizeParameters{}));
        CHECK(notes_of(single) == std::vector<SimpleNote>{{0, 10, 64, 100}, {100, 300, 60, 100}, {230, 400, 62, 100}});
    }

    TEST_CASE("apply then invert restores the exact semantic hash") {
        const auto document = make_document({{100, 300, 60}, {130, 250, 64}, {470, 700, 67}});
        const auto original = semantic_hash(document);
        const auto preview = quantize(document, select_all(document), QuantizeParameters{});
        REQUIRE(preview.has_value());
        auto edited = applied(document, preview->changes);
        CHECK(semantic_hash(edited) != original);
        const auto restored = applied(edited, invert(preview->changes));
        CHECK(semantic_hash(restored) == original);
        CHECK(restored == document);
    }

    TEST_CASE("golden 7: reverse [100,250) inside [0,1000) gives [750,900)") {
        const auto document = make_document({{100, 250, 60}, {900, 1100, 62}});
        ReverseParameters parameters;
        parameters.range = TimeRange{0, 1000};
        const auto preview = reverse(document, select_all(document), parameters);
        REQUIRE(preview.has_value());
        CHECK(preview->report.skipped == 1);
        CHECK(notes_of(run(document, preview)) == std::vector<SimpleNote>{{750, 900, 60, 100}, {900, 1100, 62, 100}});
    }

    TEST_CASE("transposing by n and then -n restores the input") {
        const auto document = make_document({{0, 240, 60}, {240, 480, 64}, {240, 480, 67}});
        TransposeParameters up;
        up.amount = 5;
        const auto raised = run(document, transpose(document, select_all(document), up));
        CHECK(std::get<2>(notes_of(raised)[0]) == 65);
        TransposeParameters down;
        down.amount = -5;
        const auto restored = run(raised, transpose(raised, select_all(raised), down));
        CHECK(semantic_hash(restored) == semantic_hash(document));

        TransposeParameters degrees;
        degrees.mode = TransposeMode::scale_degrees;
        degrees.amount = 2;
        const auto diatonic = run(document, transpose(document, select_all(document), degrees));
        CHECK(notes_of(diatonic) == std::vector<SimpleNote>{{0, 240, 64, 100}, {240, 480, 67, 100}, {240, 480, 71, 100}});
    }

    TEST_CASE("transposition at the range edge rejects or constrains the whole displacement") {
        const auto document = make_document({{0, 240, 120}, {0, 240, 125}});
        TransposeParameters parameters;
        parameters.amount = 5;
        CHECK_FALSE(transpose(document, select_all(document), parameters).has_value());
        parameters.range_policy = RangePolicy::constrain;
        const auto after = run(document, transpose(document, select_all(document), parameters));
        CHECK(notes_of(after) == std::vector<SimpleNote>{{0, 240, 122, 100}, {0, 240, 127, 100}});

        const auto drums = make_document({{0, 240, 36}}, 960, true);
        TransposeParameters octave;
        octave.mode = TransposeMode::octaves;
        octave.amount = 1;
        const auto skipped = transpose(drums, select_all(drums), octave);
        REQUIRE(skipped.has_value());
        CHECK(skipped->report.skipped == 1);
        CHECK(skipped->changes.empty());
    }

    TEST_CASE("moves crossing tick 0 or pitch 127 constrain the entire displacement") {
        const auto document = make_document({{100, 200, 120}, {300, 400, 125}});
        MoveParameters parameters;
        parameters.ticks = -200;
        parameters.semitones = 5;
        const auto preview = move_notes(document, select_all(document), parameters);
        REQUIRE(preview.has_value());
        CHECK(notes_of(run(document, preview)) == std::vector<SimpleNote>{{0, 100, 122, 100}, {200, 300, 127, 100}});
        parameters.range_policy = RangePolicy::reject;
        CHECK_FALSE(move_notes(document, select_all(document), parameters).has_value());
    }

    TEST_CASE("invert pitch reflects around the axis and rejects out-of-range results by default") {
        const auto document = make_document({{0, 100, 64}, {100, 200, 10}});
        InvertPitchParameters parameters;
        parameters.axis_pitch = 60;
        const auto after = run(document, invert_pitch(document, select_all(document), parameters));
        CHECK(notes_of(after) == std::vector<SimpleNote>{{0, 100, 56, 100}, {100, 200, 110, 100}});
        parameters.axis_pitch = 90;
        CHECK_FALSE(invert_pitch(document, select_all(document), parameters).has_value());
        parameters.out_of_range = OutOfRangeAction::skip;
        const auto skipped = invert_pitch(document, select_all(document), parameters);
        REQUIRE(skipped.has_value());
        CHECK(skipped->report.skipped == 1);
    }

    TEST_CASE("velocity clamps sounding note-ons to 1..127 and reports clamps") {
        const auto document = make_document({{0, 100, 60, 100}, {100, 200, 62, 101}, {200, 300, 64, 20}});
        VelocityParameters add;
        add.mode = VelocityMode::add;
        add.value = 50;
        const auto added = change_velocity(document, select_all(document), add);
        REQUIRE(added.has_value());
        CHECK(added->report.clipped == 2);
        CHECK(notes_of(run(document, added)) == std::vector<SimpleNote>{{0, 100, 60, 127}, {100, 200, 62, 127}, {200, 300, 64, 70}});
        VelocityParameters scale;
        scale.mode = VelocityMode::scale;
        scale.factor = {1, 2};
        CHECK(std::get<3>(notes_of(run(document, change_velocity(document, select_all(document), scale)))[1]) == 51);
        VelocityParameters set_zero;
        set_zero.value = 0;
        CHECK_FALSE(change_velocity(document, select_all(document), set_zero).has_value());
        VelocityParameters ramp;
        ramp.mode = VelocityMode::ramp;
        ramp.ramp_start = 40;
        ramp.ramp_end = 80;
        CHECK(notes_of(run(document, change_velocity(document, select_all(document), ramp))) ==
              std::vector<SimpleNote>{{0, 100, 60, 40}, {100, 200, 62, 60}, {200, 300, 64, 80}});
        VelocityParameters compress;
        compress.mode = VelocityMode::compress;
        compress.range_low = 60;
        compress.range_high = 100;
        CHECK(notes_of(run(document, change_velocity(document, select_all(document), compress))) ==
              std::vector<SimpleNote>{{0, 100, 60, 100}, {100, 200, 62, 100}, {200, 300, 64, 60}});
    }

    TEST_CASE("length set, legato and gap keep a one-tick minimum") {
        const auto document = make_document({{0, 100, 60}, {480, 580, 62}, {960, 1060, 64}});
        LengthParameters legato;
        legato.mode = LengthMode::legato_to_next;
        CHECK(notes_of(run(document, change_length(document, select_all(document), legato))) ==
              std::vector<SimpleNote>{{0, 480, 60, 100}, {480, 960, 62, 100}, {960, 1060, 64, 100}});
        LengthParameters gap;
        gap.mode = LengthMode::gap;
        gap.ticks = 10;
        CHECK(std::get<1>(notes_of(run(document, change_length(document, select_all(document), gap)))[0]) == 470);
        LengthParameters add;
        add.mode = LengthMode::add;
        add.ticks = -500;
        const auto clipped = change_length(document, select_all(document), add);
        REQUIRE(clipped.has_value());
        CHECK(clipped->report.clipped == 3);
        CHECK(std::get<1>(notes_of(run(document, clipped))[0]) == 1);
        LengthParameters zero;
        zero.ticks = 0;
        CHECK_FALSE(change_length(document, select_all(document), zero).has_value());
    }

    TEST_CASE("humanize is deterministic for a seed and keeps chord groups and line order") {
        const auto document = make_document({{0, 480, 60}, {0, 480, 64}, {0, 480, 67}, {10, 400, 72}, {20, 400, 74}, {960, 1200, 60}});
        HumanizeParameters parameters;
        parameters.seed = 1234;
        parameters.timing_range = 100;
        parameters.velocity_range = 20;
        parameters.duration_range = 30;
        parameters.chord_tolerance = 0;
        const auto first = run(document, humanize(document, select_all(document), parameters));
        const auto second = run(document, humanize(document, select_all(document), parameters));
        CHECK(semantic_hash(first) == semantic_hash(second));
        parameters.seed = 99;
        const auto other = run(document, humanize(document, select_all(document), parameters));
        CHECK(semantic_hash(other) != semantic_hash(first));
        // The three-note chord stays grouped and the line keeps its onset order.
        const auto notes = pair_notes(first.sequences.front()).notes;
        std::set<Tick> chord_starts;
        std::vector<Tick> order;
        for (const auto& note : notes) {
            if (note.pitch == 64 || note.pitch == 67 || (note.pitch == 60 && note.start < 600))
                chord_starts.insert(note.start);
        }
        CHECK(chord_starts.size() == 1);
        for (const auto& note : notes)
            order.push_back(note.start);
        const auto original = pair_notes(document.sequences.front()).notes;
        for (std::size_t a = 0; a < original.size(); ++a)
            for (std::size_t b = 0; b < original.size(); ++b)
                if (original[a].start < original[b].start) {
                    const auto find = [&](EventId id) {
                        return std::ranges::find_if(notes, [&](const NoteSpan& n) { return n.on_id == id; })->start;
                    };
                    CHECK(find(original[a].on_id) < find(original[b].on_id));
                }
        for (const auto& note : notes) {
            CHECK(note.velocity >= 1);
            CHECK(note.end > note.start);
        }
    }

    TEST_CASE("strum spreads chord onsets and keeps ends") {
        const auto document = make_document({{0, 480, 60}, {0, 480, 64}, {0, 480, 67}});
        StrumParameters parameters;
        parameters.spread = 60;
        CHECK(notes_of(run(document, strum(document, select_all(document), parameters))) ==
              std::vector<SimpleNote>{{0, 480, 60, 100}, {30, 480, 64, 100}, {60, 480, 67, 100}});
        parameters.order = StrumOrder::random;
        parameters.seed = 7;
        const auto first = run(document, strum(document, select_all(document), parameters));
        const auto second = run(document, strum(document, select_all(document), parameters));
        CHECK(semantic_hash(first) == semantic_hash(second));
    }

    TEST_CASE("arpeggiate renders concrete notes deterministically") {
        const auto document = make_document({{0, 960, 60}, {0, 960, 64}, {0, 960, 67}});
        ArpeggiateParameters parameters;
        const auto preview = arpeggiate(document, select_all(document), parameters);
        REQUIRE(preview.has_value());
        CHECK(preview->report.removed == 3);
        CHECK(notes_of(run(document, preview)) ==
              std::vector<SimpleNote>{{0, 120, 60, 100}, {240, 360, 64, 100}, {480, 600, 67, 100}, {720, 840, 60, 100}});
        parameters.pattern = ArpeggioPattern::random;
        parameters.seed = 42;
        parameters.octaves = 2;
        const auto first = run(document, arpeggiate(document, select_all(document), parameters));
        const auto second = run(document, arpeggiate(document, select_all(document), parameters));
        CHECK(fingerprint_document(first, FingerprintProfile::exact_event_content) ==
              fingerprint_document(second, FingerprintProfile::exact_event_content));
        CHECK(notes_of(first) == notes_of(second));
    }

    TEST_CASE("Euclidean rhythm uses the maximally even pattern") {
        const auto pattern = euclidean_pattern(8, 3, 0);
        REQUIRE(pattern.has_value());
        CHECK(*pattern == std::vector<bool>{true, false, false, true, false, false, true, false});
        CHECK(*euclidean_pattern(8, 3, 1) == std::vector<bool>{false, true, false, false, true, false, false, true});
        const auto document = make_document({});
        auto selection = select_all(document);
        selection.tracks = {document.sequences.front().tracks.front().id};
        EuclideanParameters parameters;
        parameters.steps = 8;
        parameters.pulses = 3;
        parameters.pitch = 36;
        const auto after = run(document, euclidean_rhythm(document, selection, parameters));
        CHECK(notes_of(after) == std::vector<SimpleNote>{{0, 120, 36, 120}, {720, 840, 36, 90}, {1440, 1560, 36, 90}});
    }

    TEST_CASE("time stretch by a rational reports rounding") {
        const auto document = make_document({{100, 201, 60}});
        TimeStretchParameters parameters;
        parameters.factor = {3, 2};
        const auto preview = time_stretch(document, select_all(document), parameters);
        REQUIRE(preview.has_value());
        CHECK(notes_of(run(document, preview)) == std::vector<SimpleNote>{{150, 302, 60, 100}});
        CHECK(preview->report.max_rounding_error == Rational{1, 2});
        parameters.factor = {0, 1};
        CHECK_FALSE(time_stretch(document, select_all(document), parameters).has_value());
    }

    TEST_CASE("rotate wraps inside the range and splits boundary-crossing parts") {
        const auto document = make_document({{800, 900, 60}, {600, 800, 62}});
        RotateParameters parameters;
        parameters.range = {0, 1000};
        parameters.shift = 300;
        const auto preview = rotate(document, select_all(document), parameters);
        REQUIRE(preview.has_value());
        CHECK(preview->report.created == 1);
        CHECK(notes_of(run(document, preview)) ==
              std::vector<SimpleNote>{{0, 100, 62, 100}, {100, 200, 60, 100}, {900, 1000, 62, 100}});
    }

    TEST_CASE("repeat, split and join") {
        const auto document = make_document({{0, 480, 60}});
        RepeatParameters repeat_parameters;
        repeat_parameters.count = 2;
        repeat_parameters.length = 480;
        repeat_parameters.pitch_step = 2;
        CHECK(notes_of(run(document, repeat(document, select_all(document), repeat_parameters))) ==
              std::vector<SimpleNote>{{0, 480, 60, 100}, {480, 960, 62, 100}, {960, 1440, 64, 100}});
        // Out-of-range recipe values are refused instead of overflowing.
        auto huge_step = repeat_parameters;
        huge_step.pitch_step = 2'000'000'000;
        CHECK_FALSE(repeat(document, select_all(document), huge_step).has_value());
        nlohmann::json huge_ticks = {{"ticks", std::numeric_limits<std::int64_t>::max()}};
        Tick ticks = 0;
        FieldReader reader(huge_ticks);
        reader.integer("ticks", ticks);
        CHECK_FALSE(reader.status().has_value());
        SplitParameters split;
        split.points = {240};
        const auto halves = run(document, split_notes(document, select_all(document), split));
        CHECK(notes_of(halves) == std::vector<SimpleNote>{{0, 240, 60, 100}, {240, 480, 60, 100}});
        const auto joined = run(halves, join_notes(halves, select_all(halves), JoinParameters{}));
        CHECK(notes_of(joined) == std::vector<SimpleNote>{{0, 480, 60, 100}});
    }

    TEST_CASE("remove exact duplicates and resolve same-pitch overlaps") {
        const auto document = make_document({{0, 480, 60, 100}, {0, 480, 60, 100}, {0, 480, 60, 90}});
        const auto preview = remove_duplicates(document, select_all(document), RemoveDuplicatesParameters{});
        REQUIRE(preview.has_value());
        CHECK(preview->report.removed == 1);
        CHECK(notes_of(run(document, preview)).size() == 2);

        const auto overlapping = make_document({{0, 480, 60}, {240, 720, 60}, {0, 480, 64}});
        const auto trimmed = run(overlapping, resolve_overlaps(overlapping, select_all(overlapping), ResolveOverlapsParameters{}));
        CHECK(notes_of(trimmed) == std::vector<SimpleNote>{{0, 240, 60, 100}, {0, 480, 64, 100}, {240, 720, 60, 100}});
        ResolveOverlapsParameters merge;
        merge.action = OverlapAction::merge;
        CHECK(notes_of(run(overlapping, resolve_overlaps(overlapping, select_all(overlapping), merge))) ==
              std::vector<SimpleNote>{{0, 480, 64, 100}, {0, 720, 60, 100}});
    }

    TEST_CASE("conform selection to scale is explicit and uses the tie rule") {
        const auto document = make_document({{0, 240, 61}, {240, 480, 66}, {480, 720, 64}});
        ConformToScaleParameters parameters;
        CHECK(notes_of(run(document, conform_to_scale(document, select_all(document), parameters))) ==
              std::vector<SimpleNote>{{0, 240, 60, 100}, {240, 480, 65, 100}, {480, 720, 64, 100}});
        parameters.tie = NearestTie::upward;
        CHECK(std::get<2>(notes_of(run(document, conform_to_scale(document, select_all(document), parameters)))[0]) == 62);
    }

    TEST_CASE("groove extraction and application") {
        const auto reference = make_document({{10, 100, 60, 110}, {250, 300, 60, 70}, {490, 600, 60, 110}, {730, 800, 60, 70}});
        GrooveExtractParameters extract;
        extract.steps = 2;
        const auto groove = extract_groove(reference, select_all(reference), extract);
        REQUIRE(groove.has_value());
        CHECK(groove->timing_offsets == std::vector<Tick>{10, 10});
        CHECK(groove->velocity_offsets == std::vector<int>{20, -20});
        const auto json = groove_to_json(*groove);
        CHECK(groove_from_json(json).value() == *groove);
        const auto target = make_document({{0, 100, 62, 90}, {240, 300, 62, 90}});
        GrooveApplyParameters apply;
        apply.groove = *groove;
        CHECK(notes_of(run(target, apply_groove(target, select_all(target), apply))) ==
              std::vector<SimpleNote>{{10, 110, 62, 110}, {250, 310, 62, 70}});
    }

    TEST_CASE("parameters serialize to versioned JSON with the PRNG description") {
        HumanizeParameters humanize_parameters;
        humanize_parameters.seed = 18446744073709551615ull;
        humanize_parameters.chord_tolerance = 12;
        const TransformParameters parameters = humanize_parameters;
        const auto json = parameters_to_json(parameters);
        const auto parsed_json = nlohmann::json::parse(json);
        CHECK(parsed_json.at("prng").at("name").get<std::string>() == "pcg32");
        CHECK(parsed_json.at("prng").at("seed").get<std::string>() == "18446744073709551615");
        const auto parsed = parameters_from_json(json);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == parameters);

        QuantizeParameters quantize_parameters;
        quantize_parameters.grid = {{1, 12}, {3, 5}, 30};
        quantize_parameters.strength = {3, 4};
        quantize_parameters.max_movement = 77;
        CHECK(parameters_from_json(parameters_to_json(quantize_parameters)).value() ==
              TransformParameters{quantize_parameters});
        GrooveApplyParameters groove;
        groove.groove.timing_offsets.assign(16, 3);
        groove.groove.velocity_offsets.assign(16, -2);
        groove.groove.sample_counts.assign(16, 1);
        CHECK(parameters_from_json(parameters_to_json(groove)).value() == TransformParameters{groove});
        EuclideanParameters euclid;
        euclid.accent_pulses = {0, 2};
        CHECK(parameters_from_json(parameters_to_json(euclid)).value() == TransformParameters{euclid});
        CHECK_FALSE(parameters_from_json("{\"format\":\"mwb.transform\",\"tool\":\"nope\",\"version\":1}").has_value());
        CHECK_FALSE(parameters_from_json("not json").has_value());
    }

    TEST_CASE("recipes replay to the same events and detect a changed source") {
        const auto document = make_document({{0, 480, 60}, {0, 480, 64}, {960, 1440, 67}});
        HumanizeParameters parameters;
        parameters.seed = 5;
        parameters.timing_range = 30;
        const auto selection = select_all(document);
        const auto recipe = make_recipe(document, selection, parameters);
        REQUIRE(recipe.has_value());
        CHECK(recipe->tool_id == "humanize");
        const auto direct = run(document, preview_transform(document, selection, parameters));
        const auto replayed = run(document, replay_recipe(document, *recipe));
        CHECK(semantic_hash(direct) == semantic_hash(replayed));
        CHECK(selection_from_json(recipe->targets_json).value() == selection);
        const auto changed = run(document, transpose(document, selection, TransposeParameters{TransposeMode::semitones, 1}));
        const auto refused = replay_recipe(changed, *recipe);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ErrorCode::conflict);
    }

    TEST_CASE("cross-PPQ paste converts every event from its exact position") {
        const auto document = make_document({{1001, 1003, 60}, {1000, 1002, 62}});
        const auto& events = document.sequences.front().tracks.front().events;
        const auto conversion = convert_events_ppq(events, 960, 480);
        REQUIRE(conversion.has_value());
        std::vector<Tick> ticks;
        for (const auto& event : conversion->events)
            ticks.push_back(event.tick);
        CHECK(ticks == std::vector<Tick>{500, 501, 501, 502});
        CHECK(conversion->max_error == Rational{1, 2});
        CHECK(conversion->collisions == 1);
        const auto anchored = convert_events_ppq(events, 960, 96, 1000, 10);
        REQUIRE(anchored.has_value());
        CHECK(anchored->events.front().tick == 10);
    }

    TEST_CASE("changing sequence PPQ converts events, regions and floors note lengths") {
        auto document = make_document({{0, 4, 60}, {960, 1920, 62}});
        Region region;
        region.id = RegionId::generate();
        region.start_tick = 480;
        region.end_tick = 1440;
        document.sequences.front().regions.push_back(region);
        ScaleRegion scale_region;
        scale_region.id = RegionId::generate();
        scale_region.start_tick = 960;
        scale_region.end_tick = 1920;
        document.sequences.front().scale_regions.push_back(scale_region);
        RecordingTake take; // recording evidence of this sequence's track (code-core-14)
        take.id = TakeId::generate();
        take.track = document.sequences.front().tracks.front().id;
        take.start_tick = 960;
        take.end_tick = 3840;
        document.takes.push_back(take);
        const auto preview = change_sequence_ppq(document, document.sequences.front().id, 96);
        REQUIRE(preview.has_value());
        CHECK(preview->report.clipped == 1);
        const auto after = run(document, preview);
        const auto& sequence = after.sequences.front();
        CHECK(sequence.division.ppq == 96);
        CHECK(notes_of(after) == std::vector<SimpleNote>{{0, 1, 60, 100}, {96, 192, 62, 100}});
        CHECK(sequence.regions.front().start_tick == 48);
        CHECK(sequence.regions.front().end_tick == 144);
        CHECK(sequence.scale_regions.front().start_tick == 96);
        CHECK(sequence.tracks.front().end_tick == 192);
        REQUIRE(after.takes.size() == 1);
        CHECK(after.takes.front().id == take.id);
        CHECK(after.takes.front().start_tick == 96);
        CHECK(after.takes.front().end_tick == 384);
        CHECK(semantic_hash(applied(after, invert(preview->changes))) == semantic_hash(document));
    }

    TEST_CASE("selections naming unknown notes or tracks are rejected") {
        const auto document = make_document({{0, 480, 60}});
        auto selection = select_all(document);
        selection.notes = {EventId::generate()};
        CHECK_FALSE(quantize(document, selection, QuantizeParameters{}).has_value());
        selection = select_all(document);
        selection.tracks = {TrackId::generate()};
        CHECK_FALSE(quantize(document, selection, QuantizeParameters{}).has_value());
        auto smpte = document;
        smpte.sequences.front().division = Division::make_smpte(SmpteFormat::fps25, 40);
        CHECK(quantize(smpte, select_all(smpte), QuantizeParameters{}).error().code == ErrorCode::unsupported);
    }
}

TEST_SUITE("analysis") {
    TEST_CASE("maximum key-down polyphony uses half-open spans with ends before starts") {
        const auto touching = make_document({{0, 480, 60}, {480, 960, 62}});
        CHECK(max_key_down_polyphony(pair_notes(touching.sequences.front()).notes) == 1);
        const auto overlapping = make_document({{0, 480, 60}, {240, 720, 62}, {300, 400, 64}});
        CHECK(max_key_down_polyphony(pair_notes(overlapping.sequences.front()).notes) == 3);
        std::vector<NoteSpan> zero_length(2);
        zero_length[0].start = 0;
        zero_length[0].end = 480;
        zero_length[1].start = 100;
        zero_length[1].end = 100;
        CHECK(max_key_down_polyphony(zero_length) == 2);
        CHECK(max_key_down_polyphony({}) == 0);
    }

    TEST_CASE("key estimation finds C major for a C-major scale phrase") {
        std::vector<NoteSpec> phrase;
        const std::vector<int> pitches{60, 62, 64, 65, 67, 69, 71, 72, 67, 64, 60};
        Tick tick = 0;
        for (const int pitch : pitches) {
            const Tick length = (pitch % 12 == 0 || pitch == 67) ? 960 : 480;
            phrase.push_back({tick, tick + length, pitch});
            tick += length;
        }
        const auto document = make_document(phrase);
        const auto estimate = estimate_key(pair_notes(document.sequences.front()).notes);
        REQUIRE_FALSE(estimate.unknown);
        CHECK(estimate.best->root_pitch_class == 0);
        CHECK(estimate.best->mode == "major");
        CHECK(estimate.confidence > 0);
        CHECK(estimate.confidence <= 1);
        CHECK(estimate.profile_name == "krumhansl_kessler");
    }

    TEST_CASE("key estimation finds A minor for an A-minor phrase") {
        std::vector<NoteSpec> phrase;
        const std::vector<std::pair<int, Tick>> pitches{{57, 960}, {59, 480}, {60, 480}, {62, 480}, {64, 960},
                                                        {68, 480}, {69, 960}, {64, 480}, {60, 480}, {57, 960}};
        Tick tick = 0;
        for (const auto& [pitch, length] : pitches) {
            phrase.push_back({tick, tick + length, pitch});
            tick += length;
        }
        const auto document = make_document(phrase);
        const auto estimate = estimate_key(pair_notes(document.sequences.front()).notes);
        REQUIRE_FALSE(estimate.unknown);
        CHECK(estimate.best->root_pitch_class == 9);
        CHECK(estimate.best->mode == "minor");
        KeyEstimationOptions with_modes;
        with_modes.profiles = KeyProfileSet::major_minor_and_modes;
        CHECK(estimate_key(pair_notes(document.sequences.front()).notes, with_modes).alternatives.size() == 5);
    }

    TEST_CASE("sparse material returns Unknown with alternatives") {
        const auto document = make_document({{0, 480, 60}, {480, 960, 67}});
        const auto estimate = estimate_key(pair_notes(document.sequences.front()).notes);
        CHECK(estimate.unknown);
        CHECK_FALSE(estimate.best.has_value());
        CHECK_FALSE(estimate.alternatives.empty());
    }

    TEST_CASE("representative tempo is the quarter-length weighted median") {
        auto document = make_document({{0, 3840, 60}});
        auto& track = document.sequences.front().tracks.front();
        Event slow = events::tempo(960, 1000000);
        Event fast = events::tempo(0, 500000);
        fast.order_key = 1;
        slow.order_key = 1;
        track.events.insert(track.events.begin(), fast);
        track.events.insert(track.events.begin() + 2, slow);
        std::ranges::stable_sort(track.events, EventOrder{});
        CHECK(representative_tempo(document.sequences.front(), 3840) == 1000000);
        CHECK(representative_tempo(document.sequences.front(), 1920) == 1000000);
        CHECK(representative_tempo(document.sequences.front(), 1000) == 500000);
        const auto summary = analyze_sequence(document.sequences.front());
        CHECK(summary.tempo_origin == TempoOrigin::explicit_tempo);
        CHECK(summary.tempo_representative.value() == doctest::Approx(60.0));
        CHECK(summary.tempo_min.value() == doctest::Approx(60.0));
        CHECK(summary.tempo_max.value() == doctest::Approx(120.0));
        CHECK(summary.tempo_change_count == 1);
    }

    TEST_CASE("basic analysis summary feeds sequence_summary and track_summary") {
        auto document = make_document({{0, 480, 60}, {0, 480, 64}, {480, 960, 72}});
        auto& track = document.sequences.front().tracks.front();
        Event program = events::program_change(0, 0, 5);
        program.order_key = 1;
        track.events.insert(track.events.begin(), program);
        Event key = events::key_signature(0, -3, true);
        key.order_key = 2;
        track.events.insert(track.events.begin() + 1, key);
        const auto summary = analyze_sequence(document.sequences.front());
        CHECK(summary.division == "ppq");
        CHECK(summary.note_count == 3);
        CHECK(summary.event_count == 8);
        CHECK(summary.min_pitch == 60);
        CHECK(summary.max_pitch == 72);
        CHECK(summary.max_polyphony == 2);
        CHECK(summary.quarter_length.value() == doctest::Approx(1.0));
        CHECK(summary.duration_seconds == doctest::Approx(0.5));
        CHECK(summary.tempo_origin == TempoOrigin::default_tempo);
        CHECK(summary.key_origin == KeyOrigin::signature);
        CHECK(summary.key_root == 0);
        CHECK(summary.key_mode == "minor");
        CHECK_FALSE(summary.has_drums);
        REQUIRE(summary.tracks.size() == 1);
        CHECK(summary.tracks.front().channel_mask == 1);
        CHECK(summary.tracks.front().programs.front().program == 5);
        CHECK(nlohmann::json::parse(summary.diagnostics_json()).at("key_signatures").size() == 1);
        CHECK(nlohmann::json::parse(summary.tracks.front().program_json()).size() == 1);
        CHECK(nlohmann::json::parse(summary.tracks.front().feature_json()).at("max_polyphony") == 2);

        const auto drums = make_document({{0, 240, 36}, {240, 480, 38}, {480, 720, 42}, {720, 960, 46}}, 960, true);
        const auto drum_summary = analyze_sequence(drums.sequences.front());
        CHECK(drum_summary.has_drums);
        CHECK(drum_summary.key_estimate.unknown);
        CHECK(drum_summary.key_origin == KeyOrigin::unknown);
    }
}

TEST_SUITE("fingerprint") {
    TEST_CASE("musical fingerprints follow their declared equivalences") {
        const auto base = make_document({{0, 480, 60}, {480, 960, 64}, {480, 960, 67}});
        const auto half_resolution = make_document({{0, 240, 60}, {240, 480, 64}, {240, 480, 67}}, 480);
        const auto transposed = make_document({{0, 480, 62}, {480, 960, 66}, {480, 960, 69}});
        const auto slower = make_document({{0, 960, 50}, {960, 1920, 52}, {960, 1920, 55}});
        const auto exact = [](const DocumentData& document, FingerprintProfile profile) {
            return fingerprint_document(document, profile);
        };
        CHECK(fingerprints_verified_equal(exact(base, FingerprintProfile::exact_notes),
                                          exact(half_resolution, FingerprintProfile::exact_notes)));
        CHECK_FALSE(exact(base, FingerprintProfile::exact_event_content) ==
                    exact(half_resolution, FingerprintProfile::exact_event_content));
        CHECK_FALSE(exact(base, FingerprintProfile::exact_notes) == exact(transposed, FingerprintProfile::exact_notes));
        CHECK(exact(base, FingerprintProfile::transposition_invariant) ==
              exact(transposed, FingerprintProfile::transposition_invariant));
        CHECK(exact(base, FingerprintProfile::rhythm_shape) == exact(slower, FingerprintProfile::rhythm_shape));
        CHECK_FALSE(exact(base, FingerprintProfile::transposition_invariant) ==
                    exact(slower, FingerprintProfile::transposition_invariant));
        // Identities and order keys never affect event-content fingerprints.
        const auto rebuilt = make_document({{0, 480, 60}, {480, 960, 64}, {480, 960, 67}});
        CHECK(exact(base, FingerprintProfile::exact_event_content) == exact(rebuilt, FingerprintProfile::exact_event_content));
        const std::vector<std::uint8_t> bytes{1, 2, 3};
        CHECK(to_hex(fingerprint_object(bytes).sha256) ==
              "039058c6f2c0cb492c533b0a4d14ef77cc0f78abccced5287d84a1a2011cfb81");
    }
}

TEST_SUITE("voice") {
    TEST_CASE("top, bottom and onset-chord profiles group onsets within 1/96 quarter") {
        const auto document = make_document({{0, 480, 60}, {5, 480, 67}, {0, 480, 64}, {480, 960, 62}, {480, 960, 65}});
        const auto top = extract_voices(document.sequences.front(), VoiceProfile::top_voice);
        REQUIRE(top.has_value());
        REQUIRE(top->size() == 1);
        CHECK(top->front().onset_tolerance == 10);
        std::vector<int> top_pitches;
        for (const auto& note : top->front().notes)
            top_pitches.push_back(note.pitch);
        CHECK(top_pitches == std::vector<int>{67, 65});
        const auto bottom = extract_voices(document.sequences.front(), VoiceProfile::bottom_voice);
        CHECK(bottom->front().notes.front().pitch == 60);
        const auto chords = extract_voices(document.sequences.front(), VoiceProfile::onset_chord);
        CHECK(chords->front().notes.front().chord_mask == pitch_class_mask(std::vector<int>{60, 64, 67}));
        CHECK(chords->front().notes.front().group_size == 3);
        CHECK(extract_voices(document.sequences.front(), VoiceProfile::monophonic_channel)->empty());
        const auto line = make_document({{0, 480, 60}, {480, 960, 62}});
        CHECK(extract_voices(line.sequences.front(), VoiceProfile::monophonic_channel)->size() == 1);
        auto smpte = line;
        smpte.sequences.front().division = Division::make_smpte(SmpteFormat::fps25, 40);
        CHECK_FALSE(extract_voices(smpte.sequences.front(), VoiceProfile::top_voice).has_value());
        VoiceExtractionOptions explicit_tolerance;
        explicit_tolerance.onset_tolerance = 2;
        CHECK(extract_voices(smpte.sequences.front(), VoiceProfile::top_voice, explicit_tolerance).has_value());
        const auto drums = make_document({{0, 240, 36}}, 960, true);
        CHECK(extract_voices(drums.sequences.front(), VoiceProfile::top_voice)->empty());
    }

    TEST_CASE("motif tokens: transposed melodies share intervals, rhythm ratios are binned") {
        const auto c_major = make_document({{0, 480, 60}, {480, 960, 64}, {960, 1920, 67}, {1920, 2400, 72}});
        const auto d_major = make_document({{0, 480, 62}, {480, 960, 66}, {960, 1920, 69}, {1920, 2400, 74}});
        const auto c_four = make_document({{0, 480, 60}, {480, 960, 65}, {960, 1920, 67}});
        const auto tokens_of = [](const DocumentData& document) {
            auto result = extract_motif_tokens(document.sequences.front(), VoiceProfile::monophonic_channel);
            REQUIRE(result.has_value());
            REQUIRE(result->size() == 1);
            return result->front().second;
        };
        const auto c_tokens = tokens_of(c_major);
        CHECK(c_tokens.interval == std::vector<int>{4, 3, 5});
        CHECK(tokens_of(d_major).interval == c_tokens.interval);
        CHECK(std::vector<int>(tokens_of(c_four).interval) != std::vector<int>{4, 3});
        CHECK(c_tokens.contour == std::vector<int>{1, 1, 1});
        CHECK(c_tokens.rhythm == std::vector<int>{0, 3});
        CHECK(c_tokens.pitch == std::vector<int>{60, 64, 67, 72});
        CHECK(rhythm_ratio_bin(480, 160) == -4);
        CHECK(rhythm_ratio_bin(480, 100000) == 5);
        CHECK(contour_token(-7, true) == -3);
        CHECK(token_family_name(TokenFamily::chord) == "chord");
    }
}

TEST_SUITE("transform") {
    TEST_CASE("every tool is deterministic and its preview inverts to the exact source state") {
        const auto document = make_document({{0, 480, 60, 90},
                                              {0, 480, 64, 80},
                                              {0, 480, 67, 70},
                                              {250, 700, 72, 100},
                                              {480, 960, 62, 110},
                                              {600, 900, 62, 60},
                                              {960, 1440, 65, 100},
                                              {960, 1440, 65, 100},
                                              {1500, 1700, 71, 50},
                                              {1700, 1800, 74, 100},
                                              {1800, 1900, 74, 100},
                                              {1900, 2000, 61, 100}});
        auto selection = select_all(document);
        selection.tracks = {document.sequences.front().tracks.front().id};
        RotateParameters rotate_parameters;
        rotate_parameters.range = {0, 1920};
        rotate_parameters.shift = 100;
        StrumParameters strum_parameters;
        strum_parameters.order = StrumOrder::random;
        ArpeggiateParameters arpeggio_parameters;
        arpeggio_parameters.pattern = ArpeggioPattern::random;
        arpeggio_parameters.octaves = 2;
        GrooveApplyParameters groove_parameters;
        groove_parameters.groove.timing_offsets.assign(16, 7);
        groove_parameters.groove.velocity_offsets.assign(16, -5);
        groove_parameters.groove.sample_counts.assign(16, 1);
        SplitParameters split_parameters;
        split_parameters.points = {240, 1000};
        TransposeParameters transpose_parameters;
        transpose_parameters.amount = 3;
        MoveParameters move_parameters;
        move_parameters.ticks = 10;
        HumanizeParameters humanize_parameters;
        humanize_parameters.timing_range = 40;
        humanize_parameters.velocity_range = 10;
        humanize_parameters.duration_range = 20;
        const std::vector<TransformParameters> tools{
            QuantizeParameters{},       humanize_parameters,          transpose_parameters,
            move_parameters,            VelocityParameters{},         LengthParameters{},
            TimeStretchParameters{},    ReverseParameters{},          InvertPitchParameters{},
            rotate_parameters,          RepeatParameters{},           strum_parameters,
            arpeggio_parameters,        EuclideanParameters{},        groove_parameters,
            RemoveDuplicatesParameters{}, ResolveOverlapsParameters{}, split_parameters,
            JoinParameters{},           ConformToScaleParameters{}};
        REQUIRE(tools.size() == std::variant_size_v<TransformParameters>);
        const auto original_hash = semantic_hash(document);
        for (const auto& tool : tools) {
            CAPTURE(tool_id(tool));
            const auto first = preview_transform(document, selection, tool);
            const auto second = preview_transform(document, selection, tool);
            REQUIRE(first.has_value());
            REQUIRE(second.has_value());
            CHECK_FALSE(first->changes.empty());
            const auto first_result = applied(document, first->changes);
            const auto second_result = applied(document, second->changes);
            CHECK(fingerprint_document(first_result, FingerprintProfile::exact_event_content) ==
                  fingerprint_document(second_result, FingerprintProfile::exact_event_content));
            CHECK(notes_of(first_result) == notes_of(second_result));
            CHECK(first->report.tool == tool_id(tool));
            CHECK(semantic_hash(applied(first_result, invert(first->changes))) == original_hash);
            const auto reparsed = parameters_from_json(parameters_to_json(tool));
            REQUIRE(reparsed.has_value());
            CHECK(*reparsed == tool);
        }
    }
}

TEST_SUITE("transform") {
    // code-core-1 through the NoteEditor path used by arrow-key nudge and transpose.
    TEST_CASE("move and transpose keep a zero-length note terminated") {
        DocumentData document = make_document({{0, 480, 48}});
        const TrackId track = document.sequences.front().tracks.front().id;
        ChangeBuilder add(document, "add");
        std::ignore = add.add_note(track, 960, 960, 0, 60, 100);
        document = applied(document, add.finish());
        CHECK(notes_of(document) == std::vector<SimpleNote>{{0, 480, 48, 100}, {960, 960, 60, 100}});
        MoveParameters move;
        move.ticks = 240;
        document = run(document, move_notes(document, select_all(document), move));
        CHECK(notes_of(document) == std::vector<SimpleNote>{{240, 720, 48, 100}, {1200, 1200, 60, 100}});
        TransposeParameters up;
        up.amount = 2;
        document = run(document, transpose(document, select_all(document), up));
        CHECK(notes_of(document) == std::vector<SimpleNote>{{240, 720, 50, 100}, {1200, 1200, 62, 100}});
        CHECK(pair_notes(document.sequences.front()).orphan_ends.empty());
    }
}

TEST_SUITE("general_midi") {
    TEST_CASE("the GM table has 128 distinct names in 16 families of 8") {
        std::set<std::string_view> names;
        for (int program = 0; program < music::gm_program_count; ++program) {
            const auto name = music::gm_program_name(static_cast<std::uint8_t>(program));
            CHECK_FALSE(name.empty());
            names.insert(name);
            CHECK(music::gm_program_named(name) == static_cast<std::uint8_t>(program));
        }
        CHECK(names.size() == 128);
        CHECK(music::gm_program_name(0) == "Acoustic Grand Piano");
        CHECK(music::gm_program_name(24) == "Acoustic Guitar (nylon)");
        CHECK(music::gm_program_name(33) == "Electric Bass (finger)");
        CHECK(music::gm_program_name(40) == "Violin");
        CHECK(music::gm_program_name(127) == "Gunshot");
        CHECK(music::gm_program_name(128) == "Acoustic Grand Piano"); // masked to 7 bits
        CHECK(music::gm_family_name(0) == "Piano");
        CHECK(music::gm_family_name(4) == "Bass");
        CHECK(music::gm_family_name(15) == "Sound Effects");
        CHECK(music::gm_family_name(16).empty());
        CHECK(music::gm_family_name(-1).empty());
        CHECK(music::gm_family_of(7) == 0);
        CHECK(music::gm_family_of(8) == 1);
        CHECK(music::gm_family_of(127) == 15);
        CHECK_FALSE(music::gm_program_named("Kazoo"));
    }

    TEST_CASE("the effective instrument is the first program change before the first note, else the setting") {
        Track track;
        track.id = TrackId::generate();
        track.instrument.program = 40;
        CHECK(music::effective_instrument(track) == music::TrackInstrument{false, 40, false});
        CHECK(music::instrument_label(track) == "Violin");
        track.events.push_back(events::program_change(0, 0, 33));
        track.events.push_back(events::note_on(0, 0, 40, 100));
        track.events.push_back(events::program_change(480, 0, 56)); // after the first note: a later change
        CHECK(music::effective_instrument(track) == music::TrackInstrument{false, 33, true});
        CHECK(music::instrument_label(track) == "Electric Bass (finger)");
        track.events.erase(track.events.begin());
        CHECK(music::effective_instrument(track).program == 40); // the setting again
        track.is_drum = true;
        CHECK(music::instrument_label(track) == "Drums");
    }

    TEST_CASE("choosing an instrument writes the setting, the timbre and a tick-0 program change; undo restores") {
        auto document = make_blank_document("gm");
        auto& track = document.sequences[0].tracks[0];
        const TrackId id = track.id;
        ChangeBuilder notes(document, "notes");
        notes.add_note(id, 480, 960, 2, 60, 100); // channel 3
        REQUIRE(apply_change_set(document, notes.finish()));
        const auto before = document;

        auto changes = music::set_track_instrument(document, id, 24, "electric-piano");
        REQUIRE(changes);
        CHECK(changes->changes.label == "Set instrument");
        REQUIRE(apply_change_set(document, changes->changes));
        const auto& changed = document.sequences[0].tracks[0];
        CHECK(changed.instrument.program == 24);
        CHECK(changed.instrument.synth_preset == "electric-piano");
        const auto program_change = std::ranges::find_if(changed.events, [](const Event& event) {
            return event.is_channel() && event.message() == ChannelMessage::program_change;
        });
        REQUIRE(program_change != changed.events.end());
        const Event& first = *program_change;
        CHECK(first.tick == 0);
        CHECK(program_change < std::ranges::find_if(changed.events, [](const Event& event) { return event.is_note_start(); }));
        CHECK(first.message() == ChannelMessage::program_change);
        CHECK(first.channel() == 2); // the notes' channel
        CHECK(first.data1() == 24);
        CHECK(music::effective_instrument(changed) == music::TrackInstrument{false, 24, true});

        // Choosing again updates that program change; it does not add a second one.
        changes = music::set_track_instrument(document, id, 40, "saw-pad");
        REQUIRE(changes);
        REQUIRE(apply_change_set(document, changes->changes));
        std::size_t programs = 0;
        for (const auto& event : document.sequences[0].tracks[0].events)
            programs += event.is_channel() && event.message() == ChannelMessage::program_change;
        CHECK(programs == 1);
        CHECK(music::instrument_label(document.sequences[0].tracks[0]) == "Violin");
        // The same choice again is no edit at all.
        changes = music::set_track_instrument(document, id, 40, "saw-pad");
        REQUIRE(changes);
        CHECK(changes->changes.empty());

        // Undo (the inverse change sets) restores the document exactly.
        auto again = before;
        auto first_change = music::set_track_instrument(again, id, 24, "electric-piano");
        REQUIRE(first_change);
        REQUIRE(apply_change_set(again, first_change->changes));
        REQUIRE(apply_change_set(again, invert(first_change->changes)));
        CHECK(again == before);
    }

    TEST_CASE("drums set the drum flag and channel 10 without a program change; an instrument undoes both") {
        auto document = make_blank_document("gm");
        const TrackId id = document.sequences[0].tracks[0].id;
        auto changes = music::set_track_instrument(document, id, std::nullopt, "");
        REQUIRE(changes);
        REQUIRE(apply_change_set(document, changes->changes));
        const auto& drums = document.sequences[0].tracks[0];
        CHECK(drums.is_drum);
        CHECK(drums.routing.channel_override == music::gm_drum_channel);
        CHECK(drums.events.size() == 2); // the tempo and meter only: no program change
        CHECK(music::instrument_label(drums) == "Drums");
        changes = music::set_track_instrument(document, id, 0, "classic");
        REQUIRE(changes);
        REQUIRE(apply_change_set(document, changes->changes));
        const auto& piano = document.sequences[0].tracks[0];
        CHECK_FALSE(piano.is_drum);
        CHECK_FALSE(piano.routing.channel_override);
        REQUIRE(piano.events.size() == 3); // no notes yet: channel 1 gets the program change
        CHECK(piano.events.back().message() == ChannelMessage::program_change);
        CHECK(piano.events.back().channel() == 0);
        CHECK(music::set_track_instrument(document, TrackId::generate(), 0, "classic").error().code == ErrorCode::not_found);
        CHECK_FALSE(music::set_track_instrument(document, id, 128, "classic"));
    }

    TEST_CASE("a melodic pick moves the track's channel-10 events to a free channel; never a program change on 10") {
        auto document = make_blank_document("gm");
        auto& sequence = document.sequences[0];
        const TrackId melody = sequence.tracks[0].id;
        Track beat;
        beat.id = TrackId::generate();
        beat.name = "Beat";
        beat.is_drum = true;
        beat.routing.channel_override = music::gm_drum_channel;
        ChangeBuilder add(document, "add");
        add.append(TrackInsert{sequence.id, 1, beat});
        REQUIRE(apply_change_set(document, add.finish()));
        ChangeBuilder notes(document, "notes");
        notes.add_note(melody, 0, 480, 0, 60, 100);  // channel 1: taken
        notes.add_note(melody, 0, 480, 2, 64, 100);  // channel 3: taken
        notes.add_note(beat.id, 0, 120, 9, 36, 100); // kick on channel 10
        notes.add_note(beat.id, 480, 600, 9, 38, 100);
        notes.insert(beat.id, events::control_change(0, 9, 7, 90), OrderPlacement::before_note_starts);
        notes.insert(beat.id, events::program_change(0, 9, 0), OrderPlacement::before_note_starts); // a kit select
        REQUIRE(apply_change_set(document, notes.finish()));
        const auto before = document;

        auto edit = music::set_track_instrument(document, beat.id, 33, "bass");
        REQUIRE(edit);
        REQUIRE(edit->moved_to);
        CHECK(*edit->moved_to == 1);       // channel 2: the lowest one the melody does not use
        CHECK(edit->moved_events == 6);    // 2 notes (on and off), the CC and the program change
        REQUIRE(apply_change_set(document, edit->changes));
        const Track& moved = document.sequences[0].tracks[1];
        CHECK_FALSE(moved.is_drum);
        CHECK_FALSE(moved.routing.channel_override);
        std::size_t programs = 0;
        for (const auto& event : moved.events) {
            if (!event.is_channel())
                continue;
            CHECK(event.channel() != music::gm_drum_channel); // nothing is left on channel 10
            CHECK(event.channel() == 1);
            if (event.message() == ChannelMessage::program_change) {
                ++programs;
                CHECK(event.data1() == 33); // the kit select became the bass program, not a second one
            }
        }
        CHECK(programs == 1);
        CHECK(music::instrument_label(moved) == "Electric Bass (finger)");
        REQUIRE(apply_change_set(document, invert(edit->changes))); // one undo restores the drums
        CHECK(document == before);

        // A track whose events are not on channel 10 moves nothing.
        auto plain = music::set_track_instrument(before, melody, 40, "saw-pad");
        REQUIRE(plain);
        CHECK_FALSE(plain->moved_to);
        CHECK(plain->moved_events == 0);
    }
}
