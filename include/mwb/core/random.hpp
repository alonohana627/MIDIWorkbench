#pragma once

#include <cstdint>
#include <memory>

namespace mwb {

/// Deterministic PCG32 generator (pcg-cpp, section 10.2) with a fixed initialization:
/// state seeded from `seed` on the fixed stream 0x4D5742. The bounded-integer mapping is
/// specified here rather than relying on std::uniform_int_distribution, so sequences are
/// identical across standard libraries.
class DeterministicRandom {
public:
    explicit DeterministicRandom(std::uint64_t seed);
    ~DeterministicRandom();
    DeterministicRandom(const DeterministicRandom&) = delete;
    DeterministicRandom& operator=(const DeterministicRandom&) = delete;

    /// Next raw 32-bit output.
    [[nodiscard]] std::uint32_t next_u32();
    /// Uniform integer in [0, bound) by multiply-shift with rejection (Lemire). Bound 0 returns 0.
    [[nodiscard]] std::uint32_t bounded(std::uint32_t bound);
    /// Uniform integer in [low, high] inclusive (high - low must fit in 32 bits).
    [[nodiscard]] std::int64_t range(std::int64_t low, std::int64_t high);
    /// Triangular integer in [-spread, spread]: sum of two draws in [0, spread] minus spread.
    [[nodiscard]] std::int64_t triangular(std::int64_t spread);
    /// Uniform double in [0,1) from 32 bits (DSP dither only).
    [[nodiscard]] double unit();

    static constexpr const char* algorithm_name = "pcg32";
    static constexpr int algorithm_version = 1;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace mwb
