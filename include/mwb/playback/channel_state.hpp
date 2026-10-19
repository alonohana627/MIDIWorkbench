#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mwb::playback {

/// One MIDI short message without timing (used for chase and cleanup output).
struct ShortMessage {
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
};

/// Maximum number of messages write_chase_messages() can produce for one channel.
inline constexpr std::size_t max_chase_messages = 160;

/// Reconstructed controller state of one route (destination plus MIDI channel), as used by
/// controller chase (specification section 13.5). Tracks bank/program, continuous controllers,
/// pitch bend, channel pressure, sustain (CC64) and the understood registered parameters
/// (pitch-bend range, fine and coarse tuning). Unknown SysEx and NRPN values are not modelled.
struct ChannelState {
    std::array<std::uint8_t, 128> controllers{}; ///< Current value of every controller number.
    std::array<std::uint64_t, 2> touched{};       ///< Bit set of controllers that were explicitly set.
    std::uint8_t bank_msb = 0;
    std::uint8_t bank_lsb = 0;
    std::uint8_t program = 0;
    bool program_known = false;   ///< A program (or instrument baseline) is defined.
    std::uint16_t pitch_bend = 8192;
    bool pitch_bend_touched = false;
    std::uint8_t channel_pressure = 0;
    bool channel_pressure_touched = false;
    std::uint8_t parameter_msb = 127; ///< Selected registered parameter (127/127 = null).
    std::uint8_t parameter_lsb = 127;
    bool parameter_is_registered = true; ///< False after NRPN selection (data entry is then ignored).
    std::uint8_t bend_range_semitones = 2;
    std::uint8_t bend_range_cents = 0;
    std::uint16_t fine_tuning = 8192;
    std::uint8_t coarse_tuning = 64;
    bool registered_touched = false;

    /// Power-on defaults: volume 100, pan 64, expression 127, bend centered, range 2 semitones.
    [[nodiscard]] static ChannelState defaults() noexcept;

    /// Applies one channel-voice message (status byte with channel). Note messages and
    /// unrelated statuses are ignored.
    void apply(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) noexcept;

    /// True when controller `number` was explicitly set.
    [[nodiscard]] bool is_touched(std::uint8_t number) const noexcept {
        return ((touched[number >> 6] >> (number & 63u)) & 1u) != 0;
    }
    /// True when the sustain pedal (CC64) is down.
    [[nodiscard]] bool sustain_down() const noexcept { return controllers[64] >= 64; }
    /// True when anything has been set on this channel.
    [[nodiscard]] bool any_touched() const noexcept {
        return touched[0] != 0 || touched[1] != 0 || program_known || pitch_bend_touched ||
               channel_pressure_touched || registered_touched;
    }
};

/// Writes the messages that move a channel from `live` to `target` in the defined chase order:
/// bank MSB/LSB and program, registered parameters (bend range, fine and coarse tuning, then the
/// null RPN), controllers in ascending order (sustain included; bank, data-entry and parameter
/// selection controllers excluded), pitch bend, channel pressure. A controller is written when
/// either state touched it, so values set after the target position are restored to defaults.
/// Returns the number of messages written (at most max_chase_messages and output.size()).
[[nodiscard]] std::size_t write_chase_messages(const ChannelState& target, const ChannelState& live,
                                               std::uint8_t channel, std::span<ShortMessage> output) noexcept;

} // namespace mwb::playback
