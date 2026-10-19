#include "mwb/document/recovery.hpp"

#include "mwb/core/serialization.hpp"
#include "mwb/document/migration.hpp"
#include "mwb/document/native_format.hpp"
#include "mwb/document/schemas.hpp"
#include "mwb/document/sqlite_support.hpp"

#include <SQLiteCpp/Transaction.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>
#include <thread>
#include <memory>
#include <limits>
#include <system_error>
#include <utility>

namespace mwb::document {

namespace {

using namespace sqlite_support;
using nlohmann::json;

/// Busy timeout for the shared per-user recovery database (several documents or instances).
constexpr int recovery_busy_timeout_ms = 5000;

std::int64_t default_clock() { return now_utc_ms(); }

std::string encode_source(const SourceReference& source) {
    const json value{
        {"version", 1},
        {"document_name", source.document_name},
        {"file_path", source.file_path_utf8 ? json(*source.file_path_utf8) : json(nullptr)},
        {"source_identity", source.source_identity ? json(*source.source_identity) : json(nullptr)},
        {"last_explicit_save_ms", source.last_explicit_save_ms ? json(*source.last_explicit_save_ms) : json(nullptr)},
    };
    // error_handler_t::replace keeps the JSON valid even for a title with invalid UTF-8.
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

Result<SourceReference> decode_source(const std::string& text) {
    const json value = json::parse(text, nullptr, false);
    if (value.is_discarded() || !value.is_object())
        return make_error(ErrorCode::invalid_data, "recovery source reference is not a JSON object");
    const auto version = value.find("version");
    if (version == value.end() || !version->is_number_integer() || version->get<std::int64_t>() != 1)
        return make_error(ErrorCode::unsupported, "recovery source reference has an unsupported version");
    SourceReference source;
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
    return source;
}

/// Opens or creates the recovery database and checks its identity, version and schema.
/// Throws SQLite::Exception; callers run it inside guarded().
Result<std::unique_ptr<SQLite::Database>> open_database(const std::filesystem::path& directory) {
    auto database = std::make_unique<SQLite::Database>(path_to_utf8(directory / recovery_database_name),
                                                       SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE,
                                                       recovery_busy_timeout_ms);
    database->exec("PRAGMA journal_mode = WAL");
    database->exec("PRAGMA synchronous = FULL");
    database->exec("PRAGMA foreign_keys = ON");
    database->exec("PRAGMA trusted_schema = OFF");
    {
        SQLite::Transaction transaction(*database, SQLite::TransactionBehavior::IMMEDIATE);
        std::int64_t objects = 0;
        {
            SQLite::Statement count(*database, "SELECT count(*) FROM sqlite_schema");
            count.executeStep();
            objects = count.getColumn(0).getInt64();
        }
        if (objects == 0 && pragma_integer(*database, "application_id") == 0)
            database->exec(std::string(schemas::recovery_v1));
        transaction.commit();
    }
    if (pragma_integer(*database, "application_id") != recovery_application_id)
        return make_error(ErrorCode::invalid_data, "recovery database has the wrong application id");
    auto access =
        migration::classify_version(migration::SchemaFamily::recovery, pragma_integer(*database, "user_version"));
    if (!access)
        return std::unexpected(std::move(access).error());
    if (*access != migration::SchemaAccess::read_write)
        return make_error(ErrorCode::unsupported, "recovery database needs a schema migration");
    MWB_TRY(verify_schema_whitelist(*database, schemas::recovery_v1, "recovery database"));
    return database;
}

Result<std::unique_ptr<SQLite::Database>> open_recovery_database(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return make_error(ErrorCode::io, "cannot create the recovery directory: " + error.message());
    return guarded("opening the recovery database", [&] { return open_database(directory); });
}

std::string kind_label(CommandKind kind) {
    switch (kind) {
    case CommandKind::edit: return "edit";
    case CommandKind::undo: return "undo";
    case CommandKind::redo: return "redo";
    }
    return "edit";
}

constexpr auto generation_limit = static_cast<Generation>(std::numeric_limits<std::int64_t>::max());

/// One background thread that computes semantic hashes for every journal in order, so recording
/// an edit never waits for an O(document) hash and never starts a thread per edit. Queued work
/// runs before the thread exits (a future must never be abandoned).
class HashExecutor {
public:
    HashExecutor()
        : thread_([this](std::stop_token stop) {
              while (true) {
                  std::packaged_task<Sha256Digest()> task;
                  {
                      std::unique_lock lock(mutex_);
                      wake_.wait(lock, stop, [this] { return !tasks_.empty(); });
                      if (tasks_.empty())
                          return; // stop requested and nothing left
                      task = std::move(tasks_.front());
                      tasks_.pop_front();
                  }
                  task();
              }
          }) {}
    ~HashExecutor() {
        thread_.request_stop();
        wake_.notify_all();
    }
    std::shared_future<Sha256Digest> submit(std::shared_ptr<const DocumentData> snapshot) {
        std::packaged_task<Sha256Digest()> task([snapshot = std::move(snapshot)] { return semantic_hash(*snapshot); });
        auto future = task.get_future().share();
        {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        wake_.notify_all();
        return future;
    }

private:
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<std::packaged_task<Sha256Digest()>> tasks_;
    std::jthread thread_;
};

HashExecutor& hash_executor() {
    static HashExecutor executor;
    return executor;
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
    bool unused = false;
    {
        std::lock_guard state_lock(state_mutex_);
        unused = discard_if_unused_ && last_command_no_ == 0;
    }
    if (unused)
        std::ignore = mark_cleanly_closed(); // nothing beyond the opened file to recover
}

void RecoveryJournal::set_discard_if_unused(bool discard) {
    std::lock_guard state_lock(state_mutex_);
    discard_if_unused_ = discard;
}

Result<std::unique_ptr<RecoveryJournal>> RecoveryJournal::create(const std::filesystem::path& directory,
                                                                 const DocumentData& document, SourceReference source,
                                                                 RecoveryPolicy policy, RecoveryClock clock) {
    if (!clock)
        clock = default_clock;
    if (document.generation > generation_limit)
        return make_error(ErrorCode::invalid_argument, "document generation does not fit the recovery journal");
    auto snapshot = native_to_bytes(document);
    if (!snapshot)
        return std::unexpected(std::move(snapshot).error().with_context("creating the recovery checkpoint"));
    auto database = open_recovery_database(directory);
    if (!database)
        return std::unexpected(std::move(database).error());

    const Uuid session_id = Uuid::generate();
    // Held for the journal's lifetime: other instances see the session as live, not crashed.
    auto lock = file_system::ExclusiveFileLock::acquire(directory / (session_id.to_string() + ".lock"));
    if (!lock)
        return std::unexpected(std::move(lock).error().with_context("locking the recovery session"));
    const std::int64_t now = clock();
    auto written = guarded("starting the recovery session", [&]() -> Status {
        SQLite::Transaction transaction(**database, SQLite::TransactionBehavior::IMMEDIATE);
        SQLite::Statement insert_session(**database, "INSERT INTO recovery_session (id, document_uuid, "
                                                     "source_reference_json, created_ms, updated_ms, cleanly_closed) "
                                                     "VALUES (?, ?, ?, ?, ?, 0)");
        bind_uuid(insert_session, 1, session_id);
        bind_uuid(insert_session, 2, document.id.uuid());
        insert_session.bind(3, encode_source(source));
        insert_session.bind(4, now);
        insert_session.bind(5, now);
        insert_session.exec();
        SQLite::Statement insert_checkpoint(**database, "INSERT INTO recovery_checkpoint (session_id, command_no, "
                                                        "generation, sha256, native_snapshot, created_ms) "
                                                        "VALUES (?, 0, ?, ?, ?, ?)");
        bind_uuid(insert_checkpoint, 1, session_id);
        insert_checkpoint.bind(2, static_cast<std::int64_t>(document.generation));
        bind_bytes(insert_checkpoint, 3, sha256(*snapshot));
        bind_bytes(insert_checkpoint, 4, *snapshot);
        insert_checkpoint.bind(5, now);
        insert_checkpoint.exec();
        transaction.commit();
        return ok();
    });
    if (!written)
        return std::unexpected(std::move(written).error());

    std::unique_ptr<RecoveryJournal> journal(
        new RecoveryJournal(std::move(*database), session_id, std::move(source), policy, std::move(clock)));
    journal->lock_ = std::move(*lock);
    journal->last_generation_ = document.generation;
    journal->last_hash_ = std::async(std::launch::deferred, [hash = semantic_hash(document)] { return hash; }).share();
    journal->last_checkpoint_ms_ = now;
    journal->last_durable_ms_ = now;
    return journal;
}

Status RecoveryJournal::record(CommandKind kind, const ChangeSet& applied, Generation base_generation,
                               Generation result_generation, const DocumentData& result_document) {
    std::lock_guard state_lock(state_mutex_);
    if (base_generation != last_generation_)
        return make_error(ErrorCode::invalid_argument,
                          "recovery journal is not contiguous: expected base generation " +
                              std::to_string(last_generation_) + ", got " + std::to_string(base_generation));
    if (result_generation <= base_generation || result_generation > generation_limit)
        return make_error(ErrorCode::invalid_argument, "recovery journal generation must increase");

    // Recording is O(change): the envelope is encoded at flush time on the recovery thread, once
    // the base document's hash (computed in the background) is available.
    PendingCommand command;
    command.command_no = last_command_no_ + 1;
    command.base_generation = base_generation;
    command.result_generation = result_generation;
    command.created_ms = clock_();
    command.envelope.command_id = Uuid::generate();
    command.envelope.kind = kind;
    command.envelope.base_generation = base_generation;
    command.envelope.result_generation = result_generation;
    command.envelope.changes = applied;
    command.base_hash = last_hash_;
    const std::size_t payload_size = applied.memory_estimate();
    pending_.push_back(std::move(command));

    last_command_no_ += 1;
    last_generation_ = result_generation;
    // Hashing and checkpoint serialization are O(document) (about 40 ms and 1.4 s at 100k events):
    // both run on background threads from one immutable copy (a few ms), never on the GUI thread.
    auto snapshot = std::make_shared<const DocumentData>(result_document);
    last_hash_ = hash_executor().submit(snapshot);
    commands_since_checkpoint_ += 1;
    bytes_since_checkpoint_ += payload_size;

    const std::int64_t now = clock_();
    const bool due = commands_since_checkpoint_ >= policy_.checkpoint_command_count ||
                     bytes_since_checkpoint_ >= policy_.checkpoint_journal_bytes ||
                     now - last_checkpoint_ms_ >= policy_.checkpoint_interval.count();
    if (due && !checkpoint_job_.valid() && !pending_checkpoint_) {
        checkpoint_job_ = std::async(std::launch::async,
                                     [snapshot, command_no = last_command_no_, result_generation, now]() -> Result<PendingCheckpoint> {
                                         auto bytes = native_to_bytes(*snapshot);
                                         if (!bytes)
                                             return std::unexpected(std::move(bytes).error().with_context(
                                                 "preparing a recovery checkpoint"));
                                         PendingCheckpoint checkpoint;
                                         checkpoint.command_no = command_no;
                                         checkpoint.generation = result_generation;
                                         checkpoint.checksum = sha256(*bytes);
                                         checkpoint.snapshot = std::move(*bytes);
                                         checkpoint.created_ms = now;
                                         return checkpoint;
                                     });
        commands_since_checkpoint_ = 0;
        bytes_since_checkpoint_ = 0;
        last_checkpoint_ms_ = now;
    }
    return ok();
}

Status RecoveryJournal::flush(bool wait_for_checkpoint) {
    std::lock_guard database_lock(database_mutex_);
    // Take the buffered work under the state lock; write it without holding that lock, so the
    // document's thread keeps recording while SQLite commits.
    std::future<Result<PendingCheckpoint>> job;
    {
        std::lock_guard state_lock(state_mutex_);
        if (checkpoint_job_.valid() &&
            (wait_for_checkpoint || checkpoint_job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready))
            job = std::move(checkpoint_job_);
    }
    std::optional<PendingCheckpoint> checkpoint;
    if (job.valid()) {
        auto finished = job.get();
        if (!finished)
            return std::unexpected(std::move(finished).error());
        checkpoint = std::move(*finished);
    }
    std::vector<PendingCommand> batch;
    SourceReference source;
    bool source_changed = false;
    {
        std::lock_guard state_lock(state_mutex_);
        if (!checkpoint && pending_checkpoint_) {
            checkpoint = std::move(pending_checkpoint_);
            pending_checkpoint_.reset();
        }
        batch = std::move(pending_);
        pending_.clear();
        source = source_;
        source_changed = source_changed_;
        source_changed_ = false;
    }
    if (batch.empty() && !checkpoint && !source_changed)
        return ok();
    // Encode deferred envelopes now (waits only for background hashes, on this thread).
    for (auto& command : batch) {
        if (command.encoded)
            continue;
        command.envelope.base_semantic_hash = command.base_hash.get();
        auto payload = encode_command(command.envelope);
        if (!payload) {
            std::lock_guard state_lock(state_mutex_);
            batch.insert(batch.end(), std::make_move_iterator(pending_.begin()), std::make_move_iterator(pending_.end()));
            pending_ = std::move(batch);
            return std::unexpected(std::move(payload).error().with_context("journaling " + kind_label(command.envelope.kind)));
        }
        command.payload = std::move(*payload);
        command.checksum = sha256(command.payload);
        command.encoded = true;
        command.envelope.changes = {}; // the payload now carries it
    }
    const auto restore = [&] {
        // On failure the buffer is kept: put the batch back in front of newer commands.
        std::lock_guard state_lock(state_mutex_);
        batch.insert(batch.end(), std::make_move_iterator(pending_.begin()), std::make_move_iterator(pending_.end()));
        pending_ = std::move(batch);
        if (checkpoint && !pending_checkpoint_)
            pending_checkpoint_ = std::move(checkpoint);
        source_changed_ = source_changed_ || source_changed;
    };
    const std::int64_t now = clock_();
    auto written = guarded("flushing the recovery journal", [&]() -> Status {
        SQLite::Transaction transaction(*database_, SQLite::TransactionBehavior::IMMEDIATE);
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
            bind_bytes(insert_command, 7, command.payload);
            insert_command.bind(8, command.created_ms);
            insert_command.exec();
            insert_command.reset();
            insert_command.clearBindings();
        }
        if (checkpoint) {
            SQLite::Statement insert_checkpoint(*database_, "INSERT INTO recovery_checkpoint (session_id, command_no, "
                                                            "generation, sha256, native_snapshot, created_ms) "
                                                            "VALUES (?, ?, ?, ?, ?, ?)");
            bind_uuid(insert_checkpoint, 1, session_id_);
            insert_checkpoint.bind(2, checkpoint->command_no);
            insert_checkpoint.bind(3, static_cast<std::int64_t>(checkpoint->generation));
            bind_bytes(insert_checkpoint, 4, checkpoint->checksum);
            bind_bytes(insert_checkpoint, 5, checkpoint->snapshot);
            insert_checkpoint.bind(6, checkpoint->created_ms);
            insert_checkpoint.exec();
        }
        SQLite::Statement update_session(*database_, "UPDATE recovery_session SET updated_ms = ?, "
                                                     "source_reference_json = ? WHERE id = ?");
        update_session.bind(1, now);
        update_session.bind(2, encode_source(source));
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
    }
    if (!checkpoint)
        return ok();
    const std::int64_t checkpoint_no = checkpoint->command_no;
    // Compaction only after the new checkpoint is committed (section 21.2).
    return guarded("compacting the recovery journal", [&]() -> Status {
        SQLite::Transaction transaction(*database_, SQLite::TransactionBehavior::IMMEDIATE);
        SQLite::Statement delete_commands(*database_, "DELETE FROM recovery_command WHERE session_id = ? AND "
                                                      "command_no <= ?");
        bind_uuid(delete_commands, 1, session_id_);
        delete_commands.bind(2, checkpoint_no);
        delete_commands.exec();
        SQLite::Statement delete_checkpoints(*database_, "DELETE FROM recovery_checkpoint WHERE session_id = ? AND "
                                                         "command_no < ?");
        bind_uuid(delete_checkpoints, 1, session_id_);
        delete_checkpoints.bind(2, checkpoint_no);
        delete_checkpoints.exec();
        transaction.commit();
        return ok();
    });
}

std::int64_t RecoveryJournal::last_durable_ms() const {
    std::lock_guard state_lock(state_mutex_);
    return last_durable_ms_;
}

std::size_t RecoveryJournal::pending_commands() const {
    std::lock_guard state_lock(state_mutex_);
    return pending_.size();
}

std::int64_t RecoveryJournal::last_command_no() const {
    std::lock_guard state_lock(state_mutex_);
    return last_command_no_;
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

void RecoveryJournal::update_source(SourceReference source) {
    std::lock_guard state_lock(state_mutex_);
    source_ = std::move(source);
    source_changed_ = true;
}

Status RecoveryJournal::mark_cleanly_closed() {
    std::lock_guard database_lock(database_mutex_);
    MWB_TRY(guarded("closing the recovery session", [&]() -> Status {
        SQLite::Transaction transaction(*database_, SQLite::TransactionBehavior::IMMEDIATE);
        SQLite::Statement update(*database_, "UPDATE recovery_session SET cleanly_closed = 1, updated_ms = ? "
                                             "WHERE id = ?");
        update.bind(1, clock_());
        bind_uuid(update, 2, session_id_);
        update.exec();
        SQLite::Statement delete_commands(*database_, "DELETE FROM recovery_command WHERE session_id = ?");
        bind_uuid(delete_commands, 1, session_id_);
        delete_commands.exec();
        SQLite::Statement delete_checkpoints(*database_, "DELETE FROM recovery_checkpoint WHERE session_id = ?");
        bind_uuid(delete_checkpoints, 1, session_id_);
        delete_checkpoints.exec();
        transaction.commit();
        return ok();
    }));
    std::lock_guard state_lock(state_mutex_);
    pending_.clear();
    pending_checkpoint_.reset();
    return ok();
}

// --- Listing, recovery and discard --------------------------------------------------------------

Result<std::vector<RecoverableSession>> list_recoverable_sessions(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::exists(directory / recovery_database_name, error))
        return std::vector<RecoverableSession>{};
    auto database = open_recovery_database(directory);
    if (!database)
        return std::unexpected(std::move(database).error());
    return guarded("listing recoverable sessions", [&]() -> Result<std::vector<RecoverableSession>> {
        std::vector<RecoverableSession> sessions;
        SQLite::Statement statement(
            **database,
            "SELECT s.id, s.document_uuid, s.source_reference_json, s.created_ms, s.updated_ms, "
            "(SELECT count(*) FROM recovery_command c WHERE c.session_id = s.id AND c.command_no > "
            "  coalesce((SELECT max(k.command_no) FROM recovery_checkpoint k WHERE k.session_id = s.id), 0)) "
            "FROM recovery_session s WHERE s.cleanly_closed = 0 ORDER BY s.updated_ms DESC");
        while (statement.executeStep()) {
            auto session_id = column_uuid(statement, 0, "recovery session id");
            auto document_id = column_uuid(statement, 1, "recovery document id");
            if (!session_id || !document_id)
                continue; // An unreadable row is not offered; the schema CHECKs make this unlikely.
            auto source = decode_source(statement.getColumn(2).getString());
            RecoverableSession session;
            session.session_id = *session_id;
            session.document_id = DocumentId(*document_id);
            if (source)
                session.source = std::move(*source);
            session.created_ms = statement.getColumn(3).getInt64();
            session.last_durable_edit_ms = statement.getColumn(4).getInt64();
            session.journaled_commands = static_cast<std::size_t>(statement.getColumn(5).getInt64());
            session.live = file_system::ExclusiveFileLock::is_held(directory / (session.session_id.to_string() + ".lock"));
            sessions.push_back(std::move(session));
        }
        return sessions;
    });
}

Result<RecoveredDocument> recover_session(const std::filesystem::path& directory, const Uuid& session_id) {
    std::error_code error;
    if (!std::filesystem::exists(directory / recovery_database_name, error))
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
            auto source = decode_source(statement.getColumn(0).getString());
            if (source)
                recovered.source = std::move(*source);
        }

        // Newest checkpoint that passes its checksum and native validation.
        bool have_checkpoint = false;
        {
            SQLite::Statement statement(db, "SELECT command_no, generation, sha256, native_snapshot FROM "
                                            "recovery_checkpoint WHERE session_id = ? ORDER BY command_no DESC");
            bind_uuid(statement, 1, session_id);
            while (statement.executeStep()) {
                const std::int64_t command_no = statement.getColumn(0).getInt64();
                const std::int64_t generation = statement.getColumn(1).getInt64();
                const auto digest = column_digest(statement, 2, "checkpoint sha256");
                const auto snapshot = column_bytes(statement, 3);
                if (!digest || generation < 0 || sha256(snapshot) != *digest) {
                    ++recovered.report.rejected_checkpoints;
                    continue;
                }
                auto document = native_from_bytes(snapshot);
                if (!document) {
                    ++recovered.report.rejected_checkpoints;
                    continue;
                }
                recovered.document = std::move(*document);
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

        // Replay through the last valid entry.
        SQLite::Statement statement(db, "SELECT command_no, codec_version, base_generation, result_generation, sha256, "
                                        "payload FROM recovery_command WHERE session_id = ? AND command_no > ? "
                                        "ORDER BY command_no");
        bind_uuid(statement, 1, session_id);
        statement.bind(2, recovered.report.checkpoint_command_no);
        DocumentData& document = recovered.document;
        std::int64_t expected_no = recovered.report.checkpoint_command_no + 1;
        bool stopped = false;
        const auto stop = [&](std::string reason) {
            recovered.report.stop_reason = std::move(reason);
            stopped = true;
        };
        while (statement.executeStep()) {
            if (stopped) {
                ++recovered.report.skipped_commands;
                continue;
            }
            const std::int64_t command_no = statement.getColumn(0).getInt64();
            const std::int64_t codec = statement.getColumn(1).getInt64();
            const std::int64_t base_generation = statement.getColumn(2).getInt64();
            const std::int64_t result_generation = statement.getColumn(3).getInt64();
            const auto digest = column_digest(statement, 4, "command sha256");
            const auto payload = column_bytes(statement, 5);
            const auto entry = "command " + std::to_string(command_no) + ": ";
            if (command_no != expected_no) {
                stop(entry + "gap in the command sequence");
            } else if (codec != command_codec_version) {
                stop(entry + "unsupported codec version " + std::to_string(codec));
            } else if (!digest || sha256(payload) != *digest) {
                stop(entry + "checksum mismatch");
            } else if (auto envelope = decode_command(payload); !envelope) {
                stop(entry + envelope.error().message);
            } else if (base_generation < 0 || envelope->base_generation != static_cast<Generation>(base_generation) ||
                       envelope->result_generation != static_cast<Generation>(result_generation)) {
                stop(entry + "generation columns disagree with the envelope");
            } else if (envelope->base_generation != document.generation) {
                stop(entry + "base generation does not follow the recovered state");
            } else if (envelope->base_semantic_hash != semantic_hash(document)) {
                stop(entry + "base semantic hash does not match the recovered state");
            } else if (auto applied = apply_change_set(document, envelope->changes); !applied) {
                stop(entry + "cannot be applied: " + applied.error().message);
            } else {
                document.generation = envelope->result_generation;
                ++recovered.report.applied_commands;
                recovered.report.last_valid_command_no = command_no;
                ++expected_no;
                continue;
            }
            ++recovered.report.skipped_commands;
        }
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
        return ok();
    });
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
        case SessionEventKind::saved: target->mark_saved(now_utc_ms()); return;
        }
        if (auto status = target->record(kind, event.applied, event.base_generation, event.result_generation,
                                         event.document);
            !status)
            target->set_last_error(std::move(status).error());
    });
}

} // namespace mwb::document
