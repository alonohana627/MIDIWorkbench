#include "mwb/core/change_set.hpp"

#include "mwb/core/utility.hpp"

#include <algorithm>
#include <functional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace mwb {

namespace {

bool order_less(const Event& a, const Event& b) {
    return a.tick != b.tick ? a.tick < b.tick : a.order_key < b.order_key;
}

bool same_slot(const Event& a, const Event& b) { return a.tick == b.tick && a.order_key == b.order_key; }

Track header_of(const Track& track) { return track_header(track); }

Sequence header_of(const Sequence& sequence) { return sequence_header(sequence); }

bool same_header(const Track& a, const Track& b) {
    return a.id == b.id && a.name == b.name && a.color_rgba == b.color_rgba && a.end_tick == b.end_tick &&
           a.muted == b.muted && a.solo == b.solo && a.is_drum == b.is_drum && a.routing == b.routing &&
           a.instrument == b.instrument;
}

void assign_header(Track& target, const Track& header) {
    target.name = header.name;
    target.color_rgba = header.color_rgba;
    target.end_tick = header.end_tick;
    target.muted = header.muted;
    target.solo = header.solo;
    target.is_drum = header.is_drum;
    target.routing = header.routing;
    target.instrument = header.instrument;
}

bool same_header(const Sequence& a, const Sequence& b) {
    return a.id == b.id && a.name == b.name && a.division == b.division && a.end_tick == b.end_tick &&
           a.root_pc == b.root_pc && a.scale_mask == b.scale_mask && a.settings == b.settings &&
           a.regions == b.regions && a.scale_regions == b.scale_regions && a.recipes == b.recipes;
}

void assign_header(Sequence& target, const Sequence& header) {
    target.name = header.name;
    target.division = header.division;
    target.end_tick = header.end_tick;
    target.root_pc = header.root_pc;
    target.scale_mask = header.scale_mask;
    target.settings = header.settings;
    target.regions = header.regions;
    target.scale_regions = header.scale_regions;
    target.recipes = header.recipes;
}

Track* mutable_track(DocumentData& document, TrackId id) {
    const auto location = find_track(document, id);
    if (!location)
        return nullptr;
    return &document.sequences[location->sequence].tracks[location->track];
}

Sequence* mutable_sequence(DocumentData& document, SequenceId id) {
    const auto index = document.sequence_index(id);
    return index ? &document.sequences[*index] : nullptr;
}

bool is_event_operation(const EditOperation& operation) {
    return std::holds_alternative<EventInsert>(operation) || std::holds_alternative<EventRemove>(operation) ||
           std::holds_alternative<EventReplace>(operation);
}

/// Applies a run of event operations as one validated batch per track.
Status apply_event_batch(DocumentData& document, const std::vector<const EditOperation*>& run) {
    struct TrackBatch {
        Track* track = nullptr;
        std::vector<const Event*> removals;   // must exist exactly as given
        std::vector<const Event*> insertions; // must not collide after removals
    };
    std::vector<std::pair<TrackId, TrackBatch>> batches;
    const auto batch_for = [&](TrackId id) -> TrackBatch* {
        for (auto& [track_id, batch] : batches)
            if (track_id == id)
                return &batch;
        Track* track = mutable_track(document, id);
        if (!track)
            return nullptr;
        batches.push_back({id, TrackBatch{track, {}, {}}});
        return &batches.back().second;
    };
    for (const EditOperation* operation : run) {
        const bool valid = std::visit(
            Overloaded{
                [&](const EventInsert& insert) {
                    auto* batch = batch_for(insert.track);
                    if (!batch)
                        return false;
                    batch->insertions.push_back(&insert.event);
                    return true;
                },
                [&](const EventRemove& remove) {
                    auto* batch = batch_for(remove.track);
                    if (!batch)
                        return false;
                    batch->removals.push_back(&remove.event);
                    return true;
                },
                [&](const EventReplace& replace) {
                    auto* batch = batch_for(replace.track);
                    if (!batch || replace.before.id != replace.after.id)
                        return false;
                    batch->removals.push_back(&replace.before);
                    batch->insertions.push_back(&replace.after);
                    return true;
                },
                [](const auto&) { return false; },
            },
            *operation);
        if (!valid)
            return make_error(ErrorCode::invalid_data, "event edit refers to a missing track");
    }

    // Validate every batch before mutating anything.
    std::vector<std::vector<std::size_t>> removal_positions(batches.size());
    for (std::size_t batch_index = 0; batch_index < batches.size(); ++batch_index) {
        auto& batch = batches[batch_index].second;
        auto& positions = removal_positions[batch_index];
        for (const Event* removal : batch.removals) {
            const auto position = locate_event(*batch.track, *removal);
            if (!position || !(batch.track->events[*position] == *removal))
                return make_error(ErrorCode::conflict, "event to change no longer matches the document");
            positions.push_back(*position);
        }
        std::sort(positions.begin(), positions.end());
        if (std::adjacent_find(positions.begin(), positions.end()) != positions.end())
            return make_error(ErrorCode::invalid_data, "the same event is changed twice in one command");
        if (!batch.insertions.empty()) {
            // A stale or replayed change set must not create a second event with an identity the
            // track still holds (a move removes the old copy first, so removed identities are free).
            std::unordered_set<EventId, UuidHash> inserted;
            inserted.reserve(batch.insertions.size());
            for (const Event* insertion : batch.insertions)
                if (!inserted.insert(insertion->id).second)
                    return make_error(ErrorCode::invalid_data, "two inserted events share one identity");
            std::size_t next_removal = 0;
            const auto& events = batch.track->events;
            for (std::size_t index = 0; index < events.size(); ++index) {
                if (next_removal < positions.size() && positions[next_removal] == index) {
                    ++next_removal;
                    continue;
                }
                if (inserted.contains(events[index].id))
                    return make_error(ErrorCode::conflict, "inserted event identity already exists in the track");
            }
        }
        std::vector<const Event*> sorted = batch.insertions;
        std::sort(sorted.begin(), sorted.end(), [](const Event* a, const Event* b) { return order_less(*a, *b); });
        for (std::size_t index = 1; index < sorted.size(); ++index)
            if (same_slot(*sorted[index - 1], *sorted[index]))
                return make_error(ErrorCode::invalid_data, "two inserted events share one order position");
        for (const Event* insertion : sorted) {
            if (insertion->tick < 0)
                return make_error(ErrorCode::invalid_data, "event time must not be negative");
            const auto& events = batch.track->events;
            const auto found = std::lower_bound(events.begin(), events.end(), *insertion, order_less);
            if (found != events.end() && same_slot(*found, *insertion)) {
                const auto position = static_cast<std::size_t>(found - events.begin());
                if (!std::binary_search(positions.begin(), positions.end(), position))
                    return make_error(ErrorCode::invalid_data, "inserted event collides with an existing order position");
            }
        }
        batch.insertions = std::move(sorted);
    }

    // Commit: remove, then merge sorted insertions.
    for (std::size_t batch_index = 0; batch_index < batches.size(); ++batch_index) {
        auto& batch = batches[batch_index].second;
        auto& events = batch.track->events;
        const auto& positions = removal_positions[batch_index];
        std::vector<Event> merged;
        merged.reserve(events.size() - positions.size() + batch.insertions.size());
        std::size_t next_removal = 0;
        std::size_t next_insertion = 0;
        for (std::size_t index = 0; index < events.size(); ++index) {
            if (next_removal < positions.size() && positions[next_removal] == index) {
                ++next_removal;
                continue;
            }
            while (next_insertion < batch.insertions.size() && order_less(*batch.insertions[next_insertion], events[index]))
                merged.push_back(*batch.insertions[next_insertion++]);
            merged.push_back(std::move(events[index]));
        }
        while (next_insertion < batch.insertions.size())
            merged.push_back(*batch.insertions[next_insertion++]);
        events = std::move(merged);
    }
    return ok();
}

Status apply_structural(DocumentData& document, const EditOperation& operation) {
    return std::visit(
        Overloaded{
            [&](const TrackInsert& insert) -> Status {
                Sequence* sequence = mutable_sequence(document, insert.sequence);
                if (!sequence || insert.index > sequence->tracks.size())
                    return make_error(ErrorCode::invalid_data, "track insert position is invalid");
                if (find_track(document, insert.track.id))
                    return make_error(ErrorCode::invalid_data, "track identity already exists");
                sequence->tracks.insert(sequence->tracks.begin() + static_cast<std::ptrdiff_t>(insert.index), insert.track);
                return ok();
            },
            [&](const TrackRemove& remove) -> Status {
                Sequence* sequence = mutable_sequence(document, remove.sequence);
                if (!sequence || remove.index >= sequence->tracks.size() ||
                    !(sequence->tracks[remove.index] == remove.track))
                    return make_error(ErrorCode::conflict, "track to remove no longer matches the document");
                sequence->tracks.erase(sequence->tracks.begin() + static_cast<std::ptrdiff_t>(remove.index));
                return ok();
            },
            [&](const TrackPropertiesChange& change) -> Status {
                Track* track = mutable_track(document, change.track);
                if (!track || !same_header(*track, change.before))
                    return make_error(ErrorCode::conflict, "track properties changed concurrently");
                assign_header(*track, change.after);
                return ok();
            },
            [&](const TrackMove& move) -> Status {
                Sequence* sequence = mutable_sequence(document, move.sequence);
                if (!sequence || move.from >= sequence->tracks.size() || move.to >= sequence->tracks.size())
                    return make_error(ErrorCode::invalid_data, "track move position is invalid");
                if (!move.track.is_nil() && sequence->tracks[move.from].id != move.track)
                    return make_error(ErrorCode::conflict, "track order changed concurrently");
                Track moved = std::move(sequence->tracks[move.from]);
                sequence->tracks.erase(sequence->tracks.begin() + static_cast<std::ptrdiff_t>(move.from));
                sequence->tracks.insert(sequence->tracks.begin() + static_cast<std::ptrdiff_t>(move.to), std::move(moved));
                return ok();
            },
            [&](const SequencePropertiesChange& change) -> Status {
                Sequence* sequence = mutable_sequence(document, change.sequence);
                if (!sequence || !same_header(*sequence, change.before))
                    return make_error(ErrorCode::conflict, "sequence properties changed concurrently");
                assign_header(*sequence, change.after);
                return ok();
            },
            [&](const SequenceInsert& insert) -> Status {
                if (insert.index > document.sequences.size() || document.sequence_index(insert.sequence.id))
                    return make_error(ErrorCode::invalid_data, "sequence insert is invalid");
                document.sequences.insert(document.sequences.begin() + static_cast<std::ptrdiff_t>(insert.index),
                                          insert.sequence);
                return ok();
            },
            [&](const SequenceRemove& remove) -> Status {
                if (remove.index >= document.sequences.size() || !(document.sequences[remove.index] == remove.sequence))
                    return make_error(ErrorCode::conflict, "sequence to remove no longer matches the document");
                document.sequences.erase(document.sequences.begin() + static_cast<std::ptrdiff_t>(remove.index));
                return ok();
            },
            [&](const DocumentPropertiesChange& change) -> Status {
                if (document.title != change.title_before || document.metadata_json != change.metadata_before)
                    return make_error(ErrorCode::conflict, "document properties changed concurrently");
                document.title = change.title_after;
                document.metadata_json = change.metadata_after;
                return ok();
            },
            [&](const TakeInsert& insert) -> Status {
                if (insert.index > document.takes.size() || !find_track(document, insert.take.track))
                    return make_error(ErrorCode::invalid_data, "take insert is invalid");
                document.takes.insert(document.takes.begin() + static_cast<std::ptrdiff_t>(insert.index), insert.take);
                return ok();
            },
            [&](const TakeRemove& remove) -> Status {
                if (remove.index >= document.takes.size() || !(document.takes[remove.index] == remove.take))
                    return make_error(ErrorCode::conflict, "take to remove no longer matches the document");
                document.takes.erase(document.takes.begin() + static_cast<std::ptrdiff_t>(remove.index));
                return ok();
            },
            [](const auto&) -> Status { return make_error(ErrorCode::internal, "unexpected event operation"); },
        },
        operation);
}

EditOperation invert_operation(const EditOperation& operation) {
    return std::visit(
        Overloaded{
            [](const EventInsert& insert) -> EditOperation { return EventRemove{insert.track, insert.event}; },
            [](const EventRemove& remove) -> EditOperation { return EventInsert{remove.track, remove.event}; },
            [](const EventReplace& replace) -> EditOperation {
                return EventReplace{replace.track, replace.after, replace.before};
            },
            [](const TrackInsert& insert) -> EditOperation {
                return TrackRemove{insert.sequence, insert.index, insert.track};
            },
            [](const TrackRemove& remove) -> EditOperation {
                return TrackInsert{remove.sequence, remove.index, remove.track};
            },
            [](const TrackPropertiesChange& change) -> EditOperation {
                return TrackPropertiesChange{change.track, change.after, change.before};
            },
            [](const TrackMove& move) -> EditOperation { return TrackMove{move.sequence, move.to, move.from, move.track}; },
            [](const SequencePropertiesChange& change) -> EditOperation {
                return SequencePropertiesChange{change.sequence, change.after, change.before};
            },
            [](const SequenceInsert& insert) -> EditOperation { return SequenceRemove{insert.index, insert.sequence}; },
            [](const SequenceRemove& remove) -> EditOperation { return SequenceInsert{remove.index, remove.sequence}; },
            [](const DocumentPropertiesChange& change) -> EditOperation {
                return DocumentPropertiesChange{change.title_after, change.title_before, change.metadata_after,
                                                change.metadata_before};
            },
            [](const TakeInsert& insert) -> EditOperation { return TakeRemove{insert.index, insert.take}; },
            [](const TakeRemove& remove) -> EditOperation { return TakeInsert{remove.index, remove.take}; },
        },
        operation);
}

/// Groups consecutive event operations into runs; returns [begin, end) index pairs.
std::vector<std::pair<std::size_t, std::size_t>> operation_runs(const std::vector<EditOperation>& operations) {
    std::vector<std::pair<std::size_t, std::size_t>> runs;
    std::size_t index = 0;
    while (index < operations.size()) {
        std::size_t end = index + 1;
        if (is_event_operation(operations[index]))
            while (end < operations.size() && is_event_operation(operations[end]))
                ++end;
        runs.emplace_back(index, end);
        index = end;
    }
    return runs;
}

Status apply_run(DocumentData& document, const std::vector<EditOperation>& operations, std::size_t begin,
                 std::size_t end) {
    if (is_event_operation(operations[begin])) {
        std::vector<const EditOperation*> run;
        for (std::size_t index = begin; index < end; ++index)
            run.push_back(&operations[index]);
        return apply_event_batch(document, run);
    }
    return apply_structural(document, operations[begin]);
}

std::size_t event_memory(const Event& event) {
    return sizeof(Event) + (event.payload.size() > Payload::inline_capacity ? event.payload.size() : 0);
}

/// Events including heap payloads (SysEx, long text), so whole-track operations count what
/// they really hold against the undo budget.
std::size_t track_memory(const Track& track) {
    std::size_t total = sizeof(Track) + track.name.size();
    for (const auto& event : track.events)
        total += event_memory(event);
    return total;
}

/// A recording take carries its raw capture evidence (possibly megabytes for long takes).
std::size_t take_memory(const RecordingTake& take) {
    std::size_t total = sizeof(RecordingTake) + take.capture_json.size();
    for (const auto& chunk : take.chunks)
        total += sizeof(chunk) + chunk.payload.size();
    return total;
}

} // namespace

