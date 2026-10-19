#include "mwb/music/note_edit.hpp"

#include "mwb/core/checked.hpp"
#include "mwb/music/json_fields.hpp"

#include <algorithm>
#include <bitset>
#include <functional>
#include <map>
#include <memory>
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
    return open_with(document, sequence, std::make_shared<const NotePairing>(pair_notes(document.sequences[*index])),
                     false);
}

Result<NoteEditor> NoteEditor::open(const DocumentData& document, SequenceId sequence, const NotePairing& pairing) {
    // Borrowed: an aliasing pointer that owns nothing.
    return open_with(document, sequence, std::shared_ptr<const NotePairing>(std::shared_ptr<void>(), &pairing), true);
}

Result<NoteEditor> NoteEditor::open_with(const DocumentData& document, SequenceId sequence,
                                         std::shared_ptr<const NotePairing> pairing, bool borrowed) {
    const auto index = document.sequence_index(sequence);
    if (!index)
        return make_error(ErrorCode::not_found, "the target sequence does not exist", sequence.to_string());
    NoteEditor editor;
    editor.document_ = &document;
    editor.sequence_index_ = *index;
    editor.pairing_ = std::move(pairing);
    editor.borrowed_pairing_ = borrowed;
    const auto& tracks = document.sequences[*index].tracks;
    for (const auto& note : editor.pairing_->notes)
        if (note.on_track >= tracks.size() || note.off_track >= tracks.size())
            return make_error(ErrorCode::conflict, "the note pairing does not match the sequence");
    editor.note_touched_.assign(editor.pairing_->notes.size(), false);
    editor.note_removed_.assign(editor.pairing_->notes.size(), false);
    return editor;
}

std::int32_t NoteEditor::ppq() const noexcept {
    const auto& division = sequence().division;
    return division.is_ppq() ? division.ppq : 0;
}

