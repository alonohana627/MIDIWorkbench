#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/model.hpp"
#include "mwb/core/time.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/// Named voice extraction profiles (specification section 26.2). Polyphonic material is never
/// sorted by start time and called a melody; every stream states its profile and version.
namespace mwb::music {

enum class VoiceProfile : std::uint8_t {
    monophonic_channel = 0, ///< An already non-overlapping line on one track/channel.
    top_voice = 1,          ///< Highest pitch of each onset group.
    bottom_voice = 2,       ///< Lowest pitch of each onset group.
    onset_chord = 3,        ///< Pitch-class set of each onset group.
    explicit_selection = 4, ///< The user's selection in its defined order.
};

/// Version of every extraction profile; a new algorithm gets a new version (section 26.2).
inline constexpr int voice_profile_version = 1;

/// Stable profile name stored as motif_stream.voice_kind.
[[nodiscard]] std::string_view voice_profile_name(VoiceProfile profile) noexcept;

struct VoiceExtractionOptions {
    /// Onset-group tolerance in ticks; default 1/96 quarter note for PPQ. SMPTE sequences must
    /// state an absolute tolerance explicitly (no invented quarter-note timing).
    std::optional<Tick> onset_tolerance;
    bool include_drums = false;         ///< Drum tracks are skipped unless explicitly selected.
};

/// One note of an extracted stream.
struct VoiceNote {
    Tick start = 0;
    Tick end = 0;
    int pitch = 0;                      ///< For onset chords: the lowest pitch of the group.
    int velocity = 0;
    EventId note_id;                    ///< Note-on identity of the chosen (or lowest) note.
    std::uint32_t ordinal = 0;          ///< Source ordinal: index in the sequence note pairing.
    std::uint16_t chord_mask = 0;       ///< Onset-chord profile: pitch classes of the group.
    std::uint8_t group_size = 1;        ///< Notes in the onset group (saturating at 255).
    friend bool operator==(const VoiceNote&, const VoiceNote&) = default;
};

/// An extracted stream for one track and channel.
struct VoiceStream {
    std::size_t track_index = 0;
    std::uint8_t channel = 0;
    VoiceProfile profile = VoiceProfile::monophonic_channel;
    int profile_version = voice_profile_version;
    Tick onset_tolerance = 0;
    std::vector<VoiceNote> notes;       ///< Ordered by onset.
};

/// Extracts one stream per eligible track/channel (tracks split by channel first). The
/// monophonic profile emits only lines whose notes do not overlap. Top/bottom ties keep the
/// earliest note in stable event order.
[[nodiscard]] Result<std::vector<VoiceStream>> extract_voices(const Sequence& sequence, VoiceProfile profile,
                                                              const VoiceExtractionOptions& options = {});

/// Explicit selection profile: the given notes in the given order (identities of note-ons).
[[nodiscard]] Result<VoiceStream> extract_selection(const Sequence& sequence, std::span<const EventId> notes);

} // namespace mwb::music
