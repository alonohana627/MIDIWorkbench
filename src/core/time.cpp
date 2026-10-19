#include "mwb/core/time.hpp"

#include <charconv>
#include <cstdlib>
#include <limits>
#include <numeric>

namespace mwb {

Result<Rational> Rational::make(std::int64_t numerator, std::int64_t denominator) {
    if (denominator == 0)
        return make_error(ErrorCode::invalid_argument, "fraction denominator must not be zero");
    if (numerator == std::numeric_limits<std::int64_t>::min() || denominator == std::numeric_limits<std::int64_t>::min())
        return make_error(ErrorCode::limit_exceeded, "fraction is out of range"); // negation would overflow
    if (denominator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    const std::int64_t divisor = std::gcd(numerator < 0 ? -numerator : numerator, denominator);
    if (divisor > 1) {
        numerator /= divisor;
        denominator /= divisor;
    }
    return Rational{numerator, denominator};
}

Result<Rational> Rational::parse(const std::string& text) {
    const auto slash = text.find('/');
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;
    const auto parse_part = [](std::string_view part, std::int64_t& value) {
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        return error == std::errc{} && end == part.data() + part.size() && !part.empty();
    };
    const std::string_view view(text);
    if (slash == std::string::npos) {
        if (!parse_part(view, numerator))
            return make_error(ErrorCode::invalid_argument, "malformed fraction", text);
    } else if (!parse_part(view.substr(0, slash), numerator) ||
               !parse_part(view.substr(slash + 1), denominator)) {
        return make_error(ErrorCode::invalid_argument, "malformed fraction", text);
    }
    return make(numerator, denominator);
}

std::string Rational::to_string() const {
    if (denominator == 1)
        return std::to_string(numerator);
    return std::to_string(numerator) + "/" + std::to_string(denominator);
}

Status Division::validate() const {
    if (kind == Kind::ppq) {
        if (ppq < 1 || ppq > 32767)
            return make_error(ErrorCode::invalid_data, "PPQ must be between 1 and 32767");
        return ok();
    }
    switch (smpte_format) {
    case SmpteFormat::fps24:
    case SmpteFormat::fps25:
    case SmpteFormat::fps29_97_drop:
    case SmpteFormat::fps30: break;
    default: return make_error(ErrorCode::invalid_data, "unknown SMPTE frame-rate code");
    }
    if (ticks_per_frame < 1 || ticks_per_frame > 255)
        return make_error(ErrorCode::invalid_data, "SMPTE ticks per frame must be between 1 and 255");
    return ok();
}

Rational smpte_frames_per_second(SmpteFormat format) {
    switch (format) {
    case SmpteFormat::fps24: return Rational{24, 1};
    case SmpteFormat::fps25: return Rational{25, 1};
    case SmpteFormat::fps29_97_drop: return Rational{30000, 1001};
    case SmpteFormat::fps30: return Rational{30, 1};
    }
    return Rational{30, 1};
}

Rational Division::smpte_seconds_per_tick() const {
    const Rational fps = smpte_frames_per_second(smpte_format);
    // seconds per tick = 1 / (fps * ticks_per_frame) = den / (num * tpf)
    return Rational{fps.denominator, fps.numerator * ticks_per_frame};
}

std::uint16_t Division::smf_word() const {
    if (kind == Kind::ppq)
        return static_cast<std::uint16_t>(ppq);
    const auto high = static_cast<std::uint8_t>(static_cast<std::int8_t>(smpte_format));
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(high) << 8) |
                                      static_cast<std::uint16_t>(ticks_per_frame & 0xFF));
}

} // namespace mwb
