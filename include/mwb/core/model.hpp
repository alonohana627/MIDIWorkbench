#pragma once

#include "mwb/core/event.hpp"
#include "mwb/core/ids.hpp"
#include "mwb/core/time.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mwb {

/// Default scale mask: chromatic (all twelve pitch classes).
inline constexpr std::uint16_t chromatic_scale_mask = 0x0FFF;

/// Policy for newly entered notes relative to the project scale (section 9.3).
enum class InputScalePolicy : std::uint8_t { allow_all, reject_out_of_scale, nearest_member, diatonic_keyboard };

/// sequence.settings_json version 1 (section 23.1).
struct SequenceSettings {
    Rational snap{1, 16};                 ///< Snap grid as a fraction of a whole note.
    std::optional<RegionId> loop_region;  ///< Loop region, or none.
    bool highlight_scale = false;
    bool fold_to_scale = false;
    InputScalePolicy input_policy = InputScalePolicy::allow_all;
    bool nearest_tie_up = false;          ///< Nearest-member ties go upward when true.
    std::uint8_t default_velocity = 100;
    Rational default_note_length{1, 16};
    friend bool operator==(const SequenceSettings&, const SequenceSettings&) = default;
};

/// Where a track sends its events (section 13.1).
enum class OutputKind : std::uint8_t { internal_synth, soundfont, external_midi };

/// track.routing_json version 1.
struct RoutingSettings {
    OutputKind output = OutputKind::internal_synth;
    std::optional<std::string> endpoint_id;     ///< Logical endpoint name for external output.
    std::optional<std::uint8_t> channel_override; ///< 0..15; playback-only until baked.
    std::int32_t transpose = 0;                 ///< Playback-only semitone offset.
    bool send_sysex = false;
    friend bool operator==(const RoutingSettings&, const RoutingSettings&) = default;
};

/// track.instrument_json version 1.
struct InstrumentSettings {
    std::string backend = "builtin";            ///< "builtin" or "sf2".
    std::optional<std::string> bank_sha256;     ///< SoundFont identity when backend is sf2.
    std::uint16_t bank = 0;
    std::uint8_t program = 0;
    std::string synth_preset = "classic";       ///< Built-in synth timbre identifier.
    friend bool operator==(const InstrumentSettings&, const InstrumentSettings&) = default;
};

/// An ordered event container with presentation and routing settings (section 2 "Track").
/// Events are kept sorted by (tick, order_key).
struct Track {
    TrackId id;
    std::string name;
    std::uint32_t color_rgba = 0x4F8FE8FF;
    Tick end_tick = 0;
    bool muted = false;
    bool solo = false;
    bool is_drum = false;
    RoutingSettings routing;
    InstrumentSettings instrument;
    std::vector<Event> events;

    /// Latest event tick, or 0 for an empty track.
    [[nodiscard]] Tick last_event_tick() const { return events.empty() ? 0 : events.back().tick; }
    friend bool operator==(const Track&, const Track&) = default;
};

/// Named half-open time range over a track selection (section 2 "Region").
struct Region {
    RegionId id;
    std::string name;
    Tick start_tick = 0;
    Tick end_tick = 1;
    std::vector<TrackId> tracks; ///< Empty means all tracks of the sequence.
    bool is_loop = false;
    friend bool operator==(const Region&, const Region&) = default;
};

/// Time-bounded scale override for display and new input (section 9.3).
struct ScaleRegion {
    RegionId id;
    Tick start_tick = 0;
    Tick end_tick = 1;
    std::uint8_t root_pc = 0;
    std::uint16_t scale_mask = chromatic_scale_mask;
    std::string scale_name;
    friend bool operator==(const ScaleRegion&, const ScaleRegion&) = default;
};

/// Stored transformation provenance (section 10, table recipe).
struct Recipe {
    RecipeId id;
    std::string tool_id;
    std::int32_t algorithm_version = 1;
    std::array<std::uint8_t, 32> source_semantic_hash{};
    std::string parameters_json = "{}";
    std::string targets_json = "[]";
    friend bool operator==(const Recipe&, const Recipe&) = default;
};

