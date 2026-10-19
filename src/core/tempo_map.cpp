#include "mwb/core/tempo_map.hpp"

#include "mwb/core/checked.hpp"

#include <algorithm>
#include <cstdio>
#include <tuple>

namespace mwb {

namespace {

struct TimedMeta {
    Tick tick;
    std::size_t track;
    std::uint64_t order;
    const Event* event;
};

/// Collects meta events of one type across tracks in stable merged order.
std::vector<TimedMeta> merged_meta(const Sequence& sequence, std::uint8_t type) {
    std::vector<TimedMeta> found;
    for (std::size_t track = 0; track < sequence.tracks.size(); ++track)
        for (const auto& event : sequence.tracks[track].events)
            if (event.is_meta(type))
                found.push_back({event.tick, track, event.order_key, &event});
    std::stable_sort(found.begin(), found.end(), [](const TimedMeta& a, const TimedMeta& b) {
        return std::tie(a.tick, a.track, a.order) < std::tie(b.tick, b.track, b.order);
    });
    return found;
}

/// Rounds a non-negative 128-bit fraction half away from zero.
std::int64_t round_fraction(__int128 numerator, __int128 denominator) {
    const bool negative = (numerator < 0) != (denominator < 0);
    if (numerator < 0)
        numerator = -numerator;
    if (denominator < 0)
        denominator = -denominator;
    const __int128 quotient = numerator / denominator;
    const __int128 remainder = numerator % denominator;
    const __int128 magnitude = remainder * 2 >= denominator ? quotient + 1 : quotient;
    return static_cast<std::int64_t>(negative ? -magnitude : magnitude);
}

} // namespace

TempoMap TempoMap::constant(const Division& division, std::int64_t microseconds_per_quarter) {
    TempoMap map;
    map.division_ = division;
    map.segments_.push_back(Segment{0, microseconds_per_quarter, 0});
    map.explicit_initial_ = true;
    return map;
}

TempoMap TempoMap::build(const Sequence& sequence) {
    TempoMap map;
    map.division_ = sequence.division;
    map.segments_.push_back(Segment{0, default_microseconds_per_quarter, 0});
    if (!sequence.division.is_ppq())
        return map; // Tempo meta events do not alter SMPTE timing (section 17.4).

    Tick previous_tick = -1;
    std::int64_t previous_value = -1;
    for (const auto& meta : merged_meta(sequence, meta_type::tempo)) {
        const auto bytes = meta.event->payload.bytes();
        if (bytes.size() != 3)
            continue; // Malformed tempo payloads are preserved but ignored (section 16.2).
        const std::int64_t value = (static_cast<std::int64_t>(bytes[0]) << 16) |
                                   (static_cast<std::int64_t>(bytes[1]) << 8) | bytes[2];
        if (value == 0)
            continue;
        if (meta.tick == previous_tick && value != previous_value)
            map.conflicts_ = true;
        previous_tick = meta.tick;
        previous_value = value;
        if (meta.tick == 0)
            map.explicit_initial_ = true;
        auto& last = map.segments_.back();
        if (meta.tick == last.tick) {
            last.microseconds_per_quarter = value; // Last event at an equal tick wins.
            continue;
        }
        if (value == last.microseconds_per_quarter)
            continue;
        const __int128 elapsed =
            last.elapsed_scaled + static_cast<__int128>(meta.tick - last.tick) * last.microseconds_per_quarter;
        map.segments_.push_back(Segment{meta.tick, value, elapsed});
    }
    return map;
}

std::size_t TempoMap::segment_at(Tick tick) const {
    const auto found = std::upper_bound(segments_.begin(), segments_.end(), tick,
                                        [](Tick value, const Segment& segment) { return value < segment.tick; });
    return found == segments_.begin() ? 0 : static_cast<std::size_t>(found - segments_.begin() - 1);
}

std::int64_t TempoMap::tempo_at(Tick tick) const { return segments_[segment_at(tick)].microseconds_per_quarter; }

__int128 TempoMap::elapsed_scaled(Tick tick) const {
    if (!division_.is_ppq())
        return tick;
    const auto& segment = segments_[segment_at(tick)];
    return segment.elapsed_scaled + static_cast<__int128>(tick - segment.tick) * segment.microseconds_per_quarter;
}

std::int64_t TempoMap::tick_to_microseconds(Tick tick) const {
    if (division_.is_ppq())
        return round_fraction(elapsed_scaled(tick), division_.ppq);
    const Rational seconds_per_tick = division_.smpte_seconds_per_tick();
    return round_fraction(static_cast<__int128>(tick) * seconds_per_tick.numerator * 1'000'000,
                          seconds_per_tick.denominator);
}

std::int64_t TempoMap::tick_to_frame(Tick tick, std::uint32_t sample_rate) const {
    if (division_.is_ppq())
        return round_fraction(elapsed_scaled(tick) * sample_rate, static_cast<__int128>(division_.ppq) * 1'000'000);
    const Rational seconds_per_tick = division_.smpte_seconds_per_tick();
    return round_fraction(static_cast<__int128>(tick) * seconds_per_tick.numerator * sample_rate,
                          seconds_per_tick.denominator);
}

double TempoMap::tick_to_seconds(Tick tick) const {
    if (division_.is_ppq())
        return static_cast<double>(elapsed_scaled(tick)) / (static_cast<double>(division_.ppq) * 1e6);
    return static_cast<double>(tick) * division_.smpte_seconds_per_tick().to_double();
}

Tick TempoMap::microseconds_to_tick(std::int64_t microseconds) const {
    if (microseconds <= 0)
        return 0;
    if (!division_.is_ppq()) {
        const Rational seconds_per_tick = division_.smpte_seconds_per_tick();
        // tick = us * den / (num * 1e6)
        return static_cast<Tick>(static_cast<__int128>(microseconds) * seconds_per_tick.denominator /
                                 (static_cast<__int128>(seconds_per_tick.numerator) * 1'000'000));
    }
    const __int128 target = static_cast<__int128>(microseconds) * division_.ppq;
    // Segments are ordered by elapsed time: binary search (rubato files have thousands).
    const auto after = std::upper_bound(segments_.begin() + 1, segments_.end(), target,
                                        [](__int128 value, const Segment& segment) { return value < segment.elapsed_scaled; });
    const auto& segment = *std::prev(after);
    return segment.tick + static_cast<Tick>((target - segment.elapsed_scaled) / segment.microseconds_per_quarter);
}

Tick TempoMap::frame_to_tick(std::int64_t frame, std::uint32_t sample_rate) const {
    if (frame <= 0 || sample_rate == 0)
        return 0;
    if (!division_.is_ppq()) {
        const Rational seconds_per_tick = division_.smpte_seconds_per_tick();
        return static_cast<Tick>(static_cast<__int128>(frame) * seconds_per_tick.denominator /
                                 (static_cast<__int128>(seconds_per_tick.numerator) * sample_rate));
    }
    // elapsed_scaled * sample_rate <= frame * ppq * 1e6
    const __int128 target = static_cast<__int128>(frame) * division_.ppq * 1'000'000;
    const auto after = std::upper_bound(segments_.begin() + 1, segments_.end(), target, [sample_rate](__int128 value, const Segment& segment) {
        return value < segment.elapsed_scaled * sample_rate;
    });
    const std::size_t index = static_cast<std::size_t>(std::prev(after) - segments_.begin());
    const auto& segment = segments_[index];
    const __int128 remaining = target - segment.elapsed_scaled * sample_rate;
    return segment.tick + static_cast<Tick>(remaining / (static_cast<__int128>(segment.microseconds_per_quarter) * sample_rate));
}

// --- MeterMap ---------------------------------------------------------------------------------

MeterMap MeterMap::constant(std::int32_t ppq, std::int32_t numerator, std::int32_t denominator) {
    MeterMap map;
    map.ppq_ = ppq;
    map.segments_.push_back(MeterSegment{0, numerator, denominator, 0, false});
    return map;
}

MeterMap MeterMap::build(const Sequence& sequence) {
    MeterMap map;
    map.ppq_ = sequence.division.is_ppq() ? sequence.division.ppq : default_ppq;
    map.segments_.push_back(MeterSegment{0, 4, 4, 0, false});
    for (const auto& meta : merged_meta(sequence, meta_type::time_signature)) {
        const auto bytes = meta.event->payload.bytes();
        if (bytes.size() != 4 || bytes[0] == 0 || bytes[1] > 6)
            continue; // Unsupported extremes stay as raw evidence (section 16.2).
        const std::int32_t numerator = bytes[0];
        const std::int32_t denominator = 1 << bytes[1];
        auto& last = map.segments_.back();
        if (meta.tick == last.tick) {
            last.numerator = numerator;
            last.denominator = denominator;
            continue;
        }
        const Tick bar_length = map.ticks_per_bar(last);
        const Tick offset = meta.tick - last.tick;
        const std::int64_t whole_bars = bar_length > 0 ? offset / bar_length : 0;
        const bool partial = bar_length > 0 && offset % bar_length != 0;
        MeterSegment next{meta.tick, numerator, denominator, last.first_bar + whole_bars + (partial ? 1 : 0), partial};
        map.segments_.push_back(next);
    }
    return map;
}

const MeterSegment& MeterMap::segment_at(Tick tick) const {
    const auto found = std::upper_bound(segments_.begin(), segments_.end(), tick,
                                        [](Tick value, const MeterSegment& segment) { return value < segment.tick; });
    return found == segments_.begin() ? segments_.front() : *(found - 1);
}

Tick MeterMap::ticks_per_bar(const MeterSegment& segment) const {
    // At least one tick: low PPQ with a large denominator would otherwise make every bar start at
    // the same tick and bar loops never advance.
    return std::max<Tick>(1, mul_div_floor(static_cast<std::int64_t>(segment.numerator) * 4, ppq_, segment.denominator).value_or(1));
}

Tick MeterMap::ticks_per_beat(const MeterSegment& segment) const {
    return std::max<Tick>(1, mul_div_floor(4, ppq_, segment.denominator).value_or(1));
}

Tick MeterMap::bar_start(std::int64_t bar) const {
    const MeterSegment* active = &segments_.front();
    for (const auto& segment : segments_) {
        if (segment.first_bar > bar)
            break;
        active = &segment;
    }
    const std::int64_t offset_bars = bar - active->first_bar;
    // Each bar start is derived from the segment origin, never accumulated (no drift).
    return active->tick +
           mul_div_floor(offset_bars * active->numerator * 4, ppq_, active->denominator).value_or(0);
}

BarBeatTick MeterMap::position(Tick tick) const {
    const auto& segment = segment_at(tick);
    const Tick bar_length = ticks_per_bar(segment);
    const Tick beat_length = ticks_per_beat(segment);
    if (bar_length <= 0 || beat_length <= 0)
        return BarBeatTick{segment.first_bar, 0, tick - segment.tick};
    const Tick offset = tick - segment.tick;
    std::int64_t bar_in_segment = offset / bar_length;
    // Recompute against the exact rational bar start to avoid drift for unusual divisions.
    Tick start = bar_start(segment.first_bar + bar_in_segment);
    if (start > tick && bar_in_segment > 0) {
        --bar_in_segment;
        start = bar_start(segment.first_bar + bar_in_segment);
    }
    const Tick in_bar = tick - start;
    return BarBeatTick{segment.first_bar + bar_in_segment, in_bar / beat_length, in_bar % beat_length};
}

std::string MeterMap::format(Tick tick) const {
    const auto position_value = position(tick);
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%lld.%lld.%03lld", static_cast<long long>(position_value.bar + 1),
                  static_cast<long long>(position_value.beat + 1), static_cast<long long>(position_value.tick_in_beat));
    return buffer;
}

} // namespace mwb
