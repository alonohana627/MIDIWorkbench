#include "mwb/music/note_edit.hpp"

#include "mwb/core/checked.hpp"
#include "mwb/music/json_fields.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <unordered_set>

namespace mwb::music {

namespace {

/// Clamps a value into a MIDI data byte.
std::uint8_t to_byte(int value) { return static_cast<std::uint8_t>(std::clamp(value, 0, 127)); }

struct CollisionKey {
    std::size_t track = 0;
    std::uint8_t channel = 0;
    int pitch = 0;
    friend auto operator<=>(const CollisionKey&, const CollisionKey&) = default;
};

/// Adjacent overlaps (or equal onsets) among spans of one key.
std::size_t count_overlaps(std::vector<std::pair<Tick, Tick>> spans) {
    std::ranges::sort(spans);
    std::size_t overlaps = 0;
    for (std::size_t index = 1; index < spans.size(); ++index)
        if (spans[index].first < spans[index - 1].second || spans[index].first == spans[index - 1].first)
            ++overlaps;
    return overlaps;
}

} // namespace

Result<NoteEditor> NoteEditor::open(const DocumentData& document, SequenceId sequence) {
    const auto index = document.sequence_index(sequence);
    if (!index)
        return make_error(ErrorCode::not_found, "the target sequence does not exist", sequence.to_string());
    NoteEditor editor;
    editor.document_ = &document;
    editor.sequence_index_ = *index;
    const Sequence& data = document.sequences[*index];
    editor.pairing_ = pair_notes(data);
    for (std::size_t track = 0; track < data.tracks.size(); ++track)
        for (std::size_t event = 0; event < data.tracks[track].events.size(); ++event)
            editor.locations_.emplace(data.tracks[track].events[event].id, EventLocation{track, event});
    for (std::size_t note = 0; note < editor.pairing_.notes.size(); ++note)
        editor.note_index_.emplace(editor.pairing_.notes[note].on_id, note);
    editor.note_touched_.assign(editor.pairing_.notes.size(), false);
    editor.note_removed_.assign(editor.pairing_.notes.size(), false);
    return editor;
}

std::int32_t NoteEditor::ppq() const noexcept {
    const auto& division = sequence().division;
    return division.is_ppq() ? division.ppq : 0;
}

Result<std::vector<std::size_t>> NoteEditor::select(const TargetSelection& selection) const {
    if (selection.sequence != sequence().id)
        return make_error(ErrorCode::invalid_argument, "selection names a different sequence");
    std::set<std::size_t> tracks;
    for (const auto& track : selection.tracks) {
        const auto index = sequence().track_index(track);
        if (!index)
            return make_error(ErrorCode::not_found, "selected track is not in the sequence", track.to_string());
        tracks.insert(*index);
    }
    std::unordered_set<EventId, UuidHash> chosen;
    for (const auto& note : selection.notes) {
        if (!note_index_.contains(note))
            return make_error(ErrorCode::not_found, "selected note does not exist", note.to_string());
        chosen.insert(note);
    }
    if (selection.range && selection.range->end < selection.range->begin)
        return make_error(ErrorCode::invalid_argument, "selection range ends before it begins");
    std::vector<std::size_t> result;
    for (std::size_t index = 0; index < pairing_.notes.size(); ++index) {
        const auto& note = pairing_.notes[index];
        if (!tracks.empty() && !tracks.contains(note.on_track))
            continue;
        if (!chosen.empty() && !chosen.contains(note.on_id))
            continue;
        if (selection.range && !selection.range->contains(note.start))
            continue;
        result.push_back(index);
    }
    std::ranges::stable_sort(result, [this](std::size_t a, std::size_t b) {
        const auto& left = pairing_.notes[a];
        const auto& right = pairing_.notes[b];
        return std::tie(left.start, left.on_track, left.pitch) < std::tie(right.start, right.on_track, right.pitch);
    });
    return result;
}

std::vector<NoteEditor::WorkingEvent>& NoteEditor::working(std::size_t track_index) {
    auto found = working_.find(track_index);
    if (found != working_.end())
        return found->second;
    std::vector<WorkingEvent> copy;
    const auto& events = sequence().tracks[track_index].events;
    copy.reserve(events.size());
    for (const auto& event : events)
        copy.push_back(WorkingEvent{event, false, false, event.tick, event.order_key, 0});
    return working_.emplace(track_index, std::move(copy)).first->second;
}

NoteEditor::WorkingEvent* NoteEditor::working_event(EventId id) {
    const auto found = locations_.find(id);
    if (found == locations_.end())
        return nullptr;
    return &working(found->second.track)[found->second.index];
}

const Event* NoteEditor::current_event(EventId id) const {
    const auto found = locations_.find(id);
    if (found == locations_.end())
        return nullptr;
    const auto track = working_.find(found->second.track);
    if (track != working_.end())
        return &track->second[found->second.index].event;
    return &sequence().tracks[found->second.track].events[found->second.index];
}

NoteValues NoteEditor::original_values(std::size_t note) const {
    const auto& span = pairing_.notes[note];
    return NoteValues{span.start, span.end, span.pitch, span.velocity, span.release_velocity, span.channel};
}

NoteValues NoteEditor::values(std::size_t note) const {
    const auto& span = pairing_.notes[note];
    NoteValues result = original_values(note);
    if (const Event* on = current_event(span.on_id)) {
        result.start = on->tick;
        result.pitch = on->data1();
        result.velocity = on->data2();
        result.channel = on->channel();
    }
    if (span.off_id) {
        if (const Event* off = current_event(*span.off_id)) {
            result.end = off->tick;
            result.release_velocity = off->message() == ChannelMessage::note_off ? off->data2() : 0;
        }
    } else {
        result.end = std::max(result.start, span.end);
    }
    return result;
}

bool NoteEditor::is_unterminated(std::size_t note) const { return !pairing_.notes[note].off_id.has_value(); }

bool NoteEditor::has_release_velocity(std::size_t note) const {
    const auto& span = pairing_.notes[note];
    if (!span.off_id)
        return false;
    const Event* off = current_event(*span.off_id);
    return off && off->message() == ChannelMessage::note_off;
}

void NoteEditor::update(std::size_t note, const NoteValues& values) {
    const auto& span = pairing_.notes[note];
    note_touched_[note] = true;
    if (WorkingEvent* on = working_event(span.on_id)) {
        Event updated = on->event;
        updated.tick = values.start;
        updated.code = static_cast<std::uint8_t>(0x90 | (values.channel & 0x0F));
        updated.payload = Payload{to_byte(values.pitch), static_cast<std::uint8_t>(std::clamp(values.velocity, 1, 127))};
        if (!(updated == on->event)) {
            on->event = std::move(updated);
            on->changed = true;
        }
    }
    if (span.off_id) {
        if (WorkingEvent* off = working_event(*span.off_id)) {
            Event updated = off->event;
            const bool note_off = off->event.message() == ChannelMessage::note_off;
            updated.tick = values.end;
            updated.code = static_cast<std::uint8_t>((note_off ? 0x80 : 0x90) | (values.channel & 0x0F));
            updated.payload = Payload{to_byte(values.pitch), note_off ? to_byte(values.release_velocity) : std::uint8_t{0}};
            if (!(updated == off->event)) {
                off->event = std::move(updated);
                off->changed = true;
            }
        }
    }
}

void NoteEditor::remove(std::size_t note) {
    const auto& span = pairing_.notes[note];
    note_touched_[note] = true;
    note_removed_[note] = true;
    if (WorkingEvent* on = working_event(span.on_id))
        on->removed = true;
    if (span.off_id)
        if (WorkingEvent* off = working_event(*span.off_id))
            off->removed = true;
}

EventId NoteEditor::add(std::size_t track_index, const NoteValues& values) {
    auto& events = working(track_index);
    Event on = events::note_on(values.start, values.channel, to_byte(values.pitch),
                               static_cast<std::uint8_t>(std::clamp(values.velocity, 1, 127)));
    Event off = events::note_off(std::max(values.end, values.start), values.channel, to_byte(values.pitch),
                                 to_byte(values.release_velocity));
    const EventId id = on.id;
    events.push_back(WorkingEvent{std::move(on), true, false, values.start, 0, ++creation_counter_});
    events.push_back(WorkingEvent{std::move(off), true, false, values.end, 0, ++creation_counter_});
    added_notes_.emplace_back(track_index, values);
    return id;
}

void NoteEditor::retime_event(std::size_t track_index, std::size_t event_index, Tick tick) {
    auto& event = working(track_index)[event_index];
    if (event.event.tick != tick) {
        event.event.tick = tick;
        event.changed = true;
    }
}

std::size_t NoteEditor::new_collisions() const {
    std::set<CollisionKey> keys;
    for (std::size_t note = 0; note < pairing_.notes.size(); ++note) {
        if (!note_touched_[note])
            continue;
        const auto before = original_values(note);
        const auto after = values(note);
        keys.insert({pairing_.notes[note].on_track, before.channel, before.pitch});
        keys.insert({pairing_.notes[note].on_track, after.channel, after.pitch});
    }
    for (const auto& [track, values] : added_notes_)
        keys.insert({track, values.channel, values.pitch});
    std::map<CollisionKey, std::vector<std::pair<Tick, Tick>>> before_spans;
    std::map<CollisionKey, std::vector<std::pair<Tick, Tick>>> after_spans;
    for (std::size_t note = 0; note < pairing_.notes.size(); ++note) {
        const auto track = pairing_.notes[note].on_track;
        const auto before = original_values(note);
        const CollisionKey before_key{track, before.channel, before.pitch};
        if (keys.contains(before_key))
            before_spans[before_key].emplace_back(before.start, before.end);
        if (note_removed_[note])
            continue;
        const auto after = values(note);
        const CollisionKey after_key{track, after.channel, after.pitch};
        if (keys.contains(after_key))
            after_spans[after_key].emplace_back(after.start, after.end);
    }
    for (const auto& [track, values] : added_notes_)
        after_spans[{track, values.channel, values.pitch}].emplace_back(values.start, values.end);
    std::size_t created = 0;
    for (const auto& key : keys) {
        const auto before = count_overlaps(before_spans[key]);
        const auto after = count_overlaps(after_spans[key]);
        if (after > before)
            created += after - before;
    }
    return created;
}

ChangeSet NoteEditor::finish(std::string label) {
    ChangeBuilder builder(*document_, std::move(label));
    std::vector<std::size_t> tracks;
    for (const auto& [track, events] : working_)
        tracks.push_back(track);
    std::ranges::sort(tracks);
    for (const auto track : tracks) {
        auto& events = working_.at(track);
        std::vector<WorkingEvent*> kept;
        bool any_change = false;
        for (auto& event : events) {
            if (event.removed) {
                any_change = true;
                continue;
            }
            any_change = any_change || event.changed;
            kept.push_back(&event);
        }
        if (!any_change)
            continue;
        const auto rank = [this](const WorkingEvent& event) {
            if (!event.changed || chronological_)
                return 1;
            if (event.event.is_note_end())
                return 0;
            if (event.event.is_note_start())
                return 2;
            return 1;
        };
        std::ranges::stable_sort(kept, [&](const WorkingEvent* a, const WorkingEvent* b) {
            return std::make_tuple(a->event.tick, rank(*a), a->original_tick, a->original_order, a->creation) <
                   std::make_tuple(b->event.tick, rank(*b), b->original_tick, b->original_order, b->creation);
        });
        std::vector<Event> result;
        result.reserve(kept.size());
        std::size_t begin = 0;
        while (begin < kept.size()) {
            std::size_t end = begin;
            // Existing keys are kept whenever they already express the chosen order, so edits
            // that do not reorder (transposition, velocity) leave order keys untouched.
            bool renumber = false;
            while (end < kept.size() && kept[end]->event.tick == kept[begin]->event.tick) {
                renumber = renumber || kept[end]->creation != 0 ||
                           (end > begin && kept[end]->event.order_key <= kept[end - 1]->event.order_key);
                ++end;
            }
            for (std::size_t index = begin; index < end; ++index) {
                Event event = kept[index]->event;
                if (renumber)
                    event.order_key = (index - begin + 1) * order_key_spacing;
                result.push_back(std::move(event));
            }
            begin = end;
        }
        builder.set_events(sequence().tracks[track].id, std::move(result));
    }
    for (auto& [track, header] : track_headers_)
        builder.set_track_properties(sequence().tracks[track].id, header);
    if (sequence_header_)
        builder.set_sequence_properties(sequence().id, *sequence_header_);
    return builder.finish();
}

void NoteEditor::set_track_header(std::size_t track_index, Track header) {
    header.events.clear();
    for (auto& [track, existing] : track_headers_) {
        if (track == track_index) {
            existing = std::move(header);
            return;
        }
    }
    track_headers_.emplace_back(track_index, std::move(header));
}

void NoteEditor::set_sequence_header(Sequence header) {
    header.tracks.clear();
    sequence_header_ = std::move(header);
}

std::string selection_to_json(const TargetSelection& selection) {
    nlohmann::json root;
    root["version"] = 1;
    root["sequence"] = selection.sequence.to_string();
    nlohmann::json tracks = nlohmann::json::array();
    for (const auto& track : selection.tracks)
        tracks.push_back(track.to_string());
    root["tracks"] = std::move(tracks);
    nlohmann::json notes = nlohmann::json::array();
    for (const auto& note : selection.notes)
        notes.push_back(note.to_string());
    root["notes"] = std::move(notes);
    if (selection.range)
        root["range"] = nlohmann::json{{"begin", selection.range->begin}, {"end", selection.range->end}};
    else
        root["range"] = nullptr;
    return root.dump();
}

Result<TargetSelection> selection_from_json(std::string_view json) {
    auto parsed = parse_json_object(json);
    if (!parsed)
        return std::unexpected(parsed.error());
    FieldReader reader(*parsed, "targets");
    int version = 0;
    reader.integer("version", version);
    MWB_TRY(reader.status());
    if (version != 1)
        return make_error(ErrorCode::unsupported, "unsupported selection version");
    TargetSelection selection;
    std::string sequence;
    reader.text("sequence", sequence);
    MWB_TRY(reader.status());
    auto sequence_id = Uuid::parse(sequence);
    if (!sequence_id)
        return std::unexpected(sequence_id.error());
    selection.sequence = SequenceId(*sequence_id);
    const auto read_ids = [&](std::string_view key, auto& out) -> Status {
        const auto* list = reader.array(key);
        MWB_TRY(reader.status());
        if (!list)
            return ok();
        for (const auto& item : *list) {
            if (!item.is_string())
                return make_error(ErrorCode::invalid_data, "selection identifiers must be strings");
            auto id = Uuid::parse(item.get<std::string>());
            if (!id)
                return std::unexpected(id.error());
            out.emplace_back(*id);
        }
        return ok();
    };
    MWB_TRY(read_ids("tracks", selection.tracks));
    MWB_TRY(read_ids("notes", selection.notes));
    const auto range = parsed->find("range");
    if (range != parsed->end() && !range->is_null()) {
        FieldReader range_reader(*range, "targets.range");
        TimeRange value;
        range_reader.integer("begin", value.begin);
        range_reader.integer("end", value.end);
        MWB_TRY(range_reader.status());
        selection.range = value;
    }
    return selection;
}

} // namespace mwb::music

