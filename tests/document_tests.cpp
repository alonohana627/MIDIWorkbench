// Tests of the document module: native .mwb format, command history and working sessions, and
// crash recovery (specification sections 21, 22.5, 23.1, 23.3 and 23.4).

#include "test_support.hpp"

#include "mwb/core/change_set.hpp"
#include "mwb/core/hashing.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/core/serialization.hpp"
#include "mwb/core/validation.hpp"
#include "mwb/document/command_codec.hpp"
#include "mwb/document/history.hpp"
#include "mwb/document/migration.hpp"
#include "mwb/document/native_format.hpp"
#include "mwb/document/recovery.hpp"
#include "mwb/document/session.hpp"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace mwb;
using namespace mwb::document;

namespace {

/// Track identity of the first track of the first sequence.
TrackId first_track(const DocumentData& document) { return document.sequences.at(0).tracks.at(0).id; }

/// Large SysEx payload (1 MiB) terminated by F7, as an SMF F0 packet stores it.
Event large_sysex(Tick tick) {
    Event event;
    event.id = EventId::generate();
    event.tick = tick;
    event.category = EventCategory::sysex_f0;
    event.code = 0xF0;
    std::vector<std::uint8_t> bytes(1024 * 1024);
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::uint8_t>(index % 128);
    bytes.back() = 0xF7;
    event.payload = Payload(bytes);
    return event;
}

/// A preserved malformed channel message (three data bytes on a note-on), flagged malformed.
Event malformed_event(Tick tick) {
    Event event;
    event.id = EventId::generate();
    event.tick = tick;
    event.category = EventCategory::channel;
    event.code = 0x93;
    event.flags = event_flag::malformed;
    event.payload = Payload{61, 99, 7};
    return event;
}

/// Document exercising every table: notes (paired, zero-length, unterminated), controllers, a
/// large SysEx, a malformed preserved event, an empty meta payload, Hebrew/emoji text, a second
/// track, a second SMPTE sequence, regions, scale regions, recipes, takes with chunks, retained
/// source bytes and whitelisted settings.
DocumentData rich_document(const std::string& title, bool smpte_first) {
    DocumentData document = make_blank_document(title);
    if (smpte_first)
        document.sequences[0].division = Division::make_smpte(SmpteFormat::fps29_97_drop, 80);
    const TrackId track = first_track(document);
    {
        ChangeBuilder builder(document, "content");
        builder.add_note(track, 0, 480, 0, 60, 100);
        builder.add_note(track, 480, 960, 0, 64, 90, 40);
        builder.add_note(track, 960, 960, 1, 67, 80);
        builder.insert(track, events::note_on(1500, 2, 72, 70)); // unterminated
        builder.insert(track, events::note_off(1600, 5, 50));    // orphan end
        builder.insert(track, events::control_change(100, 0, 7, 100));
        builder.insert(track, events::pitch_bend(200, 0, 9000));
        builder.insert(track, events::program_change(0, 0, 5));
        builder.insert(track, events::key_signature(0, -3, true));
        builder.insert(track, events::text_meta(0, meta_type::track_name, "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xF0\x9F\x8E\xB9"));
        builder.insert(track, events::text_meta(10, meta_type::marker, ""));
        builder.insert(track, large_sysex(300));
        builder.insert(track, malformed_event(400));
        const auto changes = builder.finish();
        REQUIRE(apply_change_set(document, changes));
    }
    auto& sequence = document.sequences[0];
    Track second;
    second.id = TrackId::generate();
    second.name = "Drums \xF0\x9F\xA5\x81";
    second.is_drum = true;
    second.muted = true;
    second.color_rgba = 0xFF0000FF;
    second.routing.output = OutputKind::external_midi;
    second.routing.endpoint_id = "loopMIDI Port";
    second.routing.channel_override = 9;
    second.routing.transpose = -12;
    second.routing.send_sysex = true;
    second.instrument.backend = "sf2";
    second.instrument.bank_sha256 = std::string(64, 'a');
    second.instrument.bank = 128;
    second.instrument.program = 25;
    Event drum_on = events::note_on(240, 9, 36, 110);
    drum_on.order_key = order_key_spacing;
    Event drum_off = events::note_off(1200, 9, 36);
    drum_off.order_key = order_key_spacing;
    second.events = {drum_on, drum_off};
    second.end_tick = 2000;
    sequence.tracks.push_back(second);

    Region region;
    region.id = RegionId::generate();
    region.name = "Verse \xE2\x99\xAA";
    region.start_tick = 0;
    region.end_tick = 960;
    region.tracks = {track, second.id};
    sequence.regions.push_back(region);
    sequence.scale_regions.push_back(ScaleRegion{RegionId::generate(), 0, 960, 2, 0x0AB5, "D major"});
    sequence.scale_regions.push_back(ScaleRegion{RegionId::generate(), 960, 1920, 9, 0x05AD, "A minor"});
    sequence.root_pc = 2;
    sequence.scale_mask = 0x0AB5;
    sequence.settings.highlight_scale = true;
    sequence.settings.snap = Rational{1, 8};
    sequence.settings.input_policy = InputScalePolicy::nearest_member;
    Recipe recipe;
    recipe.id = RecipeId::generate();
    recipe.tool_id = "transpose";
    recipe.algorithm_version = 2;
    recipe.source_semantic_hash = semantic_hash(sequence);
    recipe.parameters_json = R"({"version":1,"semitones":2})";
    recipe.targets_json = "[]";
    sequence.recipes.push_back(recipe);

    Sequence smpte;
    smpte.id = SequenceId::generate();
    smpte.name = "SMPTE sequence";
    smpte.division = Division::make_smpte(SmpteFormat::fps25, 40);
    smpte.end_tick = 4000;
    Track smpte_track;
    smpte_track.id = TrackId::generate();
    smpte_track.name = "Timecode";
    smpte_track.end_tick = 4000;
    Event tempo = events::tempo(0, 600000);
    tempo.order_key = order_key_spacing;
    Event on = events::note_on(100, 0, 48, 64);
    on.order_key = order_key_spacing;
    Event off = events::note_off(3000, 0, 48);
    off.order_key = order_key_spacing;
    smpte_track.events = {tempo, on, off};
    smpte.tracks.push_back(smpte_track);
    document.sequences.push_back(smpte);

    RecordingTake take;
    take.id = TakeId::generate();
    take.track = track;
    take.name = "Take 1";
    take.created_ms = 1700000000000;
    take.start_tick = 0;
    take.end_tick = 960;
    take.status = TakeStatus::accepted;
    take.capture_json = R"({"version":1,"input":"keyboard","lost_events":0})";
    take.chunks = {TakeChunk{0, 0, {0x90, 60, 100}}, TakeChunk{1, 5000000, {}}};
    document.takes.push_back(take);

    const std::vector<std::uint8_t> original{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, 1, 224};
    SourceBlob source;
    source.id = Uuid::generate();
    source.bytes = std::make_shared<const std::vector<std::uint8_t>>(original);
    source.sha256 = sha256(original);
    source.original_name = "\xD7\xA9\xD7\x99\xD7\xA8.mid";
    source.media_type = "smf";
    source.import_report_json = R"({"version":1,"warnings":[]})";
    document.sources.push_back(source);

    document.settings["editor.view"] = R"({"zoom":2,"follow":true})";
    document.settings["export.last_profile"] = R"("compatible")";
    document.metadata_json = R"({"composer":"\u05D0"})";
    document.generation = 17;
    return document;
}

/// Full equality including retained source bytes (SourceBlob equality skips the bytes).
void check_same_document(const DocumentData& loaded, const DocumentData& original) {
    CHECK(semantic_hash(loaded) == semantic_hash(original));
    CHECK(loaded == original);
    REQUIRE(loaded.sources.size() == original.sources.size());
    for (std::size_t index = 0; index < loaded.sources.size(); ++index)
        CHECK(*loaded.sources[index].bytes == *original.sources[index].bytes);
}

/// Saves `document`, then lets `tamper` modify the file through a writable connection.
template <class Tamper>
std::filesystem::path tampered_file(const std::filesystem::path& directory, const DocumentData& document,
                                    Tamper&& tamper) {
    const auto path = directory / "tampered.mwb";
    REQUIRE(save_native(document, path));
    {
        SQLite::Database database(path.string(), SQLite::OPEN_READWRITE);
        tamper(database);
    }
    return path;
}

/// Adds one note through a session; returns the new generation.
Generation add_note(WorkingSession& session, Tick start, std::uint8_t pitch) {
    ChangeBuilder builder(session.document(), "Add note");
    builder.add_note(first_track(session.document()), start, start + 240, 0, pitch, 100);
    auto generation = session.apply(builder.finish());
    REQUIRE(generation);
    return *generation;
}

/// Directory entries whose name contains `fragment` (temporary-file leak checks).
std::size_t files_containing(const std::filesystem::path& directory, const std::string& fragment) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().filename().u8string().find(std::u8string(fragment.begin(), fragment.end())) !=
            std::u8string::npos)
            ++count;
    return count;
}

} // namespace

