#pragma once

#include "mwb/core/change_set.hpp"
#include "mwb/core/error.hpp"
#include "mwb/core/hashing.hpp"
#include "mwb/core/model.hpp"
#include "mwb/document/command_codec.hpp"
#include "mwb/document/file_system.hpp"
#include "mwb/document/session.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <mutex>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace SQLite {
class Database;
}

/// Crash recovery (specification sections 21.2 and 23.3): a per-user recovery database holding,
/// per working session, native checkpoints and an append-only journal of CBOR command envelopes.
/// The application passes %LOCALAPPDATA%/MIDIWorkbench/recovery as the directory.
namespace mwb::document {

/// SQLite application_id of the recovery database.
inline constexpr std::int32_t recovery_application_id = 1297564242;
/// Current recovery schema version.
inline constexpr std::int32_t recovery_schema_version = 1;
/// File name of the recovery database inside the recovery directory.
inline constexpr const char* recovery_database_name = "recovery.sqlite";

/// When to write a fresh checkpoint: whichever threshold is reached first (section 21.2).
struct RecoveryPolicy {
    std::size_t checkpoint_command_count = 100;
    std::size_t checkpoint_journal_bytes = 10u * 1024u * 1024u;
    std::chrono::milliseconds checkpoint_interval{60'000};
};

/// Wall clock in UTC milliseconds; injectable for tests.
using RecoveryClock = std::function<std::int64_t()>;

/// Where the recovered document came from; stored as recovery_session.source_reference_json.
struct SourceReference {
    std::string document_name;                     ///< User-visible title.
    std::optional<std::string> file_path_utf8;     ///< Native file the session edits, if any.
    std::optional<std::string> source_identity;    ///< Library asset/revision or import source, if any.
    std::optional<std::int64_t> last_explicit_save_ms;
    friend bool operator==(const SourceReference&, const SourceReference&) = default;
};

/// A session that was not closed cleanly and can be recovered.
struct RecoverableSession {
    Uuid session_id;
    DocumentId document_id;
    SourceReference source;
    std::int64_t created_ms = 0;
    std::int64_t last_durable_edit_ms = 0;  ///< Time of the last committed journal write.
    std::size_t journaled_commands = 0;     ///< Commands after the newest checkpoint.
    /// The session belongs to a running process (its lock is held): it is not crashed work and
    /// must not be recovered or discarded by another instance.
    bool live = false;
};

/// What recovery did.
struct RecoveryReport {
    std::int64_t checkpoint_command_no = 0;
    Generation checkpoint_generation = 0;
    std::size_t applied_commands = 0;
    std::size_t skipped_commands = 0;         ///< Entries after the last valid one (kept as evidence).
    std::size_t rejected_checkpoints = 0;     ///< Newer checkpoints that failed their checks.
    std::int64_t last_valid_command_no = 0;
    std::optional<std::string> stop_reason;   ///< Why replay stopped early, when it did.
};

/// A recovered document: a new, unsaved document to open as a separate session.
struct RecoveredDocument {
    DocumentData document;
    SourceReference source;
    RecoveryReport report;
};

/// Durable journal of one working session. record() buffers encoded commands (and prepares a
/// checkpoint when a threshold is reached); flush() commits them; compaction runs only after a
/// new checkpoint has committed. Thread-safe: the document's thread records while flush() runs on
/// a background thread (it takes the buffered commands and writes them without blocking record).
class RecoveryJournal {
public:
    /// Opens (creating if needed) the recovery database in `directory` and starts a session with
    /// a durable checkpoint of `document`.
    [[nodiscard]] static Result<std::unique_ptr<RecoveryJournal>> create(const std::filesystem::path& directory,
                                                                         const DocumentData& document,
                                                                         SourceReference source,
                                                                         RecoveryPolicy policy = {},
                                                                         RecoveryClock clock = {});
    ~RecoveryJournal();
    RecoveryJournal(const RecoveryJournal&) = delete;
    RecoveryJournal& operator=(const RecoveryJournal&) = delete;

    /// Journals one transition. `base_generation` must equal the previous result generation.
    /// When a checkpoint threshold is reached a snapshot of `result_document` is prepared for the
    /// next flush; a snapshot failure is returned but the command itself stays journaled.
    [[nodiscard]] Status record(CommandKind kind, const ChangeSet& applied, Generation base_generation,
                                Generation result_generation, const DocumentData& result_document);

