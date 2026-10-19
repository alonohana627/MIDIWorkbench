#include "test_support.hpp"

#include "mwb/core/pitch.hpp"
#include "mwb/core/random.hpp"
#include "mwb/core/text.hpp"
#include "mwb/search/browse_index.hpp"
#include "mwb/search/motif.hpp"
#include "mwb/search/query.hpp"
#include "mwb/search/sql.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace mwb;
using namespace mwb::search;

namespace {

/// The six examples of specification section 25.2.
const std::vector<std::string> grammar_examples{
    "tag:bass bpm:140..155 notes:>=8 favorite:true",
    "key:\"F# minor\" key.origin:manual length.qn:8..32",
    "text:\"rising lead\" AND NOT drums:true",
    "has:bend AND duration:<30s",
    "(tag:melody OR tag:arpeggio) rating:>=4",
    "tempo.any:140..160 tempo.origin:explicit",
};

Query must_parse(std::string_view text) {
    auto parsed = parse_query(text);
    if (!parsed)
        FAIL("parse failed: " << parsed.error().message << " at " << parsed.error().span.begin << ".."
                              << parsed.error().span.end);
    return std::move(*parsed);
}

QueryError must_fail(std::string_view text, const QueryLimits& limits = {}) {
    auto parsed = parse_query(text, limits);
    REQUIRE_FALSE(parsed.has_value());
    return parsed.error();
}

std::string span_text(std::string_view text, const SourceSpan& span) {
    return std::string(text.substr(span.begin, span.end - span.begin));
}

bool parameters_contain(const SqlFragment& fragment, const std::string& value) {
    return std::ranges::any_of(fragment.parameters, [&](const SqlValue& parameter) {
        const auto* text = std::get_if<std::string>(&parameter);
        return text != nullptr && text->find(value) != std::string::npos;
    });
}

SequenceFacts sequence_with(double bpm, std::optional<int> key_root, std::string key_mode, std::string key_origin) {
    SequenceFacts facts;
    facts.tempo_representative = bpm;
    facts.tempo_min = bpm;
    facts.tempo_max = bpm;
    facts.tempos = {bpm};
    facts.tempo_origin = "explicit";
    facts.key_root = key_root;
    if (key_root)
        facts.key_mode = std::move(key_mode);
    facts.key_origin = std::move(key_origin);
    facts.duration_seconds = 8.0;
    facts.quarter_length = 16.0;
    facts.track_count = 1;
    facts.note_count = 12;
    facts.min_pitch = 48;
    facts.max_pitch = 72;
    facts.meters = {"4/4"};
    facts.tracks.push_back(TrackFacts{1, {0}});
    return facts;
}

} // namespace

