#include "mwb/worker/protocol.hpp"
#include "mwb/worker/worker.hpp"

#include "mwb/audio/soundfont_synth.hpp"
#include "mwb/document/native_format.hpp"

namespace mwb::worker {

namespace {

/// Worker-side state machine: hello, chunked input, parse, chunked output.
class Service {
public:
    Service(const std::function<std::size_t(std::span<std::uint8_t>)>& read,
            const std::function<bool(std::span<const std::uint8_t>)>& write)
        : read_(read), write_(write) {}

    int run() {
        std::vector<std::uint8_t> buffer(64 * 1024);
        while (true) {
            const std::size_t count = read_(buffer);
            if (count == 0)
                return 0; // parent closed the pipe
            decoder_.feed(std::span<const std::uint8_t>(buffer.data(), count));
            while (true) {
                auto frame = decoder_.next();
                if (!frame)
                    return 3; // protocol violation: abandon the stream
                if (!*frame)
                    break;
                if (!handle(**frame))
                    return 0;
            }
        }
    }

private:
    bool send(FrameKind kind, std::uint64_t request, const nlohmann::json& payload) {
        auto frame = encode_frame(kind, request, payload);
        return frame && write_(*frame);
    }

    void fail(std::uint64_t request, const Error& error) {
        std::ignore = send(FrameKind::failure, request,
                           {{"code", static_cast<int>(error.code)}, {"message", error.describe()}}); // best effort
    }

    /// Returns false when the service should exit.
    bool handle(const Frame& frame) {
        switch (frame.kind) {
        case FrameKind::hello:
            return send(FrameKind::hello, frame.request_id,
                        {{"nonce", frame.payload.value("nonce", std::string())}, {"protocol", protocol_version}});
        case FrameKind::data_chunk: {
            auto added = input_.add(frame.payload);
            if (!added) {
                fail(frame.request_id, added.error());
                input_ = ChunkAssembler{};
            } else if (*added) {
                input_bytes_ = std::move(**added);
            }
            return true;
        }
        case FrameKind::parse_midi: {
            midi::ParseLimits limits;
            limits.max_bytes = frame.payload.value("max_bytes", limits.max_bytes);
            limits.max_events = frame.payload.value("max_events", limits.max_events);
            const std::string name = frame.payload.value("name", std::string("input.mid"));
            std::ignore = send(FrameKind::heartbeat, frame.request_id, {{"stage", "parse"}}); // progress only
            auto parsed = midi::import_midi(input_bytes_, name, limits);
            input_bytes_.clear();
            if (!parsed) {
                fail(frame.request_id, parsed.error());
                return true;
            }
            auto native = document::native_to_bytes(parsed->document);
            if (!native) {
                fail(frame.request_id, native.error());
                return true;
            }
            const std::uint64_t transfer = frame.request_id;
            for (const auto& chunk : chunk_payloads(transfer, *native))
                if (!send(FrameKind::data_chunk, frame.request_id, chunk))
                    return false;
            return send(FrameKind::parse_result, frame.request_id,
                        {{"transfer", transfer}, {"report", parsed->report.to_json()}});
        }
        case FrameKind::validate_soundfont: {
            // The untrusted bank is parsed here first (TinySoundFont), so a hostile file crashes
            // or exhausts this limited process, never the caller (section 28.2).
            std::ignore = send(FrameKind::heartbeat, frame.request_id, {{"stage", "soundfont"}}); // progress only
            auto bank = audio::SoundFontSynth::load_memory(input_bytes_);
            input_bytes_.clear();
            if (!bank) {
                fail(frame.request_id, bank.error());
                return true;
            }
            return send(FrameKind::soundfont_result, frame.request_id, {{"presets", (*bank)->presets().size()}});
        }
        case FrameKind::cancel:
            input_ = ChunkAssembler{};
            input_bytes_.clear();
            return true;
        case FrameKind::shutdown: return false;
        case FrameKind::parse_result:
        case FrameKind::soundfont_result:
        case FrameKind::heartbeat:
        case FrameKind::failure: return true; // not sent to a worker; ignored
        }
        return true;
    }

    const std::function<std::size_t(std::span<std::uint8_t>)>& read_;
    const std::function<bool(std::span<const std::uint8_t>)>& write_;
    FrameDecoder decoder_;
    ChunkAssembler input_;
    std::vector<std::uint8_t> input_bytes_;
};

} // namespace

int run_worker_service(const std::function<std::size_t(std::span<std::uint8_t>)>& read,
                       const std::function<bool(std::span<const std::uint8_t>)>& write) {
    Service service(read, write);
    return service.run();
}

} // namespace mwb::worker
