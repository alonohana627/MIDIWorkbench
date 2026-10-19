#pragma once

#include <array>
#include <cstdint>
#include <limits>

namespace mwb::playback {

/// Kind of a real-time event delivered to a sink during one audio block.
enum class RealtimeEventKind : std::uint8_t {
    midi,             ///< A MIDI 1.0 short message in `bytes` (1..3 bytes).
    tempo,            ///< Informational tempo change; `value` is microseconds per quarter note.
    metronome_click,  ///< Metronome click; `value` is the accent level (0 beat, 1 group, 2 bar).
};

/// Route index used by events that do not belong to a snapshot route (live input, metronome).
inline constexpr std::uint32_t no_route = std::numeric_limits<std::uint32_t>::max();

/// One event as seen by the real-time thread: a short MIDI message (or a marker) placed at an
/// exact frame offset inside the current audio block. Trivially copyable and fixed size so it can
/// travel through preallocated queues (specification sections 13.4 and 19.2).
struct RealtimeMidiEvent {
    std::uint32_t frame_offset = 0;    ///< Offset within the current block, 0 <= offset < block size.
    std::uint32_t route = no_route;    ///< Index into the snapshot route table, or no_route.
    std::uint32_t value = 0;           ///< Payload for non-MIDI kinds (tempo, click accent).
    RealtimeEventKind kind = RealtimeEventKind::midi;
    std::uint8_t size = 0;             ///< Number of valid bytes for MIDI events.
    std::array<std::uint8_t, 3> bytes{};

    /// Status byte (valid for MIDI events).
    [[nodiscard]] constexpr std::uint8_t status() const noexcept { return bytes[0]; }
    /// Message kind (high nibble of the status byte).
    [[nodiscard]] constexpr std::uint8_t message() const noexcept {
        return static_cast<std::uint8_t>(bytes[0] & 0xF0u);
    }
    /// MIDI channel 0..15.
    [[nodiscard]] constexpr std::uint8_t channel() const noexcept {
        return static_cast<std::uint8_t>(bytes[0] & 0x0Fu);
    }
    [[nodiscard]] constexpr std::uint8_t data1() const noexcept { return bytes[1]; }
    [[nodiscard]] constexpr std::uint8_t data2() const noexcept { return bytes[2]; }

    /// Builds a MIDI short message event. Channel-voice messages with one data byte (program
    /// change, channel pressure) get size 2.
    [[nodiscard]] static constexpr RealtimeMidiEvent short_message(std::uint8_t status, std::uint8_t data1,
                                                                   std::uint8_t data2,
                                                                   std::uint32_t frame_offset = 0,
                                                                   std::uint32_t route = no_route) noexcept {
        RealtimeMidiEvent event;
        event.frame_offset = frame_offset;
        event.route = route;
        event.kind = RealtimeEventKind::midi;
        const auto kind = static_cast<std::uint8_t>(status & 0xF0u);
        event.size = (kind == 0xC0 || kind == 0xD0) ? std::uint8_t{2} : std::uint8_t{3};
        event.bytes = {status, static_cast<std::uint8_t>(data1 & 0x7Fu),
                       event.size == 3 ? static_cast<std::uint8_t>(data2 & 0x7Fu) : std::uint8_t{0}};
        return event;
    }
};

/// Receives the events of one audio block in non-decreasing frame-offset order. Implementations
/// run on the real-time thread: no allocation, locks or logging.
class IEventSink {
public:
    virtual ~IEventSink() = default;
    /// Called once per event, in order.
    virtual void on_event(const RealtimeMidiEvent& event) noexcept = 0;
};

} // namespace mwb::playback
