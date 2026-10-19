#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/hashing.hpp"
#include "mwb/core/model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

/// Musical content search of specification section 26: canonical motif streams, the versioned
/// motif-stream codec, XXH3-64 n-gram keys, exact matching (26.4) and approximate matching (26.5).
namespace mwb::search {

/// Motif-stream codec version written by encode_motif_stream.
inline constexpr std::uint16_t motif_codec_version = 1;
/// Extraction/scoring profile version (constants of sections 26.2-26.5).
inline constexpr std::uint16_t motif_profile_version = 1;
/// Gram lengths indexed by default (section 26.3).
inline constexpr std::array<std::size_t, 2> default_gram_lengths{3, 5};

/// Voice extraction profile (section 26.2).
enum class VoiceKind : std::uint8_t { top = 1, bottom = 2 };
/// Stored motif_stream.voice_kind name (the music module's profile name).
[[nodiscard]] std::string_view voice_kind_name(VoiceKind kind) noexcept;

/// One note of a canonical stream with its source event ordinal (index in the sequence's
/// deterministic note pairing).
struct MotifNote {
    Tick onset = 0;
    Tick end = 0;
    std::uint8_t pitch = 0;
    std::uint8_t velocity = 0;
    std::uint32_t ordinal = 0;
    friend bool operator==(const MotifNote&, const MotifNote&) = default;
};

/// Named extraction settings; their hash is stored in every stream header.
struct ExtractionSettings {
    VoiceKind voice = VoiceKind::top;
    /// Onset grouping tolerance in ticks; nullopt derives 1/96 quarter note (PPQ) or 10 ms (SMPTE).
    std::optional<Tick> onset_tolerance;
    bool include_drums = false;           ///< Drum tracks and channel 10 are skipped by default.
    std::size_t minimum_notes = 2;        ///< Shorter streams are not stored.
};

/// Effective onset tolerance for a division.
[[nodiscard]] Tick onset_tolerance_ticks(const ExtractionSettings& settings, const Division& division);
/// SHA-256 identity of the settings applied to a division (stored in stream headers).
[[nodiscard]] Sha256Digest extraction_settings_hash(const ExtractionSettings& settings, const Division& division);

/// A canonical per-track/channel voice stream.
struct MotifStream {
    std::uint16_t profile_version = motif_profile_version;
    VoiceKind voice = VoiceKind::top;
    Division division;
    Sha256Digest settings_hash{};
    std::uint32_t sequence_no = 0;
    std::uint32_t track_no = 0;
    std::uint8_t channel = 0;
    std::vector<MotifNote> notes;
    friend bool operator==(const MotifStream&, const MotifStream&) = default;
};

/// Splits a sequence by track and channel and extracts one voice stream per split through the
/// music module's voice profiles (drum tracks and channel 10 are skipped unless included).
[[nodiscard]] Result<std::vector<MotifStream>> extract_motif_streams(const Sequence& sequence, std::uint32_t sequence_no,
                                                             const ExtractionSettings& settings = {});

/// Codec v1: explicit little-endian header ("MWMS", versions, voice, division, settings hash,
/// location, note count) followed by varint records (onset delta, duration, pitch, velocity,
/// ordinal delta). No native structs are serialized.
[[nodiscard]] std::vector<std::uint8_t> encode_motif_stream(const MotifStream& stream);
/// Decodes and validates bounds, versions, counts and monotonic onset/ordinal order.
[[nodiscard]] Result<MotifStream> decode_motif_stream(std::span<const std::uint8_t> bytes,
                                                      std::size_t max_notes = 1'000'000);

/// Token families (section 26.3).
enum class TokenFamily : std::uint8_t { pitch = 1, interval = 2, rhythm = 3, contour = 4 };
/// Stored family name ("pitch", "interval", "rhythm", "contour").
[[nodiscard]] std::string_view token_family_name(TokenFamily family) noexcept;
/// Parses a stored family name.
[[nodiscard]] std::optional<TokenFamily> parse_token_family(std::string_view name) noexcept;
/// Notes consumed before the first token: pitch 0, interval/contour 1, rhythm 2.
[[nodiscard]] std::size_t token_offset(TokenFamily family) noexcept;

/// Minimal note view shared by streams and queries.
struct QueryNote {
    Tick onset = 0;
    Tick duration = 0;
    std::uint8_t pitch = 60;
    std::uint8_t velocity = 100;
};
/// Query notes from stream notes.
[[nodiscard]] std::vector<QueryNote> to_query_notes(std::span<const MotifNote> notes);

/// Token sequence of a family (music module token definitions v1). Token k describes notes
/// k .. k + token_offset(family). Rhythm tokens are the music module's canonical ratio bins and
/// serve candidate retrieval only; verification always uses the original ticks.
[[nodiscard]] std::vector<std::int32_t> family_tokens(std::span<const QueryNote> notes, TokenFamily family);

/// Key of one n-gram.
struct GramKey {
    TokenFamily family = TokenFamily::interval;
    std::uint8_t length = 3;
    std::uint64_t hash = 0;
    friend auto operator<=>(const GramKey&, const GramKey&) = default;
};
/// XXH3-64 (fixed project seed) over family, length and little-endian int32 tokens.
[[nodiscard]] std::uint64_t gram_hash(TokenFamily family, std::span<const std::int32_t> tokens);
/// Eight stored bytes of a gram hash, big-endian so byte order equals numeric order.
[[nodiscard]] std::array<std::uint8_t, 8> gram_hash_bytes(std::uint64_t hash) noexcept;
/// Inverse of gram_hash_bytes.
[[nodiscard]] std::uint64_t gram_hash_from_bytes(std::span<const std::uint8_t> bytes) noexcept;

/// One indexed gram occurrence (a motif_gram row).
struct MotifGram {
    GramKey key;
    std::uint32_t start_ordinal = 0; ///< Index of the gram's first note in the stream.
    Tick start_tick = 0;
};
/// Grams of every family and default length for a stream.
[[nodiscard]] std::vector<MotifGram> stream_grams(const MotifStream& stream);

// --- Index access ---------------------------------------------------------------------------------

/// Location metadata of a stored stream.
struct StreamInfo {
    std::int64_t stream_id = 0;
    std::int64_t revision_id = 0;
    std::int64_t asset_id = 0;
    std::uint32_t sequence_no = 0;
    std::uint32_t track_no = 0;
    std::uint8_t channel = 0;
};

/// A stored stream with its location.
struct StoredMotifStream {
    StreamInfo info;
    MotifStream stream;
};

/// One posting of a gram key.
struct GramPosting {
    std::int64_t stream_id = 0;
    std::uint32_t start_ordinal = 0;
    friend auto operator<=>(const GramPosting&, const GramPosting&) = default;
};

/// Read access to a motif index (SQLite catalog or in-memory). Implementations only return streams
/// of one consistent profile version.
class MotifIndexReader {
public:
    virtual ~MotifIndexReader() = default;
    /// Number of postings of a key, counted up to `cap` (a result equal to `cap` means "at least
    /// cap"): ranking only needs to know which grams are rare, and a full count of a ubiquitous
    /// gram would cost as much as reading its postings.
    [[nodiscard]] virtual Result<std::uint64_t> count_postings(const GramKey& key, std::uint64_t cap) = 0;
    /// Postings ordered by (stream_id, start_ordinal), at most `limit`.
    [[nodiscard]] virtual Result<std::vector<GramPosting>> postings(const GramKey& key, std::size_t limit) = 0;
    /// Loads and decodes one stream; nullopt when it no longer exists.
    [[nodiscard]] virtual Result<std::optional<StoredMotifStream>> load_stream(std::int64_t stream_id) = 0;
    /// Stream IDs greater than `after_id` in ascending order (constrained scans), at most `limit`.
    [[nodiscard]] virtual Result<std::vector<std::int64_t>> stream_ids(std::int64_t after_id, std::size_t limit) = 0;
};

/// In-memory motif index for tests and current-document search.
class InMemoryMotifIndex final : public MotifIndexReader {
public:
    /// Adds a stream (stream_id must be unique) and indexes its grams.
    void add(StoredMotifStream stream);