    /// Commits buffered commands, a prepared checkpoint and source updates in one transaction;
    /// then compacts entries the new checkpoint covers. On failure the buffer is kept.
    /// `wait_for_checkpoint` false (the GUI's periodic flush) commits a checkpoint only when its
    /// background serialization has finished; true waits for it (tests, close, explicit flush).
    [[nodiscard]] Status flush(bool wait_for_checkpoint = true);

    /// Records an explicit save time; written at the next flush.
    void mark_saved(std::int64_t saved_ms);
    /// Replaces the source reference (for example after Save As); written at the next flush.
    void update_source(SourceReference source);

    /// Marks the session cleanly closed and removes its checkpoints and journal.
    [[nodiscard]] Status mark_cleanly_closed();

    [[nodiscard]] const Uuid& session_id() const { return session_id_; }
    /// Time of the last committed write, for "last durable recovery" display.
    [[nodiscard]] std::int64_t last_durable_ms() const;
    [[nodiscard]] std::size_t pending_commands() const;
    [[nodiscard]] std::int64_t last_command_no() const;
    /// Last error raised by a connected session listener (connect_recovery), if any.
    [[nodiscard]] std::optional<Error> last_error() const;
    /// Stores an error raised while recording from a listener.
    void set_last_error(Error error);

private:
    /// One encoded command waiting for flush.
    struct PendingCommand {
        std::int64_t command_no = 0;
        Generation base_generation = 0;
        Generation result_generation = 0;
        Sha256Digest checksum{};
        std::vector<std::uint8_t> payload;
        std::int64_t created_ms = 0;
    };
    /// A snapshot waiting for flush.
    struct PendingCheckpoint {
        std::int64_t command_no = 0;
        Generation generation = 0;
        Sha256Digest checksum{};
        std::vector<std::uint8_t> snapshot;
        std::int64_t created_ms = 0;
    };

    RecoveryJournal(std::unique_ptr<SQLite::Database> database, Uuid session_id, SourceReference source,
                    RecoveryPolicy policy, RecoveryClock clock);

    /// Guards the buffered state below; never held during SQLite work.
    mutable std::mutex state_mutex_;
    /// Serializes database transactions (flush, close).
    std::mutex database_mutex_;
    std::unique_ptr<SQLite::Database> database_;
    file_system::ExclusiveFileLock lock_;
    Uuid session_id_;
    SourceReference source_;
    bool source_changed_ = false;
    RecoveryPolicy policy_;
    RecoveryClock clock_;
    std::vector<PendingCommand> pending_;
    std::optional<PendingCheckpoint> pending_checkpoint_;
    std::int64_t last_command_no_ = 0;
    Generation last_generation_ = 0;
    /// Semantic hash of the last recorded document, computed on a background thread from an
    /// immutable copy; the next record() waits for it (edits are rarely closer than the hash).
    std::shared_future<Sha256Digest> last_hash_;
    /// Checkpoint snapshot being serialized on a background thread.
    std::future<Result<PendingCheckpoint>> checkpoint_job_;
    std::size_t commands_since_checkpoint_ = 0;
    std::size_t bytes_since_checkpoint_ = 0;
    std::int64_t last_checkpoint_ms_ = 0;
    std::int64_t last_durable_ms_ = 0;
    std::optional<Error> last_error_;
};

/// Sessions in `directory` that were not closed cleanly, newest durable edit first. A journal
/// that is still open in this process is listed too; callers exclude their own session ids.
[[nodiscard]] Result<std::vector<RecoverableSession>> list_recoverable_sessions(const std::filesystem::path& directory);

/// Rebuilds a session's document: newest valid checkpoint, then journaled commands replayed in
/// order through the last valid entry (contiguous number, codec version, checksum, decodable
/// envelope, matching generations and base semantic hash, successful application). Nothing is
/// deleted; remaining entries stay as evidence.
[[nodiscard]] Result<RecoveredDocument> recover_session(const std::filesystem::path& directory,
                                                        const Uuid& session_id);

/// Deletes a session and all its data (after the user recovered or declined it).
[[nodiscard]] Status discard_recovery_session(const std::filesystem::path& directory, const Uuid& session_id);

/// Connects a working session to a journal: every edit, undo and redo is recorded as a new
/// transition. The listener holds only a weak reference; recording errors are stored in
/// RecoveryJournal::last_error(). A completed save is recorded with mark_saved().
ListenerId connect_recovery(WorkingSession& session, const std::shared_ptr<RecoveryJournal>& journal);

} // namespace mwb::document