// =================================================================================================

TEST_SUITE("native") {

TEST_CASE("rich PPQ document round-trips through a Hebrew/emoji path with identical semantic hash") {
    test::TemporaryDirectory directory;
    const auto folder = directory.path() / std::filesystem::path(u8"פרויקט 🎹");
    std::filesystem::create_directories(folder);
    const auto path = folder / std::filesystem::path(u8"שיר ראשון 🎵.mwb");

    const DocumentData original = rich_document("\xD7\xA9\xD7\x99\xD7\xA8 \xF0\x9F\x8E\xB6 title", false);
    REQUIRE_FALSE(has_errors(validate_document(original)));
    const auto saved = save_native(original, path);
    REQUIRE_MESSAGE(saved, (saved ? "" : saved.error().describe()));
    CHECK(std::filesystem::exists(path));
    CHECK(files_containing(folder, "mwb-tmp") == 0);
    CHECK(files_containing(folder, "-journal") == 0);

    const auto loaded = load_native(path);
    REQUIRE_MESSAGE(loaded, (loaded ? "" : loaded.error().describe()));
    check_same_document(*loaded, original);
    // The malformed event keeps its flag and bytes; the large SysEx is intact.
    const auto& events = loaded->sequences[0].tracks[0].events;
    CHECK(std::any_of(events.begin(), events.end(), [](const Event& event) {
        return (event.flags & event_flag::malformed) != 0 && event.payload.size() == 3;
    }));
    CHECK(std::any_of(events.begin(), events.end(),
                      [](const Event& event) { return event.category == EventCategory::sysex_f0 && event.payload.size() == 1024 * 1024; }));
}

TEST_CASE("SMPTE document round-trips through bytes") {
    const DocumentData original = rich_document("SMPTE", true);
    REQUIRE(original.sequences[0].division.kind == Division::Kind::smpte);
    const auto bytes = native_to_bytes(original);
    REQUIRE_MESSAGE(bytes, (bytes ? "" : bytes.error().describe()));
    const auto loaded = native_from_bytes(*bytes);
    REQUIRE_MESSAGE(loaded, (loaded ? "" : loaded.error().describe()));
    check_same_document(*loaded, original);
    CHECK(loaded->sequences[0].division == Division::make_smpte(SmpteFormat::fps29_97_drop, 80));
    CHECK(loaded->sequences[1].division == Division::make_smpte(SmpteFormat::fps25, 40));
}

TEST_CASE("blank document round-trips and the file carries the native identity") {
    test::TemporaryDirectory directory;
    const DocumentData original = make_blank_document("Blank");
    const auto path = directory.path() / "blank.mwb";
    REQUIRE(save_native(original, path));
    const auto loaded = load_native(path);
    REQUIRE(loaded);
    check_same_document(*loaded, original);

    SQLite::Database database(path.string(), SQLite::OPEN_READONLY);
    CHECK(database.execAndGet("PRAGMA application_id").getInt() == native_application_id);
    CHECK(database.execAndGet("PRAGMA user_version").getInt() == native_schema_version);
    CHECK(database.execAndGet("PRAGMA journal_mode").getString() == "delete");
}

TEST_CASE("note_link rows are the FIFO pairing projection") {
    test::TemporaryDirectory directory;
    const DocumentData original = rich_document("Links", false);
    const auto path = directory.path() / "links.mwb";
    REQUIRE(save_native(original, path));
    std::size_t expected = 0;
    for (const auto& sequence : original.sequences)
        expected += pair_notes(sequence).notes.size();
    SQLite::Database database(path.string(), SQLite::OPEN_READONLY);
    CHECK(static_cast<std::size_t>(database.execAndGet("SELECT count(*) FROM note_link").getInt64()) == expected);
    CHECK(database.execAndGet("SELECT count(*) FROM note_link WHERE pairing_policy <> 'fifo-v1' OR id <> on_event_id")
              .getInt64() == 0);
    CHECK(database.execAndGet("SELECT count(*) FROM note_link WHERE off_event_id IS NULL").getInt64() >= 2);
}

TEST_CASE("inconsistent note links are rejected") {
    test::TemporaryDirectory directory;
    const DocumentData original = rich_document("Links", false);
    SUBCASE("missing link") {
        const auto path = tampered_file(directory.path(), original, [](SQLite::Database& database) {
            database.exec("DELETE FROM note_link WHERE rowid = (SELECT min(rowid) FROM note_link)");
        });
        const auto loaded = load_native(path);
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().code == ErrorCode::invalid_data);
    }
    SUBCASE("wrong pairing") {
        const auto path = tampered_file(directory.path(), original, [](SQLite::Database& database) {
            database.exec("UPDATE note_link SET off_event_id = NULL WHERE off_event_id IS NOT NULL");
        });
        CHECK_FALSE(load_native(path));
    }
    SUBCASE("unknown policy") {
        const auto path = tampered_file(directory.path(), original, [](SQLite::Database& database) {
            database.exec("UPDATE note_link SET pairing_policy = 'lifo'");
        });
        CHECK_FALSE(load_native(path));
    }
}

