// Canonical motif streams, codec v1, tokens and n-gram keys (specification sections 26.2-26.3).

#include "mwb/search/motif.hpp"

#include "mwb/core/notes.hpp"
#include "mwb/music/motif.hpp"
#include "mwb/music/voice.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mwb::search {

namespace {

constexpr std::array<std::uint8_t, 4> codec_magic{'M', 'W', 'M', 'S'};
constexpr std::uint8_t drum_channel = 9;

/// Little-endian writer for the codec.
class ByteWriter {
public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value & 0xFF));
        u8(static_cast<std::uint8_t>(value >> 8));
    }
    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            u8(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
    void raw(std::span<const std::uint8_t> data) { bytes_.insert(bytes_.end(), data.begin(), data.end()); }
    /// Unsigned LEB128.
    void varint(std::uint64_t value) {
        while (value >= 0x80) {
            u8(static_cast<std::uint8_t>((value & 0x7F) | 0x80));
            value >>= 7;
        }
        u8(static_cast<std::uint8_t>(value));
    }
    [[nodiscard]] std::vector<std::uint8_t> finish() && { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

/// Bounds-checked little-endian reader for the codec.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::optional<std::uint8_t> u8() {
        if (position_ >= bytes_.size())
            return std::nullopt;
        return bytes_[position_++];
    }
    [[nodiscard]] std::optional<std::uint16_t> u16() {
        const auto low = u8();
        const auto high = u8();
        if (!low || !high)
            return std::nullopt;
        return static_cast<std::uint16_t>(*low | (*high << 8));
    }
    [[nodiscard]] std::optional<std::uint32_t> u32() {
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            const auto byte = u8();
            if (!byte)
                return std::nullopt;
            value |= static_cast<std::uint32_t>(*byte) << shift;
        }
        return value;
    }
    [[nodiscard]] bool raw(std::span<std::uint8_t> output) {
        if (remaining() < output.size())
            return false;
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_), output.size(), output.begin());
        position_ += output.size();
        return true;
    }
    /// Unsigned LEB128 of at most 10 bytes without overflow.
    [[nodiscard]] std::optional<std::uint64_t> varint() {
        std::uint64_t value = 0;
        for (int index = 0; index < 10; ++index) {
            const auto byte = u8();
            if (!byte)
                return std::nullopt;
            const std::uint64_t payload = *byte & 0x7Fu;
            if (index == 9 && payload > 1)
                return std::nullopt;
            value |= payload << (7 * index);
            if ((*byte & 0x80) == 0) {
                if (index > 0 && payload == 0)
                    return std::nullopt; // Overlong encoding.
                return value;
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] std::size_t remaining() const { return bytes_.size() - position_; }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_ = 0;
};

Error corrupt(std::string message) { return Error{ErrorCode::invalid_data, std::move(message), "motif stream", nullptr}; }

} // namespace

std::string_view voice_kind_name(VoiceKind kind) noexcept {
    return music::voice_profile_name(kind == VoiceKind::top ? music::VoiceProfile::top_voice
                                                            : music::VoiceProfile::bottom_voice);
}

std::string_view token_family_name(TokenFamily family) noexcept {
    switch (family) {
    case TokenFamily::pitch:
        return "pitch";
    case TokenFamily::interval:
        return "interval";
    case TokenFamily::rhythm:
        return "rhythm";
    case TokenFamily::contour:
        return "contour";
    }
    return "pitch";
}

std::optional<TokenFamily> parse_token_family(std::string_view name) noexcept {
    for (const auto family : {TokenFamily::pitch, TokenFamily::interval, TokenFamily::rhythm, TokenFamily::contour})
        if (token_family_name(family) == name)
            return family;
    return std::nullopt;
}

std::size_t token_offset(TokenFamily family) noexcept {
    switch (family) {
    case TokenFamily::pitch:
        return 0;
    case TokenFamily::interval:
    case TokenFamily::contour:
        return 1;
    case TokenFamily::rhythm:
        return 2;
    }
    return 0;
}

Tick onset_tolerance_ticks(const ExtractionSettings& settings, const Division& division) {
    if (settings.onset_tolerance)
        return std::max<Tick>(0, *settings.onset_tolerance);
    if (division.is_ppq())
        return division.ppq / 96;
    // SMPTE: a declared absolute tolerance of 10 ms (no invented quarter-note timing).
    const auto seconds_per_tick = division.smpte_seconds_per_tick();
    if (seconds_per_tick.numerator <= 0)
        return 0;
    return (seconds_per_tick.denominator) / (100 * seconds_per_tick.numerator);
}

