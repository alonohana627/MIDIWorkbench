#include "mwb/playback/transport.hpp"

#include <format>
#include <string>

namespace mwb::playback {

std::string_view transport_state_name(TransportState state) noexcept {
    switch (state) {
    case TransportState::stopped:
        return "Stopped";
    case TransportState::playing:
        return "Playing";
    case TransportState::paused:
        return "Paused";
    case TransportState::counting_in:
        return "CountingIn";
    case TransportState::recording:
        return "Recording";
    case TransportState::device_lost:
        return "DeviceLost";
    }
    return "Unknown";
}

std::string_view transport_command_name(TransportCommand command) noexcept {
    switch (command) {
    case TransportCommand::play:
        return "Play";
    case TransportCommand::record:
        return "Record";
    case TransportCommand::record_with_count_in:
        return "Record with count in";
    case TransportCommand::count_in_complete:
        return "Count in complete";
    case TransportCommand::pause:
        return "Pause";
    case TransportCommand::resume:
        return "Resume";
    case TransportCommand::punch_in:
        return "Punch in";
    case TransportCommand::punch_out:
        return "Punch out";
    case TransportCommand::stop:
        return "Stop";
    case TransportCommand::cancel:
        return "Cancel";
    case TransportCommand::device_failure:
        return "Device failure";
    case TransportCommand::reconfigure:
        return "Reconfigure";
    }
    return "Unknown";
}

Result<TransportState> Transport::transition(TransportState state, TransportCommand command) {
    using State = TransportState;
    using Command = TransportCommand;
    switch (state) {
    case State::stopped:
        if (command == Command::play)
            return State::playing;
        if (command == Command::record_with_count_in)
            return State::counting_in;
        if (command == Command::record)
            return State::recording;
        break;
    case State::counting_in:
        if (command == Command::count_in_complete)
            return State::recording;
        if (command == Command::cancel)
            return State::stopped;
        break;
    case State::playing:
        if (command == Command::pause)
            return State::paused;
        if (command == Command::punch_in)
            return State::recording;
        if (command == Command::stop)
            return State::stopped;
        if (command == Command::device_failure)
            return State::device_lost;
        break;
    case State::paused:
        if (command == Command::resume)
            return State::playing;
        if (command == Command::stop)
            return State::stopped;
        break;
    case State::recording:
        if (command == Command::punch_out)
            return State::playing;
        if (command == Command::stop)
            return State::stopped;
        if (command == Command::device_failure)
            return State::device_lost;
        break;
    case State::device_lost:
        if (command == Command::reconfigure)
            return State::stopped;
        break;
    }
    return make_error(ErrorCode::conflict,
                      std::format("transport command '{}' is not allowed in state {}",
                                  transport_command_name(command), transport_state_name(state)));
}

Status Transport::apply(TransportCommand command) {
    auto next = transition(state_, command);
    if (!next)
        return std::unexpected(std::move(next).error());
    state_ = *next;
    return ok();
}

Status Transport::seek(Tick tick) {
    if (state_ != TransportState::stopped && state_ != TransportState::paused && state_ != TransportState::playing)
        return make_error(ErrorCode::conflict,
                          std::format("seeking is rejected while {}; stop first", transport_state_name(state_)));
    if (tick < 0)
        return make_error(ErrorCode::invalid_argument, "seek position must not be negative");
    position_ = tick;
    ++epoch_;
    return ok();
}

} // namespace mwb::playback