TEST_CASE("files with unexpected schema objects are rejected") {
    test::TemporaryDirectory directory;
    const DocumentData original = make_blank_document("Schema");
    SUBCASE("trigger") {
        const auto path = tampered_file(directory.path(), original, [](SQLite::Database& database) {
            database.exec("CREATE TRIGGER evil AFTER INSERT ON midi_event BEGIN DELETE FROM track; END");
        });
        const auto loaded = load_native(path);
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().code == ErrorCode::invalid_data);
        CHECK(loaded.error().message.find("trigger") != std::string::npos);
    }
    SUBCASE("view") {
        const auto path = tampered_file(directory.path(), original, [](SQLite::Database& database) {
            database.exec("CREATE VIEW peek AS SELECT * FROM project");
        });
        CHECK_FALSE(load_native(path));
    }
    SUBCASE("extra table") {
        const auto path = tampered_file(directory.path(), original, [](SQLite::Database& database) {
            database.exec("CREATE TABLE extra (x INTEGER)");
        });
        CHECK_FALSE(load_native(path));
    }
    SUBCASE("virtual table") {
        const auto path = tampered_file(directory.path(), original, [](SQLite::Database& database) {
            database.exec("CREATE VIRTUAL TABLE search USING fts5(body)");
        });
        CHECK_FALSE(load_native(path));
    }
    SUBCASE("altered table definition") {
        const auto path = tampered_file(directory.path(), original, [](SQLite::Database& database) {
            database.exec("DROP TABLE document_setting");
            database.exec("CREATE TABLE document_setting (key TEXT PRIMARY KEY, schema_version INTEGER NOT NULL, "
                          "value_json TEXT NOT NULL) STRICT");
        });
        CHECK_FALSE(load_native(path));
    }
}

TEST_CASE("wrong application id, newer schema and non-whitelisted settings are refused") {
    test::TemporaryDirectory directory;
    const DocumentData original = make_blank_document("Identity");
    SUBCASE("application id") {
        const auto path = tampered_file(directory.path(), original,
                                        [](SQLite::Database& database) { database.exec("PRAGMA application_id = 5"); });
        const auto loaded = load_native(path);
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().code == ErrorCode::invalid_data);
    }
    SUBCASE("newer user_version") {
        const auto path = tampered_file(directory.path(), original,
                                        [](SQLite::Database& database) { database.exec("PRAGMA user_version = 2"); });
        const auto loaded = open_native(path);
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().code == ErrorCode::unsupported);
    }
    SUBCASE("WAL-mode file") {
        const auto path = tampered_file(directory.path(), original, [](SQLite::Database& database) {
            database.exec("PRAGMA journal_mode = WAL");
        });
        const auto loaded = load_native(path);
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().message.find("rollback-journal") != std::string::npos);
    }
    SUBCASE("setting key") {
        const auto path = tampered_file(directory.path(), original, [](SQLite::Database& database) {
            database.exec("INSERT INTO document_setting VALUES ('run.command', 1, '\"calc.exe\"')");
        });
        CHECK_FALSE(load_native(path));
    }
    SUBCASE("saving a non-whitelisted setting fails") {
        DocumentData document = original;
        document.settings["run.command"] = "\"calc.exe\"";
        const auto bytes = native_to_bytes(document);
        REQUIRE_FALSE(bytes);
        CHECK(bytes.error().code == ErrorCode::invalid_argument);
    }
}

