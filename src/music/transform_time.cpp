#include "mwb/music/transform.hpp"

#include "mwb/core/checked.hpp"
#include "mwb/core/random.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <tuple>

namespace mwb::music {

namespace {

/// True for fractions in [0, 1] with a positive denominator.
bool is_unit_fraction_range(const Rational& value) {
    return value.denominator > 0 && value.numerator >= 0 && value.numerator <= value.denominator;
}

/// Keeps the largest rounding error in the report.
void note_rounding(TransformReport& report, const Rational& error) {
    if (compare_rational(error, report.max_rounding_error) > 0)
        report.max_rounding_error = error;
}

/// Keeps the largest onset displacement in the report.
void note_movement(TransformReport& report, Tick from, Tick to) {
    report.max_movement = std::max(report.max_movement, to > from ? to - from : from - to);
}

/// anchor + round((tick - anchor) * factor) with its rounding error.
[[nodiscard]] Result<std::pair<Tick, Rational>> scale_about(Tick tick, Tick anchor, const Rational& factor) {
    const __int128 numerator = (static_cast<__int128>(tick) - anchor) * factor.numerator +
                               static_cast<__int128>(anchor) * factor.denominator;
    auto exact = make_rational(numerator, factor.denominator);
    if (!exact)
        return std::unexpected(exact.error());
    const Tick rounded = round_rational(*exact);
    return std::pair{rounded, rounding_error(*exact, rounded)};
}

/// Rejects fractions outside [0, 1].
[[nodiscard]] Status require_fraction_unit(const Rational& value, std::string_view name) {
    if (!is_unit_fraction_range(value))
        return make_error(ErrorCode::invalid_argument, std::string(name) + " must lie between 0 and 1");
    return ok();
}

} // namespace

// --- Quantize ------------------------------------------------------------------------------------

Result<TransformPreview> quantize(const DocumentData& document, const TargetSelection& selection,
                                  const QuantizeParameters& parameters) {
    MWB_TRY(require_fraction_unit(parameters.strength, "quantize strength"));
    if (parameters.max_movement && *parameters.max_movement < 0)
        return make_error(ErrorCode::invalid_argument, "maximum movement must not be negative");
    auto prepared = prepare_transform(document, selection, "quantize", true);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    auto grid = Grid::make(editor.ppq(), parameters.grid);
    if (!grid)
        return std::unexpected(grid.error());
    if (!grid->is_exact())
        report.messages.push_back("Rounded to project ticks: the grid is not an integer number of ticks");
    const bool starts = parameters.target != QuantizeTarget::ends;
    const bool ends = parameters.target != QuantizeTarget::starts;
    const auto quantize_tick = [&](Tick tick) -> Result<std::pair<Tick, Rational>> {
        const Rational target = grid->point(grid->nearest_index(tick));
        return move_toward(tick, target, parameters.strength);
    };
    for (const auto note : selected) {
        if (editor.is_unterminated(note) && ends) {
            ++report.skipped;
            continue;
        }
        const NoteValues before = editor.values(note);
        NoteValues after = before;
        if (starts) {
            auto moved = quantize_tick(before.start);
            if (!moved)
                return std::unexpected(moved.error());
            after.start = moved->first;
            note_rounding(report, moved->second);
        }
        if (ends) {
            auto moved = quantize_tick(before.end);
            if (!moved)
                return std::unexpected(moved.error());
            after.end = moved->first;
            note_rounding(report, moved->second);
        }
        const Tick start_move = after.start > before.start ? after.start - before.start : before.start - after.start;
        const Tick end_move = after.end > before.end ? after.end - before.end : before.end - after.end;
        if (parameters.max_movement && (start_move > *parameters.max_movement || (!starts && end_move > *parameters.max_movement))) {
            ++report.skipped;
            continue;
        }
        if (starts && !ends) {
            if (parameters.preserve_duration && !editor.is_unterminated(note))
                after.end = before.end + (after.start - before.start);
        }
        if (!editor.is_unterminated(note) && after.end <= after.start) {
            after.end = after.start + 1;
            ++report.clipped;
        }
        note_movement(report, before.start, after.start);
        if (after != before) {
            editor.update(note, after);
            ++report.changed;
        }
    }
    return finish_preview(editor, std::move(report), "Quantize");
}

// --- Humanize ------------------------------------------------------------------------------------

Result<TransformPreview> humanize(const DocumentData& document, const TargetSelection& selection,
                                  const HumanizeParameters& parameters) {
    constexpr std::int64_t max_range = 1'000'000'000;
    if (parameters.timing_range < 0 || parameters.velocity_range < 0 || parameters.duration_range < 0 ||
        parameters.timing_range > max_range || parameters.duration_range > max_range || parameters.velocity_range > 127)
        return make_error(ErrorCode::invalid_argument, "humanize ranges must be non-negative and bounded");
    if (parameters.chord_tolerance && *parameters.chord_tolerance < 0)
        return make_error(ErrorCode::invalid_argument, "chord tolerance must not be negative");
    auto prepared = prepare_transform(document, selection, "humanize", false);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    DeterministicRandom random(parameters.seed);
    const auto draw = [&](std::int64_t range) -> std::int64_t {
        if (range == 0)
            return 0;
        return parameters.distribution == RandomDistribution::uniform ? random.range(-range, range)
                                                                       : random.triangular(range);
    };
    std::vector<std::vector<std::size_t>> groups;
    if (parameters.keep_chord_groups) {
        groups = onset_groups(editor, selected, parameters.chord_tolerance.value_or(default_onset_tolerance(editor.ppq())));
    } else {
        for (const auto note : selected)
            groups.push_back({note});
    }
    // Per line (track, channel): original first onset and new first onset of the previous group.
    std::map<std::pair<std::size_t, int>, std::pair<Tick, Tick>> lines;
    for (const auto& group : groups) {
        Tick first_start = editor.values(group.front()).start;
        for (const auto note : group)
            first_start = std::min(first_start, editor.values(note).start);
        Tick offset = draw(parameters.timing_range);
        const std::pair<std::size_t, int> line{editor.notes()[group.front()].on_track,
                                               editor.values(group.front()).channel};
        if (parameters.prevent_order_inversions) {
            const auto previous = lines.find(line);
            if (previous != lines.end() && previous->second.first < first_start &&
                first_start + offset <= previous->second.second) {
                offset = previous->second.second + 1 - first_start;
                ++report.clipped;
            }
        }
        if (first_start + offset < 0) {
            offset = -first_start;
            ++report.clipped;
        }
        lines[line] = {first_start, first_start + offset};
        for (const auto note : group) {
            const NoteValues before = editor.values(note);
            NoteValues after = before;
            after.start = before.start + offset;
            const std::int64_t velocity = before.velocity + draw(parameters.velocity_range);
            after.velocity = static_cast<int>(std::clamp<std::int64_t>(velocity, 1, 127));
            if (after.velocity != velocity)
                ++report.clipped;
            const Tick duration_change = draw(parameters.duration_range);
            if (!editor.is_unterminated(note)) {
                Tick duration = before.end - before.start + duration_change;
                if (duration < 1) {
                    duration = 1;
                    ++report.clipped;
                }
                after.end = after.start + duration;
            }
            note_movement(report, before.start, after.start);
            if (after != before) {
                editor.update(note, after);
                ++report.changed;
            }
        }
    }
    return finish_preview(editor, std::move(report), "Humanize");
}

// --- Move ------------------------------------------------------------------------------------------

Result<TransformPreview> move_notes(const DocumentData& document, const TargetSelection& selection,
                                    const MoveParameters& parameters, const NotePairing* pairing) {
    auto prepared = prepare_transform(document, selection, "move", false, pairing);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    if (selected.empty())
        return finish_preview(editor, std::move(report), "Move notes");
    Tick earliest = editor.values(selected.front()).start;
    int lowest = 127;
    int highest = 0;
    for (const auto note : selected) {
        const auto values = editor.values(note);
        earliest = std::min(earliest, values.start);
        lowest = std::min(lowest, values.pitch);
        highest = std::max(highest, values.pitch);
    }
    Tick ticks = parameters.ticks;
    int semitones = parameters.semitones;
    if (earliest + ticks < 0 || lowest + semitones < 0 || highest + semitones > 127) {
        if (parameters.range_policy == RangePolicy::reject)
            return make_error(ErrorCode::invalid_argument,
                              "the move would cross tick 0 or leave the pitch range 0..127");
        // Constrain the whole displacement (section 7.3); notes are never clamped one by one.
        ticks = std::max(ticks, -earliest);
        semitones = std::clamp(semitones, -lowest, 127 - highest);
        report.messages.push_back(std::format("Displacement constrained to {} ticks and {} semitones", ticks, semitones));
        ++report.clipped;
    }
    for (const auto note : selected) {
        const NoteValues before = editor.values(note);
        NoteValues after = before;
        after.start += ticks;
        if (!editor.is_unterminated(note))
            after.end += ticks;
        after.pitch += semitones;
        note_movement(report, before.start, after.start);
        if (after != before) {
            editor.update(note, after);
            ++report.changed;
        }
    }
    return finish_preview(editor, std::move(report), "Move notes");
}

// --- Length ----------------------------------------------------------------------------------------

Result<TransformPreview> change_length(const DocumentData& document, const TargetSelection& selection,
                                       const LengthParameters& parameters) {
    if (parameters.mode == LengthMode::set && parameters.ticks < 1)
        return make_error(ErrorCode::invalid_argument, "a set length must be at least one tick");
    if (parameters.mode == LengthMode::multiply &&
        (parameters.factor.denominator <= 0 || parameters.factor.numerator <= 0))
        return make_error(ErrorCode::invalid_argument, "length factor must be positive");
    if (parameters.mode == LengthMode::gap && parameters.ticks < 0)
        return make_error(ErrorCode::invalid_argument, "gap must not be negative");
    auto prepared = prepare_transform(document, selection, "length", false);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    // Next later onset per note within its (track, channel) line among the selected notes.
    std::map<std::pair<std::size_t, int>, std::vector<std::size_t>> lines;
    for (const auto note : selected)
        lines[{editor.notes()[note].on_track, editor.values(note).channel}].push_back(note);
    std::map<std::size_t, Tick> next_onset;
    for (auto& [line, members] : lines) {
        std::ranges::stable_sort(members, [&](std::size_t a, std::size_t b) {
            return editor.values(a).start < editor.values(b).start;
        });
        // One backward pass: the next strictly later start, so a chord of c notes costs O(c),
        // not O(c^2) (code-core-11).
        std::optional<Tick> later;       // first start after the current group
        std::optional<Tick> group_start; // start of the group being walked (equal starts)
        std::optional<Tick> group_next;  // what that group's notes get
        for (std::size_t index = members.size(); index-- > 0;) {
            const Tick start = editor.values(members[index]).start;
            if (!group_start || start != *group_start) {
                later = group_start; // the group just left is the next later onset
                group_start = start;
                group_next = later;
            }
            if (group_next)
                next_onset[members[index]] = *group_next;
        }
    }
    for (const auto note : selected) {
        if (editor.is_unterminated(note)) {
            ++report.skipped;
            continue;
        }
        const NoteValues before = editor.values(note);
        const Tick duration = before.end - before.start;
        Tick wanted = duration;
        switch (parameters.mode) {
        case LengthMode::set: wanted = parameters.ticks; break;
        case LengthMode::add: wanted = duration + parameters.ticks; break;
        case LengthMode::multiply: {
            auto scaled = scale_about(duration, 0, parameters.factor);
            if (!scaled)
                return std::unexpected(scaled.error());
            wanted = scaled->first;
            note_rounding(report, scaled->second);
            break;
        }
        case LengthMode::legato_to_next:
        case LengthMode::gap: {
            const auto next = next_onset.find(note);
            if (next == next_onset.end())
                continue;
            wanted = next->second - before.start - (parameters.mode == LengthMode::gap ? parameters.ticks : 0);
            break;
        }
        }
        if (wanted < 1) {
            wanted = 1;
            ++report.clipped;
        }
        NoteValues after = before;
        after.end = before.start + wanted;
        if (after != before) {
            editor.update(note, after);
            ++report.changed;
        }
    }
    return finish_preview(editor, std::move(report), "Change length");
}

// --- Time stretch ----------------------------------------------------------------------------------

Result<TransformPreview> time_stretch(const DocumentData& document, const TargetSelection& selection,
                                      const TimeStretchParameters& parameters) {
    if (parameters.factor.denominator <= 0 || parameters.factor.numerator <= 0)
        return make_error(ErrorCode::invalid_argument, "stretch factor must be a positive rational");
    if (parameters.anchor < 0)
        return make_error(ErrorCode::invalid_argument, "stretch anchor must not be negative");
    if (parameters.event_classes == 0)
        return make_error(ErrorCode::invalid_argument, "select at least one event class to stretch");
    auto prepared = prepare_transform(document, selection, "time_stretch", false);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    const auto map_tick = [&](Tick tick) -> Result<Tick> {
        auto mapped = scale_about(tick, parameters.anchor, parameters.factor);
        if (!mapped)
            return std::unexpected(mapped.error());
        if (mapped->first < 0)
            return make_error(ErrorCode::invalid_argument, "the stretch would move events before tick 0");
        note_rounding(report, mapped->second);
        return mapped->first;
    };
    if (parameters.event_classes & event_class::notes) {
        for (const auto note : selected) {
            if (editor.is_unterminated(note)) {
                ++report.skipped;
                continue;
            }
            const NoteValues before = editor.values(note);
            NoteValues after = before;
            auto start = map_tick(before.start);
            if (!start)
                return std::unexpected(start.error());
            auto end = map_tick(before.end);
            if (!end)
                return std::unexpected(end.error());
            after.start = *start;
            after.end = *end;
            if (after.end <= after.start && before.end > before.start) {
                after.end = after.start + 1;
                ++report.clipped;
            }
            note_movement(report, before.start, after.start);
            if (after != before) {
                editor.update(note, after);
                ++report.changed;
            }
        }
    } else {
        report.targeted = 0;
    }
    const std::uint8_t other_classes = parameters.event_classes & static_cast<std::uint8_t>(~event_class::notes);
    if (other_classes != 0) {
        const Sequence& sequence = editor.sequence();
        for (std::size_t track = 0; track < sequence.tracks.size(); ++track) {
            const auto& data = sequence.tracks[track];
            if (!selection.tracks.empty() && std::ranges::find(selection.tracks, data.id) == selection.tracks.end())
                continue;
            for (std::size_t index = 0; index < data.events.size(); ++index) {
                const Event& event = data.events[index];
                if (event.is_note_start() || event.is_note_end())
                    continue;
                std::uint8_t event_class_bit = 0;
                switch (event.category) {
                case EventCategory::channel: event_class_bit = event_class::controllers; break;
                case EventCategory::meta: event_class_bit = event_class::meta; break;
                case EventCategory::sysex_f0:
                case EventCategory::sysex_f7: event_class_bit = event_class::sysex; break;
                }
                if (!(other_classes & event_class_bit))
                    continue;
                if (selection.range && !selection.range->contains(event.tick))
                    continue;
                ++report.targeted;
                auto mapped = map_tick(event.tick);
                if (!mapped)
                    return std::unexpected(mapped.error());
                if (*mapped != event.tick) {
                    editor.retime_event(track, index, *mapped);
                    ++report.changed;
                }
            }
        }
    }
    return finish_preview(editor, std::move(report), "Time stretch");
}

// --- Reverse -----------------------------------------------------------------------------------------

Result<TransformPreview> reverse(const DocumentData& document, const TargetSelection& selection,
                                 const ReverseParameters& parameters) {
    auto prepared = prepare_transform(document, selection, "reverse", false);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    std::optional<TimeRange> range = parameters.range ? parameters.range : selection.range;
    if (!range) {
        if (selected.empty())
            return finish_preview(editor, std::move(report), "Reverse");
        TimeRange extent{editor.values(selected.front()).start, editor.values(selected.front()).end};
        for (const auto note : selected) {
            extent.begin = std::min(extent.begin, editor.values(note).start);
            extent.end = std::max(extent.end, editor.values(note).end);
        }
        range = extent;
    }
    if (range->begin < 0 || range->end < range->begin)
        return make_error(ErrorCode::invalid_argument, "reverse range must be a non-negative half-open range");
    const Tick mirror = range->begin + range->end;
    for (const auto note : selected) {
        const NoteValues before = editor.values(note);
        if (editor.is_unterminated(note) || before.start < range->begin || before.end > range->end) {
            ++report.skipped;
            continue;
        }
        NoteValues after = before;
        after.start = mirror - before.end;
        after.end = mirror - before.start;
        note_movement(report, before.start, after.start);
        if (after != before) {
            editor.update(note, after);
            ++report.changed;
        }
    }
    if (report.skipped > 0)
        report.messages.push_back(std::format("{} notes cross the range boundary or have no end and were left unchanged",
                                              report.skipped));
    std::size_t controllers = 0;
    for (const auto& track : editor.sequence().tracks) {
        if (!selection.tracks.empty() && std::ranges::find(selection.tracks, track.id) == selection.tracks.end())
            continue;
        for (const auto& event : track.events)
            if (event.is_channel() && !event.is_note_start() && !event.is_note_end() && range->contains(event.tick))
                ++controllers;
    }
    switch (parameters.controllers) {
    case ControllerReversePolicy::leave_unchanged:
        if (controllers > 0)
            report.messages.push_back(std::format(
                "{} controller events in the range were left unchanged (reversing controller timestamps is not "
                "equivalent playback)",
                controllers));
        break;
    }
    return finish_preview(editor, std::move(report), "Reverse");
}

// --- Rotate --------------------------------------------------------------------------------------------

Result<TransformPreview> rotate(const DocumentData& document, const TargetSelection& selection,
                                const RotateParameters& parameters) {
    const TimeRange range = parameters.range;
    if (range.begin < 0 || range.end <= range.begin)
        return make_error(ErrorCode::invalid_argument, "rotate needs an explicit non-empty range");
    auto prepared = prepare_transform(document, selection, "rotate", false);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    const Tick length = range.length();
    const Tick shift = ((parameters.shift % length) + length) % length;
    std::vector<EventId> created;
    std::size_t splits = 0;
    for (const auto note : selected) {
        const NoteValues before = editor.values(note);
        if (editor.is_unterminated(note)) {
            ++report.skipped;
            continue;
        }
        const bool intersects = before.start < range.end && (before.end > range.begin || before.start >= range.begin);
        if (!intersects || shift == 0)
            continue;
        std::vector<std::pair<Tick, Tick>> pieces;
        if (before.start < range.begin)
            pieces.emplace_back(before.start, range.begin);
        const Tick inner_start = std::max(before.start, range.begin);
        const Tick inner_end = std::min(before.end, range.end);
        const Tick moved_start = range.begin + (inner_start - range.begin + shift) % length;
        const Tick moved_end = moved_start + (inner_end - inner_start);
        if (moved_end > range.end) {
            pieces.emplace_back(moved_start, range.end);
            pieces.emplace_back(range.begin, range.begin + (moved_end - range.end));
        } else {
            pieces.emplace_back(moved_start, moved_end);
        }
        if (before.end > range.end)
            pieces.emplace_back(range.end, before.end);
        NoteValues first = before;
        first.start = pieces.front().first;
        first.end = pieces.front().second;
        if (pieces.size() > 1)
            first.release_velocity = 0;
        note_movement(report, before.start, first.start);
        editor.update(note, first);
        ++report.changed;
        for (std::size_t index = 1; index < pieces.size(); ++index) {
            NoteValues piece = before;
            piece.start = pieces[index].first;
            piece.end = pieces[index].second;
            piece.release_velocity = index + 1 == pieces.size() ? before.release_velocity : 0;
            created.push_back(editor.add(editor.notes()[note].on_track, piece));
            ++report.created;
            ++splits;
        }
    }
    if (splits > 0)
        report.messages.push_back(std::format("{} boundary-crossing note parts were split", splits));
    auto preview = finish_preview(editor, std::move(report), "Rotate phrase");
    preview.created_notes = std::move(created);
    return preview;
}

// --- Repeat ----------------------------------------------------------------------------------------------

Result<TransformPreview> repeat(const DocumentData& document, const TargetSelection& selection,
                                const RepeatParameters& parameters) {
    if (parameters.count < 1 || parameters.count > 1024)
        return make_error(ErrorCode::invalid_argument, "repeat count must lie in 1..1024");
    if (parameters.length < 0)
        return make_error(ErrorCode::invalid_argument, "repeat length must not be negative");
    if (parameters.pitch_step < -127 || parameters.pitch_step > 127 || parameters.velocity_step < -127 ||
        parameters.velocity_step > 127)
        return make_error(ErrorCode::invalid_argument, "repeat pitch and velocity steps must lie in -127..127");
    auto prepared = prepare_transform(document, selection, "repeat", false);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    if (selected.empty())
        return finish_preview(editor, std::move(report), "Repeat");
    constexpr std::size_t created_note_budget = 1'000'000; // one command never creates more notes
    if (selected.size() * static_cast<std::size_t>(parameters.count) > created_note_budget)
        return make_error(ErrorCode::limit_exceeded, "repeat would create more than 1,000,000 notes; repeat fewer notes or "
                                                     "fewer times");
    Tick length = parameters.length;
    if (length == 0) {
        if (selection.range) {
            length = selection.range->length();
        } else {
            Tick first = editor.values(selected.front()).start;
            Tick last = first;
            for (const auto note : selected) {
                first = std::min(first, editor.values(note).start);
                last = std::max(last, editor.values(note).end);
            }
            length = last - first;
        }
    }
    if (length < 1)
        return make_error(ErrorCode::invalid_argument, "repeat length must be at least one tick");
    std::vector<EventId> created;
    for (int copy = 1; copy <= parameters.count; ++copy) {
        for (const auto note : selected) {
            if (editor.is_unterminated(note)) {
                ++report.skipped;
                continue;
            }
            const NoteValues source = editor.original_values(note);
            const auto offset = checked_mul(length, copy);
            const auto start = offset ? checked_add(source.start, *offset) : std::nullopt;
            const auto end = offset ? checked_add(source.end, *offset) : std::nullopt;
            if (!start || !end)
                return make_error(ErrorCode::limit_exceeded, "repeated notes exceed the tick range");
            NoteValues copy_values = source;
            copy_values.start = *start;
            copy_values.end = *end;
            copy_values.pitch = source.pitch + copy * parameters.pitch_step;
            if (copy_values.pitch < 0 || copy_values.pitch > 127) {
                if (parameters.out_of_range == OutOfRangeAction::reject)
                    return make_error(ErrorCode::invalid_argument, "a repetition would leave the pitch range 0..127");
                ++report.skipped;
                continue;
            }
            const int velocity = source.velocity + copy * parameters.velocity_step;
            copy_values.velocity = std::clamp(velocity, 1, 127);
            if (copy_values.velocity != velocity)
                ++report.clipped;
            created.push_back(editor.add(editor.notes()[note].on_track, copy_values));
            ++report.created;
        }
    }
    auto preview = finish_preview(editor, std::move(report), "Repeat");
    preview.created_notes = std::move(created);
    return preview;
}

// --- Split and join ------------------------------------------------------------------------------------------

Result<TransformPreview> split_notes(const DocumentData& document, const TargetSelection& selection,
                                     const SplitParameters& parameters) {
    if (parameters.points.empty())
        return make_error(ErrorCode::invalid_argument, "split needs at least one explicit split point");
    std::vector<Tick> points = parameters.points;
    std::ranges::sort(points);
    const auto [first_duplicate, last_duplicate] = std::ranges::unique(points);
    points.erase(first_duplicate, last_duplicate);
    auto prepared = prepare_transform(document, selection, "split", false);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    std::vector<EventId> created;
    for (const auto note : selected) {
        if (editor.is_unterminated(note)) {
            ++report.skipped;
            continue;
        }
        const NoteValues before = editor.values(note);
        std::vector<Tick> cuts;
        for (const Tick point : points)
            if (point > before.start && point < before.end)
                cuts.push_back(point);
        if (cuts.empty())
            continue;
        NoteValues first = before;
        first.end = cuts.front();
        first.release_velocity = 0;
        editor.update(note, first);
        ++report.changed;
        for (std::size_t index = 0; index < cuts.size(); ++index) {
            NoteValues piece = before;
            piece.start = cuts[index];
            piece.end = index + 1 < cuts.size() ? cuts[index + 1] : before.end;
            piece.release_velocity = index + 1 < cuts.size() ? 0 : before.release_velocity;
            created.push_back(editor.add(editor.notes()[note].on_track, piece));
            ++report.created;
        }
    }
    auto preview = finish_preview(editor, std::move(report), "Split notes");
    preview.created_notes = std::move(created);
    return preview;
}

Result<TransformPreview> join_notes(const DocumentData& document, const TargetSelection& selection,
                                    const JoinParameters& parameters) {
    if (parameters.max_gap < 0)
        return make_error(ErrorCode::invalid_argument, "join gap must not be negative");
    auto prepared = prepare_transform(document, selection, "join", false);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    std::map<std::tuple<std::size_t, int, int>, std::vector<std::size_t>> chains;
    for (const auto note : selected) {
        if (editor.is_unterminated(note)) {
            ++report.skipped;
            continue;
        }
        const auto values = editor.values(note);
        chains[{editor.notes()[note].on_track, values.channel, values.pitch}].push_back(note);
    }
    for (auto& [key, members] : chains) {
        std::ranges::stable_sort(members, [&](std::size_t a, std::size_t b) {
            return editor.values(a).start < editor.values(b).start;
        });
        std::size_t head = members.front();
        NoteValues joined = editor.values(head);
        bool head_changed = false;
        const auto flush = [&]() {
            if (head_changed) {
                editor.update(head, joined);
                ++report.changed;
            }
        };
        for (std::size_t index = 1; index < members.size(); ++index) {
            const NoteValues next = editor.values(members[index]);
            const Tick gap = next.start - joined.end;
            if (gap >= 0 && gap <= parameters.max_gap) {
                joined.end = std::max(joined.end, next.end);
                joined.release_velocity = next.release_velocity;
                editor.remove(members[index]);
                ++report.removed;
                head_changed = true;
                continue;
            }
            if (gap < 0)
                ++report.skipped;
            flush();
            head = members[index];
            joined = next;
            head_changed = false;
        }
        flush();
    }
    if (report.skipped > 0)
        report.messages.push_back(std::format("{} overlapping or unterminated notes were not joined", report.skipped));
    return finish_preview(editor, std::move(report), "Join notes");
}

} // namespace mwb::music
