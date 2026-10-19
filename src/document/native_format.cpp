#include "mwb/document/native_format.hpp"

#include "mwb/core/hashing.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/core/serialization.hpp"
#include "mwb/document/file_system.hpp"
#include "mwb/document/migration.hpp"
#include "mwb/document/schemas.hpp"
#include "mwb/document/sqlite_support.hpp"

#include <SQLiteCpp/Transaction.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <unordered_map>

namespace mwb::document {

namespace {

using namespace sqlite_support;

constexpr std::array<std::string_view, 3> allowed_setting_keys{"editor.view", "editor.shortcut_profile",
                                                               "export.last_profile"};

/// Identifies the writer in project.writer_version.
constexpr const char* writer_version = "MIDI Workbench native writer 1";

/// Maximum take chunk payload (take_chunk.payload CHECK).
constexpr std::size_t max_take_chunk_bytes = 1024 * 1024;
/// Maximum retained source bytes (source_blob.bytes CHECK).
constexpr std::size_t max_source_bytes = 256u * 1024u * 1024u;
/// Take chunk codec written by this version (section 23.1).
constexpr std::int64_t take_chunk_codec_version = 1;

/// Row caps for the secondary tables of an untrusted file, checked before the rows are read so
/// a crafted file of millions of tiny rows cannot allocate gigabytes before validation.
constexpr std::int64_t max_region_rows = 100'000; // per table: region, scale_region, recipe
constexpr std::int64_t max_take_rows = 10'000;
constexpr std::int64_t max_take_chunk_rows = 1'000'000;
constexpr std::int64_t max_source_rows = 1'024;

/// The secondary event index of native_v1. write_tables() creates it after the bulk insert
/// (maintaining it row by row is slower); the stored schema text is the same either way.
constexpr std::string_view event_index_ddl = "CREATE INDEX event_time ON midi_event(track_id, tick, order_key);";

/// Temporary save files are named "<target>.~<8 hex digits>.mwb-tmp".
constexpr std::string_view temporary_marker = ".~";
constexpr std::string_view temporary_suffix = ".mwb-tmp";

// --- Enumerations <-> text ----------------------------------------------------------------------

const char* category_text(EventCategory category) {
    switch (category) {
    case EventCategory::channel: return "channel";
    case EventCategory::meta: return "meta";
    case EventCategory::sysex_f0: return "sysex_f0";
    case EventCategory::sysex_f7: return "sysex_f7";
    }
    return "channel";
}

std::optional<EventCategory> category_from(const std::string& text) {
    if (text == "channel")
        return EventCategory::channel;
    if (text == "meta")
        return EventCategory::meta;
    if (text == "sysex_f0")
        return EventCategory::sysex_f0;
    if (text == "sysex_f7")
        return EventCategory::sysex_f7;
    return std::nullopt;
}

const char* take_status_text(TakeStatus status) {
    switch (status) {
    case TakeStatus::captured: return "captured";
    case TakeStatus::accepted: return "accepted";
    case TakeStatus::interrupted: return "interrupted";
    case TakeStatus::discarded: return "discarded";
    }
    return "captured";
}

std::optional<TakeStatus> take_status_from(const std::string& text) {
    if (text == "captured")
        return TakeStatus::captured;
    if (text == "accepted")
        return TakeStatus::accepted;
    if (text == "interrupted")
        return TakeStatus::interrupted;
    if (text == "discarded")
        return TakeStatus::discarded;
    return std::nullopt;
}

bool is_json_object(const std::string& text) {
    const auto value = nlohmann::json::parse(text, nullptr, false);
    return !value.is_discarded() && value.is_object();
}

bool is_json_value(const std::string& text) { return nlohmann::json::accept(text); }

/// Spelling JSON written for scale regions; the core model has no spelling field yet.
const std::string scale_spelling_json = R"({"version":1})";

// --- Pre-save checks ----------------------------------------------------------------------------

/// Checks everything the schema or the loader would reject, so a save never produces a file the
/// application cannot open again.
Status check_writable(const DocumentData& document) {
    const auto diagnostics = validate_document(document);
    for (const auto& diagnostic : diagnostics)
        if (diagnostic.severity == Severity::error)
            return make_error(ErrorCode::invalid_argument,
                              "document fails validation (" + diagnostic.code + "): " + diagnostic.message);
    if (!is_json_object(document.metadata_json))
        return make_error(ErrorCode::invalid_argument, "document metadata is not a JSON object");
    if (document.created_ms < 0 || document.modified_ms < 0)
        return make_error(ErrorCode::invalid_argument, "document timestamps must not be negative");
    if (document.generation > static_cast<Generation>(std::numeric_limits<std::int64_t>::max()))
        return make_error(ErrorCode::invalid_argument, "document generation does not fit the native format");
    for (const auto& sequence : document.sequences) {
        for (const auto& track : sequence.tracks)
            for (const auto& event : track.events)
                if (event.order_key > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                    return make_error(ErrorCode::invalid_argument, "event order key does not fit the native format");
        for (const auto& recipe : sequence.recipes) {
            if (recipe.algorithm_version <= 0)
                return make_error(ErrorCode::invalid_argument, "recipe algorithm version must be positive");
            if (!is_json_object(recipe.parameters_json) || !is_json_value(recipe.targets_json))
                return make_error(ErrorCode::invalid_argument, "recipe JSON is malformed");
        }
    }
    for (const auto& take : document.takes) {
        if (!find_track(document, take.track))
            return make_error(ErrorCode::invalid_argument, "recording take refers to a missing track");
        if (take.start_tick < 0 || take.end_tick < take.start_tick)
            return make_error(ErrorCode::invalid_argument, "recording take has an invalid range");
        if (!is_json_object(take.capture_json))
            return make_error(ErrorCode::invalid_argument, "recording take capture JSON is not an object");
        for (std::size_t index = 0; index < take.chunks.size(); ++index) {
            const auto& chunk = take.chunks[index];
            if (chunk.ordinal < 0 || chunk.first_time_ns < 0 || chunk.payload.size() > max_take_chunk_bytes)
                return make_error(ErrorCode::invalid_argument, "recording take chunk is out of range");
            if (index > 0 && take.chunks[index - 1].ordinal >= chunk.ordinal)
                return make_error(ErrorCode::invalid_argument, "recording take chunks are not in ordinal order");
        }
    }
    for (const auto& source : document.sources) {
        if (source.media_type != "smf" && source.media_type != "rmid")
            return make_error(ErrorCode::invalid_argument, "source media type must be smf or rmid");
        if (!source.bytes || source.bytes->size() > max_source_bytes)
            return make_error(ErrorCode::invalid_argument, "source bytes are missing or too large");
        if (sha256(*source.bytes) != source.sha256)
            return make_error(ErrorCode::invalid_argument, "source bytes do not match their SHA-256");
        if (!is_json_object(source.import_report_json))
            return make_error(ErrorCode::invalid_argument, "source import report is not a JSON object");
    }
    for (const auto& [key, value] : document.settings) {
        if (!is_allowed_document_setting(key))
            return make_error(ErrorCode::invalid_argument, "document setting key is not allowed: " + key);
        if (!is_json_value(value))
            return make_error(ErrorCode::invalid_argument, "document setting value is not JSON: " + key);
    }
    return ok();
}

// --- Writer -------------------------------------------------------------------------------------

/// Creates the schema and writes every row inside one transaction. Throws SQLite::Exception.
/// The DDL runs inside the transaction too (one commit and one set of fsyncs instead of one per
/// statement); its "PRAGMA foreign_keys = ON" is a no-op there, so rows are inserted without
/// per-row foreign key lookups and check_database_integrity() verifies the keys once afterwards.
void write_tables(SQLite::Database& database, const DocumentData& document) {
    std::string ddl(schemas::native_v1);
    const auto index_at = ddl.find(event_index_ddl);
    if (index_at == std::string::npos)
        throw std::logic_error("native schema has no event_time index statement");
    ddl.erase(index_at, event_index_ddl.size());
    database.exec("PRAGMA foreign_keys = OFF");
    SQLite::Transaction transaction(database);
    database.exec(ddl);

    {
        SQLite::Statement insert(database,
                                 "INSERT INTO project (singleton, uuid, title, created_ms, modified_ms, generation, "
                                 "writer_version, metadata_json) VALUES (1, ?, ?, ?, ?, ?, ?, ?)");
        bind_uuid(insert, 1, document.id.uuid());
        insert.bind(2, document.title);
        insert.bind(3, document.created_ms);
        insert.bind(4, document.modified_ms);
        insert.bind(5, static_cast<std::int64_t>(document.generation));
        insert.bind(6, writer_version);
        insert.bind(7, document.metadata_json);
        insert.exec();
    }

    SQLite::Statement insert_sequence(
        database, "INSERT INTO sequence (id, sort_order, name, division, ppq, fps_code, ticks_per_frame, end_tick, "
                  "root_pc, scale_mask, settings_json) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    SQLite::Statement insert_track(
        database, "INSERT INTO track (id, sequence_id, sort_order, name, color_rgba, end_tick, muted, solo, is_drum, "
                  "routing_json, instrument_json) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    SQLite::Statement insert_event(database, "INSERT INTO midi_event (id, track_id, tick, order_key, category, code, "
                                             "payload, flags) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    SQLite::Statement insert_link(database, "INSERT INTO note_link (id, on_event_id, off_event_id, pairing_policy, "
                                            "flags) VALUES (?, ?, ?, ?, ?)");
    SQLite::Statement insert_region(database, "INSERT INTO region (id, sequence_id, name, start_tick, end_tick, "
                                              "tracks_json, is_loop) VALUES (?, ?, ?, ?, ?, ?, ?)");
    SQLite::Statement insert_scale(database,
                                   "INSERT INTO scale_region (id, sequence_id, start_tick, end_tick, root_pc, "
                                   "scale_mask, scale_name, spelling_json) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    SQLite::Statement insert_recipe(database,
                                    "INSERT INTO recipe (id, sequence_id, tool_id, algorithm_version, "
                                    "source_semantic_hash, parameters_json, targets_json) VALUES (?, ?, ?, ?, ?, ?, ?)");

    const auto run = [](SQLite::Statement& statement) {
        statement.exec();
        statement.reset();
        statement.clearBindings();
    };

    for (std::size_t sequence_index = 0; sequence_index < document.sequences.size(); ++sequence_index) {
        const auto& sequence = document.sequences[sequence_index];
        bind_uuid(insert_sequence, 1, sequence.id.uuid());
        insert_sequence.bind(2, static_cast<std::int64_t>(sequence_index));
        insert_sequence.bind(3, sequence.name);
        if (sequence.division.is_ppq()) {
            insert_sequence.bind(4, "ppq");
            insert_sequence.bind(5, static_cast<std::int64_t>(sequence.division.ppq));
            insert_sequence.bind(6);
            insert_sequence.bind(7);
        } else {
            insert_sequence.bind(4, "smpte");
            insert_sequence.bind(5);
            insert_sequence.bind(6, static_cast<std::int64_t>(sequence.division.smpte_format));
            insert_sequence.bind(7, static_cast<std::int64_t>(sequence.division.ticks_per_frame));
        }
        insert_sequence.bind(8, sequence.end_tick);
        insert_sequence.bind(9, static_cast<std::int64_t>(sequence.root_pc));
        insert_sequence.bind(10, static_cast<std::int64_t>(sequence.scale_mask));
        insert_sequence.bind(11, sequence_settings_to_json(sequence.settings));
        run(insert_sequence);

        for (std::size_t track_index = 0; track_index < sequence.tracks.size(); ++track_index) {
            const auto& track = sequence.tracks[track_index];
            bind_uuid(insert_track, 1, track.id.uuid());
            bind_uuid(insert_track, 2, sequence.id.uuid());
            insert_track.bind(3, static_cast<std::int64_t>(track_index));
            insert_track.bind(4, track.name);
            insert_track.bind(5, static_cast<std::int64_t>(track.color_rgba));
            insert_track.bind(6, track.end_tick);
            insert_track.bind(7, static_cast<std::int64_t>(track.muted ? 1 : 0));
            insert_track.bind(8, static_cast<std::int64_t>(track.solo ? 1 : 0));
            insert_track.bind(9, static_cast<std::int64_t>(track.is_drum ? 1 : 0));
            insert_track.bind(10, routing_to_json(track.routing));
            insert_track.bind(11, instrument_to_json(track.instrument));
            run(insert_track);

            for (const auto& event : track.events) {
                bind_uuid(insert_event, 1, event.id.uuid());
                bind_uuid(insert_event, 2, track.id.uuid());
                insert_event.bind(3, event.tick);
                insert_event.bind(4, static_cast<std::int64_t>(event.order_key));
                insert_event.bind(5, category_text(event.category));
                insert_event.bind(6, static_cast<std::int64_t>(event.code));
                bind_bytes(insert_event, 7, event.payload.bytes());
                insert_event.bind(8, static_cast<std::int64_t>(event.flags));
                run(insert_event);
            }
        }

        // note_link is the validated projection of core FIFO pairing; the link id is the note-on id.
        const NotePairing pairing = pair_notes(sequence);
        for (const auto& note : pairing.notes) {
            bind_uuid(insert_link, 1, note.on_id.uuid());
            bind_uuid(insert_link, 2, note.on_id.uuid());
            if (note.off_id)
                bind_uuid(insert_link, 3, note.off_id->uuid());
            else
                insert_link.bind(3);
            insert_link.bind(4, std::string(note_pairing_policy));
            insert_link.bind(5, static_cast<std::int64_t>(note.flags));
            run(insert_link);
        }

        for (const auto& region : sequence.regions) {
            bind_uuid(insert_region, 1, region.id.uuid());
            bind_uuid(insert_region, 2, sequence.id.uuid());
            insert_region.bind(3, region.name);
            insert_region.bind(4, region.start_tick);
            insert_region.bind(5, region.end_tick);
            insert_region.bind(6, region_tracks_to_json(region.tracks));
            insert_region.bind(7, static_cast<std::int64_t>(region.is_loop ? 1 : 0));
            run(insert_region);
        }
        for (const auto& region : sequence.scale_regions) {
            bind_uuid(insert_scale, 1, region.id.uuid());
            bind_uuid(insert_scale, 2, sequence.id.uuid());
            insert_scale.bind(3, region.start_tick);
            insert_scale.bind(4, region.end_tick);
            insert_scale.bind(5, static_cast<std::int64_t>(region.root_pc));
            insert_scale.bind(6, static_cast<std::int64_t>(region.scale_mask));
            insert_scale.bind(7, region.scale_name);
            insert_scale.bind(8, scale_spelling_json);
            run(insert_scale);
        }
        for (const auto& recipe : sequence.recipes) {
            bind_uuid(insert_recipe, 1, recipe.id.uuid());
            bind_uuid(insert_recipe, 2, sequence.id.uuid());
            insert_recipe.bind(3, recipe.tool_id);
            insert_recipe.bind(4, static_cast<std::int64_t>(recipe.algorithm_version));
            bind_bytes(insert_recipe, 5, recipe.source_semantic_hash);
            insert_recipe.bind(6, recipe.parameters_json);
            insert_recipe.bind(7, recipe.targets_json);
            run(insert_recipe);
        }
    }

    {
        SQLite::Statement insert_take(database,
                                      "INSERT INTO recording_take (id, track_id, name, created_ms, start_tick, end_tick, "
                                      "status, capture_json) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
        SQLite::Statement insert_chunk(database,
                                       "INSERT INTO take_chunk (take_id, ordinal, codec_version, first_time_ns, sha256, "
                                       "payload) VALUES (?, ?, ?, ?, ?, ?)");
        for (const auto& take : document.takes) {
            bind_uuid(insert_take, 1, take.id.uuid());
            bind_uuid(insert_take, 2, take.track.uuid());
            insert_take.bind(3, take.name);
            insert_take.bind(4, take.created_ms);
            insert_take.bind(5, take.start_tick);
            insert_take.bind(6, take.end_tick);
            insert_take.bind(7, take_status_text(take.status));
            insert_take.bind(8, take.capture_json);
            run(insert_take);
            for (const auto& chunk : take.chunks) {
                bind_uuid(insert_chunk, 1, take.id.uuid());
                insert_chunk.bind(2, static_cast<std::int64_t>(chunk.ordinal));
                insert_chunk.bind(3, take_chunk_codec_version);
                insert_chunk.bind(4, chunk.first_time_ns);
                bind_bytes(insert_chunk, 5, sha256(chunk.payload));
                bind_bytes(insert_chunk, 6, chunk.payload);
                run(insert_chunk);
            }
        }
    }

    {
        SQLite::Statement insert_source(database,
                                        "INSERT INTO source_blob (id, sha256, original_name, media_type, bytes, "
                                        "import_report_json) VALUES (?, ?, ?, ?, ?, ?)");
        for (const auto& source : document.sources) {
            bind_uuid(insert_source, 1, source.id);
            bind_bytes(insert_source, 2, source.sha256);
            insert_source.bind(3, source.original_name);
            insert_source.bind(4, source.media_type);
            bind_bytes(insert_source, 5, *source.bytes);
            insert_source.bind(6, source.import_report_json);
            run(insert_source);
        }
    }

    {
        SQLite::Statement insert_setting(
            database, "INSERT INTO document_setting (key, schema_version, value_json) VALUES (?, ?, ?)");
        for (const auto& [key, value] : document.settings) {
            insert_setting.bind(1, key);
            insert_setting.bind(2, static_cast<std::int64_t>(document_setting_schema_version));
            insert_setting.bind(3, value);
            run(insert_setting);
        }
    }

    database.exec(std::string(event_index_ddl));
    transaction.commit();
}

/// Builds a complete native database on an open, empty connection and checks it.
Status build_database(SQLite::Database& database, const DocumentData& document) {
    database.exec("PRAGMA journal_mode = DELETE");
    database.exec("PRAGMA synchronous = FULL");
    write_tables(database, document);
    return check_database_integrity(database, false);
}

// --- Reader -------------------------------------------------------------------------------------

/// Reads an integer column and checks that it lies in [minimum, maximum].
Result<std::int64_t> column_integer(const SQLite::Statement& statement, int index, std::int64_t minimum,
                                    std::int64_t maximum, std::string_view what) {
    const SQLite::Column column = statement.getColumn(index);
    if (!column.isInteger())
        return make_error(ErrorCode::invalid_data, std::string(what) + " is not an integer");
    const std::int64_t value = column.getInt64();
    if (value < minimum || value > maximum)
        return make_error(ErrorCode::invalid_data, std::string(what) + " is out of range");
    return value;
}

/// Reads a TEXT column (NULL is rejected).
Result<std::string> column_text(const SQLite::Statement& statement, int index, std::string_view what) {
    const SQLite::Column column = statement.getColumn(index);
    if (!column.isText())
        return make_error(ErrorCode::invalid_data, std::string(what) + " is not text");
    return column.getString();
}

constexpr std::int64_t int64_max = std::numeric_limits<std::int64_t>::max();

/// Shorthand: evaluates a Result expression, propagating its error, else yields the value.
#define MWB_DOCUMENT_ASSIGN(target, expression)                                                                      \
    auto target##_result_ = (expression);                                                                             \
    if (!target##_result_)                                                                                            \
        return std::unexpected(std::move(target##_result_).error());                                                  \
    auto target = std::move(*target##_result_)

/// Row of note_link as stored.
struct StoredNoteLink {
    Uuid id;
    std::optional<Uuid> off;
    std::string policy;
    std::int64_t flags = 0;
};

/// Reads and validates every table into a document. Throws SQLite::Exception for SQLite errors.
Result<NativeOpenResult> read_document(SQLite::Database& database, const NativeLoadLimits& limits) {
    // Identity and version (sections 23.1 and 23.4).
    if (pragma_integer(database, "application_id") != native_application_id)
        return make_error(ErrorCode::invalid_data, "not a MIDI Workbench native document (wrong application id)");
    MWB_DOCUMENT_ASSIGN(access, migration::classify_version(migration::SchemaFamily::native,
                                                            pragma_integer(database, "user_version")));
    if (access != migration::SchemaAccess::read_write)
        return make_error(ErrorCode::unsupported, "native document needs a schema migration");
    MWB_TRY(verify_schema_whitelist(database, schemas::native_v1, "native document"));
    MWB_TRY(check_database_integrity(database, true));

    NativeOpenResult result;
    DocumentData& document = result.document;

    // Bounded counts before anything large is read. NOT INDEXED forces a scan of the table
    // itself: quick_check does not verify that index content matches table content, so a count
    // answered from a damaged index would be no cross-check for rows read through an index.
    const auto count_rows = [&](const char* table) {
        SQLite::Statement statement(database, std::string("SELECT count(*) FROM ") + table + " NOT INDEXED");
        statement.executeStep();
        return statement.getColumn(0).getInt64();
    };
    const auto event_count = count_rows("midi_event");
    if (static_cast<std::uint64_t>(event_count) > limits.max_stored_events)
        return make_error(ErrorCode::limit_exceeded, "native document has more events than the load limit");
    if (static_cast<std::uint64_t>(count_rows("sequence")) > limits.document.max_sequences)
        return make_error(ErrorCode::limit_exceeded, "native document has too many sequences");
    if (count_rows("note_link") > event_count)
        return make_error(ErrorCode::invalid_data, "native document has more note links than events");
    for (const auto& [table, cap] : {std::pair{"region", max_region_rows}, std::pair{"scale_region", max_region_rows},
                                     std::pair{"recipe", max_region_rows}, std::pair{"recording_take", max_take_rows},
                                     std::pair{"take_chunk", max_take_chunk_rows},
                                     std::pair{"source_blob", max_source_rows}})
        if (count_rows(table) > cap)
            return make_error(ErrorCode::limit_exceeded, std::string("native document has too many rows in ") + table);

    // Project row.
    {
        SQLite::Statement statement(database, "SELECT uuid, title, created_ms, modified_ms, generation, metadata_json "
                                              "FROM project");
        if (!statement.executeStep())
            return make_error(ErrorCode::invalid_data, "native document has no project row");
        MWB_DOCUMENT_ASSIGN(identity, column_uuid(statement, 0, "project uuid"));
        MWB_DOCUMENT_ASSIGN(title, column_text(statement, 1, "project title"));
        MWB_DOCUMENT_ASSIGN(created, column_integer(statement, 2, 0, int64_max, "project created_ms"));
        MWB_DOCUMENT_ASSIGN(modified, column_integer(statement, 3, 0, int64_max, "project modified_ms"));
        MWB_DOCUMENT_ASSIGN(generation, column_integer(statement, 4, 0, int64_max, "project generation"));
        MWB_DOCUMENT_ASSIGN(metadata, column_text(statement, 5, "project metadata"));
        if (!is_json_object(metadata))
            return make_error(ErrorCode::invalid_data, "project metadata is not a JSON object");
        document.id = DocumentId(identity);
        document.title = std::move(title);
        document.created_ms = created;
        document.modified_ms = modified;
        document.generation = static_cast<Generation>(generation);
        document.metadata_json = std::move(metadata);
        if (statement.executeStep())
            return make_error(ErrorCode::invalid_data, "native document has more than one project row");
    }

    // Sequences.
    std::map<Uuid, std::size_t> sequence_by_id;
    {
        SQLite::Statement statement(database, "SELECT id, name, division, ppq, fps_code, ticks_per_frame, end_tick, "
                                              "root_pc, scale_mask, settings_json FROM sequence ORDER BY sort_order");
        while (statement.executeStep()) {
            Sequence sequence;
            MWB_DOCUMENT_ASSIGN(identity, column_uuid(statement, 0, "sequence id"));
            MWB_DOCUMENT_ASSIGN(name, column_text(statement, 1, "sequence name"));
            MWB_DOCUMENT_ASSIGN(division, column_text(statement, 2, "sequence division"));
            sequence.id = SequenceId(identity);
            sequence.name = std::move(name);
            if (division == "ppq") {
                MWB_DOCUMENT_ASSIGN(ppq, column_integer(statement, 3, 1, 32767, "sequence ppq"));
                sequence.division = Division::make_ppq(static_cast<std::int32_t>(ppq));
            } else if (division == "smpte") {
                MWB_DOCUMENT_ASSIGN(fps, column_integer(statement, 4, -30, -24, "sequence fps_code"));
                MWB_DOCUMENT_ASSIGN(ticks, column_integer(statement, 5, 1, 255, "sequence ticks_per_frame"));
                if (fps != -24 && fps != -25 && fps != -29 && fps != -30)
                    return make_error(ErrorCode::invalid_data, "sequence SMPTE frame code is invalid");
                sequence.division =
                    Division::make_smpte(static_cast<SmpteFormat>(static_cast<std::int8_t>(fps)), static_cast<std::int32_t>(ticks));
            } else {
                return make_error(ErrorCode::invalid_data, "sequence division is neither ppq nor smpte");
            }
            MWB_DOCUMENT_ASSIGN(end_tick, column_integer(statement, 6, 0, int64_max, "sequence end_tick"));
            MWB_DOCUMENT_ASSIGN(root, column_integer(statement, 7, 0, 11, "sequence root_pc"));
            MWB_DOCUMENT_ASSIGN(mask, column_integer(statement, 8, 1, 4095, "sequence scale_mask"));
            MWB_DOCUMENT_ASSIGN(settings_text, column_text(statement, 9, "sequence settings"));
            MWB_DOCUMENT_ASSIGN(settings, sequence_settings_from_json(settings_text));
            sequence.end_tick = end_tick;
            sequence.root_pc = static_cast<std::uint8_t>(root);
            sequence.scale_mask = static_cast<std::uint16_t>(mask);
            sequence.settings = settings;
            if (!sequence_by_id.emplace(identity, document.sequences.size()).second)
                return make_error(ErrorCode::invalid_data, "duplicate sequence identity");
            document.sequences.push_back(std::move(sequence));
        }
    }

    // Tracks and events.
    {
        SQLite::Statement select_tracks(database,
                                        "SELECT id, name, color_rgba, end_tick, muted, solo, is_drum, routing_json, "
                                        "instrument_json FROM track WHERE sequence_id = ? ORDER BY sort_order");
        SQLite::Statement select_events(database, "SELECT id, tick, order_key, category, code, payload, flags FROM "
                                                  "midi_event WHERE track_id = ? ORDER BY tick, order_key");
        std::uint64_t tracks_seen = 0;
        std::int64_t events_seen = 0;
        for (auto& sequence : document.sequences) {
            select_tracks.reset();
            bind_uuid(select_tracks, 1, sequence.id.uuid());
            while (select_tracks.executeStep()) {
                if (sequence.tracks.size() >= limits.document.max_tracks_per_sequence)
                    return make_error(ErrorCode::limit_exceeded, "native document has too many tracks in a sequence");
                Track track;
                MWB_DOCUMENT_ASSIGN(identity, column_uuid(select_tracks, 0, "track id"));
                MWB_DOCUMENT_ASSIGN(name, column_text(select_tracks, 1, "track name"));
                MWB_DOCUMENT_ASSIGN(color, column_integer(select_tracks, 2, 0, 0xFFFFFFFFll, "track color"));
                MWB_DOCUMENT_ASSIGN(end_tick, column_integer(select_tracks, 3, 0, int64_max, "track end_tick"));
                MWB_DOCUMENT_ASSIGN(muted, column_integer(select_tracks, 4, 0, 1, "track muted"));
                MWB_DOCUMENT_ASSIGN(solo, column_integer(select_tracks, 5, 0, 1, "track solo"));
                MWB_DOCUMENT_ASSIGN(drum, column_integer(select_tracks, 6, 0, 1, "track is_drum"));
                MWB_DOCUMENT_ASSIGN(routing_text, column_text(select_tracks, 7, "track routing"));
                MWB_DOCUMENT_ASSIGN(instrument_text, column_text(select_tracks, 8, "track instrument"));
                MWB_DOCUMENT_ASSIGN(routing, routing_from_json(routing_text));
                MWB_DOCUMENT_ASSIGN(instrument, instrument_from_json(instrument_text));
                track.id = TrackId(identity);
                track.name = std::move(name);
                track.color_rgba = static_cast<std::uint32_t>(color);
                track.end_tick = end_tick;
                track.muted = muted == 1;
                track.solo = solo == 1;
                track.is_drum = drum == 1;
                track.routing = std::move(routing);
                track.instrument = std::move(instrument);

                select_events.reset();
                bind_uuid(select_events, 1, track.id.uuid());
                while (select_events.executeStep()) {
                    Event event;
                    MWB_DOCUMENT_ASSIGN(event_id, column_uuid(select_events, 0, "event id"));
                    MWB_DOCUMENT_ASSIGN(tick, column_integer(select_events, 1, 0, int64_max, "event tick"));
                    MWB_DOCUMENT_ASSIGN(order_key, column_integer(select_events, 2, 0, int64_max, "event order_key"));
                    MWB_DOCUMENT_ASSIGN(category_name, column_text(select_events, 3, "event category"));
                    MWB_DOCUMENT_ASSIGN(code, column_integer(select_events, 4, 0, 255, "event code"));
                    MWB_DOCUMENT_ASSIGN(flags, column_integer(select_events, 6, 0, 0xFFFFFFFFll, "event flags"));
                    const auto category = category_from(category_name);
                    if (!category)
                        return make_error(ErrorCode::invalid_data, "event category is unknown");
                    if (!select_events.getColumn(5).isBlob())
                        return make_error(ErrorCode::invalid_data, "event payload is not a BLOB");
                    const auto payload = column_bytes(select_events, 5);
                    if (payload.size() > limits.document.max_payload_bytes)
                        return make_error(ErrorCode::limit_exceeded, "event payload exceeds the size limit");
                    event.id = EventId(event_id);
                    event.tick = tick;
                    event.order_key = static_cast<std::uint64_t>(order_key);
                    event.category = *category;
                    event.code = static_cast<std::uint8_t>(code);
                    event.flags = static_cast<std::uint32_t>(flags);
                    event.payload = Payload(payload);
                    track.events.push_back(std::move(event));
                    ++events_seen;
                }
                sequence.tracks.push_back(std::move(track));
                ++tracks_seen;
            }
        }
        if (tracks_seen != static_cast<std::uint64_t>(count_rows("track")))
            return make_error(ErrorCode::invalid_data, "a track belongs to no sequence or the track index is damaged");
        // Events are read through the (track, tick, order) index: a damaged index that lacks rows
        // would otherwise open the document without them, and the next save would drop them.
        if (events_seen != event_count)
            return make_error(ErrorCode::invalid_data,
                              "native document index does not match its events (an event belongs to no track or the "
                              "event index is damaged)");
    }

    // Regions, scale regions and recipes, in stored (rowid) order.
    {
        SQLite::Statement statement(database, "SELECT id, sequence_id, name, start_tick, end_tick, tracks_json, is_loop "
                                              "FROM region ORDER BY rowid");
        while (statement.executeStep()) {
            Region region;
            MWB_DOCUMENT_ASSIGN(identity, column_uuid(statement, 0, "region id"));
            MWB_DOCUMENT_ASSIGN(sequence_id, column_uuid(statement, 1, "region sequence"));
            MWB_DOCUMENT_ASSIGN(name, column_text(statement, 2, "region name"));
            MWB_DOCUMENT_ASSIGN(start, column_integer(statement, 3, 0, int64_max, "region start"));
            MWB_DOCUMENT_ASSIGN(end, column_integer(statement, 4, 1, int64_max, "region end"));
            MWB_DOCUMENT_ASSIGN(tracks_text, column_text(statement, 5, "region tracks"));
            MWB_DOCUMENT_ASSIGN(loop, column_integer(statement, 6, 0, 1, "region is_loop"));
            MWB_DOCUMENT_ASSIGN(tracks, region_tracks_from_json(tracks_text));
            const auto owner = sequence_by_id.find(sequence_id);
            if (owner == sequence_by_id.end())
                return make_error(ErrorCode::invalid_data, "region refers to a missing sequence");
            region.id = RegionId(identity);
            region.name = std::move(name);
            region.start_tick = start;
            region.end_tick = end;
            region.tracks = std::move(tracks);
            region.is_loop = loop == 1;
            document.sequences[owner->second].regions.push_back(std::move(region));
        }
    }
    {
        SQLite::Statement statement(database, "SELECT id, sequence_id, start_tick, end_tick, root_pc, scale_mask, "
                                              "scale_name, spelling_json FROM scale_region ORDER BY rowid");
        while (statement.executeStep()) {
            ScaleRegion region;
            MWB_DOCUMENT_ASSIGN(identity, column_uuid(statement, 0, "scale region id"));
            MWB_DOCUMENT_ASSIGN(sequence_id, column_uuid(statement, 1, "scale region sequence"));
            MWB_DOCUMENT_ASSIGN(start, column_integer(statement, 2, 0, int64_max, "scale region start"));
            MWB_DOCUMENT_ASSIGN(end, column_integer(statement, 3, 1, int64_max, "scale region end"));
            MWB_DOCUMENT_ASSIGN(root, column_integer(statement, 4, 0, 11, "scale region root"));
            MWB_DOCUMENT_ASSIGN(mask, column_integer(statement, 5, 1, 4095, "scale region mask"));
            MWB_DOCUMENT_ASSIGN(name, column_text(statement, 6, "scale region name"));
            MWB_DOCUMENT_ASSIGN(spelling, column_text(statement, 7, "scale region spelling"));
            if (!is_json_object(spelling))
                return make_error(ErrorCode::invalid_data, "scale region spelling is not a JSON object");
            if (spelling != scale_spelling_json) {
                result.read_only = true;
                result.diagnostics.push_back(Diagnostic{Severity::warning, "native.scale_spelling",
                                                        "scale spelling data cannot be preserved by this version", -1,
                                                        -1, -1});
            }
            const auto owner = sequence_by_id.find(sequence_id);
            if (owner == sequence_by_id.end())
                return make_error(ErrorCode::invalid_data, "scale region refers to a missing sequence");
            region.id = RegionId(identity);
            region.start_tick = start;
            region.end_tick = end;
            region.root_pc = static_cast<std::uint8_t>(root);
            region.scale_mask = static_cast<std::uint16_t>(mask);
            region.scale_name = std::move(name);
            document.sequences[owner->second].scale_regions.push_back(std::move(region));
        }
    }
    {
        SQLite::Statement statement(database, "SELECT id, sequence_id, tool_id, algorithm_version, source_semantic_hash, "
                                              "parameters_json, targets_json FROM recipe ORDER BY rowid");
        while (statement.executeStep()) {
            Recipe recipe;
            MWB_DOCUMENT_ASSIGN(identity, column_uuid(statement, 0, "recipe id"));
            MWB_DOCUMENT_ASSIGN(sequence_id, column_uuid(statement, 1, "recipe sequence"));
            MWB_DOCUMENT_ASSIGN(tool, column_text(statement, 2, "recipe tool"));
            MWB_DOCUMENT_ASSIGN(version, column_integer(statement, 3, 1, std::numeric_limits<std::int32_t>::max(),
                                                        "recipe algorithm version"));
            MWB_DOCUMENT_ASSIGN(hash, column_digest(statement, 4, "recipe source hash"));
            MWB_DOCUMENT_ASSIGN(parameters, column_text(statement, 5, "recipe parameters"));
            MWB_DOCUMENT_ASSIGN(targets, column_text(statement, 6, "recipe targets"));
            if (!is_json_object(parameters) || !is_json_value(targets))
                return make_error(ErrorCode::invalid_data, "recipe JSON is malformed");
            const auto owner = sequence_by_id.find(sequence_id);
            if (owner == sequence_by_id.end())
                return make_error(ErrorCode::invalid_data, "recipe refers to a missing sequence");
            recipe.id = RecipeId(identity);
            recipe.tool_id = std::move(tool);
            recipe.algorithm_version = static_cast<std::int32_t>(version);
            recipe.source_semantic_hash = hash;
            recipe.parameters_json = std::move(parameters);
            recipe.targets_json = std::move(targets);
            document.sequences[owner->second].recipes.push_back(std::move(recipe));
        }
    }

    // Note links must equal the core pairing projection exactly.
    {
        std::unordered_map<Uuid, StoredNoteLink, UuidHash> links;
        SQLite::Statement statement(database, "SELECT id, on_event_id, off_event_id, pairing_policy, flags FROM note_link");
        while (statement.executeStep()) {
            StoredNoteLink link;
            MWB_DOCUMENT_ASSIGN(identity, column_uuid(statement, 0, "note link id"));
            MWB_DOCUMENT_ASSIGN(on_id, column_uuid(statement, 1, "note link on event"));
            if (!statement.getColumn(2).isNull()) {
                MWB_DOCUMENT_ASSIGN(off_id, column_uuid(statement, 2, "note link off event"));
                link.off = off_id;
            }
            MWB_DOCUMENT_ASSIGN(policy, column_text(statement, 3, "note link policy"));
            MWB_DOCUMENT_ASSIGN(flags, column_integer(statement, 4, 0, 0xFFFFFFFFll, "note link flags"));
            link.id = identity;
            link.policy = std::move(policy);
            link.flags = flags;
            if (!links.emplace(on_id, std::move(link)).second)
                return make_error(ErrorCode::invalid_data, "duplicate note link");
        }
        std::size_t matched = 0;
        for (const auto& sequence : document.sequences) {
            const NotePairing pairing = pair_notes(sequence);
            for (const auto& note : pairing.notes) {
                const auto found = links.find(note.on_id.uuid());
                if (found == links.end())
                    return make_error(ErrorCode::invalid_data, "note link projection is missing a note");
                const StoredNoteLink& link = found->second;
                const std::optional<Uuid> expected_off =
                    note.off_id ? std::optional<Uuid>(note.off_id->uuid()) : std::nullopt;
                if (link.id != note.on_id.uuid() || link.off != expected_off || link.policy != note_pairing_policy ||
                    link.flags != static_cast<std::int64_t>(note.flags))
                    return make_error(ErrorCode::invalid_data, "note link projection is inconsistent with the events");
                ++matched;
            }
        }
        if (matched != links.size())
            return make_error(ErrorCode::invalid_data, "note link projection contains links without notes");
        if (static_cast<std::int64_t>(links.size()) != count_rows("note_link"))
            return make_error(ErrorCode::invalid_data, "native document index does not match its note links");
    }

    // Recording takes and chunks.
    {
        SQLite::Statement select_takes(database, "SELECT id, track_id, name, created_ms, start_tick, end_tick, status, "
                                                 "capture_json FROM recording_take ORDER BY rowid");
        SQLite::Statement select_chunks(database, "SELECT ordinal, codec_version, first_time_ns, sha256, payload FROM "
                                                  "take_chunk WHERE take_id = ? ORDER BY ordinal");
        std::int64_t chunks_seen = 0;
        while (select_takes.executeStep()) {
            RecordingTake take;
            MWB_DOCUMENT_ASSIGN(identity, column_uuid(select_takes, 0, "take id"));
            MWB_DOCUMENT_ASSIGN(track_id, column_uuid(select_takes, 1, "take track"));
            MWB_DOCUMENT_ASSIGN(name, column_text(select_takes, 2, "take name"));
            MWB_DOCUMENT_ASSIGN(created, column_integer(select_takes, 3, std::numeric_limits<std::int64_t>::min(),
                                                        int64_max, "take created_ms"));
            MWB_DOCUMENT_ASSIGN(start, column_integer(select_takes, 4, 0, int64_max, "take start"));
            MWB_DOCUMENT_ASSIGN(end, column_integer(select_takes, 5, start, int64_max, "take end"));
            MWB_DOCUMENT_ASSIGN(status_text, column_text(select_takes, 6, "take status"));
            MWB_DOCUMENT_ASSIGN(capture, column_text(select_takes, 7, "take capture"));
            const auto status = take_status_from(status_text);
            if (!status)
                return make_error(ErrorCode::invalid_data, "take status is unknown");
            if (!is_json_object(capture))
                return make_error(ErrorCode::invalid_data, "take capture JSON is not an object");
            take.id = TakeId(identity);
            take.track = TrackId(track_id);
            if (!find_track(document, take.track))
                return make_error(ErrorCode::invalid_data, "take refers to a missing track");
            take.name = std::move(name);
            take.created_ms = created;
            take.start_tick = start;
            take.end_tick = end;
            take.status = *status;
            take.capture_json = std::move(capture);

            select_chunks.reset();
            bind_uuid(select_chunks, 1, take.id.uuid());
            while (select_chunks.executeStep()) {
                TakeChunk chunk;
                MWB_DOCUMENT_ASSIGN(ordinal, column_integer(select_chunks, 0, 0, std::numeric_limits<std::int32_t>::max(),
                                                            "take chunk ordinal"));
                MWB_DOCUMENT_ASSIGN(codec, column_integer(select_chunks, 1, 1, int64_max, "take chunk codec"));
                MWB_DOCUMENT_ASSIGN(first_time, column_integer(select_chunks, 2, 0, int64_max, "take chunk time"));
                MWB_DOCUMENT_ASSIGN(digest, column_digest(select_chunks, 3, "take chunk sha256"));
                if (codec != take_chunk_codec_version)
                    return make_error(ErrorCode::unsupported, "take chunk uses an unsupported codec version");
                chunk.ordinal = static_cast<std::int32_t>(ordinal);
                chunk.first_time_ns = first_time;
                chunk.payload = column_bytes(select_chunks, 4);
                if (chunk.payload.size() > max_take_chunk_bytes)
                    return make_error(ErrorCode::invalid_data, "take chunk payload is too large");
                if (sha256(chunk.payload) != digest)
                    return make_error(ErrorCode::invalid_data, "take chunk checksum mismatch");
                take.chunks.push_back(std::move(chunk));
                ++chunks_seen;
            }
            document.takes.push_back(std::move(take));
        }
        if (chunks_seen != count_rows("take_chunk"))
            return make_error(ErrorCode::invalid_data, "a take chunk belongs to no take");
    }

    // Original source bytes.
    {
        SQLite::Statement statement(database, "SELECT id, sha256, original_name, media_type, bytes, import_report_json "
                                              "FROM source_blob ORDER BY rowid");
        while (statement.executeStep()) {
            SourceBlob source;
            MWB_DOCUMENT_ASSIGN(identity, column_uuid(statement, 0, "source id"));
            MWB_DOCUMENT_ASSIGN(digest, column_digest(statement, 1, "source sha256"));
            MWB_DOCUMENT_ASSIGN(name, column_text(statement, 2, "source name"));
            MWB_DOCUMENT_ASSIGN(media, column_text(statement, 3, "source media type"));
            MWB_DOCUMENT_ASSIGN(report, column_text(statement, 5, "source import report"));
            if (media != "smf" && media != "rmid")
                return make_error(ErrorCode::invalid_data, "source media type is unknown");
            if (!is_json_object(report))
                return make_error(ErrorCode::invalid_data, "source import report is not a JSON object");
            auto bytes = std::make_shared<const std::vector<std::uint8_t>>(column_bytes(statement, 4));
            if (bytes->size() > max_source_bytes)
                return make_error(ErrorCode::invalid_data, "source bytes are too large");
            if (sha256(*bytes) != digest)
                return make_error(ErrorCode::invalid_data, "source bytes do not match their SHA-256");
            source.id = identity;
            source.sha256 = digest;
            source.original_name = std::move(name);
            source.media_type = std::move(media);
            source.bytes = std::move(bytes);
            source.import_report_json = std::move(report);
            document.sources.push_back(std::move(source));
        }
    }

    // Opaque records cannot be represented by this version's model: inspection only.
    if (const auto opaque = count_rows("opaque_record"); opaque > 0) {
        result.read_only = true;
        result.diagnostics.push_back(Diagnostic{Severity::warning, "native.opaque_records",
                                                std::to_string(opaque) +
                                                    " opaque source record(s) cannot be preserved by this version",
                                                -1, -1, -1});
    }

    // Whitelisted document settings.
    {
        SQLite::Statement statement(database, "SELECT key, schema_version, value_json FROM document_setting ORDER BY key");
        while (statement.executeStep()) {
            MWB_DOCUMENT_ASSIGN(key, column_text(statement, 0, "setting key"));
            MWB_DOCUMENT_ASSIGN(version, column_integer(statement, 1, 1, int64_max, "setting schema version"));
            MWB_DOCUMENT_ASSIGN(value, column_text(statement, 2, "setting value"));
            if (!is_allowed_document_setting(key))
                return make_error(ErrorCode::invalid_data, "document setting key is not allowed: " + key);
            if (version != document_setting_schema_version)
                return make_error(ErrorCode::unsupported, "document setting has an unsupported schema version: " + key);
            if (!is_json_value(value))
                return make_error(ErrorCode::invalid_data, "document setting value is not JSON: " + key);
            document.settings.emplace(std::move(key), std::move(value));
        }
    }

    // Domain invariants (section 18.2).
    for (auto& diagnostic : validate_document(document, limits.document)) {
        if (diagnostic.severity == Severity::error)
            return make_error(ErrorCode::invalid_data,
                              "native document fails validation (" + diagnostic.code + "): " + diagnostic.message);
        if (diagnostic.code == "limit.events")
            result.read_only = true;
        result.diagnostics.push_back(std::move(diagnostic));
    }
    return result;
}

#undef MWB_DOCUMENT_ASSIGN

/// Refuses inspection-only results for the editable load functions.
Result<DocumentData> require_writable(Result<NativeOpenResult> opened) {
    if (!opened)
        return std::unexpected(std::move(opened).error());
    if (opened->read_only) {
        std::string reason = "native document can only be opened read-only";
        for (const auto& diagnostic : opened->diagnostics)
            if (diagnostic.severity == Severity::warning)
                reason += "; " + diagnostic.message;
        return make_error(ErrorCode::unsupported, reason);
    }
    return std::move(opened->document);
}

/// Checks the 100-byte SQLite header: magic string and rollback-journal file format (bytes 18
/// and 19 equal 1). A WAL-mode image would depend on -wal/-shm sidecars, which a delivered .mwb
/// must never do (section 21.3).
Status check_native_header(std::span<const std::uint8_t> header) {
    static constexpr std::string_view magic{"SQLite format 3\0", 16};
    if (header.size() < 100 || !std::equal(magic.begin(), magic.end(), header.begin(),
                                           [](char expected, std::uint8_t actual) {
                                               return static_cast<std::uint8_t>(expected) == actual;
                                           }))
        return make_error(ErrorCode::invalid_data, "not a MIDI Workbench native document (no SQLite header)");
    if (header[18] != 1 || header[19] != 1)
        return make_error(ErrorCode::invalid_data, "native document is not in rollback-journal format");
    return ok();
}

/// Removes a file, ignoring errors (cleanup of temporary files on failure paths).
void remove_quietly(const std::filesystem::path& path) {
    std::error_code error;
    // Best-effort cleanup; the primary error is reported to the caller.
    std::ignore = std::filesystem::remove(path, error);
}

/// Removes temporary files an interrupted save of `target` left behind ("<name>.~*.mwb-tmp"
/// and their "-journal") once they are older than an hour: a younger one may belong to a save
/// another instance is running right now.
void remove_stale_temporaries(const std::filesystem::path& target) {
    const std::u8string prefix =
        target.filename().u8string() + std::u8string(temporary_marker.begin(), temporary_marker.end());
    const std::u8string suffix(temporary_suffix.begin(), temporary_suffix.end());
    const std::u8string journal_suffix = suffix + u8"-journal";
    const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    std::error_code error;
    std::filesystem::directory_iterator entries(target.parent_path(), error);
    if (error)
        return;
    for (const auto& entry : entries) {
        const std::u8string name = entry.path().filename().u8string();
        if (!name.starts_with(prefix) || !(name.ends_with(suffix) || name.ends_with(journal_suffix)))
            continue;
        std::error_code time_error;
        const auto written = std::filesystem::last_write_time(entry.path(), time_error);
        if (!time_error && written < cutoff)
            remove_quietly(entry.path());
    }
}

} // namespace

std::span<const std::string_view> document_setting_keys() { return allowed_setting_keys; }

bool is_allowed_document_setting(std::string_view key) {
    return std::find(allowed_setting_keys.begin(), allowed_setting_keys.end(), key) != allowed_setting_keys.end();
}

Status save_native(const DocumentData& document, const std::filesystem::path& path) {
    MWB_TRY(check_writable(document));
    if (path.empty() || !path.has_filename())
        return make_error(ErrorCode::invalid_argument, "save path has no file name");
    std::error_code error;
    const std::filesystem::path target = std::filesystem::absolute(path, error);
    if (error)
        return make_error(ErrorCode::io, "cannot resolve the save path: " + error.message());
    const std::filesystem::path directory = target.parent_path();
    if (!std::filesystem::is_directory(directory, error))
        return make_error(ErrorCode::io, "the destination directory does not exist");
    if (std::filesystem::is_directory(target, error))
        return make_error(ErrorCode::io, "the destination is a directory");

    remove_stale_temporaries(target);
    // Unique temporary file next to the target so the final rename stays on one volume. The
    // suffix is short (19 characters) so a target that fits MAX_PATH leaves room for it.
    std::filesystem::path temporary = target;
    temporary += std::filesystem::path(std::string(temporary_marker) + Uuid::generate().to_string().substr(0, 8) +
                                       std::string(temporary_suffix));
    std::filesystem::path journal = temporary;
    journal += "-journal";

    auto status = guarded("writing the temporary native file", [&]() -> Status {
        SQLite::Database database(path_to_utf8(temporary), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        MWB_TRY(build_database(database, document));
        return ok();
    }); // The connection is closed here, before the flush and the replacement.
    if (status)
        status = file_system::flush_file(temporary);
    if (status)
        status = file_system::replace_file(temporary, target);
    if (!status) {
        remove_quietly(temporary);
        remove_quietly(journal);
        return std::unexpected(status.error().with_context("saving native document"));
    }
    return ok();
}

Result<std::vector<std::uint8_t>> native_to_bytes(const DocumentData& document) {
    MWB_TRY(check_writable(document));
    return guarded("serializing the native document", [&]() -> Result<std::vector<std::uint8_t>> {
        SQLite::Database database(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        write_tables(database, document);
        // This code just wrote the image: the quick check (structure, NOT NULL and CHECK) and the
        // foreign key check suffice; the full integrity check stays in save_native().
        MWB_TRY(check_database_integrity(database, true));
        return serialize_database(database);
    });
}

Result<NativeOpenResult> open_native(const std::filesystem::path& path, const NativeLoadLimits& limits) {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error || !std::filesystem::exists(status))
        return make_error(ErrorCode::not_found, "native document does not exist");
    if (!std::filesystem::is_regular_file(status))
        return make_error(ErrorCode::invalid_argument, "native document path is not a regular file");
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return make_error(ErrorCode::io, "cannot read the native document size: " + error.message());
    if (size > limits.max_file_bytes)
        return make_error(ErrorCode::limit_exceeded, "native document exceeds the file size limit");
    {
        std::array<std::uint8_t, 100> header{};
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return make_error(ErrorCode::io, "cannot open the native document");
        stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
        if (stream.gcount() != static_cast<std::streamsize>(header.size()))
            return make_error(ErrorCode::invalid_data, "native document is too small");
        MWB_TRY(check_native_header(header));
    }
    return guarded("reading the native document", [&]() -> Result<NativeOpenResult> {
        SQLite::Database database(path_to_utf8(path), SQLite::OPEN_READONLY);
        MWB_TRY(harden_untrusted_connection(database, limits.max_blob_bytes));
        return read_document(database, limits);
    });
}

Result<NativeOpenResult> open_native_bytes(std::span<const std::uint8_t> bytes, const NativeLoadLimits& limits) {
    if (bytes.size() > limits.max_file_bytes)
        return make_error(ErrorCode::limit_exceeded, "native document exceeds the file size limit");
    if (bytes.size() < 512)
        return make_error(ErrorCode::invalid_data, "native document image is too small");
    MWB_TRY(check_native_header(bytes.first(100)));
    return guarded("reading the native document image", [&]() -> Result<NativeOpenResult> {
        SQLite::Database database(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        MWB_TRY(harden_untrusted_connection(database, limits.max_blob_bytes));
        MWB_TRY(deserialize_read_only(database, bytes));
        return read_document(database, limits);
    });
}

Result<DocumentData> load_native(const std::filesystem::path& path, const NativeLoadLimits& limits) {
    return require_writable(open_native(path, limits));
}

Result<DocumentData> native_from_bytes(std::span<const std::uint8_t> bytes, const NativeLoadLimits& limits) {
    return require_writable(open_native_bytes(bytes, limits));
}

} // namespace mwb::document
