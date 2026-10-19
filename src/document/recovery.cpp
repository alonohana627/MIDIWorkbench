#include "mwb/document/recovery.hpp"

#include "mwb/core/serialization.hpp"
#include "mwb/document/migration.hpp"
#include "mwb/document/native_format.hpp"
#include "mwb/document/schemas.hpp"
#include "mwb/document/sqlite_support.hpp"

#include <SQLiteCpp/Transaction.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

namespace mwb::document {

namespace {

using namespace sqlite_support;
using nlohmann::json;

/// Largest capture chunk payload (recovery_capture_chunk.payload CHECK, take_chunk limit).
constexpr std::size_t max_capture_chunk_bytes = 1024 * 1024;
/// Replay compares the base semantic hash of every n-th command (plus the first and the last).
constexpr std::size_t replay_hash_stride = 16;

std::int64_t default_clock() { return now_utc_ms(); }

/// Source reference JSON (version 1). `saved_generation` and `base_saved` are journal markers
/// the listing reads; older readers ignore unknown members.
std::string encode_source(const SourceReference& source, std::optional<Generation> saved_generation, bool base_saved) {
    json value{
        {"version", 1},
        {"document_name", source.document_name},
        {"file_path", source.file_path_utf8 ? json(*source.file_path_utf8) : json(nullptr)},
        {"source_identity", source.source_identity ? json(*source.source_identity) : json(nullptr)},
        {"last_explicit_save_ms", source.last_explicit_save_ms ? json(*source.last_explicit_save_ms) : json(nullptr)},
        {"base_saved", base_saved},
    };
    if (saved_generation)
        value["saved_generation"] = static_cast<std::int64_t>(*saved_generation);
    // error_handler_t::replace keeps the JSON valid even for a title with invalid UTF-8.
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

/// Decoded source reference plus the journal markers.
struct DecodedSource {
    SourceReference source;
    std::optional<Generation> saved_generation;
    bool base_saved = false;
};

Result<DecodedSource> decode_source(const std::string& text) {
    const json value = json::parse(text, nullptr, false);
    if (value.is_discarded() || !value.is_object())
        return make_error(ErrorCode::invalid_data, "recovery source reference is not a JSON object");
    const auto version = value.find("version");
    if (version == value.end() || !version->is_number_integer() || version->get<std::int64_t>() != 1)
        return make_error(ErrorCode::unsupported, "recovery source reference has an unsupported version");
    DecodedSource decoded;
    SourceReference& source = decoded.source;
    const auto text_member = [&](const char* key) -> std::optional<std::string> {
        const auto found = value.find(key);
        if (found == value.end() || !found->is_string())
            return std::nullopt;
        return found->get<std::string>();
    };
    source.document_name = text_member("document_name").value_or(std::string());
    source.file_path_utf8 = text_member("file_path");
    source.source_identity = text_member("source_identity");
    if (const auto found = value.find("last_explicit_save_ms"); found != value.end() && found->is_number_integer())
        source.last_explicit_save_ms = found->get<std::int64_t>();
    if (const auto found = value.find("saved_generation");
        found != value.end() && found->is_number_integer() && found->get<std::int64_t>() >= 0)
        decoded.saved_generation = static_cast<Generation>(found->get<std::int64_t>());
    if (const auto found = value.find("base_saved"); found != value.end() && found->is_boolean())
        decoded.base_saved = found->get<bool>();
    return decoded;
}

// --- Opening the recovery database ---------------------------------------------------------------

/// Message for the UI after a damaged database was set aside (take_recovery_database_notice()).
std::mutex notice_mutex;
std::optional<std::string> database_notice;

/// Opens or creates the recovery database at `path`, checks its identity, migrates version 1
/// in place and verifies the schema. The file is not modified before its identity is known, so a
/// foreign file is never touched. Throws SQLite::Exception; callers run it inside guarded().
Result<std::unique_ptr<SQLite::Database>> open_database(const std::filesystem::path& path,
                                                        std::chrono::milliseconds busy_timeout) {
    auto database = std::make_unique<SQLite::Database>(path_to_utf8(path), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE,
                                                       static_cast<int>(busy_timeout.count()));
    // Connection settings only; nothing is written yet.
    database->exec("PRAGMA foreign_keys = ON");
    database->exec("PRAGMA trusted_schema = OFF");
    const auto object_count = [&] {
        SQLite::Statement count(*database, "SELECT count(*) FROM sqlite_schema");
        count.executeStep();
        return count.getColumn(0).getInt64();
    };
    const std::int64_t objects = object_count(); // fails with SQLITE_NOTADB for a non-database
    const std::int64_t application = pragma_integer(*database, "application_id");
    const bool fresh = objects == 0 && application == 0;
    if (!fresh && application != recovery_application_id)
        return make_error(ErrorCode::invalid_data, "recovery database has the wrong application id");

    if (fresh) // must precede the first table; a no-op when another process created them first
        database->exec("PRAGMA auto_vacuum = INCREMENTAL");
    database->exec("PRAGMA journal_mode = WAL");
    database->exec("PRAGMA synchronous = FULL");
    // The WAL is truncated back to 8 MiB after checkpoints instead of keeping its largest size
    // (a full-document checkpoint is tens of megabytes).
    database->exec("PRAGMA journal_size_limit = 8388608");
    {
        SQLite::Transaction transaction(*database, SQLite::TransactionBehavior::IMMEDIATE);
        if (object_count() == 0 && pragma_integer(*database, "application_id") == 0)
            database->exec(std::string(schemas::recovery_v2));
        transaction.commit();
    }
    if (pragma_integer(*database, "application_id") != recovery_application_id)
        return make_error(ErrorCode::invalid_data, "recovery database has the wrong application id");
    auto access =
        migration::classify_version(migration::SchemaFamily::recovery, pragma_integer(*database, "user_version"));
    if (!access)
        return std::unexpected(std::move(access).error());
    if (*access == migration::SchemaAccess::needs_migration) {
        // The recovery database is a private per-user cache: migrated in place, one transaction.
        MWB_TRY(migration::verify_checksums(migration::SchemaFamily::recovery));
        SQLite::Transaction transaction(*database, SQLite::TransactionBehavior::IMMEDIATE);
        // Re-read inside the write lock: another instance may have migrated meanwhile.
        for (const auto& step : migration::migration_steps(migration::SchemaFamily::recovery))
            if (pragma_integer(*database, "user_version") == step.from_version)
                database->exec(std::string(step.sql));
        transaction.commit();
        if (pragma_integer(*database, "user_version") != recovery_schema_version)
            return make_error(ErrorCode::internal, "recovery database migration did not reach the current version");
    }
    MWB_TRY(verify_schema_whitelist(*database, schemas::recovery_v2, "recovery database"));
    return database;
}

/// open_database() with exceptions converted like guarded(), except that SQLite corruption and
/// "not a database" become invalid_data so the caller can set the file aside.
Result<std::unique_ptr<SQLite::Database>> open_classified(const std::filesystem::path& path,
                                                          std::chrono::milliseconds busy_timeout) {
    try {
        return open_database(path, busy_timeout);
    } catch (const SQLite::Exception& exception) {
        const int code = exception.getErrorCode();
        if (code == SQLITE_NOTADB || code == SQLITE_CORRUPT)
            return make_error(ErrorCode::invalid_data, std::string("recovery database is damaged: ") + exception.what());
        return make_error(ErrorCode::database, std::string("opening the recovery database: ") + exception.what());
    } catch (const std::bad_alloc&) {
        return make_error(ErrorCode::limit_exceeded, "opening the recovery database: out of memory");
    } catch (const std::exception& exception) {
        return make_error(ErrorCode::internal, std::string("opening the recovery database: ") + exception.what());
    }
}

/// True when some session lock in `directory` is held (a live journal of any instance): the
/// database is then in use and must never be set aside.
bool any_session_live(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::directory_iterator entries(directory, error);
    if (error)
        return true; // cannot tell: be conservative
    for (const auto& entry : entries)
        if (entry.path().extension() == ".lock" && file_system::ExclusiveFileLock::is_held(entry.path()))
            return true;
    return false;
}

/// Renames a damaged database (and its WAL and shared-memory files) to
/// recovery.corrupt-<utc ms>.sqlite, keeping it as evidence.
Status quarantine(const std::filesystem::path& path) {
    const std::string stamp = std::to_string(now_utc_ms());
    const std::filesystem::path target = path.parent_path() / ("recovery.corrupt-" + stamp + ".sqlite");
    std::error_code error;
    std::filesystem::rename(path, target, error);
    if (error)
        return make_error(ErrorCode::io, "cannot set the damaged recovery database aside: " + error.message());
    for (const char* sidecar : {"-wal", "-shm"}) {
        std::filesystem::path from = path;
        from += sidecar;
        std::filesystem::path to = target;
        to += sidecar;
        std::error_code ignored;
        if (std::filesystem::exists(from, ignored))
            std::filesystem::rename(from, to, ignored); // best effort: the main file is what matters
    }
    std::lock_guard lock(notice_mutex);
    database_notice = "The crash recovery database was damaged and has been set aside as " +
                      path_to_utf8(target.filename()) + "; recovery continues with a new database.";
    return ok();
}

/// Opens `path`; a damaged or foreign file is set aside and replaced by a fresh database when no
/// session is live in the directory.
Result<std::unique_ptr<SQLite::Database>> open_or_quarantine(const std::filesystem::path& directory,
                                                             const std::filesystem::path& path,
                                                             std::chrono::milliseconds busy_timeout) {
    auto database = open_classified(path, busy_timeout); // the connection is closed again on failure
    if (database || database.error().code != ErrorCode::invalid_data || any_session_live(directory))
        return database;
    MWB_TRY(quarantine(path));
    return open_classified(path, busy_timeout);
}

Result<std::unique_ptr<SQLite::Database>> open_recovery_database(const std::filesystem::path& directory,
                                                                 std::chrono::milliseconds busy_timeout =
                                                                     RecoveryPolicy{}.busy_timeout) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return make_error(ErrorCode::io, "cannot create the recovery directory: " + error.message());
    auto primary = open_or_quarantine(directory, directory / recovery_database_name, busy_timeout);
    if (primary || primary.error().code != ErrorCode::unsupported)
        return primary;
    // Written by a newer version (after a downgrade): keep this version's sessions apart instead
    // of running without crash recovery.
    return open_or_quarantine(directory, directory / recovery_fallback_database_name, busy_timeout);
}

/// True when either recovery database file exists.
bool recovery_database_exists(const std::filesystem::path& directory) {
    std::error_code error;
    return std::filesystem::exists(directory / recovery_database_name, error) ||
           std::filesystem::exists(directory / recovery_fallback_database_name, error);
}

constexpr auto generation_limit = static_cast<Generation>(std::numeric_limits<std::int64_t>::max());

// --- Background executors -----------------------------------------------------------------------

/// One background thread running queued work in order. Queued work runs before the thread exits
/// unless shutdown() dropped it (its futures then report a broken promise); a journal never
/// waits on it in a destructor.
class BackgroundExecutor {
public:
    BackgroundExecutor()
        : thread_([this](std::stop_token stop) {
              while (true) {
                  std::function<void()> task;
                  {
                      std::unique_lock lock(mutex_);
                      wake_.wait(lock, stop, [this] { return !tasks_.empty(); });
                      if (tasks_.empty())
                          return; // stop requested and nothing left
                      task = std::move(tasks_.front());
                      tasks_.pop_front();
                  }
                  task(); // packaged tasks store exceptions in their futures
              }
          }) {}
    ~BackgroundExecutor() { shutdown(false); }
    BackgroundExecutor(const BackgroundExecutor&) = delete;
    BackgroundExecutor& operator=(const BackgroundExecutor&) = delete;

