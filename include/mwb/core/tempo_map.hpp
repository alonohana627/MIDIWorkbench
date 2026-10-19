#pragma once

#include "mwb/core/model.hpp"
#include "mwb/core/time.hpp"

#include <cstdint>
#include <vector>

namespace mwb {

/// Maps ticks to absolute time for one sequence (section 17.2). Durations are accumulated
/// exactly as sum(delta_ticks * microseconds_per_quarter); rounding happens only at the
/// requested boundary such as an audio sample index.
class TempoMap {
public:
    /// One constant-tempo segment starting at `tick`.
    struct Segment {
        Tick tick = 0;
        std::int64_t microseconds_per_quarter = default_microseconds_per_quarter;
        /// Exact elapsed time at `tick` in units of microseconds / PPQ (PPQ sequences) or
        /// ticks (SMPTE sequences).
        __int128 elapsed_scaled = 0;
    };

    /// Builds the map from tempo meta events of every track in stable merged order
    /// (tick, track index, order key). At equal ticks the last event wins.
    [[nodiscard]] static TempoMap build(const Sequence& sequence);
    /// Map with a constant tempo (useful for tests and SMPTE reference tempos).
    [[nodiscard]] static TempoMap constant(const Division& division, std::int64_t microseconds_per_quarter);

    [[nodiscard]] const std::vector<Segment>& segments() const { return segments_; }
    [[nodiscard]] const Division& division() const { return division_; }
    /// True when a tempo event exists at tick 0 (otherwise the 120 BPM default was assumed).
    [[nodiscard]] bool has_explicit_initial_tempo() const { return explicit_initial_; }
    /// Number of tempo events that changed the tempo.
    [[nodiscard]] std::size_t change_count() const { return segments_.size() - 1; }
    /// True when two different tempo values were found at the same tick (type-1 conflict).
    [[nodiscard]] bool has_conflicts() const { return conflicts_; }

    /// Segment index active at `tick`.
    [[nodiscard]] std::size_t segment_at(Tick tick) const;
    /// Microseconds per quarter active at `tick`.
    [[nodiscard]] std::int64_t tempo_at(Tick tick) const;
    /// Exact elapsed time: microseconds * PPQ for PPQ sequences, ticks for SMPTE sequences.
    [[nodiscard]] __int128 elapsed_scaled(Tick tick) const;
    /// Elapsed microseconds, rounded half away from zero.
    [[nodiscard]] std::int64_t tick_to_microseconds(Tick tick) const;
    /// Audio frame index for `tick` at `sample_rate`, rounded half away from zero.
    [[nodiscard]] std::int64_t tick_to_frame(Tick tick, std::uint32_t sample_rate) const;
    /// Seconds as floating point for display and DSP only.
    [[nodiscard]] double tick_to_seconds(Tick tick) const;
    /// Largest tick whose time does not exceed the given microseconds.
    [[nodiscard]] Tick microseconds_to_tick(std::int64_t microseconds) const;
    /// Largest tick whose audio frame does not exceed `frame`.
    [[nodiscard]] Tick frame_to_tick(std::int64_t frame, std::uint32_t sample_rate) const;

private:
    Division division_;
    std::vector<Segment> segments_;
    bool explicit_initial_ = false;
    bool conflicts_ = false;
};

/// A decoded time signature active from `tick`.
struct MeterSegment {
    Tick tick = 0;
    std::int32_t numerator = 4;
    std::int32_t denominator = 4;
    std::int64_t first_bar = 0;          ///< Zero-based index of the bar starting at `tick`.
    bool previous_bar_partial = false;   ///< The preceding bar was cut short by this change.
};

/// Musical position for display: zero-based internally, one-based in the default UI.
struct BarBeatTick {
    std::int64_t bar = 0;
    std::int64_t beat = 0;
    Tick tick_in_beat = 0;
};

/// Maps ticks to bars and beats for one PPQ sequence (section 17.3).
class MeterMap {
public:
    [[nodiscard]] static MeterMap build(const Sequence& sequence);
    [[nodiscard]] static MeterMap constant(std::int32_t ppq, std::int32_t numerator, std::int32_t denominator);

    [[nodiscard]] const std::vector<MeterSegment>& segments() const { return segments_; }
    [[nodiscard]] const MeterSegment& segment_at(Tick tick) const;
    /// Ticks spanned by one bar of the given meter (floor of numerator*4*PPQ/denominator).
    [[nodiscard]] Tick ticks_per_bar(const MeterSegment& segment) const;
    /// Ticks spanned by one displayed beat (a 1/denominator note).
    [[nodiscard]] Tick ticks_per_beat(const MeterSegment& segment) const;
    /// Start tick of the zero-based bar index.
    [[nodiscard]] Tick bar_start(std::int64_t bar) const;
    /// Position of `tick`.
    [[nodiscard]] BarBeatTick position(Tick tick) const;
    /// "bar.beat.tick" one-based display string.
    [[nodiscard]] std::string format(Tick tick) const;
    [[nodiscard]] std::int32_t ppq() const { return ppq_; }

private:
    std::int32_t ppq_ = default_ppq;
    std::vector<MeterSegment> segments_;
};

} // namespace mwb