TEST_SUITE("query") {
    TEST_CASE("every section 25.2 example parses into a typed AST") {
        for (const auto& example : grammar_examples) {
            CAPTURE(example);
            const auto query = must_parse(example);
            CHECK(query.node_count > 0);
            CHECK(query.node_count <= 128);
        }
        const auto first = must_parse(grammar_examples[0]);
        REQUIRE(first.root.kind == NodeKind::and_node);
        REQUIRE(first.root.children.size() == 4);
        CHECK(first.root.children[0].predicate.field == Field::tag);
        CHECK(std::get<std::string>(first.root.children[0].predicate.value) == "bass");
        const auto& bpm = std::get<NumericCondition>(first.root.children[1].predicate.value);
        CHECK(bpm.comparator == Comparator::range);
        CHECK(bpm.value == 140.0);
        CHECK(bpm.high == 155.0);
        const auto& notes = std::get<NumericCondition>(first.root.children[2].predicate.value);
        CHECK(notes.comparator == Comparator::greater_equal);
        CHECK(std::get<bool>(first.root.children[3].predicate.value));

        const auto key = must_parse(grammar_examples[1]);
        const auto& key_value = std::get<KeyValue>(key.root.children[0].predicate.value);
        CHECK(key_value.root == 6);
        CHECK(key_value.mode == "minor");

        const auto text = must_parse(grammar_examples[2]);
        REQUIRE(text.root.children.size() == 2);
        CHECK(text.root.children[0].predicate.field == Field::text);
        CHECK(text.root.children[0].predicate.quoted);
        CHECK(text.root.children[1].kind == NodeKind::not_node);

        const auto duration = must_parse(grammar_examples[3]);
        const auto& seconds = std::get<NumericCondition>(duration.root.children[1].predicate.value);
        CHECK(seconds.comparator == Comparator::less);
        CHECK(seconds.value == 30.0);

        const auto grouped = must_parse(grammar_examples[4]);
        REQUIRE(grouped.root.children.size() == 2);
        CHECK(grouped.root.children[0].kind == NodeKind::or_node);
    }

    TEST_CASE("field names and operators are case-insensitive; text stays Unicode") {
        const auto query = must_parse("TAG:Bass and BPM:120 or Title:\"Ünïcode Lead\"");
        REQUIRE(query.root.kind == NodeKind::or_node);
        const auto& left = query.root.children[0];
        REQUIRE(left.kind == NodeKind::and_node);
        CHECK(std::get<std::string>(left.children[0].predicate.value) == "bass");
        CHECK(std::get<std::string>(query.root.children[1].predicate.value) == fold_for_search("Ünïcode Lead"));
    }

    TEST_CASE("units, note names and keys are validated and converted") {
        CHECK(std::get<NumericCondition>(must_parse("duration:500ms").root.predicate.value).value == doctest::Approx(0.5));
        CHECK(std::get<NumericCondition>(must_parse("duration:2min").root.predicate.value).value == 120.0);
        CHECK(std::get<NumericCondition>(must_parse("pitch.min:C4").root.predicate.value).value == 60.0);
        CHECK(std::get<NumericCondition>(must_parse("pitch.max:<=F#5").root.predicate.value).value == 78.0);
        CHECK(std::get<KeyValue>(must_parse("key:Am").root.predicate.value) == KeyValue{9, "minor"});
        CHECK(std::get<KeyValue>(must_parse("key:Bb").root.predicate.value) == KeyValue{10, ""});
        CHECK(std::get<MeterValue>(must_parse("meter:6/8").root.predicate.value) == MeterValue{6, 8});
        CHECK(must_parse("").root.children.empty());
    }

    TEST_CASE("parse errors identify the exact span") {
        {
            const std::string text = "tag:bass bpm:abc";
            const auto error = must_fail(text);
            CHECK(error.code == "query.value");
            CHECK(span_text(text, error.span) == "abc");
        }
        {
            const std::string text = "tag:bass colour:red";
            const auto error = must_fail(text);
            CHECK(error.code == "query.field");
            CHECK(span_text(text, error.span) == "colour");
        }
        {
            const std::string text = "bpm:150..140";
            const auto error = must_fail(text);
            CHECK(span_text(text, error.span) == "150..140");
        }
        {
            const std::string text = "(tag:a OR tag:b";
            const auto error = must_fail(text);
            CHECK(error.code == "query.syntax");
            CHECK(error.span.begin == text.size());
        }
        {
            const std::string text = "tag:a \"open quote";
            const auto error = must_fail(text);
            CHECK(error.span.begin == 6);
        }
        {
            const std::string text = "tag:a )";
            const auto error = must_fail(text);
            CHECK(span_text(text, error.span) == ")");
        }
        {
            const std::string text = "tag:";
            const auto error = must_fail(text);
            CHECK(error.span.begin == 4);
        }
        {
            const std::string text = "tag:a OR";
            const auto error = must_fail(text);
            CHECK(error.span.begin == text.size());
        }
        {
            const std::string text = "rating:>7";
            CHECK(span_text(text, must_fail(text).span) == ">7");
        }
        {
            const std::string text = "tag:>bass";
            CHECK(must_fail(text).code == "query.value");
        }
        CHECK(must_fail("\"bad \\n escape\"").code == "query.syntax");
        CHECK(must_fail("NOT").code == "query.syntax");
    }

    TEST_CASE("limits: 4 KiB, depth 16, 128 nodes") {
        CHECK(must_fail(std::string(4097, 'a')).code == "query.limit");
        std::string nested;
        for (int level = 0; level < 17; ++level)
            nested += "(";
        nested += "tag:a";
        for (int level = 0; level < 17; ++level)
            nested += ")";
        const auto depth_error = must_fail(nested);
        CHECK(depth_error.code == "query.limit");
        CHECK(depth_error.span.begin == 16);
        std::string sixteen;
        for (int level = 0; level < 15; ++level)
            sixteen += "(";
        sixteen += "tag:a";
        for (int level = 0; level < 15; ++level)
            sixteen += ")";
        CHECK(parse_query(sixteen).has_value());
        std::string many = "tag:t0";
        for (int index = 1; index < 128; ++index)
            many += " tag:t" + std::to_string(index);
        CHECK(must_fail(many).code == "query.limit");
        std::string allowed = "tag:t0";
        for (int index = 1; index < 127; ++index)
            allowed += " tag:t" + std::to_string(index);
        CHECK(parse_query(allowed).has_value());
    }

    TEST_CASE("quoted strings escape backslash and quote; FTS builder neutralizes operators") {
        const auto query = must_parse("title:\"say \\\"hi\\\" \\\\ now\"");
        CHECK(std::get<std::string>(query.root.predicate.value) == "say \"hi\" \\ now");
        CHECK(fts_string_literal("a \"b\" OR c*") == "\"a \"\"b\"\" OR c*\"");
        const auto operators = must_parse("text:\"lead OR NEAR(bass) title:x * ^\"");
        CHECK(fts_expression(operators.root.predicate) == "\"lead or near(bass) title:x * ^\"");
        CHECK(fts_expression(must_parse("title:Lead").root.predicate) == "title : \"lead\"");
    }

    TEST_CASE("compiled SQL never contains user text") {
        const std::string hostile = "x'); DROP TABLE asset; --";
        const auto query = must_parse("tag:\"" + hostile + "\" text:\"" + hostile + "\" role:\"" + hostile +
                                      "\" collection:\"" + hostile + "\" source:evil'name title:zz9");
        SearchOptions options;
        options.collections[fold_for_search(hostile)] = {7};
        const auto compiled = compile_page_query(query, options, {}, PageCursor{std::string("a'b"), 3}, 100);
        REQUIRE(compiled.has_value());
        const auto& sql = compiled->statement.sql;
        CHECK(sql.find("DROP") == std::string::npos);
        CHECK(sql.find("evil") == std::string::npos);
        CHECK(sql.find("zz9") == std::string::npos);
        CHECK(sql.find("a'b") == std::string::npos);
        CHECK(parameters_contain(compiled->statement, "drop table asset"));
        CHECK(parameters_contain(compiled->statement, "a'b"));
        // Every placeholder is numbered and has a parameter.
        CHECK(sql.find("?" + std::to_string(compiled->statement.parameters.size())) != std::string::npos);
        CHECK(sql.find("?" + std::to_string(compiled->statement.parameters.size() + 1)) == std::string::npos);
        CHECK(sql.find("OFFSET") == std::string::npos);
    }

    TEST_CASE("musical predicates of one AND group compile into one same-sequence EXISTS") {
        const auto query = must_parse("bpm:150 key:\"F minor\" tag:x");
        const auto same = compile_condition(query, {});
        REQUIRE(same.has_value());
        const auto count = [](const std::string& text, const std::string& needle) {
            std::size_t occurrences = 0;
            for (auto position = text.find(needle); position != std::string::npos; position = text.find(needle, position + 1))
                ++occurrences;
            return occurrences;
        };
        CHECK(count(same->sql, "FROM sequence_summary") == 1);
        SearchOptions across;
        across.same_sequence = false;
        CHECK(count(compile_condition(query, across)->sql, "FROM sequence_summary") == 2);
    }

    TEST_CASE("page size is clamped and relevance needs a text term") {
        const auto query = must_parse("tag:a");
        CHECK(compile_page_query(query, {}, {}, std::nullopt, 10'000)->row_limit == maximum_page_size);
        CHECK_FALSE(compile_page_query(query, {}, SortOrder{SortField::relevance}, std::nullopt, 100).has_value());
        CHECK(compile_page_query(must_parse("lead tag:a"), {}, SortOrder{SortField::relevance}, std::nullopt, 100)
                  ->relevance);
        CHECK_FALSE(compile_page_query(query, {}, SortOrder{SortField::modified}, PageCursor{std::string("x"), 1}, 100)
                        .has_value());
    }

    TEST_CASE("AST JSON round trip for smart collections") {
        for (const auto& example : grammar_examples) {
            CAPTURE(example);
            const auto query = must_parse(example);
            const auto json = query_to_json(query);
            CHECK(json.find("SELECT") == std::string::npos);
            const auto restored = query_from_json(json);
            REQUIRE(restored.has_value());
            CHECK(restored->root == query.root);
            CHECK(restored->text == query.text);
        }
        CHECK_FALSE(query_from_json("{\"version\":2,\"text\":\"\",\"root\":{}}").has_value());
        CHECK_FALSE(query_from_json("{\"version\":1,\"text\":\"x\",\"root\":{\"kind\":\"predicate\",\"field\":\"bpm\","
                                    "\"comparator\":\"=\",\"value\":-5,\"high\":-5}}")
                        .has_value());
    }

    TEST_CASE("in-memory evaluation: golden case 10 and three-valued text") {
        BrowseIndex index;
        BrowseRow row;
        row.id = 1;
        row.sort_title = "type 2";
        row.analysis = AnalysisState::ready;
        row.text[0] = "Rising Lead";
        row.sequences.push_back(sequence_with(150.0, std::nullopt, "", "unknown"));
        row.sequences.push_back(sequence_with(120.0, 5, "minor", "signature"));
        index.upsert(row);
        CHECK(index.evaluate(1, must_parse("bpm:150 key:\"F minor\""), {}) == Truth::no);
        CHECK(index.evaluate(1, must_parse("bpm:120 key:\"F minor\""), {}) == Truth::yes);
        SearchOptions across;
        across.same_sequence = false;
        CHECK(index.evaluate(1, must_parse("bpm:150 key:\"F minor\""), across) == Truth::yes);
        CHECK(index.evaluate(1, must_parse("text:\"rising lead\""), {}) == Truth::yes);
        CHECK(index.evaluate(1, must_parse("text:\"lead rising\""), {}) == Truth::no);
        CHECK(index.evaluate(1, must_parse("title:lead status:ready"), {}) == Truth::yes);
        BrowseRow unicode = row;
        unicode.id = 2;
        unicode.text[0] = "Mélodie";
        index.upsert(unicode);
        CHECK(index.evaluate(2, must_parse("title:melodie"), {}) == Truth::unknown);
        CHECK_FALSE(index.matching_ids(must_parse("title:melodie"), {}).has_value());
        CHECK(index.matching_ids(must_parse("bpm:120"), {})->size() == 2);
        CHECK(index.memory_bytes() > 0);
    }
}

// --- Motif search ---------------------------------------------------------------------------------

namespace {

std::vector<QueryNote> melody(std::initializer_list<const char*> names, Tick step = 480) {
    std::vector<QueryNote> notes;
    Tick onset = 0;
    for (const auto* name : names) {
        notes.push_back(QueryNote{onset, step / 2, *parse_pitch(name), 100});
        onset += step;
    }
    return notes;
}

StoredMotifStream stored_stream(std::int64_t id, const std::vector<QueryNote>& notes) {
    StoredMotifStream stored;
    stored.info.stream_id = id;
    stored.info.revision_id = id * 10;
    stored.stream.division = Division::make_ppq(960);
    for (std::size_t index = 0; index < notes.size(); ++index)
        stored.stream.notes.push_back(MotifNote{notes[index].onset, notes[index].onset + notes[index].duration,
                                                notes[index].pitch, notes[index].velocity,
                                                static_cast<std::uint32_t>(index * 2)});
    return stored;
}

Sequence chord_sequence() {
    Sequence sequence;
    sequence.id = SequenceId::generate();
    sequence.division = Division::make_ppq(960);
    Track track;
    track.id = TrackId::generate();
    std::uint64_t order = 0;
    const auto add = [&](Event event) {
        event.order_key = ++order * order_key_spacing;
        track.events.push_back(std::move(event));
    };
    // C major triad then D4, E4 single notes; the top voice is G4 D4 E4 (a chord is grouped).
    for (const std::uint8_t pitch : std::array<std::uint8_t, 3>{60, 64, 67})
        add(events::note_on(0, 0, pitch, 90));
    for (const std::uint8_t pitch : std::array<std::uint8_t, 3>{60, 64, 67})
        add(events::note_off(960, 0, pitch));
    add(events::note_on(960, 0, 62, 80));
    add(events::note_off(1920, 0, 62));
    add(events::note_on(1920, 0, 64, 70));
    add(events::note_off(2880, 0, 64));
    add(events::note_on(2880, 9, 36, 100)); // Drum channel note is skipped by default.
    add(events::note_off(2900, 9, 36));
    std::ranges::stable_sort(track.events, EventOrder{});
    track.end_tick = 2900;
    sequence.end_tick = 2900;
    sequence.tracks.push_back(std::move(track));
    return sequence;
}

} // namespace

TEST_SUITE("motif") {
    TEST_CASE("golden: C4 E4 G4 and D4 F#4 A4 share intervals [4,3]; C4 F4 G4 does not") {
        const auto first = melody({"C4", "E4", "G4"});
        const auto second = melody({"D4", "F#4", "A4"});
        const auto third = melody({"C4", "F4", "G4"});
        CHECK(family_tokens(first, TokenFamily::interval) == std::vector<std::int32_t>{4, 3});
        CHECK(family_tokens(second, TokenFamily::interval) == std::vector<std::int32_t>{4, 3});
        CHECK(family_tokens(third, TokenFamily::interval) == std::vector<std::int32_t>{5, 2});

        InMemoryMotifIndex index;
        index.add(stored_stream(1, first));
        index.add(stored_stream(2, second));
        index.add(stored_stream(3, third));
        ExactQuery query;
        query.mode = MatchMode::transposed;
        query.notes = first;
        const auto result = exact_search(index, query);
        REQUIRE(result.has_value());
        REQUIRE(result->occurrences.size() == 2);
        CHECK(result->occurrences[0].stream.stream_id == 1);
        CHECK(result->occurrences[0].transpose == 0);
        CHECK(result->occurrences[1].stream.stream_id == 2);
        CHECK(result->occurrences[1].transpose == 2);
        CHECK_FALSE(result->indexed); // Two-interval queries use the constrained scan.
        CHECK_FALSE(result->partial);

        query.mode = MatchMode::absolute;
        const auto absolute = exact_search(index, query);
        REQUIRE(absolute->occurrences.size() == 1);
        CHECK(absolute->occurrences[0].stream.stream_id == 1);
    }

    TEST_CASE("exact search finds every window of indexed streams, including the final offset") {
        DeterministicRandom random(42);
        InMemoryMotifIndex index;
        std::vector<std::vector<QueryNote>> streams;
        for (std::int64_t id = 1; id <= 12; ++id) {
            std::vector<QueryNote> notes;
            Tick onset = 0;
            const auto count = 8 + random.bounded(10);
            for (std::uint32_t note = 0; note < count; ++note) {
                onset += 120 * (1 + random.bounded(4));
                notes.push_back(QueryNote{onset, 100, static_cast<std::uint8_t>(48 + random.bounded(24)), 90});
            }
            index.add(stored_stream(id, notes));
            streams.push_back(std::move(notes));
        }
        for (std::size_t stream_index = 0; stream_index < streams.size(); ++stream_index) {
            const auto& notes = streams[stream_index];
            for (std::size_t length = 4; length <= 8; ++length) {
                const auto final_offset = notes.size() - length;
                for (const auto offset : {std::size_t{0}, final_offset / 2, final_offset}) {
                    for (const auto mode : {MatchMode::absolute, MatchMode::transposed, MatchMode::contour}) {
                        ExactQuery query;
                        query.mode = mode;
                        query.require_rhythm = true;
                        query.result_limit = 10'000;
                        for (std::size_t index_in_window = 0; index_in_window < length; ++index_in_window) {
                            auto note = notes[offset + index_in_window];
                            if (mode == MatchMode::transposed)
                                note.pitch = static_cast<std::uint8_t>(note.pitch + 3);
                            note.onset *= 2; // Rhythm matching is tempo-scale invariant.
                            query.notes.push_back(note);
                        }
                        const auto result = exact_search(index, query);
                        REQUIRE(result.has_value());
                        const auto found = std::ranges::any_of(result->occurrences, [&](const MotifOccurrence& occurrence) {
                            return occurrence.stream.stream_id == static_cast<std::int64_t>(stream_index + 1) &&
                                   occurrence.first_note == offset;
                        });
                        CAPTURE(stream_index);
                        CAPTURE(length);
                        CAPTURE(offset);
                        CHECK(found);
                        CHECK(result->indexed);
                        for (const auto& occurrence : result->occurrences) {
                            if (occurrence.stream.stream_id == static_cast<std::int64_t>(stream_index + 1) &&
                                occurrence.first_note == offset) {
                                CHECK(occurrence.tempo_scale_numerator == 1);
                                CHECK(occurrence.tempo_scale_denominator == 2);
                                if (mode == MatchMode::transposed)
                                    CHECK(occurrence.transpose == -3);
                            }
                        }
                    }
                }
            }
        }
    }

    TEST_CASE("rhythm constraints are independent of pitch; results past the limit are partial") {
        InMemoryMotifIndex index;
        index.add(stored_stream(1, melody({"C4", "D4", "E4", "F4", "G4", "A4"}, 480)));
        auto uneven = melody({"C4", "D4", "E4", "F4", "G4", "A4"}, 480);
        uneven[3].onset += 240;
        uneven[4].onset += 240;
        uneven[5].onset += 240;
        index.add(stored_stream(2, uneven));
        ExactQuery query;
        query.mode = MatchMode::transposed;
        query.notes = melody({"D4", "E4", "F#4", "G4", "A4", "B4"}, 240);
        auto result = exact_search(index, query);
        REQUIRE(result.has_value());
        CHECK(result->occurrences.size() == 2);
        query.require_rhythm = true;
        result = exact_search(index, query);
        REQUIRE(result->occurrences.size() == 1);
        CHECK(result->occurrences[0].stream.stream_id == 1);

        query.require_rhythm = false;
        query.result_limit = 1;
        result = exact_search(index, query);
        REQUIRE(result->occurrences.size() == 1);
        CHECK(result->partial);
        REQUIRE(result->next.has_value());
        query.after = result->next;
        result = exact_search(index, query);
        REQUIRE(result->occurrences.size() == 1);
        CHECK(result->occurrences[0].stream.stream_id == 2);
        CHECK_FALSE(result->partial);

        std::stop_source stop;
        stop.request_stop();
        query.after.reset();
        query.result_limit = 100;
        result = exact_search(index, query, stop.get_token());
        CHECK(result->partial);
    }

    TEST_CASE("approximate scoring: identical, transposed, scaled, edited and rejected windows") {
        const auto base = melody({"C4", "E4", "G4", "C5", "B4", "G4", "E4", "D4"});
        ApproximateQuery query;
        query.notes = base;
        InMemoryMotifIndex index;
        index.add(stored_stream(1, base));
        auto changed = base;
        changed[3].pitch = static_cast<std::uint8_t>(changed[3].pitch + 2);
        index.add(stored_stream(2, changed));
        auto slow = base;
        for (auto& note : slow) {
            note.onset *= 2;
            note.duration *= 2;
            note.pitch = static_cast<std::uint8_t>(note.pitch + 5);
        }
        index.add(stored_stream(3, slow));
        auto missing = base;
        missing.erase(missing.begin() + 4);
        index.add(stored_stream(4, missing));
        index.add(stored_stream(5, melody({"C2", "C6", "C2", "C6", "C2", "C6", "C2", "C6"}, 30)));

        const auto exact = align_window(query, stored_stream(1, base), 0);
        REQUIRE(exact.has_value());
        CHECK(exact->components.score == doctest::Approx(100.0));
        CHECK(exact->components.matched == base.size());

        const auto scaled = align_window(query, stored_stream(3, slow), 0);
        REQUIRE(scaled.has_value());
        CHECK(scaled->components.time_scale == doctest::Approx(2.0));
        CHECK(scaled->components.rhythm == doctest::Approx(0.0));
        CHECK(scaled->components.duration == doctest::Approx(0.0));
        CHECK(scaled->components.transpose == 5);
        CHECK(scaled->components.score == doctest::Approx(100.0));

        const auto pitch = align_window(query, stored_stream(2, changed), 0);
        REQUIRE(pitch.has_value());
        CHECK(pitch->components.pitch > 0.0);
        // Two neighbouring intervals change by 2 semitones each: mean over 7 intervals of 2/12.
        CHECK(pitch->components.pitch == doctest::Approx((2.0 / 12.0 + 2.0 / 12.0) / 7.0));
        CHECK(pitch->components.distance == doctest::Approx(0.45 * pitch->components.pitch));
        CHECK(pitch->explanation.find("pitch") != std::string::npos);

        const auto edited = align_window(query, stored_stream(4, missing), 0);
        REQUIRE(edited.has_value());
        CHECK(edited->components.deletions == 1);
        CHECK(edited->components.edit == doctest::Approx(1.0 / 8.0));
        CHECK(edited->components.score < 100.0);

        CHECK_FALSE(align_window(query, stored_stream(9, melody({"C4", "D4"})), 0).has_value());

        const auto result = approximate_search(index, query);
        REQUIRE(result.has_value());
        REQUIRE(result->matches.size() >= 3);
        CHECK(result->matches[0].components.score >= result->matches[1].components.score);
        CHECK(result->label.find("approximate search") != std::string::npos);
        const auto ranked_first = result->matches[0].stream.stream_id;
        CHECK((ranked_first == 1 || ranked_first == 3));

        ApproximateQuery rhythm;
        rhythm.notes = base;
        rhythm.rhythm_only = true;
        const auto rhythm_match = align_window(rhythm, stored_stream(2, changed), 0);
        REQUIRE(rhythm_match.has_value());
        CHECK(rhythm_match->components.weights.pitch == 0.0);
        CHECK(rhythm_match->components.score == doctest::Approx(100.0));

        ApproximateQuery short_query;
        short_query.notes = melody({"C4", "D4", "E4"});
        CHECK_FALSE(approximate_search(index, short_query).has_value());
    }

    TEST_CASE("codec round trip and corrupt-input rejection") {
        auto stored = stored_stream(1, melody({"C4", "E4", "G4", "B4"}));
        stored.stream.sequence_no = 3;
        stored.stream.track_no = 7;
        stored.stream.channel = 5;
        stored.stream.settings_hash[0] = 0xAB;
        stored.stream.notes.back().onset = 1'000'000'000'000;
        stored.stream.notes.back().end = 1'000'000'000'480;
        const auto bytes = encode_motif_stream(stored.stream);
        CHECK(std::equal(bytes.begin(), bytes.begin() + 4, std::string_view("MWMS").begin()));
        const auto decoded = decode_motif_stream(bytes);
        REQUIRE(decoded.has_value());
        CHECK(*decoded == stored.stream);

        auto smpte = stored.stream;
        smpte.division = Division::make_smpte(SmpteFormat::fps29_97_drop, 80);
        CHECK(*decode_motif_stream(encode_motif_stream(smpte)) == smpte);

        CHECK_FALSE(decode_motif_stream(std::span(bytes).first(bytes.size() - 1)).has_value());
        auto trailing = bytes;
        trailing.push_back(0);
        CHECK_FALSE(decode_motif_stream(trailing).has_value());
        auto magic = bytes;
        magic[0] = 'X';
        CHECK_FALSE(decode_motif_stream(magic).has_value());
        auto version = bytes;
        version[4] = 2;
        CHECK(decode_motif_stream(version).error().code == ErrorCode::unsupported);
        auto count = bytes;
        count[4 + 2 + 2 + 1 + 1 + 2 + 2 + 32 + 4 + 4 + 1] = 0xFF;
        CHECK_FALSE(decode_motif_stream(count).has_value());
        CHECK_FALSE(decode_motif_stream(bytes, 2).has_value());
        auto unordered = stored.stream;
        std::swap(unordered.notes[1].onset, unordered.notes[2].onset);
        std::swap(unordered.notes[1].end, unordered.notes[2].end);
        // The encoder writes deltas from the previous onset; a negative delta wraps and is rejected.
        CHECK_FALSE(decode_motif_stream(encode_motif_stream(unordered)).has_value());
        // Every single-byte corruption either fails cleanly or decodes into a valid stream.
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            auto corrupted = bytes;
            corrupted[index] ^= 0x5A;
            const auto result = decode_motif_stream(corrupted);
            if (result) {
                for (std::size_t note = 1; note < result->notes.size(); ++note) {
                    CHECK(result->notes[note].onset > result->notes[note - 1].onset);
                    CHECK(result->notes[note].ordinal > result->notes[note - 1].ordinal);
                }
            }
        }
    }

    TEST_CASE("top-voice extraction groups onsets, skips drums and indexes grams") {
        const auto streams = extract_motif_streams(chord_sequence(), 0).value();
        REQUIRE(streams.size() == 1);
        const auto& stream = streams.front();
        REQUIRE(stream.notes.size() == 3);
        CHECK(stream.notes[0].pitch == 67);
        CHECK(stream.notes[1].pitch == 62);
        CHECK(stream.notes[2].pitch == 64);
        CHECK(stream.notes[0].ordinal < stream.notes[1].ordinal);
        ExtractionSettings bottom;
        bottom.voice = VoiceKind::bottom;
        CHECK(extract_motif_streams(chord_sequence(), 0, bottom).value().front().notes[0].pitch == 60);
        ExtractionSettings drums;
        drums.include_drums = true;
        CHECK(extract_motif_streams(chord_sequence(), 0, drums).value().size() == 1); // The lone drum note is too short.
        CHECK(stream.settings_hash != extract_motif_streams(chord_sequence(), 0, bottom).value().front().settings_hash);
        const auto grams = stream_grams(stream);
        // 3 pitch tokens -> one 3-gram; 2 interval / 2 contour / 1 rhythm tokens -> none.
        CHECK(grams.size() == 1);
        CHECK(gram_hash_from_bytes(gram_hash_bytes(grams[0].key.hash)) == grams[0].key.hash);
        CHECK(gram_hash(TokenFamily::interval, std::vector<std::int32_t>{4, 3, 5}) ==
              gram_hash(TokenFamily::interval, std::vector<std::int32_t>{4, 3, 5}));
        CHECK(gram_hash(TokenFamily::interval, std::vector<std::int32_t>{4, 3, 5}) !=
              gram_hash(TokenFamily::pitch, std::vector<std::int32_t>{4, 3, 5}));
    }
}
