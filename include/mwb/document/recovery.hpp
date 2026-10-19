#pragma once

#include "mwb/core/change_set.hpp"
#include "mwb/core/error.hpp"
#include "mwb/core/hashing.hpp"
#include "mwb/core/model.hpp"
#include "mwb/document/command_codec.hpp"
#include "mwb/document/file_system.hpp"
#include "mwb/document/session.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace SQLite {
class Database;
}

/// Crash recovery (specification sections 21.2 and 23.3): a per-user recovery database holding,
/// per working session, native checkpoints, an append-only journal of CBOR command envelopes and
/// the raw capture chunks of recordings still in progress.
/// The application passes %LOCALAPPDATA%/MIDIWorkbench/recovery as the directory.
namespace mwb::document {

/// SQLite application_id of the recovery database.
inline constexpr std::int32_t recovery_application_id = 1297564242;
/// Current recovery schema version (2 adds in-progress capture chunks).
inline constexpr std::int32_t recovery_schema_version = 2;
/// File name of the recovery database inside the recovery directory.
inline constexpr const char* recovery_database_name = "recovery.sqlite";
/// Database used instead when recovery.sqlite was written by a newer version of the application
/// (after a downgrade): this version then keeps its own sessions apart and never loses recovery.
inline constexpr const char* recovery_fallback_database_name = "recovery-v2.sqlite";

/// When to write a fresh checkpoint: whichever threshold is reached first (section 21.2).
struct RecoveryPolicy {
    std::size_t checkpoint_command_count = 100;
    std::size_t checkpoint_journal_bytes = 10u * 1024u * 1024u;
    std::chrono::milliseconds checkpoint_interval{60'000};
    /// How long a flush waits for another connection (another document or instance) that is
    /// writing the shared recovery database.
    std::chrono::milliseconds busy_timeout{5'000};
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

/// A session that was not closed cleanly and can be recovered. Sessions without any committed
/// checkpoint hold nothing to recover and are never listed.
struct RecoverableSession {
    Uuid session_id;
    DocumentId document_id;
    SourceReference source;
    std::int64_t created_ms = 0;
    std::int64_t last_durable_edit_ms = 0;  ///< Time of the newest journaled command or checkpoint.
    std::size_t journaled_commands = 0;     ///< Commands after the newest checkpoint.
    /// Number of the newest journaled transition (checkpoint or command); 0 when the session
    /// was never edited.
    std::int64_t last_command_no = 0;
    /// Generation of the newest journaled state.
    Generation last_generation = 0;
    /// Generation of the last explicit save, when one was recorded.
    std::optional<Generation> saved_generation;
    /// The session's starting content is a saved file or a fresh template (nothing to recover
    /// until it is edited); false for recovered and other unsaved starting content.
    bool base_saved = false;
    /// Raw capture chunks of a recording that was in progress.
    std::size_t capture_chunks = 0;
    /// The session belongs to a running process (its lock is held): it is not crashed work and
    /// must not be recovered or discarded by another instance.
    bool live = false;

    /// True when recovering would give the user something the saved file does not have: edits
    /// after the last save, an interrupted recording, or unsaved starting content.
    [[nodiscard]] bool has_unsaved_work() const {
        if (capture_chunks > 0)
            return true;
        if (last_command_no == 0)
            return !base_saved;
        return !saved_generation || *saved_generation != last_generation;
    }
};

/// What recovery did.
struct RecoveryReport {
    std::int64_t checkpoint_command_no = 0;
    Generation checkpoint_generation = 0;
    std::size_t applied_commands = 0;
    std::size_t skipped_commands = 0;         ///< Entries after the last valid one (kept as evidence).
    std::size_t rejected_checkpoints = 0;     ///< Newer checkpoints that failed their checks.
    std::int64_t last_valid_command_no = 0;
    /// Commands whose base semantic hash was compared with the replayed state. Replay samples the
    /// first, every 16th and the last command; any mismatch re-runs replay checking every command.
    std::size_t verified_hashes = 0;
    /// Interrupted recordings rebuilt from capture chunks (added as interrupted takes).
    std::size_t recovered_takes = 0;
    std::optional<std::string> stop_reason;   ///< Why replay stopped early, when it did.
};

/// A recovered document: a new, unsaved document to open as a separate session.
struct RecoveredDocument {
    DocumentData document;
    SourceReference source;
    RecoveryReport report;
};

/// One bounded chunk of the raw capture of a recording that is still in progress (section 21.2).
/// The recorder journals one chunk every take_chunk_messages messages, so a crash during a long
/// take keeps all complete chunks. Recovery adds them to the recovered document as an
/// interrupted take.
struct CaptureChunk {
    TakeId take;                       ///< Identity the finished take will carry.
    TrackId track;                     ///< Track being recorded.
    std::int32_t ordinal = 0;          ///< 0, 1, 2, ... within the take.
    Tick start_tick = 0;               ///< Recording start, for the recovered take's range.
    std::int64_t first_time_ns = 0;    ///< Capture time of the chunk's first message.
    std::vector<std::uint8_t> payload; ///< Take chunk codec 1 bytes, at most 1 MiB.
};

/// Durable journal of one working session. record() buffers commands (they are encoded at flush
/// time, off the recording thread) and prepares a checkpoint when a threshold is reached;
/// flush() commits them; compaction runs only after a new checkpoint has committed and keeps the
/// previous checkpoint as a fallback. Thread-safe: the document's thread records while flush()
/// runs on a background thread (it takes the buffered work and writes it without blocking
/// record).
///
/// The session's first checkpoint is serialized on a background thread (create() returns
/// quickly). Until a checkpoint has committed, flush() keeps commands and capture chunks buffered
/// instead of writing entries that would have no base, so recovery never sees half a session;
/// flush(true) waits for the checkpoint and then commits everything.
class RecoveryJournal {
public:
    /// Opens (creating or migrating if needed) the recovery database in `directory`, takes the
    /// session lock and writes the session row. The first checkpoint of `document` is prepared on
    /// the checkpoint thread from an immutable copy and committed by the next flush.
    [[nodiscard]] static Result<std::unique_ptr<RecoveryJournal>> create(const std::filesystem::path& directory,
                                                                         const DocumentData& document,
                                                                         SourceReference source,
                                                                         RecoveryPolicy policy = {},
                                                                         RecoveryClock clock = {});
    ~RecoveryJournal();
    RecoveryJournal(const RecoveryJournal&) = delete;
    RecoveryJournal& operator=(const RecoveryJournal&) = delete;

    /// Journals one transition. `base_generation` must equal the previous result generation.
    /// When a checkpoint threshold is reached a snapshot of `result_document` is prepared for a
    /// later flush. A journaled TakeInsert ends the capture of that take (its chunks are deleted
    /// when the command commits).
    [[nodiscard]] Status record(CommandKind kind, const ChangeSet& applied, Generation base_generation,
                                Generation result_generation, const DocumentData& result_document);

    /// Buffers one capture chunk of an in-progress recording; written at the next flush.
    /// Thread-safe; cheap (the payload is moved). Invalid ranges return invalid_argument.
    [[nodiscard]] Status record_capture_chunk(CaptureChunk chunk);
    /// Ends the capture of `take` without journaling it (recording cancelled or discarded): its
    /// chunks are deleted at the next flush.
    void discard_capture(const TakeId& take);

    /// Commits buffered commands, capture chunks, a prepared checkpoint and source updates in one
    /// transaction; then compacts entries the previous checkpoint covers. On failure the buffer
    /// is kept. `wait_for_checkpoint` false (the GUI's periodic flush) commits a checkpoint only
    /// when its background serialization has finished; true waits for it (tests, close, explicit
    /// flush, and before discarding the evidence a recovered document came from).
    [[nodiscard]] Status flush(bool wait_for_checkpoint = true);

    /// Records an explicit save time; written at the next flush.
    void mark_saved(std::int64_t saved_ms);
    /// Records an explicit save of `saved_generation` (the session's saved event); the listing
    /// then knows whether later edits exist.
    void mark_saved(std::int64_t saved_ms, Generation saved_generation);
    /// Replaces the source reference (for example after Save As); written at the next flush.
    void update_source(SourceReference source);

    /// Marks the session cleanly closed: deletes the session with its checkpoints, journal and
    /// capture chunks. Later flushes do nothing.
    [[nodiscard]] Status mark_cleanly_closed();
    /// A journal for a document opened from a file (or a fresh template) that is destroyed before
    /// any edit was recorded closes cleanly (its content is the file itself), so it is never
    /// offered for recovery; the flag is also stored as RecoverableSession::base_saved.
    void set_discard_if_unused(bool discard);

    [[nodiscard]] const Uuid& session_id() const { return session_id_; }
    /// Time of the last committed write, for "last durable recovery" display.
    [[nodiscard]] std::int64_t last_durable_ms() const;
    [[nodiscard]] std::size_t pending_commands() const;
    [[nodiscard]] std::size_t pending_capture_chunks() const;
    [[nodiscard]] std::int64_t last_command_no() const;
    /// True once a checkpoint of this session has committed; before that nothing is recoverable
    /// and commands stay buffered.
    [[nodiscard]] bool checkpoint_committed() const;
    /// Last error raised while recording (also from a connected session listener) or while
    /// preparing a checkpoint, if any.
    [[nodiscard]] std::optional<Error> last_error() const;
    /// Stores an error raised while recording from a listener.
    void set_last_error(Error error);

private:
    /// One command waiting for flush; encoded there (off the recording thread).
    struct PendingCommand {
        std::int64_t command_no = 0;
        Generation base_generation = 0;
        Generation result_generation = 0;
        Sha256Digest checksum{};
        std::vector<std::uint8_t> payload;
        std::int64_t created_ms = 0;
        CommandEnvelope envelope;          ///< Until encoded; its base hash comes from base_hash.
        std::shared_future<Sha256Digest> base_hash;
        bool encoded = false;
    };
    /// A snapshot waiting for flush; shared, never copied (tens of megabytes for large documents).
    struct PendingCheckpoint {
        std::int64_t command_no = 0;
        Generation generation = 0;
        Sha256Digest checksum{};
        std::vector<std::uint8_t> snapshot;
        std::int64_t created_ms = 0;
    };
    using CheckpointJob = std::shared_future<Result<std::shared_ptr<const PendingCheckpoint>>>;
    /// A capture chunk waiting for flush.
    struct PendingChunk {
        CaptureChunk chunk;
        Sha256Digest checksum{};
        std::int64_t created_ms = 0;
    };

    RecoveryJournal(std::unique_ptr<SQLite::Database> database, Uuid session_id, SourceReference source,
                    RecoveryPolicy policy, RecoveryClock clock);
    /// Starts serializing a checkpoint of `snapshot` on the checkpoint thread.
    [[nodiscard]] CheckpointJob submit_checkpoint(std::shared_ptr<const DocumentData> snapshot, std::int64_t command_no,
                                                  Generation generation, std::int64_t now) const;
    /// Source reference JSON with the journal's save and base markers. Needs state_mutex_.
    [[nodiscard]] std::string encoded_source() const;

    /// Guards the buffered state below; never held during SQLite work.
    mutable std::mutex state_mutex_;
    /// Serializes database transactions (flush, close).
    std::mutex database_mutex_;
    /// Declared before the database so the connection closes before the lock is released.
    file_system::ExclusiveFileLock lock_;
    std::unique_ptr<SQLite::Database> database_;
    Uuid session_id_;
    SourceReference source_;
    bool source_changed_ = false;
    std::optional<Generation> saved_generation_;
    RecoveryPolicy policy_;
    RecoveryClock clock_;
    std::vector<PendingCommand> pending_;
    std::vector<PendingChunk> pending_chunks_;
    /// Takes whose capture ended; their chunks are deleted by the next flush.
    std::vector<TakeId> finished_captures_;
    std::shared_ptr<const PendingCheckpoint> pending_checkpoint_;
    std::int64_t last_command_no_ = 0;
    Generation last_generation_ = 0;
    /// Semantic hash of the last recorded document, computed on a background thread from an
    /// immutable copy; the next flush waits for it (edits are rarely closer than the hash).
    std::shared_future<Sha256Digest> last_hash_;
    bool discard_if_unused_ = false;
    /// Checkpoint snapshot being serialized on a background thread.
    CheckpointJob checkpoint_job_;
    /// A checkpoint has committed: commands written from now on have a base.
    bool checkpoint_committed_ = false;
    /// The next record() prepares a checkpoint (after a failed checkpoint or an unencodable
    /// command, so a newer base repairs the gap).
    bool force_checkpoint_ = false;
    bool closed_ = false;
    std::size_t commands_since_checkpoint_ = 0;
    std::size_t bytes_since_checkpoint_ = 0;
    std::int64_t last_checkpoint_ms_ = 0;
    std::int64_t last_durable_ms_ = 0;
    std::optional<Error> last_error_;
    /// Number of the newest committed checkpoint; only touched under database_mutex_.
    std::optional<std::int64_t> committed_checkpoint_no_;
    /// Set when the journal is destroyed: queued checkpoint work for it is skipped.
    std::shared_ptr<std::atomic<bool>> cancelled_ = std::make_shared<std::atomic<bool>>(false);
};

/// Sessions in `directory` that were not closed cleanly and hold a checkpoint, newest durable
/// edit first. A journal that is still open in this process is listed too; callers exclude their
/// own session ids. Dead sessions without any checkpoint (a crash before the first checkpoint
/// committed) hold nothing and are deleted.
[[nodiscard]] Result<std::vector<RecoverableSession>> list_recoverable_sessions(const std::filesystem::path& directory);

/// Rebuilds a session's document: newest valid checkpoint (falling back to the previous one),
/// then journaled commands replayed in order through the last valid entry (contiguous number,
/// codec version, checksum, decodable envelope, matching generations and base semantic hash,
/// successful application), then interrupted recordings from capture chunks. Nothing is deleted;
/// remaining entries stay as evidence.
[[nodiscard]] Result<RecoveredDocument> recover_session(const std::filesystem::path& directory,
                                                        const Uuid& session_id);

/// Deletes a session and all its data (after the user recovered or declined it).
[[nodiscard]] Status discard_recovery_session(const std::filesystem::path& directory, const Uuid& session_id);

/// Returns, once, a message when this process found the recovery database damaged or foreign
/// and set it aside (renamed to recovery.corrupt-<time>.sqlite) to start a fresh one.
[[nodiscard]] std::optional<std::string> take_recovery_database_notice();

/// Stops the background hash and checkpoint threads: queued work is dropped (its futures report
/// a broken promise) and the threads are joined. Call once at exit after the last flush or close
/// (after the application's recovery thread has drained), so no checkpoint serialization delays
/// the exit or runs during static destruction.
void shutdown_recovery_executors();

/// Connects a working session to a journal: every edit, undo and redo is recorded as a new
/// transition. The listener holds only a weak reference; recording errors (also allocation
/// failures) are stored in RecoveryJournal::last_error() and never thrown into the session.
/// A completed save is recorded with mark_saved().
ListenerId connect_recovery(WorkingSession& session, const std::shared_ptr<RecoveryJournal>& journal);

} // namespace mwb::document
