#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/time.hpp"

#include <nlohmann/json.hpp>

#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mwb::music {

/// Reads optional fields of a versioned JSON parameter object without exceptions. A missing
/// field keeps the caller's default; a field of the wrong type or out of the target integer
/// range records the first error, which status() reports. Used by recipe deserialization.
class FieldReader {
public:
    static constexpr std::int64_t max_parameter_magnitude = std::int64_t{1} << 40;

    /// The object must outlive the reader. A non-object input records an error immediately.
    explicit FieldReader(const nlohmann::json& object, std::string context = {});

    /// Reads an integer into any integral type, checking that the value fits.
    template <std::integral Integer>
    void integer(std::string_view key, Integer& out) {
        std::int64_t value = 0;
        if (!read_int64(key, value))
            return;
        // Recipe magnitudes stay far below int64 overflow: tick offsets and sums of a few
        // parameters can then never overflow (2^40 ticks is centuries at any PPQ).
        if (!std::in_range<Integer>(value) || value > max_parameter_magnitude || value < -max_parameter_magnitude) {
            fail(key, "integer out of range");
            return;
        }
        out = static_cast<Integer>(value);
    }
    /// Reads an optional integer; JSON null clears it.
    void optional_integer(std::string_view key, std::optional<int>& out);
    void boolean(std::string_view key, bool& out);
    void text(std::string_view key, std::string& out);
    /// Fractions are stored as "numerator/denominator" strings.
    void rational(std::string_view key, Rational& out);
    void integer_list(std::string_view key, std::vector<std::int64_t>& out);
    void integer_list(std::string_view key, std::vector<int>& out);
    /// Reads an enumeration stored by name.
    template <class Enumeration>
    void enumeration(std::string_view key, Enumeration& out,
                     std::span<const std::pair<std::string_view, Enumeration>> names) {
        std::string name;
        if (!present(key))
            return;
        text(key, name);
        if (!status_)
            return;
        for (const auto& [candidate, value] : names) {
            if (candidate == name) {
                out = value;
                return;
            }
        }
        fail(key, "unknown value \"" + name + "\"");
    }
    /// Nested object, or nullptr when absent or of the wrong type (an error is recorded for the
    /// wrong type).
    [[nodiscard]] const nlohmann::json* object(std::string_view key);
    /// Nested array, or nullptr when absent (an error is recorded for the wrong type).
    [[nodiscard]] const nlohmann::json* array(std::string_view key);
    /// True when the key exists (even if null).
    [[nodiscard]] bool present(std::string_view key) const;
    /// True when the key exists and holds JSON null.
    [[nodiscard]] bool is_null(std::string_view key) const;

    /// First recorded error, or success.
    [[nodiscard]] Status status() const { return status_; }
    /// Records an error for a semantic problem detected by the caller.
    void fail(std::string_view key, std::string message);

private:
    bool read_int64(std::string_view key, std::int64_t& value);
    [[nodiscard]] const nlohmann::json* find(std::string_view key) const;

    const nlohmann::json& object_;
    std::string context_;
    Status status_;
};

/// Name lookup for writing an enumeration value; returns "" when not listed.
template <class Enumeration>
[[nodiscard]] std::string_view enumeration_name(Enumeration value,
                                                std::span<const std::pair<std::string_view, Enumeration>> names) {
    for (const auto& [name, candidate] : names)
        if (candidate == value)
            return name;
    return {};
}

/// Parses JSON text without exceptions.
[[nodiscard]] Result<nlohmann::json> parse_json_object(std::string_view text);

} // namespace mwb::music
