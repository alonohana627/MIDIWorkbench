// libFuzzer target: recovery journal command codec (CBOR envelope decoder).
#include "mwb/document/command_codec.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::ignore = mwb::document::decode_command(std::span<const std::uint8_t>(data, size)); // must not crash
    return 0;
}
