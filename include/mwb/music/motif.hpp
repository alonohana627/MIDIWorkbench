#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/model.hpp"
#include "mwb/music/voice.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/// Motif token families (specification section 26.3). The search module hashes n-grams of these
/// tokens and owns indexing; this header only defines the canonical token streams.
namespace mwb::music {

/// Version of the token definitions below; stored with indexes.
inline constexpr int motif_token_version = 1;

/// Token family names as stored in motif_gram.family.
enum class TokenFamily : std::uint8_t { pitch = 0, interval = 1, rhythm = 2, contour = 3, chord = 4 };
[[nodiscard]] std::string_view token_family_name(TokenFamily family) noexcept;

struct MotifTokenOptions {
    bool contour_size_bins = false;     ///< Contour tokens carry size bins (+-1 step, +-2 third, +-3 leap).
    bool normalize_chord_root = false;  ///< Rotate chord masks so the group's lowest pitch class is bit 0.
};

/// Token streams of one voice stream. Every family is a vector of integers; positions refer to
/// the stream notes as documented per family.
struct MotifTokens {
    /// MIDI pitches; token k is note k.
    std::vector<int> pitch;
    /// Signed semitone differences; token k is note k+1 minus note k.
    std::vector<int> interval;
    /// Inter-onset ratio bins; token k compares IOI(k+1, k+2) to IOI(k, k+1). Only positive
    /// inter-onset intervals occur because onset groups merge simultaneous notes.
    std::vector<int> rhythm;
    /// -1, 0, +1 per interval, or signed size bins when enabled.
    std::vector<int> contour;
    /// 12-bit pitch-class masks per onset group (onset-chord streams; otherwise single classes).
    std::vector<int> chord;
    /// Onset of the note each pitch token belongs to (for start_tick of grams).
    std::vector<Tick> onsets;
};

/// Rhythm ratio bin v1: the nearest of the canonical ratios 1/4, 1/3, 1/2, 2/3, 3/4, 1, 4/3,
/// 3/2, 2, 3, 4 on a logarithmic scale (bins -5..5; boundaries are geometric means, compared
/// exactly in integers); ratios beyond the ends saturate. Both intervals must be positive.
[[nodiscard]] int rhythm_ratio_bin(Tick previous_interval, Tick next_interval) noexcept;

/// Contour token for one interval.
[[nodiscard]] int contour_token(int interval, bool size_bins) noexcept;

/// Derives every token family from a voice stream.
[[nodiscard]] MotifTokens make_motif_tokens(const VoiceStream& stream, const MotifTokenOptions& options = {});

/// Convenience: extract streams of a profile and derive their tokens (same order).
[[nodiscard]] Result<std::vector<std::pair<VoiceStream, MotifTokens>>>
extract_motif_tokens(const Sequence& sequence, VoiceProfile profile, const VoiceExtractionOptions& extraction = {},
                     const MotifTokenOptions& options = {});

} // namespace mwb::music
