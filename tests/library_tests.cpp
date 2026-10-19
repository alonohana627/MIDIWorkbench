#include "test_support.hpp"

#include "mwb/core/text.hpp"
#include "mwb/document/sqlite_support.hpp"
#include "mwb/library/bench.hpp"
#include "mwb/library/catalog_schema.hpp"
#include "mwb/library/library.hpp"
#include "mwb/library/maintenance.hpp"
#include "mwb/library/storage.hpp"
#include "mwb/search/browse_index.hpp"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <set>

using namespace mwb;
using namespace mwb::library;
using Bytes = std::vector<std::uint8_t>;

namespace {

// --- Hand-built Standard MIDI Files (independent of the writer) ---------------------------------

void append_vlq(Bytes& out, std::uint32_t value) {
    std::array<std::uint8_t, 5> buffer{};
    std::size_t count = 0;
    buffer[count++] = static_cast<std::uint8_t>(value & 0x7F);
    while ((value >>= 7) != 0)
        buffer[count++] = static_cast<std::uint8_t>((value & 0x7F) | 0x80);
    while (count > 0)
        out.push_back(buffer[--count]);
}

void append_be(Bytes& out, std::uint32_t value, int bytes) {
    for (int shift = (bytes - 1) * 8; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

/// Collects timed track events and serializes an MTrk body.
struct TrackBuilder {
    std::vector<std::pair<std::uint32_t, Bytes>> events;
    void add(std::uint32_t tick, Bytes message) { events.emplace_back(tick, std::move(message)); }
    [[nodiscard]] Bytes finish() {
        std::ranges::stable_sort(events, {}, &std::pair<std::uint32_t, Bytes>::first);
        Bytes out;
        std::uint32_t last = 0;
        for (const auto& [tick, message] : events) {
            append_vlq(out, tick - last);
            out.insert(out.end(), message.begin(), message.end());
            last = tick;
        }
        append_vlq(out, 0);
        out.insert(out.end(), {0xFF, 0x2F, 0x00});
        return out;
    }
};

Bytes smf(int format, const std::vector<Bytes>& tracks, std::uint16_t division = 960) {
    Bytes out{'M', 'T', 'h', 'd'};
    append_be(out, 6, 4);
    append_be(out, static_cast<std::uint32_t>(format), 2);
    append_be(out, static_cast<std::uint32_t>(tracks.size()), 2);
    append_be(out, division, 2);
    for (const auto& track : tracks) {
        out.insert(out.end(), {'M', 'T', 'r', 'k'});
        append_be(out, static_cast<std::uint32_t>(track.size()), 4);
        out.insert(out.end(), track.begin(), track.end());
    }
    return out;
}

Bytes tempo_meta(std::uint32_t microseconds) {
    return {0xFF, 0x51, 0x03, static_cast<std::uint8_t>(microseconds >> 16), static_cast<std::uint8_t>(microseconds >> 8),
            static_cast<std::uint8_t>(microseconds)};
}

/// Description of one generated phrase.
struct Phrase {
    std::vector<int> pitches{60, 62, 64, 65, 67, 69, 71, 72};
    std::uint32_t tempo_us = 500000;
    std::optional<std::uint32_t> second_tempo_us;
    std::optional<std::pair<int, bool>> key; ///< (sharps/flats, minor)
    std::string name;
    std::uint8_t channel = 0;
    int program = -1;
    bool bend = false;
    std::uint32_t step = 480;
    std::uint8_t numerator = 4;
    std::uint8_t denominator_power = 2;
    std::string lyric;

    Phrase& set_pitches(std::vector<int> value) {
        pitches = std::move(value);
        return *this;
    }
    Phrase& set_tempo_us(std::uint32_t value) {
        tempo_us = std::move(value);
        return *this;
    }
    Phrase& set_second_tempo_us(std::uint32_t value) {
        second_tempo_us = std::move(value);
        return *this;
    }
    Phrase& set_key(std::pair<int, bool> value) {
        key = std::move(value);
        return *this;
    }
    Phrase& set_name(std::string value) {
        name = std::move(value);
        return *this;
    }
    Phrase& set_channel(std::uint8_t value) {
        channel = std::move(value);
        return *this;
    }
    Phrase& set_program(int value) {
        program = std::move(value);
        return *this;
    }
    Phrase& set_bend(bool value) {
        bend = std::move(value);
        return *this;
    }
    Phrase& set_numerator(std::uint8_t value) {
        numerator = std::move(value);
        return *this;
    }
    Phrase& set_denominator_power(std::uint8_t value) {
        denominator_power = std::move(value);
        return *this;
    }
    Phrase& set_lyric(std::string value) {
        lyric = std::move(value);
        return *this;
    }
};

Bytes phrase_track(const Phrase& phrase, bool with_tempo = true) {
    TrackBuilder track;
    if (with_tempo)
        track.add(0, tempo_meta(phrase.tempo_us));
    track.add(0, {0xFF, 0x58, 0x04, phrase.numerator, phrase.denominator_power, 0x18, 0x08});
    if (phrase.key)
        track.add(0, {0xFF, 0x59, 0x02, static_cast<std::uint8_t>(static_cast<std::int8_t>(phrase.key->first)),
                      static_cast<std::uint8_t>(phrase.key->second ? 1 : 0)});
    if (!phrase.name.empty()) {
        Bytes meta{0xFF, 0x03, static_cast<std::uint8_t>(phrase.name.size())};
        meta.insert(meta.end(), phrase.name.begin(), phrase.name.end());
        track.add(0, meta);
    }
    if (!phrase.lyric.empty()) {
        Bytes meta{0xFF, 0x05, static_cast<std::uint8_t>(phrase.lyric.size())};
        meta.insert(meta.end(), phrase.lyric.begin(), phrase.lyric.end());
        track.add(0, meta);
    }
    if (phrase.program >= 0)
        track.add(0, {static_cast<std::uint8_t>(0xC0 | phrase.channel), static_cast<std::uint8_t>(phrase.program)});
    const auto pitch_count = static_cast<std::uint32_t>(phrase.pitches.size());
    if (phrase.second_tempo_us)
        track.add(phrase.step * (pitch_count / 2), tempo_meta(*phrase.second_tempo_us));
    for (std::uint32_t index = 0; index < pitch_count; ++index) {
        const auto pitch = static_cast<std::uint8_t>(phrase.pitches[index]);
        track.add(index * phrase.step, {static_cast<std::uint8_t>(0x90 | phrase.channel), pitch, 100});
        track.add(index * phrase.step + phrase.step / 2, {static_cast<std::uint8_t>(0x80 | phrase.channel), pitch, 0});
    }
    if (phrase.bend)
        track.add(phrase.step, {static_cast<std::uint8_t>(0xE0 | phrase.channel), 0x00, 0x50});
    return track.finish();
}

Bytes phrase_smf(const Phrase& phrase) { return smf(0, {phrase_track(phrase)}); }

void write_bytes(const std::filesystem::path& path, const Bytes& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::unique_ptr<Library> make_library(const std::filesystem::path& root) {
    auto library = Library::create(root);
    if (!library)
        FAIL("create failed: " << library.error().describe());
    return std::move(*library);
}

/// Every matching asset ID through keyset pages of `page_size`.
std::vector<std::int64_t> all_ids(Library& library, SearchRequest request, std::size_t page_size = 100) {
    request.page_size = page_size;
    std::vector<std::int64_t> ids;
    for (int guard = 0; guard < 10'000; ++guard) {
        auto page = library.search(request);
        if (!page)
            FAIL("search failed: " << page.error().describe());
        for (const auto& row : page->rows)
            ids.push_back(row.id);
        if (!page->next)
            break;
        request.after = page->next;
    }
    return ids;
}

std::vector<std::int64_t> sorted(std::vector<std::int64_t> ids) {
    std::ranges::sort(ids);
    return ids;
}

std::int64_t only_asset(const ImportReport& report, std::string_view path_suffix) {
    for (const auto& item : report.items)
        if (item.display_path.ends_with(path_suffix) && item.asset_id)
            return *item.asset_id;
    FAIL("no imported item ends with " << path_suffix);
    return 0;
}

GarbageCollectionOptions gc_at(std::int64_t now, std::int64_t grace) {
    GarbageCollectionOptions options;
    options.now_ms = now;
    options.grace_period_ms = grace;
    return options;
}

BenchOptions bench_options(std::size_t rows, std::size_t repetitions) {
    BenchOptions options;
    options.rows = rows;
    options.repetitions = repetitions;
    return options;
}

search::BrowseIndex build_index(Library& library) {
    search::BrowseIndex index;
    std::int64_t after = 0;
    while (true) {
        auto rows = library.browse_rows(after, 200);
        REQUIRE(rows.has_value());
        if (rows->empty())
            break;
        for (const auto& row : *rows)
            index.upsert(row);
        after = rows->back().id;
    }
    return index;
}

} // namespace

TEST_SUITE("catalog") {
    TEST_CASE("a new library has the exact section 23.2 schema and the connection policy") {
        test::TemporaryDirectory temporary;
        const auto root = temporary.path() / "library";
        {
            auto library = make_library(root);
            CHECK_FALSE(library->uuid().is_nil());
            for (const auto* entry : {"objects", "staging", "cache", "backups", "library.json", "library.sqlite"})
                CHECK(std::filesystem::exists(root / entry));
            SQLite::Database database((root / "library.sqlite").string(), SQLite::OPEN_READONLY);
            CHECK(document::sqlite_support::pragma_integer(database, "application_id") == catalog_application_id);
            CHECK(document::sqlite_support::pragma_integer(database, "user_version") == catalog_schema_version);
            SQLite::Statement mode(database, "PRAGMA journal_mode");
            REQUIRE(mode.executeStep());
            CHECK(mode.getColumn(0).getString() == "wal");
            const auto actual = document::sqlite_support::read_schema_objects(database);
            for (const auto& expected : document::sqlite_support::reference_schema_objects(catalog_ddl_v1)) {
                CAPTURE(expected.name);
                CHECK(std::ranges::find(actual, expected) != actual.end());
            }
        }
        // Reopen after close works; a second writer is refused with a busy diagnostic.
        auto first = Library::open(root);
        REQUIRE(first.has_value());
        auto second = Library::open(root);
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().code == ErrorCode::busy);
        CHECK(second.error().message.find("already open for writing") != std::string::npos);
    }

    TEST_CASE("a catalog with a newer schema is refused") {
        test::TemporaryDirectory temporary;
        const auto root = temporary.path() / "library";
        make_library(root).reset();
        {
            SQLite::Database database((root / "library.sqlite").string(), SQLite::OPEN_READWRITE);
            database.exec(std::format("PRAGMA user_version = {}", catalog_schema_version + 1));
        }
        auto opened = Library::open(root);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().code == ErrorCode::unsupported);
    }

    TEST_CASE("tags, ratings, favorites, collections and the change outbox") {
        test::TemporaryDirectory temporary;
        auto library = make_library(temporary.path() / "library");
        const auto source = temporary.path() / "in";
        write_bytes(source / "one.mid", phrase_smf(Phrase{}));
        write_bytes(source / "two.mid", phrase_smf(Phrase{}.set_pitches({50, 52, 53})));
        auto report = library->import_paths({source});
        REQUIRE(report.has_value());
        const auto one = only_asset(*report, "one.mid");
        const auto two = only_asset(*report, "two.mid");
        std::vector<std::int64_t> notified;
        library->set_change_listener([&](std::int64_t revision) { notified.push_back(revision); });
        const auto start = library->changes_since(0)->latest_revision;

        REQUIRE(library->add_tag(one, "  Bass ").has_value());
        REQUIRE(library->add_tag(one, "BASS").has_value());
        REQUIRE(library->add_tag(two, "Groß").has_value());
        auto tags = library->list_tags();
        REQUIRE(tags.has_value());
        REQUIRE(tags->size() == 2);
        CHECK(tags->at(0).normalized_name == "bass");
        CHECK(tags->at(0).name == "Bass");
        CHECK(tags->at(0).asset_count == 1);
        CHECK(tags->at(1).normalized_name == fold_for_search("Groß"));
        auto changes = library->changes_since(start);
        REQUIRE(changes.has_value());
        CHECK(changes->asset_ids == std::vector<std::int64_t>{one, two});
        CHECK(changes->structure_changed);
        CHECK_FALSE(changes->full_reload);
        CHECK(notified.size() == 2); // The duplicate tag link changed nothing and recorded nothing.
        CHECK(notified.back() == changes->latest_revision);

        const auto after_tags = changes->latest_revision;
        REQUIRE(library->set_rating(one, 4).has_value());
        REQUIRE(library->set_favorite(two, true).has_value());
        CHECK_FALSE(library->set_rating(one, 6).has_value());
        changes = library->changes_since(after_tags);
        CHECK(changes->asset_ids == std::vector<std::int64_t>{one, two});
        CHECK_FALSE(changes->structure_changed);
        CHECK(library->changes_since(0, 2)->full_reload);
        CHECK(library->changes_since(changes->latest_revision + 100)->full_reload);
        CHECK(library->asset(one)->value().rating == 4);
        CHECK(library->asset(two)->value().favorite);

        const auto manual = library->create_collection("Favourites");
        REQUIRE(manual.has_value());
        REQUIRE(library->add_to_collection(*manual, {one, two}).has_value());
        CHECK(*library->collection_members(*manual) == std::vector<std::int64_t>{one, two});
        REQUIRE(library->move_in_collection(*manual, two, 0).has_value());
        CHECK(*library->collection_members(*manual) == std::vector<std::int64_t>{two, one});
        CHECK(library->collection_members(*manual, 1)->size() == 1);
        const auto smart = library->create_smart_collection("Fast bass", "tag:bass bpm:>100");
        REQUIRE(smart.has_value());
        CHECK_FALSE(library->add_to_collection(*smart, {one}).has_value());
        auto query = library->smart_collection_query(*smart);
        REQUIRE(query.has_value());
        CHECK(query->root.children.size() == 2);
        CHECK_FALSE(library->create_smart_collection("bad", "bpm:fast").has_value());

        SearchRequest by_collection;
        by_collection.query_text = "collection:favourites";
        CHECK(sorted(all_ids(*library, by_collection)) == std::vector<std::int64_t>{one, two});
        SearchRequest scoped;
        scoped.query_text = "tag:bass";
        scoped.collection_scope = *manual;
        CHECK(all_ids(*library, scoped) == std::vector<std::int64_t>{one});
        REQUIRE(library->remove_tag(one, "bass").has_value());
        CHECK(all_ids(*library, scoped).empty());
        // Long multi-byte titles are recorded in the change outbox without splitting a character.
        std::string long_title;
        for (int index = 0; index < 120; ++index)
            long_title += "\xE6\xBC\xA2"; // a 3-byte CJK character
        REQUIRE(library->set_title(one, "x" + long_title).has_value());
        REQUIRE(library->set_title(one, long_title).has_value());
    }

    TEST_CASE("keyset pages are complete, ordered and never use offsets") {
        test::TemporaryDirectory temporary;
        auto library = make_library(temporary.path() / "library");
        REQUIRE(library->populate_synthetic(260, 7).value() == 260);
        SearchRequest request;
        const auto paged = all_ids(*library, request, 37);
        CHECK(paged.size() == 260);
        CHECK(std::set<std::int64_t>(paged.begin(), paged.end()).size() == 260);
        const auto big = all_ids(*library, request, 500);
        CHECK(paged == big);
        for (const auto field : {search::SortField::modified, search::SortField::created}) {
            SearchRequest by_time;
            by_time.order.field = field;
            by_time.order.descending = true;
            CHECK(sorted(all_ids(*library, by_time, 50)) == sorted(big));
        }
        auto compiled = library->compile_search(request);
        REQUIRE(compiled.has_value());
        CHECK(compiled->statement.sql.find("OFFSET") == std::string::npos);
        SearchRequest text;
        text.query_text = "lead";
        text.order.field = search::SortField::relevance;
        const auto relevance = all_ids(*library, text, 10);
        SearchRequest text_by_title;
        text_by_title.query_text = "lead";
        CHECK(sorted(relevance) == sorted(all_ids(*library, text_by_title)));
        CHECK_FALSE(relevance.empty());
    }

    TEST_CASE("golden case 10: type-2 tempo and key in different sequences do not satisfy one AND group") {
        test::TemporaryDirectory temporary;
        auto library = make_library(temporary.path() / "library");
        auto sequence_a = Phrase{}.set_tempo_us(400000);                      // 150 BPM, no key signature
        auto sequence_b = Phrase{}.set_tempo_us(500000).set_key(std::pair{-4, true}); // 120 BPM, F minor
        write_bytes(temporary.path() / "in" / "type2.mid", smf(2, {phrase_track(sequence_a), phrase_track(sequence_b)}));
        write_bytes(temporary.path() / "in" / "together.mid",
                    phrase_smf(Phrase{}.set_tempo_us(400000).set_key(std::pair{-4, true})));
        auto report = library->import_paths({temporary.path() / "in"});
        REQUIRE(report.has_value());
        const auto type2 = only_asset(*report, "type2.mid");
        const auto together = only_asset(*report, "together.mid");
        auto index = build_index(*library);
        const auto check = [&](const std::string& text, bool across, std::vector<std::int64_t> expected) {
            CAPTURE(text);
            SearchRequest request;
            request.query_text = text;
            request.across_sequences = across;
            CHECK(sorted(all_ids(*library, request)) == sorted(expected));
            const auto query = search::parse_query(text).value();
            const auto options = library->resolve_search_options(query, std::nullopt, across).value();
            CHECK(index.matching_ids(query, options).value() == sorted(expected));
        };
        check("bpm:150 key:\"F minor\"", false, {together});
        check("bpm:150 AND key:\"F minor\"", false, {together});
        check("bpm:150", false, {type2, together});
        check("key:\"F minor\" key.origin:signature", false, {type2, together});
        check("bpm:150 key:\"F minor\"", true, {type2, together});
    }

    TEST_CASE("parity: every section 25.2 example gives identical SQL and in-memory results") {
        test::TemporaryDirectory temporary;
        auto library = make_library(temporary.path() / "library");
        const auto in = temporary.path() / "in";
        write_bytes(in / "bass line.mid", phrase_smf(Phrase{}.set_tempo_us(400000).set_program(33)));        // 150 BPM, 8 notes
        write_bytes(in / "fsharp.mid", phrase_smf(Phrase{}.set_pitches(std::vector<int>(20, 66))));         // 9.75 quarter notes
        write_bytes(in / "rising lead.mid", phrase_smf(Phrase{}.set_name("Rising Lead")));
        write_bytes(in / "rising lead drums.mid", phrase_smf(Phrase{}.set_name("Rising Lead").set_channel(9)));
        write_bytes(in / "bendy.mid", phrase_smf(Phrase{}.set_bend(true)));
        write_bytes(in / "long.mid", phrase_smf(Phrase{}.set_pitches(std::vector<int>(160, 60))));           // 40 s at 120 BPM
        write_bytes(in / "melody.mid", phrase_smf(Phrase{}.set_pitches({72, 74, 76}).set_lyric("la la")));
        write_bytes(in / "arpeggio.mid", phrase_smf(Phrase{}.set_pitches({60, 64, 67, 72}).set_numerator(6).set_denominator_power(3)));
        write_bytes(in / "change.mid", phrase_smf(Phrase{}.set_tempo_us(500000).set_second_tempo_us(400000)));
        write_bytes(in / "broken.mid", Bytes{'n', 'o', 't', ' ', 'm', 'i', 'd', 'i'});
        auto report = library->import_paths({in});
        REQUIRE(report.has_value());
        const auto bass = only_asset(*report, "bass line.mid");
        const auto fsharp = only_asset(*report, "fsharp.mid");
        const auto melody = only_asset(*report, "melody.mid");
        const auto arpeggio = only_asset(*report, "arpeggio.mid");
        REQUIRE(library->add_tag(bass, "bass").has_value());
        REQUIRE(library->set_favorite(bass, true).has_value());
        REQUIRE(library->set_key_override(fsharp, 0, 6, "minor").has_value());
        REQUIRE(library->add_tag(melody, "melody").has_value());
        REQUIRE(library->add_tag(arpeggio, "Arpeggio").has_value());
        REQUIRE(library->set_rating(melody, 4).has_value());
        REQUIRE(library->set_rating(arpeggio, 5).has_value());
        const auto collection = library->create_collection("Picks").value();
        REQUIRE(library->add_to_collection(collection, {bass, melody}).has_value());
        REQUIRE(library->populate_synthetic(150, 3).has_value());

        const std::vector<std::string> examples{
            "tag:bass bpm:140..155 notes:>=8 favorite:true",
            "key:\"F# minor\" key.origin:manual length.qn:8..32",
            "text:\"rising lead\" AND NOT drums:true",
            "has:bend AND duration:<30s",
            "(tag:melody OR tag:arpeggio) rating:>=4",
            "tempo.any:140..160 tempo.origin:explicit",
            // Additional coverage of the field table.
            "program:34 channel:1", "meter:6/8", "pitch.min:<=C4 pitch.max:>=72", "status:error", "status:ready",
            "source:fsharp", "collection:picks", "tracks:1 notes:3", "has:lyrics OR has:bend", "NOT tag:bass rating:0",
            "duration:>=30s", "confidence:>0.5", "role:unspecified favorite:false", "bpm:>=150 OR key:C", "lead"};
        auto index = build_index(*library);
        CHECK(index.memory_bytes() > 0);
        std::size_t non_empty_examples = 0;
        for (const auto& example : examples) {
            CAPTURE(example);
            SearchRequest request;
            request.query_text = example;
            const auto sql_ids = sorted(all_ids(*library, request, 64));
            const auto query = search::parse_query(example).value();
            const auto options = library->resolve_search_options(query, std::nullopt, false).value();
            const auto memory_ids = index.matching_ids(query, options);
            REQUIRE(memory_ids.has_value());
            CHECK(sql_ids == *memory_ids);
            if (!sql_ids.empty() && &example - examples.data() < 6)
                ++non_empty_examples;
            // The index pages exactly like SQL.
            auto page = index.page(query, options, {}, std::nullopt, 500);
            REQUIRE(page.has_value());
            CHECK(sorted(page->ids) == sql_ids);
        }
        CHECK(non_empty_examples == 6); // Every specification example matches at least one fixture.
        const auto check_contains = [&](const std::string& text, std::int64_t id) {
            SearchRequest request;
            request.query_text = text;
            const auto ids = all_ids(*library, request);
            CHECK(std::ranges::find(ids, id) != ids.end());
        };
        check_contains(examples[0], bass);
        check_contains(examples[1], fsharp);
        check_contains(examples[4], melody);
        check_contains(examples[4], arpeggio);
    }
}

TEST_SUITE("import") {
    TEST_CASE("folder import: counts, duplicates, unsupported objects and re-import") {
        test::TemporaryDirectory temporary;
        auto library = make_library(temporary.path() / "library");
        const auto in = temporary.path() / "in";
        const auto shared = phrase_smf(Phrase{}.set_pitches({60, 61, 62}));
        write_bytes(in / "a" / "first.mid", phrase_smf(Phrase{}));
        write_bytes(in / "a" / "same.mid", shared);
        write_bytes(in / "b" / "copy.MID", shared);
        write_bytes(in / "b" / "deep" / "Hebrew \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D.mid", phrase_smf(Phrase{}.set_pitches({70, 71})));
        write_bytes(in / "b" / "broken.mid", Bytes{'R', 'I', 'F', 'F', 0, 0});
        write_bytes(in / "b" / "notes.txt", Bytes{'x'});
        auto report = library->import_paths({in});
        REQUIRE(report.has_value());
        const auto& counts = report->counts;
        CHECK(counts.discovered == 5);
        CHECK(counts.captured == 5);
        CHECK(counts.imported == 3);
        CHECK(counts.duplicate == 1);
        CHECK(counts.unsupported == 1);
        CHECK(counts.failed == 0);
        CHECK(report->to_json().find("\"unsupported\":1") != std::string::npos);
        const auto same = only_asset(*report, "same.mid");
        CHECK(only_asset(*report, "copy.MID") == same); // Equal bytes from another path attach a source.
        const auto broken = only_asset(*report, "broken.mid");

        // The unsupported file is stored as an object with an error state.
        const auto revisions = library->revisions(broken).value();
        REQUIRE(revisions.size() == 1);
        CHECK(std::filesystem::exists(ObjectStore(library->root()).object_path(revisions[0].object_sha256, "mid")));
        SearchRequest errors;
        errors.query_text = "status:error";
        CHECK(all_ids(*library, errors) == std::vector<std::int64_t>{broken});
        SearchRequest by_source;
        by_source.query_text = "source:copy";
        CHECK(all_ids(*library, by_source) == std::vector<std::int64_t>{same});

        // Re-import: nothing new, no redundant revisions.
        auto again = library->import_paths({in});
        REQUIRE(again.has_value());
        CHECK(again->counts.imported == 0);
        CHECK(again->counts.duplicate == 5); // The unsupported object is a duplicate of itself too.
        CHECK(again->counts.unsupported == 0);
        CHECK(library->revisions(same)->size() == 1);
        SearchRequest everything;
        CHECK(all_ids(*library, everything).size() == 4);

        // Separate-asset policy creates a new identity for equal bytes from a new path.
        write_bytes(temporary.path() / "other" / "third.mid", shared);
        ImportOptions separate;
        separate.duplicates = DuplicatePolicy::separate_asset;
        auto third = library->import_paths({temporary.path() / "other" / "third.mid"}, separate);
        REQUIRE(third.has_value());
        CHECK(third->counts.imported == 1);
        CHECK(*third->items[0].asset_id != same);
    }

    TEST_CASE("a source that changes while it is read is retried, then reported") {
        test::TemporaryDirectory temporary;
        auto library = make_library(temporary.path() / "library");
        const auto file = temporary.path() / "in" / "moving.mid";
        write_bytes(file, phrase_smf(Phrase{}));
        ImportOptions always;
        always.after_capture = [&](const std::filesystem::path& path, std::size_t attempt) {
            write_bytes(path, phrase_smf(Phrase{}.set_pitches(std::vector<int>(attempt + 2, 60))));
        };
        auto unstable = library->import_paths({file}, always);
        REQUIRE(unstable.has_value());
        CHECK(unstable->counts.changed_during_read == 1);
        CHECK(unstable->counts.imported == 0);
        CHECK(std::filesystem::is_empty(library->root() / "staging"));

        ImportOptions once;
        once.after_capture = [&](const std::filesystem::path& path, std::size_t attempt) {
            if (attempt == 1)
                write_bytes(path, phrase_smf(Phrase{}.set_pitches({40, 41, 42, 43})));
        };
        auto stable = library->import_paths({file}, once);
        REQUIRE(stable.has_value());
        CHECK(stable->counts.imported == 1);
        // The captured (second) bytes are authoritative.
        const auto asset = *stable->items[0].asset_id;
        const auto head = library->revisions(asset)->back().id;
        CHECK(library->revision_bytes(head).value() == phrase_smf(Phrase{}.set_pitches({40, 41, 42, 43})));
    }

    TEST_CASE("a changed source creates a new imported revision; cancellation is counted") {
        test::TemporaryDirectory temporary;
        auto library = make_library(temporary.path() / "library");
        const auto file = temporary.path() / "in" / "song.mid";
        write_bytes(file, phrase_smf(Phrase{}));
        const auto asset = *library->import_paths({file})->items[0].asset_id;
        write_bytes(file, phrase_smf(Phrase{}.set_pitches({50, 55, 59, 62, 67})));
        auto changed = library->import_paths({file});
        REQUIRE(changed.has_value());
        CHECK(changed->counts.imported == 1);
        CHECK(*changed->items[0].asset_id == asset);
        CHECK(library->revisions(asset)->size() == 2);

        // A user edit makes the asset diverge: the next source change needs review instead.
        const auto head = library->asset(asset)->value().current_revision_id;
        REQUIRE(library->commit_revision(CommitRequest{asset, head, RevisionOrigin::edit, "edit"},
                                         phrase_smf(Phrase{}.set_pitches({30, 31, 32})), MediaType::smf)
                    .has_value());
        write_bytes(file, phrase_smf(Phrase{}.set_pitches({90, 91, 92})));
        const auto edited_head = library->asset(asset)->value().current_revision_id;
        auto review = library->import_paths({file});
        CHECK(review->counts.source_changed == 1);
        // The changed source is kept as a candidate revision for review; the edit stays the head
        // (code-library-10), and restoring the candidate adopts the source's bytes.
        CHECK(library->revisions(asset)->size() == 4);
        CHECK(library->asset(asset)->value().current_revision_id == edited_head);
        REQUIRE(review->items[0].candidate_revision_id.has_value());
        const auto candidate = *review->items[0].candidate_revision_id;
        CHECK(library->revision_bytes(candidate).value() == phrase_smf(Phrase{}.set_pitches({90, 91, 92})));
        const auto adopted = library->restore_revision(asset, candidate, edited_head);
        REQUIRE(adopted.has_value());
        CHECK(library->revision_bytes(*adopted).value() == phrase_smf(Phrase{}.set_pitches({90, 91, 92})));

        std::stop_source stop;
        stop.request_stop();
        auto cancelled = library->import_paths({temporary.path() / "in"}, {}, stop.get_token());
        REQUIRE(cancelled.has_value());
        CHECK(cancelled->counts.cancelled == 1);
        CHECK(cancelled->counts.discovered == 1);
    }

    TEST_CASE("analysis writes motif streams, grams, fingerprints and feature vectors; motif search uses them") {
        test::TemporaryDirectory temporary;
        auto library = make_library(temporary.path() / "library");
        const auto in = temporary.path() / "in";
        write_bytes(in / "theme.mid", phrase_smf(Phrase{}.set_pitches({60, 64, 67, 72, 71, 67, 64, 62})));
        write_bytes(in / "theme up.mid", phrase_smf(Phrase{}.set_pitches({62, 66, 69, 74, 73, 69, 66, 64})));
        write_bytes(in / "other.mid", phrase_smf(Phrase{}.set_pitches({60, 65, 67, 60, 65, 67, 60, 65})));
        auto report = library->import_paths({in});
        REQUIRE(report.has_value());
        auto analyzed = library->analyze();
        REQUIRE(analyzed.has_value());
        CHECK(analyzed->revisions_analyzed == 3);
        CHECK(analyzed->streams == 3);
        CHECK(analyzed->grams > 0);
        CHECK(library->analyze()->revisions_analyzed == 0);

        // Analysis parses through the caller's parser (the isolated worker in the application).
        write_bytes(in / "late.mid", phrase_smf(Phrase{}.set_pitches({55, 57, 59, 60})));
        REQUIRE(library->import_paths({in / "late.mid"}).has_value());
        std::size_t parser_calls = 0;
        AnalyzeOptions counted;
        counted.parser = [&](std::span<const std::uint8_t> bytes, std::string_view name, const midi::ParseLimits& limits,
                             std::stop_token token) {
            ++parser_calls;
            return midi::import_midi(bytes, name, limits, std::move(token));
        };
        std::size_t progress_calls = 0;
        counted.progress = [&](std::size_t) { ++progress_calls; };
        CHECK(library->analyze(counted)->revisions_analyzed == 1);
        CHECK(parser_calls == 1);
        CHECK(progress_calls == 1);

        search::ExactQuery query;
        query.mode = search::MatchMode::transposed;
        for (const int pitch : {64, 67, 72, 71, 67})
            query.notes.push_back(search::QueryNote{0, 240, static_cast<std::uint8_t>(pitch), 100});
        for (std::size_t index = 0; index < query.notes.size(); ++index)
            query.notes[index].onset = static_cast<Tick>(index) * 480;
        auto exact = library->motif_search(query);
        REQUIRE(exact.has_value());
        REQUIRE(exact->occurrences.size() == 2); // "late.mid" does not contain the motif.
        CHECK(exact->occurrences[0].first_note == 1);
        std::set<std::int64_t> assets;
        for (const auto& occurrence : exact->occurrences)
            assets.insert(occurrence.stream.asset_id);
        CHECK(assets == std::set<std::int64_t>{only_asset(*report, "theme.mid"), only_asset(*report, "theme up.mid")});

        search::ApproximateQuery similar;
        for (const int pitch : {60, 64, 67, 72, 71, 67, 64, 62})
            similar.notes.push_back(search::QueryNote{static_cast<Tick>(similar.notes.size()) * 480, 240,
                                                      static_cast<std::uint8_t>(pitch), 100});
        auto approximate = library->similar_search(similar);
        REQUIRE(approximate.has_value());
        REQUIRE(approximate->matches.size() >= 2);
        CHECK(approximate->matches[0].components.score == doctest::Approx(100.0));
        CHECK(approximate->label.find("approximate") != std::string::npos);

        // Trashed assets drop out of musical search.
        REQUIRE(library->trash({only_asset(*report, "theme up.mid")}).has_value());
        CHECK(library->motif_search(query)->occurrences.size() == 1);
    }
}

TEST_SUITE("revisions") {
    TEST_CASE("commit with expected head, conflict, restore and native revisions") {
        test::TemporaryDirectory temporary;
        auto library = make_library(temporary.path() / "library");
        auto document = make_blank_document("Phrase");
        auto& track = document.sequences.front().tracks.front();
        auto on = events::note_on(0, 0, 60, 90);
        on.order_key = 3 * order_key_spacing;
        auto off = events::note_off(480, 0, 60);
        off.order_key = 4 * order_key_spacing;
        track.events.push_back(on);
        track.events.push_back(off);
        std::ranges::stable_sort(track.events, EventOrder{});
        const auto asset = library->create_asset(document, "My phrase");
        REQUIRE(asset.has_value());
        const auto first_head = library->asset(*asset)->value().current_revision_id;
        REQUIRE(first_head.has_value());
        auto reopened = library->open_revision(*first_head);
        REQUIRE(reopened.has_value());
        CHECK(reopened->sequences.front().tracks.front().events.size() == track.events.size());

        // Edit and save with the expected head.
        document.title = "Edited";
        const auto second = library->save_document_revision(CommitRequest{*asset, first_head, RevisionOrigin::edit, "v2"},
                                                            document);
        REQUIRE(second.has_value());
        // A stale editor gets a conflict; its object is kept for the conflict workflow.
        const auto stale_bytes = phrase_smf(Phrase{}.set_pitches({10, 20, 30}));
        auto conflict = library->commit_revision(CommitRequest{*asset, first_head, RevisionOrigin::edit, "stale"},
                                                 stale_bytes, MediaType::smf);
        REQUIRE_FALSE(conflict.has_value());
        CHECK(conflict.error().code == ErrorCode::conflict);
        CHECK(std::filesystem::exists(ObjectStore(library->root()).object_path(sha256(stale_bytes), "mid")));
        CHECK(library->revisions(*asset)->size() == 2);

        // Restore creates a new head referencing the old object; history stays.
        const auto restored = library->restore_revision(*asset, *first_head, *second);
        REQUIRE(restored.has_value());
        const auto revisions = library->revisions(*asset).value();
        REQUIRE(revisions.size() == 3);
        CHECK(revisions[2].origin == RevisionOrigin::restore);
        CHECK(revisions[2].object_sha256 == revisions[0].object_sha256);
        CHECK(revisions[2].parent_id == *second);
        CHECK(library->asset(*asset)->value().current_revision_id == *restored);
        CHECK_FALSE(library->restore_revision(*asset, *first_head, *second).has_value()); // Stale again.
        REQUIRE(library->set_revision_pinned(revisions[0].id, true).has_value());
        CHECK(library->revisions(*asset)->front().pinned);

        const auto copy = library->duplicate_asset(*asset);
        REQUIRE(copy.has_value());
        CHECK(*copy != *asset);
        CHECK(library->revisions(*copy)->front().object_sha256 == revisions[2].object_sha256);
    }

    TEST_CASE("trash, restore, purge with counts and grace-period object collection") {
        test::TemporaryDirectory temporary;
        auto library = make_library(temporary.path() / "library");
        const auto in = temporary.path() / "in";
        write_bytes(in / "keep.mid", phrase_smf(Phrase{}));
        write_bytes(in / "drop.mid", phrase_smf(Phrase{}.set_pitches({33, 35})));
        auto report = library->import_paths({in});
        const auto keep = only_asset(*report, "keep.mid");
        const auto drop = only_asset(*report, "drop.mid");
        REQUIRE(library->add_tag(drop, "doomed").has_value());
        const auto collection = library->create_collection("Set").value();
        REQUIRE(library->add_to_collection(collection, {drop}).has_value());

        REQUIRE(library->trash({drop}).has_value());
        SearchRequest all;
        CHECK(all_ids(*library, all) == std::vector<std::int64_t>{keep});
        CHECK(library->list_trash().value() == std::vector<std::int64_t>{drop});
        CHECK_FALSE(library->purge({keep}).has_value()); // Active assets are refused.
        REQUIRE(library->restore_from_trash({drop}).has_value());
        CHECK(library->asset_tags(drop).value() == std::vector<std::string>{"doomed"});
        CHECK(library->collection_members(collection).value() == std::vector<std::int64_t>{drop});

        REQUIRE(library->trash({drop}).has_value());
        const auto object = library->revisions(drop)->front().object_sha256;
        const auto preview = library->purge_preview({drop});
        REQUIRE(preview.has_value());
        CHECK(preview->assets == 1);
        CHECK(preview->revisions == 1);
        const auto purged = library->purge({drop});
        REQUIRE(purged.has_value());
        CHECK(purged->assets == 1);
        CHECK_FALSE(library->asset(drop)->has_value());
        CHECK(library->collection_members(collection)->empty());

        // Source location still references the dropped object until the source row goes away
        // with the asset (cascade); then GC waits for the grace period.
        const auto path = ObjectStore(library->root()).object_path(object, "mid");
        auto first = library->collect_garbage(gc_at(1'000, 30LL * 24 * 60 * 60 * 1000));
        REQUIRE(first.has_value());
        CHECK(first->unreferenced == 1);
        CHECK(first->deleted == 0);
        CHECK(first->waiting == 1);
        CHECK(std::filesystem::exists(path));
        {
            Library::GarbageCollectionPause pause(*library);
            CHECK(library->collect_garbage(gc_at(1'000'000'000'000, 30LL * 24 * 60 * 60 * 1000))->suspended);
        }
        auto second = library->collect_garbage(gc_at(1'000'000'000'000, 10));
        REQUIRE(second.has_value());
        CHECK(second->deleted == 1);
        CHECK_FALSE(std::filesystem::exists(path));
        CHECK(verify_library(library->root())->ok());
    }
}

TEST_SUITE("backup") {
    TEST_CASE("backup, restore and verify; a corrupted object is detected and blocks a verified backup") {
        test::TemporaryDirectory temporary;
        auto library = make_library(temporary.path() / "library");
        const auto in = temporary.path() / "in";
        write_bytes(in / "one.mid", phrase_smf(Phrase{}.set_name("Glow")));
        write_bytes(in / "two.mid", phrase_smf(Phrase{}.set_pitches({48, 50, 52})));
        auto report = library->import_paths({in});
        REQUIRE(report.has_value());
        REQUIRE(library->add_tag(only_asset(*report, "one.mid"), "keeper").has_value());

        const auto backup = temporary.path() / "backup";
        auto made = create_backup(*library, backup);
        REQUIRE(made.has_value());
        CHECK(made->complete);
        CHECK(made->object_count == 2);
        CHECK_FALSE(std::filesystem::exists(backup / incomplete_marker_name));
        CHECK(std::filesystem::exists(backup / manifest_name));
        CHECK_FALSE(create_backup(*library, backup).has_value()); // Never into a non-empty directory.

        const auto restored = temporary.path() / "restored";
        auto verified = restore_backup(backup, restored);
        REQUIRE(verified.has_value());
        CHECK(verified->ok());
        CHECK(verified->objects_checked == 2);
        {
            auto reopened = Library::open(restored);
            REQUIRE(reopened.has_value());
            SearchRequest keeper;
            keeper.query_text = "tag:keeper glow";
            CHECK(all_ids(**reopened, keeper).size() == 1);
        }
        CHECK_FALSE(restore_backup(backup, restored).has_value()); // Target must be new.

        // Tampering with a backup file is caught before anything is restored.
        const auto tampered = temporary.path() / "tampered";
        std::filesystem::copy(backup, tampered, std::filesystem::copy_options::recursive);
        write_bytes(tampered / "library.json", Bytes{'{', '}'});
        CHECK_FALSE(restore_backup(tampered, temporary.path() / "restored2").has_value());
        CHECK_FALSE(std::filesystem::exists(temporary.path() / "restored2"));

        // Corrupt one live object (same size, different bytes) and delete another.
        const auto revisions_one = library->revisions(only_asset(*report, "one.mid")).value();
        const auto revisions_two = library->revisions(only_asset(*report, "two.mid")).value();
        const auto store = ObjectStore(library->root());
        auto corrupted = read_file(store.object_path(revisions_one[0].object_sha256, "mid"), 1 << 20).value();
        corrupted.back() ^= 0xFF;
        write_bytes(store.object_path(revisions_one[0].object_sha256, "mid"), corrupted);
        auto damaged = verify_library(library->root());
        REQUIRE(damaged.has_value());
        CHECK_FALSE(damaged->ok());
        CHECK(damaged->corrupt_objects == std::vector<std::string>{to_hex(revisions_one[0].object_sha256)});
        CHECK(damaged->integrity_messages.empty());
        CHECK(damaged->foreign_key_violations == 0);
        CHECK(damaged->to_json().find("corrupt_objects") != std::string::npos);
        std::filesystem::remove(store.object_path(revisions_two[0].object_sha256, "mid"));
        CHECK(verify_library(library->root())->missing_objects.size() == 1);

        const auto broken_backup = temporary.path() / "broken-backup";
        auto failed = create_backup(*library, broken_backup);
        REQUIRE_FALSE(failed.has_value());
        CHECK(std::filesystem::exists(broken_backup / incomplete_marker_name));
        CHECK_FALSE(restore_backup(broken_backup, temporary.path() / "never").has_value());

        // Repair: safety backup first (catalog only, since objects are damaged), then FTS rebuild.
        auto repaired = repair_rebuild_fts(*library);
        REQUIRE(repaired.has_value());
        CHECK_FALSE(repaired->full_backup);
        CHECK(std::filesystem::exists(repaired->safety_backup / "library.sqlite"));
        CHECK(repaired->fts_rows == 2);
        SearchRequest glow;
        glow.query_text = "glow";
        CHECK(all_ids(*library, glow).size() == 1);
    }
}

TEST_SUITE("plans") {
    TEST_CASE("hot queries on a populated catalog avoid full scans and temporary B-tree sorts") {
        test::TemporaryDirectory temporary;
        auto library = make_library(temporary.path() / "library");
        REQUIRE(library->populate_synthetic(3000, 11).value() == 3000);
        REQUIRE(library->enqueue_job("analyze", "analyze-1", "{}").has_value());
        auto queries = hot_queries(*library);
        REQUIRE(queries.has_value());
        CHECK(queries->size() == 13); // incl. selective filters, the motif index and the analysis batch
        for (const auto& query : *queries) {
            auto plan = library->explain_query_plan(query.statement);
            REQUIRE(plan.has_value());
            std::string joined;
            for (const auto& line : *plan)
                joined += line + "\n";
            CAPTURE(query.name);
            CAPTURE(joined);
            CHECK_FALSE(plan->empty());
            CHECK(plan_problems(*plan).empty());
        }
        CHECK_FALSE(plan_problems({"SCAN asset"}).empty());
        CHECK_FALSE(plan_problems({"USE TEMP B-TREE FOR ORDER BY"}).empty());
        CHECK(plan_problems({"SCAN asset_fts VIRTUAL TABLE INDEX 0:M1"}).empty());

        auto leased = library->lease_job();
        REQUIRE(leased.has_value());
        REQUIRE(leased->has_value());
        CHECK((*leased)->attempts == 1);
        CHECK_FALSE(library->lease_job()->has_value());
        REQUIRE(library->finish_job((*leased)->id, true).has_value());
        CHECK(library->enqueue_job("analyze", "analyze-1", "{}").value() == (*leased)->id); // Idempotent.
    }

    TEST_CASE("the bench helper measures real timings on a synthetic catalog") {
        test::TemporaryDirectory temporary;
        auto options = bench_options(500, 3);
        options.motif_rows = 250; // with a planted motif every 100 streams (code-library-13)
        auto report = run_bench(temporary.path() / "bench", options);
        INFO((report ? report->to_json() : report.error().describe()));
        REQUIRE(report.has_value());
        CHECK(report->rows == 500);
        // 7 SQL timings, the index build, 5 index pages and 3 melody searches.
        CHECK(report->timings.size() == 16);
        CHECK(report->plan_problems.empty());
        for (const auto& timing : report->timings) {
            CAPTURE(timing.name);
            CHECK(timing.samples == (timing.name == "browse_index_build" ? 1u : 3u));
            CHECK(timing.p50_ms <= timing.max_ms);
        }
        for (const auto* name : {"page_first_title", "index_page_first_title", "index_select_has_sysex", "motif_exact_8",
                                 "motif_exact_32", "motif_similar_16"})
            CHECK(report->to_json().find(name) != std::string::npos);
    }
}

TEST_SUITE("analysis") {
    TEST_CASE("a restored revision keeps the user's manual key (code-library-3)") {
        test::TemporaryDirectory temporary;
        auto library = make_library(temporary.path() / "library");
        const auto file = temporary.path() / "in" / "song.mid";
        write_bytes(file, phrase_smf(Phrase{}));
        const auto asset = *library->import_paths({file})->items[0].asset_id;
        const auto first = library->asset(asset)->value().current_revision_id;
        REQUIRE(library->commit_revision(CommitRequest{asset, first, RevisionOrigin::edit, "edit"},
                                         phrase_smf(Phrase{}.set_pitches({62, 65, 69, 72})), MediaType::smf)
                    .has_value());
        REQUIRE(library->set_key_override(asset, 0, 9, "minor").has_value());
        const auto head = library->asset(asset)->value().current_revision_id;
        REQUIRE(library->restore_revision(asset, *first, head).has_value());
        SearchRequest request;
        request.query_text = "key:\"A minor\" key.origin:manual";
        auto page = library->search(request);
        REQUIRE(page.has_value());
        REQUIRE(page->rows.size() == 1);
        CHECK(page->rows[0].id == asset);
    }

    TEST_CASE("a cancelled or unreadable analysis is retried later, never marked failed (code-library-5)") {
        test::TemporaryDirectory temporary;
        auto library = make_library(temporary.path() / "library");
        write_bytes(temporary.path() / "in" / "a.mid", phrase_smf(Phrase{}));
        REQUIRE(library->import_paths({temporary.path() / "in"}).has_value());
        AnalyzeOptions cancelling;
        cancelling.parser = [](std::span<const std::uint8_t>, std::string_view, const midi::ParseLimits&,
                               std::stop_token) -> Result<midi::ImportResult> {
            return make_error(ErrorCode::cancelled, "the import was cancelled");
        };
        auto cancelled = library->analyze(cancelling);
        REQUIRE(cancelled.has_value());
        CHECK(cancelled->cancelled);
        CHECK(cancelled->revisions_failed == 0);
        // Objects briefly unreadable: recorded as retry-later.
        const auto objects = temporary.path() / "library" / "objects";
        std::filesystem::rename(objects, temporary.path() / "library" / "objects-away");
        auto unreadable = library->analyze();
        REQUIRE(unreadable.has_value());
        std::filesystem::rename(temporary.path() / "library" / "objects-away", objects);
        auto retried = library->analyze();
        REQUIRE(retried.has_value());
        CHECK(retried->revisions_analyzed == 1);
        // A genuinely broken file is recorded failed once and never re-parsed by later runs
        // (code-library-1 leftover).
        AnalyzeOptions breaking;
        breaking.parser = [](std::span<const std::uint8_t>, std::string_view, const midi::ParseLimits&,
                             std::stop_token) -> Result<midi::ImportResult> {
            return make_error(ErrorCode::invalid_data, "broken");
        };
        write_bytes(temporary.path() / "in2" / "b.mid", phrase_smf(Phrase{}.set_pitches({41, 43})));
        REQUIRE(library->import_paths({temporary.path() / "in2"}).has_value());
        CHECK(library->analyze(breaking)->revisions_failed == 1);
        int parses = 0;
        AnalyzeOptions counting;
        counting.parser = [&parses](std::span<const std::uint8_t>, std::string_view, const midi::ParseLimits&,
                                    std::stop_token) -> Result<midi::ImportResult> {
            ++parses;
            return make_error(ErrorCode::invalid_data, "broken");
        };
        auto again = library->analyze(counting);
        REQUIRE(again.has_value());
        CHECK(parses == 0);
        CHECK(again->revisions_failed == 0);
    }

    TEST_CASE("the derived index has a budget, and a new head drops the old head's streams (code-library-7)") {
        test::TemporaryDirectory temporary;
        const auto root = temporary.path() / "library";
        auto library = make_library(root);
        const auto file = temporary.path() / "in" / "song.mid";
        write_bytes(file, phrase_smf(Phrase{}.set_pitches({60, 62, 64, 65, 67, 69, 71, 72})));
        const auto asset = *library->import_paths({file})->items[0].asset_id;
        AnalyzeOptions tiny;
        tiny.budget_bytes = 1;
        auto stopped = library->analyze(tiny);
        REQUIRE(stopped.has_value());
        CHECK(stopped->budget_reached);
        CHECK(stopped->revisions_analyzed == 0);
        REQUIRE(library->analyze()->revisions_analyzed == 1); // the default budget resumes
        const auto count_streams = [&] {
            SQLite::Database database((root / "library.sqlite").string(), SQLite::OPEN_READONLY);
            SQLite::Statement query(database, "SELECT count(*) FROM motif_stream");
            query.executeStep();
            return query.getColumn(0).getInt64();
        };
        CHECK(count_streams() == 1);
        const auto head = library->asset(asset)->value().current_revision_id;
        REQUIRE(library->commit_revision(CommitRequest{asset, head, RevisionOrigin::edit, "edit"},
                                         phrase_smf(Phrase{}.set_pitches({50, 52, 53})), MediaType::smf)
                    .has_value());
        CHECK(count_streams() == 0); // the superseded head's derived rows are gone
    }

    TEST_CASE("unindexed items and unreadable streams make a melody result partial instead of silent or failing") {
        test::TemporaryDirectory temporary;
        const auto root = temporary.path() / "library";
        auto library = make_library(root);
        const auto in = temporary.path() / "in";
        write_bytes(in / "theme.mid", phrase_smf(Phrase{}.set_pitches({60, 64, 67, 72, 71, 67, 64, 62})));
        write_bytes(in / "other.mid", phrase_smf(Phrase{}.set_pitches({62, 66, 69, 74, 73, 69, 66, 64}))); // transposed
        REQUIRE(library->import_paths({in}).has_value());
        search::ExactQuery query;
        query.mode = search::MatchMode::transposed;
        for (const int pitch : {64, 67, 72, 71, 67})
            query.notes.push_back(search::QueryNote{static_cast<Tick>(query.notes.size()) * 480, 240,
                                                    static_cast<std::uint8_t>(pitch), 100});
        auto before = library->motif_search(query);
        REQUIRE(before.has_value());
        CHECK(before->partial); // nothing is indexed yet: the result says so (code-library-4)
        CHECK(before->unindexed_heads == 2);
        REQUIRE(library->analyze().has_value());
        auto indexed = library->motif_search(query);
        REQUIRE(indexed.has_value());
        CHECK_FALSE(indexed->partial);
        CHECK(indexed->occurrences.size() == 2);
        {
            // Damage one stored stream (bit rot, a restored damaged backup).
            SQLite::Database database((root / "library.sqlite").string(), SQLite::OPEN_READWRITE);
            database.exec("UPDATE motif_stream SET payload = x'00010203' WHERE id = (SELECT min(id) FROM motif_stream)");
        }
        auto damaged = library->motif_search(query);
        REQUIRE(damaged.has_value()); // not an error for every search (code-library-9)
        CHECK(damaged->partial);
        CHECK(damaged->skipped_streams == 1);
        CHECK(damaged->occurrences.size() == 1);
        REQUIRE(library->rebuild_derived().has_value()); // repair: rebuild the derived index
        REQUIRE(library->analyze()->revisions_analyzed == 2);
        CHECK(library->motif_search(query)->occurrences.size() == 2);
    }

    TEST_CASE("garbage collection removes object files the catalog does not know (code-library-12)") {
        test::TemporaryDirectory temporary;
        const auto root = temporary.path() / "library";
        auto library = make_library(root);
        const std::string hex(64, 'a');
        const auto orphan = root / "objects" / "aa" / (hex + ".mid");
        write_bytes(orphan, Bytes{'M', 'T', 'h', 'd'});
        GarbageCollectionOptions options;
        options.now_ms = 0; // tests: every unknown file counts as old
        auto report = library->collect_garbage(options);
        REQUIRE(report.has_value());
        CHECK(report->orphan_files == 1);
        CHECK_FALSE(std::filesystem::exists(orphan));
    }
}

TEST_SUITE("catalog") {
    TEST_CASE("searches are served by the browse index once built and stay current from the outbox (code-library-11)") {
        test::TemporaryDirectory temporary;
        auto library = make_library(temporary.path() / "library");
        const auto in = temporary.path() / "in";
        write_bytes(in / "bass line.mid", phrase_smf(Phrase{}.set_tempo_us(400000).set_program(33)));
        write_bytes(in / "rising lead.mid", phrase_smf(Phrase{}.set_name("Rising Lead")));
        auto report = library->import_paths({in});
        REQUIRE(report.has_value());
        const auto bass = only_asset(*report, "bass line.mid");
        const auto lead = only_asset(*report, "rising lead.mid");
        REQUIRE(library->populate_synthetic(400, 5).has_value());

        // Pages through a request; reports whether every page came from the index.
        const auto pages = [&](SearchRequest request, bool use_index, bool& all_from_index) {
            request.use_browse_index = use_index;
            request.page_size = 37; // several pages, last one partial
            std::vector<std::int64_t> ids;
            all_from_index = true;
            for (int guard = 0; guard < 1000; ++guard) {
                auto page = library->search(request);
                REQUIRE(page.has_value());
                all_from_index = all_from_index && page->from_browse_index;
                for (const auto& row : page->rows)
                    ids.push_back(row.id);
                if (!page->next)
                    break;
                request.after = page->next;
            }
            return ids;
        };
        const std::vector<std::string> queries{"", "tag:bass", "tag:picked", "bpm:120..140", "lead", "favorite:true rating:>=2",
                                               "NOT drums:true notes:>=100", "key:C"};
        const std::vector<search::SortOrder> orders{{search::SortField::title, false},
                                                     {search::SortField::title, true},
                                                     {search::SortField::modified, true},
                                                     {search::SortField::created, false}};
        // Pages in exactly the SQL order, for every sort.
        const auto check_parity = [&](bool expect_index) {
            for (const auto& text : queries)
                for (const auto& order : orders) {
                    CAPTURE(text);
                    CAPTURE(static_cast<int>(order.field));
                    CAPTURE(order.descending);
                    SearchRequest request;
                    request.query_text = text;
                    request.order = order;
                    bool sql_index = true;
                    bool memory_index = false;
                    const auto sql = pages(request, false, sql_index);
                    const auto memory = pages(request, true, memory_index);
                    CHECK_FALSE(sql_index);
                    CHECK(memory_index == expect_index);
                    CHECK(memory == sql);
                }
        };

        CHECK_FALSE(library->browse_index_status().ready);
        check_parity(false); // still loading: SQL
        auto built = library->build_browse_index();
        REQUIRE(built.has_value());
        CHECK(built->ready);
        CHECK(built->rows == 402);
        CHECK(built->memory_bytes > 0);
        check_parity(true);

        // Writes after the build reach the index through the outbox before the next page.
        REQUIRE(library->add_tag(lead, "picked").has_value());
        REQUIRE(library->set_rating(bass, 5).has_value());
        REQUIRE(library->set_favorite(bass, true).has_value());
        REQUIRE(library->set_title(lead, "aaa first").has_value());
        REQUIRE(library->trash({bass}).has_value());
        check_parity(true);
        SearchRequest tagged;
        tagged.query_text = "tag:picked";
        CHECK(all_ids(*library, tagged) == std::vector<std::int64_t>{lead});
        REQUIRE(library->restore_from_trash({bass}).has_value());
        REQUIRE(library->trash({lead}).has_value());
        REQUIRE(library->purge({lead}).has_value());
        write_bytes(temporary.path() / "more" / "new.mid", phrase_smf(Phrase{}.set_pitches({40, 41})));
        REQUIRE(library->import_paths({temporary.path() / "more"}).has_value());
        check_parity(true);
        CHECK(library->browse_index_status().rows == 402); // one purged, one imported

        // Relevance order is always SQL.
        SearchRequest relevance;
        relevance.query_text = "lead";
        relevance.order.field = search::SortField::relevance;
        auto ranked = library->search(relevance);
        REQUIRE(ranked.has_value());
        CHECK_FALSE(ranked->from_browse_index);

        // Over its budget the index is not kept: SQL paging serves the library.
        auto small = library->build_browse_index({}, 4096);
        REQUIRE(small.has_value());
        CHECK(small->over_budget);
        CHECK_FALSE(small->ready);
        CHECK(library->browse_index_status().over_budget);
        check_parity(false);

        // A cancelled build leaves the previous state untouched.
        std::stop_source stop;
        stop.request_stop();
        auto cancelled = library->build_browse_index(stop.get_token());
        REQUIRE_FALSE(cancelled.has_value());
        CHECK(cancelled.error().code == ErrorCode::cancelled);
    }
}

TEST_SUITE("analysis") {
    TEST_CASE("the M10K fixture plants a known motif that exact and similar search find (code-library-13)") {
        test::TemporaryDirectory temporary;
        auto library = make_library(temporary.path() / "library");
        REQUIRE(library->populate_synthetic(300, 9).value() == 300);
        REQUIRE(library->populate_synthetic_motifs(250, 9).value() == 250);
        const auto planted = synthetic_planted_motif();
        REQUIRE(planted.size() == 32);
        search::ExactQuery exact;
        exact.mode = search::MatchMode::transposed;
        exact.notes = planted;
        auto found = library->motif_search(exact);
        REQUIRE(found.has_value());
        CHECK(found->occurrences.size() == 3); // streams 0, 100 and 200
        CHECK(found->partial);                  // 50 heads have no streams: said, not hidden
        CHECK(found->unindexed_heads == 50);
        search::ApproximateQuery similar;
        similar.notes.assign(planted.begin() + 4, planted.begin() + 20);
        auto near = library->similar_search(similar);
        REQUIRE(near.has_value());
        CHECK(near->matches.size() >= 3);
        // A cancelled generation ends at once instead of finishing the search (code-library-8).
        std::stop_source cancel;
        cancel.request_stop();
        // It comes back as a partial result that says so, never as a complete empty one.
        auto stopped_exact = library->motif_search(exact, cancel.get_token());
        REQUIRE(stopped_exact.has_value());
        CHECK(stopped_exact->partial);
        CHECK(stopped_exact->partial_reason.find("cancelled") != std::string::npos);
        CHECK(stopped_exact->occurrences.empty());
        auto stopped_similar = library->similar_search(similar, cancel.get_token());
        REQUIRE(stopped_similar.has_value());
        CHECK(stopped_similar->partial);
        CHECK(stopped_similar->partial_reason.find("cancelled") != std::string::npos);
        CHECK(stopped_similar->matches.empty());
        // The remaining heads get streams on a second call; nothing is written twice.
        CHECK(library->populate_synthetic_motifs(1000, 9).value() == 50);
        CHECK(library->populate_synthetic_motifs(10, 9).value() == 0);
    }
}