std::size_t ChangeSet::memory_estimate() const {
    std::size_t total = sizeof(ChangeSet) + label.size();
    for (const auto& operation : operations) {
        total += sizeof(EditOperation);
        std::visit(Overloaded{
                       [&](const EventInsert& insert) { total += event_memory(insert.event); },
                       [&](const EventRemove& remove) { total += event_memory(remove.event); },
                       [&](const EventReplace& replace) {
                           total += event_memory(replace.before) + event_memory(replace.after);
                       },
                       [&](const TrackInsert& insert) { total += track_memory(insert.track); },
                       [&](const TrackRemove& remove) { total += track_memory(remove.track); },
                       [&](const SequenceInsert& insert) {
                           for (const auto& track : insert.sequence.tracks)
                               total += track_memory(track);
                       },
                       [&](const SequenceRemove& remove) {
                           for (const auto& track : remove.sequence.tracks)
                               total += track_memory(track);
                       },
                       [&](const TakeInsert& insert) { total += take_memory(insert.take); },
                       [&](const TakeRemove& remove) { total += take_memory(remove.take); },
                       [](const auto&) {},
                   },
                   operation);
    }
    return total;
}

Status apply_change_set(DocumentData& document, const ChangeSet& changes) {
    const auto runs = operation_runs(changes.operations);
    for (std::size_t run_index = 0; run_index < runs.size(); ++run_index) {
        const auto [begin, end] = runs[run_index];
        if (auto status = apply_run(document, changes.operations, begin, end); !status) {
            // Roll back the runs already applied, newest first.
            ChangeSet applied;
            applied.operations.assign(changes.operations.begin(),
                                      changes.operations.begin() + static_cast<std::ptrdiff_t>(begin));
            const ChangeSet undo = invert(applied);
            const auto undo_runs = operation_runs(undo.operations);
            for (const auto& [undo_begin, undo_end] : undo_runs) {
                std::ignore = apply_run(document, undo.operations, undo_begin, undo_end); // inverse of a valid prefix
            }
            return std::unexpected(status.error().with_context(changes.label));
        }
    }
    return ok();
}

