#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/time.hpp"

#include <cstdint>
#include <string_view>

namespace mwb::playback {

/// Transport states of the specification section 13.3 diagram.
enum class TransportState : std::uint8_t { stopped, playing, paused, counting_in, recording, device_lost };

/// Commands (diagram edge labels).
enum class TransportCommand : std::uint8_t {
    play,               ///< Stopped -> Playing.
    record,             ///< Stopped -> Recording.
    record_with_count_in, ///< Stopped -> CountingIn.
    count_in_complete,  ///< CountingIn -> Recording.
    pause,              ///< Playing -> Paused.
    resume,             ///< Paused -> Playing.
    punch_in,           ///< Playing -> Recording.
    punch_out,          ///< Recording -> Playing.
    stop,               ///< Playing/Recording/Paused -> Stopped.
    cancel,             ///< CountingIn -> Stopped.
    device_failure,     ///< Playing/Recording -> DeviceLost.
    reconfigure,        ///< DeviceLost -> Stopped.
};

[[nodiscard]] std::string_view transport_state_name(TransportState state) noexcept;
[[nodiscard]] std::string_view transport_command_name(TransportCommand command) noexcept;

/// The transport state machine, exactly as the section 13.3 diagram: any command not drawn as an
/// edge from the current state is rejected with ErrorCode::conflict and leaves the state
/// unchanged. Seeking is accepted in Stopped, Paused and Playing only (rejected while counting in,
/// recording or after device loss) and increments the transport epoch, which invalidates queued
/// future events. Owned by one non-real-time thread; the engine mirrors its commands.
class Transport {
public:
    /// Next state for `command` from `state`, or nullopt-like failure when the edge does not exist.
    [[nodiscard]] static Result<TransportState> transition(TransportState state, TransportCommand command);

    /// Applies a command.
    [[nodiscard]] Status apply(TransportCommand command);
    /// Moves the position; rejected unless Stopped, Paused or Playing.
    [[nodiscard]] Status seek(Tick tick);

    [[nodiscard]] TransportState state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
    [[nodiscard]] Tick position() const noexcept { return position_; }
    /// Updates the displayed position from engine telemetry (no epoch change).
    void set_position(Tick tick) noexcept { position_ = tick; }

private:
    TransportState state_ = TransportState::stopped;
    std::uint64_t epoch_ = 0;
    Tick position_ = 0;
};

} // namespace mwb::playback
