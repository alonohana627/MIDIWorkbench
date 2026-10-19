// libFuzzer target: transform recipe parameters (JSON from recipes, the CLI and the inspector),
// then the worker's import report decoder. Both are untrusted text.
#include "mwb/midi/codec.hpp"
#include "mwb/music/transform.hpp"

#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    if (auto parameters = mwb::music::parameters_from_json(text))
        std::ignore = mwb::music::parameters_to_json(*parameters); // a decoded value must re-encode
    std::ignore = mwb::midi::ImportReport::from_json(text);         // must not crash or throw
    return 0;
}
