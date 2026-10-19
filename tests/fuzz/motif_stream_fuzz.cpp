// libFuzzer target: motif-stream codec v1 decoder.
#include "mwb/search/motif.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::ignore = mwb::search::decode_motif_stream(std::span<const std::uint8_t>(data, size), 100'000); // must not crash
    return 0;
}