    template <class Result>
    std::shared_future<Result> submit(std::function<Result()> work) {
        auto task = std::make_shared<std::packaged_task<Result()>>(std::move(work));
        auto future = task->get_future().share();
        {
            std::lock_guard lock(mutex_);
            if (stopped_)
                return future; // `task` dies unrun: the future reports a broken promise
            tasks_.push_back([task] { (*task)(); });
        }
        wake_.notify_all();
        return future;
    }
    /// Work waiting to start (not counting the task that is running).
    [[nodiscard]] std::size_t queued() {
        std::lock_guard lock(mutex_);
        return tasks_.size();
    }
    /// Stops the thread; `drop` discards queued work first. Idempotent.
    void shutdown(bool drop) {
        std::deque<std::function<void()>> dropped;
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
            if (drop)
                dropped.swap(tasks_);
        }
        dropped.clear(); // outside the lock: destroying a packaged task completes its future
        thread_.request_stop();
        wake_.notify_all();
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id())
            thread_.join();
    }

private:
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<std::function<void()>> tasks_;
    bool stopped_ = false;
    std::jthread thread_;
};

BackgroundExecutor& hash_executor() {
    static BackgroundExecutor executor;
    return executor;
}

BackgroundExecutor& checkpoint_executor() {
    static BackgroundExecutor executor;
    return executor;
}

std::shared_future<Sha256Digest> ready_digest(const Sha256Digest& digest) {
    std::promise<Sha256Digest> promise;
    promise.set_value(digest);
    return promise.get_future().share();
}

/// Load limits for checkpoints: they are this application's own images, so the editable event
/// limit (which makes larger files inspection-only) does not apply; the stored-event cap does.
NativeLoadLimits checkpoint_limits() {
    NativeLoadLimits limits;
    limits.document.max_events_per_document = static_cast<std::size_t>(limits.max_stored_events);
    return limits;
}

} // namespace