Sha256Digest extraction_settings_hash(const ExtractionSettings& settings, const Division& division) {
    Sha256Builder builder;
    builder.update_string("mwb.motif.extraction");
    builder.update_u32(motif_profile_version);
    builder.update_u8(static_cast<std::uint8_t>(settings.voice));
    builder.update_i64(onset_tolerance_ticks(settings, division));
    builder.update_u8(settings.include_drums ? 1 : 0);
    builder.update_u64(settings.minimum_notes);
    return builder.finish();
}

Result<std::vector<MotifStream>> extract_motif_streams(const Sequence& sequence, std::uint32_t sequence_no,
                                                       const ExtractionSettings& settings) {
    // Voice extraction is the music module's (section 26.2); search owns the stored stream form.
    music::VoiceExtractionOptions options;
    options.onset_tolerance = onset_tolerance_ticks(settings, sequence.division);
    options.include_drums = settings.include_drums;
    auto voices = music::extract_voices(
        sequence, settings.voice == VoiceKind::top ? music::VoiceProfile::top_voice : music::VoiceProfile::bottom_voice,
        options);
    if (!voices)
        return std::unexpected(std::move(voices).error());
    const auto hash = extraction_settings_hash(settings, sequence.division);
    std::vector<MotifStream> streams;
    for (const auto& voice : *voices) {
        // Channel 10 is the General MIDI drum channel: not pitched material unless selected.
        if (!settings.include_drums && voice.channel == drum_channel)
            continue;
        if (voice.notes.size() < settings.minimum_notes)
            continue;
        MotifStream stream;
        stream.voice = settings.voice;
        stream.division = sequence.division;
        stream.settings_hash = hash;
        stream.sequence_no = sequence_no;
        stream.track_no = static_cast<std::uint32_t>(voice.track_index);
        stream.channel = voice.channel;
        stream.notes.reserve(voice.notes.size());
        for (const auto& note : voice.notes)
            stream.notes.push_back(MotifNote{note.start, std::max(note.end, note.start),
                                             static_cast<std::uint8_t>(std::clamp(note.pitch, 0, 127)),
                                             static_cast<std::uint8_t>(std::clamp(note.velocity, 0, 127)), note.ordinal});
        streams.push_back(std::move(stream));
    }
    return streams;
}

std::vector<std::uint8_t> encode_motif_stream(const MotifStream& stream) {
    ByteWriter writer;
    writer.raw(codec_magic);
    writer.u16(motif_codec_version);
    writer.u16(stream.profile_version);
    writer.u8(static_cast<std::uint8_t>(stream.voice));
    writer.u8(stream.division.is_ppq() ? 0 : 1);
    const auto division_value = stream.division.is_ppq() ? stream.division.ppq
                                                         : static_cast<std::int32_t>(stream.division.smpte_format);
    writer.u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(division_value)));
    writer.u16(static_cast<std::uint16_t>(stream.division.is_ppq() ? 0 : stream.division.ticks_per_frame));
    writer.raw(stream.settings_hash);
    writer.u32(stream.sequence_no);
    writer.u32(stream.track_no);
    writer.u8(stream.channel);
    writer.u32(static_cast<std::uint32_t>(stream.notes.size()));
    Tick previous_onset = 0;
    std::uint64_t next_ordinal = 0;
    for (const auto& note : stream.notes) {
        writer.varint(static_cast<std::uint64_t>(note.onset - previous_onset));
        writer.varint(static_cast<std::uint64_t>(note.end - note.onset));
        writer.u8(note.pitch);
        writer.u8(note.velocity);
        writer.varint(note.ordinal - next_ordinal);
        previous_onset = note.onset;
        next_ordinal = std::uint64_t{note.ordinal} + 1;
    }
    return std::move(writer).finish();
}

