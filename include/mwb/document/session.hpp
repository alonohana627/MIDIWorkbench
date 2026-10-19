#pragma once

#include "mwb/core/change_set.hpp"
#include "mwb/core/error.hpp"
#include "mwb/core/model.hpp"
#include "mwb/document/history.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

/// Working sessions: one open document with its history, generation counter, file path and
/// saved-state identity (sections 19.1 and 21).
namespace mwb::document {

/// What changed in a session notification.
enum class SessionEventKind : std::uint8_t {
    edited,  ///< A new command was applied.
    undone,  ///< An undo was applied (a new transition with a new generation).
    redone,  ///< A redo was applied (a new transition with a new generation).
    saved,   ///< A save completed; dirty state may have changed. Both generations are the saved one.
    /// The undo memory budget evicted older entries: the oldest undoable state is no longer the
    /// original one (section 21.1 history boundary). No change set; both generations are current.
    history_trimmed,
};

/// Notification delivered synchronously to listeners. References are valid only during the call;
/// listeners copy what they keep and never retain pointers into the session.
struct SessionEvent {
    SessionEventKind kind;
    const ChangeSet& applied;          ///< Change set applied to the document (empty for saved).
    Generation base_generation;        ///< Generation before the transition.
    Generation result_generation;      ///< Generation after the transition.
    const DocumentData& document;      ///< Document after the transition.
};

/// Listener callback; plain std::function, no Qt.
using SessionListener = std::function<void(const SessionEvent&)>;

/// Handle returned by add_listener() for removal.
using ListenerId = std::uint64_t;

/// Captured state of an explicit save (section 21.3): the generation G and state identity being
/// saved plus an immutable snapshot that a background task can serialize while editing goes on.
struct SaveTicket {
    Generation generation = 0;
    StateId state = 0;
    std::shared_ptr<const DocumentData> snapshot;
};

/// How a session starts relative to durable storage.
enum class SessionOrigin : std::uint8_t {
    new_document,   ///< Never saved; clean until edited, no file path.
    opened_file,    ///< Loaded from `file_path`; clean.
    recovered,      ///< Rebuilt from the recovery journal; unsaved and dirty, no file path.
};

/// Owns one DocumentData plus its CommandHistory, generation counter, file path and the state
/// identity of the last completed save.
class WorkingSession {
public:
    WorkingSession(DocumentData document, SessionOrigin origin,
                   std::optional<std::filesystem::path> file_path = std::nullopt,
                   std::size_t history_budget_bytes = default_history_budget_bytes);

    [[nodiscard]] const DocumentData& document() const { return document_; }
    [[nodiscard]] Generation generation() const { return generation_; }
    [[nodiscard]] StateId state() const { return history_.current_state(); }
    [[nodiscard]] const CommandHistory& history() const { return history_; }
    [[nodiscard]] CommandHistory& history() { return history_; }
    [[nodiscard]] const std::optional<std::filesystem::path>& file_path() const { return file_path_; }
    void set_file_path(std::optional<std::filesystem::path> path) { file_path_ = std::move(path); }

    /// Dirty when the current semantic state differs from the last completed save.
    [[nodiscard]] bool is_dirty() const { return !saved_state_ || *saved_state_ != history_.current_state(); }
    /// State identity of the last completed save, if any.
    [[nodiscard]] std::optional<StateId> saved_state() const { return saved_state_; }

    /// Applies one command (one undo entry). Returns the new generation; an empty change set
    /// leaves everything unchanged and returns the current generation. A change set that would
    /// grow the document beyond DocumentLimits::max_events_per_document is refused with
    /// ErrorCode::limit_exceeded (larger documents are inspection/export only, section 28.3).
    [[nodiscard]] Result<Generation> apply(ChangeSet changes);
    /// Undoes the newest entry; returns the new (increased) generation.
    [[nodiscard]] Result<Generation> undo();
    /// Redoes the next entry; returns the new (increased) generation.
    [[nodiscard]] Result<Generation> redo();

    /// Captures generation G, the state identity and a snapshot for an explicit save.
    [[nodiscard]] SaveTicket begin_save() const;
    /// Completes a save started with begin_save(). On success marks the ticket's state saved
    /// (newer edits stay dirty) and records `path`; on failure leaves history, dirty state and
    /// path unchanged and returns the failure.
    [[nodiscard]] Status complete_save(const SaveTicket& ticket, const std::filesystem::path& path, Status outcome);
    /// Synchronous save to `path` (or the current file path) through save_native().
    [[nodiscard]] Status save(std::optional<std::filesystem::path> path = std::nullopt);

    /// Marks a state identity as the saved state (for example after an external save of a
    /// snapshot of that state).
    void mark_saved(StateId state);

    /// Registers a listener; returns its handle.
    ListenerId add_listener(SessionListener listener);
    /// Removes a listener; unknown handles are ignored.
    void remove_listener(ListenerId id);

private:
    void notify(SessionEventKind kind, const ChangeSet& applied, Generation base, Generation result);

    DocumentData document_;
    CommandHistory history_;
    Generation generation_;
    std::optional<std::filesystem::path> file_path_;
    std::optional<StateId> saved_state_;
    std::vector<std::pair<ListenerId, SessionListener>> listeners_;
    ListenerId next_listener_ = 1;
};

} // namespace mwb::document
