#pragma once

#include "mwb/core/error.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace mwb {

/// A random 128-bit identity (specification section 18.1). Stored as 16 bytes in SQLite and as
/// the canonical lowercase hex form "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" in JSON.
class Uuid {
public:
    /// The all-zero value, used only as "no identity".
    constexpr Uuid() = default;
    explicit constexpr Uuid(const std::array<std::uint8_t, 16>& bytes) : bytes_(bytes) {}

    /// New random version-4 identity from the operating system's cryptographic source.
    [[nodiscard]] static Uuid generate();
    /// Parses the canonical string form; braces and uppercase are accepted.
    [[nodiscard]] static Result<Uuid> parse(std::string_view text);
    /// Builds an identity from exactly 16 bytes.
    [[nodiscard]] static Result<Uuid> from_bytes(std::span<const std::uint8_t> bytes);

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] constexpr const std::array<std::uint8_t, 16>& bytes() const { return bytes_; }
    [[nodiscard]] constexpr bool is_nil() const {
        for (auto byte : bytes_)
            if (byte != 0)
                return false;
        return true;
    }

    friend constexpr auto operator<=>(const Uuid&, const Uuid&) = default;

private:
    std::array<std::uint8_t, 16> bytes_{};
};

/// Strongly typed identity so a TrackId cannot be passed where an EventId is expected.
template <class Tag>
class TypedId {
public:
    constexpr TypedId() = default;
    explicit constexpr TypedId(Uuid value) : value_(value) {}

    [[nodiscard]] static TypedId generate() { return TypedId(Uuid::generate()); }
    [[nodiscard]] constexpr const Uuid& uuid() const { return value_; }
    [[nodiscard]] std::string to_string() const { return value_.to_string(); }
    [[nodiscard]] constexpr bool is_nil() const { return value_.is_nil(); }

    friend constexpr auto operator<=>(const TypedId&, const TypedId&) = default;

private:
    Uuid value_;
};

struct DocumentIdTag {};
struct SequenceIdTag {};
struct TrackIdTag {};
struct EventIdTag {};
struct RegionIdTag {};
struct TakeIdTag {};
struct RecipeIdTag {};

using DocumentId = TypedId<DocumentIdTag>;
using SequenceId = TypedId<SequenceIdTag>;
using TrackId = TypedId<TrackIdTag>;
using EventId = TypedId<EventIdTag>;
using RegionId = TypedId<RegionIdTag>;
using TakeId = TypedId<TakeIdTag>;
using RecipeId = TypedId<RecipeIdTag>;

/// Hash functor for unordered containers keyed by identities.
struct UuidHash {
    [[nodiscard]] std::size_t operator()(const Uuid& value) const noexcept;
    template <class Tag>
    [[nodiscard]] std::size_t operator()(const TypedId<Tag>& value) const noexcept {
        return (*this)(value.uuid());
    }
};

} // namespace mwb
