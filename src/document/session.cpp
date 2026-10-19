#include "mwb/document/session.hpp"

#include "mwb/document/native_format.hpp"

#include "mwb/core/validation.hpp"

#include <algorithm>
#include <cstdint>
#include <variant>

namespace mwb::document {

namespace {

/// Events held by the document.
std::int64_t event_count(const DocumentData& document) {
    std::int64_t count = 0;
    for (const auto& sequence : document.sequences)
        for (const auto& track : sequence.tracks)
            count += static_cast<std::int64_t>(track.events.size());
    return count;
}

std::int64_t event_count(const Sequence& sequence) {
    std::int64_t count = 0;
    for (const auto& track : sequence.tracks)
        count += static_cast<std::int64_t>(track.events.size());
    return count;
}

/// Net number of events a change set adds (negative when it removes more than it inserts).
std::int64_t event_delta(const ChangeSet& changes) {
    std::int64_t delta = 0;
    for (const auto& operation : changes.operations) {
        if (std::holds_alternative<EventInsert>(operation))
            ++delta;
        else if (std::holds_alternative<EventRemove>(operation))
            --delta;
        else if (const auto* insert = std::get_if<TrackInsert>(&operation))
            delta += static_cast<std::int64_t>(insert->track.events.size());
        else if (const auto* remove = std::get_if<TrackRemove>(&operation))
            delta -= static_cast<std::int64_t>(remove->track.events.size());
        else if (const auto* sequence_insert = std::get_if<SequenceInsert>(&operation))
            delta += event_count(sequence_insert->sequence);
        else if (const auto* sequence_remove = std::get_if<SequenceRemove>(&operation))
            delta -= event_count(sequence_remove->sequence);
    }
    return delta;
}

} // namespace

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
    // An editable document stays within the event limit: a larger one could be saved and
    // checkpointed but would reopen read-only (section 28.3).
    if (const std::int64_t delta = event_delta(changes); delta > 0) {
        const auto limit = static_cast<std::int64_t>(DocumentLimits{}.max_events_per_document);
        if (event_count(document_) + delta > limit)
            return make_error(ErrorCode::limit_exceeded,
                              "the edit would exceed the limit of " + std::to_string(limit) +
                                  " events per editable document");
    }
    const Generation base = generation_;
    const std::size_t evicted_before = history_.evicted_entries();
    // The change set moves into the history (no copy); listeners see the stored entry, which
    // is the newest undo entry and is never evicted.
    MWB_TRY(history_.execute(document_, std::move(changes)));
    generation_ = base + 1;
    document_.generation = generation_;
    if (const ChangeSet* applied = history_.newest_undo_changes())
        notify(SessionEventKind::edited, *applied, base, generation_);
    if (history_.evicted_entries() != evicted_before) {
        const ChangeSet nothing;
        notify(SessionEventKind::history_trimmed, nothing, generation_, generation_);
    }
    return generation_;
}

Result<Generation> WorkingSession::undo() {
    const Generation base = generation_;
    auto applied = history_.undo(document_);
    if (!applied)
        return std::unexpected(std::move(applied).error());
    generation_ = base + 1;
    document_.generation = generation_;
    notify(SessionEventKind::undone, *applied, base, generation_);
    return generation_;
}

Result<Generation> WorkingSession::redo() {
    const Generation base = generation_;
    auto applied = history_.redo(document_);
    if (!applied)
        return std::unexpected(std::move(applied).error());
    generation_ = base + 1;
    document_.generation = generation_;
    notify(SessionEventKind::redone, *applied, base, generation_);
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
    // The saved generation, not the current one: edits made while the save ran stay unsaved.
    notify(SessionEventKind::saved, nothing, ticket.generation, ticket.generation);
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

void WorkingSession::notify(SessionEventKind kind, const ChangeSet& applied, Generation base, Generation result) {
    // Copy so a listener may add or remove listeners while being notified.
    const auto listeners = listeners_;
    const SessionEvent event{kind, applied, base, result, document_};
    for (const auto& [id, listener] : listeners)
        if (listener)
            listener(event);
}

} // namespace mwb::document
