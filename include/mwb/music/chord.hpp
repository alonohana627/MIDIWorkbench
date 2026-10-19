#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/core/time.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mwb::music {

/// Chord types required by specification section 9.4. Values are stable (stored in recipes).
enum class ChordType : std::uint8_t {
    major = 0,
    minor = 1,
    diminished = 2,
    augmented = 3,
    suspended_second = 4,
    suspended_fourth = 5,
    power_fifth = 6,
    major_sixth = 7,
    minor_sixth = 8,
    dominant_seventh = 9,
    major_seventh = 10,
    minor_seventh = 11,
    minor_major_seventh = 12,
    half_diminished_seventh = 13,
    diminished_seventh = 14,
    added_ninth = 15,
    minor_added_ninth = 16,
};

/// Selectable extensions stacked on a chord type (section 9.4). Bit flags.
namespace chord_extension {
inline constexpr std::uint8_t ninth = 1u << 0;      ///< Major ninth (14 semitones).
inline constexpr std::uint8_t eleventh = 1u << 1;   ///< Perfect eleventh (17 semitones).
inline constexpr std::uint8_t thirteenth = 1u << 2; ///< Major thirteenth (21 semitones).
} // namespace chord_extension

/// Static description of a chord type.
struct ChordTypeInfo {
    ChordType type;
    std::string_view id;            ///< Stable identifier such as "dominant_seventh".
    std::string_view symbol;        ///< Suffix after the root name: "", "m", "7", "maj7", ...
    std::string_view display_name;  ///< "Dominant seventh".
    std::span<const int> intervals; ///< Semitones above the root, ascending, root first.
};

/// Every chord type in enumeration order.
[[nodiscard]] std::span<const ChordTypeInfo> chord_types() noexcept;
/// Information for one chord type.
[[nodiscard]] const ChordTypeInfo& chord_type_info(ChordType type) noexcept;
/// Looks up a chord type by its stable identifier.
[[nodiscard]] std::optional<ChordType> chord_type_from_id(std::string_view id) noexcept;
/// Intervals of a type plus the selected extensions, ascending and without duplicates.
[[nodiscard]] std::vector<int> chord_intervals(ChordType type, std::uint8_t extensions = 0);
/// Chord symbol suffix including extensions, such as "m9" or "maj13".
[[nodiscard]] std::string chord_symbol_suffix(ChordType type, std::uint8_t extensions = 0);

/// Spread or open-voicing rule applied after inversion (section 9.4).
enum class ChordSpread : std::uint8_t {
    close = 0,  ///< All tones inside one octave above the bass of the inversion.
    drop_two = 1,   ///< Second-highest tone dropped an octave.
    drop_three = 2, ///< Third-highest tone dropped an octave.
    open = 3,       ///< Every other tone from the second-lowest raised an octave.
};

/// Parameters for building actual chord pitches.
struct ChordSpec {
    int root_pitch = 60;               ///< MIDI pitch of the root in root position (register anchor).
    ChordType type = ChordType::major;
    std::uint8_t extensions = 0;       ///< chord_extension flags.
    int inversion = 0;                 ///< 0 = root position; k moves the k lowest tones up an octave.
    std::optional<int> bass_pitch_class; ///< Slash bass placed below the voicing.
    ChordSpread spread = ChordSpread::close;
    int note_count = 0;                ///< 0 = natural size; more doubles tones upward, fewer omits.
    int low_limit = 0;                 ///< Register floor for out-of-range reporting.
    int high_limit = 127;              ///< Register ceiling for out-of-range reporting.
    friend bool operator==(const ChordSpec&, const ChordSpec&) = default;
};

/// Actual generated pitches. Out-of-range pitches are listed but never silently clipped.
struct ChordVoicing {
    std::vector<int> pitches;      ///< Ascending; may contain values outside 0..127.
    std::vector<int> out_of_range; ///< Pitches outside [low_limit, high_limit] or 0..127.
    std::string symbol;            ///< Display symbol such as "Am7/G".
};

/// Builds the pitches of a chord (section 9.4). Fails on an invalid inversion or note count.
[[nodiscard]] Result<ChordVoicing> voice_chord(const ChordSpec& spec);

/// Strum direction for rendered chords.
enum class StrumDirection : std::uint8_t { up = 0, down = 1 };

