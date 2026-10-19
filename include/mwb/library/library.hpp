#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/hashing.hpp"
#include "mwb/core/ids.hpp"
#include "mwb/core/model.hpp"
#include "mwb/midi/codec.hpp"
#include "mwb/search/browse_index.hpp"
#include "mwb/search/motif.hpp"
#include "mwb/search/query.hpp"
#include "mwb/search/sql.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

/// Managed MIDI library: directory layout (section 22.1), catalog (23.2), import transaction (22.2),
/// revisions (22.3), organization and trash (24), search (25, 26), change outbox (19.4).
///
/// Threading: a Library object owns the single writer connection; every mutating call serializes
/// on it (the application runs them on its library writer thread, in user order). Read calls
/// (search, browse rows, listings) use a bounded pool of read-only connections and may run on
/// query workers concurrently. No call holds a read transaction after it returns.
namespace mwb::library {

/// Asset kinds of the catalog.
enum class AssetKind : std::uint8_t { song, phrase, template_asset };
/// Revision origins of the catalog.
enum class RevisionOrigin : std::uint8_t { import, edit, extract, restore };
/// Object media types.
enum class MediaType : std::uint8_t { smf, rmid, native };

[[nodiscard]] std::string_view asset_kind_name(AssetKind kind) noexcept;
[[nodiscard]] std::string_view revision_origin_name(RevisionOrigin origin) noexcept;
[[nodiscard]] std::string_view media_type_name(MediaType type) noexcept;
/// Object file extension of a media type ("mid", "rmi", "mwb").
[[nodiscard]] std::string_view media_extension(MediaType type) noexcept;

/// Connection settings (section 22.5).
struct LibraryOptions {
    int busy_timeout_ms = 5000;
    std::size_t read_connections = 4;
};

/// What to do when equal bytes arrive from another path (section 24.2).
enum class DuplicatePolicy : std::uint8_t { attach_source, separate_asset };

/// Import options.
struct ImportOptions {
    bool recursive = true;
    DuplicatePolicy duplicates = DuplicatePolicy::attach_source;
    std::size_t max_capture_attempts = 3;
    midi::ParseLimits limits;
    /// Parser for imported SMF/RMID bytes; empty parses in this process. The application passes
    /// the isolated parser worker (section 28.2).
    midi::MidiParser parser;
    /// Fault-injection point between capture and the identity recheck (tests, diagnostics).
    std::function<void(const std::filesystem::path& source, std::size_t attempt)> after_capture;
};

/// Per-item outcome.
enum class ImportOutcome : std::uint8_t {
    imported,            ///< New asset or new revision.
    duplicate,           ///< Equal bytes already present (same source or another source attached).
    unsupported,         ///< Stored as an object with an Unsupported/invalid state.
    changed_during_read, ///< Source kept changing; not indexed.
    failed,
    cancelled,
    source_changed,      ///< Source has new bytes but the asset has user-authored edits: needs review.
};
[[nodiscard]] std::string_view import_outcome_name(ImportOutcome outcome) noexcept;

/// One imported item.
struct ImportItem {
    std::string display_path; ///< Relative to its source root.
    ImportOutcome outcome = ImportOutcome::failed;
    std::optional<std::int64_t> asset_id;
    std::string reason;
};

/// Section 24.2 counts.
struct ImportCounts {
    std::size_t discovered = 0;
    std::size_t captured = 0;
    std::size_t imported = 0;
    std::size_t duplicate = 0;
    std::size_t unsupported = 0;
    std::size_t changed_during_read = 0;
    std::size_t failed = 0;
    std::size_t cancelled = 0;
    std::size_t source_changed = 0;
};

/// Import report.
struct ImportReport {
    ImportCounts counts;
    std::vector<ImportItem> items;
    /// Versioned JSON for `library import --json`.
    [[nodiscard]] std::string to_json() const;
};

/// One asset's catalog metadata.
struct AssetInfo {
    std::int64_t id = 0;
    Uuid uuid;
    std::string title;
    std::string sort_title;
    std::string description;
    AssetKind kind = AssetKind::song;
    std::string role;
    int rating = 0;
    bool favorite = false;
    std::int64_t created_ms = 0;
    std::int64_t modified_ms = 0;
    std::optional<std::int64_t> deleted_ms;
    std::optional<std::int64_t> current_revision_id;
    std::int64_t metadata_generation = 0;
};

/// One revision.
struct RevisionInfo {
    std::int64_t id = 0;
    Uuid uuid;
    std::int64_t asset_id = 0;
    std::int64_t ordinal = 0;
    std::optional<std::int64_t> parent_id;
    Sha256Digest object_sha256{};
    MediaType media_type = MediaType::smf;
    std::uint64_t byte_count = 0;
    std::int64_t created_ms = 0;
    RevisionOrigin origin = RevisionOrigin::import;
    std::string label;
    bool pinned = false;
};

/// One tag with its usage count.
struct TagInfo {
    std::int64_t id = 0;
    std::string name;
    std::string normalized_name;
    std::int64_t asset_count = 0;
};

/// One collection.
struct CollectionInfo {
    std::int64_t id = 0;
    Uuid uuid;
    std::optional<std::int64_t> parent_id;
    std::string name;
    bool smart = false;
};

/// Change outbox answer (section 19.4).
struct ChangeSet {
    std::int64_t latest_revision = 0;      ///< Pass this to the next changes_since call.
    std::vector<std::int64_t> asset_ids;   ///< Changed assets (sorted, unique).
    bool structure_changed = false;        ///< Tags, collections or folders changed.
    bool full_reload = false;              ///< Too much changed; reload everything.
};

/// One page row of a search.
struct SearchRow {
    std::int64_t id = 0;
    std::string sort_title;
    std::int64_t modified_ms = 0;
    std::int64_t created_ms = 0;
    std::optional<double> rank;
};

/// Search request (sections 25.3-25.4).
struct SearchRequest {
    std::string query_text;
    search::SortOrder order;
    std::optional<search::PageCursor> after;
    std::size_t page_size = search::default_page_size;
    std::optional<std::int64_t> collection_scope;
    bool across_sequences = false; ///< Explicit "across any sequences" mode.
};

/// Search page.
struct SearchPage {
    std::vector<SearchRow> rows;
    std::optional<search::PageCursor> next;
    bool truncated = false; ///< Relevance results were bounded to relevance_materialize_limit.
};

/// Revision commit conflict detail: the head moved since the editor loaded it.
struct CommitRequest {
    std::int64_t asset_id = 0;
    std::optional<std::int64_t> expected_head;
    RevisionOrigin origin = RevisionOrigin::edit;
    std::string label;
};

/// Trash purge result.
struct PurgeReport {
    std::size_t assets = 0;
    std::size_t revisions = 0;
};

/// Object garbage collection options and report.
struct GarbageCollectionOptions {
    std::int64_t grace_period_ms = 30LL * 24 * 60 * 60 * 1000;
    std::optional<std::int64_t> now_ms; ///< Override for tests; defaults to the wall clock.
};
struct GarbageCollectionReport {
    std::size_t unreferenced = 0;  ///< Objects without catalog references.
    std::size_t deleted = 0;       ///< Removed after their grace period.
    std::size_t waiting = 0;       ///< Still inside the grace period.
    std::size_t staging_removed = 0;
    bool suspended = false;        ///< A backup is active; nothing was removed.
};

/// Explicit analysis (CLI `library analyze`).
struct AnalyzeOptions {
    bool fingerprints = true;
    bool motif = true;
    bool features = true;
    std::size_t batch_size = 64;
};
struct AnalyzeReport {
    std::size_t revisions_analyzed = 0;
    std::size_t revisions_skipped = 0;  ///< Already complete for the current versions.
    std::size_t revisions_failed = 0;
    std::size_t streams = 0;
    std::size_t grams = 0;
    bool cancelled = false;
};

/// A leased job (job table).
struct Job {
    std::int64_t id = 0;
    std::string kind;
    std::string idempotency_key;
    std::string arguments_json;
    std::int64_t attempts = 0;
};

/// Job leasing statement (hot query of section 22.5): oldest queued job becomes running.
inline constexpr std::string_view lease_job_sql =
    "UPDATE job SET state = 'running', attempts = attempts + 1, updated_ms = ?1 WHERE id = (SELECT id FROM job "
    "INDEXED BY job_queue WHERE state = 'queued' ORDER BY id LIMIT 1) RETURNING id, kind, idempotency_key, "
    "arguments_json, attempts";

/// The managed library.
class Library {
public:
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    /// Creates a new library in an empty or missing directory.
    [[nodiscard]] static Result<std::unique_ptr<Library>> create(const std::filesystem::path& root,
                                                                 const LibraryOptions& options = {});
    /// Opens an existing library for writing; fails with ErrorCode::busy when another writer holds it.
    [[nodiscard]] static Result<std::unique_ptr<Library>> open(const std::filesystem::path& root,
                                                               const LibraryOptions& options = {});