// --- RecoveryJournal ----------------------------------------------------------------------------

RecoveryJournal::RecoveryJournal(std::unique_ptr<SQLite::Database> database, Uuid session_id, SourceReference source,
                                 RecoveryPolicy policy, RecoveryClock clock)
    : database_(std::move(database)),
      session_id_(session_id),
      source_(std::move(source)),
      policy_(policy),
      clock_(std::move(clock)) {}

RecoveryJournal::~RecoveryJournal() {
    cancelled_->store(true); // a checkpoint still queued for this journal is skipped
    bool unused = false;
    {
        std::lock_guard state_lock(state_mutex_);
        unused = !closed_ && discard_if_unused_ && last_command_no_ == 0 && pending_chunks_.empty();
    }
    if (unused)
        std::ignore = mark_cleanly_closed(); // nothing beyond the opened file to recover
}

void RecoveryJournal::set_discard_if_unused(bool discard) {
    std::lock_guard state_lock(state_mutex_);
    discard_if_unused_ = discard;
    source_changed_ = true; // stored as base_saved
}

std::string RecoveryJournal::encoded_source() const {
    return encode_source(source_, saved_generation_, discard_if_unused_);
}

RecoveryJournal::CheckpointJob RecoveryJournal::submit_checkpoint(std::shared_ptr<const DocumentData> snapshot,
                                                                  std::int64_t command_no, Generation generation,
                                                                  std::int64_t now) const {
    using Prepared = Result<std::shared_ptr<const PendingCheckpoint>>;
    return checkpoint_executor().submit<Prepared>(
        [snapshot = std::move(snapshot), command_no, generation, now, cancelled = cancelled_]() -> Prepared {
            if (cancelled->load())
                return make_error(ErrorCode::cancelled, "the recovery journal was closed");
            auto bytes = native_to_bytes(*snapshot);
            if (!bytes)
                return std::unexpected(std::move(bytes).error().with_context("preparing a recovery checkpoint"));
            auto checkpoint = std::make_shared<PendingCheckpoint>();
            checkpoint->command_no = command_no;
            checkpoint->generation = generation;
            checkpoint->checksum = sha256(*bytes);
            checkpoint->snapshot = std::move(*bytes);
            checkpoint->created_ms = now;
            return std::shared_ptr<const PendingCheckpoint>(std::move(checkpoint));
        });
}

Result<std::unique_ptr<RecoveryJournal>> RecoveryJournal::create(const std::filesystem::path& directory,
                                                                 const DocumentData& document, SourceReference source,
                                                                 RecoveryPolicy policy, RecoveryClock clock) {
    if (!clock)
        clock = default_clock;
    if (document.generation > generation_limit)
        return make_error(ErrorCode::invalid_argument, "document generation does not fit the recovery journal");
    // One immutable copy (milliseconds even for large documents) feeds the background hash and
    // checkpoint; the caller's thread never serializes the document (section 19.4).
    auto snapshot = guarded("copying the document for recovery",
                            [&]() -> Result<std::shared_ptr<const DocumentData>> {
                                return std::make_shared<const DocumentData>(document);
                            });
    if (!snapshot)
        return std::unexpected(std::move(snapshot).error());
    auto database = open_recovery_database(directory, policy.busy_timeout);
    if (!database)
        return std::unexpected(std::move(database).error());

    const Uuid session_id = Uuid::generate();
    // Held for the journal's lifetime: other instances see the session as live, not crashed.
    // Taken before the session row exists, so a row without a held lock is always dead.
    auto lock = file_system::ExclusiveFileLock::acquire(directory / (session_id.to_string() + ".lock"));
    if (!lock)
        return std::unexpected(std::move(lock).error().with_context("locking the recovery session"));
    const std::int64_t now = clock();
    auto written = guarded("starting the recovery session", [&]() -> Status {
        SQLite::Statement insert_session(**database, "INSERT INTO recovery_session (id, document_uuid, "
                                                     "source_reference_json, created_ms, updated_ms, cleanly_closed) "
                                                     "VALUES (?, ?, ?, ?, ?, 0)");
        bind_uuid(insert_session, 1, session_id);
        bind_uuid(insert_session, 2, document.id.uuid());
        insert_session.bind(3, encode_source(source, std::nullopt, false));
        insert_session.bind(4, now);
        insert_session.bind(5, now);
        insert_session.exec();
        return ok();
    });
    if (!written)
        return std::unexpected(std::move(written).error());

    std::unique_ptr<RecoveryJournal> journal(
        new RecoveryJournal(std::move(*database), session_id, std::move(source), policy, std::move(clock)));
    journal->lock_ = std::move(*lock);
    journal->last_generation_ = document.generation;
    std::shared_ptr<const DocumentData> base = std::move(*snapshot);
    journal->last_hash_ = hash_executor().submit<Sha256Digest>([base] { return semantic_hash(*base); });
    journal->checkpoint_job_ = journal->submit_checkpoint(base, 0, document.generation, now);
    journal->last_checkpoint_ms_ = now;
    journal->last_durable_ms_ = now;
    return journal;
}