/// Velocity shape across the voicing from lowest to highest tone.
enum class VelocityProfile : std::uint8_t {
    flat = 0,        ///< Every tone uses the base velocity.
    accent_bass = 1, ///< Lowest tone gets base + accent.
    accent_top = 2,  ///< Highest tone gets base + accent.
    ramp_up = 3,     ///< Linear from base to base + accent across the tones.
    ramp_down = 4,   ///< Linear from base + accent to base.
};

/// Timing and dynamics used to render a voicing as notes.
struct ChordRenderSpec {
    Tick start = 0;
    Tick duration = 960;       ///< At least one tick.
    Tick strum = 0;            ///< Total spread from first to last onset.
    StrumDirection direction = StrumDirection::up;
    int velocity = 100;        ///< Base velocity 1..127.
    int accent = 0;            ///< Accent amount for the velocity profile (may be negative).
    VelocityProfile profile = VelocityProfile::flat;
    std::uint8_t channel = 0;
};

/// One concrete note produced by a generator.
struct GeneratedNote {
    Tick start = 0;
    Tick end = 0;
    int pitch = 60;
    int velocity = 100;
    friend bool operator==(const GeneratedNote&, const GeneratedNote&) = default;
};

/// Renders voicing pitches into notes with strum offsets and velocity profile. Ends stay fixed
/// at start + duration; each strummed onset keeps at least one tick. Velocities clamp to 1..127.
[[nodiscard]] Result<std::vector<GeneratedNote>> render_chord(std::span<const int> pitches,
                                                              const ChordRenderSpec& render);

/// Recognition tuning (advisory, section 9.4).
struct RecognitionOptions {
    int max_missing = 1;       ///< Template tones that may be absent.
    int max_extra = 2;         ///< Sounding tones outside the template that may be present.
    std::size_t max_results = 6;
    bool include_extensions = true; ///< Also score 9th/11th/13th templates.
};

/// One recognized alternative.
struct ChordCandidate {
    int root_pitch_class = 0;
    ChordType type = ChordType::major;
    std::uint8_t extensions = 0;
    std::optional<int> bass_pitch_class;   ///< Set when the bass is not the root.
    std::vector<int> missing_intervals;    ///< Template intervals (mod 12) not sounding.
    std::vector<int> extra_pitch_classes;  ///< Sounding pitch classes outside the template.
    double score = 0;                      ///< Higher is better; comparable within one call.
    std::string symbol;                    ///< "C6", "Am7/C", ...
};

/// Scores chord templates on every root against a pitch-class mask (bit 0 = C), allowing omitted
/// and added tones, and returns ranked alternatives such as "C6" and "Am7/C" for C-E-G-A.
/// Scoring: +2 per matched tone, -0.5 for a missing fifth, -1.5 per other missing tone (-2 for
/// a missing root), -1.5 per extra tone, +0.5 when the bass is the root, -0.1 per template tone.
/// Results are sorted by score, then fewer tones, then root, then type; never rewrites notes.
[[nodiscard]] std::vector<ChordCandidate> recognize_chord(std::uint16_t pitch_class_mask,
                                                          std::optional<int> bass_pitch_class = std::nullopt,
                                                          const RecognitionOptions& options = {});

/// Pitch-class mask of a set of MIDI pitches.
[[nodiscard]] std::uint16_t pitch_class_mask(std::span<const int> pitches) noexcept;

/// Named grouping policy for chord analysis (section 27.2).
enum class ChordSegmentPolicy : std::uint8_t {
    sounding_between_onsets = 0, ///< Segment at every onset group; all notes sounding at it count.
};

/// One analysed chord segment with ranked alternatives.
struct ChordSegment {
    Tick start = 0;
    Tick end = 0;
    std::uint16_t pitch_class_mask = 0;
    std::optional<int> bass_pitch_class;
    std::vector<ChordCandidate> alternatives;
};

/// Chord analysis over note spans under a named policy: onsets within `onset_tolerance` ticks
/// form one group; each segment spans from its group onset to the next group onset (or the
/// latest note end) and contains every note sounding at its onset. Consecutive segments with
/// equal pitch-class content merge. Segments with fewer than two pitch classes have no
/// alternatives.
[[nodiscard]] std::vector<ChordSegment> analyze_chords(std::span<const NoteSpan> notes, Tick onset_tolerance,
                                                       ChordSegmentPolicy policy = ChordSegmentPolicy::sounding_between_onsets,
                                                       const RecognitionOptions& options = {});

} // namespace mwb::music