TEST_CASE("corrupted, truncated and foreign bytes are rejected without crashing") {
    const DocumentData original = rich_document("Corrupt", false);
    const auto bytes = native_to_bytes(original);
    REQUIRE(bytes);
    REQUIRE(bytes->size() > 16384);

    SUBCASE("damaged b-tree page") {
        auto damaged = *bytes;
        for (std::size_t index = 4096; index < 4096 + 64; ++index)
            damaged[index] = 0xFF;
        CHECK_FALSE(native_from_bytes(damaged));
    }
    SUBCASE("damaged header") {
        auto damaged = *bytes;
        damaged[0] = 'X';
        CHECK_FALSE(native_from_bytes(damaged));
    }
    SUBCASE("scattered byte flips") {
        auto damaged = *bytes;
        for (std::size_t index = 100; index < damaged.size(); index += 997)
            damaged[index] = static_cast<std::uint8_t>(damaged[index] ^ 0x5A);
        CHECK_FALSE(native_from_bytes(damaged));
    }
    SUBCASE("truncated") {
        const std::vector<std::uint8_t> truncated(bytes->begin(), bytes->begin() + static_cast<std::ptrdiff_t>(bytes->size() / 2));
        CHECK_FALSE(native_from_bytes(truncated));
    }
    SUBCASE("garbage") {
        std::vector<std::uint8_t> garbage(8192);
        for (std::size_t index = 0; index < garbage.size(); ++index)
            garbage[index] = static_cast<std::uint8_t>((index * 2654435761u) >> 13);
        CHECK_FALSE(native_from_bytes(garbage));
        CHECK_FALSE(native_from_bytes(std::vector<std::uint8_t>{}));
    }
    SUBCASE("file size limit") {
        NativeLoadLimits limits;
        limits.max_file_bytes = 1024;
        const auto loaded = native_from_bytes(*bytes, limits);
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().code == ErrorCode::limit_exceeded);
    }
}

TEST_CASE("opaque records make a file inspection-only") {
    test::TemporaryDirectory directory;
    const DocumentData original = rich_document("Opaque", false);
    const auto path = tampered_file(directory.path(), original, [](SQLite::Database& database) {
        database.exec("INSERT INTO opaque_record (id, source_id, record_kind, source_offset, placement_json, bytes) "
                      "SELECT randomblob(16), id, 'chunk', 14, '{}', x'0102' FROM source_blob");
    });
    const auto loaded = load_native(path);
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().code == ErrorCode::unsupported);
    const auto opened = open_native(path);
    REQUIRE(opened);
    CHECK(opened->read_only);
    check_same_document(opened->document, original);
}

TEST_CASE("a failed save keeps the previous file") {
    test::TemporaryDirectory directory;
    const auto path = directory.path() / "keep.mwb";
    const DocumentData first = rich_document("First", false);
    REQUIRE(save_native(first, path));
    DocumentData second = first;
    second.title = "Second";

    SUBCASE("missing destination directory") {
        const auto status = save_native(second, directory.path() / "missing" / "keep.mwb");
        REQUIRE_FALSE(status);
        CHECK(status.error().code == ErrorCode::io);
    }
    SUBCASE("destination is a directory") {
        std::filesystem::create_directory(directory.path() / "folder.mwb");
        CHECK_FALSE(save_native(second, directory.path() / "folder.mwb"));
        CHECK(std::filesystem::is_directory(directory.path() / "folder.mwb"));
    }
    SUBCASE("invalid document") {
        second.sequences[0].tracks[0].end_tick = -5;
        const auto status = save_native(second, path);
        REQUIRE_FALSE(status);
        CHECK(status.error().code == ErrorCode::invalid_argument);
    }
#ifdef _WIN32
    SUBCASE("read-only destination file") {
        std::filesystem::permissions(path, std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                                               std::filesystem::perms::others_write,
                                     std::filesystem::perm_options::remove);
        const auto status = save_native(second, path);
        std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
        REQUIRE_FALSE(status);
        CHECK(status.error().code == ErrorCode::io);
    }
#endif
    const auto loaded = load_native(path);
    REQUIRE(loaded);
    check_same_document(*loaded, first);
    CHECK(files_containing(directory.path(), "mwb-tmp") == 0);
}

TEST_CASE("schema checksums and version classification") {
    CHECK(migration::verify_checksums(migration::SchemaFamily::native));
    CHECK(migration::verify_checksums(migration::SchemaFamily::recovery));
    CHECK(migration::migration_steps(migration::SchemaFamily::native).empty());
    const auto current = migration::classify_version(migration::SchemaFamily::native, 1);
    REQUIRE(current);
    CHECK(*current == migration::SchemaAccess::read_write);
    const auto newer = migration::classify_version(migration::SchemaFamily::native, 2);
    REQUIRE_FALSE(newer);
    CHECK(newer.error().code == ErrorCode::unsupported);
    const auto zero = migration::classify_version(migration::SchemaFamily::recovery, 0);
    REQUIRE_FALSE(zero);
    CHECK(zero.error().code == ErrorCode::invalid_data);
}

} // TEST_SUITE native

// =================================================================================================