namespace mwb::music {

Tick default_onset_tolerance(std::int32_t ppq) noexcept {
    if (ppq <= 0)
        return 0;
    return mul_div_round(ppq, 1, 96).value_or(0);
}

std::vector<std::vector<std::size_t>> onset_groups(const NoteEditor& editor, const std::vector<std::size_t>& notes,
                                                   Tick tolerance) {
    std::map<std::size_t, std::vector<std::size_t>> by_track;
    for (const auto note : notes)
        by_track[editor.notes()[note].on_track].push_back(note);
    std::vector<std::vector<std::size_t>> groups;
    for (auto& [track, members] : by_track) {
        std::ranges::stable_sort(members, [&](std::size_t a, std::size_t b) {
            return editor.values(a).start < editor.values(b).start;
        });
        Tick group_start = 0;
        for (const auto note : members) {
            const Tick start = editor.values(note).start;
            if (groups.empty() || editor.notes()[groups.back().front()].on_track != track ||
                start - group_start > tolerance) {
                groups.emplace_back();
                group_start = start;
            }
            groups.back().push_back(note);
        }
    }
    std::ranges::stable_sort(groups, [&](const std::vector<std::size_t>& a, const std::vector<std::size_t>& b) {
        return std::make_tuple(editor.values(a.front()).start, editor.notes()[a.front()].on_track) <
               std::make_tuple(editor.values(b.front()).start, editor.notes()[b.front()].on_track);
    });
    return groups;
}

TransformPreview finish_preview(NoteEditor& editor, TransformReport report, std::string label) {
    report.collisions += editor.new_collisions();
    TransformPreview preview;
    preview.report = std::move(report);
    preview.changes = editor.finish(std::move(label));
    return preview;
}

} // namespace mwb::music

namespace mwb::music {

Result<PreparedTransform> prepare_transform(const DocumentData& document, const TargetSelection& selection,
                                            std::string_view tool, bool require_ppq) {
    auto editor = NoteEditor::open(document, selection.sequence);
    if (!editor)
        return std::unexpected(editor.error());
    if (require_ppq && editor->ppq() <= 0)
        return make_error(ErrorCode::unsupported,
                          "this tool needs a PPQ sequence; convert the SMPTE sequence explicitly first",
                          std::string(tool));
    auto selected = editor->select(selection);
    if (!selected)
        return std::unexpected(selected.error());
    TransformReport report;
    report.tool = std::string(tool);
    report.targeted = selected->size();
    return PreparedTransform{std::move(*editor), std::move(*selected), std::move(report)};
}

} // namespace mwb::music