Result<MotifStream> decode_motif_stream(std::span<const std::uint8_t> bytes, std::size_t max_notes) {
    ByteReader reader(bytes);
    std::array<std::uint8_t, 4> magic{};
    if (!reader.raw(magic) || magic != codec_magic)
        return std::unexpected(corrupt("missing motif-stream magic"));
    const auto codec = reader.u16();
    if (!codec || *codec != motif_codec_version)
        return std::unexpected(Error{ErrorCode::unsupported, "unsupported motif-stream codec version", {}, nullptr});
    MotifStream stream;
    const auto profile = reader.u16();
    const auto voice = reader.u8();
    const auto division_kind = reader.u8();
    const auto division_value = reader.u16();
    const auto ticks_per_frame = reader.u16();
    if (!profile || !voice || !division_kind || !division_value || !ticks_per_frame || *profile == 0)
        return std::unexpected(corrupt("truncated motif-stream header"));
    if (*voice != static_cast<std::uint8_t>(VoiceKind::top) && *voice != static_cast<std::uint8_t>(VoiceKind::bottom))
        return std::unexpected(corrupt("unknown voice kind"));
    stream.profile_version = *profile;
    stream.voice = static_cast<VoiceKind>(*voice);
    const auto signed_value = static_cast<std::int16_t>(*division_value);
    if (*division_kind == 0) {
        if (*ticks_per_frame != 0)
            return std::unexpected(corrupt("PPQ stream with ticks per frame"));
        stream.division = Division::make_ppq(signed_value);
    } else if (*division_kind == 1) {
        if (signed_value != -24 && signed_value != -25 && signed_value != -29 && signed_value != -30)
            return std::unexpected(corrupt("invalid SMPTE format"));
        stream.division = Division::make_smpte(static_cast<SmpteFormat>(signed_value), *ticks_per_frame);
    } else {
        return std::unexpected(corrupt("invalid division kind"));
    }
    if (!stream.division.validate())
        return std::unexpected(corrupt("invalid division"));
    if (!reader.raw(stream.settings_hash))
        return std::unexpected(corrupt("truncated settings hash"));
    const auto sequence_no = reader.u32();
    const auto track_no = reader.u32();
    const auto channel = reader.u8();
    const auto note_count = reader.u32();
    if (!sequence_no || !track_no || !channel || !note_count)
        return std::unexpected(corrupt("truncated motif-stream header"));
    if (*channel > 15)
        return std::unexpected(corrupt("channel out of range"));
    // Every record needs at least four bytes, so the declared count is bounded by the payload.
    if (*note_count > max_notes || *note_count > reader.remaining() / 4)
        return std::unexpected(corrupt("note count exceeds the payload or the limit"));
    stream.sequence_no = *sequence_no;
    stream.track_no = *track_no;
    stream.channel = *channel;
    stream.notes.reserve(*note_count);
    Tick previous_onset = 0;
    std::uint64_t next_ordinal = 0;
    constexpr auto tick_limit = static_cast<std::uint64_t>(std::numeric_limits<Tick>::max());
    for (std::uint32_t index = 0; index < *note_count; ++index) {
        const auto onset_delta = reader.varint();
        const auto duration = reader.varint();
        const auto pitch = reader.u8();
        const auto velocity = reader.u8();
        const auto ordinal_delta = reader.varint();
        if (!onset_delta || !duration || !pitch || !velocity || !ordinal_delta)
            return std::unexpected(corrupt("truncated or malformed note record"));
        if (*pitch > 127 || *velocity > 127)
            return std::unexpected(corrupt("pitch or velocity out of range"));
        if (*onset_delta > tick_limit - static_cast<std::uint64_t>(previous_onset))
            return std::unexpected(corrupt("onset overflow"));
        const auto onset = previous_onset + static_cast<Tick>(*onset_delta);
        if (index > 0 && onset <= previous_onset)
            return std::unexpected(corrupt("onsets are not strictly increasing"));
        if (*duration > tick_limit - static_cast<std::uint64_t>(onset))
            return std::unexpected(corrupt("note end overflow"));
        if (*ordinal_delta > std::numeric_limits<std::uint32_t>::max() - next_ordinal)
            return std::unexpected(corrupt("ordinal overflow"));
        const auto ordinal = next_ordinal + *ordinal_delta;
        stream.notes.push_back(MotifNote{onset, onset + static_cast<Tick>(*duration), *pitch, *velocity,
                                         static_cast<std::uint32_t>(ordinal)});
        previous_onset = onset;
        next_ordinal = ordinal + 1;
    }
    if (reader.remaining() != 0)
        return std::unexpected(corrupt("trailing bytes after the last note record"));
    return stream;
}

std::vector<QueryNote> to_query_notes(std::span<const MotifNote> notes) {
    std::vector<QueryNote> result;
    result.reserve(notes.size());
    for (const auto& note : notes)
        result.push_back(QueryNote{note.onset, note.end - note.onset, note.pitch, note.velocity});
    return result;
}

