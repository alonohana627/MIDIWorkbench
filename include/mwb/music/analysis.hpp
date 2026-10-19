#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/model.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/core/time.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

/// Basic analysis, polyphony and key estimation (specification sections 27.1 and 27.2). Results
/// are rebuildable derived data tied to analysis_algorithm_version.
namespace mwb::music {

/// Version of the basic analysis written to sequence_summary.analysis_version.
inline constexpr int analysis_algorithm_version = 1;

// --- Polyphony -------------------------------------------------------------------------------------

/// Maximum key-down polyphony (section 27.1): half-open note spans [start, end); at equal ticks
/// note ends are processed before note starts. A retained zero-length note counts as sounding
/// at its own tick only. Pedals are ignored (sustained polyphony is a different analysis).
[[nodiscard]] std::size_t max_key_down_polyphony(std::span<const NoteSpan> notes);

// --- Key estimation ----------------------------------------------------------------------------------

/// Profiles compared by the key estimator.
enum class KeyProfileSet : std::uint8_t {
    major_minor = 0,          ///< Krumhansl-Kessler major and minor probe-tone profiles.
    major_minor_and_modes = 1, ///< Adds the MWB church-mode profiles v1 (Dorian ... Locrian).
};

struct KeyEstimationOptions {
    KeyProfileSet profiles = KeyProfileSet::major_minor;
    std::int32_t ppq = default_ppq;     ///< Resolution of the note ticks (bounds durations).
    Rational duration_bound{4, 1};      ///< Longest counted duration in quarter notes.
    bool weight_by_velocity = false;    ///< Multiply weights by velocity / 127.
    std::size_t minimum_notes = 4;      ///< Fewer notes return Unknown.
    int minimum_pitch_classes = 3;      ///< Fewer distinct pitch classes return Unknown.
    double minimum_correlation = 0.5;   ///< A lower best correlation returns Unknown.
    std::size_t alternatives = 4;       ///< Ranked alternatives returned besides the best.
};

/// One key hypothesis.
struct KeyCandidate {
    int root_pitch_class = 0;
    std::string mode;                   ///< "major", "minor", "dorian", "phrygian", ...
    double correlation = 0;             ///< Pearson correlation with the rotated profile, -1..1.
};

/// Result of key estimation. Confidence is an estimator score (not a probability), pending
/// calibration on the retrieval corpus: clamp(r, 0, 1) * (0.5 + 0.5 * clamp(margin / 0.1, 0, 1))
/// where r is the best correlation and margin its lead over the second candidate.
struct KeyEstimate {
    bool unknown = true;                ///< True for sparse or ambiguous material.
    std::optional<KeyCandidate> best;   ///< Absent when unknown.
    double confidence = 0;
    double margin = 0;
    std::vector<KeyCandidate> alternatives; ///< Ranked candidates (including the best first).
    std::string profile_name;           ///< "krumhansl_kessler" or "krumhansl_kessler+mwb_modes".
    int profile_version = 1;
    std::array<double, 12> histogram{}; ///< Duration-weighted pitch-class histogram (quarter notes).
};

/// Duration-weighted pitch-class histogram in quarter notes, durations bounded by the options.
[[nodiscard]] std::array<double, 12> pitch_class_histogram(std::span<const NoteSpan> notes,
                                                           const KeyEstimationOptions& options);

/// Estimates the key of the given notes (callers exclude drum tracks; analyze_sequence does).
[[nodiscard]] KeyEstimate estimate_key(std::span<const NoteSpan> notes, const KeyEstimationOptions& options = {});

// --- Basic analysis ------------------------------------------------------------------------------------

/// Origin of the representative tempo (sequence_summary.tempo_origin).
enum class TempoOrigin : std::uint8_t { explicit_tempo, default_tempo, reference, unknown };
/// Origin of the key (sequence_summary.key_origin).
enum class KeyOrigin : std::uint8_t { manual, signature, estimated, unknown };

[[nodiscard]] std::string_view tempo_origin_name(TempoOrigin origin) noexcept;
[[nodiscard]] std::string_view key_origin_name(KeyOrigin origin) noexcept;

/// Program change observed on a track.
struct ProgramUse {
    std::uint8_t channel = 0;
    std::uint8_t program = 0;
    std::optional<std::uint8_t> bank_msb;
    std::optional<std::uint8_t> bank_lsb;
    Tick tick = 0;
    friend bool operator==(const ProgramUse&, const ProgramUse&) = default;
};

/// Row of the catalog track_summary table (section 23.2).
struct TrackSummary {
    std::size_t track_no = 0;
    std::string name;
    std::uint16_t channel_mask = 0;     ///< Bit n = channel n used by a channel event.
    std::vector<ProgramUse> programs;
    std::size_t note_count = 0;
    std::optional<int> min_pitch;
    std::optional<int> max_pitch;
    std::size_t max_polyphony = 0;
    bool is_drum = false;               ///< Explicitly marked as drums.
    bool drum_candidate = false;        ///< Marked as drums or every note on MIDI channel 10.

