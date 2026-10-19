#pragma once

#include "mwb/core/error.hpp"

#include <compare>
#include <cstdint>
#include <numeric>
#include <optional>
#include <string>

namespace mwb {

/// Sequence-relative musical position in ticks (specification section 17.1). Stored event
/// times are never negative; signed arithmetic keeps displacements simple.
using Tick = std::int64_t;

/// Monotonically increasing document state counter.
using Generation = std::uint64_t;

/// Default resolution of new sequences.
inline constexpr std::int32_t default_ppq = 960;

/// Default tempo before any tempo event: 120 quarter-note BPM.
inline constexpr std::int64_t default_microseconds_per_quarter = 500000;

/// Half-open tick range [begin, end).
struct TimeRange {
    Tick begin = 0;
    Tick end = 0;

    [[nodiscard]] constexpr Tick length() const { return end - begin; }
    [[nodiscard]] constexpr bool empty() const { return end <= begin; }
    [[nodiscard]] constexpr bool contains(Tick tick) const { return tick >= begin && tick < end; }
    [[nodiscard]] constexpr bool overlaps(const TimeRange& other) const {
        return begin < other.end && other.begin < end;
    }
    friend constexpr bool operator==(const TimeRange&, const TimeRange&) = default;
};

/// Exact non-negative-denominator fraction used for grids, swing ratios and stretch factors.
struct Rational {
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;

    /// Normalized fraction; denominator zero is rejected.
    [[nodiscard]] static Result<Rational> make(std::int64_t numerator, std::int64_t denominator);
    /// Parses "a/b" or an integer.
    [[nodiscard]] static Result<Rational> parse(const std::string& text);

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] double to_double() const {
        return static_cast<double>(numerator) / static_cast<double>(denominator);
    }
    friend constexpr bool operator==(const Rational&, const Rational&) = default;
};

/// SMPTE frame-rate codes as stored in the SMF division word (section 17.4).
enum class SmpteFormat : std::int8_t {
    fps24 = -24,
    fps25 = -25,
    fps29_97_drop = -29, ///< 30000/1001 frames per second.
    fps30 = -30,
};

/// One sequence's time division: PPQ or SMPTE frames with ticks per frame.
struct Division {
    enum class Kind : std::uint8_t { ppq, smpte };

    Kind kind = Kind::ppq;
    std::int32_t ppq = default_ppq;
    SmpteFormat smpte_format = SmpteFormat::fps30;
    std::int32_t ticks_per_frame = 0;

    [[nodiscard]] static Division make_ppq(std::int32_t ppq) { return Division{Kind::ppq, ppq, SmpteFormat::fps30, 0}; }
    [[nodiscard]] static Division make_smpte(SmpteFormat format, std::int32_t ticks_per_frame) {
        return Division{Kind::smpte, 0, format, ticks_per_frame};
    }
    [[nodiscard]] bool is_ppq() const { return kind == Kind::ppq; }
    /// Validates the ranges of section 23.1 (PPQ 1..32767, ticks/frame 1..255).
    [[nodiscard]] Status validate() const;
    /// Exact tick duration in seconds for SMPTE divisions as numerator/denominator.
    [[nodiscard]] Rational smpte_seconds_per_tick() const;
    /// 16-bit SMF division word.
    [[nodiscard]] std::uint16_t smf_word() const;
    friend bool operator==(const Division&, const Division&) = default;
};

/// Frames per second of an SMPTE format as an exact fraction.
[[nodiscard]] Rational smpte_frames_per_second(SmpteFormat format);

} // namespace mwb