ChangeSet invert(const ChangeSet& changes) {
    ChangeSet inverse;
    inverse.label = changes.label;
    inverse.operations.reserve(changes.operations.size());
    for (auto it = changes.operations.rbegin(); it != changes.operations.rend(); ++it)
        inverse.operations.push_back(invert_operation(*it));
    return inverse;
}

std::optional<std::size_t> locate_event(const Track& track, const Event& hint) {
    const auto found = std::lower_bound(track.events.begin(), track.events.end(), hint, order_less);
    if (found != track.events.end() && same_slot(*found, hint) && found->id == hint.id)
        return static_cast<std::size_t>(found - track.events.begin());
    return find_event(track, hint.id);
}

std::optional<std::size_t> find_event(const Track& track, EventId id) {
    for (std::size_t index = 0; index < track.events.size(); ++index)
        if (track.events[index].id == id)
            return index;
    return std::nullopt;
}

// --- ChangeBuilder ------------------------------------------------------------------------------

ChangeBuilder::ChangeBuilder(const DocumentData& document, std::string label)
    : document_(document), label_(std::move(label)) {}

ChangeBuilder::WorkingTrack* ChangeBuilder::touch(TrackId track) {
    for (auto& working : working_)
        if (working.track.id == track)
            return &working;
    const auto location = find_track(document_, track);
    if (!location)
        return nullptr;
    working_.push_back(
        WorkingTrack{*location, document_.sequences[location->sequence].tracks[location->track], {}, {}, false});
    return &working_.back();
}

