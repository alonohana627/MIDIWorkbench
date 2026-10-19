// libFuzzer target: native .mwb loader (bounded read-only SQLite connection plus validation).
#include "mwb/document/native_format.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::ignore = mwb::document::open_native_bytes(std::span<const std::uint8_t>(data, size)); // must not crash
    return 0;
}