    /// program_json: [{"channel":c,"program":p,"bank_msb":m|null,"bank_lsb":l|null,"tick":t}, ...]
    [[nodiscard]] std::string program_json() const;
    /// feature_json: versioned object with pitch range, polyphony and drum flags.
    [[nodiscard]] std::string feature_json() const;
};

/// A time signature or key signature found in the sequence.
struct MeterChange {
    Tick tick = 0;
    int numerator = 4;
    int denominator = 4;
};
struct KeySignature {
    Tick tick = 0;
    int sharps_flats = 0;
    bool minor = false;
};

/// Optional inputs of the basic analysis.
struct AnalysisOptions {
    KeyEstimationOptions key;           ///< ppq is overwritten with the sequence PPQ.
    std::optional<std::pair<int, std::string>> manual_key; ///< User key (root, mode) when chosen.
    bool include_drums_in_key = false;
};

/// Row of the catalog sequence_summary table plus its diagnostics envelope (section 23.2).
struct SequenceSummary {
    int analysis_version = analysis_algorithm_version;
    std::string division;               ///< "ppq" or "smpte".
    double duration_seconds = 0;
    std::optional<double> quarter_length; ///< Length in quarter notes (PPQ only).
    std::optional<double> tempo_min;      ///< Quarter-note BPM.
    std::optional<double> tempo_max;
    std::optional<double> tempo_representative;
    TempoOrigin tempo_origin = TempoOrigin::unknown;
    std::size_t tempo_change_count = 0;
    std::optional<int> key_root;
    std::optional<std::string> key_mode;
    KeyOrigin key_origin = KeyOrigin::unknown;
    std::optional<double> key_confidence;
    KeyEstimate key_estimate;
    std::size_t track_count = 0;
    std::size_t event_count = 0;
    std::size_t note_count = 0;
    std::optional<int> min_pitch;
    std::optional<int> max_pitch;
    std::size_t max_polyphony = 0;
    bool has_drums = false;
    double note_density = 0;            ///< Notes per second (0 for empty material).
    std::uint16_t channel_mask = 0;
    std::vector<MeterChange> meters;
    std::vector<KeySignature> key_signatures;
    bool has_lyrics = false;
    bool has_markers = false;
    bool has_sysex = false;
    std::size_t orphan_note_ends = 0;
    std::size_t ambiguous_notes = 0;
    std::vector<std::string> warnings;
    std::vector<TrackSummary> tracks;

    /// diagnostics_json envelope (version 1): meters, key signatures, event-class presence,
    /// channels, key alternatives and warnings.
    [[nodiscard]] std::string diagnostics_json() const;
};

/// Representative tempo (section 27.1): the median tempo weighted by each segment's
/// quarter-note length through `end_tick`; ties resolve to the slower tempo. Returns microseconds
/// per quarter note.
[[nodiscard]] std::int64_t representative_tempo(const Sequence& sequence, Tick end_tick);

/// Computes the basic analysis of one sequence.
[[nodiscard]] SequenceSummary analyze_sequence(const Sequence& sequence, const AnalysisOptions& options = {});

} // namespace mwb::music