std::vector<Event>& ChangeBuilder::group(WorkingTrack& working, Tick tick) {
    auto [entry, created] = working.groups.try_emplace(tick);
    if (created) {
        const auto& events = working.track.events;
        const auto lower = std::ranges::lower_bound(events, tick, std::less<>{}, &Event::tick);
        const auto upper = std::ranges::upper_bound(lower, events.end(), tick, std::less<>{}, &Event::tick);
        entry->second.assign(lower, upper);
    }
    return entry->second;
}

std::optional<Tick> ChangeBuilder::tick_of(WorkingTrack& working, EventId id) {
    if (!working.ticks_built) {
        materialize(working);
        working.ticks.reserve(working.track.events.size());
        for (const auto& event : working.track.events)
            working.ticks.emplace(event.id, event.tick);
        working.ticks_built = true;
    }
    const auto found = working.ticks.find(id);
    if (found == working.ticks.end())
        return std::nullopt;
    return found->second;
}

void ChangeBuilder::materialize(WorkingTrack& working) {
    if (working.groups.empty())
        return;
    auto& events = working.track.events;
    std::size_t extra = 0;
    for (const auto& [tick, events_at_tick] : working.groups)
        extra += events_at_tick.size();
    std::vector<Event> merged;
    merged.reserve(events.size() + extra);
    auto group_it = working.groups.begin();
    std::size_t index = 0;
    while (index < events.size() || group_it != working.groups.end()) {
        if (group_it != working.groups.end() && (index == events.size() || group_it->first <= events[index].tick)) {
            // The changed group replaces the track's events at that tick.
            while (index < events.size() && events[index].tick == group_it->first)
                ++index;
            for (auto& event : group_it->second)
                merged.push_back(std::move(event));
            ++group_it;
        } else {
            merged.push_back(std::move(events[index++]));
        }
    }
    events = std::move(merged);
    working.groups.clear();
}

