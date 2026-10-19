#pragma once

#include "mwb/core/ids.hpp"
#include "mwb/core/time.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mwb {

/// Storage category of an event (section 18.1, table midi_event.category).
enum class EventCategory : std::uint8_t {
    channel,  ///< Channel voice message; code is the full status byte including channel.
    meta,     ///< SMF meta event; code is the meta type (end-of-track is never stored).
    sysex_f0, ///< SMF F0 SysEx packet; payload excludes the F0 byte.
    sysex_f7, ///< SMF F7 escape/continuation packet; payload excludes the F7 byte.
};

/// Channel message kinds (high nibble of the status byte).
enum class ChannelMessage : std::uint8_t {
    note_off = 0x80,
    note_on = 0x90,
    poly_pressure = 0xA0,
    control_change = 0xB0,
    program_change = 0xC0,
    channel_pressure = 0xD0,
    pitch_bend = 0xE0,
};

/// Meta event types the application understands; others are preserved opaquely.
namespace meta_type {
inline constexpr std::uint8_t sequence_number = 0x00;
inline constexpr std::uint8_t text = 0x01;
inline constexpr std::uint8_t copyright = 0x02;
inline constexpr std::uint8_t track_name = 0x03;
inline constexpr std::uint8_t instrument_name = 0x04;
inline constexpr std::uint8_t lyric = 0x05;
inline constexpr std::uint8_t marker = 0x06;
inline constexpr std::uint8_t cue_point = 0x07;
inline constexpr std::uint8_t program_name = 0x08;
inline constexpr std::uint8_t device_name = 0x09;
inline constexpr std::uint8_t channel_prefix = 0x20;
inline constexpr std::uint8_t port_prefix = 0x21;
inline constexpr std::uint8_t end_of_track = 0x2F;
inline constexpr std::uint8_t tempo = 0x51;
inline constexpr std::uint8_t smpte_offset = 0x54;
inline constexpr std::uint8_t time_signature = 0x58;
inline constexpr std::uint8_t key_signature = 0x59;
inline constexpr std::uint8_t sequencer_specific = 0x7F;
} // namespace meta_type

/// Event flag bits (midi_event.flags).
namespace event_flag {
inline constexpr std::uint32_t synthetic_closure = 1u << 0; ///< Generated note end (recording stop, loop split).
inline constexpr std::uint32_t malformed = 1u << 1;         ///< Preserved payload that fails validation.
inline constexpr std::uint32_t text_edited = 1u << 2;       ///< Text bytes were re-entered by the user.
inline constexpr std::uint32_t muted = 1u << 3;             ///< Note muted in the editor (not exported as silence).
} // namespace event_flag

/// Event payload bytes without the status byte. Short payloads are stored inline; long SysEx
/// and text payloads live in an immutable shared blob (section 18.3).
class Payload {
public:
    static constexpr std::size_t inline_capacity = 8;

    Payload() = default;
    explicit Payload(std::span<const std::uint8_t> bytes);
    Payload(std::initializer_list<std::uint8_t> bytes);

    [[nodiscard]] std::span<const std::uint8_t> bytes() const;
    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    [[nodiscard]] std::uint8_t operator[](std::size_t index) const { return bytes()[index]; }
    [[nodiscard]] std::vector<std::uint8_t> to_vector() const;

    friend bool operator==(const Payload& left, const Payload& right);

private:
    std::array<std::uint8_t, inline_capacity> inline_bytes_{};
    std::uint32_t size_ = 0;
    std::shared_ptr<const std::vector<std::uint8_t>> heap_;
};

/// A timed MIDI channel message, meta event or SysEx packet (section 2 "Event").
/// Ordering within a track is (tick, order_key); order keys are spaced for insertion.
struct Event {
    EventId id;
    Tick tick = 0;
    std::uint64_t order_key = 0;
    EventCategory category = EventCategory::channel;
    std::uint8_t code = 0;
    std::uint32_t flags = 0;
    Payload payload;

