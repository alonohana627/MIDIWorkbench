#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace mwb {

/// SHA-256 digest (picosha2, section 4.4.1).
using Sha256Digest = std::array<std::uint8_t, 32>;

/// SHA-256 of a byte range.
[[nodiscard]] Sha256Digest sha256(std::span<const std::uint8_t> bytes);
/// SHA-256 of text bytes.
[[nodiscard]] Sha256Digest sha256(std::string_view text);
/// Lowercase hexadecimal form of a digest.
[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> bytes);
/// Parses 64 hex characters into a digest; returns false on malformed input.
[[nodiscard]] bool parse_sha256_hex(std::string_view text, Sha256Digest& digest);

/// Incremental SHA-256 for large or streamed content.
class Sha256Builder {
public:
    Sha256Builder();
    ~Sha256Builder();
    Sha256Builder(const Sha256Builder&) = delete;
    Sha256Builder& operator=(const Sha256Builder&) = delete;

    void update(std::span<const std::uint8_t> bytes);
    void update(std::string_view text);
    void update_u64(std::uint64_t value);      ///< Little-endian, fixed width.
    void update_i64(std::int64_t value);       ///< Little-endian two's complement.
    void update_u32(std::uint32_t value);
    void update_u8(std::uint8_t value);
    void update_string(std::string_view text); ///< Length-prefixed.
    [[nodiscard]] Sha256Digest finish();

private:
    struct State;
    State* state_;
};

/// Fixed seed for XXH3-64 keys (motif n-grams, cache keys, IPC checksums).
inline constexpr std::uint64_t xxh3_seed = 0x4D57423131313131ull;

/// XXH3-64 of a byte range with the project seed (xxHash, section 4.4.1).
[[nodiscard]] std::uint64_t xxh3_64(std::span<const std::uint8_t> bytes, std::uint64_t seed = xxh3_seed);

} // namespace mwb
