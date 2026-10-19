#pragma once

#include "mwb/core/hashing.hpp"
#include "mwb/core/model.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/// Fingerprint profiles for duplicate detection (specification section 27.3). Every canonical
/// encoding starts with a length-prefixed profile tag and a version, uses little-endian
/// fixed-width integers and length prefixes, and is hashed with SHA-256 (picosha2 through core).
/// Canonical bytes are returned too so equal fingerprints can be verified against content.
namespace mwb::music {

enum class FingerprintProfile : std::uint8_t {
    exact_object = 0,               ///< All original bytes.
    exact_event_content = 1,        ///< Timed event stream, division, track order; no IDs/order keys/flags.
    exact_notes = 2,                ///< Pitch, onset, duration, channel; units reduced by their GCD.
    transposition_invariant = 3,    ///< As exact_notes with pitches relative to the lowest pitch.
    rhythm_shape = 4,               ///< Onset groups, IOIs and durations reduced by their GCD; no pitch.
};

/// Version of the canonical encodings below.
inline constexpr int fingerprint_version = 1;

/// Stable profile name stored in fingerprint.profile.
[[nodiscard]] std::string_view fingerprint_profile_name(FingerprintProfile profile) noexcept;

struct Fingerprint {
    FingerprintProfile profile = FingerprintProfile::exact_object;
    int version = fingerprint_version;
    Sha256Digest sha256{};
    std::vector<std::uint8_t> canonical;   ///< Encoded content the digest was computed from.
    friend bool operator==(const Fingerprint&, const Fingerprint&) = default;
};

/// Exact-object fingerprint of original bytes (digest only; canonical holds no copy).
[[nodiscard]] Fingerprint fingerprint_object(std::span<const std::uint8_t> bytes);

/// Content fingerprints of a whole document (every sequence in order). Notes are paired per
/// sequence with core pair_notes; unterminated notes end at the sequence end.
[[nodiscard]] Fingerprint fingerprint_document(const DocumentData& document, FingerprintProfile profile);
/// Content fingerprints of one sequence.
[[nodiscard]] Fingerprint fingerprint_sequence(const Sequence& sequence, FingerprintProfile profile);

/// Verifies that two fingerprints of the same profile and version match on canonical content,
/// not only on the digest (required before a destructive merge).
[[nodiscard]] bool fingerprints_verified_equal(const Fingerprint& left, const Fingerprint& right) noexcept;

} // namespace mwb::music