std::uint64_t ChangeBuilder::allocate_order_key(std::vector<Event>& group, OrderPlacement placement,
                                                std::size_t& insert_position) {
    const std::size_t end = group.size();
    std::size_t position = end;
    if (placement == OrderPlacement::before_existing) {
        position = 0;
    } else if (placement == OrderPlacement::before_note_starts) {
        position = 0;
        while (position < end && !group[position].is_note_start())
            ++position;
    }
    const auto key_below = [&](std::size_t at) -> std::uint64_t { return at > 0 ? group[at - 1].order_key : 0; };
    const auto key_above = [&](std::size_t at) -> std::uint64_t {
        return at < end ? group[at].order_key : key_below(at) + 2 * order_key_spacing;
    };
    std::uint64_t below = key_below(position);
    std::uint64_t above = key_above(position);
    if (above - below < 2) {
        // No room: renumber the tick group (a semantic no-op recorded by the diff).
        for (std::size_t index = 0; index < end; ++index)
            group[index].order_key = (index + 1) * order_key_spacing * 2;
        below = key_below(position);
        above = key_above(position);
    }
    insert_position = position;
    if (position == end && end > 0)
        return below + order_key_spacing;
    if (position == end)
        return order_key_spacing;
    return below + (above - below) / 2;
}