Result<std::vector<std::size_t>> NoteEditor::select(const TargetSelection& selection) const {
    if (selection.sequence != sequence().id)
        return make_error(ErrorCode::invalid_argument, "selection names a different sequence");
    std::vector<bool> tracks(sequence().tracks.size(), false);
    for (const auto& track : selection.tracks) {
        const auto index = sequence().track_index(track);
        if (!index)
            return make_error(ErrorCode::not_found, "selected track is not in the sequence", track.to_string());
        tracks[*index] = true;
    }
    if (selection.range && selection.range->end < selection.range->begin)
        return make_error(ErrorCode::invalid_argument, "selection range ends before it begins");
    // One linear pass over the pairing; small explicit selections (an arrow-key nudge) compare
    // identities directly instead of hashing every note of the sequence.
    const std::unordered_set<EventId, UuidHash> chosen(selection.notes.begin(), selection.notes.end());
    const bool few = chosen.size() <= 16;
    const auto is_chosen = [&](EventId id) {
        return few ? std::ranges::find(selection.notes, id) != selection.notes.end() : chosen.contains(id);
    };
    const auto& notes = pairing_->notes;
    std::size_t matched = 0;
    std::vector<std::size_t> result;
    for (std::size_t index = 0; index < notes.size(); ++index) {
        const auto& note = notes[index];
        if (!chosen.empty()) {
            if (!is_chosen(note.on_id))
                continue;
            ++matched;
        }
        if (!selection.tracks.empty() && !tracks[note.on_track])
            continue;
        if (selection.range && !selection.range->contains(note.start))
            continue;
        result.push_back(index);
    }
    if (matched != chosen.size()) {
        std::unordered_set<EventId, UuidHash> found;
        for (const auto& note : notes)
            if (chosen.contains(note.on_id))
                found.insert(note.on_id);
        for (const auto& id : selection.notes)
            if (!found.contains(id))
                return make_error(ErrorCode::not_found, "selected note does not exist", id.to_string());
    }
    if (borrowed_pairing_) {
        // A borrowed pairing must describe this document state: check every selected note.
        for (const auto index : result) {
            const auto& note = notes[index];
            if (!original_position(note.on_track, note.on_id, note.start) ||
                (note.off_id && !original_position(note.off_track, *note.off_id, note.end)))
                return make_error(ErrorCode::conflict, "the cached note pairing is out of date");
        }
    }
    std::ranges::stable_sort(result, [&](std::size_t a, std::size_t b) {
        const auto& left = notes[a];
        const auto& right = notes[b];
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
    copy.reserve(events.size() + 16);
    for (const auto& event : events)
        copy.push_back(WorkingEvent{event, false, false, event.tick, event.order_key, 0});
    return working_.emplace(track_index, std::move(copy)).first->second;
}

std::optional<std::size_t> NoteEditor::original_position(std::size_t track_index, EventId id, Tick tick) const {
    const auto& events = sequence().tracks[track_index].events;
    auto it = std::ranges::lower_bound(events, tick, std::less<>{}, &Event::tick);
    for (; it != events.end() && it->tick == tick; ++it)
        if (it->id == id)
            return static_cast<std::size_t>(it - events.begin());
    return std::nullopt;
}

NoteEditor::NotePositions NoteEditor::positions(std::size_t note) const {
    if (const auto cached = note_positions_.find(note); cached != note_positions_.end())
        return cached->second;
    const auto& span = pairing_->notes[note];
    NotePositions result;
    result.on = original_position(span.on_track, span.on_id, span.start);
    if (span.off_id)
        result.off = original_position(span.off_track, *span.off_id, span.end);
    return result;
}

const Event* NoteEditor::current_event(std::size_t track_index, std::optional<std::size_t> position) const {
    if (!position)
        return nullptr;
    const auto track = working_.find(track_index);
    if (track != working_.end())
        return &track->second[*position].event;
    return &sequence().tracks[track_index].events[*position];
}

NoteValues NoteEditor::original_values(std::size_t note) const {
    const auto& span = pairing_->notes[note];
    return NoteValues{span.start, span.end, span.pitch, span.velocity, span.release_velocity, span.channel};
}

NoteValues NoteEditor::values(std::size_t note) const {
    // Only update/remove (touched notes) and retime_event change note events, so untouched notes
    // are answered from the pairing without any event lookup.
    if (!note_touched_[note] && !retimed_)
        return original_values(note);
    const auto& span = pairing_->notes[note];
    NoteValues result = original_values(note);
    const auto where = positions(note);
    if (const Event* on = current_event(span.on_track, where.on)) {
        result.start = on->tick;
        result.pitch = on->data1();
        result.velocity = on->data2();
        result.channel = on->channel();
    }
    if (span.off_id) {
        if (const Event* off = current_event(span.off_track, where.off)) {
            result.end = off->tick;
            result.release_velocity = off->message() == ChannelMessage::note_off ? off->data2() : 0;
        }
    } else {
        result.end = std::max(result.start, span.end);
    }
    return result;
}

bool NoteEditor::is_unterminated(std::size_t note) const { return !pairing_->notes[note].off_id.has_value(); }

bool NoteEditor::has_release_velocity(std::size_t note) const {
    const auto& span = pairing_->notes[note];
    if (!span.off_id)
        return false;
    const Event* off = current_event(span.off_track, positions(note).off);
    return off && off->message() == ChannelMessage::note_off;
}

void NoteEditor::touch_note(std::size_t note) {
    if (note_touched_[note])
        return;
    note_positions_.emplace(note, positions(note)); // positions never change: working copies keep indices
    note_touched_[note] = true;
    touched_notes_.push_back(note);
}

void NoteEditor::update(std::size_t note, const NoteValues& values) {
    const auto& span = pairing_->notes[note];
    touch_note(note);
    const auto where = positions(note);
    if (where.on) {
        WorkingEvent& on = working(span.on_track)[*where.on];
        Event updated = on.event;
        updated.tick = values.start;
        updated.code = static_cast<std::uint8_t>(0x90 | (values.channel & 0x0F));
        updated.payload = Payload{to_byte(values.pitch), static_cast<std::uint8_t>(std::clamp(values.velocity, 1, 127))};
        if (!(updated == on.event)) {
            on.event = std::move(updated);
            on.changed = true;
        }
    }
    if (span.off_id && where.off) {
        WorkingEvent& off = working(span.off_track)[*where.off];
        Event updated = off.event;
        const bool note_off = off.event.message() == ChannelMessage::note_off;
        updated.tick = values.end;
        updated.code = static_cast<std::uint8_t>((note_off ? 0x80 : 0x90) | (values.channel & 0x0F));
        updated.payload = Payload{to_byte(values.pitch), note_off ? to_byte(values.release_velocity) : std::uint8_t{0}};
        if (!(updated == off.event)) {
            off.event = std::move(updated);
            off.changed = true;
        }
    }
}

void NoteEditor::remove(std::size_t note) {
    const auto& span = pairing_->notes[note];
    touch_note(note);
    note_removed_[note] = true;
    const auto where = positions(note);
    if (where.on)
        working(span.on_track)[*where.on].removed = true;
    if (span.off_id && where.off)
        working(span.off_track)[*where.off].removed = true;
}

EventId NoteEditor::add(std::size_t track_index, const NoteValues& values) {
    auto& events = working(track_index);
    Event on = events::note_on(values.start, values.channel, to_byte(values.pitch),
                               static_cast<std::uint8_t>(std::clamp(values.velocity, 1, 127)));
    Event off = events::note_off(std::max(values.end, values.start), values.channel, to_byte(values.pitch),
                                 to_byte(values.release_velocity));
    const EventId id = on.id;
    if (values.end <= values.start)
        zero_length_added_ends_.insert(off.id);
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
        retimed_ = true;
    }
}

std::size_t NoteEditor::new_collisions() const {
    // Keys (track, channel, pitch) touched by this session; only notes of those keys can gain
    // overlaps, and a per-track bit set keeps the scan over the pairing a cheap linear pass.
    constexpr std::size_t key_count = 16 * 128;
    const auto key_bit = [](std::uint8_t channel, int pitch) {
        return static_cast<std::size_t>(channel & 0x0F) * 128 + static_cast<std::size_t>(std::clamp(pitch, 0, 127));
    };
    std::set<CollisionKey> keys;
    const auto& notes = pairing_->notes;
    for (const auto note : touched_notes_) {
        const auto before = original_values(note);
        const auto after = values(note);
        keys.insert({notes[note].on_track, before.channel, before.pitch});
        keys.insert({notes[note].on_track, after.channel, after.pitch});
    }
    for (const auto& [track, values] : added_notes_)
        keys.insert({track, values.channel, values.pitch});
    if (keys.empty())
        return 0;
    std::vector<std::unique_ptr<std::bitset<key_count>>> bits(sequence().tracks.size());
    for (const auto& key : keys) {
        auto& slot = bits[key.track];
        if (!slot)
            slot = std::make_unique<std::bitset<key_count>>();
        slot->set(key_bit(key.channel, key.pitch));
    }
    std::map<CollisionKey, std::vector<std::pair<Tick, Tick>>> before_spans;
    std::map<CollisionKey, std::vector<std::pair<Tick, Tick>>> after_spans;
    for (std::size_t note = 0; note < notes.size(); ++note) {
        const auto track = notes[note].on_track;
        const auto* slot = bits[track].get();
        if (!slot)
            continue;
        const auto before = original_values(note);
        if (slot->test(key_bit(before.channel, before.pitch)))
            before_spans[{track, before.channel, before.pitch}].emplace_back(before.start, before.end);
        if (note_removed_[note])
            continue;
        const auto after = values(note);
        if (slot->test(key_bit(after.channel, after.pitch)))
            after_spans[{track, after.channel, after.pitch}].emplace_back(after.start, after.end);
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
    // Ends of edited notes that are zero-length afterwards: they must stay after their own start
    // at the shared tick, otherwise FIFO pairing meets the end first (an orphan end, or one that
    // closes another sounding note) and the moved note never ends (section 16.4).
    std::unordered_set<EventId, UuidHash> zero_length_ends = zero_length_added_ends_;
    for (const auto note : touched_notes_) {
        const auto& span = pairing_->notes[note];
        if (note_removed_[note] || !span.off_id)
            continue;
        const auto current = values(note);
        if (current.end == current.start)
            zero_length_ends.insert(*span.off_id);
    }
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
        // Rank inside a tick: changed note ends, unchanged events, changed note starts, then the
        // ends of edited zero-length notes (after their own start whether or not either changed).
        const auto rank = [&](const WorkingEvent& event) {
            if (chronological_)
                return 1;
            if (!zero_length_ends.empty() && event.event.is_note_end() && zero_length_ends.contains(event.event.id))
                return 3;
            if (!event.changed)
                return 1;
            if (event.event.is_note_end())
                return 0;
            if (event.event.is_note_start())
                return 2;
            return 1;
        };
        for (auto* event : kept)
            event->rank = rank(*event);
        std::ranges::stable_sort(kept, [](const WorkingEvent* a, const WorkingEvent* b) {
            return std::make_tuple(a->event.tick, a->rank, a->original_tick, a->original_order, a->creation) <
                   std::make_tuple(b->event.tick, b->rank, b->original_tick, b->original_order, b->creation);
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
                                            std::string_view tool, bool require_ppq, const NotePairing* pairing) {
    auto editor = pairing ? NoteEditor::open(document, selection.sequence, *pairing)
                          : NoteEditor::open(document, selection.sequence);
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
