#pragma once

#include "mwb/core/change_set.hpp"
#include "mwb/core/error.hpp"
#include "mwb/core/model.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

/// Per-document command history (specification section 21.1).
namespace mwb::document {

/// Identity of one semantic document state. Every new command creates a fresh identity; undo and
/// redo return to the identity the state had before, so "undo back to the saved state" compares
/// equal to the saved identity and the document is clean again.
using StateId = std::uint64_t;

/// Default undo memory budget per document (section 28.3).
inline constexpr std::size_t default_history_budget_bytes = 256u * 1024u * 1024u;

/// Undo/redo over core ChangeSets. One entry per command; new edits discard the redo branch;
/// whole oldest entries are evicted when the memory budget is exceeded and a visible history
/// boundary is reported.
class CommandHistory {
public:
    explicit CommandHistory(std::size_t budget_bytes = default_history_budget_bytes);

    /// Applies `changes` to `document` and records one undo entry. An empty change set is a
    /// no-op that records nothing. On failure the document is unchanged (core apply_change_set
    /// rolls back) and the history is untouched.
    [[nodiscard]] Status execute(DocumentData& document, ChangeSet changes);

    /// Reverts the newest undoable entry; returns the change set that was applied (the inverse).
    [[nodiscard]] Result<ChangeSet> undo(DocumentData& document);

    /// Re-applies the oldest redoable entry; returns the change set that was applied.
    [[nodiscard]] Result<ChangeSet> redo(DocumentData& document);

    [[nodiscard]] bool can_undo() const { return cursor_ > 0; }
    [[nodiscard]] bool can_redo() const { return cursor_ < entries_.size(); }
    /// Label of the entry undo() would revert.
    [[nodiscard]] std::optional<std::string> undo_label() const;
    /// Label of the entry redo() would apply.
    [[nodiscard]] std::optional<std::string> redo_label() const;
    [[nodiscard]] std::size_t undo_depth() const { return cursor_; }
    [[nodiscard]] std::size_t redo_depth() const { return entries_.size() - cursor_; }

    /// Semantic identity of the current document state.
    [[nodiscard]] StateId current_state() const { return current_state_; }

    /// True once older entries were evicted: the oldest undoable state is not the original one.
    [[nodiscard]] bool history_boundary() const { return evicted_entries_ > 0; }
    /// Number of whole entries evicted so far.
    [[nodiscard]] std::size_t evicted_entries() const { return evicted_entries_; }

    /// Memory held by the recorded change sets (core ChangeSet::memory_estimate).
    [[nodiscard]] std::size_t memory_bytes() const { return memory_bytes_; }
    [[nodiscard]] std::size_t budget_bytes() const { return budget_bytes_; }
    /// Changes the budget; evicts immediately when the new budget is smaller.
    void set_budget_bytes(std::size_t budget_bytes);

    /// Forgets every entry (for example after reloading from disk). The current state keeps its
    /// identity.
    void clear();

private:
    /// One undoable command: the forward change set and the states it connects.
    struct Entry {
        ChangeSet forward;
        StateId before = 0;
        StateId after = 0;
        std::size_t memory = 0;
    };

    void evict_to_budget();

    std::deque<Entry> entries_;
    std::size_t cursor_ = 0; ///< entries_[0, cursor_) are undoable, the rest redoable.
    std::size_t budget_bytes_;
    std::size_t memory_bytes_ = 0;
    std::size_t evicted_entries_ = 0;
    StateId current_state_ = 1;
    StateId next_state_ = 2;
};

} // namespace mwb::document