EventId ChangeBuilder::insert(TrackId track, Event event, OrderPlacement placement) {
    WorkingTrack* working = touch(track);
    if (!working)
        return EventId{};
    if (event.id.is_nil())
        event.id = EventId::generate();
    auto& events = group(*working, event.tick);
    std::size_t position = 0;
    event.order_key = allocate_order_key(events, placement, position);
    const EventId id = event.id;
    if (working->ticks_built)
        working->ticks.insert_or_assign(id, event.tick);
    events.insert(events.begin() + static_cast<std::ptrdiff_t>(position), std::move(event));
    return id;
}

bool ChangeBuilder::remove(TrackId track, EventId event) {
    WorkingTrack* working = touch(track);
    if (!working)
        return false;
    const auto tick = tick_of(*working, event);
    if (!tick)
        return false;
    auto& events = group(*working, *tick);
    const auto found = std::ranges::find(events, event, &Event::id);
    if (found == events.end())
        return false;
    events.erase(found);
    working->ticks.erase(event);
    return true;
}

bool ChangeBuilder::replace(TrackId track, EventId event, Event updated, OrderPlacement placement) {
    WorkingTrack* working = touch(track);
    if (!working)
        return false;
    const auto tick = tick_of(*working, event);
    if (!tick)
        return false;
    auto& events = group(*working, *tick);
    const auto found = std::ranges::find(events, event, &Event::id);
    if (found == events.end())
        return false;
    updated.id = found->id;
    if (updated.tick == found->tick) {
        updated.order_key = found->order_key;
        *found = std::move(updated);
        return true;
    }
    events.erase(found);
    // Map nodes are stable, so the source group reference above stays valid.
    auto& destination = group(*working, updated.tick);
    std::size_t insert_position = 0;
    updated.order_key = allocate_order_key(destination, placement, insert_position);
    working->ticks.insert_or_assign(updated.id, updated.tick);
    destination.insert(destination.begin() + static_cast<std::ptrdiff_t>(insert_position), std::move(updated));
    return true;
}

EventId ChangeBuilder::add_note(TrackId track, Tick start, Tick end, std::uint8_t channel, std::uint8_t pitch,
                                std::uint8_t velocity, std::uint8_t release_velocity) {
    const EventId id = insert(track, events::note_on(start, channel, pitch, std::max<std::uint8_t>(velocity, 1)),
                              OrderPlacement::after_existing);
    // A zero-length note's end goes after its own start (it would otherwise precede it and
    // never close it, section 16.4); a positive-length end goes before note starts at its tick.
    insert(track, events::note_off(std::max(end, start), channel, pitch, release_velocity),
           end <= start ? OrderPlacement::after_existing : OrderPlacement::before_note_starts);
    return id;
}

const Track* ChangeBuilder::working_track(TrackId track) {
    WorkingTrack* working = touch(track);
    if (!working)
        return nullptr;
    materialize(*working);
    return &working->track;
}

void ChangeBuilder::set_track_properties(TrackId track, const Track& updated) {
    for (auto& [id, header] : track_updates_) {
        if (id == track) {
            header = header_of(updated);
            return;
        }
    }
    track_updates_.emplace_back(track, header_of(updated));
}

