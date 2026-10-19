#include "mwb/playback/scheduler.hpp"

#include <algorithm>

namespace mwb::playback {

namespace {

/// round(numerator / denominator) half away from zero for non-negative values.
std::int64_t round_positive(__int128 numerator, __int128 denominator) noexcept {
    return static_cast<std::int64_t>((numerator * 2 + denominator) / (denominator * 2));
}

constexpr std::uint8_t control_change = 0xB0;

} // namespace

// --- OwnedNoteRegistry ------------------------------------------------------------------------

OwnedNoteRegistry::OwnedNoteRegistry(std::size_t capacity) : entries_(std::max<std::size_t>(capacity, 1)) {}

bool OwnedNoteRegistry::add(const OwnedNote& note) noexcept {
    if (size_ >= entries_.size())
        return false;
    entries_[size_++] = note;
    return true;
}

std::optional<OwnedNote> OwnedNoteRegistry::remove_note(std::uint32_t note) noexcept {
    for (std::size_t index = 0; index < size_; ++index) {
        if (entries_[index].note == note) {
            const OwnedNote removed = entries_[index];
            erase_at(index);
            return removed;
        }
    }
    return std::nullopt;
}

void OwnedNoteRegistry::erase_at(std::size_t position) noexcept {
    if (position >= size_)
        return;
    entries_[position] = entries_[size_ - 1];
    --size_;
}

// --- Scheduler --------------------------------------------------------------------------------

Scheduler::Scheduler(SchedulerSettings settings)
    : settings_(settings),
      registry_(settings.owned_note_capacity),
      live_(std::max<std::size_t>(settings.route_capacity, 1), ChannelState::defaults()),
      scratch_(std::max<std::size_t>(settings.route_capacity, 1), ChannelState::defaults()),
      route_used_(std::max<std::size_t>(settings.route_capacity, 1), 0),
      scratch_used_(std::max<std::size_t>(settings.route_capacity, 1), 0) {}

void Scheduler::emit_event(IEventSink& sink, const RealtimeMidiEvent& event) noexcept {
    ++emitted_;
    sink.on_event(event);
}

void Scheduler::emit_message(IEventSink& sink, std::uint32_t route, std::uint8_t status, std::uint8_t data1,
                             std::uint8_t data2, std::uint32_t offset) noexcept {
    if (route < live_.size()) {
        live_[route].apply(status, data1, data2);
        route_used_[route] = 1;
    }
    emit_event(sink, RealtimeMidiEvent::short_message(status, data1, data2, offset, route));
}

std::int64_t Scheduler::loop_iteration_start(std::int64_t iteration) const noexcept {
    if (snapshot_ == nullptr || !snapshot_->loop)
        return anchor_;
    const LoopInfo& loop = *snapshot_->loop;
    return anchor_ + round_positive(static_cast<__int128>(iteration) * loop.length_numerator, loop.length_denominator);
}

std::int64_t Scheduler::event_absolute_frame(std::size_t index) const noexcept {
    const ScheduledEvent& event = snapshot_->events[index];
    if (!looping_)
        return anchor_ + event.frame;
    const LoopInfo& loop = *snapshot_->loop;
    const std::int64_t absolute = loop_iteration_start(iteration_) + (event.frame - loop.start_frame);
    // Events inside the loop range always play in their iteration, even when rounding would push
    // them onto the (integer) iteration boundary.
    const std::int64_t last = loop_iteration_start(iteration_ + 1) - 1;
    return std::min(absolute, last);
}

std::int64_t Scheduler::song_frame_at(std::int64_t absolute) const noexcept {
    if (snapshot_ == nullptr)
        return 0;
    if (looping_)
        return snapshot_->loop->start_frame + (absolute - loop_iteration_start(iteration_));
    return absolute - anchor_;
}

std::int64_t Scheduler::song_frame() const noexcept {
    if (snapshot_ == nullptr)
        return 0;
    if (!playing_)
        return snapshot_->mapping.frame_at(stopped_tick_);
    return song_frame_at(absolute_frame_);
}

Tick Scheduler::position_tick() const noexcept {
    if (!playing_ || snapshot_ == nullptr)
        return stopped_tick_;
    return snapshot_->mapping.first_tick_at_or_after_frame(song_frame_at(absolute_frame_));
}

void Scheduler::dispatch(IEventSink& sink, std::size_t index, std::uint32_t offset) noexcept {
    const ScheduledEvent& event = snapshot_->events[index];
    switch (event.kind) {
    case RealtimeEventKind::tempo: {
        RealtimeMidiEvent marker;
        marker.kind = RealtimeEventKind::tempo;
        marker.frame_offset = offset;
        marker.value = event.value;
        emit_event(sink, marker);
        return;
    }
    case RealtimeEventKind::metronome_click: {
        if (!settings_.metronome)
            return;
        RealtimeMidiEvent click;
        click.kind = RealtimeEventKind::metronome_click;
        click.frame_offset = offset;
        click.value = event.value;
        emit_event(sink, click);
        return;
    }
    case RealtimeEventKind::midi:
        break;
    }
    switch (event.role) {
    case NoteRole::note_on:
        if (!registry_.add(OwnedNote{event.note, event.route, static_cast<std::uint8_t>(event.bytes[0] & 0x0Fu),
                                     event.bytes[1]})) {
            ++dropped_note_ons_;
            return;
        }
        if (event.route < route_used_.size())
            route_used_[event.route] = 1;
        emit_event(sink, RealtimeMidiEvent::short_message(event.bytes[0], event.bytes[1], event.bytes[2], offset, event.route));
        return;
    case NoteRole::note_off:
        if (!registry_.remove_note(event.note))
            return; // Not owned (started before the playback position or dropped): nothing to end.
        emit_event(sink, RealtimeMidiEvent::short_message(event.bytes[0], event.bytes[1], event.bytes[2], offset, event.route));
        return;
    case NoteRole::none:
        emit_message(sink, event.route, event.bytes[0], event.bytes[1], event.bytes[2], offset);
        return;
    }
}

void Scheduler::release_owned(IEventSink& sink, std::uint32_t offset) noexcept {
    for (const OwnedNote& note : registry_.active())
        emit_event(sink, RealtimeMidiEvent::short_message(static_cast<std::uint8_t>(0x80u | note.channel), note.pitch, 0,
                                                    offset, note.route));
    registry_.clear();
}

void Scheduler::sustain_off(IEventSink& sink, std::uint32_t offset) noexcept {
    if (snapshot_ == nullptr)
        return;
    const std::size_t count = std::min(snapshot_->routes.size(), live_.size());
    for (std::size_t route = 0; route < count; ++route) {
        if (!live_[route].sustain_down())
            continue;
        emit_message(sink, static_cast<std::uint32_t>(route),
                     static_cast<std::uint8_t>(control_change | snapshot_->routes[route].channel), 64, 0, offset);
    }
}

void Scheduler::chase_to(std::size_t event_index, Tick tick, IEventSink& sink, std::uint32_t offset) noexcept {
    const std::size_t count = std::min(snapshot_->routes.size(), live_.size());
    snapshot_->state_before(event_index, std::span<ChannelState>(scratch_.data(), count));
    for (std::size_t route = 0; route < count; ++route) {
        if (!scratch_[route].any_touched() && !live_[route].any_touched())
            continue;
        const std::uint8_t channel = snapshot_->routes[route].channel;
        const std::size_t written = write_chase_messages(scratch_[route], live_[route], channel, chase_buffer_);
        for (std::size_t index = 0; index < written; ++index) {
            const ShortMessage& message = chase_buffer_[index];
            emit_event(sink, RealtimeMidiEvent::short_message(message.status, message.data1, message.data2, offset,
                                                        static_cast<std::uint32_t>(route)));
        }
        if (written > 0)
            route_used_[route] = 1;
        live_[route] = scratch_[route];
    }
    if (settings_.chase != ChaseMode::state_and_held_notes)
        return;
    // Held-note chase: restart notes crossing `tick` with their remaining duration.
    for (std::uint32_t index = 0; index < snapshot_->notes.size(); ++index) {
        const NoteBoundary& note = snapshot_->notes[index];
        if (note.start_tick >= tick)
            break; // Notes are ordered by start.
        if (note.end_tick <= tick)
            continue;
        if (!registry_.add(OwnedNote{index, note.route, note.channel, note.pitch})) {
            ++dropped_note_ons_;
            continue;
        }
        emit_event(sink, RealtimeMidiEvent::short_message(static_cast<std::uint8_t>(0x90u | note.channel), note.pitch,
                                                    note.velocity, offset, note.route));
    }
}

void Scheduler::anchor_at(Tick tick, std::int64_t absolute) noexcept {
    const std::int64_t position = snapshot_->mapping.frame_at(tick);
    const auto& loop = snapshot_->loop;
    iteration_ = 0;
    cursor_ = snapshot_->first_event_at_or_after(tick);
    if (settings_.loop_enabled && loop && tick < loop->end_tick &&
        loop->length_numerator >= loop->length_denominator) {
        looping_ = true;
        anchor_ = absolute - position + loop->start_frame;
        iteration_end_ = loop->end_event;
    } else {
        looping_ = false;
        anchor_ = absolute - position;
        iteration_end_ = snapshot_->events.size();
    }
}

bool Scheduler::set_snapshot(const PlaybackSnapshot* snapshot, IEventSink& sink) noexcept {
    if (snapshot == snapshot_)
        return true;
    if (snapshot != nullptr && snapshot->routes.size() > live_.size())
        return false;
    if (snapshot == nullptr) {
        stop(sink);
        snapshot_ = nullptr;
        return true;
    }
    if (snapshot_ == nullptr) {
        snapshot_ = snapshot;
        std::fill(live_.begin(), live_.end(), ChannelState::defaults());
        std::fill(route_used_.begin(), route_used_.end(), std::uint8_t{0});
        return true;
    }
    const PlaybackSnapshot* old = snapshot_;
    const Tick current = position_tick();

    // Reconcile owned notes by stable identity; release removed or changed notes through the old
    // routes before switching.
    if (playing_) {
        for (std::size_t position = registry_.size(); position-- > 0;) {
            OwnedNote& entry = registry_.active_mutable()[position];
            bool keep = false;
            if (entry.note < old->notes.size()) {
                const std::uint32_t index = snapshot->find_note(old->notes[entry.note].owner);
                if (index != no_note) {
                    const NoteBoundary& note = snapshot->notes[index];
                    keep = entry.route < old->routes.size() &&
                           snapshot->routes[note.route].key == old->routes[entry.route].key &&
                           note.pitch == entry.pitch && note.start_tick < current && note.end_tick > current;
                    if (keep) {
                        entry.note = index;
                        entry.route = note.route;
                    }
                }
            }
            if (!keep) {
                emit_event(sink, RealtimeMidiEvent::short_message(static_cast<std::uint8_t>(0x80u | entry.channel),
                                                            entry.pitch, 0, 0, entry.route));
                registry_.erase_at(position);
            }
        }
    }

    // Remap the sent channel state by route identity; release sustain on routes that disappear.
    for (std::size_t old_route = 0; old_route < old->routes.size(); ++old_route) {
        bool survives = false;
        for (const Route& route : snapshot->routes)
            survives = survives || route.key == old->routes[old_route].key;
        if (!survives && live_[old_route].sustain_down())
            emit_message(sink, static_cast<std::uint32_t>(old_route),
                         static_cast<std::uint8_t>(control_change | old->routes[old_route].channel), 64, 0, 0);
    }
    for (std::size_t new_route = 0; new_route < snapshot->routes.size(); ++new_route) {
        scratch_[new_route] = ChannelState::defaults();
        scratch_used_[new_route] = 0;
        for (std::size_t old_route = 0; old_route < old->routes.size(); ++old_route) {
            if (old->routes[old_route].key == snapshot->routes[new_route].key) {
                scratch_[new_route] = live_[old_route];
                scratch_used_[new_route] = route_used_[old_route];
                break;
            }
        }
    }
    std::copy_n(scratch_.begin(), snapshot->routes.size(), live_.begin());
    std::copy_n(scratch_used_.begin(), snapshot->routes.size(), route_used_.begin());

    snapshot_ = snapshot;
    if (playing_)
        anchor_at(current, absolute_frame_);
    return true;
}

void Scheduler::start(Tick tick, IEventSink& sink, std::uint32_t offset) noexcept {
    tick = std::max<Tick>(tick, 0);
    if (snapshot_ == nullptr) {
        stopped_tick_ = tick;
        return;
    }
    if (playing_) {
        release_owned(sink, offset);
        sustain_off(sink, offset);
    }
    anchor_at(tick, absolute_frame_ + offset);
    chase_to(cursor_, tick, sink, offset);
    playing_ = true;
}

void Scheduler::seek(Tick tick, IEventSink& sink, std::uint32_t offset) noexcept {
    ++epoch_;
    if (playing_)
        start(tick, sink, offset);
    else
        stopped_tick_ = std::max<Tick>(tick, 0);
}

void Scheduler::stop(IEventSink& sink, std::uint32_t offset) noexcept {
    if (playing_)
        stopped_tick_ = position_tick();
    release_owned(sink, offset);
    sustain_off(sink, offset);
    playing_ = false;
}

void Scheduler::panic(IEventSink& sink, std::uint32_t offset) noexcept {
    stop(sink, offset);
    if (snapshot_ == nullptr)
        return;
    const std::size_t count = std::min(snapshot_->routes.size(), live_.size());
    for (std::size_t route = 0; route < count; ++route) {
        if (route_used_[route] == 0)
            continue;
        const auto route_index = static_cast<std::uint32_t>(route);
        const std::uint8_t channel = snapshot_->routes[route].channel;
        const auto status = static_cast<std::uint8_t>(control_change | channel);
        emit_message(sink, route_index, status, 64, 0, offset);
        emit_message(sink, route_index, status, 123, 0, offset);
        emit_message(sink, route_index, status, 120, 0, offset);
        emit_message(sink, route_index, static_cast<std::uint8_t>(0xE0u | channel), 0x00, 0x40, offset);
    }
}

BlockResult Scheduler::process_block(std::uint32_t frame_count, IEventSink& sink) noexcept {
    BlockResult result;
    emitted_ = 0;
    const std::int64_t block_start = absolute_frame_;
    const std::int64_t block_end = block_start + frame_count;
    if (!playing_ || snapshot_ == nullptr) {
        absolute_frame_ = block_end;
        return result;
    }
    while (true) {
        if (cursor_ < iteration_end_) {
            const std::int64_t frame = event_absolute_frame(cursor_);
            if (frame < block_end) {
                const auto offset = static_cast<std::uint32_t>(std::max<std::int64_t>(frame - block_start, 0));
                dispatch(sink, cursor_, offset);
                ++cursor_;
                continue;
            }
        }
        if (looping_ && cursor_ >= iteration_end_) {
            const std::int64_t wrap = loop_iteration_start(iteration_ + 1);
            if (wrap < block_end) {
                const auto offset = static_cast<std::uint32_t>(std::max<std::int64_t>(wrap - block_start, 0));
                const LoopInfo& loop = *snapshot_->loop;
                release_owned(sink, offset);
                sustain_off(sink, offset);
                ++iteration_;
                ++result.loop_wraps;
                cursor_ = loop.first_event;
                iteration_end_ = loop.end_event;
                chase_to(loop.first_event, loop.start_tick, sink, offset);
                continue;
            }
        }
        break;
    }
    if (!looping_ && cursor_ >= snapshot_->events.size() && block_end - anchor_ >= snapshot_->end_frame)
        result.reached_end = true;
    absolute_frame_ = block_end;
    result.events = emitted_;
    return result;
}

} // namespace mwb::playback
