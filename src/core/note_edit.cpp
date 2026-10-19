#include "mwb/core/note_edit.hpp"

#include "mwb/core/checked.hpp"

#include <algorithm>
#include <unordered_map>

namespace mwb {

namespace {

Status validate_note_values(Tick start, Tick end, int pitch, bool allow_zero_length) {
    if (start < 0)
        return make_error(ErrorCode::invalid_argument, "note would start before the beginning of the sequence");
    if (end < start || (!allow_zero_length && end == start))
        return make_error(ErrorCode::invalid_argument, "note length must be positive");
    if (pitch < 0 || pitch > 127)
        return make_error(ErrorCode::invalid_argument, "pitch would leave the MIDI range 0..127");
    return ok();
}

std::unordered_map<EventId, const NoteSpan*, UuidHash> index_notes(const NotePairing& pairing) {
    std::unordered_map<EventId, const NoteSpan*, UuidHash> index;
    index.reserve(pairing.notes.size());
    for (const auto& note : pairing.notes)
        index.emplace(note.on_id, &note);
    return index;
}

} // namespace

const NoteSpan* find_note(const NotePairing& pairing, EventId note) {
    for (const auto& span : pairing.notes)
        if (span.on_id == note)
            return &span;
    return nullptr;
}

Result<ChangeSet> insert_notes(const DocumentData& document, std::span<const NewNote> notes, std::string label) {
    // One merge per destination track instead of two mid-vector inserts per note.
    std::unordered_map<TrackId, BulkTrackEdit, UuidHash> edits;
    for (const auto& note : notes) {
        MWB_TRY(validate_note_values(note.start, note.end, note.pitch, false));
        // Same contract as update_notes: out-of-range values are rejected, never masked or clamped.
        if (note.channel > 15 || note.velocity < 1 || note.velocity > 127 || note.release_velocity > 127)
            return make_error(ErrorCode::invalid_argument, "channel or velocity is out of range");
        auto [entry, created] = edits.try_emplace(note.track);
        if (created && !find_track(document, note.track))
            return make_error(ErrorCode::not_found, "destination track does not exist");
        entry->second.insert.emplace_back(
            events::note_on(note.start, note.channel, note.pitch, std::max<std::uint8_t>(note.velocity, 1)),
            OrderPlacement::after_existing);
        entry->second.insert.emplace_back(events::note_off(note.end, note.channel, note.pitch, note.release_velocity),
                                          OrderPlacement::before_note_starts);
    }
    ChangeBuilder builder(document, std::move(label));
    for (auto& [track, bulk] : edits)
        if (!builder.edit_track(track, std::move(bulk)))
            return make_error(ErrorCode::not_found, "destination track does not exist");
    return builder.finish();
}

Result<ChangeSet> update_notes(const DocumentData& document, SequenceId sequence_id,
                               std::span<const NoteUpdate> updates, std::string label) {
    const auto sequence_index = document.sequence_index(sequence_id);
    if (!sequence_index)
        return make_error(ErrorCode::not_found, "sequence does not exist");
    const Sequence& sequence = document.sequences[*sequence_index];
    const auto pairing = pair_notes(sequence);
    const auto index = index_notes(pairing);
    // Event lookups through a per-track identity index (built once per touched track), then one
    // merge per track: O(n + k log k) instead of two linear scans and a vector shift per note.
    std::unordered_map<std::uint32_t, std::unordered_map<EventId, std::size_t, UuidHash>> positions;
    const auto event_of = [&](std::uint32_t track_index, EventId id) -> const Event* {
        auto [entry, created] = positions.try_emplace(track_index);
        const auto& events = sequence.tracks[track_index].events;
        if (created) {
            entry->second.reserve(events.size());
            for (std::size_t position = 0; position < events.size(); ++position)
                entry->second.emplace(events[position].id, position);
        }
        const auto found = entry->second.find(id);
        return found == entry->second.end() ? nullptr : &events[found->second];
    };
    std::unordered_map<TrackId, BulkTrackEdit, UuidHash> edits;
    const auto place = [&](TrackId track, const Event& current, Event updated, OrderPlacement placement) {
        auto& bulk = edits[track];
        if (updated.tick == current.tick) {
            bulk.replace_in_place.insert_or_assign(current.id, std::move(updated));
        } else {
            updated.id = current.id; // a moved event keeps its identity
            bulk.remove.insert(current.id);
            bulk.insert.emplace_back(std::move(updated), placement);
        }
    };
    for (const auto& update : updates) {
        const auto found = index.find(update.note);
        if (found == index.end())
            return make_error(ErrorCode::not_found, "note to edit no longer exists");
        const NoteSpan& span = *found->second;
        const bool was_zero_length = (span.flags & note_flag::zero_length) != 0 && update.end == update.start;
        MWB_TRY(validate_note_values(update.start, update.end, update.pitch, was_zero_length));
        if (update.channel > 15 || update.velocity < 1 || update.velocity > 127 || update.release_velocity > 127)
            return make_error(ErrorCode::invalid_argument, "channel or velocity is out of range");
        const TrackId on_track = sequence.tracks[span.on_track].id;
        const Event* on_event = event_of(span.on_track, span.on_id);
        if (!on_event)
            return make_error(ErrorCode::internal, "note start event is missing");
        Event on = *on_event;
        const bool on_changed = on.tick != update.start || on.data1() != update.pitch || on.data2() != update.velocity ||
                                on.channel() != update.channel;
        // A zero-length result keeps its end after its own start at the shared tick: a moved
        // start and end are both placed after the existing events, start first (list order),
        // otherwise FIFO pairing meets the end first and the note never ends (section 16.4).
        const bool zero_length = update.end == update.start;
        if (on_changed) {
            on.tick = update.start;
            on.code = static_cast<std::uint8_t>(0x90 | update.channel);
            on.payload = Payload{update.pitch, update.velocity};
            place(on_track, *on_event, std::move(on), OrderPlacement::after_existing);
        }
        if (span.off_id) {
            const TrackId off_track = sequence.tracks[span.off_track].id;
            const Event* off_event = event_of(span.off_track, *span.off_id);
            if (!off_event)
                return make_error(ErrorCode::internal, "note end event is missing");
            Event off = *off_event;
            // Keep the stored note-end encoding (note-off or velocity-zero note-on).
            const bool velocity_zero_encoding = off.message() == ChannelMessage::note_on;
            const std::uint8_t release = velocity_zero_encoding ? 0 : update.release_velocity;
            const bool off_changed = off.tick != update.end || off.data1() != update.pitch ||
                                     off.channel() != update.channel || (!velocity_zero_encoding && off.data2() != release);
            if (off_changed) {
                off.tick = update.end;
                off.code = static_cast<std::uint8_t>((velocity_zero_encoding ? 0x90 : 0x80) | update.channel);
                off.payload = Payload{update.pitch, release};
                place(off_track, *off_event, std::move(off),
                      zero_length ? OrderPlacement::after_existing : OrderPlacement::before_note_starts);
            }
        } else if (update.end != span.end) {
            // An unterminated note gets an explicit end once the user sets one.
            edits[on_track].insert.emplace_back(
                events::note_off(update.end, update.channel, update.pitch, update.release_velocity),
                OrderPlacement::before_note_starts);
        }
    }
    ChangeBuilder builder(document, std::move(label));
    for (auto& [track, bulk] : edits)
        if (!builder.edit_track(track, std::move(bulk)))
            return make_error(ErrorCode::internal, "note track is missing");
    return builder.finish();
}

Result<ChangeSet> delete_notes(const DocumentData& document, SequenceId sequence_id, std::span<const EventId> notes,
                               std::string label) {
    const auto sequence_index = document.sequence_index(sequence_id);
    if (!sequence_index)
        return make_error(ErrorCode::not_found, "sequence does not exist");
    const Sequence& sequence = document.sequences[*sequence_index];
    const auto pairing = pair_notes(sequence);
    const auto index = index_notes(pairing);
    // Group removals per track and remove in bulk to stay linear for large selections.
    std::unordered_map<TrackId, std::vector<EventId>, UuidHash> removals;
    for (const auto& note : notes) {
        const auto found = index.find(note);
        if (found == index.end())
            continue;
        const NoteSpan& span = *found->second;
        removals[sequence.tracks[span.on_track].id].push_back(span.on_id);
        if (span.off_id)
            removals[sequence.tracks[span.off_track].id].push_back(*span.off_id);
    }
    ChangeBuilder builder(document, std::move(label));
    for (auto& [track, ids] : removals) {
        const Track* working = builder.working_track(track);
        std::unordered_map<EventId, bool, UuidHash> doomed;
        for (const auto& id : ids)
            doomed.emplace(id, true);
        std::vector<Event> kept;
        kept.reserve(working->events.size());
        for (const auto& event : working->events)
            if (!doomed.contains(event.id))
                kept.push_back(event);
        builder.set_events(track, std::move(kept));
    }
    return builder.finish();
}

Result<SplitNote> split_note(const DocumentData& document, SequenceId sequence_id, EventId note, Tick at,
                             std::string label) {
    const auto sequence_index = document.sequence_index(sequence_id);
    if (!sequence_index)
        return make_error(ErrorCode::not_found, "sequence does not exist");
    const Sequence& sequence = document.sequences[*sequence_index];
    const auto pairing = pair_notes(sequence);
    const NoteSpan* span = find_note(pairing, note);
    if (!span)
        return make_error(ErrorCode::not_found, "note to split no longer exists");
    if (at <= span->start || at >= span->end)
        return make_error(ErrorCode::invalid_argument, "split point must lie inside the note");
    // One bulk edit per track, so the moved note end and the new note start are ordered in one
    // pass (note ends before note starts at the split tick).
    std::unordered_map<TrackId, BulkTrackEdit, UuidHash> edits;
    const TrackId on_track = sequence.tracks[span->on_track].id;
    if (span->off_id) {
        const auto& off_events = sequence.tracks[span->off_track].events;
        const auto position = find_event(sequence.tracks[span->off_track], *span->off_id);
        if (!position)
            return make_error(ErrorCode::internal, "note end event is missing");
        Event moved = off_events[*position];
        moved.tick = at;
        auto& bulk = edits[sequence.tracks[span->off_track].id];
        bulk.remove.insert(moved.id);
        bulk.insert.emplace_back(moved, OrderPlacement::before_note_starts);
    } else {
        edits[on_track].insert.emplace_back(events::note_off(at, span->channel, span->pitch, span->release_velocity),
                                            OrderPlacement::before_note_starts);
    }
    Event tail_on = events::note_on(at, span->channel, span->pitch, std::max<std::uint8_t>(span->velocity, 1));
    tail_on.id = EventId::generate();
    auto& bulk = edits[on_track];
    bulk.insert.emplace_back(tail_on, OrderPlacement::after_existing);
    if (span->off_id)
        bulk.insert.emplace_back(events::note_off(span->end, span->channel, span->pitch, span->release_velocity),
                                 OrderPlacement::before_note_starts);
    ChangeBuilder builder(document, std::move(label));
    for (auto& [track, edit] : edits)
        if (!builder.edit_track(track, std::move(edit)))
            return make_error(ErrorCode::internal, "note track is missing");
    return SplitNote{builder.finish(), tail_on.id};
}

Result<DuplicatedNotes> duplicate_notes(const DocumentData& document, SequenceId sequence_id,
                                        std::span<const EventId> notes, Tick offset, int transpose,
                                        std::optional<TrackId> destination, std::string label) {
    const auto sequence_index = document.sequence_index(sequence_id);
    if (!sequence_index)
        return make_error(ErrorCode::not_found, "sequence does not exist");
    const Sequence& sequence = document.sequences[*sequence_index];
    const auto pairing = pair_notes(sequence);
    const auto index = index_notes(pairing);
    std::vector<const NoteSpan*> spans;
    for (const auto& note : notes) {
        const auto found = index.find(note);
        if (found != index.end())
            spans.push_back(found->second);
    }
    // Constrain as a whole: if any copy would leave the valid range, reject (section 7.3).
    if (transpose < -127 || transpose > 127)
        return make_error(ErrorCode::invalid_argument, "pitch would leave the MIDI range 0..127");
    for (const auto* span : spans) {
        const auto start = checked_add(span->start, offset);
        const auto end = checked_add(span->end, offset);
        if (!start || !end)
            return make_error(ErrorCode::invalid_argument, "the copy would leave the representable time range");
        MWB_TRY(validate_note_values(*start, *end, span->pitch + transpose, (span->flags & note_flag::zero_length) != 0));
    }
    if (destination) {
        // Ticks are only meaningful inside the source sequence's division and tempo map.
        const auto location = find_track(document, *destination);
        if (!location)
            return make_error(ErrorCode::not_found, "destination track does not exist");
        if (location->sequence != *sequence_index)
            return make_error(ErrorCode::invalid_argument, "destination track belongs to another sequence");
    }
    DuplicatedNotes result;
    std::unordered_map<TrackId, BulkTrackEdit, UuidHash> edits;
    for (const auto* span : spans) {
        const TrackId track = destination.value_or(sequence.tracks[span->on_track].id);
        Event on = events::note_on(span->start + offset, span->channel, static_cast<std::uint8_t>(span->pitch + transpose),
                                   std::max<std::uint8_t>(span->velocity, 1));
        on.id = EventId::generate();
        on.flags = (span->flags & note_flag::muted) != 0 ? event_flag::muted : 0u; // a muted note copies as muted
        result.created.push_back(on.id);
        auto& bulk = edits[track];
        bulk.insert.emplace_back(std::move(on), OrderPlacement::after_existing);
        // A zero-length copy keeps its end after its own start (list order inside the same
        // placement); a positive-length copy ends before note starts at its end tick.
        const bool zero_length = span->end == span->start;
        bulk.insert.emplace_back(events::note_off(span->end + offset, span->channel,
                                                  static_cast<std::uint8_t>(span->pitch + transpose),
                                                  span->release_velocity),
                                 zero_length ? OrderPlacement::after_existing : OrderPlacement::before_note_starts);
    }
    ChangeBuilder builder(document, std::move(label));
    for (auto& [track, bulk] : edits)
        if (!builder.edit_track(track, std::move(bulk)))
            return make_error(ErrorCode::not_found, "destination track does not exist");
    result.changes = builder.finish();
    return result;
}

} // namespace mwb
