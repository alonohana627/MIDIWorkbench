#pragma once

#include "mwb/core/hashing.hpp"
#include "mwb/core/model.hpp"
#include "mwb/core/validation.hpp"
#include "mwb/music/analysis.hpp"
#include "mwb/music/fingerprint.hpp"
#include "mwb/search/browse_index.hpp"
#include "mwb/search/motif.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

/// Rebuildable derived data written into the catalog for one immutable revision (specification
/// sections 23.2, 26.3, 27.1, 27.3): basic sequence/track summaries, fingerprints, motif streams
/// and feature vectors. All functions are pure; the library writes their results.
namespace mwb::library {

/// Version of the basic summary analyzer (sequence_summary.analysis_version, analysis_run "basic").
inline constexpr std::int32_t basic_analysis_version = music::analysis_algorithm_version;
/// Version of the fingerprint profiles.
inline constexpr std::int32_t fingerprint_profile_version = music::fingerprint_version;
/// Version and dimension of the feature-vector profile.
inline constexpr std::int32_t feature_profile_version = 1;
inline constexpr std::size_t feature_dimension = 32;
/// Upper bound of embedded MIDI text kept for full-text search (lyrics, markers, track names).
inline constexpr std::size_t searchable_text_limit = 64u * 1024u;

/// One track_summary row.
struct TrackSummary {
    std::uint32_t track_no = 0;
    std::string name;
    std::uint16_t channel_mask = 0;
    std::vector<std::uint8_t> programs; ///< Zero-based program numbers in first-use order.
    std::string program_json;           ///< Music module format: [{"channel","program","bank_msb","bank_lsb","tick"}].
    std::int64_t note_count = 0;
    std::string feature_json;
};

/// One sequence_summary row with its tracks.
struct SequenceSummary {
    std::uint32_t sequence_no = 0;
    bool ppq = true;
    double duration_seconds = 0.0;
    std::optional<double> quarter_length;
    std::optional<double> tempo_min;
    std::optional<double> tempo_max;
    std::optional<double> tempo_representative;
    std::string tempo_origin = "unknown";
    std::optional<int> key_root;
    std::optional<std::string> key_mode;
    std::string key_origin = "unknown";
    std::optional<double> key_confidence;
    std::int64_t track_count = 0;
    std::int64_t event_count = 0;
    std::int64_t note_count = 0;
    std::optional<int> min_pitch;
    std::optional<int> max_pitch;
    std::int64_t max_polyphony = 0;
    bool has_drums = false;
    std::vector<double> tempos;               ///< Distinct BPM values (diagnostics_json.tempos).
    std::vector<std::string> meters;          ///< "n/d" (diagnostics_json.meter_names).
    std::vector<std::string> event_classes;   ///< diagnostics_json.event_classes.
    std::int64_t tempo_change_count = 0;
    std::string diagnostics_json;             ///< Versioned envelope built from the fields above.
    std::vector<TrackSummary> tracks;

    /// Browse-index facts carrying exactly the stored values.
    [[nodiscard]] search::SequenceFacts facts() const;
};

/// Basic analysis of a whole document (section 27.1).
struct DocumentAnalysis {
    std::vector<SequenceSummary> sequences;
    std::string searchable_text;       ///< Lyrics, markers, text events and track names (bounded).
    bool searchable_text_truncated = false;
    std::size_t warning_count = 0;
};

/// Computes summaries through music::analyze_sequence (keys, tempo, polyphony, tracks) plus the
/// search envelope fields. `diagnostics` are the import diagnostics (counted as warnings).
[[nodiscard]] DocumentAnalysis analyze_document(const DocumentData& document,
                                                const std::vector<Diagnostic>& diagnostics = {});

/// Minimal diagnostics_json (version 1) with the search envelope fields of a summary built
/// without the music analysis (synthetic benchmark rows).
[[nodiscard]] std::string summary_diagnostics_json(const SequenceSummary& summary);

/// program_json in the music module's format for programs used on one channel.
[[nodiscard]] std::string program_json_for(const std::vector<std::uint8_t>& programs, std::uint8_t channel);

/// One fingerprint row (section 27.3).
struct Fingerprint {
    std::string profile; ///< exact_object, exact_events, exact_notes, transposition_invariant, rhythm_shape
    Sha256Digest sha256{};
};

/// The five fingerprint profiles (music module canonical encodings, SHA-256).
[[nodiscard]] std::vector<Fingerprint> compute_fingerprints(const DocumentData& document, const Sha256Digest& object_sha256);

/// One feature_vector row: little-endian IEEE-754 float32 values in the profile's named order.
struct FeatureVector {
    std::uint32_t sequence_no = 0;
    std::vector<float> values;
    /// Little-endian payload bytes (length = 4 * dimension).
    [[nodiscard]] std::vector<std::uint8_t> payload() const;
};

/// Names of the feature dimensions of profile version 1, in payload order.
[[nodiscard]] const std::vector<std::string>& feature_dimension_names();

/// Feature vectors per sequence (finite values only).
[[nodiscard]] Result<std::vector<FeatureVector>> compute_feature_vectors(const DocumentData& document);
/// The same from motif streams already extracted with compute_motif_streams (analysis computes
/// them once for both the motif index and the features).
[[nodiscard]] Result<std::vector<FeatureVector>> compute_feature_vectors(const DocumentData& document,
                                                                         std::span<const search::MotifStream> streams);

/// Motif streams of every sequence with the default extraction profile.
[[nodiscard]] Result<std::vector<search::MotifStream>> compute_motif_streams(const DocumentData& document);

} // namespace mwb::library
