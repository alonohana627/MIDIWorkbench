#include "mwb/ui/document_controller.hpp"

#include "mwb/ui/task_thread.hpp"

#include "mwb/document/native_format.hpp"
#include "mwb/document/recovery.hpp"
#include "mwb/document/session.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <cmath>
#include <thread>
#include <unordered_map>

namespace mwb::ui {

DocumentController::DocumentController(std::unique_ptr<document::WorkingSession> session, QObject* parent)
    : QObject(parent), session_(std::move(session)) {
    if (session_->file_path())
        file_path_ = *session_->file_path();
    const auto& document = session_->document();
    if (!document.sequences.empty() && !document.sequences.front().tracks.empty()) {
        // Prefer the first track that contains notes as the active write track.
        active_track_ = document.sequences.front().tracks.front().id;
        for (const auto& track : document.sequences.front().tracks) {
            const bool has_notes = std::any_of(track.events.begin(), track.events.end(),
                                               [](const Event& event) { return event.is_note_start(); });
            if (has_notes) {
                active_track_ = track.id;
                break;
            }
        }
    }
    last_dirty_ = session_->is_dirty();
}

DocumentController::~DocumentController() = default;

void DocumentController::attach_recovery(std::shared_ptr<document::RecoveryJournal> journal) {
    journal_ = std::move(journal);
    if (journal_)
        document::connect_recovery(*session_, journal_);
}

bool DocumentController::flush_recovery(QString* error) {
    if (!journal_)
        return true;
    if (auto status = journal_->flush(false); !status) { // never waits for a checkpoint in progress
        if (error)
            *error = QString::fromStdString(status.error().describe());
        return false;
    }
    if (const auto failure = journal_->last_error()) {
        if (error)
            *error = QString::fromStdString(failure->describe());
        return false;
    }
    return true;
}

namespace {
/// Recovery journal flushes for every document run here, in order.
TaskThread& recovery_thread() {
    static TaskThread thread(true); // drains: closing sessions must reach the database
    return thread;
}
} // namespace

void drain_recovery_thread() { recovery_thread().wait_idle(); }

void DocumentController::flush_recovery_async() {
    if (!journal_ || recovery_flush_running_)
        return;
    recovery_flush_running_ = true;
    auto journal = journal_;
    QPointer<DocumentController> guard(this);
    const auto deliver = [guard](QString error) {
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, error] {
                if (!guard)
                    return;
                guard->recovery_flush_running_ = false;
                guard->recovery_error_ = error;
                emit guard->recovery_flushed(error);
            },
            Qt::QueuedConnection);
    };
    const bool queued = recovery_thread().post(
        [journal, deliver](std::stop_token) {
            QString error;
            if (auto status = journal->flush(true); !status)
                error = QString::fromStdString(status.error().describe());
            else if (const auto failure = journal->last_error())
                error = QString::fromStdString(failure->describe());
            deliver(error);
        },
        // A throwing flush (a storage exception the journal did not wrap) must not block every
        // later flush of this document: the flag is cleared and the failure shown.
        [deliver] { deliver(tr("the recovery database reported an unexpected error")); });
    if (!queued)
        recovery_flush_running_ = false;
}

void DocumentController::close_recovery() {
    if (!journal_)
        return;
    // A clean close deletes the session's journal and checkpoints, so nothing needs flushing first.
    // It runs on the recovery thread (after any flush in progress); the window drains that thread
    // before the process ends.
    recovery_thread().post([journal = std::move(journal_)](std::stop_token) {
        std::ignore = journal->mark_cleanly_closed(); // a failure only leaves recoverable evidence behind
    });
    journal_.reset();
}

const DocumentData& DocumentController::document() const { return session_->document(); }

bool DocumentController::apply(const ChangeSet& changes, QString* error) {
    if (changes.empty())
        return true;
    auto result = session_->apply(changes);
    if (!result) {
        const QString message = QString::fromStdString(changes.label + ": " + result.error().describe());
        if (error)
            *error = message;
        emit edit_rejected(message);
        return false;
    }
    after_change();
    return true;
}

bool DocumentController::undo() {
    if (!session_->history().can_undo())
        return false;
    auto result = session_->undo();
    if (!result) {
        emit edit_rejected(tr("Undo failed: %1").arg(QString::fromStdString(result.error().describe())));
        return false;
    }
    after_change();
    return true;
}

bool DocumentController::redo() {
    if (!session_->history().can_redo())
        return false;
    auto result = session_->redo();
    if (!result) {
        emit edit_rejected(tr("Redo failed: %1").arg(QString::fromStdString(result.error().describe())));
        return false;
    }
    after_change();
    return true;
}

bool DocumentController::can_undo() const { return session_->history().can_undo(); }
bool DocumentController::can_redo() const { return session_->history().can_redo(); }

