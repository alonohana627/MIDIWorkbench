#include "mwb/document/history.hpp"

#include <iterator>
#include <utility>

namespace mwb::document {

CommandHistory::CommandHistory(std::size_t budget_bytes) : budget_bytes_(budget_bytes) {}

Status CommandHistory::execute(DocumentData& document, ChangeSet changes) {
    if (changes.empty())
        return ok();
    MWB_TRY(apply_change_set(document, changes));

    // A new edit after undo discards the redo branch; its states can never be reached again.
    while (entries_.size() > cursor_) {
        memory_bytes_ -= entries_.back().memory;
        entries_.pop_back();
    }
    Entry entry;
    entry.memory = changes.memory_estimate();
    entry.forward = std::move(changes);
    entry.before = current_state_;
    entry.after = next_state_++;
    current_state_ = entry.after;
    memory_bytes_ += entry.memory;
    entries_.push_back(std::move(entry));
    cursor_ = entries_.size();
    evict_to_budget();
    return ok();
}

Result<ChangeSet> CommandHistory::undo(DocumentData& document) {
    if (!can_undo())
        return make_error(ErrorCode::invalid_argument, "nothing to undo");
    Entry& entry = entries_[cursor_ - 1];
    ChangeSet inverse = invert(entry.forward);
    MWB_TRY(apply_change_set(document, inverse));
    --cursor_;
    current_state_ = entry.before;
    return inverse;
}

Result<ChangeSet> CommandHistory::redo(DocumentData& document) {
    if (!can_redo())
        return make_error(ErrorCode::invalid_argument, "nothing to redo");
    Entry& entry = entries_[cursor_];
    MWB_TRY(apply_change_set(document, entry.forward));
    ++cursor_;
    current_state_ = entry.after;
    return entry.forward;
}

std::optional<std::string> CommandHistory::undo_label() const {
    if (!can_undo())
        return std::nullopt;
    return entries_[cursor_ - 1].forward.label;
}

std::optional<std::string> CommandHistory::redo_label() const {
    if (!can_redo())
        return std::nullopt;
    return entries_[cursor_].forward.label;
}

void CommandHistory::set_budget_bytes(std::size_t budget_bytes) {
    budget_bytes_ = budget_bytes;
    evict_to_budget();
}

void CommandHistory::clear() {
    entries_.clear();
    cursor_ = 0;
    memory_bytes_ = 0;
}

void CommandHistory::evict_to_budget() {
    // Redo entries are only dropped by a new edit; eviction removes whole oldest undo entries.
    while (memory_bytes_ > budget_bytes_ && cursor_ > 0) {
        memory_bytes_ -= entries_.front().memory;
        entries_.pop_front();
        --cursor_;
        ++evicted_entries_;
    }
}

} // namespace mwb::document
