#include "mwb/core/event.hpp"

#include <algorithm>
#include <cstring>

namespace mwb {

Payload::Payload(std::span<const std::uint8_t> bytes) : size_(static_cast<std::uint32_t>(bytes.size())) {
    if (bytes.size() <= inline_capacity) {
        std::copy(bytes.begin(), bytes.end(), inline_bytes_.begin());
    } else {
        heap_ = std::make_shared<const std::vector<std::uint8_t>>(bytes.begin(), bytes.end());
    }
}

Payload::Payload(std::initializer_list<std::uint8_t> bytes)
    : Payload(std::span<const std::uint8_t>(bytes.begin(), bytes.size())) {}

std::span<const std::uint8_t> Payload::bytes() const {
    if (heap_)
        return {heap_->data(), heap_->size()};
    return {inline_bytes_.data(), size_};
}

std::vector<std::uint8_t> Payload::to_vector() const {
    const auto view = bytes();
    return {view.begin(), view.end()};
}

bool operator==(const Payload& left, const Payload& right) {
    const auto a = left.bytes();
    const auto b = right.bytes();
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

namespace events {

namespace {

Event channel_event(Tick tick, std::uint8_t status, std::initializer_list<std::uint8_t> data) {
    Event event;
    event.id = EventId::generate();
    event.tick = tick;
    event.category = EventCategory::channel;
    event.code = status;
    event.payload = Payload(data);
    return event;
}

std::uint8_t clamp7(int value) { return static_cast<std::uint8_t>(std::clamp(value, 0, 127)); }

std::uint8_t status(ChannelMessage kind, std::uint8_t channel) {
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(kind) | (channel & 0x0F));
}

Event meta_event(Tick tick, std::uint8_t type, std::span<const std::uint8_t> data) {
    Event event;
    event.id = EventId::generate();
    event.tick = tick;
    event.category = EventCategory::meta;
    event.code = type;
    event.payload = Payload(data);
    return event;
}

} // namespace

Event note_on(Tick tick, std::uint8_t channel, std::uint8_t pitch, std::uint8_t velocity) {
    return channel_event(tick, status(ChannelMessage::note_on, channel), {clamp7(pitch), clamp7(velocity)});
}

Event note_off(Tick tick, std::uint8_t channel, std::uint8_t pitch, std::uint8_t velocity) {
    return channel_event(tick, status(ChannelMessage::note_off, channel), {clamp7(pitch), clamp7(velocity)});
}

Event control_change(Tick tick, std::uint8_t channel, std::uint8_t controller, std::uint8_t value) {
    return channel_event(tick, status(ChannelMessage::control_change, channel), {clamp7(controller), clamp7(value)});
}

Event program_change(Tick tick, std::uint8_t channel, std::uint8_t program) {
    return channel_event(tick, status(ChannelMessage::program_change, channel), {clamp7(program)});
}

Event pitch_bend(Tick tick, std::uint8_t channel, std::int32_t value) {
    const int clamped = std::clamp(value, 0, 16383);
    return channel_event(tick, status(ChannelMessage::pitch_bend, channel),
                         {static_cast<std::uint8_t>(clamped & 0x7F), static_cast<std::uint8_t>((clamped >> 7) & 0x7F)});
}

Event channel_pressure(Tick tick, std::uint8_t channel, std::uint8_t value) {
    return channel_event(tick, status(ChannelMessage::channel_pressure, channel), {clamp7(value)});
}

Event poly_pressure(Tick tick, std::uint8_t channel, std::uint8_t pitch, std::uint8_t value) {
    return channel_event(tick, status(ChannelMessage::poly_pressure, channel), {clamp7(pitch), clamp7(value)});
}

Event tempo(Tick tick, std::uint32_t microseconds_per_quarter) {
    const std::uint8_t data[3] = {static_cast<std::uint8_t>((microseconds_per_quarter >> 16) & 0xFF),
                                  static_cast<std::uint8_t>((microseconds_per_quarter >> 8) & 0xFF),
                                  static_cast<std::uint8_t>(microseconds_per_quarter & 0xFF)};
    return meta_event(tick, meta_type::tempo, data);
}

Event time_signature(Tick tick, std::uint8_t numerator, std::uint8_t denominator_power, std::uint8_t clocks_per_click,
                     std::uint8_t thirty_seconds_per_quarter) {
    const std::uint8_t data[4] = {numerator, denominator_power, clocks_per_click, thirty_seconds_per_quarter};
    return meta_event(tick, meta_type::time_signature, data);
}

Event key_signature(Tick tick, std::int8_t sharps_flats, bool minor) {
    const std::uint8_t data[2] = {static_cast<std::uint8_t>(sharps_flats), static_cast<std::uint8_t>(minor ? 1 : 0)};
    return meta_event(tick, meta_type::key_signature, data);
}

Event text_meta(Tick tick, std::uint8_t type, const std::string& utf8_text) {
    std::vector<std::uint8_t> data(utf8_text.begin(), utf8_text.end());
    return meta_event(tick, type, data);
}

} // namespace events

} // namespace mwb
