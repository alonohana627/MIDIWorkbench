#include "mwb/core/random.hpp"

#include <pcg_random.hpp>

namespace mwb {

struct DeterministicRandom::State {
    pcg32 generator;
};

DeterministicRandom::DeterministicRandom(std::uint64_t seed)
    : state_(std::make_unique<State>(State{pcg32(seed, 0x4D5742u)})) {}

DeterministicRandom::~DeterministicRandom() = default;

std::uint32_t DeterministicRandom::next_u32() { return static_cast<std::uint32_t>(state_->generator()); }

std::uint32_t DeterministicRandom::bounded(std::uint32_t bound) {
    if (bound == 0)
        return 0;
    // Lemire's nearly divisionless method with an explicit rejection threshold.
    std::uint64_t product = static_cast<std::uint64_t>(next_u32()) * bound;
    auto low = static_cast<std::uint32_t>(product);
    if (low < bound) {
        const std::uint32_t threshold = static_cast<std::uint32_t>(-bound) % bound;
        while (low < threshold) {
            product = static_cast<std::uint64_t>(next_u32()) * bound;
            low = static_cast<std::uint32_t>(product);
        }
    }
    return static_cast<std::uint32_t>(product >> 32);
}

std::int64_t DeterministicRandom::range(std::int64_t low, std::int64_t high) {
    if (high <= low)
        return low;
    const auto span = static_cast<std::uint64_t>(high - low) + 1;
    if (span > 0xFFFFFFFFull)
        return low + static_cast<std::int64_t>(next_u32());
    return low + static_cast<std::int64_t>(bounded(static_cast<std::uint32_t>(span)));
}

std::int64_t DeterministicRandom::triangular(std::int64_t spread) {
    if (spread <= 0)
        return 0;
    return range(0, spread) + range(0, spread) - spread;
}

double DeterministicRandom::unit() { return static_cast<double>(next_u32()) / 4294967296.0; }

} // namespace mwb