    [[nodiscard]] const std::filesystem::path& root() const;
    [[nodiscard]] const Uuid& uuid() const;

    // --- Import ------------------------------------------------------------------------------
    /// Imports files and folders (section 22.2). Folder import continues past item failures;
    /// cancellation stops between items and rolls back only the item in flight.
    [[nodiscard]] Result<ImportReport> import_paths(const std::vector<std::filesystem::path>& sources,
                                                    const ImportOptions& options = {}, std::stop_token stop = {});

    // --- Assets and metadata -------------------------------------------------------------------
    [[nodiscard]] Result<std::optional<AssetInfo>> asset(std::int64_t asset_id);
    /// Creates a new asset from a document (saved as a native object).
    [[nodiscard]] Result<std::int64_t> create_asset(const DocumentData& document, std::string title,
                                                    AssetKind kind = AssetKind::phrase,
                                                    RevisionOrigin origin = RevisionOrigin::edit);
    /// Explicit Duplicate: a new asset identity sharing the current head object.
    [[nodiscard]] Result<std::int64_t> duplicate_asset(std::int64_t asset_id);
    [[nodiscard]] Status set_title(std::int64_t asset_id, std::string title);
    [[nodiscard]] Status set_description(std::int64_t asset_id, std::string description);
    [[nodiscard]] Status set_role(std::int64_t asset_id, std::string role);
    [[nodiscard]] Status set_favorite(std::int64_t asset_id, bool favorite);
    [[nodiscard]] Status set_rating(std::int64_t asset_id, int rating);
    /// User key override for one sequence (stored in asset_override; effective summary origin
    /// becomes "manual" while the analysis evidence stays in analysis_run).
    [[nodiscard]] Status set_key_override(std::int64_t asset_id, std::uint32_t sequence_no, int root,
                                          std::string mode);

