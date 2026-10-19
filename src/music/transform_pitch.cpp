#include "mwb/music/transform.hpp"

#include "mwb/core/checked.hpp"

#include <algorithm>
#include <format>

namespace mwb::music {

namespace {

bool on_drum_track(const NoteEditor& editor, std::size_t note) {
    return editor.sequence().tracks[editor.notes()[note].on_track].is_drum;
}

} // namespace

// --- Transpose -----------------------------------------------------------------------------------

Result<TransformPreview> transpose(const DocumentData& document, const TargetSelection& selection,
                                   const TransposeParameters& parameters, const NotePairing* pairing) {
    const int limit = parameters.mode == TransposeMode::octaves ? 10 : parameters.mode == TransposeMode::semitones ? 127 : 128;
    if (parameters.amount < -limit || parameters.amount > limit)
        return make_error(ErrorCode::invalid_argument, "transposition amount is out of range");
    std::optional<Scale> scale;
    if (parameters.mode == TransposeMode::scale_degrees) {
        auto parsed = Scale::custom(parameters.scale_root, parameters.scale_mask, "Transpose scale");
        if (!parsed)
            return std::unexpected(parsed.error());
        scale = *parsed;
    }
    auto prepared = prepare_transform(document, selection, "transpose", false, pairing);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    std::vector<std::size_t> affected;
    for (const auto note : selected) {
        if (parameters.exclude_drums && on_drum_track(editor, note)) {
            ++report.skipped;
            continue;
        }
        affected.push_back(note);
    }
    if (report.skipped > 0)
        report.messages.push_back(std::format("{} notes on drum tracks were excluded", report.skipped));
    const auto target_pitch = [&](std::size_t note, int amount) {
        const int pitch = editor.values(note).pitch;
        if (scale)
            return scale->transpose_by_degrees(pitch, amount, parameters.out_of_scale, parameters.tie);
        return pitch + amount;
    };
    // Chromatic modes shift every pitch equally, so the extremes decide; only degree mode needs
    // the per-note check (keeps constrained transposition of large selections linear).
    int lowest = 127;
    int highest = 0;
    for (const auto note : affected) {
        lowest = std::min(lowest, editor.values(note).pitch);
        highest = std::max(highest, editor.values(note).pitch);
    }
    const auto fits = [&](int amount) {
        if (!scale)
            return affected.empty() || (lowest + amount >= 0 && highest + amount <= 127);
        for (const auto note : affected) {
            const int pitch = target_pitch(note, amount);
            if (pitch < 0 || pitch > 127)
                return false;
        }
        return true;
    };
    int amount = parameters.mode == TransposeMode::octaves ? 12 * parameters.amount : parameters.amount;
    if (!fits(amount)) {
        if (parameters.range_policy == RangePolicy::reject)
            return make_error(ErrorCode::invalid_argument,
                              "the transposition would move notes outside 0..127; choose constrain to reduce it");
        // Reduce the whole displacement toward zero until every note fits (section 7.3).
        const int step = parameters.mode == TransposeMode::octaves ? 12 : 1;
        while (amount != 0 && !fits(amount))
            amount += amount > 0 ? -step : step;
        report.messages.push_back(std::format("Transposition constrained to {}", amount));
        ++report.clipped;
    }
    for (const auto note : affected) {
        const NoteValues before = editor.values(note);
        NoteValues after = before;
        after.pitch = target_pitch(note, amount);
        if (after != before) {
            editor.update(note, after);
            ++report.changed;
        }
    }
    return finish_preview(editor, std::move(report), "Transpose");
}

// --- Invert pitch ----------------------------------------------------------------------------------

Result<TransformPreview> invert_pitch(const DocumentData& document, const TargetSelection& selection,
                                      const InvertPitchParameters& parameters) {
    if (parameters.axis_pitch < 0 || parameters.axis_pitch > 127)
        return make_error(ErrorCode::invalid_argument, "inversion axis must lie in 0..127");
    auto prepared = prepare_transform(document, selection, "invert_pitch", false);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    const int doubled_axis = 2 * parameters.axis_pitch + (parameters.axis_between_keys ? 1 : 0);
    for (const auto note : selected) {
        const NoteValues before = editor.values(note);
        const int pitch = doubled_axis - before.pitch;
        if (pitch < 0 || pitch > 127) {
            if (parameters.out_of_range == OutOfRangeAction::reject)
                return make_error(ErrorCode::invalid_argument,
                                  std::format("inverting pitch {} around the axis leaves 0..127", before.pitch));
            ++report.skipped;
            continue;
        }
        NoteValues after = before;
        after.pitch = pitch;
        if (after != before) {
            editor.update(note, after);
            ++report.changed;
        }
    }
    return finish_preview(editor, std::move(report), "Invert pitch");
}

// --- Conform to scale --------------------------------------------------------------------------------

Result<TransformPreview> conform_to_scale(const DocumentData& document, const TargetSelection& selection,
                                          const ConformToScaleParameters& parameters) {
    auto scale = Scale::custom(parameters.scale_root, parameters.scale_mask, "Conform scale");
    if (!scale)
        return std::unexpected(scale.error());
    auto prepared = prepare_transform(document, selection, "conform_to_scale", false);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    for (const auto note : selected) {
        if (parameters.exclude_drums && on_drum_track(editor, note)) {
            ++report.skipped;
            continue;
        }
        const NoteValues before = editor.values(note);
        int pitch = scale->nearest_member(before.pitch, parameters.tie);
        if (pitch > 127) {
            pitch = before.pitch;
            while (!scale->contains(pitch))
                --pitch;
        } else if (pitch < 0) {
            pitch = before.pitch;
            while (!scale->contains(pitch))
                ++pitch;
        }
        NoteValues after = before;
        after.pitch = pitch;
        if (after != before) {
            editor.update(note, after);
            ++report.changed;
        }
    }
    return finish_preview(editor, std::move(report), "Conform to scale");
}

Result<ChangeSet> set_sequence_scale(const DocumentData& document, SequenceId sequence, const Scale& scale,
                                     bool highlight, bool fold) {
    const auto index = document.sequence_index(sequence);
    if (!index)
        return make_error(ErrorCode::not_found, "the target sequence does not exist");
    Sequence updated = document.sequences[*index];
    updated.root_pc = static_cast<std::uint8_t>(scale.root_pitch_class());
    updated.scale_mask = scale.interval_mask();
    updated.settings.highlight_scale = highlight;
    updated.settings.fold_to_scale = fold;
    ChangeBuilder builder(document, "Select scale");
    builder.set_sequence_properties(sequence, updated);
    return builder.finish();
}

// --- Velocity ----------------------------------------------------------------------------------------

Result<TransformPreview> change_velocity(const DocumentData& document, const TargetSelection& selection,
                                         const VelocityParameters& parameters) {
    const bool release = parameters.target == VelocityTarget::release;
    const int floor = release ? 0 : 1;
    switch (parameters.mode) {
    case VelocityMode::set:
        if (parameters.value < floor || parameters.value > 127)
            return make_error(ErrorCode::invalid_argument, "velocity must lie in the valid range");
        break;
    case VelocityMode::add:
        if (parameters.value < -127 || parameters.value > 127)
            return make_error(ErrorCode::invalid_argument, "velocity offset must lie in -127..127");
        break;
    case VelocityMode::scale:
        if (parameters.factor.denominator <= 0 || parameters.factor.numerator < 0)
            return make_error(ErrorCode::invalid_argument, "velocity factor must not be negative");
        break;
    case VelocityMode::ramp:
        if (parameters.ramp_start < floor || parameters.ramp_start > 127 || parameters.ramp_end < floor ||
            parameters.ramp_end > 127)
            return make_error(ErrorCode::invalid_argument, "ramp velocities must lie in the valid range");
        break;
    case VelocityMode::curve:
        if (parameters.curve < -100 || parameters.curve > 100)
            return make_error(ErrorCode::invalid_argument, "velocity curve must lie in -100..100");
        break;
    case VelocityMode::compress:
        if (parameters.range_low < floor || parameters.range_high > 127 || parameters.range_low > parameters.range_high)
            return make_error(ErrorCode::invalid_argument, "compressed range must be ordered and valid");
        break;
    }
    auto prepared = prepare_transform(document, selection, "velocity", false);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    std::vector<std::size_t> affected;
    for (const auto note : selected) {
        if (release && !editor.has_release_velocity(note)) {
            ++report.skipped;
            continue;
        }
        affected.push_back(note);
    }
    if (release && report.skipped > 0)
        report.messages.push_back(std::format(
            "{} notes end with a velocity-zero note-on or have no end and cannot store a release velocity",
            report.skipped));
    const auto current = [&](std::size_t note) {
        const auto values = editor.values(note);
        return release ? values.release_velocity : values.velocity;
    };
    Tick first_onset = 0;
    Tick last_onset = 0;
    int lowest = 127;
    int highest = 0;
    for (std::size_t index = 0; index < affected.size(); ++index) {
        const auto values = editor.values(affected[index]);
        first_onset = index == 0 ? values.start : std::min(first_onset, values.start);
        last_onset = index == 0 ? values.start : std::max(last_onset, values.start);
        lowest = std::min(lowest, current(affected[index]));
        highest = std::max(highest, current(affected[index]));
    }
    for (const auto note : affected) {
        const NoteValues before = editor.values(note);
        const std::int64_t value = current(note);
        std::int64_t result = value;
        switch (parameters.mode) {
        case VelocityMode::set: result = parameters.value; break;
        case VelocityMode::add: result = value + parameters.value; break;
        case VelocityMode::scale:
            result = mul_div_round(value, parameters.factor.numerator, parameters.factor.denominator).value_or(127);
            break;
        case VelocityMode::ramp:
            result = last_onset == first_onset
                         ? parameters.ramp_start
                         : parameters.ramp_start + mul_div_round(parameters.ramp_end - parameters.ramp_start,
                                                                 before.start - first_onset, last_onset - first_onset)
                                                       .value_or(0);
            break;
        case VelocityMode::curve:
            result = value + mul_div_round(static_cast<std::int64_t>(parameters.curve) * value, 127 - value, 12700)
                                 .value_or(0);
            break;
        case VelocityMode::compress:
            result = highest == lowest
                         ? mul_div_round(parameters.range_low + parameters.range_high, 1, 2).value_or(parameters.range_low)
                         : parameters.range_low + mul_div_round(parameters.range_high - parameters.range_low,
                                                                value - lowest, highest - lowest)
                                                      .value_or(0);
            break;
        }
        const std::int64_t clamped = std::clamp<std::int64_t>(result, floor, 127);
        if (clamped != result)
            ++report.clipped;
        NoteValues after = before;
        if (release)
            after.release_velocity = static_cast<int>(clamped);
        else
            after.velocity = static_cast<int>(clamped);
        if (after != before) {
            editor.update(note, after);
            ++report.changed;
        }
    }
    return finish_preview(editor, std::move(report), "Change velocity");
}

} // namespace mwb::music
