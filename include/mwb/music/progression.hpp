#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/time.hpp"
#include "mwb/music/chord.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mwb::music {

/// One chord step of a progression row (specification section 9.5).
struct ProgressionStep {
    int root_pitch_class = 0;
    ChordType type = ChordType::major;
    std::uint8_t extensions = 0;
    Tick duration = 3840;                ///< Step length in ticks (at least one).
    std::optional<int> inversion;        ///< Forces the chord tone in the bass (0 = root).
    std::optional<int> bass_pitch_class; ///< Slash bass; overrides inversion for the lowest voice.
    int velocity = 100;                  ///< 1..127.
    Rational gate{1, 1};                 ///< Articulation: sounding fraction of the step (0, 1].
    friend bool operator==(const ProgressionStep&, const ProgressionStep&) = default;
};

/// Integer cost weights of the voice-leading dynamic program (section 9.5). They are reported
/// with the result so the user sees exactly what was optimized.
struct VoiceLeadingWeights {
    std::int64_t movement = 1;            ///< Per semitone of total voice movement.
    std::int64_t crossing = 6;            ///< Per voice crossing or overlap between adjacent chords.
    std::int64_t register_violation = 10; ///< Per semitone outside the user register.
    std::int64_t common_tone = 2;         ///< Bonus per voice holding a common tone (when enabled).
    friend bool operator==(const VoiceLeadingWeights&, const VoiceLeadingWeights&) = default;
};

/// Bounds of the voicing enumeration.
struct VoiceLeadingOptions {
    int low_pitch = 48;                  ///< User register floor (inclusive).
    int high_pitch = 79;                 ///< User register ceiling (inclusive).
    int register_tolerance = 4;          ///< Candidates may exceed the register by this many semitones (penalized).
    int voice_count = 0;                 ///< 0 = number of distinct chord tones (at least 3 when possible).
    std::size_t candidate_limit = 64;    ///< Voicings kept per step after static ranking.
    bool retain_common_tones = true;     ///< Enables the common-tone bonus term.
    VoiceLeadingWeights weights;
    std::vector<int> start_voicing;      ///< Optional previous voicing the first chord moves from.
};

/// Result of the dynamic program: the best path within the enumerated candidates, not a
/// globally optimal musical solution.
struct VoiceLeadingResult {
    std::vector<std::vector<int>> voicings;       ///< Chosen ascending voicing per step.
    std::vector<std::int64_t> step_costs;         ///< Transition plus register cost per step.
    std::int64_t total_cost = 0;
    std::vector<std::size_t> enumerated_counts;   ///< Candidates enumerated per step before the limit.
    std::vector<std::size_t> kept_counts;         ///< Candidates kept per step after the limit.
    bool truncated = false;                       ///< True when any step hit the candidate limit.
    VoiceLeadingWeights weights;                  ///< Weights actually used.
    std::size_t candidate_limit = 0;              ///< Limit actually used.
};

/// Enumerates bounded voicings for every step and finds the minimum-cost path by dynamic
/// programming. Cost per transition = movement * sum |semitone moves| + crossing * crossings
/// + register_violation * semitones outside the register - common_tone * held tones.
/// Ties resolve to the candidate enumerated first, so results are deterministic.
[[nodiscard]] Result<VoiceLeadingResult> voice_lead(std::span<const ProgressionStep> steps,
                                                    const VoiceLeadingOptions& options = {});

/// Concrete notes for the progression: step k starts at `start + sum(previous durations)` and
/// sounds for `round(duration * gate)` ticks (at least one).
[[nodiscard]] Result<std::vector<GeneratedNote>> render_progression(std::span<const ProgressionStep> steps,
                                                                    const VoiceLeadingResult& voicing, Tick start);

/// Versioned JSON form of a progression and its options for recipe provenance.
[[nodiscard]] std::string progression_to_json(std::span<const ProgressionStep> steps,
                                              const VoiceLeadingOptions& options);
/// Parses the form written by progression_to_json.
[[nodiscard]] Result<std::pair<std::vector<ProgressionStep>, VoiceLeadingOptions>>
progression_from_json(std::string_view json);

/// Algorithm version of the voice-leading optimizer stored in recipes.
inline constexpr int voice_leading_algorithm_version = 1;

} // namespace mwb::music