    // --- Tags ------------------------------------------------------------------------------------
    [[nodiscard]] Status add_tag(std::int64_t asset_id, const std::string& name);
    [[nodiscard]] Status remove_tag(std::int64_t asset_id, const std::string& name);
    [[nodiscard]] Result<std::vector<TagInfo>> list_tags();
    [[nodiscard]] Result<std::vector<std::string>> asset_tags(std::int64_t asset_id);

    // --- Collections -------------------------------------------------------------------------
    [[nodiscard]] Result<std::int64_t> create_collection(const std::string& name,
                                                         std::optional<std::int64_t> parent = std::nullopt);
    /// Smart collections store the versioned query AST JSON, never SQL.
    [[nodiscard]] Result<std::int64_t> create_smart_collection(const std::string& name, const std::string& query_text,
                                                               std::optional<std::int64_t> parent = std::nullopt);
    [[nodiscard]] Status add_to_collection(std::int64_t collection_id, const std::vector<std::int64_t>& asset_ids);
    [[nodiscard]] Status remove_from_collection(std::int64_t collection_id, const std::vector<std::int64_t>& asset_ids);
    /// Moves an asset to `index` within the manual order (positions are renumbered compactly).
    [[nodiscard]] Status move_in_collection(std::int64_t collection_id, std::int64_t asset_id, std::size_t index);
    /// Members in manual order, keyset-paged by position.
    [[nodiscard]] Result<std::vector<std::int64_t>> collection_members(std::int64_t collection_id,
                                                                       std::optional<std::int64_t> after_position = {},
                                                                       std::size_t limit = 500);
    [[nodiscard]] Result<std::vector<CollectionInfo>> list_collections();
    [[nodiscard]] Result<search::Query> smart_collection_query(std::int64_t collection_id);

    // --- Revisions ---------------------------------------------------------------------------
    [[nodiscard]] Result<std::vector<RevisionInfo>> revisions(std::int64_t asset_id);
    /// Publishes the object first, then commits a new head if `expected_head` is still current;
    /// otherwise ErrorCode::conflict (the object stays for a conflict workflow).
    [[nodiscard]] Result<std::int64_t> commit_revision(const CommitRequest& request, std::span<const std::uint8_t> bytes,
                                                       MediaType media_type);
    /// Saves a document as a native revision (section 22.3).
    [[nodiscard]] Result<std::int64_t> save_document_revision(const CommitRequest& request, const DocumentData& document);
    /// Restore: a new head referencing the old revision's object; history is kept.
    [[nodiscard]] Result<std::int64_t> restore_revision(std::int64_t asset_id, std::int64_t revision_id,
                                                        std::optional<std::int64_t> expected_head);
    [[nodiscard]] Status set_revision_pinned(std::int64_t revision_id, bool pinned);
    /// Exact object bytes of a revision (verified against the hash).
    [[nodiscard]] Result<std::vector<std::uint8_t>> revision_bytes(std::int64_t revision_id);
    /// Opens a revision as a document (SMF through the midi adapter, native through the document module).
    /// Opens a revision for editing. MIDI bytes go through `parser` when given (the isolated
    /// worker), with default parse limits; native revisions are validated by the native loader.
    [[nodiscard]] Result<DocumentData> open_revision(std::int64_t revision_id, const midi::MidiParser& parser = {},
                                                     std::stop_token stop = {});

