// libFuzzer target: SMF/RMID import through libremidi's reader and the mwb::midi adapter.
#include "mwb/midi/codec.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    mwb::midi::ParseLimits limits;
    limits.max_events = 200'000;
    auto result = mwb::midi::import_midi(std::span<const std::uint8_t>(data, size), "fuzz.mid", limits);
    if (result) {
        auto exported = mwb::midi::export_midi(result->document, {});
        std::ignore = exported; // export must not crash on any imported document
    }
    return 0;
}
