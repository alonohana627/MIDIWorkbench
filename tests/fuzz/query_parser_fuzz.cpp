// libFuzzer target: search query grammar (PEGTL) and limits.
#include "mwb/search/query.hpp"

#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    std::ignore = mwb::search::parse_query(text); // must not crash or hang
    return 0;
}