Status RecoveryJournal::record(CommandKind kind, const ChangeSet& applied, Generation base_generation,
                               Generation result_generation, const DocumentData& result_document) {
    std::lock_guard state_lock(state_mutex_);
    if (closed_)
        return make_error(ErrorCode::invalid_argument, "the recovery session is closed");
    if (base_generation != last_generation_)
        return make_error(ErrorCode::invalid_argument,
                          "recovery journal is not contiguous: expected base generation " +
                              std::to_string(last_generation_) + ", got " + std::to_string(base_generation));
    if (result_generation <= base_generation || result_generation > generation_limit)
        return make_error(ErrorCode::invalid_argument, "recovery journal generation must increase");

    const std::int64_t now = clock_();
    const std::size_t payload_size = applied.memory_estimate();
    const bool due = force_checkpoint_ || commands_since_checkpoint_ + 1 >= policy_.checkpoint_command_count ||
                     bytes_since_checkpoint_ + payload_size >= policy_.checkpoint_journal_bytes ||
                     now - last_checkpoint_ms_ >= policy_.checkpoint_interval.count();
    // A newer checkpoint supersedes one still serializing or waiting for flush (flush keeps the
    // newest); thresholds reset on submission, so at most one starts per threshold interval.
    const bool checkpoint_now = due;
    // The next command's base hash is computed when the hashing thread keeps up. Under a burst of
    // edits (auto-repeat on a large document) hashes are skipped instead of queueing a document
    // copy per edit: such commands carry a zero base hash, and replay checks the others.
    const bool hash_now = hash_executor().queued() == 0;
    // Hashing and checkpoint serialization are O(document) (about 40 ms and 2 s at 100k events):
    // both run on background threads from one immutable copy (a few ms), never on this thread.
    // The copy is made before any state changes, so an allocation failure leaves the journal
    // consistent (the edit is then unprotected and reported).
    std::shared_ptr<const DocumentData> snapshot;
    std::optional<Error> copy_error;
    if (hash_now || checkpoint_now) {
        try {
            snapshot = std::make_shared<const DocumentData>(result_document);
        } catch (const std::exception& exception) {
            copy_error = make_error(ErrorCode::limit_exceeded,
                                    std::string("copying the document for recovery: ") + exception.what())
                             .error();
        }
    }

    PendingCommand command;
    command.command_no = last_command_no_ + 1;
    command.base_generation = base_generation;
    command.result_generation = result_generation;
    command.created_ms = now;
    command.envelope.command_id = Uuid::generate();
    command.envelope.kind = kind;
    command.envelope.base_generation = base_generation;
    command.envelope.result_generation = result_generation;
    command.envelope.changes = applied;
    command.base_hash = last_hash_;
    pending_.push_back(std::move(command));

    last_command_no_ += 1;
    last_generation_ = result_generation;
    commands_since_checkpoint_ += 1;
    bytes_since_checkpoint_ += payload_size;
    last_hash_ = snapshot && hash_now
                     ? hash_executor().submit<Sha256Digest>([snapshot] { return semantic_hash(*snapshot); })
                     : ready_digest(Sha256Digest{});
    if (checkpoint_now && snapshot) {
        checkpoint_job_ = submit_checkpoint(snapshot, last_command_no_, result_generation, now);
        commands_since_checkpoint_ = 0;
        bytes_since_checkpoint_ = 0;
        last_checkpoint_ms_ = now;
        force_checkpoint_ = false;
    }

    // A journaled take ends its in-progress capture: the chunks are deleted when this command
    // commits (same transaction), so the evidence is never lost in between.
    for (const auto& operation : applied.operations) {
        if (const auto* insert = std::get_if<TakeInsert>(&operation)) {
            std::erase_if(pending_chunks_, [&](const PendingChunk& chunk) { return chunk.chunk.take == insert->take.id; });
            finished_captures_.push_back(insert->take.id);
        }
    }
    if (copy_error) {
        last_error_ = *copy_error;
        return std::unexpected(std::move(*copy_error));
    }
    return ok();
}

Status RecoveryJournal::record_capture_chunk(CaptureChunk chunk) {
    if (chunk.ordinal < 0 || chunk.first_time_ns < 0 || chunk.start_tick < 0)
        return make_error(ErrorCode::invalid_argument, "capture chunk has a negative ordinal, time or tick");
    if (chunk.payload.size() > max_capture_chunk_bytes)
        return make_error(ErrorCode::invalid_argument, "capture chunk exceeds 1 MiB");
    PendingChunk pending;
    pending.checksum = sha256(chunk.payload);
    pending.chunk = std::move(chunk);
    std::lock_guard state_lock(state_mutex_);
    if (closed_)
        return make_error(ErrorCode::invalid_argument, "the recovery session is closed");
    pending.created_ms = clock_();
    pending_chunks_.push_back(std::move(pending));
    return ok();
}

void RecoveryJournal::discard_capture(const TakeId& take) {
    std::lock_guard state_lock(state_mutex_);
    std::erase_if(pending_chunks_, [&](const PendingChunk& chunk) { return chunk.chunk.take == take; });
    finished_captures_.push_back(take);
}

