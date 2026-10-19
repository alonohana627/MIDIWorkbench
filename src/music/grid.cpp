#include "mwb/music/grid.hpp"

#include "mwb/core/checked.hpp"

#include <limits>

namespace mwb::music {

namespace {

constexpr __int128 int64_max = std::numeric_limits<std::int64_t>::max();
constexpr __int128 int64_min = std::numeric_limits<std::int64_t>::min();

/// Largest common denominator accepted for grid points; keeps tick * Q far inside 128 bits.
constexpr std::int64_t max_grid_denominator = std::int64_t{1} << 24;

/// Absolute value of a 128-bit integer.
__int128 absolute(__int128 value) { return value < 0 ? -value : value; }

/// Euclid on 128-bit magnitudes.
__int128 greatest_common_divisor(__int128 left, __int128 right) {
    left = absolute(left);
    right = absolute(right);
    while (right != 0) {
        const __int128 remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

/// Floor division for a positive divisor.
__int128 floor_divide(__int128 numerator, __int128 divisor) {
    __int128 quotient = numerator / divisor;
    if (numerator % divisor != 0 && numerator < 0)
        --quotient;
    return quotient;
}

/// True when a 128-bit value fits in 64 bits.
bool fits_int64(__int128 value) { return value <= int64_max && value >= int64_min; }

} // namespace

Result<Rational> make_rational(__int128 numerator, __int128 denominator) {
    if (denominator == 0)
        return make_error(ErrorCode::invalid_argument, "fraction denominator must not be zero");
    if (denominator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    const __int128 divisor = greatest_common_divisor(numerator, denominator);
    if (divisor > 1) {
        numerator /= divisor;
        denominator /= divisor;
    }
    if (!fits_int64(numerator) || !fits_int64(denominator))
        return make_error(ErrorCode::limit_exceeded, "exact fraction exceeds 64-bit range");
    return Rational{static_cast<std::int64_t>(numerator), static_cast<std::int64_t>(denominator)};
}

Tick round_rational(const Rational& value) noexcept {
    const auto rounded = mul_div_round(value.numerator, 1, value.denominator);
    return rounded.value_or(0);
}

int compare_rational(const Rational& left, const Rational& right) noexcept {
    const __int128 a = static_cast<__int128>(left.numerator) * right.denominator;
    const __int128 b = static_cast<__int128>(right.numerator) * left.denominator;
    const bool flip = (left.denominator < 0) != (right.denominator < 0);
    if (a == b)
        return 0;
    return ((a < b) != flip) ? -1 : 1;
}

Rational rounding_error(const Rational& exact, Tick rounded) noexcept {
    const __int128 difference =
        absolute(static_cast<__int128>(exact.numerator) - static_cast<__int128>(rounded) * exact.denominator);
    auto result = make_rational(difference, exact.denominator);
    return result ? *result : Rational{0, 1};
}

Result<std::pair<Tick, Rational>> move_toward(Tick tick, const Rational& target, const Rational& strength) {
    const __int128 denominator = static_cast<__int128>(target.denominator) * strength.denominator;
    const __int128 numerator =
        static_cast<__int128>(tick) * denominator +
        static_cast<__int128>(strength.numerator) *
            (static_cast<__int128>(target.numerator) - static_cast<__int128>(tick) * target.denominator);
    auto exact = make_rational(numerator, denominator);
    if (!exact)
        return std::unexpected(exact.error());
    const Tick rounded = round_rational(*exact);
    return std::pair{rounded, rounding_error(*exact, rounded)};
}

Result<Grid> Grid::make(std::int32_t ppq, const GridSpec& spec) {
    if (ppq <= 0)
        return make_error(ErrorCode::invalid_argument, "grid requires a positive PPQ resolution");
    if (spec.step.denominator <= 0 || spec.step.numerator <= 0)
        return make_error(ErrorCode::invalid_argument, "grid step must be a positive fraction of a whole note");
    if (spec.swing.denominator <= 0 || spec.swing.numerator <= 0 || spec.swing.numerator >= spec.swing.denominator)
        return make_error(ErrorCode::invalid_argument, "swing ratio must lie strictly between 0 and 1");
    if (spec.origin < 0)
        return make_error(ErrorCode::invalid_argument, "grid origin must not be negative");
    const __int128 denominator = static_cast<__int128>(spec.step.denominator) * spec.swing.denominator;
    // Pair span 2d where d = 4 * PPQ * step ticks; scaled by Q = step.den * swing.den.
    const __int128 pair_span = static_cast<__int128>(8) * ppq * spec.step.numerator * spec.swing.denominator;
    const __int128 swing_offset = static_cast<__int128>(8) * ppq * spec.step.numerator * spec.swing.numerator;
    if (denominator > max_grid_denominator || !fits_int64(pair_span))
        return make_error(ErrorCode::limit_exceeded, "grid fraction is too fine for exact arithmetic");
    Grid grid;
    grid.spec_ = spec;
    grid.denominator_ = static_cast<std::int64_t>(denominator);
    grid.pair_span_ = static_cast<std::int64_t>(pair_span);
    grid.swing_offset_ = static_cast<std::int64_t>(swing_offset);
    return grid;
}

__int128 Grid::relative_numerator(std::int64_t index) const noexcept {
    const __int128 pair = floor_divide(index, 2);
    const __int128 odd = static_cast<__int128>(index) - 2 * pair;
    return pair * pair_span_ + odd * swing_offset_;
}

Rational Grid::point(std::int64_t index) const noexcept {
    const __int128 numerator = static_cast<__int128>(spec_.origin) * denominator_ + relative_numerator(index);
    auto value = make_rational(numerator, denominator_);
    return value ? *value : Rational{0, 1};
}

Tick Grid::point_tick(std::int64_t index) const noexcept { return round_rational(point(index)); }

std::int64_t Grid::floor_index(Tick tick) const noexcept {
    const __int128 relative = (static_cast<__int128>(tick) - spec_.origin) * denominator_;
    const __int128 pair = floor_divide(relative, pair_span_);
    const __int128 within = relative - pair * pair_span_;
    return static_cast<std::int64_t>(2 * pair + (within >= swing_offset_ ? 1 : 0));
}

std::int64_t Grid::nearest_index(Tick tick) const noexcept {
    const __int128 relative = (static_cast<__int128>(tick) - spec_.origin) * denominator_;
    const std::int64_t first = floor_index(tick);
    const __int128 absolute_origin = static_cast<__int128>(spec_.origin) * denominator_;
    std::int64_t best = first;
    __int128 best_distance = -1;
    // The floor point and the next point bracket the tick; iterate in increasing order so an
    // exact tie keeps the earlier point.
    for (std::int64_t candidate = first; candidate <= first + 1; ++candidate) {
        const __int128 position = relative_numerator(candidate);
        if (absolute_origin + position < 0)
            continue;
        const __int128 distance = absolute(position - relative);
        if (best_distance < 0 || distance < best_distance) {
            best = candidate;
            best_distance = distance;
        }
    }
    return best;
}

bool Grid::is_exact() const noexcept {
    return pair_span_ % denominator_ == 0 && swing_offset_ % denominator_ == 0;
}

Rational Grid::step_ticks() const noexcept {
    auto value = make_rational(static_cast<__int128>(pair_span_), static_cast<__int128>(denominator_) * 2);
    return value ? *value : Rational{0, 1};
}

} // namespace mwb::music
