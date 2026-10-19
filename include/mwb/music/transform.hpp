#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/model.hpp"
#include "mwb/core/time.hpp"
#include "mwb/music/chord.hpp"
#include "mwb/music/grid.hpp"
#include "mwb/music/note_edit.hpp"
#include "mwb/music/progression.hpp"
#include "mwb/music/scale.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <variant>
#include <vector>

/// Note and rhythm transformations (specification section 10). Every tool is a pure function
/// from an immutable document snapshot, a TargetSelection and validated parameters to a
/// TransformPreview (change set plus report). Applying the preview is apply_change_set and is
/// one undo entry; cancelling discards the preview. The same input, parameters and seed always
/// produce the same events (newly created notes receive fresh identities; their content is
/// identical). Parameters serialize to versioned JSON for recipes.
namespace mwb::music {

/// How a displacement that would leave 0..127 or cross tick 0 is handled (section 7.3).
enum class RangePolicy : std::uint8_t {
    reject = 0,    ///< Fail with an explanation; nothing changes.
    constrain = 1, ///< Reduce the whole displacement until every note fits; never clamp notes individually.
};

/// How notes whose individual result would be out of range are handled.
enum class OutOfRangeAction : std::uint8_t {
    reject = 0, ///< Fail with an explanation (default).
    skip = 1,   ///< Leave those notes unchanged and report them as skipped.
};

// --- Quantize (section 10.1) -----------------------------------------------------------------

/// Which note edges quantization moves.
enum class QuantizeTarget : std::uint8_t { starts = 0, ends = 1, starts_and_ends = 2 };

struct QuantizeParameters {
    GridSpec grid;                       ///< Grid, swing ratio and origin.
    Rational strength{1, 1};             ///< 0..1 (UI shows 0-100%).
    QuantizeTarget target = QuantizeTarget::starts;
    bool preserve_duration = true;       ///< With starts only: the end moves by the same delta.
    std::optional<Tick> max_movement;    ///< Notes needing a larger onset move stay unchanged (skipped).
    friend bool operator==(const QuantizeParameters&, const QuantizeParameters&) = default;
};
/// t_new = round_tick(t + s * (g - t)) with exact rational intermediates; half-tick ties round
/// away from zero; nearest-grid ties choose the earlier grid point. Starts-and-ends mode clamps
/// durations to one tick and reports collisions. Unselected notes never move.
[[nodiscard]] Result<TransformPreview> quantize(const DocumentData& document, const TargetSelection& selection,
                                                const QuantizeParameters& parameters);

// --- Humanize (section 10.2) -----------------------------------------------------------------

/// Random offset distribution. Both use DeterministicRandom (PCG32) with its specified mapping.
enum class RandomDistribution : std::uint8_t {
    uniform = 0,    ///< range(-r, r)
    triangular = 1, ///< triangular(r): sum of two uniform draws in [0, r] minus r
};

struct HumanizeParameters {
    std::uint64_t seed = 1;
    Tick timing_range = 10;              ///< Maximum onset offset in ticks (either direction).
    int velocity_range = 8;              ///< Maximum velocity offset.
    Tick duration_range = 0;             ///< Maximum duration offset in ticks.
    RandomDistribution distribution = RandomDistribution::uniform;
    bool keep_chord_groups = true;       ///< One timing offset per onset group.
    bool prevent_order_inversions = true; ///< Onset groups of one track/channel line keep their order.
    std::optional<Tick> chord_tolerance; ///< Onset grouping tolerance; default 1/96 quarter.
    friend bool operator==(const HumanizeParameters&, const HumanizeParameters&) = default;
};
/// Draw order is fixed: groups in (start, track) order; per group one timing draw (or one per
/// note when groups are not kept), then per note in group order a velocity draw and a duration
/// draw. Starts never go below zero, velocities clamp to 1..127 and durations to one tick
/// (counted as clipped).
[[nodiscard]] Result<TransformPreview> humanize(const DocumentData& document, const TargetSelection& selection,
                                                const HumanizeParameters& parameters);

// --- Transpose and move ------------------------------------------------------------------------

enum class TransposeMode : std::uint8_t { semitones = 0, octaves = 1, scale_degrees = 2 };

struct TransposeParameters {
    TransposeMode mode = TransposeMode::semitones;
    int amount = 0;                      ///< Semitones, octaves or scale degrees.
    int scale_root = 0;                  ///< Scale for degree mode.
    std::uint16_t scale_mask = 0x0AB5;   ///< Interval mask for degree mode (default major).
    OutOfScalePolicy out_of_scale = OutOfScalePolicy::preserve_chromatic_offset;
    NearestTie tie = NearestTie::downward;
    bool exclude_drums = true;           ///< Notes on drum tracks are skipped.
    RangePolicy range_policy = RangePolicy::reject;
    friend bool operator==(const TransposeParameters&, const TransposeParameters&) = default;
};
/// Chromatic transposition moves every note by the same amount; with constrain the amount is
/// reduced (toward zero) until every selected note fits 0..127. Degree mode moves by ordered
/// scale degrees; with constrain the degree count is reduced until every note fits.
[[nodiscard]] Result<TransformPreview> transpose(const DocumentData& document, const TargetSelection& selection,
                                                 const TransposeParameters& parameters);

struct MoveParameters {
    Tick ticks = 0;
    int semitones = 0;
    RangePolicy range_policy = RangePolicy::constrain; ///< Section 7.3 default.
    friend bool operator==(const MoveParameters&, const MoveParameters&) = default;
};
/// Moves the selection preserving internal timing and intervals. A move that would cross tick 0
/// or pitch 0/127 constrains the entire displacement of that axis (section 7.3).
[[nodiscard]] Result<TransformPreview> move_notes(const DocumentData& document, const TargetSelection& selection,
                                                  const MoveParameters& parameters);

// --- Velocity ----------------------------------------------------------------------------------

enum class VelocityMode : std::uint8_t {
    set = 0,      ///< v = value
    add = 1,      ///< v = v + value
    scale = 2,    ///< v = round(v * factor)
    ramp = 3,     ///< Linear by onset from ramp_start to ramp_end across the selection.
    curve = 4,    ///< v = v + round(curve * v * (127 - v) / 12700), curve in -100..100.
    compress = 5, ///< Maps the selection's [min, max] linearly onto [range_low, range_high].
};
enum class VelocityTarget : std::uint8_t { note_on = 0, release = 1 };

struct VelocityParameters {
    VelocityMode mode = VelocityMode::set;
    VelocityTarget target = VelocityTarget::note_on;
    int value = 100;
    Rational factor{1, 1};
    int ramp_start = 64;
    int ramp_end = 127;
    int curve = 0;
    int range_low = 40;
    int range_high = 110;
    friend bool operator==(const VelocityParameters&, const VelocityParameters&) = default;
};
/// Sounding note-on velocities clamp deliberately to 1..127 and release velocities to 0..127;
/// clamps are counted. Release velocity of notes ended by velocity-zero note-ons is skipped.
[[nodiscard]] Result<TransformPreview> change_velocity(const DocumentData& document, const TargetSelection& selection,
                                                       const VelocityParameters& parameters);

// --- Length ------------------------------------------------------------------------------------

enum class LengthMode : std::uint8_t {
    set = 0,            ///< duration = ticks
    multiply = 1,       ///< duration = round(duration * factor)
    add = 2,            ///< duration = duration + ticks
    legato_to_next = 3, ///< End at the next later onset of the same track/channel line.
    gap = 4,            ///< End `ticks` before the next later onset of the line.
};

struct LengthParameters {
    LengthMode mode = LengthMode::set;
    Tick ticks = 240;
    Rational factor{1, 1};
    friend bool operator==(const LengthParameters&, const LengthParameters&) = default;
};
/// Durations never fall below one tick (clamps counted). Unterminated notes are skipped; the
/// last note of a line keeps its length in legato and gap modes.
[[nodiscard]] Result<TransformPreview> change_length(const DocumentData& document, const TargetSelection& selection,
                                                     const LengthParameters& parameters);

// --- Time stretch ------------------------------------------------------------------------------

/// Event classes included in a time stretch. Bit flags.
namespace event_class {
inline constexpr std::uint8_t notes = 1u << 0;        ///< Selected note spans.
inline constexpr std::uint8_t controllers = 1u << 1;  ///< CC, pitch bend, pressures, program change.
inline constexpr std::uint8_t meta = 1u << 2;         ///< Meta events (tempo, meter, text, ...).
inline constexpr std::uint8_t sysex = 1u << 3;        ///< SysEx packets.
} // namespace event_class

struct TimeStretchParameters {
    Rational factor{2, 1};               ///< Positive rational.
    Tick anchor = 0;
    std::uint8_t event_classes = event_class::notes;
    friend bool operator==(const TimeStretchParameters&, const TimeStretchParameters&) = default;
};
/// t' = anchor + round((t - anchor) * factor) per event from its exact original position.
/// Non-note classes are taken from the selected tracks (inside the selection range when given).
/// Reports the maximum rounding error; durations keep at least one tick. Results before tick 0
/// are rejected.
[[nodiscard]] Result<TransformPreview> time_stretch(const DocumentData& document, const TargetSelection& selection,
                                                    const TimeStretchParameters& parameters);

// --- Reverse (section 10.3) --------------------------------------------------------------------

/// Controller handling for reverse. Reversing CC timestamps is not equivalent playback, so the
/// only supported policy leaves controllers in place and reports how many lie in the range.
enum class ControllerReversePolicy : std::uint8_t { leave_unchanged = 0 };

struct ReverseParameters {
    std::optional<TimeRange> range;      ///< [L, R); defaults to the selection range or note extent.
    ControllerReversePolicy controllers = ControllerReversePolicy::leave_unchanged;
    friend bool operator==(const ReverseParameters&, const ReverseParameters&) = default;
};
/// Maps every selected note [start, end) lying inside [L, R) to [L+R-end, L+R-start). Notes
/// crossing the range boundary are skipped and reported.
[[nodiscard]] Result<TransformPreview> reverse(const DocumentData& document, const TargetSelection& selection,
                                               const ReverseParameters& parameters);

// --- Invert pitch ------------------------------------------------------------------------------

struct InvertPitchParameters {
    int axis_pitch = 60;
    bool axis_between_keys = false;      ///< Axis lies at axis_pitch + 1/2.
    OutOfRangeAction out_of_range = OutOfRangeAction::reject;
    friend bool operator==(const InvertPitchParameters&, const InvertPitchParameters&) = default;
};
/// p' = 2 * axis - p (plus one when the axis lies between keys).
[[nodiscard]] Result<TransformPreview> invert_pitch(const DocumentData& document, const TargetSelection& selection,
                                                    const InvertPitchParameters& parameters);

// --- Rotate ------------------------------------------------------------------------------------

struct RotateParameters {
    TimeRange range;                     ///< Explicit [L, R), required.
    Tick shift = 0;                      ///< Positive moves later; wraps inside the range.
    friend bool operator==(const RotateParameters&, const RotateParameters&) = default;
};
/// Circular time shift inside the range. Parts of notes outside the range stay; a rotated part
/// that crosses R is split at the boundary and continues from L (splits are reported).
[[nodiscard]] Result<TransformPreview> rotate(const DocumentData& document, const TargetSelection& selection,
                                              const RotateParameters& parameters);

// --- Repeat ------------------------------------------------------------------------------------

struct RepeatParameters {
    int count = 1;                       ///< Additional copies (1..1024).
    Tick length = 0;                     ///< Copy spacing; 0 = selection range length or note extent.
    int pitch_step = 0;                  ///< Semitones added per repetition.
    int velocity_step = 0;               ///< Velocity added per repetition (clamped 1..127).
    OutOfRangeAction out_of_range = OutOfRangeAction::reject;
    friend bool operator==(const RepeatParameters&, const RepeatParameters&) = default;
};
[[nodiscard]] Result<TransformPreview> repeat(const DocumentData& document, const TargetSelection& selection,
                                              const RepeatParameters& parameters);

// --- Strum -------------------------------------------------------------------------------------

enum class StrumOrder : std::uint8_t { up = 0, down = 1, alternate = 2, random = 3 };

struct StrumParameters {
    StrumOrder order = StrumOrder::up;
    Tick spread = 60;                    ///< Total spread from first to last onset of a chord.
    std::uint64_t seed = 1;              ///< Used by the random order.
    bool preserve_chord_grouping = true; ///< Ends stay at their original ticks.
    std::optional<Tick> chord_tolerance;
    friend bool operator==(const StrumParameters&, const StrumParameters&) = default;
};
/// Within each onset group the k-th note (in order) starts at group_start + round(spread*k/(n-1)).
/// Alternate flips direction on every other chord; random shuffles with Fisher-Yates using
/// DeterministicRandom::bounded.
[[nodiscard]] Result<TransformPreview> strum(const DocumentData& document, const TargetSelection& selection,
                                             const StrumParameters& parameters);

// --- Arpeggiate --------------------------------------------------------------------------------

enum class ArpeggioPattern : std::uint8_t { up = 0, down = 1, up_down = 2, as_played = 3, random = 4 };

struct ArpeggiateParameters {
    ArpeggioPattern pattern = ArpeggioPattern::up;
    Rational rate{1, 16};                ///< Step as a fraction of a whole note.
    int octaves = 1;                     ///< 1..4 octave range.
    Rational gate{1, 2};                 ///< Sounding fraction of a step, (0, 1].
    Rational swing{1, 2};                ///< Pair swing ratio anchored to each chord's onset.
    std::uint64_t seed = 1;              ///< Used by the random pattern.
    std::optional<Tick> chord_tolerance;
    friend bool operator==(const ArpeggiateParameters&, const ArpeggiateParameters&) = default;
};
/// Each onset group (chord) is replaced by concrete notes stepping through its pitches from its
/// onset until its latest end. No probability events are created (section 10.3).
[[nodiscard]] Result<TransformPreview> arpeggiate(const DocumentData& document, const TargetSelection& selection,
                                                  const ArpeggiateParameters& parameters);

// --- Euclidean rhythm --------------------------------------------------------------------------

struct EuclideanParameters {
    int steps = 16;                      ///< 1..256
    int pulses = 5;                      ///< 0..steps
    int rotation = 0;                    ///< Rotates the pattern later by this many steps.
    int pitch = 36;
    std::uint8_t channel = 9;
    Rational step_length{1, 16};
    Tick start = 0;
    int cycles = 1;                      ///< Pattern repetitions (1..1024).
    Rational gate{1, 2};
    int velocity = 90;
    int accent_velocity = 120;
    std::vector<int> accent_pulses{0};   ///< Pulse indices (0-based) that use the accent velocity.
    friend bool operator==(const EuclideanParameters&, const EuclideanParameters&) = default;
};
/// Step i (after rotation) is a pulse when (i * pulses) mod steps < pulses, the maximally even
/// distribution with an onset on step 0. Notes go to the first selected track.
[[nodiscard]] Result<TransformPreview> euclidean_rhythm(const DocumentData& document, const TargetSelection& selection,
                                                        const EuclideanParameters& parameters);
/// The pattern alone (true = pulse), for previews and drum-grid display.
[[nodiscard]] Result<std::vector<bool>> euclidean_pattern(int steps, int pulses, int rotation);

// --- Groove ------------------------------------------------------------------------------------

/// Timing and velocity offsets per grid slot, extracted from reference notes.
struct Groove {
    std::int32_t ppq = default_ppq;      ///< Resolution of timing_offsets.
    GridSpec grid;                       ///< Reference grid (origin included).
    int steps = 16;                      ///< Slots per groove cycle.
    std::vector<Tick> timing_offsets;    ///< Mean onset offset from the grid point per slot.
    std::vector<int> velocity_offsets;   ///< Mean velocity minus overall mean velocity per slot.
    std::vector<int> sample_counts;      ///< Notes measured per slot.
    friend bool operator==(const Groove&, const Groove&) = default;
};

struct GrooveExtractParameters {
    GridSpec grid;
    int steps = 16;
    friend bool operator==(const GrooveExtractParameters&, const GrooveExtractParameters&) = default;
};
/// Measures onset offsets from the nearest reference grid point and velocity deviations from
/// the mean, averaged per slot (means rounded half away from zero).
[[nodiscard]] Result<Groove> extract_groove(const DocumentData& document, const TargetSelection& selection,
                                            const GrooveExtractParameters& parameters);

struct GrooveApplyParameters {
    Groove groove;
    GridSpec target_grid;                ///< Grid whose points receive the groove slots.
    Rational strength{1, 1};             ///< Timing strength 0..1.
    Rational velocity_strength{1, 1};    ///< Velocity strength 0..1.
    bool preserve_duration = true;
    friend bool operator==(const GrooveApplyParameters&, const GrooveApplyParameters&) = default;
};
/// Each selected onset moves toward its nearest target grid point plus that slot's groove offset
/// (rescaled to the sequence PPQ): t' = round(t + s * (g + offset - t)).
[[nodiscard]] Result<TransformPreview> apply_groove(const DocumentData& document, const TargetSelection& selection,
                                                    const GrooveApplyParameters& parameters);
[[nodiscard]] std::string groove_to_json(const Groove& groove);
[[nodiscard]] Result<Groove> groove_from_json(std::string_view json);

// --- Cleanup -----------------------------------------------------------------------------------

enum class DuplicatePolicy : std::uint8_t {
    exact = 0,           ///< Same track, channel, pitch, start, end, velocity and release (default).
    ignore_velocity = 1, ///< Same track, channel, pitch, start and end.
    same_onset = 2,      ///< Same track, channel, pitch and start; the longest note is kept.
};
struct RemoveDuplicatesParameters {
    DuplicatePolicy policy = DuplicatePolicy::exact;
    bool across_tracks = false;          ///< Compare notes of different tracks too.
    friend bool operator==(const RemoveDuplicatesParameters&, const RemoveDuplicatesParameters&) = default;
};
/// Keeps the first note of every duplicate set (pairing order) and removes the others.
[[nodiscard]] Result<TransformPreview> remove_duplicates(const DocumentData& document, const TargetSelection& selection,
                                                         const RemoveDuplicatesParameters& parameters);

enum class OverlapAction : std::uint8_t {
    trim_earlier = 0, ///< The earlier note ends where the later one starts.
    merge = 1,        ///< One note from the earliest start to the latest end (first velocity).
    leave = 2,        ///< Report only.
};
struct ResolveOverlapsParameters {
    OverlapAction action = OverlapAction::trim_earlier;
    friend bool operator==(const ResolveOverlapsParameters&, const ResolveOverlapsParameters&) = default;
};
/// Only notes of the same track, channel (route) and pitch are considered. Overlaps of notes
/// with equal starts are ambiguous: they are reported and left unchanged.
[[nodiscard]] Result<TransformPreview> resolve_overlaps(const DocumentData& document, const TargetSelection& selection,
                                                        const ResolveOverlapsParameters& parameters);

struct SplitParameters {
    std::vector<Tick> points;            ///< Explicit split ticks.
    friend bool operator==(const SplitParameters&, const SplitParameters&) = default;
};
/// Splits every selected note at each point strictly inside it.
[[nodiscard]] Result<TransformPreview> split_notes(const DocumentData& document, const TargetSelection& selection,
                                                   const SplitParameters& parameters);

struct JoinParameters {
    Tick max_gap = 0;                    ///< Join when 0 <= next.start - previous.end <= max_gap.
    friend bool operator==(const JoinParameters&, const JoinParameters&) = default;
};
/// Joins consecutive selected notes of the same track, channel and pitch whose gap satisfies the
/// rule; overlapping notes are not joined and are reported as skipped.
[[nodiscard]] Result<TransformPreview> join_notes(const DocumentData& document, const TargetSelection& selection,
                                                  const JoinParameters& parameters);

// --- Scale tools (section 9) -------------------------------------------------------------------

struct ConformToScaleParameters {
    int scale_root = 0;
    std::uint16_t scale_mask = 0x0AB5;
    NearestTie tie = NearestTie::downward;
    bool exclude_drums = true;
    friend bool operator==(const ConformToScaleParameters&, const ConformToScaleParameters&) = default;
};
/// Moves out-of-scale selected notes to the nearest member (explicit, previewed, undoable).
/// When the nearest member would leave 0..127 the other neighbour is used.
[[nodiscard]] Result<TransformPreview> conform_to_scale(const DocumentData& document, const TargetSelection& selection,
                                                        const ConformToScaleParameters& parameters);

/// Changes only a sequence's scale selection and display flags (highlight/fold). Events are
/// never touched (section 9.1); the change set contains a single sequence property change.
[[nodiscard]] Result<ChangeSet> set_sequence_scale(const DocumentData& document, SequenceId sequence,
                                                   const Scale& scale, bool highlight, bool fold);

// --- Generators --------------------------------------------------------------------------------

/// Inserts rendered chord notes into a track. Out-of-range pitches reject by default; with
/// `skip` they are omitted and reported.
[[nodiscard]] Result<TransformPreview> insert_chord(const DocumentData& document, TrackId track,
                                                    const ChordSpec& chord, const ChordRenderSpec& render,
                                                    OutOfRangeAction out_of_range = OutOfRangeAction::reject);

/// Materializes a voiced progression into a track and stores its recipe (tool "progression") in
/// the sequence recipes as provenance for regeneration (section 9.5).
[[nodiscard]] Result<TransformPreview> insert_progression(const DocumentData& document, TrackId track,
                                                          std::span<const ProgressionStep> steps,
                                                          const VoiceLeadingOptions& options, Tick start,
                                                          std::uint8_t channel);

/// Inserts generated notes (from any generator) into a track on a channel.
[[nodiscard]] Result<TransformPreview> insert_notes(const DocumentData& document, TrackId track,
                                                    std::span<const GeneratedNote> notes, std::uint8_t channel,
                                                    std::string tool);

// --- Uniform lifecycle ---------------------------------------------------------------------------

/// Every recipe-capable tool's parameters.
using TransformParameters =
    std::variant<QuantizeParameters, HumanizeParameters, TransposeParameters, MoveParameters, VelocityParameters,
                 LengthParameters, TimeStretchParameters, ReverseParameters, InvertPitchParameters, RotateParameters,
                 RepeatParameters, StrumParameters, ArpeggiateParameters, EuclideanParameters, GrooveApplyParameters,
                 RemoveDuplicatesParameters, ResolveOverlapsParameters, SplitParameters, JoinParameters,
                 ConformToScaleParameters>;

/// Stable tool identifier ("quantize", "humanize", ...).
[[nodiscard]] std::string_view tool_id(const TransformParameters& parameters) noexcept;
/// Algorithm version stored with recipes (currently 1 for every tool).
[[nodiscard]] int algorithm_version(const TransformParameters& parameters) noexcept;
/// True for tools that consume a PRNG seed.
[[nodiscard]] bool uses_random_seed(const TransformParameters& parameters) noexcept;

/// Runs the tool named by the parameters.
[[nodiscard]] Result<TransformPreview> preview_transform(const DocumentData& document,
                                                         const TargetSelection& selection,
                                                         const TransformParameters& parameters);

/// Versioned JSON: {"format":"mwb.transform","tool":..., "version":..., "parameters":{...},
/// "prng":{"name":"pcg32","version":1,"seed":...} (seeded tools only)}.
[[nodiscard]] std::string parameters_to_json(const TransformParameters& parameters);
/// Parses the form written by parameters_to_json.
[[nodiscard]] Result<TransformParameters> parameters_from_json(std::string_view json);

/// Recipe provenance for a transformation: tool, algorithm version, source sequence semantic
/// hash, parameters and targets. A new recipe identity is generated.
[[nodiscard]] Result<Recipe> make_recipe(const DocumentData& document, const TargetSelection& selection,
                                         const TransformParameters& parameters);
/// Re-runs a stored recipe. Fails with `conflict` when the sequence no longer has the recorded
/// semantic hash unless `allow_changed_source` is set.
[[nodiscard]] Result<TransformPreview> replay_recipe(const DocumentData& document, const Recipe& recipe,
                                                     bool allow_changed_source = false);

} // namespace mwb::music
