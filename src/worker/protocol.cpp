#include "mwb/worker/protocol.hpp"

#include "mwb/core/hashing.hpp"

#include <cstring>

namespace mwb::worker {

namespace {

void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}
void put64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}
std::uint64_t get(const std::uint8_t* data, int bytes) {
    std::uint64_t value = 0;
    for (int index = bytes - 1; index >= 0; --index)
        value = (value << 8) | data[index];
    return value;
}

bool known_kind(std::uint16_t kind) { return kind >= 1 && kind <= 8; }

} // namespace

Result<std::vector<std::uint8_t>> encode_frame(FrameKind kind, std::uint64_t request_id, const nlohmann::json& payload) {
    const std::vector<std::uint8_t> body = nlohmann::json::to_msgpack(payload);
    if (body.size() > max_frame_payload)
        return make_error(ErrorCode::limit_exceeded, "IPC frame payload exceeds the frame limit; send it in chunks");
    std::vector<std::uint8_t> frame;
    frame.reserve(frame_header_bytes + body.size());
    frame.insert(frame.end(), {'M', 'W', 'B', 'F'});
    put16(frame, protocol_version);
    put16(frame, static_cast<std::uint16_t>(kind));
    put64(frame, request_id);
    put32(frame, static_cast<std::uint32_t>(body.size()));
    put64(frame, xxh3_64(body));
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

void FrameDecoder::feed(std::span<const std::uint8_t> bytes) {
    if (offset_ > 0 && offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

Result<std::optional<Frame>> FrameDecoder::next() {
    const std::size_t available = buffer_.size() - offset_;
    if (available < frame_header_bytes)
        return std::optional<Frame>{};
    const std::uint8_t* header = buffer_.data() + offset_;
    if (std::memcmp(header, "MWBF", 4) != 0)
        return make_error(ErrorCode::invalid_data, "IPC frame has a bad magic value");
    if (get(header + 4, 2) != protocol_version)
        return make_error(ErrorCode::unsupported, "IPC protocol version mismatch");
    const auto kind = static_cast<std::uint16_t>(get(header + 6, 2));
    if (!known_kind(kind))
        return make_error(ErrorCode::invalid_data, "IPC frame has an unknown kind");
    const std::uint64_t request_id = get(header + 8, 8);
    const auto length = static_cast<std::uint32_t>(get(header + 16, 4));
    const std::uint64_t checksum = get(header + 20, 8);
    if (length > max_frame_payload)
        return make_error(ErrorCode::limit_exceeded, "IPC frame declares an oversized payload");
    if (available < frame_header_bytes + length)
        return std::optional<Frame>{};
    const std::span<const std::uint8_t> body(header + frame_header_bytes, length);
    if (xxh3_64(body) != checksum)
        return make_error(ErrorCode::invalid_data, "IPC frame checksum mismatch");
    Frame frame;
    frame.kind = static_cast<FrameKind>(kind);
    frame.request_id = request_id;
    frame.payload = nlohmann::json::from_msgpack(body.begin(), body.end(), true, false);
    if (frame.payload.is_discarded())
        return make_error(ErrorCode::invalid_data, "IPC frame payload is not valid MessagePack");
    offset_ += frame_header_bytes + length;
    if (offset_ > (1u << 20) && offset_ * 2 > buffer_.size()) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
        offset_ = 0;
    }
    return std::optional<Frame>(std::move(frame));
}

std::vector<nlohmann::json> chunk_payloads(std::uint64_t transfer_id, std::span<const std::uint8_t> bytes,
                                           std::size_t chunk_bytes) {
    std::vector<nlohmann::json> chunks;
    const std::size_t total = bytes.empty() ? 1 : (bytes.size() + chunk_bytes - 1) / chunk_bytes;
    for (std::size_t index = 0; index < total; ++index) {
        const std::size_t begin = index * chunk_bytes;
        const std::size_t end = std::min(bytes.size(), begin + chunk_bytes);
        std::vector<std::uint8_t> piece(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                                        bytes.begin() + static_cast<std::ptrdiff_t>(end));
        chunks.push_back({{"transfer", transfer_id},
                          {"index", index},
                          {"total", total},
                          {"size", bytes.size()},
                          {"bytes", nlohmann::json::binary(std::move(piece))}});
    }
    return chunks;
}

Result<std::optional<std::vector<std::uint8_t>>> ChunkAssembler::add(const nlohmann::json& chunk) {
    const auto invalid = [] { return make_error(ErrorCode::invalid_data, "IPC data chunk is malformed or out of order"); };
    if (!chunk.is_object() || !chunk.contains("bytes") || !chunk["bytes"].is_binary())
        return invalid();
    const auto transfer = chunk.value("transfer", std::uint64_t{0});
    const auto index = chunk.value("index", std::uint64_t{UINT64_MAX});
    const auto total = chunk.value("total", std::uint64_t{0});
    const auto size = chunk.value("size", std::uint64_t{UINT64_MAX});
    if (size > limit_)
        return make_error(ErrorCode::limit_exceeded, "IPC transfer exceeds the aggregate limit");
    if (!transfer_) {
        if (index != 0 || total == 0)
            return invalid();
        transfer_ = transfer;
        total_ = total;
        data_.reserve(static_cast<std::size_t>(size));
    }
    if (transfer != *transfer_ || index != expected_index_ || total != total_)
        return invalid();
    const auto& piece = chunk["bytes"].get_binary();
    if (data_.size() + piece.size() > size)
        return invalid();
    data_.insert(data_.end(), piece.begin(), piece.end());
    ++expected_index_;
    if (expected_index_ < total_)
        return std::optional<std::vector<std::uint8_t>>{};
    if (data_.size() != size)
        return invalid();
    std::vector<std::uint8_t> finished = std::move(data_);
    transfer_.reset();
    expected_index_ = 0;
    total_ = 0;
    data_ = {};
    return std::optional<std::vector<std::uint8_t>>(std::move(finished));
}

} // namespace mwb::worker