QString DocumentController::undo_text() const {
    const auto label = session_->history().undo_label();
    return label ? QString::fromStdString(*label) : QString();
}

QString DocumentController::redo_text() const {
    const auto label = session_->history().redo_label();
    return label ? QString::fromStdString(*label) : QString();
}

bool DocumentController::is_dirty() const { return session_->is_dirty(); }

void DocumentController::set_file_path(std::filesystem::path path) {
    file_path_ = std::move(path);
    session_->set_file_path(file_path_);
}

bool DocumentController::save_native(const std::filesystem::path& path, QString* error) {
    auto status = session_->save(path);
    if (!status) {
        // A failed save leaves history and dirty state intact (section 21.1).
        if (error)
            *error = QString::fromStdString(status.error().describe());
        return false;
    }
    file_path_ = path;
    if (journal_) {
        journal_->mark_saved(now_utc_ms());
        document::SourceReference source;
        source.document_name = document().title;
        const auto utf8 = path.u8string();
        source.file_path_utf8 = std::string(utf8.begin(), utf8.end());
        source.last_explicit_save_ms = now_utc_ms();
        journal_->update_source(std::move(source));
    }
    const bool dirty = session_->is_dirty();
    if (dirty != last_dirty_) {
        last_dirty_ = dirty;
        emit dirty_changed(dirty);
    }
    return true;
}

void DocumentController::save_native_async(const std::filesystem::path& path) {
    if (saving_)
        return;
    saving_ = true;
    auto pending = std::make_shared<PendingSave>();
    pending->ticket = session_->begin_save(); // immutable snapshot of generation G
    pending->path = path;
    pending_save_ = pending;
    QPointer<DocumentController> guard(this);
    // Joinable: closing the window waits for a save the user asked for (wait_for_save).
    save_thread_ = std::jthread([guard, pending] {
        DocumentData stamped = *pending->ticket.snapshot;
        stamped.modified_ms = now_utc_ms(); // as WorkingSession::save does
        Status outcome = document::save_native(stamped, pending->path);
        {
            std::lock_guard lock(pending->mutex);
            pending->outcome = std::move(outcome);
        }
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, pending] {
                if (guard)
                    guard->finish_save(pending);
            },
            Qt::QueuedConnection);
    });
}

void DocumentController::wait_for_save() {
    if (save_thread_.joinable())
        save_thread_.join();
    if (pending_save_)
        finish_save(pending_save_);
}

void DocumentController::finish_save(std::shared_ptr<PendingSave> pending) { // by value: it outlives pending_save_.reset()
    if (pending != pending_save_)
        return; // already completed (wait_for_save ran first)
    Status outcome = make_error(ErrorCode::internal, "save did not finish");
    {
        std::lock_guard lock(pending->mutex);
        if (!pending->outcome)
            return; // still running
        outcome = *pending->outcome;
    }
    pending_save_.reset();
    saving_ = false;
    const auto& path = pending->path;
    auto completed = session_->complete_save(pending->ticket, path, outcome);
    if (!completed) {
        emit save_finished(QString::fromStdString(completed.error().describe()));
        return;
    }
    file_path_ = path;
    if (journal_) {
        journal_->mark_saved(now_utc_ms());
        document::SourceReference source;
        source.document_name = document().title;
        const auto utf8 = path.u8string();
        source.file_path_utf8 = std::string(utf8.begin(), utf8.end());
        source.last_explicit_save_ms = now_utc_ms();
        journal_->update_source(std::move(source));
    }
    const bool dirty = session_->is_dirty();
    if (dirty != last_dirty_) {
        last_dirty_ = dirty;
        emit dirty_changed(dirty);
    }
    emit save_finished(QString());
}

void DocumentController::after_change() {
    ++generation_;
    // Drop selected notes that no longer exist; never keep pointers across edits.
    const auto& pairing = notes();
    std::set<EventId> surviving;
    if (!selection_.empty()) {
        std::unordered_map<EventId, bool, UuidHash> present;
        for (const auto& note : pairing.notes)
            present.emplace(note.on_id, true);
        for (const auto& id : selection_)
            if (present.contains(id))
                surviving.insert(id);
    }
    const bool selection_shrank = surviving.size() != selection_.size();
    selection_ = std::move(surviving);
    if (active_sequence_ >= document().sequences.size())
        active_sequence_ = 0;
    if (!active_track_index() && !document().sequences.empty() && !active_sequence().tracks.empty())
        active_track_ = active_sequence().tracks.front().id;
    emit document_changed();
    if (selection_shrank)
        emit selection_changed();
    const bool dirty = session_->is_dirty();
    if (dirty != last_dirty_) {
        last_dirty_ = dirty;
        emit dirty_changed(dirty);
    }
}

