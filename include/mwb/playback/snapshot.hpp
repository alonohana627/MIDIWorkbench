#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/hashing.hpp"
#include "mwb/core/model.hpp"
#include "mwb/core/tempo_map.hpp"
#include "mwb/core/time.hpp"
#include "mwb/playback/channel_state.hpp"
#include "mwb/playback/realtime_event.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace mwb::playback {

/// Controller chase behaviour at seek, start and loop restart (specification section 13.5).
enum class ChaseMode : std::uint8_t {
    state,                 ///< Default: restore controllers, never restart notes begun earlier.
    state_and_held_notes,  ///< Also restart notes crossing the target with their remaining duration.
                           ///< The original envelope phase cannot be recovered.
};

/// Display label of a chase mode; the held-note mode carries its limitation in the label.
[[nodiscard]] const char* chase_mode_label(ChaseMode mode) noexcept;

/// Transport/session settings that affect playback but are not document edits (section 13.3).
struct SessionSettings {
    std::optional<TimeRange> loop;      ///< Loop range [begin,end) with positive length, or none.
    Rational tempo_multiplier{1, 1};    ///< Multiplier on PPQ tempo (2/1 plays twice as fast).
    std::int32_t preview_transpose = 0; ///< Semitones applied to melodic (non-drum) tracks.
    friend bool operator==(const SessionSettings&, const SessionSettings&) = default;
};

/// Inputs of compile_playback().
struct PlaybackOptions {
    SequenceId sequence;                     ///< Sequence to play; nil selects the first sequence.
    std::uint32_t sample_rate = 48000;       ///< Frame rate of the frame mapping (device or render rate).
    SessionSettings session;
    bool include_muted_tracks = false;       ///< Offline render option: ignore mute/solo.
    bool master_mute = false;                ///< Master mute always wins (section 12).
    bool metronome_clicks = true;            ///< Compile beat clicks (the scheduler decides whether to emit).
    std::size_t max_events_per_block = 8192; ///< Real-time budget (section 20.2).
    std::uint32_t preflight_block_frames = 4096; ///< Largest audio block the live engine renders.
    std::size_t max_routes = 256;            ///< Upper bound on distinct routes.
    std::size_t checkpoint_interval = 1024;  ///< Events between controller checkpoints.
};

/// A logical destination plus MIDI channel (section 2 "Route"). Events sharing a route share
/// channel state; tracks sharing a route cannot have independent program or bend state.
struct Route {
    OutputKind output = OutputKind::internal_synth;
    std::string endpoint;               ///< Logical endpoint name for external output.
    std::uint8_t channel = 0;           ///< Effective channel after the playback-only override.
    bool drum = false;                  ///< A drum track (or channel 10) feeds this route.
    InstrumentSettings instrument;      ///< Instrument of the first track using the route.
    std::uint64_t key = 0;              ///< Stable identity hash of (output, endpoint, channel).
    std::uint32_t first_track = 0;      ///< Index of the first track using the route.
};

/// Role of a scheduled event with respect to note ownership.
enum class NoteRole : std::uint8_t { none, note_on, note_off };

/// Marker for "no note".
inline constexpr std::uint32_t no_note = std::numeric_limits<std::uint32_t>::max();

/// One event of the flattened, stably ordered snapshot stream.
struct ScheduledEvent {
    Tick tick = 0;
    std::int64_t frame = 0;          ///< Exact frame index on the snapshot timeline (rounded once).
    std::uint32_t route = no_route;  ///< Route index for MIDI events.
    std::uint32_t note = no_note;    ///< Index into PlaybackSnapshot::notes for note events.
    std::uint32_t value = 0;         ///< Tempo (microseconds/quarter) or click accent.
    RealtimeEventKind kind = RealtimeEventKind::midi;
    NoteRole role = NoteRole::none;
    std::uint8_t size = 0;
    std::array<std::uint8_t, 3> bytes{};
};

/// One playable note with its owner identity (the note-on event ID, stable across snapshots).
struct NoteBoundary {
    EventId owner;
    std::uint32_t route = 0;
    std::uint8_t channel = 0;
    std::uint8_t pitch = 0;          ///< Sounding pitch after routing and preview transpose.
    std::uint8_t velocity = 0;
    std::uint8_t release_velocity = 0;
    Tick start_tick = 0;
    Tick end_tick = 0;
    std::int64_t start_frame = 0;
    std::int64_t end_frame = 0;
};

/// Exact tick <-> frame mapping for one sequence, sample rate and tempo multiplier. Elapsed time
/// is kept as an exact rational and rounded (half away from zero) only at the frame boundary, so
/// tempo segments never accumulate rounding error (section 17.2). SMPTE sequences ignore the
/// tempo multiplier (absolute time, section 13.3).
class TimeMapping {
public:
    /// Exact frame position as numerator / denominator (denominator > 0).
    struct ExactFrame {
        __int128 numerator = 0;
        __int128 denominator = 1;
    };