    // --- Trash and garbage collection ----------------------------------------------------------
    [[nodiscard]] Status trash(const std::vector<std::int64_t>& asset_ids);
    [[nodiscard]] Status restore_from_trash(const std::vector<std::int64_t>& asset_ids);
    /// Counts what a purge would remove (shown before confirming).
    [[nodiscard]] Result<PurgeReport> purge_preview(const std::vector<std::int64_t>& asset_ids);
    /// Permanently removes trashed assets (active assets are refused).
    [[nodiscard]] Result<PurgeReport> purge(const std::vector<std::int64_t>& asset_ids);
    /// Trashed assets ordered by (deleted_ms, sort_title, id), keyset paged.
    [[nodiscard]] Result<std::vector<std::int64_t>> list_trash(std::size_t limit = 500);
    [[nodiscard]] Result<GarbageCollectionReport> collect_garbage(const GarbageCollectionOptions& options = {});

    // --- Change outbox ---------------------------------------------------------------------------
    [[nodiscard]] Result<ChangeSet> changes_since(std::int64_t revision, std::size_t max_changes = 1000);
    /// Called after every committed mutation with the newest outbox revision (on the writer's thread).
    void set_change_listener(std::function<void(std::int64_t)> listener);

    // --- Search ----------------------------------------------------------------------------------
    [[nodiscard]] Result<SearchPage> search(const SearchRequest& request, std::stop_token stop = {});
    /// Compiles a request to the exact SQL the search runs (diagnostics, EXPLAIN tests).
    [[nodiscard]] Result<search::CompiledSearch> compile_search(const SearchRequest& request);
    /// Browse-index rows (section 19.4), keyset paged by asset ID.
    [[nodiscard]] Result<std::vector<search::BrowseRow>> browse_rows(std::int64_t after_id, std::size_t limit);
    /// Browse-index rows of specific assets (change outbox application).
    [[nodiscard]] Result<std::vector<search::BrowseRow>> browse_rows_for(const std::vector<std::int64_t>& asset_ids);
    /// Search options with collection names resolved (shared by SQL and the browse index).
    [[nodiscard]] Result<search::SearchOptions> resolve_search_options(const search::Query& query,
                                                                       std::optional<std::int64_t> scope,
                                                                       bool across_sequences);

    // --- Analysis and musical search ----------------------------------------------------------
    [[nodiscard]] Result<AnalyzeReport> analyze(const AnalyzeOptions& options = {}, std::stop_token stop = {});
    [[nodiscard]] Result<search::ExactSearchResult> motif_search(const search::ExactQuery& query,
                                                                 std::stop_token stop = {});
    [[nodiscard]] Result<search::ApproximateSearchResult> similar_search(const search::ApproximateQuery& query,
                                                                         std::stop_token stop = {});

    // --- Jobs ------------------------------------------------------------------------------------
    [[nodiscard]] Result<std::int64_t> enqueue_job(const std::string& kind, const std::string& idempotency_key,
                                                   const std::string& arguments_json);
    /// Leases the oldest queued job (state -> running, attempts + 1).
    [[nodiscard]] Result<std::optional<Job>> lease_job();
    [[nodiscard]] Status finish_job(std::int64_t job_id, bool success, const std::string& error_json = {});

    // --- Maintenance hooks used by backup/repair -------------------------------------------------
    /// EXPLAIN QUERY PLAN detail lines of a statement on a read connection.
    [[nodiscard]] Result<std::vector<std::string>> explain_query_plan(const search::SqlFragment& statement);
    /// Online backup of the catalog into `destination` (SQLite backup API).
    [[nodiscard]] Status backup_catalog(const std::filesystem::path& destination);
    /// Rebuilds asset_fts from catalog metadata and stored analysis text in one transaction.
    [[nodiscard]] Result<std::size_t> rebuild_fts();
    /// Benchmark and plan-test fixture: appends `rows` deterministic synthetic assets (one shared
    /// real SMF object, summaries, tags and FTS rows) in batched transactions.
    [[nodiscard]] Result<std::size_t> populate_synthetic(std::size_t rows, std::uint64_t seed = 1,
                                                         std::stop_token stop = {});
    /// Suspends object garbage collection while alive (backups).
    class GarbageCollectionPause {
    public:
        explicit GarbageCollectionPause(Library& library);
        ~GarbageCollectionPause();
        GarbageCollectionPause(const GarbageCollectionPause&) = delete;
        GarbageCollectionPause& operator=(const GarbageCollectionPause&) = delete;

    private:
        Library& library_;
    };

    /// Implementation state (opaque).
    struct State;

private:
    explicit Library(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace mwb::library