const Sequence& DocumentController::active_sequence() const {
    return document().sequences.at(std::min(active_sequence_, document().sequences.size() - 1));
}

void DocumentController::set_active_sequence(std::size_t index) {
    if (index >= document().sequences.size() || index == active_sequence_)
        return;
    active_sequence_ = index;
    selection_.clear();
    if (!active_sequence().tracks.empty())
        active_track_ = active_sequence().tracks.front().id;
    emit view_state_changed();
    emit selection_changed();
}

void DocumentController::set_active_track(TrackId track) {
    if (track == active_track_ || !active_sequence().track_index(track))
        return;
    active_track_ = track;
    // Selection only ever holds notes of the active write track.
    std::set<EventId> kept;
    const auto index = active_track_index();
    for (const auto& note : notes().notes)
        if (index && note.on_track == *index && selection_.contains(note.on_id))
            kept.insert(note.on_id);
    selection_ = std::move(kept);
    emit view_state_changed();
    emit selection_changed();
}

void DocumentController::set_selection(std::set<EventId> notes) {
    if (notes == selection_)
        return;
    selection_ = std::move(notes);
    emit selection_changed();
}

bool DocumentController::set_tempo_at(Tick position, double bpm) {
    const Sequence& sequence = active_sequence();
    if (!sequence.division.is_ppq() || sequence.tracks.empty() || !std::isfinite(bpm))
        return false;
    const auto microseconds = static_cast<std::uint32_t>(std::llround(60'000'000.0 / std::clamp(bpm, 5.0, 999.0)));
    const Event wanted = events::tempo(0, microseconds);
    // Find the tempo event to edit: the one at tick 0 when the song has no later tempo changes,
    // otherwise the last one at or before `position` (ties: the later in track order wins, as in
    // the tempo map).
    bool later_changes = false;
    for (const Track& track : sequence.tracks)
        for (const Event& event : track.events)
            later_changes = later_changes || (event.tick > 0 && event.is_meta(meta_type::tempo));
    const Tick limit = later_changes ? std::max<Tick>(position, 0) : 0;
    const Track* target_track = nullptr;
    const Event* target = nullptr;
    for (const Track& track : sequence.tracks)
        for (const Event& event : track.events) {
            if (event.tick > limit)
                break; // events are sorted by tick
            if (event.is_meta(meta_type::tempo) && (!target || event.tick >= target->tick)) {
                target_track = &track;
                target = &event;
            }
        }
    ChangeBuilder builder(document(), "Change tempo");
    if (target) {
        if (target->payload == wanted.payload)
            return false;
        Event updated = *target;
        updated.payload = wanted.payload;
        builder.replace(target_track->id, target->id, updated);
    } else {
        std::ignore = builder.insert(sequence.tracks.front().id, wanted, OrderPlacement::before_existing);
    }
    auto changes = builder.finish();
    return !changes.empty() && apply(changes);
}

void DocumentController::set_edit_cursor(Tick tick) {
    const Tick clamped = std::max<Tick>(0, tick); // compared after clamping: no signal without a change
    if (clamped == edit_cursor_)
        return;
    edit_cursor_ = clamped;
    emit edit_cursor_changed(); // not view_state_changed: panels need not rebuild while scrubbing
}

void DocumentController::set_show_ghost_tracks(bool show) {
    if (show == show_ghost_tracks_)
        return;
    show_ghost_tracks_ = show;
    emit view_state_changed();
}

std::optional<std::size_t> DocumentController::active_track_index() const {
    return active_sequence().track_index(active_track_);
}

void DocumentController::rebuild_caches() const {
    if (cache_generation_ == generation_ && cache_sequence_ == active_sequence_)
        return;
    const auto& sequence = active_sequence();
    pairing_ = pair_notes(sequence);
    index_ = NoteIndex(pairing_.notes);
    tempo_ = TempoMap::build(sequence);
    meter_ = MeterMap::build(sequence);
    cache_generation_ = generation_;
    cache_sequence_ = active_sequence_;
}

const NotePairing& DocumentController::notes() const {
    rebuild_caches();
    return pairing_;
}

const NoteIndex& DocumentController::note_index() const {
    rebuild_caches();
    return index_;
}

const TempoMap& DocumentController::tempo_map() const {
    rebuild_caches();
    return *tempo_;
}

const MeterMap& DocumentController::meter_map() const {
    rebuild_caches();
    return *meter_;
}

std::vector<NoteSpan> DocumentController::selected_notes() const {
    std::vector<NoteSpan> result;
    if (selection_.empty())
        return result;
    for (const auto& note : notes().notes)
        if (selection_.contains(note.on_id))
            result.push_back(note);
    return result;
}

} // namespace mwb::ui