/// One independent timeline (section 2 "Sequence").
struct Sequence {
    SequenceId id;
    std::string name;
    Division division;
    Tick end_tick = 0;
    std::uint8_t root_pc = 0;
    std::uint16_t scale_mask = chromatic_scale_mask;
    SequenceSettings settings;
    std::vector<Track> tracks;
    std::vector<Region> regions;
    std::vector<ScaleRegion> scale_regions;
    std::vector<Recipe> recipes;

    /// Index of a track by identity.
    [[nodiscard]] std::optional<std::size_t> track_index(TrackId track) const;
    /// Maximum of explicit track end ticks, region ends and the sequence end.
    [[nodiscard]] Tick effective_end() const;
    friend bool operator==(const Sequence&, const Sequence&) = default;
};

/// A track's properties without its events, and a sequence's without its tracks: property edits
/// copy only these (copying the whole track or sequence is O(events)).
[[nodiscard]] Track track_header(const Track& track);
[[nodiscard]] Sequence sequence_header(const Sequence& sequence);

/// Recording take lifecycle (table recording_take.status).
enum class TakeStatus : std::uint8_t { captured, accepted, interrupted, discarded };

/// One bounded chunk of raw capture evidence (take_chunk, codec v1).
struct TakeChunk {
    std::int32_t ordinal = 0;
    std::int64_t first_time_ns = 0;
    std::vector<std::uint8_t> payload;
    friend bool operator==(const TakeChunk&, const TakeChunk&) = default;
};

/// A recording take with its raw capture evidence (section 8.4).
struct RecordingTake {
    TakeId id;
    TrackId track;
    std::string name;
    std::int64_t created_ms = 0;
    Tick start_tick = 0;
    Tick end_tick = 0;
    TakeStatus status = TakeStatus::captured;
    std::string capture_json = "{}";
    std::vector<TakeChunk> chunks;
    friend bool operator==(const RecordingTake&, const RecordingTake&) = default;
};

/// Imported original bytes kept for Original export (section 16.5).
struct SourceBlob {
    Uuid id;
    std::array<std::uint8_t, 32> sha256{};
    std::string original_name;
    std::string media_type = "smf"; ///< "smf" or "rmid".
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    std::string import_report_json = "{}";
    friend bool operator==(const SourceBlob& left, const SourceBlob& right) {
        return left.id == right.id && left.sha256 == right.sha256 && left.original_name == right.original_name &&
               left.media_type == right.media_type && left.import_report_json == right.import_report_json;
    }
};

/// The authoritative editable model of one native document (section 18.1).
struct DocumentData {
    DocumentId id;
    std::string title = "Untitled";
    std::int64_t created_ms = 0;
    std::int64_t modified_ms = 0;
    Generation generation = 0;
    std::string metadata_json = "{}";
    std::vector<Sequence> sequences;
    std::vector<SourceBlob> sources;
    std::vector<RecordingTake> takes;
    std::map<std::string, std::string> settings; ///< Whitelisted document_setting keys -> JSON.

    [[nodiscard]] std::optional<std::size_t> sequence_index(SequenceId sequence) const;
    friend bool operator==(const DocumentData&, const DocumentData&) = default;
};

/// Location of a track inside a document.
struct TrackLocation {
    std::size_t sequence = 0;
    std::size_t track = 0;
};

/// Finds a track anywhere in the document.
[[nodiscard]] std::optional<TrackLocation> find_track(const DocumentData& document, TrackId track);

/// Blank document per section 5.1: one sequence, one melodic track, PPQ 960, 120 BPM, 4/4,
/// four-bar loop region, snap 1/16, velocity 100, root C, highlighting off, internal synth.
[[nodiscard]] DocumentData make_blank_document(std::string title = "Untitled");

/// Current wall clock in UTC milliseconds.
[[nodiscard]] std::int64_t now_utc_ms();

} // namespace mwb