    TimeMapping() = default;
    /// Validates the rate (1..768000) and multiplier (positive) and builds the mapping.
    [[nodiscard]] static Result<TimeMapping> make(TempoMap tempo_map, std::uint32_t sample_rate,
                                                  Rational tempo_multiplier);

    [[nodiscard]] ExactFrame exact_frame(Tick tick) const noexcept;
    /// Frame index of `tick`, rounded half away from zero.
    [[nodiscard]] std::int64_t frame_at(Tick tick) const noexcept;
    /// Smallest tick whose frame is >= `frame` (the first tick not yet played at that frame).
    [[nodiscard]] Tick first_tick_at_or_after_frame(std::int64_t frame) const noexcept;
    [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }
    [[nodiscard]] const TempoMap& tempo_map() const noexcept { return tempo_map_; }
    [[nodiscard]] Rational tempo_multiplier() const noexcept { return multiplier_; }

private:
    TempoMap tempo_map_;
    std::uint32_t sample_rate_ = 48000;
    Rational multiplier_{1, 1};
    __int128 denominator_ = 1;
};

/// Loop range compiled to frames with an exact rational length (section 13.4).
struct LoopInfo {
    Tick start_tick = 0;
    Tick end_tick = 0;
    std::int64_t start_frame = 0;
    std::int64_t end_frame = 0;
    __int128 length_numerator = 0;   ///< Exact loop length in frames = numerator / denominator.
    __int128 length_denominator = 1;
    std::size_t first_event = 0;     ///< First event index with tick >= start_tick.
    std::size_t end_event = 0;       ///< First event index with tick >= end_tick.
};

/// Diagnostics gathered while compiling.
struct SnapshotStatistics {
    std::size_t audible_tracks = 0;
    std::size_t notes = 0;
    std::size_t notes_out_of_range = 0;   ///< Dropped because transposition left 0..127.
    std::size_t muted_notes = 0;          ///< Note-ons carrying the muted flag (not played).
    std::size_t orphan_note_ends = 0;     ///< Note ends without a note-on (never sent).
    std::size_t route_conflicts = 0;      ///< Tracks sharing a route with different instruments.
    std::size_t skipped_sysex = 0;        ///< SysEx packets (never replayed automatically).
    std::size_t peak_events_per_block = 0;///< Maximum events inside any preflight block window.
};

/// Immutable playback snapshot compiled off the real-time thread from a document sequence
/// (section 13.4). It holds the flattened stable event order, resolved routes, the exact time
/// mapping, note boundaries with owner identity, controller checkpoints keyed by route, the loop
/// range and the real-time preflight verdict. Shared read-only between threads.
struct PlaybackSnapshot {
    Generation document_generation = 0;
    std::uint64_t snapshot_generation = 0; ///< Unique, increasing per compile in this process.
    SequenceId sequence;
    Sha256Digest source_semantic_hash{};   ///< Semantic hash of the compiled sequence.
    SessionSettings session;
    TimeMapping mapping;
    std::vector<Route> routes;
    std::vector<ScheduledEvent> events;
    std::vector<NoteBoundary> notes;
    std::vector<std::pair<EventId, std::uint32_t>> notes_by_owner; ///< Sorted by owner.
    std::vector<ChannelState> baseline_states;   ///< Per route, before any event.
    std::vector<std::size_t> checkpoint_events;  ///< Event index of each checkpoint (first is 0).
    std::vector<ChannelState> checkpoint_states; ///< checkpoint count x route count.
    std::optional<LoopInfo> loop;
    Tick end_tick = 0;
    std::int64_t end_frame = 0;
    bool live_playback_allowed = true;
    std::string live_refusal;               ///< Explanation when live playback is refused.
    SnapshotStatistics statistics;

    /// Index of the first event with tick >= `tick`.
    [[nodiscard]] std::size_t first_event_at_or_after(Tick tick) const noexcept;
    /// Index of the note with the given owner, or no_note.
    [[nodiscard]] std::uint32_t find_note(EventId owner) const noexcept;
    /// Writes the controller state of every route just before event `event_index` into `states`
    /// (which must hold routes.size() entries). Bounded work: one checkpoint copy plus at most
    /// checkpoint_interval events. Real-time safe.
    void state_before(std::size_t event_index, std::span<ChannelState> states) const noexcept;
};

/// Compiles a playback snapshot (section 19.2 sketch). Runs off the real-time thread; checks the
/// stop token every few thousand events. Fails with limit_exceeded when the route limit is hit,
/// invalid_argument for bad options and cancelled when stopped. A pathological event density is
/// not an error: the snapshot is produced with live_playback_allowed = false (section 20.2).
[[nodiscard]] Result<std::unique_ptr<const PlaybackSnapshot>> compile_playback(const DocumentData& document,
                                                                             const PlaybackOptions& options,
                                                                             std::stop_token stop = {});

/// Solo/mute rule of section 12: master mute silences everything; otherwise any soloed track is
/// audible unless muted; with no solo every unmuted track is audible.
[[nodiscard]] std::vector<bool> audible_tracks(const Sequence& sequence, bool master_mute);

} // namespace mwb::playback