    [[nodiscard]] Result<std::uint64_t> count_postings(const GramKey& key, std::uint64_t cap) override;
    [[nodiscard]] Result<std::vector<GramPosting>> postings(const GramKey& key, std::size_t limit) override;
    [[nodiscard]] Result<std::optional<StoredMotifStream>> load_stream(std::int64_t stream_id) override;
    [[nodiscard]] Result<std::vector<std::int64_t>> stream_ids(std::int64_t after_id, std::size_t limit) override;

private:
    std::map<std::int64_t, StoredMotifStream> streams_;
    std::map<GramKey, std::vector<GramPosting>> postings_;
};

// --- Exact matching (section 26.4) ----------------------------------------------------------------

/// Exact search modes (section 26.1).
enum class MatchMode : std::uint8_t { absolute, transposed, contour, rhythm };

/// Continuation after the last returned occurrence.
struct ExactContinuation {
    std::int64_t stream_id = 0;
    std::uint32_t first_note = 0;
};

/// Exact query: 1-256 notes in a chosen mode, optionally also constraining rhythm.
struct ExactQuery {
    MatchMode mode = MatchMode::transposed;
    std::vector<QueryNote> notes;
    /// Also require proportional inter-onset intervals (rhythm is independent of pitch).
    bool require_rhythm = false;
    /// Allowed deviation of each scaled candidate inter-onset interval, in candidate ticks.
    Tick rhythm_tolerance = 0;
    std::size_t result_limit = 100;
    /// Continue after this occurrence (the previous result's `next`). A scan cut short by
    /// scan_stream_limit also yields `next`, so short queries can always continue.
    std::optional<ExactContinuation> after;
    /// Bound on postings read per gram and on streams scanned for short queries.
    std::size_t posting_limit = 1'000'000;
    std::size_t scan_stream_limit = 20'000;
};

/// One verified occurrence.
struct MotifOccurrence {
    StreamInfo stream;
    std::uint32_t first_note = 0;
    std::uint32_t end_note = 0; ///< Exclusive.
    Tick start_tick = 0;
    Tick end_tick = 0;
    int transpose = 0;                     ///< Candidate minus query pitch (absolute: 0).
    std::int64_t tempo_scale_numerator = 1;  ///< Candidate IOI / query IOI, reduced (1/1 if unknown).
    std::int64_t tempo_scale_denominator = 1;
    std::vector<std::uint32_t> ordinals;   ///< Source note ordinals of the matched notes.
};

/// Exact search result. `partial` is set whenever cancellation, a budget or the result limit cut
/// the search short; it is never presented as exhaustive in that case.
struct ExactSearchResult {
    std::vector<MotifOccurrence> occurrences;
    bool partial = false;
    std::string partial_reason;
    std::optional<ExactContinuation> next;
    std::size_t candidates_verified = 0;
    bool indexed = true; ///< False when a constrained scan was used (short queries).
};

/// Verifies one window of a stream against the query; returns the occurrence when it matches.
[[nodiscard]] std::optional<MotifOccurrence> verify_exact(const ExactQuery& query, const StoredMotifStream& stream,
                                                          std::uint32_t first_note);

/// Runs the exact search: rarest grams -> intersected candidates -> full verification.
[[nodiscard]] Result<ExactSearchResult> exact_search(MotifIndexReader& index, const ExactQuery& query,
                                                     std::stop_token stop = {});

// --- Approximate matching (section 26.5) ----------------------------------------------------------

/// Distance weights; stored with each query and result.
struct ApproximateWeights {
    double pitch = 0.45;
    double rhythm = 0.30;
    double duration = 0.10;
    double velocity = 0.05;
    double edit = 0.10;
};

/// Approximate query with the default budgets of section 26.5.
struct ApproximateQuery {
    std::vector<QueryNote> notes; ///< 4-256 notes.
    ApproximateWeights weights;
    bool rhythm_only = false;     ///< Disables pitch and velocity terms and renormalizes.
    std::size_t candidate_streams = 2000;
    std::size_t windows_per_stream = 20;
    std::size_t result_limit = 50;
    std::size_t posting_limit = 200'000;
};

/// Profile-1 alignment constants.
inline constexpr double alignment_gap_cost = 0.75;
inline constexpr double alignment_band_fraction = 0.20;
inline constexpr std::size_t alignment_min_band = 2;
inline constexpr double alignment_min_aligned_fraction = 0.70;

/// Pair of aligned notes (query index, candidate stream index).
struct AlignedPair {
    std::uint32_t query_index = 0;
    std::uint32_t candidate_index = 0;
};

/// Individual distance components with the effective weights (explanation of a score).
struct ScoreComponents {
    double pitch = 0.0;
    double rhythm = 0.0;
    double duration = 0.0;
    double velocity = 0.0;
    double edit = 0.0;
    double distance = 0.0;
    double score = 0.0;          ///< 100 * (1 - clamp(distance, 0, 1)).
    double time_scale = 1.0;     ///< Candidate time / query time (median matched IOI ratio).
    int transpose = 0;           ///< Median candidate-minus-query pitch.
    std::size_t matched = 0;
    std::size_t insertions = 0;  ///< Candidate notes inside the window left unmatched.
    std::size_t deletions = 0;   ///< Query notes left unmatched.
    std::size_t window_notes = 0;
    ApproximateWeights weights;  ///< Effective (renormalized) weights.
};

/// One approximate match with its explanation.
struct ApproximateMatch {
    StreamInfo stream;
    std::uint32_t first_note = 0;
    std::uint32_t end_note = 0; ///< Exclusive.
    Tick start_tick = 0;
    Tick end_tick = 0;
    ScoreComponents components;
    std::vector<AlignedPair> pairs;
    std::vector<std::uint32_t> ordinals;
    std::string explanation;
};

/// Approximate search result; always labeled approximate with its budget.
struct ApproximateSearchResult {
    std::vector<ApproximateMatch> matches;
    std::size_t streams_considered = 0;
    std::size_t windows_aligned = 0;
    bool candidate_budget_reached = false;
    bool partial = false;
    std::string partial_reason;
    std::string label; ///< "approximate search (budget: N streams, M windows per stream)".
};

/// Aligns the query against the candidate stream around `implied_start` with the banded monotone
/// DP and computes the displayed distance. Nullopt when fewer than 70% of query notes align.
[[nodiscard]] std::optional<ApproximateMatch> align_window(const ApproximateQuery& query,
                                                           const StoredMotifStream& candidate,
                                                           std::int64_t implied_start);

/// Gram-overlap candidate retrieval followed by bounded alignment and ranking.
[[nodiscard]] Result<ApproximateSearchResult> approximate_search(MotifIndexReader& index,
                                                                 const ApproximateQuery& query,
                                                                 std::stop_token stop = {});

} // namespace mwb::search
