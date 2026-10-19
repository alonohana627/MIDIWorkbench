#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace mwb {

/// Exact (a * b) / c with a 128-bit intermediate, rounding toward negative infinity.
/// Returns nullopt on division by zero or when the result does not fit in 64 bits
/// (specification section 17.1: checked 128-bit intermediate multiplication).
[[nodiscard]] constexpr std::optional<std::int64_t> mul_div_floor(std::int64_t a, std::int64_t b,
                                                                  std::int64_t c) noexcept {
    if (c == 0)
        return std::nullopt;
    const __int128 product = static_cast<__int128>(a) * b;
    __int128 quotient = product / c;
    if ((product % c != 0) && ((product < 0) != (c < 0)))
        --quotient;
    if (quotient > std::numeric_limits<std::int64_t>::max() ||
        quotient < std::numeric_limits<std::int64_t>::min())
        return std::nullopt;
    return static_cast<std::int64_t>(quotient);
}

/// Exact (a * b) / c rounded to nearest, half away from zero (section 10.1 tie rule).
[[nodiscard]] constexpr std::optional<std::int64_t> mul_div_round(std::int64_t a, std::int64_t b,
                                                                  std::int64_t c) noexcept {
    if (c == 0)
        return std::nullopt;
    __int128 numerator = static_cast<__int128>(a) * b;
    __int128 denominator = c;
    if (denominator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    const bool negative = numerator < 0;
    const __int128 magnitude = negative ? -numerator : numerator;
    const __int128 floor_part = magnitude / denominator;
    const __int128 remainder = magnitude % denominator;
    // An exact half rounds up in magnitude, which is away from zero.
    const __int128 result_magnitude = (remainder * 2 >= denominator) ? floor_part + 1 : floor_part;
    const __int128 result = negative ? -result_magnitude : result_magnitude;
    if (result > std::numeric_limits<std::int64_t>::max() ||
        result < std::numeric_limits<std::int64_t>::min())
        return std::nullopt;
    return static_cast<std::int64_t>(result);
}

/// Checked addition.
[[nodiscard]] constexpr std::optional<std::int64_t> checked_add(std::int64_t a,
                                                                std::int64_t b) noexcept {
    std::int64_t result = 0;
    if (__builtin_add_overflow(a, b, &result))
        return std::nullopt;
    return result;
}

/// Checked subtraction.
[[nodiscard]] constexpr std::optional<std::int64_t> checked_sub(std::int64_t a,
                                                                std::int64_t b) noexcept {
    std::int64_t result = 0;
    if (__builtin_sub_overflow(a, b, &result))
        return std::nullopt;
    return result;
}

/// Checked multiplication.
[[nodiscard]] constexpr std::optional<std::int64_t> checked_mul(std::int64_t a,
                                                                std::int64_t b) noexcept {
    std::int64_t result = 0;
    if (__builtin_mul_overflow(a, b, &result))
        return std::nullopt;
    return result;
}

} // namespace mwb