    // --- Channel message accessors (valid only when category == channel) ---
    [[nodiscard]] ChannelMessage message() const { return static_cast<ChannelMessage>(code & 0xF0); }
    [[nodiscard]] std::uint8_t channel() const { return static_cast<std::uint8_t>(code & 0x0F); }
    [[nodiscard]] std::uint8_t data1() const { return payload.size() > 0 ? payload[0] : 0; }
    [[nodiscard]] std::uint8_t data2() const { return payload.size() > 1 ? payload[1] : 0; }
    [[nodiscard]] bool is_channel() const { return category == EventCategory::channel; }
    /// Sounding note start: note-on with nonzero velocity.
    [[nodiscard]] bool is_note_start() const {
        return is_channel() && message() == ChannelMessage::note_on && data2() != 0;
    }
    /// Note end: note-off, or note-on with velocity zero (preserved encoding, section 16.3).
    [[nodiscard]] bool is_note_end() const {
        return is_channel() && (message() == ChannelMessage::note_off ||
                                (message() == ChannelMessage::note_on && data2() == 0));
    }
    [[nodiscard]] bool is_meta(std::uint8_t type) const {
        return category == EventCategory::meta && code == type;
    }
    /// Pitch bend value 0..16383 (center 8192).
    [[nodiscard]] std::int32_t pitch_bend_value() const {
        return static_cast<std::int32_t>(data1()) | (static_cast<std::int32_t>(data2()) << 7);
    }

    /// Fields that define musical content; the identity and order key are excluded.
    [[nodiscard]] bool same_content(const Event& other) const {
        return tick == other.tick && category == other.category && code == other.code &&
               flags == other.flags && payload == other.payload;
    }
    friend bool operator==(const Event&, const Event&) = default;
};

/// Spacing between consecutive order keys of imported or renumbered events.
inline constexpr std::uint64_t order_key_spacing = 1024;

/// Orders events by (tick, order_key).
struct EventOrder {
    [[nodiscard]] bool operator()(const Event& left, const Event& right) const {
        return left.tick != right.tick ? left.tick < right.tick : left.order_key < right.order_key;
    }
};

/// Number of data bytes that follow a channel status byte (1 or 2).
[[nodiscard]] constexpr std::size_t channel_data_length(std::uint8_t status) {
    const auto kind = status & 0xF0;
    return (kind == 0xC0 || kind == 0xD0) ? 1 : 2;
}

/// Builders for newly authored events. Identities are generated; order keys are assigned by the
/// caller (usually the change builder).
namespace events {
[[nodiscard]] Event note_on(Tick tick, std::uint8_t channel, std::uint8_t pitch, std::uint8_t velocity);
[[nodiscard]] Event note_off(Tick tick, std::uint8_t channel, std::uint8_t pitch, std::uint8_t velocity = 0);
[[nodiscard]] Event control_change(Tick tick, std::uint8_t channel, std::uint8_t controller, std::uint8_t value);
[[nodiscard]] Event program_change(Tick tick, std::uint8_t channel, std::uint8_t program);
[[nodiscard]] Event pitch_bend(Tick tick, std::uint8_t channel, std::int32_t value);
[[nodiscard]] Event channel_pressure(Tick tick, std::uint8_t channel, std::uint8_t value);
[[nodiscard]] Event poly_pressure(Tick tick, std::uint8_t channel, std::uint8_t pitch, std::uint8_t value);
[[nodiscard]] Event tempo(Tick tick, std::uint32_t microseconds_per_quarter);
[[nodiscard]] Event time_signature(Tick tick, std::uint8_t numerator, std::uint8_t denominator_power,
                                   std::uint8_t clocks_per_click = 24, std::uint8_t thirty_seconds_per_quarter = 8);
[[nodiscard]] Event key_signature(Tick tick, std::int8_t sharps_flats, bool minor);
[[nodiscard]] Event text_meta(Tick tick, std::uint8_t type, const std::string& utf8_text);
} // namespace events

} // namespace mwb
