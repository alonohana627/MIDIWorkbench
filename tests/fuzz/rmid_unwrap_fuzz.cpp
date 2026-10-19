// libFuzzer target: RIFF RMID unwrapper.
#include "mwb/midi/codec.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::ignore = mwb::midi::unwrap_rmid(std::span<const std::uint8_t>(data, size)); // result irrelevant; must not crash
    return 0;
}
