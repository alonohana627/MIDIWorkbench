#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/time.hpp"

#include <cstdint>
#include <utility>

namespace mwb::music {

/// A rhythmic grid (specification sections 7.4 and 10.1): a step length expressed as a fraction
/// of a whole note, a paired-subdivision swing ratio and an origin tick. With swing ratio `r`
/// each pair of steps spans `2d` ticks and its second point lies at `2d*r` (r = 1/2 is straight,
/// r = 2/3 is triplet swing). Pair boundaries are anchored to the origin, never to notes.
struct GridSpec {
    Rational step{1, 16};  ///< Fraction of a whole note; 1/16 is a sixteenth note.
    Rational swing{1, 2};  ///< Second point of each pair as a fraction of the pair span.
    Tick origin = 0;       ///< Grid origin tick (point index 0).
    friend bool operator==(const GridSpec&, const GridSpec&) = default;
};

/// Grid points computed from exact rational positions. Every point is derived independently
/// from the origin, so rounding never accumulates (section 7.4). Point index 2k is the start of
/// pair k; index 2k+1 is its swung second point. Indices may be negative (before the origin).
class Grid {
public:
    /// Validates the specification for a PPQ resolution: step > 0, 0 < swing < 1, PPQ > 0, and
    /// that exact arithmetic on realistic ticks stays inside 64-bit numerators.
    [[nodiscard]] static Result<Grid> make(std::int32_t ppq, const GridSpec& spec);

    [[nodiscard]] const GridSpec& spec() const noexcept { return spec_; }
    /// Exact rational tick of point `index` (normalized fraction, positive denominator).
    [[nodiscard]] Rational point(std::int64_t index) const noexcept;
    /// Point rounded to a tick, half away from zero (section 10.1).
    [[nodiscard]] Tick point_tick(std::int64_t index) const noexcept;
    /// Index of the point nearest to `tick`; exact distance ties choose the earlier point.
    /// Points before tick 0 are never chosen when a non-negative point exists.
    [[nodiscard]] std::int64_t nearest_index(Tick tick) const noexcept;
    /// Index of the last point at or before `tick`.
    [[nodiscard]] std::int64_t floor_index(Tick tick) const noexcept;
    /// True when every point falls on an integer tick (otherwise the UI shows
    /// "Rounded to project ticks").
    [[nodiscard]] bool is_exact() const noexcept;
    /// Exact length of one unswung step in ticks.
    [[nodiscard]] Rational step_ticks() const noexcept;

private:
    Grid() = default;
    /// Numerator of point `index` over the common denominator `denominator_`, relative to origin.
    [[nodiscard]] __int128 relative_numerator(std::int64_t index) const noexcept;

    GridSpec spec_;
    std::int64_t denominator_ = 1;   ///< Common denominator Q of every point.
    std::int64_t pair_span_ = 1;     ///< Pair span 2d scaled by Q.
    std::int64_t swing_offset_ = 1;  ///< Second point offset 2d*r scaled by Q.
};

/// Normalizes a fraction held in 128-bit integers; fails when the denominator is zero or the
/// reduced fraction does not fit in 64 bits.
[[nodiscard]] Result<Rational> make_rational(__int128 numerator, __int128 denominator);

/// Rounds an exact fraction to the nearest integer tick, half away from zero.
[[nodiscard]] Tick round_rational(const Rational& value) noexcept;

/// Compares two fractions exactly: negative, zero or positive like operator<=>.
[[nodiscard]] int compare_rational(const Rational& left, const Rational& right) noexcept;

/// Exact t + s * (g - t) for tick t, exact target g and strength s (section 10.1), rounded half
/// away from zero; returns the rounded tick and its rounding error.
[[nodiscard]] Result<std::pair<Tick, Rational>> move_toward(Tick tick, const Rational& target,
                                                            const Rational& strength);

/// Absolute difference between an exact fraction and an integer tick, as a fraction.
[[nodiscard]] Rational rounding_error(const Rational& exact, Tick rounded) noexcept;

} // namespace mwb::music
