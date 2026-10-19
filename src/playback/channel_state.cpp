#include "mwb/playback/channel_state.hpp"

namespace mwb::playback {

namespace {

constexpr std::uint8_t control_change_status = 0xB0;

/// True for controllers that chase must not replay individually (bank select, data entry,
/// increment/decrement, parameter selection) or that are channel mode messages (120..127).
constexpr bool is_structural_controller(std::uint8_t number) noexcept {
    switch (number) {
    case 0:
    case 32:
    case 6:
    case 38:
    case 96:
    case 97:
    case 98:
    case 99:
    case 100:
    case 101:
        return true;
    default:
        return number >= 120;
    }
}

/// Appends a message when there is room.
void push(std::span<ShortMessage> output, std::size_t& count, std::uint8_t status, std::uint8_t data1,
          std::uint8_t data2) noexcept {
    if (count < output.size())
        output[count++] = ShortMessage{status, data1, data2};
}

} // namespace

ChannelState ChannelState::defaults() noexcept {
    ChannelState state;
    state.controllers[7] = 100;
    state.controllers[8] = 64;
    state.controllers[10] = 64;
    state.controllers[11] = 127;
    state.controllers[101] = 127;
    state.controllers[100] = 127;
    state.controllers[99] = 127;
    state.controllers[98] = 127;
    return state;
}

void ChannelState::apply(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) noexcept {
    const auto kind = static_cast<std::uint8_t>(status & 0xF0u);
    data1 = static_cast<std::uint8_t>(data1 & 0x7Fu);
    data2 = static_cast<std::uint8_t>(data2 & 0x7Fu);
    switch (kind) {
    case 0xB0: {
        if (data1 >= 120)
            return; // Channel mode messages are not state to chase.
        controllers[data1] = data2;
        touched[data1 >> 6] |= std::uint64_t{1} << (data1 & 63u);
        switch (data1) {
        case 0:
            bank_msb = data2;
            break;
        case 32:
            bank_lsb = data2;
            break;
        case 101:
            parameter_msb = data2;
            parameter_is_registered = true;
            break;
        case 100:
            parameter_lsb = data2;
            parameter_is_registered = true;
            break;
        case 99:
            nrpn_msb = data2;
            parameter_is_registered = false;
            break;
        case 98:
            nrpn_lsb = data2;
            parameter_is_registered = false;
            break;
        case 6:
        case 38:
            if (!parameter_is_registered || parameter_msb != 0)
                break;
            registered_touched = true;
            if (parameter_lsb == 0) {
                if (data1 == 6)
                    bend_range_semitones = data2;
                else
                    bend_range_cents = data2;
            } else if (parameter_lsb == 1) {
                if (data1 == 6)
                    fine_tuning = static_cast<std::uint16_t>((fine_tuning & 0x7Fu) | (static_cast<unsigned>(data2) << 7));
                else
                    fine_tuning = static_cast<std::uint16_t>((fine_tuning & 0x3F80u) | data2);
            } else if (parameter_lsb == 2 && data1 == 6) {
                coarse_tuning = data2;
            }
            break;
        default:
            break;
        }
        return;
    }
    case 0xC0:
        program = data1;
        program_known = true;
        return;
    case 0xD0:
        channel_pressure = data1;
        channel_pressure_touched = true;
        return;
    case 0xE0:
        pitch_bend = static_cast<std::uint16_t>(data1 | (static_cast<unsigned>(data2) << 7));
        pitch_bend_touched = true;
        return;
    default:
        return;
    }
}

std::size_t write_chase_messages(const ChannelState& target, const ChannelState& live, std::uint8_t channel,
                                 std::span<ShortMessage> output) noexcept {
    std::size_t count = 0;
    const auto control = static_cast<std::uint8_t>(control_change_status | (channel & 0x0Fu));
    const ChannelState reference = ChannelState::defaults();

    // 1. Bank and program.
    if (target.program_known || live.program_known || target.is_touched(0) || live.is_touched(0)) {
        push(output, count, control, 0, target.bank_msb);
        push(output, count, control, 32, target.bank_lsb);
        push(output, count, static_cast<std::uint8_t>(0xC0u | (channel & 0x0Fu)), target.program, 0);
    }
    // 2. Registered parameters, then deselect with the null RPN.
    if (target.registered_touched || live.registered_touched) {
        push(output, count, control, 101, 0);
        push(output, count, control, 100, 0);
        push(output, count, control, 6, target.bend_range_semitones);
        push(output, count, control, 38, target.bend_range_cents);
        push(output, count, control, 101, 0);
        push(output, count, control, 100, 1);
        push(output, count, control, 6, static_cast<std::uint8_t>(target.fine_tuning >> 7));
        push(output, count, control, 38, static_cast<std::uint8_t>(target.fine_tuning & 0x7Fu));
        push(output, count, control, 101, 0);
        push(output, count, control, 100, 2);
        push(output, count, control, 6, target.coarse_tuning);
        // Re-select the target's parameter (not always the null RPN), so data entry that follows
        // the chase target in the sequence still reaches the parameter the sequence selected.
        if (target.parameter_is_registered) {
            push(output, count, control, 101, target.parameter_msb);
            push(output, count, control, 100, target.parameter_lsb);
        } else {
            push(output, count, control, 99, target.nrpn_msb);
            push(output, count, control, 98, target.nrpn_lsb);
        }
    }
    // 3. Continuous controllers in ascending order (sustain included).
    for (std::uint8_t number = 0; number < 120; ++number) {
        if (is_structural_controller(number))
            continue;
        if (!target.is_touched(number) && !live.is_touched(number))
            continue;
        const std::uint8_t value =
            target.is_touched(number) ? target.controllers[number] : reference.controllers[number];
        push(output, count, control, number, value);
    }
    // 4. Pitch bend.
    if (target.pitch_bend_touched || live.pitch_bend_touched) {
        push(output, count, static_cast<std::uint8_t>(0xE0u | (channel & 0x0Fu)),
             static_cast<std::uint8_t>(target.pitch_bend & 0x7Fu),
             static_cast<std::uint8_t>((target.pitch_bend >> 7) & 0x7Fu));
    }
    // 5. Channel pressure.
    if (target.channel_pressure_touched || live.channel_pressure_touched)
        push(output, count, static_cast<std::uint8_t>(0xD0u | (channel & 0x0Fu)), target.channel_pressure, 0);
    return count;
}

std::size_t write_chase_differences(const ChannelState& target, const ChannelState& live, std::uint8_t channel,
                                    std::span<ShortMessage> output) noexcept {
    std::size_t count = 0;
    const auto control = static_cast<std::uint8_t>(control_change_status | (channel & 0x0Fu));
    const ChannelState reference = ChannelState::defaults();
    const auto effective = [&](const ChannelState& state, std::uint8_t number) {
        return state.is_touched(number) ? state.controllers[number] : reference.controllers[number];
    };

    // 1. Bank and program, when the target defines one that the device does not have.
    if (target.program_known && (!live.program_known || target.program != live.program ||
                                 target.bank_msb != live.bank_msb || target.bank_lsb != live.bank_lsb)) {
        push(output, count, control, 0, target.bank_msb);
        push(output, count, control, 32, target.bank_lsb);
        push(output, count, static_cast<std::uint8_t>(0xC0u | (channel & 0x0Fu)), target.program, 0);
    }
    // 2. Registered parameters that changed, leaving the target's parameter selected.
    if ((target.registered_touched || live.registered_touched) &&
        (target.bend_range_semitones != live.bend_range_semitones || target.bend_range_cents != live.bend_range_cents ||
         target.fine_tuning != live.fine_tuning || target.coarse_tuning != live.coarse_tuning)) {
        push(output, count, control, 101, 0);
        push(output, count, control, 100, 0);
        push(output, count, control, 6, target.bend_range_semitones);
        push(output, count, control, 38, target.bend_range_cents);
        push(output, count, control, 101, 0);
        push(output, count, control, 100, 1);
        push(output, count, control, 6, static_cast<std::uint8_t>(target.fine_tuning >> 7));
        push(output, count, control, 38, static_cast<std::uint8_t>(target.fine_tuning & 0x7Fu));
        push(output, count, control, 101, 0);
        push(output, count, control, 100, 2);
        push(output, count, control, 6, target.coarse_tuning);
        if (target.parameter_is_registered) {
            push(output, count, control, 101, target.parameter_msb);
            push(output, count, control, 100, target.parameter_lsb);
        } else {
            push(output, count, control, 99, target.nrpn_msb);
            push(output, count, control, 98, target.nrpn_lsb);
        }
    }
    // 3. Continuous controllers whose sounding value differs.
    for (std::uint8_t number = 0; number < 120; ++number) {
        if (is_structural_controller(number))
            continue;
        if (!target.is_touched(number) && !live.is_touched(number))
            continue;
        const std::uint8_t value = effective(target, number);
        if (value != effective(live, number))
            push(output, count, control, number, value);
    }
    // 4. Pitch bend and 5. channel pressure, when they differ.
    const std::uint16_t target_bend = target.pitch_bend_touched ? target.pitch_bend : std::uint16_t{8192};
    const std::uint16_t live_bend = live.pitch_bend_touched ? live.pitch_bend : std::uint16_t{8192};
    if (target_bend != live_bend)
        push(output, count, static_cast<std::uint8_t>(0xE0u | (channel & 0x0Fu)),
             static_cast<std::uint8_t>(target_bend & 0x7Fu), static_cast<std::uint8_t>((target_bend >> 7) & 0x7Fu));
    const std::uint8_t target_pressure = target.channel_pressure_touched ? target.channel_pressure : std::uint8_t{0};
    const std::uint8_t live_pressure = live.channel_pressure_touched ? live.channel_pressure : std::uint8_t{0};
    if (target_pressure != live_pressure)
        push(output, count, static_cast<std::uint8_t>(0xD0u | (channel & 0x0Fu)), target_pressure, 0);
    return count;
}

} // namespace mwb::playback
