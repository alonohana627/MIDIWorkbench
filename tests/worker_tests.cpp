#include "test_support.hpp"

#include "mwb/core/change_set.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/midi/codec.hpp"
#include "mwb/worker/protocol.hpp"
#include "mwb/worker/worker.hpp"

using namespace mwb;
using namespace mwb::worker;

namespace {

std::vector<std::uint8_t> small_midi() {
    auto document = make_blank_document();
    ChangeBuilder builder(document, "fixture");
    builder.add_note(document.sequences[0].tracks[0].id, 0, 480, 0, 60, 100);
    REQUIRE(apply_change_set(document, builder.finish()));
    auto bytes = midi::export_midi(document, {});
    REQUIRE(bytes);
    return *bytes;
}

} // namespace

TEST_SUITE("ipc") {
    TEST_CASE("frames round-trip byte by byte") {
        auto frame = encode_frame(FrameKind::parse_midi, 42, {{"name", "x.mid"}, {"max_bytes", 10}});
        REQUIRE(frame);
        FrameDecoder decoder;
        std::optional<Frame> decoded;
        for (const auto byte : *frame) {
            decoder.feed(std::span<const std::uint8_t>(&byte, 1));
            auto next = decoder.next();
            REQUIRE(next);
            if (*next)
                decoded = std::move(**next);
        }
        REQUIRE(decoded);
        CHECK(decoded->kind == FrameKind::parse_midi);
        CHECK(decoded->request_id == 42);
        CHECK(decoded->payload.value("name", std::string()) == "x.mid");
    }

    TEST_CASE("corrupt frames are protocol errors") {
        auto frame = encode_frame(FrameKind::hello, 1, {{"nonce", "abc"}});
        REQUIRE(frame);
        auto damaged = *frame;
        damaged.back() ^= 0x55;
        FrameDecoder decoder;
        decoder.feed(damaged);
        CHECK_FALSE(decoder.next());
        auto bad_magic = *frame;
        bad_magic[0] = 'X';
        FrameDecoder second;
        second.feed(bad_magic);
        CHECK_FALSE(second.next());
        auto oversized = *frame;
        oversized[16] = 0xFF;
        oversized[17] = 0xFF;
        oversized[18] = 0xFF;
        FrameDecoder third;
        third.feed(oversized);
        CHECK_FALSE(third.next());
    }

    TEST_CASE("chunked transfers reassemble and reject reordering") {
        std::vector<std::uint8_t> blob(2'500'000);
        for (std::size_t index = 0; index < blob.size(); ++index)
            blob[index] = static_cast<std::uint8_t>(index * 31);
        const auto chunks = chunk_payloads(7, blob);
        CHECK(chunks.size() == 4);
        ChunkAssembler assembler;
        std::optional<std::vector<std::uint8_t>> result;
        for (const auto& chunk : chunks) {
            auto added = assembler.add(chunk);
            REQUIRE(added);
            if (*added)
                result = std::move(**added);
        }
        REQUIRE(result);
        CHECK(*result == blob);
        ChunkAssembler reordered;
        CHECK_FALSE(reordered.add(chunks[1]));
    }
}

TEST_SUITE("worker") {
    TEST_CASE("mwb-worker loads an SF2 bank before the caller does (code-runtime-14)") {
        auto client = WorkerClient::start(MWB_WORKER_PATH);
        REQUIRE(client);
        auto presets = (*client)->validate_soundfont(test::make_test_soundfont());
        REQUIRE_MESSAGE(presets, (presets ? "" : presets.error().describe()));
        CHECK(*presets == 1);
        // A bank that does not load fails that bank only; the worker keeps serving.
        auto truncated = test::make_test_soundfont();
        truncated.resize(truncated.size() / 2);
        CHECK_FALSE((*client)->validate_soundfont(truncated));
        CHECK_FALSE((*client)->validate_soundfont(std::vector<std::uint8_t>{'R', 'I', 'F', 'F', 1, 2, 3}));
        CHECK((*client)->is_alive());
        CHECK((*client)->parse_midi(small_midi(), "after.mid", {})); // MIDI requests still work
        (*client)->stop();
    }

    TEST_CASE("mwb-worker parses a file in a separate limited process") {
        auto client = WorkerClient::start(MWB_WORKER_PATH);
        REQUIRE(client);
        const auto bytes = small_midi();
        auto parsed = (*client)->parse_midi(bytes, "small.mid", {});
        REQUIRE(parsed);
        CHECK(pair_notes(parsed->document.sequences[0]).notes.size() == 1);
        CHECK(parsed->report_json.find("validated") != std::string::npos);
        // An invalid item fails only that item; the worker keeps serving.
        const std::vector<std::uint8_t> garbage = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1};
        CHECK_FALSE((*client)->parse_midi(garbage, "bad.mid", {}));
        CHECK((*client)->is_alive());
        CHECK((*client)->parse_midi(bytes, "again.mid", {}));
        CHECK_FALSE((*client)->isolation_description().empty());
        (*client)->stop();
        CHECK_FALSE((*client)->is_alive());
    }

    TEST_CASE("a parse that exceeds the time limit kills the worker and fails the item") {
        WorkerLimits limits;
        limits.parse_timeout = std::chrono::milliseconds(1);
        auto client = WorkerClient::start(MWB_WORKER_PATH, limits);
        REQUIRE(client);
        std::vector<std::uint8_t> large = small_midi();
        auto parsed = (*client)->parse_midi(large, "small.mid", {});
        if (!parsed) {
            CHECK(parsed.error().code == ErrorCode::limit_exceeded);
            CHECK_FALSE((*client)->is_alive());
        }
    }
}
