#pragma once

#include "mwb/core/error.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/// Framed, versioned IPC between the application and mwb-worker.exe (specification section 28.2).
/// Every frame: explicit little-endian header (magic "MWBF", protocol version, kind, request id,
/// payload length, XXH3-64 checksum of the payload) followed by a MessagePack payload produced
/// by nlohmann/json — never hand-packed structs.
namespace mwb::worker {

inline constexpr std::uint16_t protocol_version = 1;
inline constexpr std::size_t frame_header_bytes = 28;
/// Default maximum payload per frame; larger data streams in bounded chunks.
inline constexpr std::uint32_t max_frame_payload = 1024u * 1024u;
/// Aggregate limit for chunked transfers (the largest accepted source file plus headroom).
inline constexpr std::uint64_t max_aggregate_bytes = 512ull * 1024ull * 1024ull;

/// Frame kinds.
enum class FrameKind : std::uint16_t {
    hello = 1,          ///< Parent -> worker: session nonce. Worker -> parent: echo + capabilities.
    data_chunk = 2,     ///< Either direction: {transfer, index, total, bytes} piece of a large blob.
    parse_midi = 3,     ///< Parent -> worker: parse the transferred bytes as SMF/RMID.
    parse_result = 4,   ///< Worker -> parent: report and the transfer id holding native bytes.
    heartbeat = 5,      ///< Worker -> parent: progress while working.
    cancel = 6,         ///< Parent -> worker: cancel the request.
    failure = 7,        ///< Worker -> parent: structured error for a request.
    shutdown = 8,       ///< Parent -> worker: exit cleanly.
    validate_soundfont = 9, ///< Parent -> worker: load the transferred bytes as an SF2 bank.
    soundfont_result = 10,  ///< Worker -> parent: the bank loaded; {presets}.
};

/// A decoded frame.
struct Frame {
    FrameKind kind = FrameKind::hello;
    std::uint64_t request_id = 0;
    nlohmann::json payload;
};

/// Encodes one frame. Fails when the payload exceeds max_frame_payload.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_frame(FrameKind kind, std::uint64_t request_id,
                                                             const nlohmann::json& payload);

/// Incremental decoder: feed bytes as they arrive; complete frames are returned in order. Any
/// malformed header, unknown kind, oversized length or checksum mismatch is a protocol error and
/// the stream must be abandoned.
class FrameDecoder {
public:
    /// Appends received bytes.
    void feed(std::span<const std::uint8_t> bytes);
    /// Next complete frame, nullopt when more bytes are needed, or an error.
    [[nodiscard]] Result<std::optional<Frame>> next();
    [[nodiscard]] std::size_t buffered() const { return buffer_.size() - offset_; }

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t offset_ = 0;
};

/// Splits a blob into data_chunk payloads of at most `chunk_bytes`.
[[nodiscard]] std::vector<nlohmann::json> chunk_payloads(std::uint64_t transfer_id, std::span<const std::uint8_t> bytes,
                                                         std::size_t chunk_bytes = 768u * 1024u);

/// Reassembles data_chunk payloads with bounds and order checks.
class ChunkAssembler {
public:
    explicit ChunkAssembler(std::uint64_t aggregate_limit = max_aggregate_bytes) : limit_(aggregate_limit) {}
    /// Adds one chunk; returns the finished blob when the last chunk arrived.
    [[nodiscard]] Result<std::optional<std::vector<std::uint8_t>>> add(const nlohmann::json& chunk);
    [[nodiscard]] std::optional<std::uint64_t> transfer() const { return transfer_; }

private:
    std::uint64_t limit_;
    std::optional<std::uint64_t> transfer_;
    std::uint64_t expected_index_ = 0;
    std::uint64_t total_ = 0;
    std::uint64_t size_ = 0; ///< Declared by the first chunk; every later chunk must agree.
    std::vector<std::uint8_t> data_;
};

} // namespace mwb::worker
