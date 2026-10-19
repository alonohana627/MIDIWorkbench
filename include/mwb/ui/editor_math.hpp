#pragma once

#include "mwb/core/change_set.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/core/time.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/// Qt-free editor arithmetic: grids, snapping and drag mathematics of specification section 7.3.
/// Kept separate from widgets so it is unit tested directly.
namespace mwb::ui {

/// A snap grid in ticks. `exact` is false when the PPQ cannot represent the rational grid and
/// points are rounded to project ticks ("Rounded to project ticks", section 7.4).
struct Grid {
    Rational fraction{1, 16};  ///< Fraction of a whole note.
    std::int32_t ppq = default_ppq;
    bool exact = true;

    /// Grid for a fraction of a whole note at a PPQ.
    [[nodiscard]] static Grid make(Rational fraction, std::int32_t ppq);
    /// Tick of grid point `index`, derived from the origin (never accumulated).
    [[nodiscard]] Tick point(std::int64_t index) const;
    /// Nearest grid point to `tick`; ties choose the earlier point.
    [[nodiscard]] Tick nearest(Tick tick) const;
    /// Largest grid point not after `tick`.
    [[nodiscard]] Tick floor(Tick tick) const;
    /// Smallest grid point strictly after `tick`.
    [[nodiscard]] Tick next(Tick tick) const;
    /// Approximate length of one grid step (display only).
    [[nodiscard]] double step() const;
};

/// One selectable grid value: the fraction of a whole note as text ("1/16") and its label. The
/// toolbar and the inspector share this list so both show every value the other can set.
struct SnapChoice {
    const char* value;
    const char* label;
};
[[nodiscard]] std::span<const SnapChoice> snap_choices();

/// Snap modes named in section 7.3.
enum class SnapMode : std::uint8_t {
    absolute, ///< Snap the anchor (earliest selected note) position to the grid.
    relative, ///< Snap the displacement to whole grid steps.
    off,      ///< No time snapping (Alt during drag).
};

/// The selected notes captured once at gesture start; every frame is computed from this snapshot.
struct DragSnapshot {
    std::vector<NoteSpan> notes;
    Tick earliest_start = 0;
    Tick latest_end = 0;
    std::uint8_t lowest_pitch = 127;
    std::uint8_t highest_pitch = 0;

    [[nodiscard]] static DragSnapshot capture(std::vector<NoteSpan> notes);
};

/// A constrained displacement for moving the whole selection.
struct MoveResult {
    Tick delta_ticks = 0;
    int delta_pitch = 0;
    bool constrained = false; ///< The raw displacement hit tick 0 or pitch 0/127 and was limited as a whole.
};

/// Computes a move: the whole displacement is constrained so no note crosses tick 0 or pitch
/// 0/127, preserving internal timing and intervals (never clamping notes individually).
[[nodiscard]] MoveResult compute_move(const DragSnapshot& snapshot, Tick raw_delta_ticks, int raw_delta_pitch,
                                      const Grid& grid, SnapMode mode);

/// Resizing the end of every selected note by one delta (group mode). Durations never fall below
/// `minimum_length` (1 tick for authored notes).
[[nodiscard]] Tick compute_end_delta(const DragSnapshot& snapshot, const NoteSpan& handle, Tick raw_delta,
                                     const Grid& grid, SnapMode mode, Tick minimum_length = 1);

/// Resizing starts while preserving ends.
[[nodiscard]] Tick compute_start_delta(const DragSnapshot& snapshot, const NoteSpan& handle, Tick raw_delta,
                                       const Grid& grid, SnapMode mode, Tick minimum_length = 1);

/// The smallest grid-aligned range containing the notes including their ends (section 7.6
/// duplicate-to-end when no explicit range is selected).
[[nodiscard]] TimeRange grid_aligned_extent(const std::vector<NoteSpan>& notes, const Grid& grid);

/// Identities of the note-ons a change set inserts, in operation order: the notes a draw, insert or
/// paste created, without diffing the whole note list.
[[nodiscard]] std::vector<EventId> inserted_note_starts(const ChangeSet& changes);

/// Maps between musical coordinates and pixels with 64-bit time (section 7.4).
struct ViewMapping {
    Tick origin_tick = 0;          ///< Tick at the left edge of the note area.
    double ticks_per_pixel = 4.0;
    int top_pitch = 84;            ///< Pitch whose row is at the top edge.
    double row_height = 12.0;

    [[nodiscard]] double tick_to_x(Tick tick) const {
        return static_cast<double>(tick - origin_tick) / ticks_per_pixel;
    }
    [[nodiscard]] Tick x_to_tick(double x) const;
    [[nodiscard]] double pitch_to_y(int pitch) const { return (top_pitch - pitch) * row_height; }
    [[nodiscard]] int y_to_pitch(double y) const;
    /// Zooms time around a pixel position so the tick under it stays put.
    void zoom_time_around(double x, double factor);
    /// Zooms pitch rows around a pixel position.
    void zoom_pitch_around(double y, double factor);
};

} // namespace mwb::ui
