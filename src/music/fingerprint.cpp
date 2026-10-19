#include "mwb/music/fingerprint.hpp"

#include "mwb/core/notes.hpp"

#include <algorithm>
#include <map>
#include <numeric>
#include <string>
#include <tuple>

namespace mwb::music {

namespace {

/// Little-endian, fixed-width, length-prefixed canonical byte writer.
class CanonicalWriter {
public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void u64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8)
            bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }
    void text(std::string_view value) {
        u64(value.size());
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void blob(std::span<const std::uint8_t> value) {
        u64(value.size());
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

/// Division fields in canonical form (unused fields are zero).
void write_division(CanonicalWriter& writer, const Division& division) {
    writer.u8(static_cast<std::uint8_t>(division.kind));
    writer.i64(division.is_ppq() ? division.ppq : 0);
    writer.i64(division.is_ppq() ? 0 : static_cast<std::int64_t>(division.smpte_format));
    writer.i64(division.is_ppq() ? 0 : division.ticks_per_frame);
}

/// Exact event content: division, track order and every event without identity, order key or flags.
void write_event_content(CanonicalWriter& writer, const Sequence& sequence) {
    write_division(writer, sequence.division);
    writer.u64(sequence.tracks.size());
    for (const auto& track : sequence.tracks) {
        writer.u64(track.events.size());
        for (const auto& event : track.events) {
            writer.i64(event.tick);
            writer.u8(static_cast<std::uint8_t>(event.category));
            writer.u8(event.code);
            writer.blob(event.payload.bytes());
        }
    }
}

struct PlainNote {
    Tick start = 0;
    Tick duration = 0;
    int pitch = 0;
    std::uint8_t channel = 0;
};

/// Paired notes of a sequence in canonical order (start, channel, pitch, duration).
std::vector<PlainNote> plain_notes(const Sequence& sequence) {
    std::vector<PlainNote> notes;
    for (const auto& span : pair_notes(sequence).notes)
        notes.push_back(PlainNote{span.start, span.end - span.start, span.pitch, span.channel});
    std::ranges::sort(notes, [](const PlainNote& a, const PlainNote& b) {
        return std::tie(a.start, a.channel, a.pitch, a.duration) < std::tie(b.start, b.channel, b.pitch, b.duration);
    });
    return notes;
}

/// Note list with GCD-reduced time units; pitches optionally relative to the lowest pitch.
void write_notes(CanonicalWriter& writer, const Sequence& sequence, bool relative_pitch) {
    const auto notes = plain_notes(sequence);
    // The time unit is one quarter note (PPQ) or one frame (SMPTE); every value is divided by
    // the GCD of the unit and all onsets and durations, so equal music at another resolution
    // encodes identically.
    const std::int64_t unit = sequence.division.is_ppq() ? sequence.division.ppq : sequence.division.ticks_per_frame;
    std::int64_t divisor = std::max<std::int64_t>(unit, 1);
    int lowest = 127;
    for (const auto& note : notes) {
        divisor = std::gcd(divisor, note.start);
        divisor = std::gcd(divisor, note.duration);
        lowest = std::min(lowest, note.pitch);
    }
    writer.u8(static_cast<std::uint8_t>(sequence.division.kind));
    writer.i64(sequence.division.is_ppq() ? 0 : static_cast<std::int64_t>(sequence.division.smpte_format));
    writer.i64(std::max<std::int64_t>(unit, 1) / divisor);
    writer.u64(notes.size());
    for (const auto& note : notes) {
        writer.i64(note.start / divisor);
        writer.i64(note.duration / divisor);
        writer.i64(relative_pitch ? note.pitch - lowest : note.pitch);
        writer.u8(note.channel);
    }
}

/// Onset groups with GCD-reduced inter-onset intervals and sorted durations; pitch is ignored.
void write_rhythm_shape(CanonicalWriter& writer, const Sequence& sequence) {
    const auto notes = plain_notes(sequence);
    std::map<Tick, std::vector<Tick>> groups; // onset -> durations
    for (const auto& note : notes)
        groups[note.start].push_back(note.duration);
    std::int64_t divisor = 0;
    Tick previous = -1;
    for (auto& [onset, durations] : groups) {
        std::ranges::sort(durations);
        if (previous >= 0)
            divisor = std::gcd(divisor, onset - previous);
        for (const Tick duration : durations)
            divisor = std::gcd(divisor, duration);
        previous = onset;
    }
    divisor = std::max<std::int64_t>(divisor, 1);
    writer.u64(groups.size());
    for (auto group = groups.begin(); group != groups.end(); ++group) {
        const auto next = std::next(group);
        writer.u32(static_cast<std::uint32_t>(group->second.size()));
        writer.i64(next == groups.end() ? 0 : (next->first - group->first) / divisor);
        for (const Tick duration : group->second)
            writer.i64(duration / divisor);
    }
}

/// Dispatches the per-sequence encoding of a profile.
void write_sequence(CanonicalWriter& writer, const Sequence& sequence, FingerprintProfile profile) {
    switch (profile) {
    case FingerprintProfile::exact_object: break;
    case FingerprintProfile::exact_event_content: write_event_content(writer, sequence); break;
    case FingerprintProfile::exact_notes: write_notes(writer, sequence, false); break;
    case FingerprintProfile::transposition_invariant: write_notes(writer, sequence, true); break;
    case FingerprintProfile::rhythm_shape: write_rhythm_shape(writer, sequence); break;
    }
}

/// Length-prefixed profile tag, encoding version and scope.
void write_header(CanonicalWriter& writer, FingerprintProfile profile, std::string_view scope) {
    writer.text("mwb.fingerprint." + std::string(fingerprint_profile_name(profile)));
    writer.u32(static_cast<std::uint32_t>(fingerprint_version));
    writer.text(scope);
}

/// Hashes canonical bytes into a fingerprint that keeps them for verification.
Fingerprint finish(FingerprintProfile profile, std::vector<std::uint8_t> canonical) {
    Fingerprint fingerprint;
    fingerprint.profile = profile;
    fingerprint.sha256 = sha256(canonical);
    fingerprint.canonical = std::move(canonical);
    return fingerprint;
}

} // namespace

std::string_view fingerprint_profile_name(FingerprintProfile profile) noexcept {
    switch (profile) {
    case FingerprintProfile::exact_object: return "exact_object";
    case FingerprintProfile::exact_event_content: return "exact_event_content";
    case FingerprintProfile::exact_notes: return "exact_notes";
    case FingerprintProfile::transposition_invariant: return "transposition_invariant";
    case FingerprintProfile::rhythm_shape: return "rhythm_shape";
    }
    return "unknown";
}

Fingerprint fingerprint_object(std::span<const std::uint8_t> bytes) {
    Fingerprint fingerprint;
    fingerprint.profile = FingerprintProfile::exact_object;
    fingerprint.sha256 = sha256(bytes);
    return fingerprint;
}

Fingerprint fingerprint_document(const DocumentData& document, FingerprintProfile profile) {
    CanonicalWriter writer;
    write_header(writer, profile, "document");
    writer.u64(document.sequences.size());
    for (const auto& sequence : document.sequences)
        write_sequence(writer, sequence, profile);
    return finish(profile, writer.take());
}

Fingerprint fingerprint_sequence(const Sequence& sequence, FingerprintProfile profile) {
    CanonicalWriter writer;
    write_header(writer, profile, "sequence");
    write_sequence(writer, sequence, profile);
    return finish(profile, writer.take());
}

bool fingerprints_verified_equal(const Fingerprint& left, const Fingerprint& right) noexcept {
    if (left.profile != right.profile || left.version != right.version || left.sha256 != right.sha256)
        return false;
    return left.canonical == right.canonical;
}

} // namespace mwb::music
