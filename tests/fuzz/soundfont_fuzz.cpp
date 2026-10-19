// libFuzzer target: SoundFont 2 loading through TinySoundFont (vendored parser of untrusted data).
#include "mwb/audio/soundfont_synth.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::ignore = mwb::audio::SoundFontSynth::load_memory(std::span<const std::uint8_t>(data, size)); // must not crash
    return 0;
}
