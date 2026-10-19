// libFuzzer target: worker IPC frame decoder and chunk assembler.
#include "mwb/worker/protocol.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    mwb::worker::FrameDecoder decoder;
    mwb::worker::ChunkAssembler assembler(1u << 20);
    decoder.feed(std::span<const std::uint8_t>(data, size));
    for (int count = 0; count < 64; ++count) {
        auto frame = decoder.next();
        if (!frame || !*frame)
            break;
        if ((*frame)->kind == mwb::worker::FrameKind::data_chunk)
            std::ignore = assembler.add((*frame)->payload); // malformed chunks must be rejected, not crash
    }
    return 0;
}
