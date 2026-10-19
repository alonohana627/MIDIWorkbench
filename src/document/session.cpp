#include "mwb/document/session.hpp"

#include "mwb/document/native_format.hpp"

#include <algorithm>

namespace mwb::document {

WorkingSession::WorkingSession(DocumentData document, SessionOrigin origin,
                               std::optional<std::filesystem::path> file_path, std::size_t history_budget_bytes)
    : document_(std::move(document)),
      history_(history_budget_bytes),
      generation_(document_.generation),
      file_path_(std::move(file_path)) {
    switch (origin) {
    case SessionOrigin::new_document:
    case SessionOrigin::opened_file: saved_state_ = history_.current_state(); break;
    case SessionOrigin::recovered: saved_state_ = std::nullopt; break;
    }
}

Result<Generation> WorkingSession::apply(ChangeSet changes) {
    if (changes.empty())
        return generation_;
    const Generation base = generation_;
    MWB_TRY(history_.execute(document_, changes));
    generation_ = base + 1;
    document_.generation = generation_;
    notify(SessionEventKind::edited, changes, base);
    return generation_;
}

Result<Generation> WorkingSession::undo() {
    const Generation base = generation_;
    auto applied = history_.undo(document_);
    if (!applied)
        return std::unexpected(std::move(applied).error());
    generation_ = base + 1;
    document_.generation = generation_;
    notify(SessionEventKind::undone, *applied, base);
    return generation_;
}

Result<Generation> WorkingSession::redo() {
    const Generation base = generation_;
    auto applied = history_.redo(document_);
    if (!applied)
        return std::unexpected(std::move(applied).error());
    generation_ = base + 1;
    document_.generation = generation_;
    notify(SessionEventKind::redone, *applied, base);
    return generation_;
}

SaveTicket WorkingSession::begin_save() const {
    return SaveTicket{generation_, history_.current_state(), std::make_shared<const DocumentData>(document_)};
}

Status WorkingSession::complete_save(const SaveTicket& ticket, const std::filesystem::path& path, Status outcome) {
    if (!outcome)
        return outcome;
    saved_state_ = ticket.state;
    file_path_ = path;
    const ChangeSet nothing;
    notify(SessionEventKind::saved, nothing, generation_);
    return ok();
}

Status WorkingSession::save(std::optional<std::filesystem::path> path) {
    const std::optional<std::filesystem::path> destination = path ? path : file_path_;
    if (!destination)
        return make_error(ErrorCode::invalid_argument, "the document has no file path yet");
    const SaveTicket ticket = begin_save();
    DocumentData stamped = *ticket.snapshot;
    stamped.modified_ms = now_utc_ms();
    return complete_save(ticket, *destination, save_native(stamped, *destination));
}

void WorkingSession::mark_saved(StateId state) {
    saved_state_ = state;
}

ListenerId WorkingSession::add_listener(SessionListener listener) {
    const ListenerId id = next_listener_++;
    listeners_.emplace_back(id, std::move(listener));
    return id;
}

void WorkingSession::remove_listener(ListenerId id) {
    std::erase_if(listeners_, [id](const auto& entry) { return entry.first == id; });
}

void WorkingSession::notify(SessionEventKind kind, const ChangeSet& applied, Generation base) {
    // Copy so a listener may add or remove listeners while being notified.
    const auto listeners = listeners_;
    const SessionEvent event{kind, applied, base, generation_, document_};
    for (const auto& [id, listener] : listeners)
        if (listener)
            listener(event);
}

} // namespace mwb::document
