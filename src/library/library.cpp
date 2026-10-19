// Managed library: catalog transactions, import, revisions, organization, trash, change outbox,
// search execution and derived-index writers (specification sections 19.4, 22-27).

#include "mwb/library/library.hpp"

#include "mwb/core/random.hpp"
#include "mwb/core/text.hpp"
#include "mwb/document/native_format.hpp"
#include "mwb/document/sqlite_support.hpp"
#include "mwb/library/analysis.hpp"
#include "mwb/library/catalog_connection.hpp"
#include "mwb/library/storage.hpp"

#include <SQLiteCpp/Transaction.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <shared_mutex>
#include <atomic>
#include <condition_variable>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <set>

namespace mwb::library {

namespace sqlite_support = document::sqlite_support;
using Json = nlohmann::json;
using SQLite::Statement;

namespace {

constexpr std::string_view catalog_file_name = "library.sqlite";
constexpr std::string_view descriptor_file_name = "library.json";
constexpr std::size_t change_value_limit = 256;
constexpr std::size_t change_asset_limit = 1000;

std::string truncate_for_change(std::string_view text) {
    std::size_t length = std::min(text.size(), change_value_limit);
    // Never split a UTF-8 sequence: back off over continuation bytes.
    while (length > 0 && length < text.size() && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
        --length;
    return std::string(text.substr(0, length));
}

std::string sort_key(std::string_view title) { return fold_for_search(title); }

/// The digest an object file is named after (64 lowercase or uppercase hex digits), or nullopt.
std::optional<Sha256Digest> parse_sha256_hex(std::string_view text) {
    if (text.size() != 64)
        return std::nullopt;
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    Sha256Digest digest{};
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const int high = nibble(text[2 * index]);
        const int low = nibble(text[2 * index + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        digest[index] = static_cast<std::uint8_t>(high * 16 + low);
    }
    return digest;
}

Sha256Digest analyzer_settings_hash(std::string_view analyzer, std::int32_t version) {
    Sha256Builder builder;
    builder.update_string("mwb.analysis.settings");
    builder.update_string(analyzer);
    builder.update_u32(static_cast<std::uint32_t>(version));
    return builder.finish();
}

std::optional<MediaType> parse_media_type(std::string_view text) {
    if (text == "smf")
        return MediaType::smf;
    if (text == "rmid")
        return MediaType::rmid;
    if (text == "native")
        return MediaType::native;
    return std::nullopt;
}

std::optional<RevisionOrigin> parse_origin(std::string_view text) {
    for (const auto origin : {RevisionOrigin::import, RevisionOrigin::edit, RevisionOrigin::extract, RevisionOrigin::restore})
        if (revision_origin_name(origin) == text)
            return origin;
    return std::nullopt;
}

std::optional<AssetKind> parse_kind(std::string_view text) {
    for (const auto kind : {AssetKind::song, AssetKind::phrase, AssetKind::template_asset})
        if (asset_kind_name(kind) == text)
            return kind;
    return std::nullopt;
}

std::optional<double> column_optional_double(const Statement& statement, int index) {
    const auto column = statement.getColumn(index);
    if (column.isNull())
        return std::nullopt;
    return column.getDouble();
}

std::optional<std::int64_t> column_optional_int(const Statement& statement, int index) {
    const auto column = statement.getColumn(index);
    if (column.isNull())
        return std::nullopt;
    return column.getInt64();
}

/// Removes a staging file on scope exit unless it was moved away by publication.
class StagingGuard {
public:
    explicit StagingGuard(std::filesystem::path path) : path_(std::move(path)) {}
    ~StagingGuard() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    StagingGuard(const StagingGuard&) = delete;
    StagingGuard& operator=(const StagingGuard&) = delete;

private:
    std::filesystem::path path_;
};

bool is_midi_extension(const std::filesystem::path& path) {
    const auto extension = ascii_lower(path_utf8(path.extension()));
    return extension == ".mid" || extension == ".midi" || extension == ".smf" || extension == ".kar" ||
           extension == ".rmi";
}

} // namespace

std::string_view asset_kind_name(AssetKind kind) noexcept {
    switch (kind) {
    case AssetKind::song:
        return "song";
    case AssetKind::phrase:
        return "phrase";
    case AssetKind::template_asset:
        return "template";
    }
    return "song";
}

std::string_view revision_origin_name(RevisionOrigin origin) noexcept {
    switch (origin) {
    case RevisionOrigin::import:
        return "import";
    case RevisionOrigin::edit:
        return "edit";
    case RevisionOrigin::extract:
        return "extract";
    case RevisionOrigin::restore:
        return "restore";
    }
    return "edit";
}

std::string_view media_type_name(MediaType type) noexcept {
    switch (type) {
    case MediaType::smf:
        return "smf";
    case MediaType::rmid:
        return "rmid";
    case MediaType::native:
        return "native";
    }
    return "smf";
}

std::string_view media_extension(MediaType type) noexcept {
    switch (type) {
    case MediaType::smf:
        return "mid";
    case MediaType::rmid:
        return "rmi";
    case MediaType::native:
        return "mwb";
    }
    return "mid";
}

std::string_view import_outcome_name(ImportOutcome outcome) noexcept {
    switch (outcome) {
    case ImportOutcome::imported:
        return "imported";
    case ImportOutcome::duplicate:
        return "duplicate";
    case ImportOutcome::unsupported:
        return "unsupported";
    case ImportOutcome::changed_during_read:
        return "changed_during_read";
    case ImportOutcome::failed:
        return "failed";
    case ImportOutcome::cancelled:
        return "cancelled";
    case ImportOutcome::source_changed:
        return "source_changed";
    }
    return "failed";
}

std::string ImportReport::to_json() const {
    Json object;
    object["version"] = 1;
    object["counts"] = {{"discovered", counts.discovered},
                        {"captured", counts.captured},
                        {"imported", counts.imported},
                        {"duplicate", counts.duplicate},
                        {"unsupported", counts.unsupported},
                        {"changed_during_read", counts.changed_during_read},
                        {"failed", counts.failed},
                        {"cancelled", counts.cancelled},
                        {"source_changed", counts.source_changed}};
    Json list = Json::array();
    for (const auto& item : items) {
        Json entry{{"path", item.display_path}, {"outcome", import_outcome_name(item.outcome)}, {"reason", item.reason}};
        if (item.asset_id)
            entry["asset_id"] = *item.asset_id;
        list.push_back(std::move(entry));
    }
    object["items"] = std::move(list);
    return object.dump();
}

// --- State ------------------------------------------------------------------------------------------

/// Parsed and analyzed content of one object, ready for a catalog transaction.
struct ParsedObject {
    std::string state = "ready"; ///< ready, warning, unsupported, invalid
    std::string reason;
    std::optional<DocumentAnalysis> analysis;
};

struct Library::State {
    std::filesystem::path root;
    LibraryOptions options;
    WriterLock writer_lock;
    Uuid uuid;
    ObjectStore objects;
    std::mutex writer_mutex;
    std::unique_ptr<SQLite::Database> writer;
    std::mutex readers_mutex;
    std::condition_variable readers_available;
    std::vector<std::unique_ptr<SQLite::Database>> idle_readers;
    std::size_t open_readers = 0;
    std::mutex listener_mutex;
    std::function<void(std::int64_t)> listener;
    std::int64_t pending_change = 0;
    std::atomic<int> gc_pauses{0};
    /// Publishing an object file and committing the catalog rows that reference it hold this
    /// shared; garbage collection holds it exclusively from choosing candidates to removing their
    /// files, so it can never delete a file a concurrent import or commit is about to reference
    /// again (spec 22.2, code-library-12).
    std::shared_mutex objects_lock;
    /// The section 19.4 browse index; the mutex covers catching up with the outbox and paging.
    struct BrowseCache {
        std::mutex mutex;
        std::unique_ptr<search::BrowseIndex> index; ///< Null until built, after a full reload or over budget.
        std::int64_t revision = 0;                   ///< Outbox revision the index reflects.
        bool over_budget = false;
        std::function<void()> dropped;               ///< Rebuild request after a full reload.
    } browse;

    State(std::filesystem::path library_root, LibraryOptions library_options, WriterLock acquired_lock)
        : root(std::move(library_root)), options(library_options), writer_lock(std::move(acquired_lock)), objects(root) {}

    /// A read connection borrowed from the bounded pool; returned on destruction.
    class Reader {
    public:
        Reader(State& state, std::unique_ptr<SQLite::Database> database)
            : state_(state), database_(std::move(database)) {}
        ~Reader() {
            std::lock_guard lock(state_.readers_mutex);
            state_.idle_readers.push_back(std::move(database_));
            state_.readers_available.notify_one();
        }
        Reader(const Reader&) = delete;
        Reader& operator=(const Reader&) = delete;
        SQLite::Database& operator*() { return *database_; }
        SQLite::Database* operator->() { return database_.get(); }

    private:
        State& state_;
        std::unique_ptr<SQLite::Database> database_;
    };

    Result<std::unique_ptr<Reader>> reader() {
        std::unique_lock lock(readers_mutex);
        readers_available.wait(lock, [&] { return !idle_readers.empty() || open_readers < options.read_connections; });
        if (!idle_readers.empty()) {
            auto database = std::move(idle_readers.back());
            idle_readers.pop_back();
            return std::make_unique<Reader>(*this, std::move(database));
        }
        ++open_readers;
        lock.unlock();
        auto database = open_catalog_connection(root / catalog_file_name, ConnectionRole::reader, options.busy_timeout_ms);
        if (!database) {
            std::lock_guard relock(readers_mutex);
            --open_readers;
            readers_available.notify_one();
            return std::unexpected(std::move(database).error());
        }
        return std::make_unique<Reader>(*this, std::move(*database));
    }

    /// Runs `body` inside one IMMEDIATE transaction on the writer connection and notifies the
    /// change listener only after the commit (section 22.2 step 7).
    template <class Function>
    auto write(std::string_view operation, Function&& body) -> decltype(body(std::declval<SQLite::Database&>())) {
        using ResultType = decltype(body(std::declval<SQLite::Database&>()));
        std::int64_t committed_change = 0;
        ResultType result = [&]() -> ResultType {
            std::lock_guard lock(writer_mutex);
            return sqlite_support::guarded(operation, [&]() -> ResultType {
                SQLite::Transaction transaction(*writer, SQLite::TransactionBehavior::IMMEDIATE);
                pending_change = 0;
                ResultType inner = body(*writer);
                if (!inner)
                    return inner; // The transaction destructor rolls back.
                transaction.commit();
                committed_change = pending_change;
                return inner;
            });
        }();
        if (result && committed_change != 0) {
            std::function<void(std::int64_t)> callback;
            {
                std::lock_guard lock(listener_mutex);
                callback = listener;
            }
            if (callback)
                callback(committed_change);
        }
        return result;
    }

    /// Appends one outbox entry and advances the catalog generation.
    void record_change(SQLite::Database& database, std::string_view operation, const std::vector<std::int64_t>& assets,
                       bool structure, const Json& before = Json::object(), Json after = Json::object()) {
        after["version"] = 1;
        if (assets.size() > change_asset_limit)
            after["assets"] = "many";
        else
            after["assets"] = assets;
        after["structure"] = structure;
        Statement insert(database, "INSERT INTO catalog_change(operation_uuid, created_ms, operation, schema_version, "
                                   "before_json, after_json) VALUES (?1, ?2, ?3, 1, ?4, ?5)");
        sqlite_support::bind_uuid(insert, 1, Uuid::generate());
        insert.bind(2, now_utc_ms());
        insert.bind(3, std::string(operation));
        insert.bind(4, before.dump());
        insert.bind(5, after.dump());
        insert.exec();
        pending_change = database.getLastInsertRowid();
        database.exec("UPDATE catalog SET generation = generation + 1 WHERE singleton = 1");
    }

    // --- catalog helpers (writer transaction) --------------------------------------------------

    void insert_object_row(SQLite::Database& database, const Sha256Digest& sha, MediaType media, std::uint64_t size) {
        Statement insert(database, "INSERT OR IGNORE INTO object_file(sha256, media_type, extension, byte_count, "
                                   "created_ms, verified_ms) VALUES (?1, ?2, ?3, ?4, ?5, ?5)");
        sqlite_support::bind_bytes(insert, 1, sha);
        insert.bind(2, std::string(media_type_name(media)));
        insert.bind(3, std::string(media_extension(media)));
        insert.bind(4, static_cast<std::int64_t>(size));
        insert.bind(5, now_utc_ms());
        insert.exec();
    }

    std::int64_t insert_asset(SQLite::Database& database, const std::string& title, AssetKind kind) {
        Statement insert(database, "INSERT INTO asset(uuid, title, sort_title, kind, created_ms, modified_ms) VALUES "
                                   "(?1, ?2, ?3, ?4, ?5, ?5)");
        sqlite_support::bind_uuid(insert, 1, Uuid::generate());
        insert.bind(2, title);
        insert.bind(3, sort_key(title));
        insert.bind(4, std::string(asset_kind_name(kind)));
        insert.bind(5, now_utc_ms());
        insert.exec();
        return database.getLastInsertRowid();
    }

    /// Inserts a revision without making it the head (a source-change candidate).
    std::int64_t insert_revision(SQLite::Database& database, std::int64_t asset_id, std::optional<std::int64_t> parent,
                                 const Sha256Digest& sha, RevisionOrigin origin, const std::string& label) {
        Statement ordinal(database, "SELECT coalesce(max(ordinal), 0) + 1 FROM revision WHERE asset_id = ?1");
        ordinal.bind(1, asset_id);
        ordinal.executeStep();
        const auto next = ordinal.getColumn(0).getInt64();
        Statement insert(database, "INSERT INTO revision(uuid, asset_id, ordinal, parent_id, object_sha256, created_ms, "
                                   "origin, label) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
        sqlite_support::bind_uuid(insert, 1, Uuid::generate());
        insert.bind(2, asset_id);
        insert.bind(3, next);
        if (parent)
            insert.bind(4, *parent);
        else
            insert.bind(4);
        sqlite_support::bind_bytes(insert, 5, sha);
        insert.bind(6, now_utc_ms());
        insert.bind(7, std::string(revision_origin_name(origin)));
        insert.bind(8, label);
        insert.exec();
        return database.getLastInsertRowid();
    }

    /// Derived index rows (motif grams) kept: a running count in catalog_setting, initialized
    /// from the table once, so the budget check never scans the index.
    std::int64_t derived_gram_rows(SQLite::Database& database) {
        Statement read(database, "SELECT value_json FROM catalog_setting WHERE key = 'derived.gram_rows'");
        if (read.executeStep()) {
            const auto value = Json::parse(read.getColumn(0).getString(), nullptr, false);
            if (value.is_number_integer())
                return value.get<std::int64_t>();
        }
        Statement count(database, "SELECT count(*) FROM motif_gram");
        count.executeStep();
        const auto rows = count.getColumn(0).getInt64();
        store_gram_rows(database, rows);
        return rows;
    }

    void store_gram_rows(SQLite::Database& database, std::int64_t rows) {
        Statement store(database, "INSERT OR REPLACE INTO catalog_setting(key, schema_version, value_json) VALUES "
                                  "('derived.gram_rows', 1, ?1)");
        store.bind(1, Json(std::max<std::int64_t>(0, rows)).dump());
        store.exec();
    }

    /// Deletes a revision's derived rows (fingerprints, motif streams with their grams, feature
    /// vectors) and keeps the gram count right. Derived data is disposable (section 26.6).
    void delete_derived(SQLite::Database& database, std::int64_t revision_id) {
        Statement grams(database, "SELECT count(*) FROM motif_gram WHERE stream_id IN (SELECT id FROM motif_stream WHERE "
                                  "revision_id = ?1)");
        grams.bind(1, revision_id);
        grams.executeStep();
        const auto removed = grams.getColumn(0).getInt64();
        for (const auto* table : {"fingerprint", "motif_stream", "feature_vector"}) {
            Statement clear(database, std::format("DELETE FROM {} WHERE revision_id = ?1", table));
            clear.bind(1, revision_id);
            clear.exec();
        }
        if (removed > 0)
            store_gram_rows(database, derived_gram_rows(database) - removed);
    }

    /// Applies the asset's manual key overrides to a revision's effective summaries (origin
    /// "manual"), e.g. after a restore copied another revision's estimated keys (code-library-3).
    void apply_key_overrides(SQLite::Database& database, std::int64_t asset_id, std::int64_t revision_id) {
        Statement overrides(database, "SELECT sequence_no, value_json FROM asset_override WHERE asset_id = ?1 AND "
                                      "field = 'key'");
        overrides.bind(1, asset_id);
        while (overrides.executeStep()) {
            const auto value = Json::parse(overrides.getColumn(1).getString(), nullptr, false);
            if (!value.is_object() || !value.contains("root") || !value["root"].is_number_integer() ||
                !value.contains("mode") || !value["mode"].is_string())
                continue;
            Statement update(database, "UPDATE sequence_summary SET key_root = ?1, key_mode = ?2, key_origin = 'manual', "
                                       "key_confidence = NULL WHERE revision_id = ?3 AND sequence_no = ?4");
            update.bind(1, static_cast<std::int64_t>(value["root"].get<int>()));
            update.bind(2, value["mode"].get<std::string>());
            update.bind(3, revision_id);
            update.bind(4, overrides.getColumn(0).getInt64());
            update.exec();
        }
    }

    /// Inserts a revision and makes it the asset's head. The previous head's derived index rows
    /// are dropped: only heads are searched, so they would only grow the index (code-library-7).
    std::int64_t insert_head_revision(SQLite::Database& database, std::int64_t asset_id, std::optional<std::int64_t> parent,
                                      const Sha256Digest& sha, RevisionOrigin origin, const std::string& label) {
        if (parent)
            delete_derived(database, *parent);
        Statement ordinal(database, "SELECT coalesce(max(ordinal), 0) + 1 FROM revision WHERE asset_id = ?1");
        ordinal.bind(1, asset_id);
        ordinal.executeStep();
        const auto next = ordinal.getColumn(0).getInt64();
        Statement insert(database, "INSERT INTO revision(uuid, asset_id, ordinal, parent_id, object_sha256, created_ms, "
                                   "origin, label) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
        sqlite_support::bind_uuid(insert, 1, Uuid::generate());
        insert.bind(2, asset_id);
        insert.bind(3, next);
        if (parent)
            insert.bind(4, *parent);
        else
            insert.bind(4);
        sqlite_support::bind_bytes(insert, 5, sha);
        insert.bind(6, now_utc_ms());
        insert.bind(7, std::string(revision_origin_name(origin)));
        insert.bind(8, label);
        insert.exec();
        const auto revision_id = database.getLastInsertRowid();
        Statement head(database, "UPDATE asset SET current_revision_id = ?1, modified_ms = ?2, metadata_generation = "
                                 "metadata_generation + 1 WHERE id = ?3");
        head.bind(1, revision_id);
        head.bind(2, now_utc_ms());
        head.bind(3, asset_id);
        head.exec();
        return revision_id;
    }

    /// Writes sequence/track summaries and the "basic" analysis run of a revision. User key
    /// overrides of the asset are applied to the effective summary (origin "manual").
    void write_basic_analysis(SQLite::Database& database, std::int64_t asset_id, std::int64_t revision_id,
                              const ParsedObject& parsed) {
        Json result;
        result["version"] = 1;
        result["state"] = parsed.state;
        if (!parsed.reason.empty())
            result["reason"] = parsed.reason;
        const bool failed = parsed.state == "unsupported" || parsed.state == "invalid";
        if (parsed.analysis) {
            const auto& analysis = *parsed.analysis;
            result["warnings"] = analysis.warning_count;
            result["text"] = analysis.searchable_text;
            result["text_truncated"] = analysis.searchable_text_truncated;
            Json keys = Json::array();
            std::map<std::uint32_t, std::pair<int, std::string>> overrides;
            Statement override_query(database, "SELECT sequence_no, value_json FROM asset_override WHERE asset_id = ?1 "
                                               "AND field = 'key'");
            override_query.bind(1, asset_id);
            while (override_query.executeStep()) {
                const auto value = Json::parse(override_query.getColumn(1).getString(), nullptr, false);
                if (value.is_object() && value.contains("root") && value["root"].is_number_integer() &&
                    value.contains("mode") && value["mode"].is_string())
                    overrides[static_cast<std::uint32_t>(override_query.getColumn(0).getInt64())] = {
                        value["root"].get<int>(), value["mode"].get<std::string>()};
            }
            for (const auto& sequence : analysis.sequences) {
                Json evidence{{"sequence", sequence.sequence_no}, {"origin", sequence.key_origin}};
                if (sequence.key_root)
                    evidence["root"] = *sequence.key_root;
                if (sequence.key_mode)
                    evidence["mode"] = *sequence.key_mode;
                keys.push_back(std::move(evidence));
                auto effective = sequence;
                if (const auto found = overrides.find(sequence.sequence_no); found != overrides.end()) {
                    effective.key_root = found->second.first;
                    effective.key_mode = found->second.second;
                    effective.key_origin = "manual";
                    effective.key_confidence.reset();
                }
                insert_sequence_summary(database, revision_id, effective);
            }
            result["keys"] = std::move(keys);
            insert_sequence_facts(database, revision_id);
        }
        Statement run(database, "INSERT OR REPLACE INTO analysis_run(revision_id, analyzer, version, settings_sha256, "
                                "status, completed_ms, result_json) VALUES (?1, 'basic', ?2, ?3, ?4, ?5, ?6)");
        run.bind(1, revision_id);
        run.bind(2, basic_analysis_version);
        sqlite_support::bind_bytes(run, 3, analyzer_settings_hash("basic", basic_analysis_version));
        run.bind(4, std::string(failed ? "failed" : "complete"));
        run.bind(5, now_utc_ms());
        run.bind(6, result.dump());
        run.exec();
    }

    /// Derives the indexed search facts (sequence_fact, schema v2) of one revision from its
    /// stored summaries, with the same SQL the migration backfill uses.
    void insert_sequence_facts(SQLite::Database& database, std::int64_t revision_id) {
        const auto sql = sequence_fact_sql(true);
        std::size_t begin = 0;
        while (begin < sql.size()) {
            auto end = sql.find(';', begin);
            if (end == std::string::npos)
                end = sql.size();
            const auto text = trim(std::string_view(sql).substr(begin, end - begin));
            begin = end + 1;
            if (text.empty())
                continue;
            Statement insert(database, std::string(text));
            insert.bind(1, revision_id);
            insert.exec();
        }
    }

    void insert_sequence_summary(SQLite::Database& database, std::int64_t revision_id, const SequenceSummary& summary) {
        Statement insert(database,
                         "INSERT INTO sequence_summary(revision_id, sequence_no, analysis_version, division, "
                         "duration_seconds, quarter_length, tempo_min, tempo_max, tempo_representative, tempo_origin, "
                         "key_root, key_mode, key_origin, key_confidence, track_count, event_count, note_count, "
                         "min_pitch, max_pitch, max_polyphony, has_drums, diagnostics_json) VALUES (?1, ?2, ?3, ?4, "
                         "?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22)");
        const auto bind_optional_double = [&](int index, const std::optional<double>& value) {
            if (value)
                insert.bind(index, *value);
            else
                insert.bind(index);
        };
        const auto bind_optional_int = [&](int index, const std::optional<int>& value) {
            if (value)
                insert.bind(index, static_cast<std::int64_t>(*value));
            else
                insert.bind(index);
        };
        insert.bind(1, revision_id);
        insert.bind(2, static_cast<std::int64_t>(summary.sequence_no));
        insert.bind(3, basic_analysis_version);
        insert.bind(4, std::string(summary.ppq ? "ppq" : "smpte"));
        insert.bind(5, summary.duration_seconds);
        bind_optional_double(6, summary.quarter_length);
        bind_optional_double(7, summary.tempo_min);
        bind_optional_double(8, summary.tempo_max);
        bind_optional_double(9, summary.tempo_representative);
        insert.bind(10, summary.tempo_origin);
        bind_optional_int(11, summary.key_root);
        if (summary.key_mode)
            insert.bind(12, *summary.key_mode);
        else
            insert.bind(12);
        insert.bind(13, summary.key_origin);
        bind_optional_double(14, summary.key_confidence);
        insert.bind(15, summary.track_count);
        insert.bind(16, summary.event_count);
        insert.bind(17, summary.note_count);
        bind_optional_int(18, summary.min_pitch);
        bind_optional_int(19, summary.max_pitch);
        insert.bind(20, summary.max_polyphony);
        insert.bind(21, summary.has_drums ? 1 : 0);
        insert.bind(22, summary.diagnostics_json);
        insert.exec();
        for (const auto& track : summary.tracks) {
            Statement track_insert(database, "INSERT INTO track_summary(revision_id, sequence_no, track_no, name, "
                                             "channel_mask, program_json, note_count, feature_json) VALUES (?1, ?2, ?3, "
                                             "?4, ?5, ?6, ?7, ?8)");
            track_insert.bind(1, revision_id);
            track_insert.bind(2, static_cast<std::int64_t>(summary.sequence_no));
            track_insert.bind(3, static_cast<std::int64_t>(track.track_no));
            track_insert.bind(4, track.name);
            track_insert.bind(5, static_cast<std::int64_t>(track.channel_mask));
            track_insert.bind(6, track.program_json.empty() ? program_json_for(track.programs, 0) : track.program_json);
            track_insert.bind(7, track.note_count);
            track_insert.bind(8, track.feature_json);
            track_insert.exec();
        }
    }

    /// Copies summaries and the basic run of one revision to another (restore, duplicate).
    void copy_basic_analysis(SQLite::Database& database, std::int64_t from, std::int64_t to) {
        Statement summaries(database,
                            "INSERT INTO sequence_summary SELECT ?2, sequence_no, analysis_version, division, "
                            "duration_seconds, quarter_length, tempo_min, tempo_max, tempo_representative, "
                            "tempo_origin, key_root, key_mode, key_origin, key_confidence, track_count, event_count, "
                            "note_count, min_pitch, max_pitch, max_polyphony, has_drums, diagnostics_json FROM "
                            "sequence_summary WHERE revision_id = ?1");
        summaries.bind(1, from);
        summaries.bind(2, to);
        summaries.exec();
        Statement tracks(database, "INSERT INTO track_summary SELECT ?2, sequence_no, track_no, name, channel_mask, "
                                   "program_json, note_count, feature_json FROM track_summary WHERE revision_id = ?1");
        tracks.bind(1, from);
        tracks.bind(2, to);
        tracks.exec();
        Statement runs(database, "INSERT INTO analysis_run(revision_id, analyzer, version, settings_sha256, status, "
                                 "completed_ms, result_json) SELECT ?2, analyzer, version, settings_sha256, status, "
                                 "completed_ms, result_json FROM analysis_run WHERE revision_id = ?1 AND analyzer = 'basic'");
        runs.bind(1, from);
        runs.bind(2, to);
        runs.exec();
        insert_sequence_facts(database, to);
    }

    /// Rebuilds the asset's FTS row from catalog metadata and the head's stored analysis text.
    void refresh_fts(SQLite::Database& database, std::int64_t asset_id) {
        Statement asset_query(database, "SELECT title, description, current_revision_id FROM asset WHERE id = ?1");
        asset_query.bind(1, asset_id);
        if (!asset_query.executeStep())
            return;
        const auto title = asset_query.getColumn(0).getString();
        const auto description = asset_query.getColumn(1).getString();
        const auto head = column_optional_int(asset_query, 2);
        std::string sources;
        Statement source_query(database, "SELECT relative_path FROM source_location WHERE asset_id = ?1 ORDER BY id");
        source_query.bind(1, asset_id);
        while (source_query.executeStep()) {
            const auto relative = source_query.getColumn(0).getString();
            const auto slash = relative.find_last_of('/');
            if (!sources.empty())
                sources += ' ';
            sources += slash == std::string::npos ? relative : relative.substr(slash + 1);
        }
        std::string tags;
        Statement tag_query(database, "SELECT t.name FROM asset_tag AS at JOIN tag AS t ON t.id = at.tag_id WHERE "
                                      "at.asset_id = ?1 ORDER BY t.normalized_name");
        tag_query.bind(1, asset_id);
        while (tag_query.executeStep()) {
            if (!tags.empty())
                tags += ' ';
            tags += tag_query.getColumn(0).getString();
        }
        std::string text;
        if (head) {
            Statement text_query(database, "SELECT json_extract(result_json, '$.text') FROM analysis_run WHERE "
                                           "revision_id = ?1 AND analyzer = 'basic'");
            text_query.bind(1, *head);
            if (text_query.executeStep() && !text_query.getColumn(0).isNull())
                text = text_query.getColumn(0).getString();
        }
        Statement remove(database, "DELETE FROM asset_fts WHERE rowid = ?1");
        remove.bind(1, asset_id);
        remove.exec();
        Statement insert(database, "INSERT INTO asset_fts(rowid, title, source_names, tags, description, lyrics) "
                                   "VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
        insert.bind(1, asset_id);
        insert.bind(2, title);
        insert.bind(3, sources);
        insert.bind(4, tags);
        insert.bind(5, description);
        insert.bind(6, text);
        insert.exec();
    }

    std::optional<Sha256Digest> head_object(SQLite::Database& database, std::int64_t asset_id) {
        Statement query(database, "SELECT r.object_sha256 FROM asset AS a JOIN revision AS r ON r.id = "
                                  "a.current_revision_id WHERE a.id = ?1");
        query.bind(1, asset_id);
        if (!query.executeStep())
            return std::nullopt;
        const auto bytes = sqlite_support::column_bytes(query, 0);
        if (bytes.size() != 32)
            return std::nullopt;
        Sha256Digest digest{};
        std::ranges::copy(bytes, digest.begin());
        return digest;
    }

    std::optional<std::int64_t> head_revision(SQLite::Database& database, std::int64_t asset_id) {
        Statement query(database, "SELECT current_revision_id FROM asset WHERE id = ?1");
        query.bind(1, asset_id);
        if (!query.executeStep())
            return std::nullopt;
        return column_optional_int(query, 0);
    }

    bool asset_exists(SQLite::Database& database, std::int64_t asset_id) {
        Statement query(database, "SELECT 1 FROM asset WHERE id = ?1");
        query.bind(1, asset_id);
        return query.executeStep();
    }
};

namespace {

/// Parses and analyzes object bytes for the basic summaries.
ParsedObject parse_object(std::span<const std::uint8_t> bytes, MediaType media, std::string_view name,
                          const midi::ParseLimits& limits, std::stop_token stop, bool* cancelled = nullptr,
                          const midi::MidiParser& parser = {}) {
    ParsedObject parsed;
    if (media == MediaType::native) {
        auto document = document::native_from_bytes(bytes);
        if (!document) {
            parsed.state = "invalid";
            parsed.reason = document.error().message;
            return parsed;
        }
        parsed.analysis = analyze_document(*document);
        parsed.state = parsed.analysis->warning_count > 0 ? "warning" : "ready";
        return parsed;
    }
    if (midi::detect_container(bytes) == midi::Container::unknown) {
        parsed.state = "unsupported";
        parsed.reason = "not a Standard MIDI File or RMID file (detected by content)";
        return parsed;
    }
    auto imported = parser ? parser(bytes, name, limits, std::move(stop)) : midi::import_midi(bytes, name, limits, std::move(stop));
    if (!imported) {
        if (imported.error().code == ErrorCode::cancelled && cancelled != nullptr)
            *cancelled = true;
        parsed.state = imported.error().code == ErrorCode::unsupported ? "unsupported" : "invalid";
        parsed.reason = imported.error().message;
        return parsed;
    }
    if (imported->report.reader_result == "invalid") {
        parsed.state = "invalid";
        parsed.reason = "the MIDI reader rejected the file";
        return parsed;
    }
    parsed.analysis = analyze_document(imported->document, imported->report.diagnostics);
    parsed.state = (imported->report.reader_result == "incomplete" || parsed.analysis->warning_count > 0) ? "warning"
                                                                                                         : "ready";
    if (imported->report.reader_result == "incomplete")
        parsed.reason = "the file is truncated; a partial interpretation was indexed";
    return parsed;
}

} // namespace

// --- Lifecycle ------------------------------------------------------------------------------------

Library::Library(std::unique_ptr<State> state) : state_(std::move(state)) {}
Library::~Library() = default;

const std::filesystem::path& Library::root() const { return state_->root; }
const Uuid& Library::uuid() const { return state_->uuid; }

Result<std::unique_ptr<Library>> Library::create(const std::filesystem::path& root, const LibraryOptions& options) {
    std::error_code error;
    if (std::filesystem::exists(root, error) && !std::filesystem::is_empty(root, error))
        return make_error(ErrorCode::invalid_argument, "a new library needs an empty or missing directory",
                          path_utf8(root.filename()));
    for (const auto* directory : {"objects", "staging", "cache", "backups"}) {
        std::filesystem::create_directories(root / directory, error);
        if (error)
            return make_error(ErrorCode::io, "cannot create the library directories");
    }
    {
        // Held while the catalog and descriptor are created; open() acquires it again.
        auto lock = WriterLock::acquire(root);
        if (!lock)
            return std::unexpected(std::move(lock).error());
        const auto library_uuid = Uuid::generate();
        const auto created = now_utc_ms();
        {
            auto database = open_catalog_connection(root / catalog_file_name, ConnectionRole::writer,
                                                    options.busy_timeout_ms, true);
            if (!database)
                return std::unexpected(std::move(database).error());
            MWB_TRY(create_catalog_schema(**database, library_uuid, created));
        }
        Json descriptor{{"format", "midi-workbench-library"},
                        {"version", 1},
                        {"uuid", library_uuid.to_string()},
                        {"created_ms", created},
                        {"catalog", std::string(catalog_file_name)}};
        const auto text = descriptor.dump(2);
        MWB_TRY(write_file_durably(root / descriptor_file_name,
                                   std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
    }
    return open(root, options);
}

Result<std::unique_ptr<Library>> Library::open(const std::filesystem::path& root, const LibraryOptions& options) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(root / descriptor_file_name, error) ||
        !std::filesystem::is_regular_file(root / catalog_file_name, error))
        return make_error(ErrorCode::not_found, "the directory is not a MIDI Workbench library", path_utf8(root.filename()));
    auto lock = WriterLock::acquire(root);
    if (!lock)
        return std::unexpected(std::move(lock).error());
    auto state = std::make_unique<State>(root, options, std::move(*lock));
    auto database = open_catalog_connection(root / catalog_file_name, ConnectionRole::writer, options.busy_timeout_ms);
    if (!database)
        return std::unexpected(std::move(database).error());
    state->writer = std::move(*database);
    MWB_TRY(check_catalog_header(*state->writer));
    // Older catalogs gain the search facts and covering indexes the SQL compiler relies on.
    MWB_TRY(migrate_catalog(*state->writer));
    auto library_uuid = read_catalog_uuid(*state->writer);
    if (!library_uuid)
        return std::unexpected(std::move(library_uuid).error());
    state->uuid = *library_uuid;
    auto descriptor_bytes = read_file(root / descriptor_file_name, 1u << 20);
    if (!descriptor_bytes)
        return std::unexpected(std::move(descriptor_bytes).error());
    const auto descriptor = Json::parse(descriptor_bytes->begin(), descriptor_bytes->end(), nullptr, false);
    if (descriptor.is_discarded() || !descriptor.contains("uuid") || !descriptor["uuid"].is_string() ||
        descriptor["uuid"].get<std::string>() != state->uuid.to_string())
        return make_error(ErrorCode::invalid_data, "library.json does not describe this catalog");
    std::filesystem::create_directories(root / "staging", error);
    return std::unique_ptr<Library>(new Library(std::move(state)));
}

// --- Import -----------------------------------------------------------------------------------------

namespace {

/// Files to import below a source, sorted for determinism; reparse-point directories are not
/// followed (directory_options::none).
std::vector<std::filesystem::path> discover_files(const std::filesystem::path& directory, bool recursive,
                                                  const std::filesystem::path& library_root) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    const auto library = std::filesystem::weakly_canonical(library_root, error);
    const auto consider = [&](const std::filesystem::directory_entry& entry) {
        std::error_code entry_error;
        if (!entry.is_regular_file(entry_error) || entry.is_symlink(entry_error) || !is_midi_extension(entry.path()))
            return;
        files.push_back(entry.path());
    };
    if (recursive) {
        std::filesystem::recursive_directory_iterator iterator(
            directory, std::filesystem::directory_options::skip_permission_denied, error);
        for (; !error && iterator != std::filesystem::recursive_directory_iterator(); iterator.increment(error)) {
            const auto& entry = *iterator;
            std::error_code entry_error;
            if (entry.is_directory(entry_error) &&
                (entry.is_symlink(entry_error) ||
                 std::filesystem::weakly_canonical(entry.path(), entry_error) == library)) {
                iterator.disable_recursion_pending();
                continue;
            }
            consider(entry);
        }
    } else {
        for (const auto& entry : std::filesystem::directory_iterator(
                 directory, std::filesystem::directory_options::skip_permission_denied, error))
            consider(entry);
    }
    std::ranges::sort(files);
    return files;
}

} // namespace

Result<ImportReport> Library::import_paths(const std::vector<std::filesystem::path>& sources,
                                           const ImportOptions& options, std::stop_token stop) {
    ImportReport report;
    auto& state = *state_;
    std::size_t expected_files = 0;
    const auto add_item = [&](std::string path, ImportOutcome outcome, std::optional<std::int64_t> asset,
                              std::string reason) {
        switch (outcome) {
        case ImportOutcome::imported:
            ++report.counts.imported;
            break;
        case ImportOutcome::duplicate:
            ++report.counts.duplicate;
            break;
        case ImportOutcome::unsupported:
            ++report.counts.unsupported;
            break;
        case ImportOutcome::changed_during_read:
            ++report.counts.changed_during_read;
            break;
        case ImportOutcome::failed:
            ++report.counts.failed;
            break;
        case ImportOutcome::cancelled:
            ++report.counts.cancelled;
            break;
        case ImportOutcome::source_changed:
            ++report.counts.source_changed;
            break;
        }
        report.items.push_back(ImportItem{std::move(path), outcome, asset, std::move(reason)});
        if (options.progress)
            options.progress(report.items.size(), expected_files);
    };

    for (const auto& source : sources) {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(source, error).lexically_normal();
        std::filesystem::path root;
        std::vector<std::filesystem::path> files;
        if (std::filesystem::is_directory(absolute, error)) {
            root = absolute;
            files = discover_files(absolute, options.recursive, state.root);
        } else if (std::filesystem::is_regular_file(absolute, error)) {
            root = absolute.parent_path();
            files.push_back(absolute);
        } else {
            add_item(path_utf8(source.filename()), ImportOutcome::failed, std::nullopt, "the source does not exist");
            continue;
        }
        if (options.root) {
            const auto given = std::filesystem::absolute(*options.root, error).lexically_normal();
            const auto inside = absolute.lexically_relative(given);
            if (inside.empty() || *inside.begin() == "..") {
                add_item(path_utf8(source.filename()), ImportOutcome::failed, std::nullopt,
                         "the source is not inside the given source folder");
                continue;
            }
            root = given;
        }
        const auto root_display = path_utf8(root);
        auto root_id = state.write("register source root", [&](SQLite::Database& database) -> Result<std::int64_t> {
            Statement find(database, "SELECT id FROM source_root WHERE display_path = ?1");
            find.bind(1, root_display);
            if (find.executeStep()) {
                const auto existing = find.getColumn(0).getInt64();
                if (options.watch) {
                    Statement watch(database, "UPDATE source_root SET watch_enabled = 1 WHERE id = ?1");
                    watch.bind(1, existing);
                    watch.exec();
                }
                return existing;
            }
            Statement insert(database, "INSERT INTO source_root(uuid, display_path, watch_enabled, policy_json) VALUES "
                                       "(?1, ?2, ?4, ?3)");
            sqlite_support::bind_uuid(insert, 1, Uuid::generate());
            insert.bind(2, root_display);
            insert.bind(3, Json{{"version", 1}, {"recursive", options.recursive}}.dump());
            insert.bind(4, options.watch ? 1 : 0);
            insert.exec();
            const auto id = database.getLastInsertRowid();
            state.record_change(database, "source_root.create", {}, true);
            return id;
        });
        if (!root_id)
            return std::unexpected(std::move(root_id).error());

        expected_files += files.size();
        for (const auto& file : files) {
            ++report.counts.discovered;
            const auto relative = path_utf8(file.lexically_relative(root));
            if (stop.stop_requested()) {
                add_item(relative, ImportOutcome::cancelled, std::nullopt, "import cancelled");
                continue;
            }
            // Steps 1-3: stable capture with identity recheck and bounded retries.
            std::optional<CapturedFile> captured;
            std::string capture_failure;
            bool cancelled = false;
            for (std::size_t attempt = 1; attempt <= std::max<std::size_t>(1, options.max_capture_attempts); ++attempt) {
                auto capture = capture_file(file, state.objects.staging_directory(), options.limits.max_bytes, stop);
                if (!capture) {
                    cancelled = capture.error().code == ErrorCode::cancelled;
                    capture_failure = capture.error().message;
                    break;
                }
                StagingGuard guard(capture->staging_path);
                if (options.after_capture)
                    options.after_capture(file, attempt);
                const auto now_identity = query_file_identity(file);
                if (capture->identity_at_open == capture->identity_after_copy && now_identity &&
                    *now_identity == capture->identity_at_open) {
                    // Keep the staging file: move ownership out of the guard by renaming it.
                    auto kept = capture->staging_path;
                    kept += ".stable";
                    std::filesystem::rename(capture->staging_path, kept, error);
                    if (error) {
                        capture_failure = "cannot keep the staging copy";
                        break;
                    }
                    capture->staging_path = kept;
                    captured = std::move(*capture);
                    break;
                }
                capture_failure = "the source changed while it was being read";
            }
            if (cancelled) {
                add_item(relative, ImportOutcome::cancelled, std::nullopt, "import cancelled");
                continue;
            }
            if (!captured) {
                if (capture_failure == "the source changed while it was being read") {
                    const auto key = filesystem_comparison_key(relative);
                    auto marked = state.write("mark unstable source", [&](SQLite::Database& database) -> Status {
                        Statement update(database, "UPDATE source_location SET state = 'unstable', last_seen_ms = ?1 "
                                                   "WHERE root_id = ?2 AND comparison_key = ?3");
                        update.bind(1, now_utc_ms());
                        update.bind(2, *root_id);
                        update.bind(3, key);
                        update.exec();
                        return ok();
                    });
                    std::ignore = marked; // Best effort: the item outcome below is what the report shows.
                    add_item(relative, ImportOutcome::changed_during_read, std::nullopt, capture_failure);
                } else {
                    add_item(relative, ImportOutcome::failed, std::nullopt, capture_failure);
                }
                continue;
            }
            ++report.counts.captured;
            StagingGuard staging_guard(captured->staging_path);
            // Step 4: parse the captured bytes (never a second read of the source).
            const auto container = midi::detect_container(captured->bytes);
            const auto media = container == midi::Container::rmid ||
                                       (container == midi::Container::unknown &&
                                        ascii_lower(path_utf8(file.extension())) == ".rmi")
                                   ? MediaType::rmid
                                   : MediaType::smf;
            bool parse_cancelled = false;
            const auto parsed = parse_object(captured->bytes, media, path_utf8(file.filename()), options.limits, stop,
                                             &parse_cancelled, options.parser);
            if (parse_cancelled) {
                add_item(relative, ImportOutcome::cancelled, std::nullopt, "import cancelled");
                continue;
            }
            // Step 5: publish the object under its hash (reuse when present). Held until the
            // catalog transaction below has committed its reference (garbage collection waits).
            std::shared_lock objects_guard(state.objects_lock);
            auto published = state.objects.publish(captured->staging_path, captured->sha256, media_extension(media),
                                                   captured->bytes.size());
            if (!published) {
                add_item(relative, ImportOutcome::failed, std::nullopt, published.error().message);
                continue;
            }
            // Step 6: one short catalog transaction.
            const bool unsupported = parsed.state == "unsupported" || parsed.state == "invalid";
            const auto key = filesystem_comparison_key(relative);
            const auto title = path_utf8(file.stem());
            struct Outcome {
                ImportOutcome outcome;
                std::int64_t asset;
                std::optional<std::int64_t> candidate{};
            };
            auto outcome = state.write("import", [&](SQLite::Database& database) -> Result<Outcome> {
                state.insert_object_row(database, captured->sha256, media, captured->bytes.size());
                const auto update_location = [&](std::int64_t location_id, std::string_view location_state,
                                                 bool update_object) {
                    Statement update(database, update_object
                                                   ? "UPDATE source_location SET file_identity = ?1, byte_count = ?2, "
                                                     "modified_ms = ?3, last_seen_ms = ?4, state = ?5, "
                                                     "last_object_sha256 = ?6 WHERE id = ?7"
                                                   : "UPDATE source_location SET file_identity = ?1, byte_count = ?2, "
                                                     "modified_ms = ?3, last_seen_ms = ?4, state = ?5 WHERE id = ?7");
                    sqlite_support::bind_bytes(update, 1, captured->identity_at_open.identity);
                    update.bind(2, static_cast<std::int64_t>(captured->identity_at_open.byte_count));
                    update.bind(3, captured->identity_at_open.modified_ms);
                    update.bind(4, now_utc_ms());
                    update.bind(5, std::string(location_state));
                    if (update_object)
                        sqlite_support::bind_bytes(update, 6, captured->sha256);
                    update.bind(7, location_id);
                    update.exec();
                };
                const auto insert_location = [&](std::int64_t asset_id) {
                    Statement insert(database, "INSERT INTO source_location(root_id, asset_id, relative_path, "
                                               "comparison_key, file_identity, byte_count, modified_ms, last_seen_ms, "
                                               "last_object_sha256, state) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, "
                                               "'present')");
                    insert.bind(1, *root_id);
                    insert.bind(2, asset_id);
                    insert.bind(3, relative);
                    insert.bind(4, key);
                    sqlite_support::bind_bytes(insert, 5, captured->identity_at_open.identity);
                    insert.bind(6, static_cast<std::int64_t>(captured->identity_at_open.byte_count));
                    insert.bind(7, captured->identity_at_open.modified_ms);
                    insert.bind(8, now_utc_ms());
                    sqlite_support::bind_bytes(insert, 9, captured->sha256);
                    insert.exec();
                };

                Statement location(database, "SELECT id, asset_id, last_object_sha256 FROM source_location WHERE "
                                             "root_id = ?1 AND comparison_key = ?2");
                location.bind(1, *root_id);
                location.bind(2, key);
                if (location.executeStep()) {
                    const auto location_id = location.getColumn(0).getInt64();
                    const auto asset_id = location.getColumn(1).getInt64();
                    const auto last_object = sqlite_support::column_bytes(location, 2);
                    const auto head = state.head_object(database, asset_id);
                    if (head && *head == captured->sha256) {
                        update_location(location_id, "present", true);
                        // Re-importing a song that sits in the trash brings it back instead of
                        // reporting a duplicate nobody can see.
                        Statement restore(database, "UPDATE asset SET deleted_ms = NULL, metadata_generation = "
                                                    "metadata_generation + 1 WHERE id = ?1 AND deleted_ms IS NOT NULL");
                        restore.bind(1, asset_id);
                        if (restore.exec() > 0) {
                            state.record_change(database, "asset.restore", {asset_id}, false);
                            return Outcome{ImportOutcome::imported, asset_id};
                        }
                        return Outcome{ImportOutcome::duplicate, asset_id};
                    }
                    const bool follows_import =
                        head && std::ranges::equal(std::span<const std::uint8_t>(*head), last_object);
                    if (!follows_import) {
                        // The asset was edited here: the changed source is kept as a non-head
                        // candidate revision for review (section 22.4), never applied over the edit
                        // and referenced, so garbage collection keeps its bytes.
                        const auto candidate = state.insert_revision(database, asset_id, state.head_revision(database, asset_id),
                                                                     captured->sha256, RevisionOrigin::import,
                                                                     "source change candidate");
                        state.write_basic_analysis(database, asset_id, candidate, parsed);
                        update_location(location_id, "changed", false);
                        state.record_change(database, "source.changed", {asset_id}, false, Json::object(),
                                            Json{{"candidate", candidate}});
                        return Outcome{ImportOutcome::source_changed, asset_id, candidate};
                    }
                    const auto revision = state.insert_head_revision(database, asset_id, state.head_revision(database, asset_id),
                                                                     captured->sha256, RevisionOrigin::import, "");
                    state.write_basic_analysis(database, asset_id, revision, parsed);
                    update_location(location_id, "present", true);
                    state.refresh_fts(database, asset_id);
                    state.record_change(database, "import.revision", {asset_id}, false);
                    return Outcome{unsupported ? ImportOutcome::unsupported : ImportOutcome::imported, asset_id};
                }
                if (options.duplicates == DuplicatePolicy::attach_source) {
                    Statement existing(database, "SELECT a.id FROM revision AS r JOIN asset AS a ON a.current_revision_id "
                                                 "= r.id WHERE r.object_sha256 = ?1 AND a.deleted_ms IS NULL ORDER BY "
                                                 "a.id LIMIT 1");
                    sqlite_support::bind_bytes(existing, 1, captured->sha256);
                    if (existing.executeStep()) {
                        const auto asset_id = existing.getColumn(0).getInt64();
                        insert_location(asset_id);
                        state.refresh_fts(database, asset_id);
                        state.record_change(database, "import.attach_source", {asset_id}, false);
                        return Outcome{ImportOutcome::duplicate, asset_id};
                    }
                }
                const auto asset_id = state.insert_asset(database, title, AssetKind::song);
                const auto revision = state.insert_head_revision(database, asset_id, std::nullopt, captured->sha256,
                                                                 RevisionOrigin::import, "");
                state.write_basic_analysis(database, asset_id, revision, parsed);
                insert_location(asset_id);
                state.refresh_fts(database, asset_id);
                state.record_change(database, "import.asset", {asset_id}, false);
                return Outcome{unsupported ? ImportOutcome::unsupported : ImportOutcome::imported, asset_id};
            });
            if (!outcome) {
                add_item(relative, ImportOutcome::failed, std::nullopt, outcome.error().message);
                continue;
            }
            add_item(relative, outcome->outcome, outcome->asset, parsed.reason);
            report.items.back().candidate_revision_id = outcome->candidate;
        }
    }
    return report;
}

// --- Assets and metadata ----------------------------------------------------------------------------

Result<std::optional<AssetInfo>> Library::asset(std::int64_t asset_id) {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    return sqlite_support::guarded("read asset", [&]() -> Result<std::optional<AssetInfo>> {
        Statement query(***reader, "SELECT id, uuid, title, sort_title, description, kind, role, rating, favorite, "
                                   "created_ms, modified_ms, deleted_ms, current_revision_id, metadata_generation FROM "
                                   "asset WHERE id = ?1");
        query.bind(1, asset_id);
        if (!query.executeStep())
            return std::optional<AssetInfo>{};
        AssetInfo info;
        info.id = query.getColumn(0).getInt64();
        auto identity = sqlite_support::column_uuid(query, 1, "asset uuid");
        if (!identity)
            return std::unexpected(std::move(identity).error());
        info.uuid = *identity;
        info.title = query.getColumn(2).getString();
        info.sort_title = query.getColumn(3).getString();
        info.description = query.getColumn(4).getString();
        info.kind = parse_kind(query.getColumn(5).getString()).value_or(AssetKind::song);
        info.role = query.getColumn(6).getString();
        info.rating = query.getColumn(7).getInt();
        info.favorite = query.getColumn(8).getInt() != 0;
        info.created_ms = query.getColumn(9).getInt64();
        info.modified_ms = query.getColumn(10).getInt64();
        info.deleted_ms = column_optional_int(query, 11);
        info.current_revision_id = column_optional_int(query, 12);
        info.metadata_generation = query.getColumn(13).getInt64();
        return std::optional<AssetInfo>{std::move(info)};
    });
}

Result<std::int64_t> Library::create_asset(const DocumentData& document, std::string title, AssetKind kind,
                                           RevisionOrigin origin) {
    auto bytes = document::native_to_bytes(document);
    if (!bytes)
        return std::unexpected(std::move(bytes).error());
    const auto digest = sha256(*bytes);
    std::shared_lock objects_guard(state_->objects_lock); // through the catalog commit below
    MWB_TRY(state_->objects.publish_bytes(*bytes, digest, media_extension(MediaType::native)));
    ParsedObject parsed;
    parsed.analysis = analyze_document(document);
    parsed.state = "ready";
    auto& state = *state_;
    return state.write("create asset", [&](SQLite::Database& database) -> Result<std::int64_t> {
        state.insert_object_row(database, digest, MediaType::native, bytes->size());
        const auto asset_id = state.insert_asset(database, title, kind);
        const auto revision = state.insert_head_revision(database, asset_id, std::nullopt, digest, origin, "");
        state.write_basic_analysis(database, asset_id, revision, parsed);
        state.refresh_fts(database, asset_id);
        state.record_change(database, "asset.create", {asset_id}, false);
        return asset_id;
    });
}

Result<std::int64_t> Library::duplicate_asset(std::int64_t asset_id) {
    auto& state = *state_;
    return state.write("duplicate asset", [&](SQLite::Database& database) -> Result<std::int64_t> {
        Statement source(database, "SELECT a.title, a.description, a.kind, a.role, r.object_sha256, r.id FROM asset AS a "
                                   "JOIN revision AS r ON r.id = a.current_revision_id WHERE a.id = ?1");
        source.bind(1, asset_id);
        if (!source.executeStep())
            return make_error(ErrorCode::not_found, "the asset has no current revision");
        const auto title = source.getColumn(0).getString();
        const auto description = source.getColumn(1).getString();
        const auto kind = parse_kind(source.getColumn(2).getString()).value_or(AssetKind::song);
        const auto role = source.getColumn(3).getString();
        const auto object = sqlite_support::column_bytes(source, 4);
        const auto head = source.getColumn(5).getInt64();
        Sha256Digest digest{};
        std::ranges::copy(object, digest.begin());
        const auto copy_id = state.insert_asset(database, title, kind);
        Statement metadata(database, "UPDATE asset SET description = ?1, role = ?2 WHERE id = ?3");
        metadata.bind(1, description);
        metadata.bind(2, role);
        metadata.bind(3, copy_id);
        metadata.exec();
        const auto revision = state.insert_head_revision(database, copy_id, std::nullopt, digest, RevisionOrigin::edit,
                                                         "duplicate");
        state.copy_basic_analysis(database, head, revision);
        // Manual keys travel with the copy (its summaries say "manual"; the override row says why).
        Statement copy_overrides(database, "INSERT INTO asset_override(asset_id, sequence_no, field, schema_version, "
                                           "value_json, modified_ms) SELECT ?2, sequence_no, field, schema_version, "
                                           "value_json, modified_ms FROM asset_override WHERE asset_id = ?1");
        copy_overrides.bind(1, asset_id);
        copy_overrides.bind(2, copy_id);
        copy_overrides.exec();
        state.refresh_fts(database, copy_id);
        state.record_change(database, "asset.duplicate", {copy_id}, false, Json{{"source", asset_id}});
        return copy_id;
    });
}

namespace {

/// Updates one asset column; `column` is a fixed identifier chosen by this file, never user text.
template <class Value>
Status update_asset_column(Library::State& state, std::int64_t asset_id, std::string_view column, const Value& value,
                           bool refresh_text, std::string_view operation) {
    return state.write(operation, [&](SQLite::Database& database) -> Status {
        Statement before(database, std::format("SELECT {} FROM asset WHERE id = ?1", column));
        before.bind(1, asset_id);
        if (!before.executeStep())
            return make_error(ErrorCode::not_found, "the asset does not exist");
        const auto old_value = before.getColumn(0).getString();
        Statement update(database, std::format("UPDATE asset SET {} = ?1, modified_ms = ?2, metadata_generation = "
                                               "metadata_generation + 1 WHERE id = ?3",
                                               column));
        update.bind(1, value);
        update.bind(2, now_utc_ms());
        update.bind(3, asset_id);
        update.exec();
        if (refresh_text)
            state.refresh_fts(database, asset_id);
        Json after{{"field", column}};
        if constexpr (std::is_same_v<Value, std::string>)
            after["value"] = truncate_for_change(value);
        else
            after["value"] = value;
        state.record_change(database, operation, {asset_id}, false,
                            Json{{"field", column}, {"value", truncate_for_change(old_value)}}, after);
        return ok();
    });
}

} // namespace

Status Library::set_title(std::int64_t asset_id, std::string title) {
    auto& state = *state_;
    return state.write("asset.title", [&](SQLite::Database& database) -> Status {
        Statement before(database, "SELECT title FROM asset WHERE id = ?1");
        before.bind(1, asset_id);
        if (!before.executeStep())
            return make_error(ErrorCode::not_found, "the asset does not exist");
        const auto old_title = before.getColumn(0).getString();
        Statement update(database, "UPDATE asset SET title = ?1, sort_title = ?2, modified_ms = ?3, metadata_generation = "
                                   "metadata_generation + 1 WHERE id = ?4");
        update.bind(1, title);
        update.bind(2, sort_key(title));
        update.bind(3, now_utc_ms());
        update.bind(4, asset_id);
        update.exec();
        state.refresh_fts(database, asset_id);
        state.record_change(database, "asset.title", {asset_id}, false, Json{{"title", truncate_for_change(old_title)}},
                            Json{{"title", truncate_for_change(title)}});
        return ok();
    });
}

Status Library::set_description(std::int64_t asset_id, std::string description) {
    return update_asset_column(*state_, asset_id, "description", description, true, "asset.description");
}

Status Library::set_role(std::int64_t asset_id, std::string role) {
    const auto normalized = fold_for_search(trim(role));
    return update_asset_column(*state_, asset_id, "role", normalized.empty() ? std::string("unspecified") : normalized,
                               false, "asset.role");
}

Status Library::set_favorite(std::int64_t asset_id, bool favorite) {
    return update_asset_column(*state_, asset_id, "favorite", std::int64_t{favorite ? 1 : 0}, false, "asset.favorite");
}

Status Library::set_rating(std::int64_t asset_id, int rating) {
    if (rating < 0 || rating > 5)
        return make_error(ErrorCode::invalid_argument, "ratings are 0 to 5");
    return update_asset_column(*state_, asset_id, "rating", std::int64_t{rating}, false, "asset.rating");
}

Status Library::set_key_override(std::int64_t asset_id, std::uint32_t sequence_no, int root, std::string mode) {
    if (root < 0 || root > 11)
        return make_error(ErrorCode::invalid_argument, "the key root is a pitch class 0-11");
    auto& state = *state_;
    return state.write("asset.key_override", [&](SQLite::Database& database) -> Status {
        const auto head = state.head_revision(database, asset_id);
        if (!head)
            return make_error(ErrorCode::not_found, "the asset has no current revision");
        Statement insert(database, "INSERT OR REPLACE INTO asset_override(asset_id, sequence_no, field, schema_version, "
                                   "value_json, modified_ms) VALUES (?1, ?2, 'key', 1, ?3, ?4)");
        insert.bind(1, asset_id);
        insert.bind(2, static_cast<std::int64_t>(sequence_no));
        insert.bind(3, Json{{"root", root}, {"mode", mode}}.dump());
        insert.bind(4, now_utc_ms());
        insert.exec();
        // Effective summary of the head; the analysis evidence stays in analysis_run.result_json.
        Statement update(database, "UPDATE sequence_summary SET key_root = ?1, key_mode = ?2, key_origin = 'manual', "
                                   "key_confidence = NULL WHERE revision_id = ?3 AND sequence_no = ?4");
        update.bind(1, static_cast<std::int64_t>(root));
        update.bind(2, mode);
        update.bind(3, *head);
        update.bind(4, static_cast<std::int64_t>(sequence_no));
        update.exec();
        state.record_change(database, "asset.key_override", {asset_id}, false, Json::object(),
                            Json{{"sequence", sequence_no}, {"root", root}, {"mode", mode}});
        return ok();
    });
}

// --- Tags -------------------------------------------------------------------------------------------

Status Library::add_tag(std::int64_t asset_id, const std::string& name) {
    const auto display = trim(name);
    const auto normalized = fold_for_search(display);
    if (normalized.empty())
        return make_error(ErrorCode::invalid_argument, "a tag needs a name");
    auto& state = *state_;
    return state.write("tag.add", [&](SQLite::Database& database) -> Status {
        if (!state.asset_exists(database, asset_id))
            return make_error(ErrorCode::not_found, "the asset does not exist");
        bool created = false;
        Statement find(database, "SELECT id FROM tag WHERE normalized_name = ?1");
        find.bind(1, normalized);
        std::int64_t tag_id = 0;
        if (find.executeStep()) {
            tag_id = find.getColumn(0).getInt64();
        } else {
            Statement insert(database, "INSERT INTO tag(name, normalized_name) VALUES (?1, ?2)");
            insert.bind(1, display);
            insert.bind(2, normalized);
            insert.exec();
            tag_id = database.getLastInsertRowid();
            created = true;
        }
        Statement link(database, "INSERT OR IGNORE INTO asset_tag(asset_id, tag_id) VALUES (?1, ?2)");
        link.bind(1, asset_id);
        link.bind(2, tag_id);
        if (link.exec() == 0 && !created)
            return ok();
        Statement touch(database, "UPDATE asset SET metadata_generation = metadata_generation + 1 WHERE id = ?1");
        touch.bind(1, asset_id);
        touch.exec();
        state.refresh_fts(database, asset_id);
        state.record_change(database, "tag.add", {asset_id}, created, Json::object(), Json{{"tag", normalized}});
        return ok();
    });
}

Status Library::remove_tag(std::int64_t asset_id, const std::string& name) {
    const auto normalized = fold_for_search(trim(name));
    auto& state = *state_;
    return state.write("tag.remove", [&](SQLite::Database& database) -> Status {
        Statement unlink(database, "DELETE FROM asset_tag WHERE asset_id = ?1 AND tag_id = (SELECT id FROM tag WHERE "
                                   "normalized_name = ?2)");
        unlink.bind(1, asset_id);
        unlink.bind(2, normalized);
        if (unlink.exec() == 0)
            return ok();
        Statement touch(database, "UPDATE asset SET metadata_generation = metadata_generation + 1 WHERE id = ?1");
        touch.bind(1, asset_id);
        touch.exec();
        state.refresh_fts(database, asset_id);
        state.record_change(database, "tag.remove", {asset_id}, false, Json{{"tag", normalized}});
        return ok();
    });
}

Result<std::vector<TagInfo>> Library::list_tags() {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    return sqlite_support::guarded("list tags", [&]() -> Result<std::vector<TagInfo>> {
        Statement query(***reader, "SELECT t.id, t.name, t.normalized_name, (SELECT count(*) FROM asset_tag AS at "
                                   "WHERE at.tag_id = t.id) FROM tag AS t ORDER BY t.normalized_name");
        std::vector<TagInfo> tags;
        while (query.executeStep())
            tags.push_back(TagInfo{query.getColumn(0).getInt64(), query.getColumn(1).getString(),
                                   query.getColumn(2).getString(), query.getColumn(3).getInt64()});
        return tags;
    });
}

Result<std::vector<std::string>> Library::asset_tags(std::int64_t asset_id) {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    return sqlite_support::guarded("asset tags", [&]() -> Result<std::vector<std::string>> {
        Statement query(***reader, "SELECT t.normalized_name FROM asset_tag AS at JOIN tag AS t ON t.id = at.tag_id "
                                   "WHERE at.asset_id = ?1 ORDER BY t.normalized_name");
        query.bind(1, asset_id);
        std::vector<std::string> tags;
        while (query.executeStep())
            tags.push_back(query.getColumn(0).getString());
        return tags;
    });
}

// --- Collections --------------------------------------------------------------------------------

namespace {

Result<std::int64_t> insert_collection(Library::State& state, const std::string& name, std::optional<std::int64_t> parent,
                                       const std::optional<std::string>& query_json) {
    if (trim(name).empty())
        return make_error(ErrorCode::invalid_argument, "a collection needs a name");
    return state.write("collection.create", [&](SQLite::Database& database) -> Result<std::int64_t> {
        if (parent) {
            Statement exists(database, "SELECT 1 FROM collection WHERE id = ?1");
            exists.bind(1, *parent);
            if (!exists.executeStep())
                return make_error(ErrorCode::not_found, "the parent collection does not exist");
        }
        Statement insert(database, "INSERT INTO collection(uuid, parent_id, name, kind, query_json) VALUES (?1, ?2, ?3, "
                                   "?4, ?5)");
        sqlite_support::bind_uuid(insert, 1, Uuid::generate());
        if (parent)
            insert.bind(2, *parent);
        else
            insert.bind(2);
        insert.bind(3, trim(name));
        insert.bind(4, std::string(query_json ? "smart" : "manual"));
        if (query_json)
            insert.bind(5, *query_json);
        else
            insert.bind(5);
        insert.exec();
        const auto id = database.getLastInsertRowid();
        state.record_change(database, "collection.create", {}, true, Json::object(), Json{{"collection", id}});
        return id;
    });
}

} // namespace

Result<std::int64_t> Library::create_collection(const std::string& name, std::optional<std::int64_t> parent) {
    return insert_collection(*state_, name, parent, std::nullopt);
}

Result<std::int64_t> Library::create_smart_collection(const std::string& name, const std::string& query_text,
                                                      std::optional<std::int64_t> parent) {
    auto query = search::parse_query(query_text);
    if (!query)
        return std::unexpected(query.error().to_error());
    return insert_collection(*state_, name, parent, search::query_to_json(*query));
}

Status Library::add_to_collection(std::int64_t collection_id, const std::vector<std::int64_t>& asset_ids) {
    auto& state = *state_;
    return state.write("collection.add", [&](SQLite::Database& database) -> Status {
        Statement kind(database, "SELECT kind FROM collection WHERE id = ?1");
        kind.bind(1, collection_id);
        if (!kind.executeStep())
            return make_error(ErrorCode::not_found, "the collection does not exist");
        if (kind.getColumn(0).getString() != "manual")
            return make_error(ErrorCode::invalid_argument, "only manual collections have explicit members");
        Statement last(database, "SELECT coalesce(max(position), 0) FROM collection_asset WHERE collection_id = ?1");
        last.bind(1, collection_id);
        last.executeStep();
        auto position = last.getColumn(0).getInt64();
        std::vector<std::int64_t> added;
        for (const auto asset_id : asset_ids) {
            if (!state.asset_exists(database, asset_id))
                return make_error(ErrorCode::not_found, "an asset does not exist", std::to_string(asset_id));
            Statement insert(database, "INSERT OR IGNORE INTO collection_asset(collection_id, asset_id, position) VALUES "
                                       "(?1, ?2, ?3)");
            insert.bind(1, collection_id);
            insert.bind(2, asset_id);
            insert.bind(3, position + 1);
            if (insert.exec() > 0) {
                ++position;
                added.push_back(asset_id);
            }
        }
        state.record_change(database, "collection.add", added, false, Json::object(), Json{{"collection", collection_id}});
        return ok();
    });
}

Status Library::remove_from_collection(std::int64_t collection_id, const std::vector<std::int64_t>& asset_ids) {
    auto& state = *state_;
    return state.write("collection.remove", [&](SQLite::Database& database) -> Status {
        for (const auto asset_id : asset_ids) {
            Statement remove(database, "DELETE FROM collection_asset WHERE collection_id = ?1 AND asset_id = ?2");
            remove.bind(1, collection_id);
            remove.bind(2, asset_id);
            remove.exec();
        }
        state.record_change(database, "collection.remove", asset_ids, false, Json{{"collection", collection_id}});
        return ok();
    });
}

Status Library::move_in_collection(std::int64_t collection_id, std::int64_t asset_id, std::size_t index) {
    auto& state = *state_;
    return state.write("collection.move", [&](SQLite::Database& database) -> Status {
        std::vector<std::int64_t> order;
        Statement members(database, "SELECT asset_id FROM collection_asset WHERE collection_id = ?1 ORDER BY position, "
                                    "asset_id");
        members.bind(1, collection_id);
        while (members.executeStep())
            order.push_back(members.getColumn(0).getInt64());
        const auto found = std::ranges::find(order, asset_id);
        if (found == order.end())
            return make_error(ErrorCode::not_found, "the asset is not in the collection");
        order.erase(found);
        order.insert(order.begin() + static_cast<std::ptrdiff_t>(std::min(index, order.size())), asset_id);
        for (std::size_t position = 0; position < order.size(); ++position) {
            Statement update(database, "UPDATE collection_asset SET position = ?1 WHERE collection_id = ?2 AND asset_id = ?3");
            update.bind(1, static_cast<std::int64_t>(position + 1));
            update.bind(2, collection_id);
            update.bind(3, order[position]);
            update.exec();
        }
        state.record_change(database, "collection.move", {asset_id}, false, Json::object(),
                            Json{{"collection", collection_id}, {"index", index}});
        return ok();
    });
}

Result<std::vector<std::int64_t>> Library::collection_members(std::int64_t collection_id,
                                                              std::optional<std::int64_t> after_position,
                                                              std::size_t limit) {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    return sqlite_support::guarded("collection members", [&]() -> Result<std::vector<std::int64_t>> {
        Statement query(***reader, "SELECT asset_id FROM collection_asset INDEXED BY collection_order WHERE "
                                   "collection_id = ?1 AND position > ?2 ORDER BY position, asset_id LIMIT ?3");
        query.bind(1, collection_id);
        query.bind(2, after_position.value_or(std::numeric_limits<std::int64_t>::min()));
        query.bind(3, static_cast<std::int64_t>(std::min(limit, search::maximum_page_size)));
        std::vector<std::int64_t> members;
        while (query.executeStep())
            members.push_back(query.getColumn(0).getInt64());
        return members;
    });
}

Result<std::vector<CollectionInfo>> Library::list_collections() {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    return sqlite_support::guarded("list collections", [&]() -> Result<std::vector<CollectionInfo>> {
        Statement query(***reader, "SELECT id, uuid, parent_id, name, kind FROM collection ORDER BY id");
        std::vector<CollectionInfo> collections;
        while (query.executeStep()) {
            CollectionInfo info;
            info.id = query.getColumn(0).getInt64();
            auto identity = sqlite_support::column_uuid(query, 1, "collection uuid");
            if (!identity)
                return std::unexpected(std::move(identity).error());
            info.uuid = *identity;
            info.parent_id = column_optional_int(query, 2);
            info.name = query.getColumn(3).getString();
            info.smart = query.getColumn(4).getString() == "smart";
            collections.push_back(std::move(info));
        }
        return collections;
    });
}

Result<search::Query> Library::smart_collection_query(std::int64_t collection_id) {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    auto json = sqlite_support::guarded("smart collection", [&]() -> Result<std::string> {
        Statement query(***reader, "SELECT query_json FROM collection WHERE id = ?1 AND kind = 'smart'");
        query.bind(1, collection_id);
        if (!query.executeStep())
            return make_error(ErrorCode::not_found, "no smart collection with this ID");
        return query.getColumn(0).getString();
    });
    if (!json)
        return std::unexpected(std::move(json).error());
    return search::query_from_json(*json);
}

// --- Revisions ------------------------------------------------------------------------------------

Result<std::vector<RevisionInfo>> Library::revisions(std::int64_t asset_id) {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    return sqlite_support::guarded("list revisions", [&]() -> Result<std::vector<RevisionInfo>> {
        Statement query(***reader, "SELECT r.id, r.uuid, r.asset_id, r.ordinal, r.parent_id, r.object_sha256, "
                                   "o.media_type, o.byte_count, r.created_ms, r.origin, r.label, r.pinned FROM revision AS "
                                   "r JOIN object_file AS o ON o.sha256 = r.object_sha256 WHERE r.asset_id = ?1 ORDER "
                                   "BY r.ordinal");
        query.bind(1, asset_id);
        std::vector<RevisionInfo> revisions;
        while (query.executeStep()) {
            RevisionInfo info;
            info.id = query.getColumn(0).getInt64();
            auto identity = sqlite_support::column_uuid(query, 1, "revision uuid");
            if (!identity)
                return std::unexpected(std::move(identity).error());
            info.uuid = *identity;
            info.asset_id = query.getColumn(2).getInt64();
            info.ordinal = query.getColumn(3).getInt64();
            info.parent_id = column_optional_int(query, 4);
            auto digest = sqlite_support::column_digest(query, 5, "object hash");
            if (!digest)
                return std::unexpected(std::move(digest).error());
            info.object_sha256 = *digest;
            info.media_type = parse_media_type(query.getColumn(6).getString()).value_or(MediaType::smf);
            info.byte_count = static_cast<std::uint64_t>(query.getColumn(7).getInt64());
            info.created_ms = query.getColumn(8).getInt64();
            info.origin = parse_origin(query.getColumn(9).getString()).value_or(RevisionOrigin::edit);
            info.label = query.getColumn(10).getString();
            info.pinned = query.getColumn(11).getInt() != 0;
            revisions.push_back(std::move(info));
        }
        return revisions;
    });
}

Result<std::int64_t> Library::commit_revision(const CommitRequest& request, std::span<const std::uint8_t> bytes,
                                              MediaType media_type) {
    const auto digest = sha256(bytes);
    // Section 22.3: publish the immutable object first; a conflicting head keeps it for review.
    std::shared_lock objects_guard(state_->objects_lock); // through the catalog commit below
    MWB_TRY(state_->objects.publish_bytes(bytes, digest, media_extension(media_type)));
    const auto parsed = parse_object(bytes, media_type, "revision", {}, {});
    auto& state = *state_;
    return state.write("revision.commit", [&](SQLite::Database& database) -> Result<std::int64_t> {
        if (!state.asset_exists(database, request.asset_id))
            return make_error(ErrorCode::not_found, "the asset does not exist");
        const auto head = state.head_revision(database, request.asset_id);
        if (head != request.expected_head)
            return make_error(ErrorCode::conflict,
                              "the asset changed since it was opened; save as a new asset or review and retry",
                              std::format("expected head {}, current head {}", request.expected_head.value_or(0),
                                          head.value_or(0)));
        state.insert_object_row(database, digest, media_type, bytes.size());
        const auto revision = state.insert_head_revision(database, request.asset_id, head, digest, request.origin,
                                                         request.label);
        state.write_basic_analysis(database, request.asset_id, revision, parsed);
        state.refresh_fts(database, request.asset_id);
        state.record_change(database, "revision.commit", {request.asset_id}, false, Json{{"head", head.value_or(0)}},
                            Json{{"head", revision}});
        return revision;
    });
}

Result<std::int64_t> Library::save_document_revision(const CommitRequest& request, const DocumentData& document) {
    auto bytes = document::native_to_bytes(document);
    if (!bytes)
        return std::unexpected(std::move(bytes).error());
    return commit_revision(request, *bytes, MediaType::native);
}

Result<std::int64_t> Library::restore_revision(std::int64_t asset_id, std::int64_t revision_id,
                                               std::optional<std::int64_t> expected_head) {
    auto& state = *state_;
    return state.write("revision.restore", [&](SQLite::Database& database) -> Result<std::int64_t> {
        Statement source(database, "SELECT object_sha256 FROM revision WHERE id = ?1 AND asset_id = ?2");
        source.bind(1, revision_id);
        source.bind(2, asset_id);
        if (!source.executeStep())
            return make_error(ErrorCode::not_found, "the revision does not belong to the asset");
        const auto object = sqlite_support::column_bytes(source, 0);
        const auto head = state.head_revision(database, asset_id);
        if (head != expected_head)
            return make_error(ErrorCode::conflict, "the asset changed since the revision list was shown");
        Sha256Digest digest{};
        std::ranges::copy(object, digest.begin());
        const auto restored = state.insert_head_revision(database, asset_id, head, digest, RevisionOrigin::restore,
                                                         std::format("restore of revision {}", revision_id));
        state.copy_basic_analysis(database, revision_id, restored);
        state.apply_key_overrides(database, asset_id, restored); // the user's key survives a restore
        state.refresh_fts(database, asset_id);
        state.record_change(database, "revision.restore", {asset_id}, false, Json{{"head", head.value_or(0)}},
                            Json{{"head", restored}, {"source", revision_id}});
        return restored;
    });
}

Status Library::set_revision_pinned(std::int64_t revision_id, bool pinned) {
    auto& state = *state_;
    return state.write("revision.pin", [&](SQLite::Database& database) -> Status {
        Statement update(database, "UPDATE revision SET pinned = ?1 WHERE id = ?2");
        update.bind(1, pinned ? 1 : 0);
        update.bind(2, revision_id);
        if (update.exec() == 0)
            return make_error(ErrorCode::not_found, "the revision does not exist");
        Statement owner(database, "SELECT asset_id FROM revision WHERE id = ?1");
        owner.bind(1, revision_id);
        owner.executeStep();
        state.record_change(database, "revision.pin", {owner.getColumn(0).getInt64()}, false);
        return ok();
    });
}

Result<std::vector<std::uint8_t>> Library::revision_bytes(std::int64_t revision_id) {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    auto object = sqlite_support::guarded("revision object", [&]() -> Result<std::pair<Sha256Digest, std::string>> {
        Statement query(***reader, "SELECT r.object_sha256, o.extension FROM revision AS r JOIN object_file AS o ON "
                                   "o.sha256 = r.object_sha256 WHERE r.id = ?1");
        query.bind(1, revision_id);
        if (!query.executeStep())
            return make_error(ErrorCode::not_found, "the revision does not exist");
        auto digest = sqlite_support::column_digest(query, 0, "object hash");
        if (!digest)
            return std::unexpected(std::move(digest).error());
        return std::pair{*digest, query.getColumn(1).getString()};
    });
    if (!object)
        return std::unexpected(std::move(object).error());
    return state_->objects.read(object->first, object->second);
}

Result<DocumentData> Library::open_revision(std::int64_t revision_id, const midi::MidiParser& parser,
                                           std::stop_token stop) {
    auto bytes = revision_bytes(revision_id);
    if (!bytes)
        return std::unexpected(std::move(bytes).error());
    if (midi::detect_container(*bytes) != midi::Container::unknown) {
        const midi::ParseLimits limits;
        auto imported = parser ? parser(*bytes, "revision", limits, std::move(stop))
                               : midi::import_midi(*bytes, "revision", limits, std::move(stop));
        if (!imported)
            return std::unexpected(std::move(imported).error());
        return std::move(imported->document);
    }
    return document::native_from_bytes(*bytes);
}

// --- Trash and garbage collection ---------------------------------------------------------------

Status Library::trash(const std::vector<std::int64_t>& asset_ids) {
    auto& state = *state_;
    return state.write("asset.trash", [&](SQLite::Database& database) -> Status {
        const auto now = now_utc_ms();
        std::vector<std::int64_t> changed;
        for (const auto asset_id : asset_ids) {
            Statement update(database, "UPDATE asset SET deleted_ms = ?1, metadata_generation = metadata_generation + 1 "
                                       "WHERE id = ?2 AND deleted_ms IS NULL");
            update.bind(1, now);
            update.bind(2, asset_id);
            if (update.exec() > 0)
                changed.push_back(asset_id);
        }
        state.record_change(database, "asset.trash", changed, false);
        return ok();
    });
}

Status Library::restore_from_trash(const std::vector<std::int64_t>& asset_ids) {
    auto& state = *state_;
    return state.write("asset.restore", [&](SQLite::Database& database) -> Status {
        std::vector<std::int64_t> changed;
        for (const auto asset_id : asset_ids) {
            Statement update(database, "UPDATE asset SET deleted_ms = NULL, metadata_generation = metadata_generation + "
                                       "1 WHERE id = ?1 AND deleted_ms IS NOT NULL");
            update.bind(1, asset_id);
            if (update.exec() > 0)
                changed.push_back(asset_id);
        }
        state.record_change(database, "asset.restore", changed, false);
        return ok();
    });
}

namespace {

Result<PurgeReport> count_purge(SQLite::Database& database, const std::vector<std::int64_t>& asset_ids) {
    PurgeReport report;
    for (const auto asset_id : asset_ids) {
        Statement asset(database, "SELECT deleted_ms FROM asset WHERE id = ?1");
        asset.bind(1, asset_id);
        if (!asset.executeStep())
            continue;
        if (asset.getColumn(0).isNull())
            return make_error(ErrorCode::invalid_argument, "only trashed assets can be purged", std::to_string(asset_id));
        Statement revisions(database, "SELECT count(*) FROM revision WHERE asset_id = ?1");
        revisions.bind(1, asset_id);
        revisions.executeStep();
        ++report.assets;
        report.revisions += static_cast<std::size_t>(revisions.getColumn(0).getInt64());
    }
    return report;
}

} // namespace

Result<PurgeReport> Library::purge_preview(const std::vector<std::int64_t>& asset_ids) {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    return sqlite_support::guarded("purge preview", [&]() { return count_purge(***reader, asset_ids); });
}

Result<PurgeReport> Library::purge(const std::vector<std::int64_t>& asset_ids) {
    auto& state = *state_;
    return state.write("asset.purge", [&](SQLite::Database& database) -> Result<PurgeReport> {
        auto report = count_purge(database, asset_ids);
        if (!report)
            return report;
        for (const auto asset_id : asset_ids) {
            Statement fts(database, "DELETE FROM asset_fts WHERE rowid = ?1");
            fts.bind(1, asset_id);
            fts.exec();
            // Detach the head first so the deferred head reference never dangles.
            Statement detach(database, "UPDATE asset SET current_revision_id = NULL WHERE id = ?1 AND deleted_ms IS NOT NULL");
            detach.bind(1, asset_id);
            detach.exec();
            Statement remove(database, "DELETE FROM asset WHERE id = ?1 AND deleted_ms IS NOT NULL");
            remove.bind(1, asset_id);
            remove.exec();
        }
        // Permanent purge ends the undo history of the affected entries.
        state.record_change(database, "asset.purge", asset_ids, true, Json::object(),
                            Json{{"revisions", report->revisions}, {"ends_undo", true}});
        return report;
    });
}

Result<std::vector<std::int64_t>> Library::list_trash(std::size_t limit) {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    return sqlite_support::guarded("list trash", [&]() -> Result<std::vector<std::int64_t>> {
        Statement query(***reader, "SELECT id FROM asset INDEXED BY asset_browse WHERE deleted_ms IS NOT NULL ORDER BY "
                                   "deleted_ms, sort_title, id LIMIT ?1");
        query.bind(1, static_cast<std::int64_t>(std::min(limit, search::maximum_page_size)));
        std::vector<std::int64_t> ids;
        while (query.executeStep())
            ids.push_back(query.getColumn(0).getInt64());
        return ids;
    });
}

Library::GarbageCollectionPause::GarbageCollectionPause(Library& library) : library_(library) {
    ++library_.state_->gc_pauses;
}
Library::GarbageCollectionPause::~GarbageCollectionPause() { --library_.state_->gc_pauses; }

Result<GarbageCollectionReport> Library::collect_garbage(const GarbageCollectionOptions& options) {
    auto& state = *state_;
    GarbageCollectionReport report;
    if (state.gc_pauses.load() > 0) {
        report.suspended = true;
        return report;
    }
    const auto now = options.now_ms.value_or(now_utc_ms());
    // Exclusive: no object is published and referenced again between choosing it and removing it.
    std::unique_lock objects_guard(state.objects_lock);
    struct Candidate {
        Sha256Digest sha;
        std::string extension;
    };
    std::vector<Candidate> expired;
    auto marked = state.write("gc.mark", [&](SQLite::Database& database) -> Status {
        Statement setting(database, "SELECT value_json FROM catalog_setting WHERE key = 'gc.unreferenced_since'");
        Json since = Json::object();
        if (setting.executeStep()) {
            since = Json::parse(setting.getColumn(0).getString(), nullptr, false);
            if (!since.is_object())
                since = Json::object();
        }
        Json still = Json::object();
        Statement unreferenced(database, "SELECT o.sha256, o.extension FROM object_file AS o WHERE NOT EXISTS (SELECT 1 "
                                         "FROM revision AS r WHERE r.object_sha256 = o.sha256) AND NOT EXISTS (SELECT 1 "
                                         "FROM source_location AS s WHERE s.last_object_sha256 = o.sha256)");
        while (unreferenced.executeStep()) {
            auto digest = sqlite_support::column_digest(unreferenced, 0, "object hash");
            if (!digest)
                continue;
            ++report.unreferenced;
            const auto hex = to_hex(*digest);
            const auto first_seen = since.contains(hex) && since[hex].is_number_integer() ? since[hex].get<std::int64_t>() : now;
            if (now - first_seen >= options.grace_period_ms)
                expired.push_back(Candidate{*digest, unreferenced.getColumn(1).getString()});
            else
                still[hex] = first_seen;
        }
        report.waiting = still.size();
        for (const auto& candidate : expired) {
            Statement remove(database, "DELETE FROM object_file WHERE sha256 = ?1");
            sqlite_support::bind_bytes(remove, 1, candidate.sha);
            remove.exec();
        }
        Statement store(database, "INSERT OR REPLACE INTO catalog_setting(key, schema_version, value_json) VALUES "
                                  "('gc.unreferenced_since', 1, ?1)");
        store.bind(1, still.dump());
        store.exec();
        return ok();
    });
    if (!marked)
        return std::unexpected(std::move(marked).error());
    // Files are removed only after the catalog no longer references them (never the reverse).
    for (const auto& candidate : expired) {
        std::error_code error;
        if (std::filesystem::remove(state.objects.object_path(candidate.sha, candidate.extension), error) || !error)
            ++report.deleted;
    }
    // Files no catalog row knows (a failed transaction after publishing, a conflicting commit's
    // object from before candidates were kept) are removed once they are older than the grace period.
    // The walk over every object runs without the exclusive lock (imports and saves continue); each
    // candidate is checked again under it right before removal.
    objects_guard.unlock();
    std::vector<std::pair<std::filesystem::path, Sha256Digest>> orphans;
    {
        auto reader = state.reader();
        std::error_code walk_error;
        const auto objects_root = state.objects.root() / "objects";
        for (auto it = std::filesystem::recursive_directory_iterator(objects_root, walk_error);
             !walk_error && it != std::filesystem::recursive_directory_iterator(); it.increment(walk_error)) {
            if (!reader || !it->is_regular_file(walk_error))
                continue;
            const auto stem = path_utf8(it->path().stem());
            const auto parsed_sha = parse_sha256_hex(stem);
            if (!parsed_sha)
                continue; // not an object file name
            const Sha256Digest sha = *parsed_sha;
            const bool known = sqlite_support::guarded("gc orphan check", [&]() -> Result<bool> {
                                   Statement query(***reader, "SELECT 1 FROM object_file WHERE sha256 = ?1");
                                   sqlite_support::bind_bytes(query, 1, sha);
                                   return query.executeStep();
                               }).value_or(true);
            if (known)
                continue;
            std::error_code age_error;
            const auto written = std::filesystem::last_write_time(it->path(), age_error);
            if (age_error)
                continue;
            const auto age = std::filesystem::file_time_type::clock::now() - written;
            const bool old = options.now_ms ? true : age > std::chrono::milliseconds(options.grace_period_ms);
            if (old)
                orphans.emplace_back(it->path(), sha);
        }
    }
    if (!orphans.empty()) {
        objects_guard.lock();
        auto reader = state.reader();
        for (const auto& [path, sha] : orphans) {
            const bool known = reader ? sqlite_support::guarded("gc orphan check", [&]() -> Result<bool> {
                                            Statement query(***reader, "SELECT 1 FROM object_file WHERE sha256 = ?1");
                                            sqlite_support::bind_bytes(query, 1, sha);
                                            return query.executeStep();
                                        }).value_or(true)
                                      : true;
            std::error_code remove_error;
            if (!known && std::filesystem::remove(path, remove_error))
                ++report.orphan_files;
        }
        objects_guard.unlock();
    }
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(state.objects.staging_directory(), error)) {
        std::error_code entry_error;
        const auto written = std::filesystem::last_write_time(entry.path(), entry_error);
        if (entry_error)
            continue;
        const auto age = std::filesystem::file_time_type::clock::now() - written;
        if (age > std::chrono::milliseconds(options.grace_period_ms) &&
            std::filesystem::remove(entry.path(), entry_error))
            ++report.staging_removed;
    }
    return report;
}

// --- Change outbox -----------------------------------------------------------------------------------

Result<ChangeSet> Library::changes_since(std::int64_t revision, std::size_t max_changes) {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    return sqlite_support::guarded("changes since", [&]() -> Result<ChangeSet> {
        ChangeSet changes;
        Statement latest(***reader, "SELECT coalesce(max(id), 0) FROM catalog_change");
        latest.executeStep();
        changes.latest_revision = latest.getColumn(0).getInt64();
        if (revision > changes.latest_revision) {
            changes.full_reload = true; // The catalog was replaced (restore); start over.
            return changes;
        }
        Statement query(***reader, "SELECT id, after_json FROM catalog_change WHERE id > ?1 ORDER BY id LIMIT ?2");
        query.bind(1, revision);
        query.bind(2, static_cast<std::int64_t>(max_changes + 1));
        std::set<std::int64_t> assets;
        std::size_t rows = 0;
        while (query.executeStep()) {
            if (++rows > max_changes) {
                changes.full_reload = true;
                break;
            }
            const auto after = Json::parse(query.getColumn(1).getString(), nullptr, false);
            if (!after.is_object()) {
                changes.full_reload = true;
                continue;
            }
            if (after.contains("structure") && after["structure"].is_boolean() && after["structure"].get<bool>())
                changes.structure_changed = true;
            if (after.contains("assets")) {
                if (after["assets"].is_array()) {
                    for (const auto& id : after["assets"])
                        if (id.is_number_integer())
                            assets.insert(id.get<std::int64_t>());
                } else {
                    changes.full_reload = true;
                }
            }
        }
        if (!changes.full_reload)
            changes.asset_ids.assign(assets.begin(), assets.end());
        return changes;
    });
}

Result<std::vector<SourceRootInfo>> Library::source_roots() {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    return sqlite_support::guarded("source roots", [&]() -> Result<std::vector<SourceRootInfo>> {
        Statement query(***reader, "SELECT id, display_path, watch_enabled FROM source_root ORDER BY id");
        std::vector<SourceRootInfo> roots;
        while (query.executeStep())
            roots.push_back(SourceRootInfo{query.getColumn(0).getInt64(), path_from_utf8_text(query.getColumn(1).getString()),
                                           query.getColumn(2).getInt() != 0});
        return roots;
    });
}

Result<std::size_t> Library::mark_sources_missing(const std::filesystem::path& root,
                                                  const std::vector<std::filesystem::path>& files) {
    auto& state = *state_;
    std::error_code error;
    const auto root_display = path_utf8(std::filesystem::absolute(root, error).lexically_normal());
    return state.write("source.missing", [&](SQLite::Database& database) -> Result<std::size_t> {
        Statement find(database, "SELECT id FROM source_root WHERE display_path = ?1");
        find.bind(1, root_display);
        if (!find.executeStep())
            return make_error(ErrorCode::not_found, "the folder is not a registered source", root_display);
        const auto root_id = find.getColumn(0).getInt64();
        Statement update(database, "UPDATE source_location SET state = 'missing', last_seen_ms = ?1 WHERE root_id = ?2 AND "
                                   "comparison_key = ?3 AND state <> 'missing' RETURNING asset_id");
        std::vector<std::int64_t> assets;
        for (const auto& file : files) {
            std::error_code file_error;
            const auto relative = std::filesystem::absolute(file, file_error).lexically_normal().lexically_relative(
                path_from_utf8_text(root_display));
            if (relative.empty() || *relative.begin() == "..")
                continue; // not inside this root
            update.reset();
            update.bind(1, now_utc_ms());
            update.bind(2, root_id);
            update.bind(3, filesystem_comparison_key(path_utf8(relative)));
            while (update.executeStep())
                assets.push_back(update.getColumn(0).getInt64());
        }
        if (!assets.empty())
            state.record_change(database, "source.missing", assets, false); // rows show "source missing"
        return assets.size();
    });
}

void Library::set_change_listener(std::function<void(std::int64_t)> listener) {
    std::lock_guard lock(state_->listener_mutex);
    state_->listener = std::move(listener);
}

// --- Search ---------------------------------------------------------------------------------------

Result<search::SearchOptions> Library::resolve_search_options(const search::Query& query,
                                                              std::optional<std::int64_t> scope,
                                                              bool across_sequences) {
    search::SearchOptions options;
    options.same_sequence = !across_sequences;
    options.collection_scope = scope;
    bool needs_collections = false;
    const std::function<void(const search::QueryNode&)> visit = [&](const search::QueryNode& node) {
        if (node.kind == search::NodeKind::predicate && node.predicate.field == search::Field::collection)
            needs_collections = true;
        for (const auto& child : node.children)
            visit(child);
    };
    visit(query.root);
    if (!needs_collections)
        return options;
    auto collections = list_collections();
    if (!collections)
        return std::unexpected(std::move(collections).error());
    for (const auto& collection : *collections)
        if (!collection.smart)
            options.collections[fold_for_search(trim(collection.name))].push_back(collection.id);
    return options;
}

Result<search::CompiledSearch> Library::compile_search(const SearchRequest& request) {
    auto query = search::parse_query(request.query_text);
    if (!query)
        return std::unexpected(query.error().to_error());
    auto options = resolve_search_options(*query, request.collection_scope, request.across_sequences);
    if (!options)
        return std::unexpected(std::move(options).error());
    return search::compile_page_query(*query, *options, request.order,
                                      request.order.field == search::SortField::relevance ? std::nullopt : request.after,
                                      request.page_size);
}

Result<SearchPage> Library::search(const SearchRequest& request, std::stop_token stop) {
    auto query = search::parse_query(request.query_text);
    if (!query)
        return std::unexpected(query.error().to_error());
    auto options = resolve_search_options(*query, request.collection_scope, request.across_sequences);
    if (!options)
        return std::unexpected(std::move(options).error());
    if (request.use_browse_index && request.order.field != search::SortField::relevance)
        if (auto served = search_browse_index(*query, *options, request))
            return std::move(*served);
    auto compiled = search::compile_page_query(*query, *options, request.order,
                                               request.order.field == search::SortField::relevance ? std::nullopt
                                                                                                   : request.after,
                                               request.page_size);
    if (!compiled)
        return std::unexpected(std::move(compiled).error());
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    auto rows = sqlite_support::guarded("search", [&]() -> Result<std::vector<SearchRow>> {
        ProgressInterrupt interrupt(***reader, stop);
        Statement statement(***reader, compiled->statement.sql);
        bind_parameters(statement, compiled->statement.parameters);
        std::vector<SearchRow> result;
        while (statement.executeStep()) {
            SearchRow row;
            row.id = statement.getColumn(0).getInt64();
            row.sort_title = statement.getColumn(1).getString();
            row.modified_ms = statement.getColumn(2).getInt64();
            row.created_ms = statement.getColumn(3).getInt64();
            row.rank = column_optional_double(statement, 4);
            result.push_back(std::move(row));
        }
        return result;
    });
    if (!rows) {
        if (stop.stop_requested())
            return make_error(ErrorCode::cancelled, "search cancelled");
        return std::unexpected(std::move(rows).error());
    }
    SearchPage page;
    const auto limit = std::min(request.page_size, search::maximum_page_size);
    if (compiled->relevance) {
        // Bounded materialized result in (rank, id) order, paged by keyset in memory.
        page.truncated = rows->size() >= compiled->row_limit;
        std::ranges::stable_sort(*rows, [](const SearchRow& left, const SearchRow& right) {
            if (*left.rank != *right.rank)
                return *left.rank < *right.rank;
            return left.id < right.id;
        });
        for (const auto& row : *rows) {
            if (request.after) {
                const auto* cursor_rank = std::get_if<double>(&request.after->sort_value);
                if (cursor_rank == nullptr)
                    return make_error(ErrorCode::invalid_argument, "the page cursor does not match the sort order");
                if (std::tie(*row.rank, row.id) <= std::tie(*cursor_rank, request.after->id))
                    continue;
            }
            if (page.rows.size() == limit)
                break;
            page.rows.push_back(row);
        }
        if (page.rows.size() == limit)
            page.next = search::PageCursor{*page.rows.back().rank, page.rows.back().id};
        return page;
    }
    page.rows = std::move(*rows);
    if (page.rows.size() == compiled->row_limit) {
        const auto& last = page.rows.back();
        search::SqlValue key;
        switch (request.order.field) {
        case search::SortField::modified:
            key = last.modified_ms;
            break;
        case search::SortField::created:
            key = last.created_ms;
            break;
        case search::SortField::title:
        case search::SortField::relevance:
            key = last.sort_title;
            break;
        }
        page.next = search::PageCursor{std::move(key), last.id};
    }
    return page;
}

std::optional<SearchPage> Library::search_browse_index(const search::Query& query, const search::SearchOptions& options,
                                                       const SearchRequest& request) {
    auto& cache = state_->browse;
    std::function<void()> rebuild; // called after the lock is released
    struct Notify {
        std::function<void()>& callback;
        ~Notify() {
            if (callback)
                callback();
        }
    } notify{rebuild};
    std::lock_guard lock(cache.mutex);
    if (!cache.index)
        return std::nullopt;
    // Catch up with the change outbox first, so a page never shows a state older than SQL would.
    auto changes = changes_since(cache.revision);
    if (!changes)
        return std::nullopt;
    if (changes->full_reload) {
        cache.index.reset(); // too much changed (or the catalog was restored): SQL until rebuilt
        rebuild = cache.dropped;
        return std::nullopt;
    }
    if (!changes->asset_ids.empty()) {
        auto rows = browse_rows_for(changes->asset_ids);
        if (!rows)
            return std::nullopt;
        std::set<std::int64_t> present;
        for (const auto& row : *rows) {
            cache.index->upsert(row);
            present.insert(row.id);
        }
        for (const auto id : changes->asset_ids)
            if (!present.contains(id))
                cache.index->remove(id); // purged
    }
    // Rows were read after the outbox position, so they are at least that new; re-applying a
    // later change is an idempotent upsert.
    cache.revision = changes->latest_revision;
    if (cache.index->over_budget()) {
        cache.index.reset();
        cache.over_budget = true;
        return std::nullopt;
    }
    auto page = cache.index->page(query, options, request.order, request.after, request.page_size);
    if (!page)
        return std::nullopt; // relevance or text only SQLite tokenizes exactly: SQL answers
    SearchPage result;
    result.from_browse_index = true;
    for (const auto id : page->ids) {
        const auto* row = cache.index->find(id);
        if (row == nullptr)
            continue;
        result.rows.push_back(SearchRow{row->id, row->sort_title, row->modified_ms, row->created_ms, std::nullopt});
    }
    result.next = page->next;
    return result;
}

Result<BrowseIndexStatus> Library::build_browse_index(std::stop_token stop, std::size_t budget_bytes) {
    // The outbox position is taken before the first page: changes during the build are applied
    // again by the next search (idempotent upserts), so nothing committed meanwhile is missed.
    std::int64_t revision = 0;
    std::size_t total_rows = 0;
    {
        auto reader = state_->reader();
        if (!reader)
            return std::unexpected(std::move(reader).error());
        auto latest = sqlite_support::guarded("browse index position", [&]() -> Result<std::int64_t> {
            Statement statement(***reader, "SELECT coalesce(max(id), 0) FROM catalog_change");
            statement.executeStep();
            Statement count(***reader, "SELECT count(*) FROM asset");
            count.executeStep();
            total_rows = static_cast<std::size_t>(count.getColumn(0).getInt64());
            return statement.getColumn(0).getInt64();
        });
        if (!latest)
            return std::unexpected(std::move(latest).error());
        revision = *latest;
    }
    auto index = std::make_unique<search::BrowseIndex>(budget_bytes);
    BrowseIndexStatus status;
    std::int64_t after = 0;
    while (true) {
        if (stop.stop_requested())
            return make_error(ErrorCode::cancelled, "browse index build cancelled");
        auto rows = browse_rows(after, 1000); // short read transactions; writers are never blocked long
        if (!rows)
            return std::unexpected(std::move(rows).error());
        if (rows->empty())
            break;
        for (const auto& row : *rows)
            index->upsert(row);
        after = rows->back().id;
        // Measured size per row so far, projected onto the whole catalog: a library that will not
        // fit stops after one page instead of loading up to the budget and discarding it.
        const auto projected = index->memory_bytes() / std::max<std::size_t>(1, index->size()) * total_rows;
        if (index->over_budget() || (index->size() >= 1000 && projected > budget_bytes)) {
            status.over_budget = true; // stop loading: SQL paging serves this library
            break;
        }
    }
    status.rows = index->size();
    status.memory_bytes = index->memory_bytes();
    auto& cache = state_->browse;
    std::lock_guard lock(cache.mutex);
    cache.over_budget = status.over_budget;
    if (status.over_budget) {
        cache.index.reset();
        return status;
    }
    cache.index = std::move(index);
    cache.revision = revision;
    status.ready = true;
    return status;
}

void Library::set_browse_index_dropped_listener(std::function<void()> listener) {
    auto& cache = state_->browse;
    std::lock_guard lock(cache.mutex);
    cache.dropped = std::move(listener);
}

BrowseIndexStatus Library::browse_index_status() {
    auto& cache = state_->browse;
    std::lock_guard lock(cache.mutex);
    BrowseIndexStatus status;
    status.over_budget = cache.over_budget;
    if (cache.index) {
        status.ready = true;
        status.rows = cache.index->size();
        status.memory_bytes = cache.index->memory_bytes();
    }
    return status;
}

namespace {

std::vector<double> json_numbers(const Json& object, const char* key) {
    std::vector<double> values;
    if (object.contains(key) && object[key].is_array())
        for (const auto& value : object[key])
            if (value.is_number())
                values.push_back(value.get<double>());
    return values;
}

std::vector<std::string> json_strings(const Json& object, const char* key) {
    std::vector<std::string> values;
    if (object.contains(key) && object[key].is_array())
        for (const auto& value : object[key])
            if (value.is_string())
                values.push_back(value.get<std::string>());
    return values;
}

/// The statements a browse row needs, prepared once per page and reset per row (preparing them
/// for every row was ~8 prepares per asset, code-library-11).
struct BrowseStatements {
    explicit BrowseStatements(SQLite::Database& database)
        : tags(database, "SELECT t.normalized_name FROM asset_tag AS at JOIN tag AS t ON t.id = at.tag_id WHERE "
                         "at.asset_id = ?1"),
          collections(database, "SELECT collection_id FROM collection_asset WHERE asset_id = ?1"),
          missing(database, "SELECT 1 FROM source_location WHERE asset_id = ?1 AND state IN ('missing', 'offline') "
                            "LIMIT 1"),
          text(database, "SELECT title, source_names, tags, description, lyrics FROM asset_fts WHERE rowid = ?1"),
          run(database, "SELECT status, json_extract(result_json, '$.state') FROM analysis_run WHERE revision_id = "
                        "?1 AND analyzer = 'basic' AND status IN ('complete', 'failed') LIMIT 1"),
          sequences(database, "SELECT sequence_no, tempo_representative, tempo_min, tempo_max, tempo_origin, "
                              "key_root, key_mode, key_origin, key_confidence, quarter_length, duration_seconds, "
                              "track_count, note_count, min_pitch, max_pitch, has_drums, diagnostics_json FROM "
                              "sequence_summary WHERE revision_id = ?1 ORDER BY sequence_no"),
          tracks(database, "SELECT channel_mask, program_json FROM track_summary WHERE revision_id = ?1 AND "
                           "sequence_no = ?2 ORDER BY track_no") {}

    Statement tags;
    Statement collections;
    Statement missing;
    Statement text;
    Statement run;
    Statement sequences;
    Statement tracks;
};

/// Loads one browse row on a connection inside the caller's read transaction.
search::BrowseRow load_browse_row(BrowseStatements& statements, const Statement& asset) {
    search::BrowseRow row;
    row.id = asset.getColumn(0).getInt64();
    row.sort_title = asset.getColumn(1).getString();
    row.modified_ms = asset.getColumn(2).getInt64();
    row.created_ms = asset.getColumn(3).getInt64();
    row.deleted = !asset.getColumn(4).isNull();
    row.role = asset.getColumn(5).getString();
    row.favorite = asset.getColumn(6).getInt() != 0;
    row.rating = asset.getColumn(7).getInt();
    const auto head = column_optional_int(asset, 8);

    for (Statement* statement : {&statements.tags, &statements.collections, &statements.missing, &statements.text,
                                 &statements.run, &statements.sequences}) {
        statement->reset();
        statement->clearBindings();
    }
    auto& tags = statements.tags;
    tags.bind(1, row.id);
    while (tags.executeStep())
        row.tags.push_back(tags.getColumn(0).getString());
    auto& collections = statements.collections;
    collections.bind(1, row.id);
    while (collections.executeStep())
        row.collections.push_back(collections.getColumn(0).getInt64());
    auto& missing = statements.missing;
    missing.bind(1, row.id);
    row.source_missing = missing.executeStep();
    auto& text = statements.text;
    text.bind(1, row.id);
    if (text.executeStep())
        for (int column = 0; column < static_cast<int>(search::fts_column_count); ++column)
            row.text[static_cast<std::size_t>(column)] = text.getColumn(column).getString();
    if (!head)
        return row;
    auto& run = statements.run;
    run.bind(1, *head);
    if (run.executeStep()) {
        if (run.getColumn(0).getString() == "failed")
            row.analysis = search::AnalysisState::error;
        else if (!run.getColumn(1).isNull() && run.getColumn(1).getString() == "ready")
            row.analysis = search::AnalysisState::ready;
        else if (!run.getColumn(1).isNull() && run.getColumn(1).getString() == "warning")
            row.analysis = search::AnalysisState::warning;
        else
            row.analysis = search::AnalysisState::indexing;
    }
    auto& sequences = statements.sequences;
    sequences.bind(1, *head);
    while (sequences.executeStep()) {
        search::SequenceFacts facts;
        const auto sequence_no = sequences.getColumn(0).getInt64();
        facts.tempo_representative = column_optional_double(sequences, 1);
        facts.tempo_min = column_optional_double(sequences, 2);
        facts.tempo_max = column_optional_double(sequences, 3);
        facts.tempo_origin = sequences.getColumn(4).getString();
        if (const auto root = column_optional_int(sequences, 5))
            facts.key_root = static_cast<int>(*root);
        if (!sequences.getColumn(6).isNull())
            facts.key_mode = sequences.getColumn(6).getString();
        facts.key_origin = sequences.getColumn(7).getString();
        facts.key_confidence = column_optional_double(sequences, 8);
        facts.quarter_length = column_optional_double(sequences, 9);
        facts.duration_seconds = sequences.getColumn(10).getDouble();
        facts.track_count = sequences.getColumn(11).getInt64();
        facts.note_count = sequences.getColumn(12).getInt64();
        if (const auto low = column_optional_int(sequences, 13))
            facts.min_pitch = static_cast<int>(*low);
        if (const auto high = column_optional_int(sequences, 14))
            facts.max_pitch = static_cast<int>(*high);
        facts.has_drums = sequences.getColumn(15).getInt() != 0;
        const auto diagnostics = Json::parse(sequences.getColumn(16).getString(), nullptr, false);
        if (diagnostics.is_object()) {
            facts.tempos = json_numbers(diagnostics, "tempos");
            facts.meters = json_strings(diagnostics, "meter_names");
            facts.event_classes = json_strings(diagnostics, "event_classes");
        }
        auto& tracks = statements.tracks;
        tracks.reset();
        tracks.bind(1, *head);
        tracks.bind(2, sequence_no);
        while (tracks.executeStep()) {
            search::TrackFacts track;
            track.channel_mask = static_cast<std::uint16_t>(tracks.getColumn(0).getInt64());
            const auto programs = Json::parse(tracks.getColumn(1).getString(), nullptr, false);
            if (programs.is_array())
                for (const auto& program : programs)
                    if (program.is_object() && program.contains("program") && program["program"].is_number_integer())
                        track.programs.push_back(static_cast<std::uint8_t>(program["program"].get<int>()));
            facts.tracks.push_back(std::move(track));
        }
        row.sequences.push_back(std::move(facts));
    }
    return row;
}

constexpr std::string_view browse_columns = "SELECT id, sort_title, modified_ms, created_ms, deleted_ms, role, favorite, "
                                            "rating, current_revision_id FROM asset";

} // namespace

Result<std::vector<search::BrowseRow>> Library::browse_rows(std::int64_t after_id, std::size_t limit) {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    return sqlite_support::guarded("browse rows", [&]() -> Result<std::vector<search::BrowseRow>> {
        SQLite::Transaction snapshot(***reader); // One short consistent read; committed below.
        Statement assets(***reader, std::string(browse_columns) + " WHERE id > ?1 ORDER BY id LIMIT ?2");
        assets.bind(1, after_id);
        assets.bind(2, static_cast<std::int64_t>(limit));
        std::vector<search::BrowseRow> rows;
        BrowseStatements statements(***reader);
        while (assets.executeStep())
            rows.push_back(load_browse_row(statements, assets));
        snapshot.commit();
        return rows;
    });
}

Result<std::vector<search::BrowseRow>> Library::browse_rows_for(const std::vector<std::int64_t>& asset_ids) {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    return sqlite_support::guarded("browse rows", [&]() -> Result<std::vector<search::BrowseRow>> {
        SQLite::Transaction snapshot(***reader);
        std::vector<search::BrowseRow> rows;
        BrowseStatements statements(***reader);
        Statement asset(***reader, std::string(browse_columns) + " WHERE id = ?1");
        for (const auto asset_id : asset_ids) {
            asset.reset();
            asset.bind(1, asset_id);
            if (asset.executeStep())
                rows.push_back(load_browse_row(statements, asset));
        }
        snapshot.commit();
        return rows;
    });
}

// --- Analysis and motif search ------------------------------------------------------------------

namespace {

constexpr std::int32_t derived_analysis_version = 1;

/// Writes one motif stream with its grams; returns the gram count (analysis and the M10K fixture).
std::size_t insert_motif_stream(SQLite::Database& database, std::int64_t revision, const search::MotifStream& stream) {
    const auto payload = search::encode_motif_stream(stream);
    Statement insert(database, "INSERT INTO motif_stream(revision_id, sequence_no, track_no, channel, voice_kind, "
                               "profile_version, note_count, stream_sha256, codec_version, payload) VALUES (?1, ?2, "
                               "?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)");
    insert.bind(1, revision);
    insert.bind(2, static_cast<std::int64_t>(stream.sequence_no));
    insert.bind(3, static_cast<std::int64_t>(stream.track_no));
    insert.bind(4, static_cast<std::int64_t>(stream.channel));
    insert.bind(5, std::string(search::voice_kind_name(stream.voice)));
    insert.bind(6, static_cast<std::int64_t>(stream.profile_version));
    insert.bind(7, static_cast<std::int64_t>(stream.notes.size()));
    sqlite_support::bind_bytes(insert, 8, sha256(payload));
    insert.bind(9, static_cast<std::int64_t>(search::motif_codec_version));
    sqlite_support::bind_bytes(insert, 10, payload);
    insert.exec();
    const auto stream_id = database.getLastInsertRowid();
    Statement gram(database, "INSERT INTO motif_gram(stream_id, family, gram_length, gram_hash, start_ordinal, "
                             "start_tick) VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
    std::size_t count = 0;
    for (const auto& item_gram : search::stream_grams(stream)) {
        gram.reset();
        gram.bind(1, stream_id);
        gram.bind(2, std::string(search::token_family_name(item_gram.key.family)));
        gram.bind(3, static_cast<std::int64_t>(item_gram.key.length));
        const auto hash = search::gram_hash_bytes(item_gram.key.hash);
        sqlite_support::bind_bytes(gram, 4, hash);
        gram.bind(5, static_cast<std::int64_t>(item_gram.start_ordinal));
        gram.bind(6, item_gram.start_tick);
        gram.exec();
        ++count;
    }
    return count;
}

/// Motif index over the catalog: only current heads of active assets, one profile version.
class CatalogMotifIndex final : public search::MotifIndexReader {
public:
    explicit CatalogMotifIndex(SQLite::Database& database) : database_(database) {}

    Result<std::uint64_t> count_postings(const search::GramKey& key, std::uint64_t cap) override {
        return sqlite_support::guarded("motif postings", [&]() -> Result<std::uint64_t> {
            // Ranking estimate from the covering index alone, capped: no join per posting, and a
            // ubiquitous gram costs at most `cap` index steps. postings() still filters to heads.
            Statement query(database_, "SELECT count(*) FROM (SELECT 1 FROM motif_gram INDEXED BY gram_lookup WHERE "
                                       "family = ?1 AND gram_length = ?2 AND gram_hash = ?3 LIMIT ?4)");
            query.bind(1, std::string(search::token_family_name(key.family)));
            query.bind(2, static_cast<std::int64_t>(key.length));
            const auto bytes = search::gram_hash_bytes(key.hash);
            sqlite_support::bind_bytes(query, 3, bytes);
            query.bind(4, static_cast<std::int64_t>(std::min<std::uint64_t>(cap, std::numeric_limits<std::int64_t>::max())));
            query.executeStep();
            return static_cast<std::uint64_t>(query.getColumn(0).getInt64());
        });
    }

    Result<std::vector<search::GramPosting>> postings(const search::GramKey& key, std::size_t limit) override {
        return sqlite_support::guarded("motif postings", [&]() -> Result<std::vector<search::GramPosting>> {
            Statement query(database_, std::string(motif_postings_sql));
            bind_key(query, key);
            query.bind(5, static_cast<std::int64_t>(limit));
            std::vector<search::GramPosting> result;
            while (query.executeStep())
                result.push_back(search::GramPosting{query.getColumn(0).getInt64(),
                                                     static_cast<std::uint32_t>(query.getColumn(1).getInt64())});
            return result;
        });
    }

    Result<std::optional<search::StoredMotifStream>> load_stream(std::int64_t stream_id) override {
        return sqlite_support::guarded("motif stream", [&]() -> Result<std::optional<search::StoredMotifStream>> {
            Statement query(database_, std::string(motif_stream_sql));
            query.bind(1, stream_id);
            if (!query.executeStep())
                return std::optional<search::StoredMotifStream>{};
            const auto payload = sqlite_support::column_bytes(query, 2);
            auto stream = search::decode_motif_stream(payload);
            if (!stream) {
                // One damaged stream must not fail every search (code-library-9): it is skipped,
                // counted, and the result says it is partial. `library repair --rebuild-derived`
                // rebuilds the index.
                ++skipped_;
                return std::optional<search::StoredMotifStream>{};
            }
            search::StoredMotifStream stored;
            stored.info.stream_id = stream_id;
            stored.info.revision_id = query.getColumn(0).getInt64();
            stored.info.asset_id = query.getColumn(1).getInt64();
            stored.info.sequence_no = stream->sequence_no;
            stored.info.track_no = stream->track_no;
            stored.info.channel = stream->channel;
            stored.stream = std::move(*stream);
            return std::optional<search::StoredMotifStream>{std::move(stored)};
        });
    }

    Result<std::vector<std::int64_t>> stream_ids(std::int64_t after_id, std::size_t limit) override {
        return sqlite_support::guarded("motif streams", [&]() -> Result<std::vector<std::int64_t>> {
            Statement query(database_, std::string(motif_stream_ids_sql));
            query.bind(1, after_id);
            query.bind(2, static_cast<std::int64_t>(search::motif_profile_version));
            query.bind(3, static_cast<std::int64_t>(limit));
            std::vector<std::int64_t> ids;
            while (query.executeStep())
                ids.push_back(query.getColumn(0).getInt64());
            return ids;
        });
    }

    [[nodiscard]] std::size_t skipped() const { return skipped_; }

private:
    static void bind_key(Statement& query, const search::GramKey& key) {
        query.bind(1, std::string(search::token_family_name(key.family)));
        query.bind(2, static_cast<std::int64_t>(key.length));
        const auto bytes = search::gram_hash_bytes(key.hash);
        sqlite_support::bind_bytes(query, 3, bytes);
        query.bind(4, static_cast<std::int64_t>(search::motif_profile_version));
    }

    SQLite::Database& database_;
    std::size_t skipped_ = 0;
};

/// Active heads without a complete derived analysis: never searched musically.
std::size_t count_unindexed_heads(SQLite::Database& database) {
    return sqlite_support::guarded("unindexed heads", [&]() -> Result<std::size_t> {
               Statement query(database, "SELECT count(*) FROM asset AS a WHERE a.deleted_ms IS NULL AND "
                                         "a.current_revision_id IS NOT NULL AND NOT EXISTS (SELECT 1 FROM analysis_run AS x "
                                         "WHERE x.revision_id = a.current_revision_id AND x.analyzer = 'derived' AND "
                                         "x.status = 'complete')");
               query.executeStep();
               return static_cast<std::size_t>(query.getColumn(0).getInt64());
           })
        .value_or(0);
}

/// Marks a motif or similar result partial when some items were not searched (spec 26.6: never
/// imply that unindexed assets were searched musically).
template <typename Result>
void report_coverage(Result& result, std::size_t unindexed, std::size_t skipped) {
    result.unindexed_heads = unindexed;
    result.skipped_streams = skipped;
    std::vector<std::string> reasons;
    if (!result.partial_reason.empty())
        reasons.push_back(result.partial_reason);
    if (unindexed > 0)
        reasons.push_back(std::format("{} item(s) not indexed for melody search yet", unindexed));
    if (skipped > 0)
        reasons.push_back(std::format("{} stored stream(s) could not be read", skipped));
    if (unindexed > 0 || skipped > 0) {
        result.partial = true;
        std::string joined;
        for (const auto& reason : reasons)
            joined += (joined.empty() ? "" : "; ") + reason;
        result.partial_reason = joined;
    }
}

} // namespace

Result<AnalyzeReport> Library::analyze(const AnalyzeOptions& options, std::stop_token stop) {
    AnalyzeReport report;
    auto& state = *state_;
    std::int64_t after_revision = 0;
    // A run with some profiles off is recorded as "derived-partial", so it never makes a later
    // full analysis skip the revision.
    const bool full_profile = options.fingerprints && options.motif && options.features;
    const std::string analyzer_name = full_profile ? "derived" : "derived-partial";
    const auto settings = analyzer_settings_hash(analyzer_name, derived_analysis_version);
    while (true) {
        if (stop.stop_requested()) {
            report.cancelled = true;
            break;
        }
        struct Pending {
            std::int64_t revision;
            Sha256Digest sha;
            MediaType media;
            std::string extension;
        };
        std::vector<Pending> batch;
        {
            auto reader = state.reader();
            if (!reader)
                return std::unexpected(std::move(reader).error());
            auto loaded = sqlite_support::guarded("analysis batch", [&]() -> Result<std::vector<Pending>> {
                // Driven by the revision key (SEARCH r USING INTEGER PRIMARY KEY), so each batch
                // reads only its own rows instead of every active asset (code-library-6).
                Statement query(***reader, std::string(analysis_batch_sql));
                query.bind(1, after_revision);
                query.bind(2, derived_analysis_version);
                query.bind(3, static_cast<std::int64_t>(std::max<std::size_t>(1, options.batch_size)));
                std::vector<Pending> pending;
                while (query.executeStep()) {
                    auto digest = sqlite_support::column_digest(query, 1, "object hash");
                    if (!digest)
                        return std::unexpected(std::move(digest).error());
                    pending.push_back(Pending{query.getColumn(0).getInt64(), *digest,
                                              parse_media_type(query.getColumn(2).getString()).value_or(MediaType::smf),
                                              query.getColumn(3).getString()});
                }
                return pending;
            });
            if (!loaded)
                return std::unexpected(std::move(loaded).error());
            batch = std::move(*loaded);
        }
        if (batch.empty())
            break;
        for (const auto& item : batch) {
            after_revision = item.revision;
            if (stop.stop_requested()) {
                report.cancelled = true;
                break;
            }
            std::optional<DocumentData> document;
            std::string failure;
            // A read error that may pass (a locked or briefly missing file) is retried by a later
            // run instead of marking the revision failed forever (code-library-5).
            bool retry_later = false;
            auto bytes = state.objects.read(item.sha, item.extension);
            if (!bytes) {
                failure = bytes.error().message;
                retry_later = bytes.error().code != ErrorCode::invalid_data;
            } else if (item.media == MediaType::native) {
                auto loaded = document::native_from_bytes(*bytes);
                if (loaded)
                    document = std::move(*loaded);
                else
                    failure = loaded.error().message;
            } else if (midi::detect_container(*bytes) == midi::Container::unknown) {
                failure = "unsupported content";
            } else {
                // The same untrusted bytes as import: parse them through the caller's parser (the
                // isolated worker in the application, section 28.2), never unconditionally in process.
                auto imported = options.parser ? options.parser(*bytes, "analysis", options.limits, stop)
                                               : midi::import_midi(*bytes, "analysis", options.limits, stop);
                if (imported) {
                    document = std::move(imported->document);
                } else if (stop.stop_requested() || imported.error().code == ErrorCode::cancelled) {
                    // Cancelled, not broken: nothing is recorded, a later run analyzes it.
                    report.cancelled = true;
                    break;
                } else {
                    failure = imported.error().message;
                }
            }
            std::vector<Fingerprint> fingerprints;
            std::vector<search::MotifStream> streams;
            std::vector<FeatureVector> features;
            if (document) {
                if (options.fingerprints)
                    fingerprints = compute_fingerprints(*document, item.sha);
                if (options.motif) {
                    auto extracted = compute_motif_streams(*document);
                    if (extracted)
                        streams = std::move(*extracted);
                    else
                        failure = extracted.error().message;
                }
                if (options.features) {
                    auto computed = compute_feature_vectors(*document);
                    if (computed)
                        features = std::move(*computed);
                    else
                        failure = computed.error().message;
                }
                if (!failure.empty()) {
                    // A partial derived set is never published as complete (section 28.1).
                    document.reset();
                    fingerprints.clear();
                    streams.clear();
                    features.clear();
                }
            }
            std::size_t gram_count = 0;
            std::size_t planned_grams = 0;
            for (const auto& stream : streams)
                planned_grams += search::stream_grams(stream).size();
            bool over_budget = false;
            auto written = state.write("analysis.derived", [&](SQLite::Database& database) -> Status {
                // About 64 bytes per gram row with its two B-tree entries: stop before the derived
                // index would pass its budget (spec 26.6/28.3), the revision stays unanalyzed.
                constexpr std::uint64_t bytes_per_gram = 64;
                const auto rows = static_cast<std::uint64_t>(state.derived_gram_rows(database));
                if ((rows + planned_grams) * bytes_per_gram > options.budget_bytes && planned_grams > 0) {
                    over_budget = true;
                    return ok();
                }
                state.delete_derived(database, item.revision);
                for (const auto& fingerprint : fingerprints) {
                    Statement insert(database, "INSERT INTO fingerprint(revision_id, profile, profile_version, sha256) "
                                               "VALUES (?1, ?2, ?3, ?4)");
                    insert.bind(1, item.revision);
                    insert.bind(2, fingerprint.profile);
                    insert.bind(3, fingerprint_profile_version);
                    sqlite_support::bind_bytes(insert, 4, fingerprint.sha256);
                    insert.exec();
                }
                for (const auto& stream : streams)
                    gram_count += insert_motif_stream(database, item.revision, stream);
                for (const auto& feature : features) {
                    Statement insert(database, "INSERT INTO feature_vector(revision_id, sequence_no, profile_version, "
                                               "dimension, payload) VALUES (?1, ?2, ?3, ?4, ?5)");
                    insert.bind(1, item.revision);
                    insert.bind(2, static_cast<std::int64_t>(feature.sequence_no));
                    insert.bind(3, feature_profile_version);
                    insert.bind(4, static_cast<std::int64_t>(feature.values.size()));
                    sqlite_support::bind_bytes(insert, 5, feature.payload());
                    insert.exec();
                }
                Statement run(database, "INSERT OR REPLACE INTO analysis_run(revision_id, analyzer, version, "
                                        "settings_sha256, status, completed_ms, result_json) VALUES (?1, ?7, ?2, "
                                        "?3, ?4, ?5, ?6)");
                run.bind(7, analyzer_name);
                run.bind(1, item.revision);
                run.bind(2, derived_analysis_version);
                sqlite_support::bind_bytes(run, 3, settings);
                run.bind(4, std::string(document ? "complete" : retry_later ? "cancelled" : "failed"));
                run.bind(5, now_utc_ms());
                Json result{{"version", 1},
                            {"fingerprints", fingerprints.size()},
                            {"streams", streams.size()},
                            {"grams", gram_count},
                            {"features", features.size()}};
                if (!failure.empty())
                    result["reason"] = failure;
                run.bind(6, result.dump());
                run.exec();
                state.store_gram_rows(database, state.derived_gram_rows(database) + static_cast<std::int64_t>(gram_count));
                return ok();
            });
            if (!written)
                return std::unexpected(std::move(written).error());
            if (over_budget) {
                report.budget_reached = true;
                break;
            }
            if (document) {
                ++report.revisions_analyzed;
                report.streams += streams.size();
                report.grams += gram_count;
            } else {
                ++report.revisions_failed;
            }
            if (options.progress)
                options.progress(report.revisions_analyzed + report.revisions_failed);
        }
        if (report.cancelled || report.budget_reached)
            break;
    }
    return report;
}

Result<std::size_t> Library::rebuild_derived() {
    auto& state = *state_;
    return state.write("analysis.rebuild", [&](SQLite::Database& database) -> Result<std::size_t> {
        std::size_t removed = 0;
        for (const auto* table : {"fingerprint", "motif_stream", "feature_vector"}) {
            Statement clear(database, std::format("DELETE FROM {}", table));
            removed += static_cast<std::size_t>(clear.exec());
        }
        Statement runs(database, "DELETE FROM analysis_run WHERE analyzer IN ('derived', 'derived-partial')");
        runs.exec();
        state.store_gram_rows(database, 0);
        return removed;
    });
}

Result<search::ExactSearchResult> Library::motif_search(const search::ExactQuery& query, std::stop_token stop) {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    ProgressInterrupt interrupt(***reader, stop); // long SQL steps end when the search is cancelled
    CatalogMotifIndex index(***reader);
    auto result = search::exact_search(index, query, std::move(stop));
    if (result)
        report_coverage(*result, count_unindexed_heads(***reader), index.skipped());
    return result;
}

Result<search::ApproximateSearchResult> Library::similar_search(const search::ApproximateQuery& query,
                                                                std::stop_token stop) {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    ProgressInterrupt interrupt(***reader, stop);
    CatalogMotifIndex index(***reader);
    auto result = search::approximate_search(index, query, std::move(stop));
    if (result)
        report_coverage(*result, count_unindexed_heads(***reader), index.skipped());
    return result;
}

// --- Jobs -------------------------------------------------------------------------------------------

Result<std::int64_t> Library::enqueue_job(const std::string& kind, const std::string& idempotency_key,
                                          const std::string& arguments_json) {
    auto& state = *state_;
    return state.write("job.enqueue", [&](SQLite::Database& database) -> Result<std::int64_t> {
        Statement insert(database, "INSERT INTO job(kind, idempotency_key, arguments_json, state, created_ms, updated_ms) "
                                   "VALUES (?1, ?2, ?3, 'queued', ?4, ?4) ON CONFLICT(idempotency_key) DO NOTHING");
        insert.bind(1, kind);
        insert.bind(2, idempotency_key);
        insert.bind(3, arguments_json);
        insert.bind(4, now_utc_ms());
        insert.exec();
        Statement find(database, "SELECT id FROM job WHERE idempotency_key = ?1");
        find.bind(1, idempotency_key);
        find.executeStep();
        return find.getColumn(0).getInt64();
    });
}

Result<std::optional<Job>> Library::lease_job() {
    auto& state = *state_;
    return state.write("job.lease", [&](SQLite::Database& database) -> Result<std::optional<Job>> {
        Statement lease(database, std::string(lease_job_sql));
        lease.bind(1, now_utc_ms());
        if (!lease.executeStep())
            return std::optional<Job>{};
        Job job{lease.getColumn(0).getInt64(), lease.getColumn(1).getString(), lease.getColumn(2).getString(),
                lease.getColumn(3).getString(), lease.getColumn(4).getInt64()};
        while (lease.executeStep()) {
        }
        return std::optional<Job>{std::move(job)};
    });
}

Status Library::finish_job(std::int64_t job_id, bool success, const std::string& error_json) {
    auto& state = *state_;
    return state.write("job.finish", [&](SQLite::Database& database) -> Status {
        Statement update(database, "UPDATE job SET state = ?1, updated_ms = ?2, error_json = ?3 WHERE id = ?4 AND state = "
                                   "'running'");
        update.bind(1, std::string(success ? "complete" : "failed"));
        update.bind(2, now_utc_ms());
        if (error_json.empty())
            update.bind(3);
        else
            update.bind(3, error_json);
        update.bind(4, job_id);
        if (update.exec() == 0)
            return make_error(ErrorCode::not_found, "no running job with this ID");
        return ok();
    });
}

std::vector<search::QueryNote> synthetic_planted_motif() {
    // A fixed, distinctive 32-note line (wide leaps, uneven rhythm) that a random walk of small
    // steps practically never reproduces, so its hits are exactly the planted ones.
    static constexpr std::array<int, 32> pitches{60, 67, 63, 72, 58, 65, 70, 61, 68, 64, 75, 59, 66, 71, 62, 69,
                                                 57, 73, 64, 60, 76, 63, 67, 58, 74, 61, 69, 65, 72, 59, 70, 62};
    static constexpr std::array<Tick, 4> steps{240, 480, 240, 720};
    std::vector<search::QueryNote> notes;
    Tick onset = 0;
    for (std::size_t index = 0; index < pitches.size(); ++index) {
        const auto step = steps[index % steps.size()];
        notes.push_back(search::QueryNote{onset, step - 40, static_cast<std::uint8_t>(pitches[index]), 96});
        onset += step;
    }
    return notes;
}

Result<std::size_t> Library::populate_synthetic_motifs(std::size_t count, std::uint64_t seed, std::stop_token stop) {
    auto& state = *state_;
    const auto division = Division::make_ppq(default_ppq);
    const auto settings_hash = search::extraction_settings_hash(search::ExtractionSettings{}, division);
    const auto run_settings = analyzer_settings_hash("derived", derived_analysis_version);
    const auto planted = synthetic_planted_motif();
    DeterministicRandom random(seed);
    const auto pick = [&](std::uint32_t bound) { return static_cast<std::int64_t>(random.bounded(bound)); };
    std::size_t written = 0;
    while (written < count) {
        if (stop.stop_requested())
            return make_error(ErrorCode::cancelled, "synthetic population cancelled");
        auto inserted = state.write("bench.motifs", [&](SQLite::Database& database) -> Result<std::size_t> {
            Statement heads(database, std::string(analysis_batch_sql));
            heads.bind(1, std::int64_t{0});
            heads.bind(2, derived_analysis_version);
            heads.bind(3, static_cast<std::int64_t>(std::min<std::size_t>(500, count - written)));
            std::vector<std::int64_t> revisions;
            while (heads.executeStep())
                revisions.push_back(heads.getColumn(0).getInt64());
            std::size_t grams = 0;
            for (std::size_t position = 0; position < revisions.size(); ++position) {
                const auto revision = revisions[position];
                search::MotifStream stream;
                stream.division = division;
                stream.settings_hash = settings_hash;
                const auto length = static_cast<std::size_t>(16 + pick(49));
                const bool plant = (written + position) % 100 == 0;
                const auto plant_at = plant ? static_cast<std::size_t>(pick(static_cast<std::uint32_t>(length))) : length;
                const auto transpose = pick(13) - 6;
                Tick onset = 0;
                int pitch = 48 + static_cast<int>(pick(24));
                std::uint32_t ordinal = 0;
                for (std::size_t index = 0; index < length; ++index) {
                    if (index == plant_at) {
                        const Tick base = onset;
                        for (const auto& note : planted) {
                            const auto planted_pitch = static_cast<std::uint8_t>(note.pitch + transpose);
                            stream.notes.push_back(search::MotifNote{base + note.onset, base + note.onset + note.duration,
                                                                     planted_pitch, note.velocity, ordinal++});
                            onset = base + note.onset + 480;
                        }
                    }
                    pitch = std::clamp(pitch + static_cast<int>(pick(5)) - 2, 36, 96);
                    const Tick step = 120 * (1 + pick(4));
                    stream.notes.push_back(search::MotifNote{onset, onset + step - 20, static_cast<std::uint8_t>(pitch),
                                                             static_cast<std::uint8_t>(70 + pick(50)), ordinal++});
                    onset += step;
                }
                grams += insert_motif_stream(database, revision, stream);
                Statement run(database, "INSERT OR REPLACE INTO analysis_run(revision_id, analyzer, version, "
                                        "settings_sha256, status, completed_ms, result_json) VALUES (?1, 'derived', "
                                        "?2, ?3, 'complete', ?4, ?5)");
                run.bind(1, revision);
                run.bind(2, derived_analysis_version);
                sqlite_support::bind_bytes(run, 3, run_settings);
                run.bind(4, now_utc_ms());
                run.bind(5, Json{{"version", 1}, {"streams", 1}, {"synthetic", true}}.dump());
                run.exec();
            }
            state.store_gram_rows(database, state.derived_gram_rows(database) + static_cast<std::int64_t>(grams));
            return revisions.size();
        });
        if (!inserted)
            return std::unexpected(std::move(inserted).error());
        if (*inserted == 0)
            break; // no unanalyzed heads left
        written += *inserted;
    }
    return written;
}

// --- Maintenance hooks -----------------------------------------------------------------------------

Result<std::vector<std::string>> Library::explain_query_plan(const search::SqlFragment& statement) {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    return query_plan(***reader, statement);
}

Status Library::backup_catalog(const std::filesystem::path& destination) {
    auto reader = state_->reader();
    if (!reader)
        return std::unexpected(std::move(reader).error());
    // The reader is query-only; the backup API only reads from it.
    return backup_database(***reader, destination);
}

Result<std::size_t> Library::populate_synthetic(std::size_t rows, std::uint64_t seed, std::stop_token stop) {
    auto& state = *state_;
    // One real, valid SMF object shared by every synthetic revision keeps verify/backup meaningful.
    auto document = make_blank_document("synthetic");
    auto& track = document.sequences.front().tracks.front();
    for (std::uint8_t index = 0; index < 8; ++index) {
        auto on = events::note_on(index * 480, 0, static_cast<std::uint8_t>(60 + index), 100);
        auto off = events::note_off(index * 480 + 240, 0, static_cast<std::uint8_t>(60 + index));
        on.order_key = (track.events.size() + 1) * order_key_spacing;
        track.events.push_back(on);
        off.order_key = (track.events.size() + 1) * order_key_spacing;
        track.events.push_back(off);
    }
    std::ranges::stable_sort(track.events, EventOrder{});
    auto bytes = midi::export_midi(document, midi::ExportOptions{});
    if (!bytes)
        return std::unexpected(std::move(bytes).error());
    const auto digest = sha256(*bytes);
    std::shared_lock objects_guard(state.objects_lock); // through the catalog commits below
    MWB_TRY(state.objects.publish_bytes(*bytes, digest, media_extension(MediaType::smf)));

    static constexpr std::array<std::string_view, 24> words{
        "bass",  "lead",  "pad",   "arp",   "rising", "falling",  "dark",  "bright", "groove", "chord", "melody", "hook",
        "drums", "synth", "piano", "string", "brass", "straight", "swing", "pluck",  "minor",  "major", "riff",   "loop"};
    static constexpr std::array<std::string_view, 5> modes{"major", "minor", "dorian", "mixolydian", "phrygian"};
    DeterministicRandom random(seed);
    const auto pick = [&](std::size_t count) { return static_cast<std::size_t>(random.bounded(static_cast<std::uint32_t>(count))); };
    std::size_t written = 0;
    while (written < rows) {
        if (stop.stop_requested())
            return make_error(ErrorCode::cancelled, "synthetic population cancelled");
        const auto batch = std::min<std::size_t>(1000, rows - written);
        auto inserted = state.write("bench.populate", [&](SQLite::Database& database) -> Result<std::size_t> {
            state.insert_object_row(database, digest, MediaType::smf, bytes->size());
            std::vector<std::int64_t> ids;
            for (std::size_t index = 0; index < batch; ++index) {
                const auto first = words[pick(words.size())];
                const auto second = words[pick(words.size())];
                const auto title = std::format("{} {} {:07}", first, second, written + index);
                const auto asset_id = state.insert_asset(database, title, AssetKind::phrase);
                ids.push_back(asset_id);
                if (pick(10) == 0) {
                    Statement favorite(database, "UPDATE asset SET favorite = 1, rating = ?1 WHERE id = ?2");
                    favorite.bind(1, static_cast<std::int64_t>(pick(6)));
                    favorite.bind(2, asset_id);
                    favorite.exec();
                }
                const auto revision = state.insert_head_revision(database, asset_id, std::nullopt, digest,
                                                                 RevisionOrigin::import, "");
                ParsedObject parsed;
                DocumentAnalysis analysis;
                const auto sequences = pick(8) == 0 ? 2u : 1u;
                for (std::uint32_t sequence_no = 0; sequence_no < sequences; ++sequence_no) {
                    SequenceSummary summary;
                    summary.sequence_no = sequence_no;
                    const auto bpm = 60.0 + static_cast<double>(pick(141));
                    summary.tempo_min = bpm;
                    summary.tempo_max = bpm;
                    summary.tempo_representative = bpm;
                    summary.tempos = {bpm};
                    summary.tempo_origin = pick(4) == 0 ? "default" : "explicit";
                    summary.key_root = static_cast<int>(pick(12));
                    summary.key_mode = std::string(modes[pick(modes.size())]);
                    summary.key_origin = "signature";
                    summary.quarter_length = static_cast<double>(4 * (1 + pick(64)));
                    summary.duration_seconds = *summary.quarter_length * 60.0 / bpm;
                    summary.track_count = 1;
                    summary.event_count = 16;
                    summary.note_count = static_cast<std::int64_t>(1 + pick(512));
                    summary.min_pitch = static_cast<int>(36 + pick(24));
                    summary.max_pitch = *summary.min_pitch + static_cast<int>(pick(36));
                    summary.max_polyphony = static_cast<std::int64_t>(1 + pick(6));
                    summary.has_drums = pick(5) == 0;
                    summary.meters = {pick(6) == 0 ? "6/8" : "4/4"};
                    if (pick(4) == 0)
                        summary.event_classes.push_back("bend");
                    TrackSummary track_summary;
                    track_summary.channel_mask = static_cast<std::uint16_t>(1u << pick(16));
                    track_summary.programs = {static_cast<std::uint8_t>(pick(128))};
                    track_summary.note_count = summary.note_count;
                    track_summary.feature_json = "{\"version\":1}";
                    summary.tracks.push_back(std::move(track_summary));
                    summary.diagnostics_json = summary_diagnostics_json(summary);
                    analysis.sequences.push_back(std::move(summary));
                }
                analysis.searchable_text = std::format("{} {}", second, first);
                parsed.analysis = std::move(analysis);
                state.write_basic_analysis(database, asset_id, revision, parsed);
                for (int tag_index = 0; tag_index < 2; ++tag_index) {
                    const auto tag = std::string(words[pick(words.size())]);
                    Statement insert_tag(database, "INSERT OR IGNORE INTO tag(name, normalized_name) VALUES (?1, ?1)");
                    insert_tag.bind(1, tag);
                    insert_tag.exec();
                    Statement link(database, "INSERT OR IGNORE INTO asset_tag(asset_id, tag_id) SELECT ?1, id FROM tag "
                                             "WHERE normalized_name = ?2");
                    link.bind(1, asset_id);
                    link.bind(2, tag);
                    link.exec();
                }
                state.refresh_fts(database, asset_id);
            }
            state.record_change(database, "bench.populate", ids, true);
            return ids.size();
        });
        if (!inserted)
            return std::unexpected(std::move(inserted).error());
        written += *inserted;
    }
    return written;
}

Result<std::size_t> Library::rebuild_fts() {
    auto& state = *state_;
    return state.write("repair.rebuild_fts", [&](SQLite::Database& database) -> Result<std::size_t> {
        database.exec("DELETE FROM asset_fts");
        std::vector<std::int64_t> ids;
        Statement assets(database, "SELECT id FROM asset ORDER BY id");
        while (assets.executeStep())
            ids.push_back(assets.getColumn(0).getInt64());
        for (const auto id : ids)
            state.refresh_fts(database, id);
        state.record_change(database, "repair.rebuild_fts", ids, false);
        return ids.size();
    });
}

} // namespace mwb::library