Status RecoveryJournal::flush(bool wait_for_checkpoint) {
    std::lock_guard database_lock(database_mutex_);
    // Take the buffered work under the state lock; write it without holding that lock, so the
    // document's thread keeps recording while SQLite commits.
    CheckpointJob job;
    {
        std::lock_guard state_lock(state_mutex_);
        if (closed_)
            return ok();
        if (checkpoint_job_.valid() &&
            (wait_for_checkpoint || checkpoint_job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
            job = checkpoint_job_;
            checkpoint_job_ = {};
        }
    }
    std::shared_ptr<const PendingCheckpoint> checkpoint;
    std::optional<Error> checkpoint_error;
    if (job.valid()) {
        // get() rethrows an exception stored by the background task (allocation failure, or a
        // broken promise after shutdown); it must not escape to the recovery thread.
        auto finished = guarded("preparing a recovery checkpoint",
                                [&]() -> Result<std::shared_ptr<const PendingCheckpoint>> { return job.get(); });
        if (finished) {
            checkpoint = std::move(*finished);
        } else {
            // A newer checkpoint is prepared at the next edit.
            checkpoint_error = finished.error();
            std::lock_guard state_lock(state_mutex_);
            last_error_ = finished.error();
            force_checkpoint_ = true;
        }
    }

    std::vector<PendingCommand> batch;
    std::vector<PendingChunk> chunks;
    std::vector<TakeId> finished_captures;
    std::string source_json;
    bool source_changed = false;
    bool writes_entries = false;
    {
        std::lock_guard state_lock(state_mutex_);
        // Keep only the newest checkpoint: an older one waiting after a failed flush is
        // superseded (it would be written after the newer one and block compaction).
        if (pending_checkpoint_ && (!checkpoint || pending_checkpoint_->command_no > checkpoint->command_no))
            checkpoint = pending_checkpoint_;
        pending_checkpoint_.reset();
        // Commands and chunks need a committed base: before the first checkpoint lands they stay
        // buffered (flush(true) waited for it above).
        writes_entries = checkpoint_committed_ || checkpoint;
        if (writes_entries) {
            batch = std::move(pending_);
            pending_.clear();
            chunks = std::move(pending_chunks_);
            pending_chunks_.clear();
            finished_captures = std::move(finished_captures_);
            finished_captures_.clear();
        }
        source_json = encoded_source();
        source_changed = source_changed_;
        source_changed_ = false;
    }
    const auto restore = [&] {
        // On failure the buffer is kept: put the batch back in front of newer entries.
        std::lock_guard state_lock(state_mutex_);
        batch.insert(batch.end(), std::make_move_iterator(pending_.begin()), std::make_move_iterator(pending_.end()));
        pending_ = std::move(batch);
        chunks.insert(chunks.end(), std::make_move_iterator(pending_chunks_.begin()),
                      std::make_move_iterator(pending_chunks_.end()));
        pending_chunks_ = std::move(chunks);
        finished_captures.insert(finished_captures.end(), finished_captures_.begin(), finished_captures_.end());
        finished_captures_ = std::move(finished_captures);
        if (checkpoint && (!pending_checkpoint_ || pending_checkpoint_->command_no < checkpoint->command_no))
            pending_checkpoint_ = checkpoint;
        source_changed_ = source_changed_ || source_changed;
    };
    // Without a committed base, a failed checkpoint means nothing can be protected yet: report it.
    const auto unprotected = [&]() -> Status {
        if (checkpoint_error && !writes_entries)
            return std::unexpected(*checkpoint_error);
        return ok();
    };
    if (batch.empty() && chunks.empty() && finished_captures.empty() && !checkpoint && !source_changed)
        return unprotected();

    // Encode deferred envelopes now (waits only for background hashes, on this thread).
    std::optional<Error> encode_error;
    auto encoded = guarded("encoding recovery commands", [&]() -> Status {
        for (auto it = batch.begin(); it != batch.end();) {
            PendingCommand& command = *it;
            if (command.encoded) {
                ++it;
                continue;
            }
            command.envelope.base_semantic_hash = command.base_hash.get();
            auto payload = encode_command(command.envelope);
            if (!payload) {
                // An envelope that cannot be encoded will never encode: drop it instead of
                // retrying it on every flush (which would hold back every later command). Replay
                // stops at the gap until the checkpoint requested here covers it.
                encode_error = payload.error().with_context(std::string("journaling ") +
                                                            command_kind_name(command.envelope.kind));
                it = batch.erase(it);
                continue;
            }
            command.payload = std::move(*payload);
            command.checksum = sha256(command.payload);
            command.encoded = true;
            command.envelope.changes = {}; // the payload now carries it
            ++it;
        }
        return ok();
    });
    if (!encoded) {
        restore();
        return std::unexpected(std::move(encoded).error());
    }
    if (encode_error) {
        std::lock_guard state_lock(state_mutex_);
        last_error_ = *encode_error;
        force_checkpoint_ = true;
    }

    const std::int64_t now = clock_();
    auto written = guarded("flushing the recovery journal", [&]() -> Status {
        SQLite::Transaction transaction(*database_, SQLite::TransactionBehavior::IMMEDIATE);
        {
            SQLite::Statement insert_command(*database_, "INSERT INTO recovery_command (session_id, command_no, "
                                                         "codec_version, base_generation, result_generation, sha256, "
                                                         "payload, created_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
            for (const auto& command : batch) {
                bind_uuid(insert_command, 1, session_id_);
                insert_command.bind(2, command.command_no);
                insert_command.bind(3, static_cast<std::int64_t>(command_codec_version));
                insert_command.bind(4, static_cast<std::int64_t>(command.base_generation));
                insert_command.bind(5, static_cast<std::int64_t>(command.result_generation));
                bind_bytes(insert_command, 6, command.checksum);
                bind_bytes_no_copy(insert_command, 7, command.payload);
                insert_command.bind(8, command.created_ms);
                insert_command.exec();
                insert_command.reset();
                insert_command.clearBindings();
            }
        }
        if (!chunks.empty()) {
            SQLite::Statement insert_chunk(*database_,
                                           "INSERT OR REPLACE INTO recovery_capture_chunk (session_id, take_id, track_id, "
                                           "ordinal, start_tick, first_time_ns, sha256, payload, created_ms) "
                                           "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
            for (const auto& pending : chunks) {
                bind_uuid(insert_chunk, 1, session_id_);
                bind_uuid(insert_chunk, 2, pending.chunk.take.uuid());
                bind_uuid(insert_chunk, 3, pending.chunk.track.uuid());
                insert_chunk.bind(4, static_cast<std::int64_t>(pending.chunk.ordinal));
                insert_chunk.bind(5, pending.chunk.start_tick);
                insert_chunk.bind(6, pending.chunk.first_time_ns);
                bind_bytes(insert_chunk, 7, pending.checksum);
                bind_bytes_no_copy(insert_chunk, 8, pending.chunk.payload);
                insert_chunk.bind(9, pending.created_ms);
                insert_chunk.exec();
                insert_chunk.reset();
                insert_chunk.clearBindings();
            }
        }
        if (!finished_captures.empty()) {
            SQLite::Statement delete_chunks(*database_, "DELETE FROM recovery_capture_chunk WHERE session_id = ? AND "
                                                        "take_id = ?");
            for (const auto& take : finished_captures) {
                bind_uuid(delete_chunks, 1, session_id_);
                bind_uuid(delete_chunks, 2, take.uuid());
                delete_chunks.exec();
                delete_chunks.reset();
                delete_chunks.clearBindings();
            }
        }
        if (checkpoint) {
            SQLite::Statement insert_checkpoint(*database_, "INSERT OR REPLACE INTO recovery_checkpoint (session_id, "
                                                            "command_no, generation, sha256, native_snapshot, "
                                                            "created_ms) VALUES (?, ?, ?, ?, ?, ?)");
            bind_uuid(insert_checkpoint, 1, session_id_);
            insert_checkpoint.bind(2, checkpoint->command_no);
            insert_checkpoint.bind(3, static_cast<std::int64_t>(checkpoint->generation));
            bind_bytes(insert_checkpoint, 4, checkpoint->checksum);
            bind_bytes_no_copy(insert_checkpoint, 5, checkpoint->snapshot); // tens of MB: never copied
            insert_checkpoint.bind(6, checkpoint->created_ms);
            insert_checkpoint.exec();
        }
        SQLite::Statement update_session(*database_, "UPDATE recovery_session SET updated_ms = ?, "
                                                     "source_reference_json = ? WHERE id = ?");
        update_session.bind(1, now);
        update_session.bind(2, source_json);
        bind_uuid(update_session, 3, session_id_);
        if (update_session.exec() != 1)
            return make_error(ErrorCode::not_found, "recovery session no longer exists");
        transaction.commit();
        return ok();
    });
    if (!written) {
        restore();
        return std::unexpected(std::move(written).error());
    }
    {
        std::lock_guard state_lock(state_mutex_);
        last_durable_ms_ = now;
        if (checkpoint)
            checkpoint_committed_ = true;
    }
    if (!checkpoint)
        return encode_error ? Status(std::unexpected(std::move(*encode_error))) : unprotected();

    // Compaction only after the new checkpoint is committed (section 21.2). The previous
    // checkpoint and the commands after it stay, so recovery can fall back to it when the newest
    // checkpoint fails its checks.
    const std::optional<std::int64_t> previous = committed_checkpoint_no_;
    committed_checkpoint_no_ = std::max(checkpoint->command_no, previous.value_or(checkpoint->command_no));
    if (previous && *previous < checkpoint->command_no) {
        auto compacted = guarded("compacting the recovery journal", [&]() -> Status {
            SQLite::Transaction transaction(*database_, SQLite::TransactionBehavior::IMMEDIATE);
            SQLite::Statement delete_commands(*database_, "DELETE FROM recovery_command WHERE session_id = ? AND "
                                                          "command_no <= ?");
            bind_uuid(delete_commands, 1, session_id_);
            delete_commands.bind(2, *previous);
            delete_commands.exec();
            SQLite::Statement delete_checkpoints(*database_, "DELETE FROM recovery_checkpoint WHERE session_id = ? "
                                                             "AND command_no < ?");
            bind_uuid(delete_checkpoints, 1, session_id_);
            delete_checkpoints.bind(2, *previous);
            delete_checkpoints.exec();
            transaction.commit();
            database_->exec("PRAGMA incremental_vacuum"); // return the freed snapshot pages
            return ok();
        });
        if (!compacted) {
            // The flush itself committed; leftover entries only cost space and are retried at
            // the next checkpoint.
            std::lock_guard state_lock(state_mutex_);
            last_error_ = compacted.error();
        }
    }
    return encode_error ? Status(std::unexpected(std::move(*encode_error))) : ok();
}

std::int64_t RecoveryJournal::last_durable_ms() const {
    std::lock_guard state_lock(state_mutex_);
    return last_durable_ms_;
}

std::size_t RecoveryJournal::pending_commands() const {
    std::lock_guard state_lock(state_mutex_);
    return pending_.size();
}

std::size_t RecoveryJournal::pending_capture_chunks() const {
    std::lock_guard state_lock(state_mutex_);
    return pending_chunks_.size();
}

std::int64_t RecoveryJournal::last_command_no() const {
    std::lock_guard state_lock(state_mutex_);
    return last_command_no_;
}

bool RecoveryJournal::checkpoint_committed() const {
    std::lock_guard state_lock(state_mutex_);
    return checkpoint_committed_;
}

std::optional<Error> RecoveryJournal::last_error() const {
    std::lock_guard state_lock(state_mutex_);
    return last_error_;
}

void RecoveryJournal::set_last_error(Error error) {
    std::lock_guard state_lock(state_mutex_);
    last_error_ = std::move(error);
}

void RecoveryJournal::mark_saved(std::int64_t saved_ms) {
    std::lock_guard state_lock(state_mutex_);
    source_.last_explicit_save_ms = saved_ms;
    source_changed_ = true;
}

void RecoveryJournal::mark_saved(std::int64_t saved_ms, Generation saved_generation) {
    std::lock_guard state_lock(state_mutex_);
    source_.last_explicit_save_ms = saved_ms;
    saved_generation_ = saved_generation;
    source_changed_ = true;
}

void RecoveryJournal::update_source(SourceReference source) {
    std::lock_guard state_lock(state_mutex_);
    source_ = std::move(source);
    source_changed_ = true;
}

Status RecoveryJournal::mark_cleanly_closed() {
    std::lock_guard database_lock(database_mutex_);
    {
        std::lock_guard state_lock(state_mutex_);
        if (closed_)
            return ok();
    }
    // Deleting the session row removes its checkpoints, commands and chunks (ON DELETE CASCADE),
    // so the database does not keep one row per document ever opened.
    MWB_TRY(guarded("closing the recovery session", [&]() -> Status {
        SQLite::Statement remove(*database_, "DELETE FROM recovery_session WHERE id = ?");
        bind_uuid(remove, 1, session_id_);
        remove.exec();
        database_->exec("PRAGMA incremental_vacuum");
        return ok();
    }));
    std::lock_guard state_lock(state_mutex_);
    closed_ = true;
    pending_.clear();
    pending_chunks_.clear();
    finished_captures_.clear();
    pending_checkpoint_.reset();
    checkpoint_job_ = {};
    cancelled_->store(true);
    return ok();
}

// --- Listing, recovery and discard --------------------------------------------------------------

Result<std::vector<RecoverableSession>> list_recoverable_sessions(const std::filesystem::path& directory) {
    if (!recovery_database_exists(directory))
        return std::vector<RecoverableSession>{};
    auto database = open_recovery_database(directory);
    if (!database)
        return std::unexpected(std::move(database).error());
    return guarded("listing recoverable sessions", [&]() -> Result<std::vector<RecoverableSession>> {
        std::vector<RecoverableSession> sessions;
        std::vector<Uuid> dead_without_checkpoint;
        SQLite::Statement statement(
            **database,
            "SELECT s.id, s.document_uuid, s.source_reference_json, s.created_ms, "
            "  (SELECT max(k.command_no) FROM recovery_checkpoint k WHERE k.session_id = s.id), "
            "  (SELECT max(c.command_no) FROM recovery_command c WHERE c.session_id = s.id), "
            "  (SELECT count(*) FROM recovery_command c WHERE c.session_id = s.id AND c.command_no > "
            "     coalesce((SELECT max(k.command_no) FROM recovery_checkpoint k WHERE k.session_id = s.id), 0)), "
            "  (SELECT max(c.result_generation) FROM recovery_command c WHERE c.session_id = s.id), "
            "  (SELECT k.generation FROM recovery_checkpoint k WHERE k.session_id = s.id "
            "     ORDER BY k.command_no DESC LIMIT 1), "
            "  (SELECT count(*) FROM recovery_capture_chunk x WHERE x.session_id = s.id), "
            "  max(coalesce((SELECT max(c.created_ms) FROM recovery_command c WHERE c.session_id = s.id), 0), "
            "      coalesce((SELECT max(k.created_ms) FROM recovery_checkpoint k WHERE k.session_id = s.id), 0), "
            "      coalesce((SELECT max(x.created_ms) FROM recovery_capture_chunk x WHERE x.session_id = s.id), 0), "
            "      s.created_ms) AS last_edit "
            "FROM recovery_session s WHERE s.cleanly_closed = 0 ORDER BY last_edit DESC");
        while (statement.executeStep()) {
            auto session_id = column_uuid(statement, 0, "recovery session id");
            auto document_id = column_uuid(statement, 1, "recovery document id");
            if (!session_id || !document_id)
                continue; // An unreadable row is not offered; the schema CHECKs make this unlikely.
            const bool live =
                file_system::ExclusiveFileLock::is_held(directory / (session_id->to_string() + ".lock"));
            if (statement.getColumn(4).isNull()) {
                // No checkpoint: nothing was ever committed for this session (commands wait for
                // one). A dead one is garbage; a live one is still writing its first checkpoint.
                if (!live)
                    dead_without_checkpoint.push_back(*session_id);
                continue;
            }
            RecoverableSession session;
            session.session_id = *session_id;
            session.document_id = DocumentId(*document_id);
            if (auto source = decode_source(statement.getColumn(2).getString())) {
                session.source = std::move(source->source);
                session.saved_generation = source->saved_generation;
                session.base_saved = source->base_saved;
            }
            session.created_ms = statement.getColumn(3).getInt64();
            const std::int64_t checkpoint_no = statement.getColumn(4).getInt64();
            const std::int64_t command_no = statement.getColumn(5).isNull() ? 0 : statement.getColumn(5).getInt64();
            session.last_command_no = std::max(checkpoint_no, command_no);
            session.journaled_commands = static_cast<std::size_t>(statement.getColumn(6).getInt64());
            const std::int64_t checkpoint_generation = statement.getColumn(8).getInt64();
            const std::int64_t command_generation =
                statement.getColumn(7).isNull() ? 0 : statement.getColumn(7).getInt64();
            session.last_generation = static_cast<Generation>(std::max<std::int64_t>(
                0, std::max(checkpoint_generation, command_generation)));
            session.capture_chunks = static_cast<std::size_t>(statement.getColumn(9).getInt64());
            session.last_durable_edit_ms = statement.getColumn(10).getInt64();
            session.live = live;
            sessions.push_back(std::move(session));
        }
        statement.reset();
        for (const auto& session_id : dead_without_checkpoint) {
            SQLite::Statement remove(**database, "DELETE FROM recovery_session WHERE id = ?");
            bind_uuid(remove, 1, session_id);
            remove.exec();
        }
        return sessions;
    });
}

namespace {

/// One journaled command as stored.
struct StoredCommand {
    std::int64_t command_no = 0;
    std::int64_t codec = 0;
    std::int64_t base_generation = 0;
    std::int64_t result_generation = 0;
    std::optional<Sha256Digest> digest;
    std::vector<std::uint8_t> payload;
};

/// Replays `commands` onto `document` through the last valid entry. With `verify_all` false the
/// base semantic hash (an O(document) computation) is compared for the first, every 16th and the
/// last command only; returns true when such a sampled comparison failed, so the caller re-runs
/// with every hash checked to find the exact last valid command.
bool replay_commands(DocumentData& document, const std::vector<StoredCommand>& commands, bool verify_all,
                     RecoveryReport& report) {
    std::int64_t expected_no = report.checkpoint_command_no + 1;
    bool stopped = false;
    const auto stop = [&](std::string reason) {
        report.stop_reason = std::move(reason);
        stopped = true;
    };
    const auto hash_mismatch = [&](const CommandEnvelope& envelope) {
        ++report.verified_hashes;
        return envelope.base_semantic_hash != semantic_hash(document);
    };
    for (std::size_t index = 0; index < commands.size(); ++index) {
        if (stopped) {
            ++report.skipped_commands;
            continue;
        }
        const StoredCommand& row = commands[index];
        const auto entry = "command " + std::to_string(row.command_no) + ": ";
        const bool check_hash =
            verify_all || index % replay_hash_stride == 0 || index + 1 == commands.size();
        if (row.command_no != expected_no) {
            stop(entry + "gap in the command sequence");
        } else if (row.codec != command_codec_version) {
            stop(entry + "unsupported codec version " + std::to_string(row.codec));
        } else if (!row.digest || sha256(row.payload) != *row.digest) {
            stop(entry + "checksum mismatch");
        } else if (auto envelope = decode_command(row.payload); !envelope) {
            stop(entry + envelope.error().message);
        } else if (row.base_generation < 0 ||
                   envelope->base_generation != static_cast<Generation>(row.base_generation) ||
                   envelope->result_generation != static_cast<Generation>(row.result_generation)) {
            stop(entry + "generation columns disagree with the envelope");
        } else if (envelope->base_generation != document.generation) {
            stop(entry + "base generation does not follow the recovered state");
        } else if (check_hash && envelope->base_semantic_hash != Sha256Digest{} && // zero: not recorded
                   hash_mismatch(*envelope)) {
            if (!verify_all)
                return true;
            stop(entry + "base semantic hash does not match the recovered state");
        } else if (auto applied = apply_change_set(document, envelope->changes); !applied) {
            stop(entry + "cannot be applied: " + applied.error().message);
        } else {
            document.generation = envelope->result_generation;
            ++report.applied_commands;
            report.last_valid_command_no = row.command_no;
            ++expected_no;
            continue;
        }
        ++report.skipped_commands;
    }
    return false;
}

/// One stored capture chunk.
struct StoredChunk {
    TrackId track;
    std::int32_t ordinal = 0;
    Tick start_tick = 0;
    std::int64_t first_time_ns = 0;
    std::int64_t created_ms = 0;
    std::vector<std::uint8_t> payload;
};

/// Adds interrupted recordings rebuilt from capture chunks. A take already in the document (its
/// TakeInsert was replayed) or on a track that no longer exists is skipped; a take keeps its
/// chunks up to the first damaged or missing one.
void add_interrupted_takes(DocumentData& document, std::map<Uuid, std::vector<StoredChunk>>& captures,
                           RecoveryReport& report) {
    for (auto& [take_uuid, chunks] : captures) {
        const TakeId take_id(take_uuid);
        if (chunks.empty() || !find_track(document, chunks.front().track) ||
            std::any_of(document.takes.begin(), document.takes.end(),
                        [&](const RecordingTake& take) { return take.id == take_id; }))
            continue;
        RecordingTake take;
        take.id = take_id;
        take.track = chunks.front().track;
        take.name = "Recovered take";
        take.created_ms = chunks.front().created_ms;
        take.start_tick = chunks.front().start_tick;
        take.end_tick = take.start_tick;
        take.status = TakeStatus::interrupted;
        take.capture_json = json{{"version", 1},
                                 {"raw_division", "nanoseconds"},
                                 {"complete", false},
                                 {"interruption", "the application ended during the recording"},
                                 {"recovered_chunks", chunks.size()}}
                                .dump();
        for (auto& chunk : chunks)
            take.chunks.push_back(TakeChunk{chunk.ordinal, chunk.first_time_ns, std::move(chunk.payload)});
        ChangeSet changes;
        changes.label = "Recovered take";
        changes.operations.emplace_back(TakeInsert{document.takes.size(), std::move(take)});
        if (apply_change_set(document, changes))
            ++report.recovered_takes;
    }
}

} // namespace

Result<RecoveredDocument> recover_session(const std::filesystem::path& directory, const Uuid& session_id) {
    if (!recovery_database_exists(directory))
        return make_error(ErrorCode::not_found, "there is no recovery database");
    auto database = open_recovery_database(directory);
    if (!database)
        return std::unexpected(std::move(database).error());
    return guarded("recovering the session", [&]() -> Result<RecoveredDocument> {
        SQLite::Database& db = **database;
        RecoveredDocument recovered;
        {
            SQLite::Statement statement(db, "SELECT source_reference_json FROM recovery_session WHERE id = ?");
            bind_uuid(statement, 1, session_id);
            if (!statement.executeStep())
                return make_error(ErrorCode::not_found, "recovery session does not exist");
            if (auto source = decode_source(statement.getColumn(0).getString()))
                recovered.source = std::move(source->source);
        }

        // Newest checkpoint that passes its checksum and native validation; an older one is the
        // fallback (compaction keeps the previous checkpoint and the commands after it).
        bool have_checkpoint = false;
        {
            SQLite::Statement statement(db, "SELECT command_no, generation, sha256, native_snapshot FROM "
                                            "recovery_checkpoint WHERE session_id = ? ORDER BY command_no DESC");
            bind_uuid(statement, 1, session_id);
            const NativeLoadLimits limits = checkpoint_limits();
            while (statement.executeStep()) {
                const std::int64_t command_no = statement.getColumn(0).getInt64();
                const std::int64_t generation = statement.getColumn(1).getInt64();
                const auto digest = column_digest(statement, 2, "checkpoint sha256");
                const auto snapshot = column_bytes(statement, 3);
                if (!digest || generation < 0 || sha256(snapshot) != *digest) {
                    ++recovered.report.rejected_checkpoints;
                    continue;
                }
                // Documents above the editable event limit (for example a large import) are
                // still recovered; only damage or content this version cannot keep is refused.
                auto opened = open_native_bytes(snapshot, limits);
                if (!opened || opened->read_only) {
                    ++recovered.report.rejected_checkpoints;
                    continue;
                }
                recovered.document = std::move(opened->document);
                recovered.document.generation = static_cast<Generation>(generation);
                recovered.report.checkpoint_command_no = command_no;
                recovered.report.checkpoint_generation = static_cast<Generation>(generation);
                recovered.report.last_valid_command_no = command_no;
                have_checkpoint = true;
                break;
            }
        }
        if (!have_checkpoint)
            return make_error(ErrorCode::invalid_data, "recovery session has no valid checkpoint");

        std::vector<StoredCommand> commands;
        {
            SQLite::Statement statement(db, "SELECT command_no, codec_version, base_generation, result_generation, "
                                            "sha256, payload FROM recovery_command WHERE session_id = ? AND "
                                            "command_no > ? ORDER BY command_no");
            bind_uuid(statement, 1, session_id);
            statement.bind(2, recovered.report.checkpoint_command_no);
            while (statement.executeStep()) {
                StoredCommand row;
                row.command_no = statement.getColumn(0).getInt64();
                row.codec = statement.getColumn(1).getInt64();
                row.base_generation = statement.getColumn(2).getInt64();
                row.result_generation = statement.getColumn(3).getInt64();
                if (auto digest = column_digest(statement, 4, "command sha256"))
                    row.digest = *digest;
                row.payload = column_bytes(statement, 5);
                commands.push_back(std::move(row));
            }
        }
        // Replay with sampled hash checks (each is O(document)); a failed sample replays again
        // from the checkpoint checking every command, so the result is exact either way.
        const DocumentData checkpoint_document = recovered.document;
        const RecoveryReport checkpoint_report = recovered.report;
        if (replay_commands(recovered.document, commands, false, recovered.report)) {
            recovered.document = checkpoint_document;
            recovered.report = checkpoint_report;
            std::ignore = replay_commands(recovered.document, commands, true, recovered.report);
        }

        // Recordings that were in progress (section 21.2).
        std::map<Uuid, std::vector<StoredChunk>> captures;
        {
            SQLite::Statement statement(db, "SELECT take_id, track_id, ordinal, start_tick, first_time_ns, sha256, "
                                            "payload, created_ms FROM recovery_capture_chunk WHERE session_id = ? "
                                            "ORDER BY take_id, ordinal");
            bind_uuid(statement, 1, session_id);
            std::map<Uuid, bool> broken;
            while (statement.executeStep()) {
                auto take = column_uuid(statement, 0, "capture take id");
                auto track = column_uuid(statement, 1, "capture track id");
                if (!take || !track || broken[*take])
                    continue;
                StoredChunk chunk;
                chunk.track = TrackId(*track);
                chunk.ordinal = static_cast<std::int32_t>(
                    std::clamp<std::int64_t>(statement.getColumn(2).getInt64(), 0, std::numeric_limits<std::int32_t>::max()));
                chunk.start_tick = statement.getColumn(3).getInt64();
                chunk.first_time_ns = statement.getColumn(4).getInt64();
                const auto digest = column_digest(statement, 5, "capture chunk sha256");
                chunk.payload = column_bytes(statement, 6);
                chunk.created_ms = statement.getColumn(7).getInt64();
                auto& group = captures[*take];
                const std::int32_t expected = group.empty() ? 0 : group.back().ordinal + 1;
                if (!digest || sha256(chunk.payload) != *digest || chunk.ordinal != expected ||
                    (!group.empty() && group.front().track != chunk.track)) {
                    broken[*take] = true; // keep the intact prefix only
                    continue;
                }
                group.push_back(std::move(chunk));
            }
        }
        add_interrupted_takes(recovered.document, captures, recovered.report);
        return recovered;
    });
}

Status discard_recovery_session(const std::filesystem::path& directory, const Uuid& session_id) {
    if (file_system::ExclusiveFileLock::is_held(directory / (session_id.to_string() + ".lock")))
        return make_error(ErrorCode::conflict, "the session belongs to a running instance and cannot be discarded");
    auto database = open_recovery_database(directory);
    if (!database)
        return std::unexpected(std::move(database).error());
    return guarded("discarding the recovery session", [&]() -> Status {
        SQLite::Statement statement(**database, "DELETE FROM recovery_session WHERE id = ?");
        bind_uuid(statement, 1, session_id);
        if (statement.exec() == 0)
            return make_error(ErrorCode::not_found, "recovery session does not exist");
        (*database)->exec("PRAGMA incremental_vacuum");
        return ok();
    });
}

std::optional<std::string> take_recovery_database_notice() {
    std::lock_guard lock(notice_mutex);
    return std::exchange(database_notice, std::nullopt);
}

void shutdown_recovery_executors() {
    checkpoint_executor().shutdown(true);
    hash_executor().shutdown(true);
}

ListenerId connect_recovery(WorkingSession& session, const std::shared_ptr<RecoveryJournal>& journal) {
    std::weak_ptr<RecoveryJournal> weak = journal;
    return session.add_listener([weak](const SessionEvent& event) {
        const auto target = weak.lock();
        if (!target)
            return;
        CommandKind kind = CommandKind::edit;
        switch (event.kind) {
        case SessionEventKind::edited: kind = CommandKind::edit; break;
        case SessionEventKind::undone: kind = CommandKind::undo; break;
        case SessionEventKind::redone: kind = CommandKind::redo; break;
        case SessionEventKind::saved: target->mark_saved(now_utc_ms(), event.result_generation); return;
        case SessionEventKind::history_trimmed: return; // not a document transition
        }
        // Never throw into the session: the edit is already applied and the listener runs inside
        // WorkingSession::apply (a Qt slot in the application).
        try {
            if (auto status = target->record(kind, event.applied, event.base_generation, event.result_generation,
                                             event.document);
                !status)
                target->set_last_error(std::move(status).error());
        } catch (const std::exception& exception) {
            target->set_last_error(
                make_error(ErrorCode::limit_exceeded, std::string("recording the edit for recovery: ") + exception.what())
                    .error());
        }
    });
}

} // namespace mwb::document