void ChangeBuilder::set_sequence_properties(SequenceId sequence, const Sequence& updated) {
    for (auto& [id, header] : sequence_updates_) {
        if (id == sequence) {
            header = header_of(updated);
            return;
        }
    }
    sequence_updates_.emplace_back(sequence, header_of(updated));
}

bool ChangeBuilder::edit_track(TrackId track, BulkTrackEdit edit) {
    WorkingTrack* working = touch(track);
    if (!working)
        return false;
    materialize(*working);
    working->ticks.clear(); // ticks and identities change wholesale; rebuilt on demand
    working->ticks_built = false;
    auto& events = working->track.events;
    std::vector<Event> kept;
    kept.reserve(events.size() + edit.insert.size());
    for (auto& event : events) {
        if (edit.remove.contains(event.id))
            continue;
        if (auto found = edit.replace_in_place.find(event.id); found != edit.replace_in_place.end()) {
            Event updated = std::move(found->second);
            updated.id = event.id;
            updated.tick = event.tick;
            updated.order_key = event.order_key;
            kept.push_back(std::move(updated));
        } else {
            kept.push_back(std::move(event));
        }
    }
    // Insertion order inside a tick (see the header): events placed before existing ones stack
    // in reverse list order, the other placements keep list order.
    struct Pending {
        Event event;
        int bucket;
        std::size_t sequence;
    };
    std::vector<Pending> pending;
    pending.reserve(edit.insert.size());
    for (std::size_t index = 0; index < edit.insert.size(); ++index) {
        auto& [event, placement] = edit.insert[index];
        if (event.id.is_nil())
            event.id = EventId::generate();
        const int bucket = placement == OrderPlacement::before_existing      ? 0
                           : placement == OrderPlacement::before_note_starts ? 1
                                                                             : 2;
        pending.push_back(Pending{std::move(event), bucket, bucket == 0 ? edit.insert.size() - index : index});
    }
    std::sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) {
        return std::tie(a.event.tick, a.bucket, a.sequence) < std::tie(b.event.tick, b.bucket, b.sequence);
    });

    std::vector<Event> merged;
    merged.reserve(kept.size() + pending.size());
    std::vector<bool> added; // per event of the current tick group
    std::size_t k = 0;
    std::size_t p = 0;
    while (k < kept.size() || p < pending.size()) {
        const Tick tick = k == kept.size()      ? pending[p].event.tick
                          : p == pending.size() ? kept[k].tick
                                                : std::min(kept[k].tick, pending[p].event.tick);
        std::size_t k_end = k;
        while (k_end < kept.size() && kept[k_end].tick == tick)
            ++k_end;
        std::size_t p_end = p;
        while (p_end < pending.size() && pending[p_end].event.tick == tick)
            ++p_end;
        if (p == p_end) {
            for (; k < k_end; ++k)
                merged.push_back(std::move(kept[k]));
            continue;
        }
        const std::size_t group_begin = merged.size();
        added.clear();
        const auto take_pending = [&](int bucket) {
            for (; p < p_end && pending[p].bucket == bucket; ++p) {
                merged.push_back(std::move(pending[p].event));
                added.push_back(true);
            }
        };
        const auto take_kept = [&](std::size_t end) {
            for (; k < end; ++k) {
                merged.push_back(std::move(kept[k]));
                added.push_back(false);
            }
        };
        std::size_t first_start = k;
        while (first_start < k_end && !kept[first_start].is_note_start())
            ++first_start;
        take_pending(0);
        take_kept(first_start);
        take_pending(1);
        take_kept(k_end);
        take_pending(2);
        // Order keys for inserted runs between their existing neighbours; renumber the group
        // only when a gap is too small (a semantic no-op recorded by the diff).
        const std::size_t group_size = merged.size() - group_begin;
        bool renumber = false;
        for (std::size_t run = 0; run < group_size && !renumber;) {
            if (!added[run]) {
                ++run;
                continue;
            }
            std::size_t run_end = run;
            while (run_end < group_size && added[run_end])
                ++run_end;
            const std::uint64_t below = run > 0 ? merged[group_begin + run - 1].order_key : 0;
            const std::size_t count = run_end - run;
            if (run_end == group_size) {
                for (std::size_t index = 0; index < count; ++index)
                    merged[group_begin + run + index].order_key = below + (index + 1) * order_key_spacing;
            } else {
                const std::uint64_t above = merged[group_begin + run_end].order_key;
                const std::uint64_t step = above > below ? (above - below) / (count + 1) : 0;
                if (step == 0)
                    renumber = true;
                else
                    for (std::size_t index = 0; index < count; ++index)
                        merged[group_begin + run + index].order_key = below + (index + 1) * step;
            }
            run = run_end;
        }
        if (renumber)
            for (std::size_t index = 0; index < group_size; ++index)
                merged[group_begin + index].order_key = (index + 1) * order_key_spacing;
    }
    events = std::move(merged);
    return true;
}