std::vector<std::int32_t> family_tokens(std::span<const QueryNote> notes, TokenFamily family) {
    // Token definitions come from the music module (section 26.3); search hashes and indexes them.
    music::VoiceStream stream;
    stream.profile = music::VoiceProfile::explicit_selection;
    stream.notes.reserve(notes.size());
    for (std::size_t index = 0; index < notes.size(); ++index) {
        music::VoiceNote note;
        note.start = notes[index].onset;
        note.end = notes[index].onset + std::max<Tick>(0, notes[index].duration);
        note.pitch = notes[index].pitch;
        note.velocity = notes[index].velocity;
        note.ordinal = static_cast<std::uint32_t>(index);
        stream.notes.push_back(note);
    }
    const auto tokens = music::make_motif_tokens(stream);
    const std::vector<int>* source = &tokens.pitch;
    switch (family) {
    case TokenFamily::pitch:
        source = &tokens.pitch;
        break;
    case TokenFamily::interval:
        source = &tokens.interval;
        break;
    case TokenFamily::rhythm:
        source = &tokens.rhythm;
        break;
    case TokenFamily::contour:
        source = &tokens.contour;
        break;
    }
    return std::vector<std::int32_t>(source->begin(), source->end());
}

std::uint64_t gram_hash(TokenFamily family, std::span<const std::int32_t> tokens) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(2 + tokens.size() * 4);
    bytes.push_back(static_cast<std::uint8_t>(family));
    bytes.push_back(static_cast<std::uint8_t>(tokens.size()));
    for (const auto token : tokens) {
        const auto value = static_cast<std::uint32_t>(token);
        for (int shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
    return xxh3_64(bytes);
}

std::array<std::uint8_t, 8> gram_hash_bytes(std::uint64_t hash) noexcept {
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t index = 0; index < 8; ++index)
        bytes[index] = static_cast<std::uint8_t>((hash >> (56 - 8 * index)) & 0xFF);
    return bytes;
}

std::uint64_t gram_hash_from_bytes(std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t hash = 0;
    for (std::size_t index = 0; index < 8 && index < bytes.size(); ++index)
        hash = (hash << 8) | bytes[index];
    return hash;
}

std::vector<MotifGram> stream_grams(const MotifStream& stream) {
    std::vector<MotifGram> grams;
    const auto notes = to_query_notes(stream.notes);
    for (const auto family : {TokenFamily::pitch, TokenFamily::interval, TokenFamily::rhythm, TokenFamily::contour}) {
        const auto tokens = family_tokens(notes, family);
        for (const auto length : default_gram_lengths) {
            for (std::size_t start = 0; start + length <= tokens.size(); ++start) {
                MotifGram gram;
                gram.key = GramKey{family, static_cast<std::uint8_t>(length),
                                   gram_hash(family, std::span(tokens).subspan(start, length))};
                gram.start_ordinal = static_cast<std::uint32_t>(start);
                gram.start_tick = stream.notes[start].onset;
                grams.push_back(gram);
            }
        }
    }
    return grams;
}

void InMemoryMotifIndex::add(StoredMotifStream stream) {
    const auto id = stream.info.stream_id;
    for (const auto& gram : stream_grams(stream.stream)) {
        auto& list = postings_[gram.key];
        list.insert(std::ranges::upper_bound(list, GramPosting{id, gram.start_ordinal}),
                    GramPosting{id, gram.start_ordinal});
    }
    streams_.insert_or_assign(id, std::move(stream));
}

Result<std::uint64_t> InMemoryMotifIndex::count_postings(const GramKey& key) {
    const auto found = postings_.find(key);
    return found == postings_.end() ? std::uint64_t{0} : std::uint64_t{found->second.size()};
}

Result<std::vector<GramPosting>> InMemoryMotifIndex::postings(const GramKey& key, std::size_t limit) {
    const auto found = postings_.find(key);
    if (found == postings_.end())
        return std::vector<GramPosting>{};
    const auto count = std::min(limit, found->second.size());
    return std::vector<GramPosting>(found->second.begin(), found->second.begin() + static_cast<std::ptrdiff_t>(count));
}

Result<std::optional<StoredMotifStream>> InMemoryMotifIndex::load_stream(std::int64_t stream_id) {
    const auto found = streams_.find(stream_id);
    if (found == streams_.end())
        return std::optional<StoredMotifStream>{};
    return std::optional<StoredMotifStream>{found->second};
}

Result<std::vector<std::int64_t>> InMemoryMotifIndex::stream_ids(std::int64_t after_id, std::size_t limit) {
    std::vector<std::int64_t> ids;
    for (auto iterator = streams_.upper_bound(after_id); iterator != streams_.end() && ids.size() < limit; ++iterator)
        ids.push_back(iterator->first);
    return ids;
}

} // namespace mwb::search