TEST_SUITE("history") {

TEST_CASE("undo and redo restore exact semantic states") {
    DocumentData document = make_blank_document("History");
    CommandHistory history;
    const auto initial = semantic_hash(document);
    const StateId initial_state = history.current_state();

    ChangeBuilder first(document, "First note");
    first.add_note(first_track(document), 0, 480, 0, 60, 100);
    REQUIRE(history.execute(document, first.finish()));
    const auto after_first = semantic_hash(document);
    ChangeBuilder second(document, "Second note");
    second.add_note(first_track(document), 480, 960, 0, 62, 100);
    REQUIRE(history.execute(document, second.finish()));
    const auto after_second = semantic_hash(document);

    CHECK(history.undo_label() == std::optional<std::string>("Second note"));
    REQUIRE(history.undo(document));
    CHECK(semantic_hash(document) == after_first);
    REQUIRE(history.undo(document));
    CHECK(semantic_hash(document) == initial);
    CHECK(history.current_state() == initial_state);
    CHECK_FALSE(history.can_undo());
    CHECK_FALSE(history.undo(document));
    CHECK(history.redo_label() == std::optional<std::string>("First note"));
    REQUIRE(history.redo(document));
    REQUIRE(history.redo(document));
    CHECK(semantic_hash(document) == after_second);
    CHECK_FALSE(history.redo(document));
    REQUIRE(history.execute(document, ChangeSet{}));
    CHECK(history.undo_depth() == 2);
}

TEST_CASE("a new edit after undo discards the redo branch") {
    DocumentData document = make_blank_document("Branch");
    CommandHistory history;
    for (int index = 0; index < 3; ++index) {
        ChangeBuilder builder(document, "note " + std::to_string(index));
        builder.add_note(first_track(document), index * 480, index * 480 + 240, 0, 60, 100);
        REQUIRE(history.execute(document, builder.finish()));
    }
    REQUIRE(history.undo(document));
    REQUIRE(history.undo(document));
    CHECK(history.redo_depth() == 2);
    const std::size_t memory_before = history.memory_bytes();
    ChangeBuilder builder(document, "branch");
    builder.add_note(first_track(document), 2000, 2400, 0, 70, 100);
    REQUIRE(history.execute(document, builder.finish()));
    CHECK_FALSE(history.can_redo());
    CHECK(history.undo_depth() == 2);
    CHECK(history.memory_bytes() < memory_before);
}

TEST_CASE("memory budget evicts whole oldest entries and reports a history boundary") {
    DocumentData document = make_blank_document("Budget");
    ChangeBuilder probe(document, "probe");
    probe.add_note(first_track(document), 0, 240, 0, 60, 100);
    const std::size_t entry_size = probe.finish().memory_estimate();

    CommandHistory history(entry_size * 3 + entry_size / 2);
    for (int index = 0; index < 10; ++index) {
        ChangeBuilder builder(document, "note");
        builder.add_note(first_track(document), index * 480, index * 480 + 240, 0, 60, 100);
        REQUIRE(history.execute(document, builder.finish()));
        CHECK(history.memory_bytes() <= history.budget_bytes());
    }
    CHECK(history.history_boundary());
    CHECK(history.undo_depth() == 3);
    CHECK(history.evicted_entries() == 7);
    while (history.can_undo())
        REQUIRE(history.undo(document));
    // Seven notes remain: the evicted commands cannot be undone.
    CHECK(pair_notes(document.sequences[0]).notes.size() == 7);
    history.set_budget_bytes(0);
    CHECK(history.memory_bytes() <= history.redo_depth() * entry_size);
}

TEST_CASE("dirty state follows semantic state identity") {
    test::TemporaryDirectory directory;
    WorkingSession session(make_blank_document("Dirty"), SessionOrigin::new_document);
    CHECK_FALSE(session.is_dirty());
    add_note(session, 0, 60);
    CHECK(session.is_dirty());
    const auto path = directory.path() / "dirty.mwb";
    REQUIRE(session.save(path));
    CHECK_FALSE(session.is_dirty());
    CHECK(session.file_path() == std::optional<std::filesystem::path>(path));

    add_note(session, 480, 62);
    CHECK(session.is_dirty());
    REQUIRE(session.undo());
    CHECK_FALSE(session.is_dirty()); // back at the saved state
    REQUIRE(session.redo());
    CHECK(session.is_dirty());
    REQUIRE(session.undo());
    REQUIRE(session.undo());
    CHECK(session.is_dirty()); // before the saved state
    add_note(session, 960, 64); // discards the redo branch that held the saved state
    CHECK(session.is_dirty());
    CHECK_FALSE(session.history().can_redo());
    REQUIRE(session.undo());
    CHECK(session.is_dirty()); // the saved state is unreachable now
}

TEST_CASE("edits during a save stay dirty; a failed save leaves history and dirty state intact") {
    test::TemporaryDirectory directory;
    WorkingSession session(make_blank_document("Save"), SessionOrigin::new_document);
    add_note(session, 0, 60);
    const SaveTicket ticket = session.begin_save();
    add_note(session, 480, 62); // edit while the save runs
    const auto path = directory.path() / "save.mwb";
    REQUIRE(session.complete_save(ticket, path, save_native(*ticket.snapshot, path)));
    CHECK(session.is_dirty());
    CHECK(session.saved_state() == std::optional<StateId>(ticket.state));
    REQUIRE(session.undo());
    CHECK_FALSE(session.is_dirty());
    const auto loaded = load_native(path);
    REQUIRE(loaded);
    CHECK(semantic_hash(*loaded) == semantic_hash(session.document()));

    REQUIRE(session.redo());
    const std::size_t depth = session.history().undo_depth();
    const auto failed = session.save(directory.path() / "missing" / "x.mwb");
    REQUIRE_FALSE(failed);
    CHECK(session.is_dirty());
    CHECK(session.history().undo_depth() == depth);
    CHECK(session.file_path() == std::optional<std::filesystem::path>(path));
}

TEST_CASE("listeners see every transition with increasing generations") {
    WorkingSession session(make_blank_document("Listen"), SessionOrigin::new_document);
    std::vector<std::pair<SessionEventKind, Generation>> seen;
    const ListenerId id = session.add_listener([&seen](const SessionEvent& event) {
        CHECK(event.result_generation == event.base_generation + 1);
        seen.emplace_back(event.kind, event.result_generation);
    });
    const Generation start = session.generation();
    add_note(session, 0, 60);
    REQUIRE(session.undo());
    REQUIRE(session.redo());
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == std::pair{SessionEventKind::edited, start + 1});
    CHECK(seen[1] == std::pair{SessionEventKind::undone, start + 2});
    CHECK(seen[2] == std::pair{SessionEventKind::redone, start + 3});
    CHECK(session.document().generation == start + 3);
    session.remove_listener(id);
    add_note(session, 480, 62);
    CHECK(seen.size() == 3);
}

} // TEST_SUITE history

