#include "mwb/ui/editor_math.hpp"

#include "mwb/core/checked.hpp"

#include <algorithm>
#include <cmath>

namespace mwb::ui {

Grid Grid::make(Rational fraction, std::int32_t ppq) {
    Grid grid;
    grid.fraction = fraction.numerator > 0 && fraction.denominator > 0 ? fraction : Rational{1, 16};
    grid.ppq = ppq > 0 ? ppq : default_ppq;
    // step = 4 * ppq * numerator / denominator ticks
    const __int128 numerator = static_cast<__int128>(4) * grid.ppq * grid.fraction.numerator;
    grid.exact = numerator % grid.fraction.denominator == 0;
    return grid;
}

Tick Grid::point(std::int64_t index) const {
    // index * 4 * numerator can overflow for far-away indices: checked, saturating to 0 like the
    // division below (callers bound ticks well inside 64 bits).
    const auto scaled = checked_mul(index, 4 * fraction.numerator);
    if (!scaled)
        return 0;
    return mul_div_round(*scaled, ppq, fraction.denominator).value_or(0);
}

std::span<const SnapChoice> snap_choices() {
    static constexpr SnapChoice choices[] = {
        {"1/1", "1/1 (bar in 4/4)"}, {"1/2", "1/2"}, {"1/4", "1/4"}, {"1/8", "1/8"}, {"1/16", "1/16"},
        {"1/32", "1/32"}, {"1/64", "1/64"}, {"1/128", "1/128"}, {"1/6", "1/4 triplet"}, {"1/12", "1/8 triplet"},
        {"1/24", "1/16 triplet"}, {"1/48", "1/32 triplet"}, {"3/8", "Dotted 1/4"}, {"3/16", "Dotted 1/8"},
        {"3/32", "Dotted 1/16"}, {"1/20", "1/16 quintuplet"}, {"1/28", "1/16 septuplet"},
    };
    return choices;
}

double Grid::step() const {
    return 4.0 * ppq * static_cast<double>(fraction.numerator) / static_cast<double>(fraction.denominator);
}

Tick Grid::floor(Tick tick) const {
    // index = floor(tick * den / (4 * ppq * num)), then correct for rounding of point().
    std::int64_t index = mul_div_floor(tick, fraction.denominator, 4 * static_cast<std::int64_t>(ppq) * fraction.numerator)
                             .value_or(0);
    while (point(index) > tick)
        --index;
    while (point(index + 1) <= tick)
        ++index;
    return point(index);
}

Tick Grid::next(Tick tick) const {
    const Tick base = floor(tick);
    std::int64_t index = mul_div_floor(base, fraction.denominator, 4 * static_cast<std::int64_t>(ppq) * fraction.numerator)
                             .value_or(0);
    while (point(index) <= tick)
        ++index;
    return point(index);
}

Tick Grid::nearest(Tick tick) const {
    const Tick below = floor(tick);
    const Tick above = next(below);
    // Ties choose the earlier grid point (section 10.1).
    return (tick - below) <= (above - tick) ? below : above;
}

DragSnapshot DragSnapshot::capture(std::vector<NoteSpan> notes) {
    DragSnapshot snapshot;
    snapshot.notes = std::move(notes);
    if (snapshot.notes.empty())
        return snapshot;
    snapshot.earliest_start = snapshot.notes.front().start;
    snapshot.latest_end = snapshot.notes.front().end;
    for (const auto& note : snapshot.notes) {
        snapshot.earliest_start = std::min(snapshot.earliest_start, note.start);
        snapshot.latest_end = std::max(snapshot.latest_end, note.end);
        snapshot.lowest_pitch = std::min(snapshot.lowest_pitch, note.pitch);
        snapshot.highest_pitch = std::max(snapshot.highest_pitch, note.pitch);
    }
    return snapshot;
}

namespace {

Tick snap_delta(Tick anchor, Tick raw_delta, const Grid& grid, SnapMode mode) {
    switch (mode) {
    case SnapMode::off: return raw_delta;
    case SnapMode::relative: {
        const double steps = std::round(static_cast<double>(raw_delta) / grid.step());
        return grid.point(static_cast<std::int64_t>(steps));
    }
    case SnapMode::absolute: return grid.nearest(anchor + raw_delta) - anchor;
    }
    return raw_delta;
}

} // namespace

MoveResult compute_move(const DragSnapshot& snapshot, Tick raw_delta_ticks, int raw_delta_pitch, const Grid& grid,
                        SnapMode mode) {
    MoveResult result;
    if (snapshot.notes.empty())
        return result;
    Tick delta = snap_delta(snapshot.earliest_start, raw_delta_ticks, grid, mode);
    if (snapshot.earliest_start + delta < 0) {
        delta = -snapshot.earliest_start;
        result.constrained = true;
    }
    int pitch_delta = raw_delta_pitch;
    if (snapshot.lowest_pitch + pitch_delta < 0) {
        pitch_delta = -snapshot.lowest_pitch;
        result.constrained = true;
    }
    if (snapshot.highest_pitch + pitch_delta > 127) {
        pitch_delta = 127 - snapshot.highest_pitch;
        result.constrained = true;
    }
    result.delta_ticks = delta;
    result.delta_pitch = pitch_delta;
    return result;
}

Tick compute_end_delta(const DragSnapshot& snapshot, const NoteSpan& handle, Tick raw_delta, const Grid& grid,
                       SnapMode mode, Tick minimum_length) {
    Tick delta = snap_delta(handle.end, raw_delta, grid, mode);
    // One duration delta for the group; the shortest note limits shortening.
    for (const auto& note : snapshot.notes)
        delta = std::max(delta, minimum_length - note.duration());
    return delta;
}

Tick compute_start_delta(const DragSnapshot& snapshot, const NoteSpan& handle, Tick raw_delta, const Grid& grid,
                         SnapMode mode, Tick minimum_length) {
    Tick delta = snap_delta(handle.start, raw_delta, grid, mode);
    for (const auto& note : snapshot.notes) {
        delta = std::min(delta, note.duration() - minimum_length);
        delta = std::max(delta, -note.start);
    }
    return delta;
}

TimeRange grid_aligned_extent(const std::vector<NoteSpan>& notes, const Grid& grid) {
    if (notes.empty())
        return {};
    Tick start = notes.front().start;
    Tick end = notes.front().end;
    for (const auto& note : notes) {
        start = std::min(start, note.start);
        end = std::max(end, note.end);
    }
    const Tick aligned_start = grid.floor(start);
    Tick aligned_end = grid.floor(end);
    if (aligned_end < end)
        aligned_end = grid.next(aligned_end);
    if (aligned_end <= aligned_start)
        aligned_end = grid.next(aligned_start);
    return TimeRange{aligned_start, aligned_end};
}

std::vector<EventId> inserted_note_starts(const ChangeSet& changes) {
    std::vector<EventId> created;
    for (const auto& operation : changes.operations)
        if (const auto* insert = std::get_if<EventInsert>(&operation); insert && insert->event.is_note_start())
            created.push_back(insert->event.id);
    return created;
}

Tick ViewMapping::x_to_tick(double x) const {
    return origin_tick + static_cast<Tick>(std::floor(x * ticks_per_pixel));
}

int ViewMapping::y_to_pitch(double y) const {
    return top_pitch - static_cast<int>(std::floor(y / row_height));
}

void ViewMapping::zoom_time_around(double x, double factor) {
    const Tick anchor = x_to_tick(x);
    ticks_per_pixel = std::clamp(ticks_per_pixel / factor, 0.05, 1.0e7);
    origin_tick = std::max<Tick>(0, anchor - static_cast<Tick>(x * ticks_per_pixel));
}

void ViewMapping::zoom_pitch_around(double y, double factor) {
    const int anchor = y_to_pitch(y);
    row_height = std::clamp(row_height * factor, 4.0, 48.0);
    top_pitch = std::clamp(anchor + static_cast<int>(y / row_height), 0, 127);
}

} // namespace mwb::ui
