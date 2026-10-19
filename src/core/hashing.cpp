#include "mwb/core/hashing.hpp"

#include <picosha2.h>
#include <xxhash.h>

namespace mwb {

Sha256Digest sha256(std::span<const std::uint8_t> bytes) {
    Sha256Digest digest{};
    picosha2::hash256(bytes.begin(), bytes.end(), digest.begin(), digest.end());
    return digest;
}

Sha256Digest sha256(std::string_view text) {
    Sha256Digest digest{};
    picosha2::hash256(text.begin(), text.end(), digest.begin(), digest.end());
    return digest;
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        text.push_back(digits[byte >> 4]);
        text.push_back(digits[byte & 0x0F]);
    }
    return text;
}

bool parse_sha256_hex(std::string_view text, Sha256Digest& digest) {
    if (text.size() != 64)
        return false;
    const auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9')
            return character - '0';
        if (character >= 'a' && character <= 'f')
            return character - 'a' + 10;
        if (character >= 'A' && character <= 'F')
            return character - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < 32; ++index) {
        const int high = nibble(text[2 * index]);
        const int low = nibble(text[2 * index + 1]);
        if (high < 0 || low < 0)
            return false;
        digest[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

struct Sha256Builder::State {
    picosha2::hash256_one_by_one hasher;
};

Sha256Builder::Sha256Builder() : state_(new State) { state_->hasher.init(); }

Sha256Builder::~Sha256Builder() { delete state_; }

void Sha256Builder::update(std::span<const std::uint8_t> bytes) { state_->hasher.process(bytes.begin(), bytes.end()); }

void Sha256Builder::update(std::string_view text) { state_->hasher.process(text.begin(), text.end()); }

void Sha256Builder::update_u64(std::uint64_t value) {
    std::uint8_t bytes[8];
    for (int index = 0; index < 8; ++index)
        bytes[index] = static_cast<std::uint8_t>(value >> (8 * index));
    update(std::span<const std::uint8_t>(bytes, 8));
}

void Sha256Builder::update_i64(std::int64_t value) { update_u64(static_cast<std::uint64_t>(value)); }

void Sha256Builder::update_u32(std::uint32_t value) {
    std::uint8_t bytes[4];
    for (int index = 0; index < 4; ++index)
        bytes[index] = static_cast<std::uint8_t>(value >> (8 * index));
    update(std::span<const std::uint8_t>(bytes, 4));
}

void Sha256Builder::update_u8(std::uint8_t value) { update(std::span<const std::uint8_t>(&value, 1)); }

void Sha256Builder::update_string(std::string_view text) {
    update_u64(text.size());
    update(text);
}

Sha256Digest Sha256Builder::finish() {
    state_->hasher.finish();
    Sha256Digest digest{};
    state_->hasher.get_hash_bytes(digest.begin(), digest.end());
    return digest;
}

std::uint64_t xxh3_64(std::span<const std::uint8_t> bytes, std::uint64_t seed) {
    return XXH3_64bits_withSeed(bytes.data(), bytes.size(), seed);
}

} // namespace mwb
