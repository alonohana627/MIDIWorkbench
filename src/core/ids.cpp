#include "mwb/core/ids.hpp"

#include "mwb/core/hashing.hpp"

#include <uuid.h>

#include <cstring>
#include <random>

namespace mwb {

namespace {

/// One OS-backed generator per thread. libc++ implements std::random_device on Windows with
/// rand_s (RtlGenRandom) and on POSIX with the kernel random source, so every draw comes from
/// the operating system's cryptographic generator (section 18.1).
std::random_device& thread_random_device() {
    thread_local std::random_device device;
    return device;
}

} // namespace

Uuid Uuid::generate() {
    // stduuid's version-4 generator over the OS cryptographic source.
    uuids::basic_uuid_random_generator<std::random_device> generator(thread_random_device());
    const auto value = generator();
    std::array<std::uint8_t, 16> bytes{};
    const auto span = value.as_bytes();
    std::memcpy(bytes.data(), span.data(), bytes.size());
    return Uuid(bytes);
}

Result<Uuid> Uuid::parse(std::string_view text) {
    const auto parsed = uuids::uuid::from_string(text);
    if (!parsed)
        return make_error(ErrorCode::invalid_data, "malformed identity", std::string(text));
    std::array<std::uint8_t, 16> bytes{};
    const auto span = parsed->as_bytes();
    std::memcpy(bytes.data(), span.data(), bytes.size());
    return Uuid(bytes);
}

Result<Uuid> Uuid::from_bytes(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != 16)
        return make_error(ErrorCode::invalid_data, "identity must be exactly 16 bytes");
    std::array<std::uint8_t, 16> copy{};
    std::memcpy(copy.data(), bytes.data(), copy.size());
    return Uuid(copy);
}

std::string Uuid::to_string() const {
    const uuids::uuid value(bytes_.begin(), bytes_.end());
    return uuids::to_string(value);
}

std::size_t UuidHash::operator()(const Uuid& value) const noexcept {
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::memcpy(&first, value.bytes().data(), 8);
    std::memcpy(&second, value.bytes().data() + 8, 8);
    return static_cast<std::size_t>(first ^ (second * 0x9E3779B97F4A7C15ull));
}

} // namespace mwb
