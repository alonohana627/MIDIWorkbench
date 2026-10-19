#pragma once

#include "mwb/core/change_set.hpp"
#include "mwb/core/model.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/core/tempo_map.hpp"
#include "mwb/document/session.hpp"

#include <QObject>
#include <QString>

#include <filesystem>
#include <thread>
#include <mutex>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace mwb::document {
class WorkingSession;
class RecoveryJournal;
}

namespace mwb::ui {

/// Waits until queued recovery work (flushes, clean closes) has run; called when the main window
/// closes. The thread stays available (it is shared by every document).
void drain_recovery_thread();

/// GUI-side owner of one open document: the working session (model, history, saves) plus view
/// state that is not document content (active sequence and track, selection, edit cursor).
/// Edits are applied on the GUI thread immediately (optimistic, section 19.4); derived caches
/// (note pairing, interval index, tempo and meter maps) are rebuilt per generation.
class DocumentController : public QObject {
    Q_OBJECT

public:
    explicit DocumentController(std::unique_ptr<document::WorkingSession> session, QObject* parent = nullptr);
    ~DocumentController() override;

    [[nodiscard]] const DocumentData& document() const;
    /// An immutable copy of the current document, made once per generation and shared by the
    /// background users (playback compile, transform preview, library save): repeated requests
    /// within a generation do not copy the document again.
    [[nodiscard]] std::shared_ptr<const DocumentData> shared_snapshot() const;
    [[nodiscard]] document::WorkingSession& session() { return *session_; }
    [[nodiscard]] std::uint64_t generation() const { return generation_; }

    /// Applies one command; returns false and sets `error` when it was rejected (the document
    /// is then unchanged).
    bool apply(const ChangeSet& changes, QString* error = nullptr);
    /// Sets the song tempo heard at `position` of the active sequence as one undo entry ("Change
    /// tempo"). Without tempo changes after tick 0 the tempo at bar 1 is replaced (or inserted on
    /// the first track, as the Inspector's Song section does); with tempo changes, the tempo
    /// event in effect at `position` is replaced, so the rest of the tempo map is kept. `bpm` is
    /// clamped to 5..999. False when the value is unchanged, the sequence is not PPQ, or refused.
    bool set_tempo_at(Tick position, double bpm);
    bool undo();
    bool redo();
    [[nodiscard]] bool can_undo() const;
    [[nodiscard]] bool can_redo() const;
    [[nodiscard]] QString undo_text() const;
    [[nodiscard]] QString redo_text() const;
    [[nodiscard]] bool is_dirty() const;

    /// File currently associated with the document (empty for new or imported documents).
    [[nodiscard]] const std::filesystem::path& file_path() const { return file_path_; }
    void set_file_path(std::filesystem::path path);
    /// Saves to the native format; on success the current state becomes the saved state.
    bool save_native(const std::filesystem::path& path, QString* error);
    /// Saves on a background thread (section 21.3): captures generation G now, editing continues,
    /// and save_finished() reports durable completion. Edits made meanwhile stay dirty.
    void save_native_async(const std::filesystem::path& path);
    [[nodiscard]] bool saving() const { return saving_; }
    /// Blocks until a background save finishes and applies its result (used before closing).
    void wait_for_save();

    /// Journals every transition to the recovery database (section 21.2).
    void attach_recovery(std::shared_ptr<document::RecoveryJournal> journal);
    [[nodiscard]] document::RecoveryJournal* recovery_journal() const { return journal_.get(); }
    /// Commits buffered journal entries; returns false and the reason when storage failed.
    bool flush_recovery(QString* error = nullptr);
    /// Flushes the journal on the recovery thread (the GUI never waits for SQLite); the result
    /// arrives as recovery_flushed(). At most one flush runs per document.
    void flush_recovery_async();
    /// Error text of the last background flush (empty when it succeeded).
    [[nodiscard]] const QString& recovery_error() const { return recovery_error_; }
    /// Marks the session cleanly closed (its recovery data is removed).
    void close_recovery();

    // --- View state (never part of the document) -----------------------------------------------
    [[nodiscard]] std::size_t active_sequence_index() const { return active_sequence_; }
    [[nodiscard]] const Sequence& active_sequence() const;
    void set_active_sequence(std::size_t index);
    [[nodiscard]] TrackId active_track() const { return active_track_; }
    void set_active_track(TrackId track);
    [[nodiscard]] const std::set<EventId>& selection() const { return selection_; }
    void set_selection(std::set<EventId> notes);
    [[nodiscard]] Tick edit_cursor() const { return edit_cursor_; }
    void set_edit_cursor(Tick tick);
    /// Tracks other than the active one that are shown as read-only ghosts.
    [[nodiscard]] bool show_ghost_tracks() const { return show_ghost_tracks_; }
    void set_show_ghost_tracks(bool show);

    // --- Derived caches for the active sequence --------------------------------------------------
    [[nodiscard]] const NotePairing& notes() const;
    [[nodiscard]] const NoteIndex& note_index() const;
    [[nodiscard]] const TempoMap& tempo_map() const;
    [[nodiscard]] const MeterMap& meter_map() const;
    /// Spans of the selected notes.
    [[nodiscard]] std::vector<NoteSpan> selected_notes() const;
    [[nodiscard]] std::optional<std::size_t> active_track_index() const;

signals:
    /// Emitted after any change of document content.
    void document_changed();
    void selection_changed();
    void view_state_changed();
    /// Only the edit cursor moved (fires on every scrub step; listeners must be cheap).
    void edit_cursor_changed();
    void dirty_changed(bool dirty);
    /// A command was rejected; the message names the operation and the reason.
    void edit_rejected(const QString& message);
    /// The undo memory budget dropped the oldest undo steps (section 21.1 history boundary).
    void history_trimmed();
    /// A background save finished; `error` is empty on durable success.
    void save_finished(const QString& error);
    void recovery_flushed(const QString& error);

private:
    void rebuild_caches() const;
    void after_change();

    std::unique_ptr<document::WorkingSession> session_;
    std::shared_ptr<document::RecoveryJournal> journal_;
    std::filesystem::path file_path_;
    std::uint64_t generation_ = 0;
    std::size_t active_sequence_ = 0;
    TrackId active_track_;
    mutable std::shared_ptr<const DocumentData> snapshot_;   ///< shared_snapshot() of...
    mutable std::uint64_t snapshot_generation_ = 0;          ///< ...this generation.
    std::set<EventId> selection_;
    Tick edit_cursor_ = 0;
    bool show_ghost_tracks_ = true;
    bool last_dirty_ = false;
    bool saving_ = false;
    bool recovery_flush_running_ = false;
    QString recovery_error_;
    struct PendingSave {
        document::SaveTicket ticket;
        std::filesystem::path path;
        std::mutex mutex;
        std::optional<Status> outcome;
    };
    void finish_save(std::shared_ptr<PendingSave> pending);
    std::shared_ptr<PendingSave> pending_save_;

    mutable std::uint64_t cache_generation_ = UINT64_MAX;
    mutable std::size_t cache_sequence_ = SIZE_MAX;
    mutable NotePairing pairing_;
    mutable NoteIndex index_;
    mutable std::optional<TempoMap> tempo_;
    mutable std::optional<MeterMap> meter_;

    std::jthread save_thread_; // declared last: joined before the members it uses are destroyed
};

} // namespace mwb::ui