void ChangeBuilder::set_events(TrackId track, std::vector<Event> events) {
    WorkingTrack* working = touch(track);
    if (!working)
        return;
    for (auto& event : events)
        if (event.id.is_nil())
            event.id = EventId::generate();
    std::stable_sort(events.begin(), events.end(), order_less);
    working->groups.clear();
    working->ticks.clear();
    working->ticks_built = false;
    // Renumber only tick groups that contain duplicate order positions.
    std::size_t begin = 0;
    while (begin < events.size()) {
        std::size_t end = begin + 1;
        bool duplicate = false;
        while (end < events.size() && events[end].tick == events[begin].tick) {
            if (events[end].order_key == events[end - 1].order_key)
                duplicate = true;
            ++end;
        }
        if (duplicate)
            for (std::size_t index = begin; index < end; ++index)
                events[index].order_key = (index - begin + 1) * order_key_spacing;
        begin = end;
    }
    working->track.events = std::move(events);
}

void ChangeBuilder::append(EditOperation operation) { explicit_operations_.push_back(std::move(operation)); }

ChangeSet ChangeBuilder::finish() {
    for (auto& working : working_)
        materialize(working);
    ChangeSet changes;
    changes.label = label_;
    changes.operations = std::move(explicit_operations_);

    for (const auto& [id, header] : sequence_updates_) {
        const auto index = document_.sequence_index(id);
        if (!index)
            continue;
        const Sequence before = header_of(document_.sequences[*index]);
        if (!same_header(before, header))
            changes.operations.push_back(SequencePropertiesChange{id, before, header});
    }

    // Track property changes, including end-tick extension to cover edited events.
    std::vector<std::pair<TrackId, Track>> headers = track_updates_;
    for (const auto& working : working_) {
        const Track& original = document_.sequences[working.location.sequence].tracks[working.location.track];
        Track* header = nullptr;
        for (auto& [id, candidate] : headers)
            if (id == working.track.id)
                header = &candidate;
        const Tick needed = working.track.last_event_tick();
        const Tick current_end = header ? header->end_tick : original.end_tick;
        if (needed > current_end) {
            if (!header) {
                headers.emplace_back(working.track.id, header_of(original));
                header = &headers.back().second;
            }
            header->end_tick = needed;
        }
    }
    for (const auto& [id, header] : headers) {
        const auto location = find_track(document_, id);
        if (!location)
            continue;
        const Track before = header_of(document_.sequences[location->sequence].tracks[location->track]);
        if (!same_header(before, header))
            changes.operations.push_back(TrackPropertiesChange{id, before, header});
    }

    // Event diffs by identity: removals, replacements, insertions.
    for (const auto& working : working_) {
        const Track& original = document_.sequences[working.location.sequence].tracks[working.location.track];
        std::unordered_map<EventId, std::size_t, UuidHash> original_index;
        original_index.reserve(original.events.size());
        for (std::size_t index = 0; index < original.events.size(); ++index)
            original_index.emplace(original.events[index].id, index);
        std::unordered_set<EventId, UuidHash> kept;
        kept.reserve(working.track.events.size());
        std::vector<EditOperation> inserts;
        for (const auto& event : working.track.events) {
            kept.insert(event.id);
            const auto found = original_index.find(event.id);
            if (found == original_index.end()) {
                inserts.push_back(EventInsert{working.track.id, event});
            } else if (!(original.events[found->second] == event)) {
                changes.operations.push_back(EventReplace{working.track.id, original.events[found->second], event});
            }
        }
        for (const auto& event : original.events)
            if (!kept.contains(event.id))
                changes.operations.push_back(EventRemove{working.track.id, event});
        for (auto& insert : inserts)
            changes.operations.push_back(std::move(insert));
    }
    return changes;
}

} // namespace mwb