// =================================================================================================

TEST_SUITE("recovery") {

TEST_CASE("command codec round-trips every operation kind") {
    DocumentData document = rich_document("Codec", false);
    const auto& sequence = document.sequences[0];
    const Track& track = sequence.tracks[0];
    Track header = track;
    header.events.clear();
    Track renamed = header;
    renamed.name = "Renamed \xF0\x9F\x8E\xB8";
    Sequence sequence_header = sequence;
    sequence_header.tracks.clear();
    Sequence sequence_after = sequence_header;
    sequence_after.division = Division::make_smpte(SmpteFormat::fps30, 100);
    Event moved = track.events[3];
    moved.tick += 10;

    CommandEnvelope envelope;
    envelope.command_id = Uuid::generate();
    envelope.kind = CommandKind::redo;
    envelope.base_semantic_hash = semantic_hash(document);
    envelope.base_generation = 4;
    envelope.result_generation = 5;
    envelope.changes.label = "Everything \xD7\x90";
    envelope.changes.operations = {
        EventInsert{track.id, large_sysex(5)},
        EventRemove{track.id, track.events[0]},
        EventReplace{track.id, track.events[3], moved},
        TrackInsert{sequence.id, 1, sequence.tracks[1]},
        TrackRemove{sequence.id, 0, track},
        TrackPropertiesChange{track.id, header, renamed},
        TrackMove{sequence.id, 0, 1, track.id}, // the track identity round-trips too
        SequencePropertiesChange{sequence.id, sequence_header, sequence_after},
        SequenceInsert{1, document.sequences[1]},
        SequenceRemove{0, sequence},
        DocumentPropertiesChange{"a", "b", "{}", R"({"x":1})"},
    };
    const auto bytes = encode_command(envelope);
    REQUIRE(bytes);
    const auto decoded = decode_command(*bytes);
    REQUIRE_MESSAGE(decoded, (decoded ? "" : decoded.error().describe()));
    CHECK(decoded->command_id == envelope.command_id);
    CHECK(decoded->kind == CommandKind::redo);
    CHECK(decoded->base_semantic_hash == envelope.base_semantic_hash);
    CHECK(decoded->changes.operations.size() == envelope.changes.operations.size());
    const auto again = encode_command(*decoded);
    REQUIRE(again);
    // Command ids are carried, so re-encoding the decoded envelope reproduces the bytes.
    CHECK(*again == *bytes);
    const auto& replace = std::get<EventReplace>(decoded->changes.operations[2]);
    CHECK(replace.after == moved);
    CHECK(std::get<TrackInsert>(decoded->changes.operations[3]).track == sequence.tracks[1]);
    CHECK(std::get<SequenceRemove>(decoded->changes.operations[9]).sequence == sequence);

    SUBCASE("damaged envelopes are rejected") {
        auto damaged = *bytes;
        damaged.resize(damaged.size() / 3);
        CHECK_FALSE(decode_command(damaged));
        CHECK_FALSE(decode_command(std::vector<std::uint8_t>{0xFF, 0x00, 0x13}));
        CommandEnvelope bad = envelope;
        bad.result_generation = bad.base_generation;
        CHECK_FALSE(encode_command(bad));
    }
}

TEST_CASE("an unclosed session recovers to the live document") {
    test::TemporaryDirectory directory;
    const auto recovery_directory = directory.path() / std::filesystem::path(u8"recovery ✓");
    auto session = std::make_unique<WorkingSession>(rich_document("Live \xF0\x9F\x8E\xB9", false),
                                                    SessionOrigin::opened_file, directory.path() / "live.mwb");
    std::shared_ptr<RecoveryJournal> journal;
    {
        SourceReference source;
        source.document_name = session->document().title;
        source.file_path_utf8 = "C:/music/live.mwb";
        auto created = RecoveryJournal::create(recovery_directory, session->document(), source);
        REQUIRE_MESSAGE(created, (created ? "" : created.error().describe()));
        journal = std::move(*created);
    }
    connect_recovery(*session, journal);

    for (int index = 0; index < 6; ++index)
        add_note(*session, 2000 + index * 240, static_cast<std::uint8_t>(60 + index));
    REQUIRE(session->undo());
    REQUIRE(session->undo());
    REQUIRE(session->redo());
    {
        ChangeBuilder builder(session->document(), "Rename");
        DocumentPropertiesChange rename{session->document().title, "Renamed", session->document().metadata_json,
                                        session->document().metadata_json};
        builder.append(rename);
        REQUIRE(session->apply(builder.finish()));
    }
    CHECK_FALSE(journal->last_error().has_value());
    CHECK(journal->pending_commands() == 10);
    REQUIRE(journal->flush());
    journal->mark_saved(1234);
    REQUIRE(journal->flush());

    const auto live_hash = semantic_hash(session->document());
    const Generation live_generation = session->generation();
    const Uuid session_id = journal->session_id();
    // While its journal is open the session is live: listed as such, and not discardable.
    const auto live_sessions = list_recoverable_sessions(recovery_directory);
    REQUIRE(live_sessions);
    REQUIRE(live_sessions->size() == 1);
    CHECK(live_sessions->at(0).live);
    CHECK(discard_recovery_session(recovery_directory, session_id).error().code == ErrorCode::conflict);
    journal.reset(); // simulated crash: the session is never marked cleanly closed

    const auto sessions = list_recoverable_sessions(recovery_directory);
    REQUIRE(sessions);
    REQUIRE(sessions->size() == 1);
    CHECK(sessions->at(0).session_id == session_id);
    CHECK(sessions->at(0).document_id == session->document().id);
    CHECK(sessions->at(0).source.document_name == "Live \xF0\x9F\x8E\xB9");
    CHECK(sessions->at(0).source.last_explicit_save_ms == std::optional<std::int64_t>(1234));
    CHECK(sessions->at(0).journaled_commands == 10);
    CHECK_FALSE(sessions->at(0).live);

    const auto recovered = recover_session(recovery_directory, session_id);
    REQUIRE_MESSAGE(recovered, (recovered ? "" : recovered.error().describe()));
    CHECK(semantic_hash(recovered->document) == live_hash);
    CHECK(recovered->document.generation == live_generation);
    CHECK(recovered->report.applied_commands == 10);
    CHECK(recovered->report.skipped_commands == 0);
    CHECK_FALSE(recovered->report.stop_reason.has_value());

    WorkingSession reopened(recovered->document, SessionOrigin::recovered);
    CHECK(reopened.is_dirty());
    CHECK_FALSE(reopened.file_path().has_value());

    REQUIRE(discard_recovery_session(recovery_directory, session_id));
    const auto after_discard = list_recoverable_sessions(recovery_directory);
    REQUIRE(after_discard);
    CHECK(after_discard->empty());
}

TEST_CASE("a corrupt tail recovers through the last valid entry") {
    test::TemporaryDirectory directory;
    WorkingSession session(make_blank_document("Tail"), SessionOrigin::new_document);
    auto created = RecoveryJournal::create(directory.path(), session.document(), SourceReference{"Tail", {}, {}, {}});
    REQUIRE(created);
    std::shared_ptr<RecoveryJournal> journal = std::move(*created);
    connect_recovery(session, journal);
    std::vector<Sha256Digest> hashes;
    for (int index = 0; index < 5; ++index) {
        add_note(session, index * 480, static_cast<std::uint8_t>(60 + index));
        hashes.push_back(semantic_hash(session.document()));
    }
    REQUIRE(journal->flush());
    const Uuid session_id = journal->session_id();
    journal.reset();

    SUBCASE("damaged payload of the fourth command") {
        {
            SQLite::Database database((directory.path() / recovery_database_name).string(), SQLite::OPEN_READWRITE);
            database.exec("UPDATE recovery_command SET payload = randomblob(length(payload)) WHERE command_no = 4");
        }
        const auto recovered = recover_session(directory.path(), session_id);
        REQUIRE(recovered);
        CHECK(recovered->report.applied_commands == 3);
        CHECK(recovered->report.skipped_commands == 2);
        CHECK(recovered->report.last_valid_command_no == 3);
        REQUIRE(recovered->report.stop_reason.has_value());
        CHECK(recovered->report.stop_reason->find("checksum") != std::string::npos);
        CHECK(semantic_hash(recovered->document) == hashes[2]);
        // The evidence remains.
        SQLite::Database database((directory.path() / recovery_database_name).string(), SQLite::OPEN_READONLY);
        CHECK(database.execAndGet("SELECT count(*) FROM recovery_command").getInt() == 5);
    }
    SUBCASE("unsupported codec version of the last command") {
        {
            SQLite::Database database((directory.path() / recovery_database_name).string(), SQLite::OPEN_READWRITE);
            database.exec("UPDATE recovery_command SET codec_version = 9 WHERE command_no = 5");
        }
        const auto recovered = recover_session(directory.path(), session_id);
        REQUIRE(recovered);
        CHECK(recovered->report.applied_commands == 4);
        CHECK(semantic_hash(recovered->document) == hashes[3]);
    }
    SUBCASE("missing middle command stops at the gap") {
        {
            SQLite::Database database((directory.path() / recovery_database_name).string(), SQLite::OPEN_READWRITE);
            database.exec("DELETE FROM recovery_command WHERE command_no = 2");
        }
        const auto recovered = recover_session(directory.path(), session_id);
        REQUIRE(recovered);
        CHECK(recovered->report.applied_commands == 1);
        CHECK(semantic_hash(recovered->document) == hashes[0]);
    }
}

TEST_CASE("checkpoints are written by count, size and time and compaction follows them") {
    test::TemporaryDirectory directory;
    std::int64_t clock_ms = 1'000'000;
    WorkingSession session(make_blank_document("Checkpoints"), SessionOrigin::new_document);
    RecoveryPolicy policy;
    policy.checkpoint_command_count = 4;
    auto created = RecoveryJournal::create(directory.path(), session.document(), SourceReference{"Checkpoints", {}, {}, {}},
                                           policy, [&clock_ms] { return clock_ms; });
    REQUIRE(created);
    std::shared_ptr<RecoveryJournal> journal = std::move(*created);
    connect_recovery(session, journal);
    const auto database_path = (directory.path() / recovery_database_name).string();
    const auto count = [&](const char* sql) {
        SQLite::Database database(database_path, SQLite::OPEN_READONLY);
        return database.execAndGet(sql).getInt64();
    };

    for (int index = 0; index < 9; ++index) {
        add_note(session, index * 480, 60);
        REQUIRE(journal->flush());
    }
    // Checkpoints 0 (initial), 4 and 8; compaction keeps the previous checkpoint (4) as a
    // fallback together with the commands after it (5..9).
    CHECK(count("SELECT count(*) FROM recovery_checkpoint") == 2);
    CHECK(count("SELECT max(command_no) FROM recovery_checkpoint") == 8);
    CHECK(count("SELECT min(command_no) FROM recovery_checkpoint") == 4);
    CHECK(count("SELECT count(*) FROM recovery_command") == 5);

    SUBCASE("time threshold") {
        clock_ms += 61'000;
        add_note(session, 6000, 61);
        REQUIRE(journal->flush());
        CHECK(count("SELECT max(command_no) FROM recovery_checkpoint") == 10);
        CHECK(count("SELECT count(*) FROM recovery_command") == 2); // 9 and 10 after checkpoint 8
    }
    SUBCASE("size threshold") {
        RecoveryPolicy big;
        big.checkpoint_journal_bytes = 64 * 1024;
        big.checkpoint_command_count = 1000;
        journal.reset();
        auto restarted = RecoveryJournal::create(directory.path(), session.document(),
                                                 SourceReference{"Size", {}, {}, {}}, big, [&clock_ms] { return clock_ms; });
        REQUIRE(restarted);
        journal = std::move(*restarted);
        connect_recovery(session, journal);
        ChangeBuilder builder(session.document(), "SysEx");
        builder.insert(first_track(session.document()), large_sysex(7000));
        REQUIRE(session.apply(builder.finish()));
        REQUIRE(journal->flush());
        SQLite::Database database(database_path, SQLite::OPEN_READONLY);
        SQLite::Statement statement(database, "SELECT max(command_no) FROM recovery_checkpoint WHERE session_id = ?");
        statement.bind(1, journal->session_id().bytes().data(), 16);
        REQUIRE(statement.executeStep());
        CHECK(statement.getColumn(0).getInt64() == 1);
    }

    const auto live_hash = semantic_hash(session.document());
    const Uuid session_id = journal->session_id();
    journal.reset();
    const auto recovered = recover_session(directory.path(), session_id);
    REQUIRE(recovered);
    CHECK(semantic_hash(recovered->document) == live_hash);
}

TEST_CASE("a cleanly closed session is not offered for recovery") {
    test::TemporaryDirectory directory;
    WorkingSession session(make_blank_document("Closed"), SessionOrigin::new_document);
    auto created = RecoveryJournal::create(directory.path(), session.document(), SourceReference{"Closed", {}, {}, {}});
    REQUIRE(created);
    std::shared_ptr<RecoveryJournal> journal = std::move(*created);
    connect_recovery(session, journal);
    add_note(session, 0, 60);
    REQUIRE(journal->flush());
    REQUIRE(journal->mark_cleanly_closed());
    journal.reset();
    const auto sessions = list_recoverable_sessions(directory.path());
    REQUIRE(sessions);
    CHECK(sessions->empty());
    const auto empty_directory = list_recoverable_sessions(directory.path() / "nothing-here");
    REQUIRE(empty_directory);
    CHECK(empty_directory->empty());
}

TEST_CASE("the journal refuses non-contiguous generations") {
    test::TemporaryDirectory directory;
    DocumentData document = make_blank_document("Gaps");
    auto created = RecoveryJournal::create(directory.path(), document, SourceReference{"Gaps", {}, {}, {}});
    REQUIRE(created);
    ChangeBuilder builder(document, "note");
    builder.add_note(first_track(document), 0, 240, 0, 60, 100);
    const ChangeSet changes = builder.finish();
    REQUIRE(apply_change_set(document, changes));
    const auto status = (*created)->record(CommandKind::edit, changes, document.generation + 3, document.generation + 4, document);
    REQUIRE_FALSE(status);
    CHECK(status.error().code == ErrorCode::invalid_argument);
}

TEST_CASE("create() leaves the first checkpoint to the background; flush(true) makes the session recoverable") {
    // code-document-1 / C4: opening a document must not serialize it on the caller's thread, and a
    // session whose first checkpoint never committed is never offered (nor half-recovered).
    test::TemporaryDirectory directory;
    const DocumentData document = rich_document("Quick open", false);
    {
        auto created = RecoveryJournal::create(directory.path(), document, SourceReference{"Quick open", {}, {}, {}});
        REQUIRE(created);
        // Simulated crash right after opening: the checkpoint may or may not have committed.
    }
    const auto early = list_recoverable_sessions(directory.path());
    REQUIRE(early);
    for (const auto& session : *early) {
        const auto recovered = recover_session(directory.path(), session.session_id);
        REQUIRE_MESSAGE(recovered, (recovered ? "" : recovered.error().describe()));
        CHECK(semantic_hash(recovered->document) == semantic_hash(document));
        REQUIRE(discard_recovery_session(directory.path(), session.session_id));
    }

    auto created = RecoveryJournal::create(directory.path(), document, SourceReference{"Quick open", {}, {}, {}});
    REQUIRE(created);
    std::shared_ptr<RecoveryJournal> journal = std::move(*created);
    REQUIRE(journal->flush(true)); // what the recovery path waits for before discarding old evidence
    CHECK(journal->checkpoint_committed());
    const Uuid session_id = journal->session_id();
    journal.reset(); // crash after the checkpoint
    const auto sessions = list_recoverable_sessions(directory.path());
    REQUIRE(sessions);
    REQUIRE(sessions->size() == 1);
    CHECK(sessions->at(0).session_id == session_id);
    const auto recovered = recover_session(directory.path(), session_id);
    REQUIRE(recovered);
    CHECK(semantic_hash(recovered->document) == semantic_hash(document));
}

} // TEST_SUITE recovery
