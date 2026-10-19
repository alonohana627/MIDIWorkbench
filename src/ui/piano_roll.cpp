#include "mwb/ui/piano_roll.hpp"

#include "mwb/ui/theme.hpp"

#include "mwb/core/pitch.hpp"
#include "mwb/music/scale.hpp"
#include "mwb/ui/document_controller.hpp"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QImage>
#include <QKeySequence>
#include <QLineF>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <functional>

namespace mwb::ui {

namespace {

constexpr double handle_zone = 6.0;          // DIP, section 7.2
/// Hard cap for per-note painting; below it the pixel density decides (see paint_notes).
constexpr std::size_t density_threshold = 30000;
/// Visible notes per pixel column above which notes are summarized (level of detail, section 7.4).
constexpr double density_per_column = 6.0;
/// Above this many visible notes each note is a plain rectangle (no rounding, outline or label).
constexpr std::size_t simple_note_threshold = 4000;
constexpr int scrollbar_resolution = 100000;
constexpr std::size_t zoom_history_limit = 50;

/// Row colors while the scale is shown. Scale membership is the only thing rows show then:
/// scale rows are green (a hue no note color mode uses by default), the root rows a stronger
/// green, and rows outside the scale dark gray with hatching, so the pattern reads at a glance.
struct ScaleRowColors {
    QColor in;
    QColor root;
    QColor out;
    QColor hatch;
};

ScaleRowColors scale_row_colors(const RollColors& colors) {
    if (colors.high_contrast)
        return {QColor("#00703C"), QColor("#00B860"), QColor("#000000"), QColor("#4A4A4A")};
    if (colors.dark)
        return {QColor("#1F6B45"), QColor("#2FA066"), QColor("#0C0D0F"), QColor("#262A30")};
    return {QColor("#B4E6C6"), QColor("#3CAA6E"), QColor("#A9AFB8"), QColor("#8E959F")};
}

bool is_black_key(int pitch) {
    switch (pitch % 12) {
    case 1:
    case 3:
    case 6:
    case 8:
    case 10: return true;
    default: return false;
    }
}

QColor channel_color(int channel) {
    return QColor::fromHsv((channel * 37) % 360, 150, 220);
}

/// True when the segment from `a` to `b` touches `rect` (erase strokes between mouse samples).
bool segment_hits_rect(QPointF a, QPointF b, const QRectF& rect) {
    if (rect.contains(a) || rect.contains(b))
        return true;
    const QLineF segment(a, b);
    const QLineF edges[] = {QLineF(rect.topLeft(), rect.topRight()), QLineF(rect.topRight(), rect.bottomRight()),
                            QLineF(rect.bottomRight(), rect.bottomLeft()), QLineF(rect.bottomLeft(), rect.topLeft())};
    for (const auto& edge : edges)
        if (segment.intersects(edge) == QLineF::BoundedIntersection)
            return true;
    return false;
}

} // namespace

std::uint8_t default_channel_for(const DocumentController& controller) {
    const auto active = controller.active_track_index();
    if (!active)
        return 0;
    for (const auto& note : controller.notes().notes)
        if (note.on_track == *active)
            return note.channel; // continue the track's own channel
    // No notes yet: the track's channel events decide (a program change or controller on channel 11).
    for (const auto& event : controller.active_sequence().tracks[*active].events)
        if (event.is_channel())
            return event.channel();
    return controller.active_sequence().tracks[*active].is_drum ? 9 : 0;
}

bool set_loop_range(DocumentController& controller, TimeRange range) {
    if (range.end <= range.begin || range.begin < 0)
        return false;
    const auto& sequence = controller.active_sequence();
    Sequence updated = sequence_header(sequence); // properties only, no events
    Region* loop = nullptr;
    if (updated.settings.loop_region)
        for (auto& region : updated.regions)
            if (region.id == *updated.settings.loop_region)
                loop = &region;
    if (!loop) // reuse a loop region that was switched off, if any
        for (auto& region : updated.regions)
            if (region.is_loop) {
                loop = &region;
                break;
            }
    if (!loop) {
        Region region;
        region.id = RegionId::generate();
        region.name = "Loop";
        region.is_loop = true;
        updated.regions.push_back(std::move(region));
        loop = &updated.regions.back();
    }
    loop->start_tick = range.begin;
    loop->end_tick = range.end;
    updated.settings.loop_region = loop->id;
    ChangeBuilder builder(controller.document(), "Set loop");
    builder.set_sequence_properties(sequence.id, updated);
    auto changes = builder.finish();
    return !changes.empty() && controller.apply(changes);
}

PianoRollView::PianoRollView(DocumentController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAccessibleName(tr("Piano roll"));
    setAccessibleDescription(tr("Note editor. Use the event list for a keyboard-operable alternative."));
    setMinimumSize(320, 200);
    auto_scroll_timer_.setInterval(16);
    connect(&auto_scroll_timer_, &QTimer::timeout, this, &PianoRollView::auto_scroll_step);
    connect(&controller_, &DocumentController::document_changed, this, qOverload<>(&QWidget::update));
    connect(&controller_, &DocumentController::selection_changed, this, qOverload<>(&QWidget::update));
    connect(&controller_, &DocumentController::view_state_changed, this, qOverload<>(&QWidget::update));
    connect(&controller_, &DocumentController::edit_cursor_changed, this, qOverload<>(&QWidget::update));
    mapping_.ticks_per_pixel = 960.0 / 48.0; // one quarter note = 48 px
    mapping_.top_pitch = 72; // C4 sits mid-view in a fresh document
    mapping_.row_height = 16.0;
}

void PianoRollView::set_tool(EditTool tool) {
    cancel_gesture();
    const bool changed = tool != tool_;
    tool_ = tool;
    if (changed)
        emit tool_changed(tool);
    switch (tool) {
    case EditTool::select: setCursor(Qt::ArrowCursor); break;
    case EditTool::draw: setCursor(Qt::CrossCursor); break;
    case EditTool::erase: setCursor(Qt::ForbiddenCursor); break;
    case EditTool::split: setCursor(Qt::SplitHCursor); break;
    case EditTool::audition: setCursor(Qt::PointingHandCursor); break;
    }
}

void PianoRollView::set_color_mode(NoteColorMode mode) {
    color_mode_ = mode;
    update();
}

void PianoRollView::set_snap_mode(SnapMode mode) {
    snap_mode_ = mode;
    update();
}

Tick PianoRollView::visible_ticks() const {
    return std::max<Tick>(1, mapping_.x_to_tick(note_area().width()) - mapping_.origin_tick);
}

int PianoRollView::visible_row_count() const {
    return std::max(1, static_cast<int>(note_area().height() / std::max(1.0, mapping_.row_height)));
}

Grid PianoRollView::grid() const {
    const auto& sequence = controller_.active_sequence();
    const std::int32_t ppq = sequence.division.is_ppq() ? sequence.division.ppq : default_ppq;
    return Grid::make(sequence.settings.snap, ppq);
}

void PianoRollView::update_playhead_area(std::optional<Tick> tick) {
    if (!tick)
        return;
    // The line in the note area plus the triangle marker in the ruler (5 px each side).
    const double x = keyboard_width + mapping_.tick_to_x(*tick);
    if (x < keyboard_width - 8 || x > width() + 8)
        return;
    update(QRect(static_cast<int>(std::floor(x)) - 7, 0, 15, height()));
}

void PianoRollView::set_playhead(std::optional<Tick> tick) {
    if (playhead_ == tick)
        return;
    const bool state_change = playhead_.has_value() != tick.has_value();
    const auto previous = playhead_;
    playhead_ = tick;
    if (!tick)
        follow_suspended_ = false; // the next Play follows again
    if (tick && follow_playhead_ && !follow_suspended_ && gesture_ == Gesture::none) {
        // Page the view when the playhead leaves the window (or wraps back at a loop start).
        const Tick visible = visible_ticks();
        if (*tick < mapping_.origin_tick || *tick > mapping_.origin_tick + visible * 9 / 10) {
            mapping_.origin_tick = std::max<Tick>(0, *tick - visible / 10);
            emit mapping_changed();
            update();
            return;
        }
    }
    if (state_change) {
        update(); // the edit cursor changes style between stopped and playing
        return;
    }
    // Only the old and new playhead columns repaint at the transport rate.
    update_playhead_area(previous);
    update_playhead_area(tick);
}

void PianoRollView::set_follow_playhead(bool follow) {
    follow_playhead_ = follow;
    follow_suspended_ = false;
}

void PianoRollView::suspend_follow() {
    if (playhead_)
        follow_suspended_ = true; // a manual scroll while playing keeps the user's view
}

void PianoRollView::set_sounding_pitches(std::set<int> pitches) {
    if (pitches == sounding_)
        return;
    sounding_ = std::move(pitches);
    update();
}

void PianoRollView::set_preview_notes(std::vector<NoteSpan> notes, std::uint64_t generation) {
    preview_notes_ = std::move(notes);
    preview_generation_ = generation;
    update();
}

QRectF PianoRollView::note_area() const {
    return QRectF(keyboard_width, ruler_height, std::max(1, width() - keyboard_width),
                  std::max(1, height() - ruler_height));
}

QPointF PianoRollView::to_note_area(QPointF widget_point) const {
    return widget_point - QPointF(keyboard_width, ruler_height);
}

bool PianoRollView::in_scale(int pitch) const {
    const auto& sequence = controller_.active_sequence();
    // Membership formula of section 9.2 through the music module's Scale (no local copy).
    if (!scale_cache_ || scale_cache_->root_pitch_class() != sequence.root_pc ||
        scale_cache_->interval_mask() != sequence.scale_mask) {
        auto scale = music::Scale::custom(sequence.root_pc, sequence.scale_mask);
        if (!scale)
            return true;
        scale_cache_ = *scale;
    }
    return scale_cache_->contains(pitch);
}

bool PianoRollView::is_root(int pitch) const { return (pitch % 12) == controller_.active_sequence().root_pc; }

bool PianoRollView::note_editable(const NoteSpan& note) const {
    const auto active = controller_.active_track_index();
    return active && note.on_track == *active;
}

Tick PianoRollView::default_note_length() const {
    const auto& sequence = controller_.active_sequence();
    const auto length = Grid::make(sequence.settings.default_note_length,
                                   sequence.division.is_ppq() ? sequence.division.ppq : default_ppq);
    return std::max<Tick>(1, length.point(1));
}

std::uint8_t PianoRollView::default_velocity() const { return controller_.active_sequence().settings.default_velocity; }

SnapMode PianoRollView::effective_snap(Qt::KeyboardModifiers modifiers) const {
    if (modifiers & Qt::AltModifier)
        return SnapMode::off; // Alt bypasses time snapping during a drag (section 7.2)
    return snap_mode_;
}

Tick PianoRollView::snap_floor(Tick tick, Qt::KeyboardModifiers modifiers) const {
    return effective_snap(modifiers) == SnapMode::off ? tick : grid().floor(tick);
}

Tick PianoRollView::snap_nearest(Tick tick, Qt::KeyboardModifiers modifiers) const {
    return effective_snap(modifiers) == SnapMode::off ? tick : grid().nearest(tick);
}

PianoRollView::Hit PianoRollView::hit_test(QPointF widget_point) const {
    Hit hit;
    const QRectF area = note_area();
    if (!area.contains(widget_point))
        return hit;
    const QPointF point = to_note_area(widget_point);
    const Tick tick = mapping_.x_to_tick(point.x());
    const int pitch = pitch_at_y(point.y());
    const auto& index = controller_.note_index();
    const Tick slack = static_cast<Tick>(std::ceil(handle_zone * mapping_.ticks_per_pixel));
    const auto candidates = index.query(TimeRange{tick - slack, tick + slack + 1},
                                        static_cast<std::uint8_t>(std::clamp(pitch, 0, 127)),
                                        static_cast<std::uint8_t>(std::clamp(pitch, 0, 127)));
    // Prefer editable notes and the most recently starting one under the pointer. Hidden ghosts
    // (Show Other Tracks off) are never hit.
    std::optional<std::size_t> best;
    for (const auto position : candidates) {
        const auto& note = index.notes()[position];
        if (note.pitch != pitch)
            continue;
        if (!note_editable(note) && !controller_.show_ghost_tracks())
            continue;
        const double left = mapping_.tick_to_x(note.start);
        const double right = std::max(left + 2.0, mapping_.tick_to_x(note.end));
        if (point.x() < left - 1.0 || point.x() > right + 1.0)
            continue;
        if (!best || (note_editable(note) && !note_editable(index.notes()[*best])) ||
            (note_editable(note) == note_editable(index.notes()[*best]) && note.start >= index.notes()[*best].start))
            best = position;
    }
    if (!best)
        return hit;
    hit.note = best;
    const auto& note = index.notes()[*best];
    const double left = mapping_.tick_to_x(note.start);
    const double right = mapping_.tick_to_x(note.end);
    const double width_pixels = right - left;
    hit.zone = HitZone::body;
    if (width_pixels >= 3 * handle_zone) {
        if (point.x() >= right - handle_zone)
            hit.zone = HitZone::right_edge;
        else if (point.x() <= left + handle_zone)
            hit.zone = HitZone::left_edge;
    } else if (width_pixels >= 6.0 && point.x() >= right - width_pixels / 3.0) {
        // Short notes (a 1/16 at default zoom) keep an end-resize zone: their rightmost third.
        hit.zone = HitZone::right_edge;
    }
    return hit;
}

// --- Rows (chromatic or folded) ------------------------------------------------------------------

void PianoRollView::rebuild_fold_rows() const {
    const auto& sequence = controller_.active_sequence();
    // Cached per document generation, sequence and scale: painting calls this for every row.
    const FoldKey key{controller_.generation(), controller_.active_sequence_index(), sequence.scale_mask,
                      sequence.root_pc, sequence.settings.fold_to_scale};
    if (fold_key_ && *fold_key_ == key)
        return;
    fold_key_ = key;
    fold_rows_.clear();
    if (!sequence.settings.fold_to_scale)
        return;
    // Fold to scale plus every used pitch: hidden notes can never become unselectable (section 9.1).
    std::array<bool, 128> used{};
    for (const auto& note : controller_.notes().notes)
        used[note.pitch] = true;
    for (int pitch = 127; pitch >= 0; --pitch)
        if (in_scale(pitch) || used[static_cast<std::size_t>(pitch)])
            fold_rows_.push_back(pitch);
}

std::size_t PianoRollView::top_row_index() const {
    std::size_t index = 0;
    while (index + 1 < fold_rows_.size() && fold_rows_[index] > mapping_.top_pitch)
        ++index;
    return index;
}

double PianoRollView::row_y(int pitch) const {
    rebuild_fold_rows();
    if (fold_rows_.empty())
        return mapping_.pitch_to_y(pitch);
    const auto found = std::find_if(fold_rows_.begin(), fold_rows_.end(), [pitch](int row) { return row <= pitch; });
    const auto row = static_cast<double>(found - fold_rows_.begin());
    return (row - static_cast<double>(top_row_index())) * mapping_.row_height;
}

std::array<double, 128> PianoRollView::row_table() const {
    std::array<double, 128> table{};
    rebuild_fold_rows();
    if (fold_rows_.empty()) {
        for (int pitch = 0; pitch < 128; ++pitch)
            table[static_cast<std::size_t>(pitch)] = mapping_.pitch_to_y(pitch);
        return table;
    }
    const auto top = static_cast<double>(top_row_index());
    std::size_t row = fold_rows_.size(); // pitches below the lowest row sit past the last row
    for (int pitch = 0; pitch < 128; ++pitch) {
        // Descending rows: the first row <= pitch (as row_y finds it), walking up from the bottom.
        while (row > 0 && fold_rows_[row - 1] <= pitch)
            --row;
        table[static_cast<std::size_t>(pitch)] = (static_cast<double>(row) - top) * mapping_.row_height;
    }
    return table;
}

int PianoRollView::pitch_at_y(double y) const {
    rebuild_fold_rows();
    if (fold_rows_.empty())
        return mapping_.y_to_pitch(y);
    const auto offset = static_cast<std::int64_t>(std::floor(y / mapping_.row_height));
    const auto index = std::clamp<std::int64_t>(static_cast<std::int64_t>(top_row_index()) + offset, 0,
                                                static_cast<std::int64_t>(fold_rows_.size()) - 1);
    return fold_rows_[static_cast<std::size_t>(index)];
}

std::vector<int> PianoRollView::visible_rows(double height) const {
    rebuild_fold_rows();
    std::vector<int> rows;
    if (fold_rows_.empty()) {
        const int bottom = mapping_.y_to_pitch(height);
        for (int pitch = std::min(127, mapping_.top_pitch); pitch >= std::max(0, bottom); --pitch)
            rows.push_back(pitch);
        return rows;
    }
    const auto visible = static_cast<std::size_t>(std::ceil(height / mapping_.row_height)) + 1;
    for (std::size_t index = top_row_index(); index < fold_rows_.size() && rows.size() < visible; ++index)
        rows.push_back(fold_rows_[index]);
    return rows;
}

int PianoRollView::step_rows(int pitch, int rows) const {
    rebuild_fold_rows();
    if (fold_rows_.empty())
        return std::clamp(pitch + rows, 0, 127);
    const auto found = std::find_if(fold_rows_.begin(), fold_rows_.end(), [pitch](int row) { return row <= pitch; });
    const auto index = std::clamp<std::int64_t>(static_cast<std::int64_t>(found - fold_rows_.begin()) - rows, 0,
                                                static_cast<std::int64_t>(fold_rows_.size()) - 1);
    return fold_rows_[static_cast<std::size_t>(index)];
}

int PianoRollView::clamp_top(int pitch) const {
    rebuild_fold_rows();
    const int visible = visible_row_count();
    if (fold_rows_.empty())
        return std::clamp(pitch, std::min(127, visible - 1), 127);
    // Folded: the top row index may not pass the point where the last row fills the bottom.
    const auto found = std::find_if(fold_rows_.begin(), fold_rows_.end(), [pitch](int row) { return row <= pitch; });
    const auto last_top = std::max<std::int64_t>(0, static_cast<std::int64_t>(fold_rows_.size()) - visible);
    const auto index = std::clamp<std::int64_t>(found - fold_rows_.begin(), 0, last_top);
    return fold_rows_[static_cast<std::size_t>(index)];
}

int PianoRollView::row_count() const {
    rebuild_fold_rows();
    return fold_rows_.empty() ? 128 : static_cast<int>(fold_rows_.size());
}

int PianoRollView::top_row() const {
    rebuild_fold_rows();
    return fold_rows_.empty() ? 127 - std::clamp(mapping_.top_pitch, 0, 127) : static_cast<int>(top_row_index());
}

void PianoRollView::set_top_row(int row) {
    rebuild_fold_rows();
    if (fold_rows_.empty()) {
        set_top_pitch(127 - row);
        return;
    }
    const auto index = static_cast<std::size_t>(std::clamp(row, 0, static_cast<int>(fold_rows_.size()) - 1));
    set_top_pitch(fold_rows_[index]);
}

// --- Painting ------------------------------------------------------------------------------------

QString PianoRollView::empty_hint() const {
    // Built from the live bindings so a rebound shortcut never makes the hint wrong.
    const auto shortcut = [this](const char* id) {
        const auto* action = window() ? window()->findChild<QAction*>(QString::fromLatin1(id)) : nullptr;
        return action ? action->shortcut().toString(QKeySequence::NativeText) : QString();
    };
    QStringList first;
    QStringList second;
    if (const QString draw = shortcut("tool.draw"); !draw.isEmpty())
        first << tr("Press %1 (Draw) and click to add notes").arg(draw);
    else
        first << tr("Double-click to add notes");
    if (const QString play = shortcut("transport.play_stop"); !play.isEmpty())
        first << tr("%1 plays and stops").arg(play);
    if (const QString open = shortcut("file.open"); !open.isEmpty())
        second << tr("%1 opens a MIDI file").arg(open);
    second << tr("drop .mid files here");
    if (const QString palette = shortcut("view.command_palette"); !palette.isEmpty())
        second << tr("%1 finds any command").arg(palette);
    return first.join(QStringLiteral("  ·  ")) + '\n' + second.join(QStringLiteral("  ·  "));
}

void PianoRollView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const RollColors colors = roll_colors();
    painter.fillRect(rect(), colors.white_row);
    const QRectF area = note_area();
    painter.save();
    painter.setClipRect(area);
    painter.translate(area.topLeft());
    const QRectF local(0, 0, area.width(), area.height());
    paint_rows(painter, local);
    paint_grid(painter, local);
    paint_notes(painter, local);
    paint_transform_preview(painter, local);
    paint_drag_preview(painter, local);
    if (hasFocus() && controller_.selection().empty()) {
        // Keyboard entry position: the edit cursor on the entry pitch row.
        const double y = row_y(cursor_pitch_);
        QColor row = palette().color(QPalette::Highlight);
        row.setAlpha(40);
        painter.fillRect(QRectF(0, y, local.width(), mapping_.row_height), row);
    }
    if (!playhead_ && controller_.edit_cursor() >= mapping_.origin_tick) {
        // Stopped: the edit cursor is where playback starts, drawn as the red transport line
        // (Left/Right move it by the grid; a click in the ruler sets it).
        const double x = mapping_.tick_to_x(controller_.edit_cursor());
        painter.setPen(QPen(colors.playhead, 2));
        painter.drawLine(QPointF(x, 0), QPointF(x, local.height()));
    } else if (playhead_ && controller_.edit_cursor() >= mapping_.origin_tick) {
        const double x = mapping_.tick_to_x(controller_.edit_cursor());
        painter.setPen(QPen(colors.cursor, 1, Qt::DashLine)); // return point while playing
        painter.drawLine(QPointF(x, 0), QPointF(x, local.height()));
    }
    if (playhead_) {
        const double x = mapping_.tick_to_x(*playhead_);
        painter.setPen(QPen(colors.playhead, 2));
        painter.drawLine(QPointF(x, 0), QPointF(x, local.height()));
    }
    if (controller_.notes().notes.empty() && gesture_ == Gesture::none) {
        // Empty state: say how to start instead of showing a blank grid.
        painter.setPen(colors.hint_text);
        QFont hint_font = font();
        hint_font.setPointSizeF(hint_font.pointSizeF() + 1);
        painter.setFont(hint_font);
        // Wrapped inside the visible grid, so a narrow roll (library dock open) never clips it.
        painter.drawText(local.adjusted(12, 0, -12, 0), Qt::AlignCenter | Qt::TextWordWrap, empty_hint());
    }
    painter.restore();
    paint_keyboard(painter);
    paint_ruler(painter);
    if (hasFocus()) {
        painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(rect()).adjusted(1, 1, -1, -1));
    }
}

void PianoRollView::paint_rows(QPainter& painter, const QRectF& area) {
    const auto& sequence = controller_.active_sequence();
    const bool highlight = sequence.settings.highlight_scale;
    const RollColors colors = roll_colors();
    const QColor white = colors.white_row;
    const QColor black = colors.black_row;
    // With the scale on, rows read by scale membership instead of key color: scale rows are
    // light, rows outside the scale are dark, and the root rows carry the accent.
    const ScaleRowColors scale = scale_row_colors(colors);
    const QBrush hatch(scale.hatch, Qt::BDiagPattern);
    for (const int pitch : visible_rows(area.height())) {
        const QRectF row(0, row_y(pitch), area.width(), mapping_.row_height);
        if (highlight) {
            if (!in_scale(pitch)) {
                painter.fillRect(row, scale.out);
                painter.fillRect(row, hatch);
            } else {
                painter.fillRect(row, is_root(pitch) ? scale.root : scale.in);
            }
        } else {
            painter.fillRect(row, is_black_key(pitch) ? black : white);
        }
        if (pitch % 12 == 0) {
            painter.setPen(QPen(colors.c_line, 1));
            painter.drawLine(QPointF(0, row.bottom()), QPointF(area.width(), row.bottom()));
        }
    }
}

void PianoRollView::paint_grid(QPainter& painter, const QRectF& area) {
    const auto& meter = controller_.meter_map();
    const Tick first = mapping_.origin_tick;
    const Tick last = mapping_.x_to_tick(area.width());
    const RollColors colors = roll_colors();
    const QColor bar_color = colors.bar;
    const QColor beat_color = colors.beat;
    const QColor grid_color = colors.snap;
    // Snap grid (only when far enough apart to read; a quiet color so bars stand out).
    const Grid snap = grid();
    if (snap_mode_ != SnapMode::off && snap.step() / mapping_.ticks_per_pixel >= 8.0) {
        painter.setPen(QPen(grid_color, 1));
        for (Tick tick = snap.floor(first); tick <= last; tick = snap.next(tick)) {
            const double x = mapping_.tick_to_x(tick);
            painter.drawLine(QPointF(x, 0), QPointF(x, area.height()));
        }
    }
    // Beats and bars from the meter map (adaptive density).
    const auto position = meter.position(std::max<Tick>(0, first));
    for (std::int64_t bar = position.bar; bar < position.bar + 100000; ++bar) {
        const Tick start = meter.bar_start(bar);
        if (start > last)
            break;
        const auto& segment = meter.segment_at(start);
        const Tick beat = meter.ticks_per_beat(segment);
        const Tick bar_length = meter.ticks_per_bar(segment);
        if (beat > 0 && static_cast<double>(beat) / mapping_.ticks_per_pixel >= 8.0) {
            painter.setPen(QPen(beat_color, 1));
            for (Tick tick = start + beat; tick < start + bar_length; tick += beat) {
                const double x = mapping_.tick_to_x(tick);
                painter.drawLine(QPointF(x, 0), QPointF(x, area.height()));
            }
        }
        if (static_cast<double>(bar_length) / mapping_.ticks_per_pixel >= 3.0 || bar % 4 == 0) {
            painter.setPen(bar % 4 == 0 ? QPen(colors.bar_strong, 1.5) : QPen(bar_color, 1));
            const double x = mapping_.tick_to_x(start);
            painter.drawLine(QPointF(x, 0), QPointF(x, area.height()));
        }
    }
}

QColor PianoRollView::note_color(const NoteSpan& note) const {
    switch (color_mode_) {
    case NoteColorMode::channel: return channel_color(note.channel);
    case NoteColorMode::velocity: {
        // Perceptual ramp blue -> purple -> rose.
        const double t = std::clamp(note.velocity / 127.0, 0.0, 1.0);
        const QColor low(0x3B, 0x82, 0xF6), mid(0xA8, 0x55, 0xF7), high(0xF4, 0x3F, 0x5E);
        const auto mix = [](const QColor& a, const QColor& b, double f) {
            return QColor::fromRgbF(static_cast<float>(a.redF() + (b.redF() - a.redF()) * f),
                                    static_cast<float>(a.greenF() + (b.greenF() - a.greenF()) * f),
                                    static_cast<float>(a.blueF() + (b.blueF() - a.blueF()) * f));
        };
        return t < 0.5 ? mix(low, mid, t * 2) : mix(mid, high, (t - 0.5) * 2);
    }
    case NoteColorMode::scale_degree: {
        const int degree = ((note.pitch % 12) - controller_.active_sequence().root_pc + 12) % 12;
        return QColor::fromHsv(degree * 30, 140, 230);
    }
    case NoteColorMode::track: {
        const auto& sequence = controller_.active_sequence();
        if (note.on_track < sequence.tracks.size()) {
            const std::uint32_t rgba = sequence.tracks[note.on_track].color_rgba;
            return QColor(static_cast<int>((rgba >> 24) & 0xFF), static_cast<int>((rgba >> 16) & 0xFF),
                          static_cast<int>((rgba >> 8) & 0xFF));
        }
        return QColor(79, 143, 232);
    }
    }
    return QColor(79, 143, 232);
}

void PianoRollView::paint_notes(QPainter& painter, const QRectF& area) {
    const auto& index = controller_.note_index();
    const Tick first = mapping_.origin_tick;
    const Tick last = mapping_.x_to_tick(area.width()) + 1;
    const int low = std::clamp(pitch_at_y(area.height()), 0, 127);
    const int high = std::clamp(mapping_.top_pitch, 0, 127);
    const auto visible = index.query(TimeRange{first, last}, static_cast<std::uint8_t>(low),
                                     static_cast<std::uint8_t>(high));
    // Level of detail by pixel density, not only by count: many notes per column are summarized.
    if (visible.size() > density_threshold ||
        static_cast<double>(visible.size()) > std::max(1.0, area.width()) * density_per_column) {
        paint_density(painter, area, visible);
        return;
    }
    const auto& selection = controller_.selection();
    const RollColors colors = roll_colors();
    const bool highlight_scale = controller_.active_sequence().settings.highlight_scale;
    const bool hide_originals = gesture_ == Gesture::move || gesture_ == Gesture::resize_end ||
                                gesture_ == Gesture::resize_start;
    const bool erasing = gesture_ == Gesture::erase && !erase_hits_.empty();
    const bool simple = visible.size() > simple_note_threshold;
    const auto ys = row_table();
    // Labels only where they are readable (about 7 pt and up).
    const bool labels = !simple && mapping_.row_height >= 11;
    QFont label_font = font();
    label_font.setPointSizeF(std::max(7.0, mapping_.row_height * 0.6));
    painter.setFont(label_font);
    const QColor danger = danger_color();
    for (const auto position : visible) {
        const auto& note = index.notes()[position];
        const bool editable = note_editable(note);
        if (!editable && !controller_.show_ghost_tracks())
            continue;
        const bool selected = selection.contains(note.on_id);
        const double left = mapping_.tick_to_x(note.start);
        const double right = std::max(left + 2.0, mapping_.tick_to_x(note.end));
        const QRectF body(left, ys[note.pitch] + 1, right - left, mapping_.row_height - 2);
        QColor fill = color_mode_ == NoteColorMode::velocity ? note_color(note) : note_fill(note_color(note), note.velocity);
        if (selected)
            fill = fill.lighter(135);
        if (!editable)
            fill.setAlpha(70); // read-only ghost
        if (selected && hide_originals)
            fill.setAlpha(60);
        if (note.flags & note_flag::muted)
            fill = fill.lighter(150);
        const bool erase_target = erasing && erase_hits_.contains(note.on_id);
        if (erase_target)
            fill.setAlpha(60); // will be removed on release
        if (simple || body.width() < 4) {
            // Narrow or numerous notes: one plain rectangle each (no rounding, outline or label).
            painter.fillRect(body, fill);
            if (selected || erase_target) {
                painter.setPen(QPen(erase_target ? danger : colors.selected_outline, 1));
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(body);
            }
            continue;
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(body, 2, 2);
        if (note.flags & note_flag::muted) {
            painter.setBrush(QBrush(palette().color(QPalette::Mid), Qt::BDiagPattern));
            painter.drawRect(body);
        }
        if (sounding_.contains(note.pitch) && playhead_ && *playhead_ >= note.start && *playhead_ < note.end) {
            painter.setBrush(QColor(255, 255, 255, 160)); // currently sounding: a bright top strip
            painter.drawRect(QRectF(body.left(), body.top(), body.width(), std::min(2.0, body.height())));
        }
        // Selected notes read in grayscale too: lighter fill, strong outline, inner contrast line.
        QPen outline(selected ? colors.selected_outline : fill.darker(140), selected ? (colors.high_contrast ? 3 : 2) : 1);
        if (selected && body.width() > 6 && body.height() > 6) {
            painter.setPen(QPen(colors.selected_inner, 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(body.adjusted(2, 2, -2, -2), 1, 1);
        }
        if (highlight_scale && !in_scale(note.pitch)) {
            // Out of scale: a warning outline plus a solid bar at the note's start, so it reads
            // without color too; a selected note keeps its selection outline and the bar.
            const QColor warning = warning_color();
            if (!selected)
                outline = QPen(warning, 2);
            painter.setPen(Qt::NoPen);
            painter.setBrush(warning);
            painter.drawRect(QRectF(body.left(), body.top(), std::min(4.0, body.width()), body.height()));
        }
        if (note.flags & (note_flag::unterminated | note_flag::zero_length))
            outline = QPen(danger, 2, Qt::DotLine); // invalid/orphan-derived
        if (erase_target)
            outline = QPen(danger, 2);
        painter.setPen(outline);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(body, 2, 2);
        if (labels && body.width() > 28) {
            // Label color by relative luminance, so labels keep contrast on light and dark fills.
            const double luminance = 0.2126 * fill.redF() + 0.7152 * fill.greenF() + 0.0722 * fill.blueF();
            painter.setPen(luminance > 0.45 ? Qt::black : Qt::white);
            painter.drawText(body.adjusted(3, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft,
                             QString::fromStdString(pitch_name(note.pitch)));
        }
    }
    if (gesture_ == Gesture::rubber_band) {
        const QRectF band = QRectF(to_note_area(press_point_), to_note_area(current_point_)).normalized();
        painter.setPen(QPen(palette().color(QPalette::Highlight), 1, Qt::DashLine));
        QColor band_fill = palette().color(QPalette::Highlight);
        band_fill.setAlpha(40);
        painter.setBrush(band_fill);
        painter.drawRect(band);
    }
}

void PianoRollView::paint_density(QPainter& painter, const QRectF& area, const std::vector<std::size_t>& visible) {
    // Occupancy summary (section 7.4): one pixel per (column, visible row) accumulated with a
    // difference array per row, then drawn as a single scaled image.
    const int columns = std::max(1, static_cast<int>(area.width()));
    const auto rows = visible_rows(area.height());
    if (rows.empty())
        return;
    std::array<int, 128> row_of{};
    row_of.fill(-1);
    for (std::size_t row = 0; row < rows.size(); ++row)
        row_of[static_cast<std::size_t>(rows[row])] = static_cast<int>(row);
    const auto stride = static_cast<std::size_t>(columns) + 1;
    std::vector<std::int32_t> difference(stride * rows.size(), 0);
    const auto& notes = controller_.note_index().notes();
    for (const auto position : visible) {
        const auto& note = notes[position];
        const int row = row_of[note.pitch];
        if (row < 0)
            continue;
        const int x0 = std::clamp(static_cast<int>(mapping_.tick_to_x(note.start)), 0, columns - 1);
        const int x1 = std::clamp(static_cast<int>(mapping_.tick_to_x(note.end)), x0, columns - 1);
        auto* line = difference.data() + static_cast<std::size_t>(row) * stride;
        ++line[x0];
        --line[x1 + 1];
    }
    QImage image(columns, static_cast<int>(rows.size()), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    const QColor base = palette().color(QPalette::Highlight);
    for (std::size_t row = 0; row < rows.size(); ++row) {
        auto* pixels = reinterpret_cast<QRgb*>(image.scanLine(static_cast<int>(row)));
        const auto* line = difference.data() + row * stride;
        std::int32_t count = 0;
        for (int x = 0; x < columns; ++x) {
            count += line[x];
            if (count <= 0)
                continue;
            const int alpha = std::min(255, 60 + count * 20);
            pixels[x] = qPremultiply(qRgba(base.red(), base.green(), base.blue(), alpha));
        }
    }
    const double top = row_y(rows.front());
    painter.drawImage(QRectF(0, top, columns, static_cast<double>(rows.size()) * mapping_.row_height), image);
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(area.adjusted(6, 4, -6, -4), Qt::AlignTop | Qt::AlignRight,
                     tr("Density view: %1 notes — zoom in to edit").arg(visible.size()));
}

void PianoRollView::paint_transform_preview(QPainter& painter, const QRectF& area) {
    if (preview_notes_.empty() || preview_generation_ != controller_.generation())
        return;
    // Result of the pending transform: outlined ghosts, so the change is judged before Apply.
    const Tick first = mapping_.origin_tick;
    const Tick last = mapping_.x_to_tick(area.width()) + 1;
    const auto ys = row_table();
    QColor outline = palette().color(QPalette::Highlight);
    painter.setPen(QPen(outline, 1.5, Qt::DashLine));
    outline.setAlpha(50);
    painter.setBrush(outline);
    for (const auto& note : preview_notes_) {
        if (note.end < first || note.start > last)
            continue;
        const double left = mapping_.tick_to_x(note.start);
        const double right = std::max(left + 2, mapping_.tick_to_x(note.end));
        const double y = ys[note.pitch];
        if (y < -mapping_.row_height || y > area.height())
            continue;
        painter.drawRect(QRectF(left, y + 1, right - left, mapping_.row_height - 2));
    }
}

void PianoRollView::paint_drag_preview(QPainter& painter, const QRectF& area) {
    if (gesture_ != Gesture::move && gesture_ != Gesture::duplicate_move && gesture_ != Gesture::resize_end &&
        gesture_ != Gesture::resize_start && gesture_ != Gesture::draw)
        return;
    QColor fill = palette().color(QPalette::Highlight);
    fill.setAlpha(170);
    painter.setPen(QPen(fill.darker(150), 1));
    painter.setBrush(fill);
    const auto ys = row_table();
    if (gesture_ == Gesture::draw && handle_note_) {
        const auto& note = *handle_note_;
        const double left = mapping_.tick_to_x(note.start);
        const double right = std::max(left + 2, mapping_.tick_to_x(note.end));
        painter.drawRect(QRectF(left, ys[note.pitch] + 1, right - left, mapping_.row_height - 2));
        return;
    }
    // Only displaced notes inside the window are drawn; large selections become plain rectangles.
    const Tick first = mapping_.origin_tick;
    const Tick last = mapping_.x_to_tick(area.width()) + 1;
    const bool simple = snapshot_.notes.size() > simple_note_threshold;
    if (simple)
        painter.setPen(Qt::NoPen);
    for (const auto& note : snapshot_.notes) {
        Tick start = note.start;
        Tick end = note.end;
        int pitch = note.pitch;
        if (gesture_ == Gesture::move || gesture_ == Gesture::duplicate_move) {
            start += preview_move_.delta_ticks;
            end += preview_move_.delta_ticks;
            pitch += preview_move_.delta_pitch;
        } else if (gesture_ == Gesture::resize_end) {
            end += preview_resize_delta_;
        } else {
            start += preview_resize_delta_;
        }
        if (end < first || start > last || pitch < 0 || pitch > 127)
            continue;
        const double y = ys[static_cast<std::size_t>(pitch)];
        if (y < -mapping_.row_height || y > area.height())
            continue;
        const double left = mapping_.tick_to_x(start);
        const double right = std::max(left + 2, mapping_.tick_to_x(end));
        if (simple)
            painter.fillRect(QRectF(left, y + 1, right - left, mapping_.row_height - 2), fill);
        else
            painter.drawRect(QRectF(left, y + 1, right - left, mapping_.row_height - 2));
    }
}

void PianoRollView::paint_keyboard(QPainter& painter) {
    // A label column with the octave names (C3, C4...) and a piano: every row is a white key, and
    // black-key rows carry a solid black bar over the left part, as on a real keyboard.
    const QRectF gutter(0, ruler_height, keyboard_width, height() - ruler_height);
    painter.save();
    painter.setClipRect(gutter);
    painter.fillRect(gutter, roll_colors().ruler_background);
    painter.translate(0, ruler_height);
    const auto& sequence = controller_.active_sequence();
    const RollColors colors = roll_colors();
    const double key_left = keyboard_label_width;
    const double key_width = keyboard_width - 1 - key_left;
    const double black_width = std::round(key_width * 0.62);
    const QColor sounding = palette().color(QPalette::Highlight);
    QFont label_font = font();
    label_font.setPointSizeF(std::clamp(mapping_.row_height * 0.55, 7.0, font().pointSizeF()));
    painter.setFont(label_font);
    for (const int pitch : visible_rows(gutter.height())) {
        const double y = row_y(pitch);
        const QRectF key(key_left, y, key_width, mapping_.row_height);
        const bool black = is_black_key(pitch);
        const bool lit = sounding_.contains(pitch) || (audition_pitch_ && *audition_pitch_ == pitch);
        painter.fillRect(key, lit && !black ? sounding : colors.key_white);
        if (black)
            painter.fillRect(QRectF(key_left, y, black_width, mapping_.row_height), lit ? sounding : colors.key_black);
        // Key edges: a line under every row, drawn only on the white part so black bars stay solid.
        painter.setPen(colors.high_contrast ? colors.key_black : colors.bar);
        const double edge_left = black ? key_left + black_width : key_left;
        painter.drawLine(QPointF(edge_left, y + mapping_.row_height - 0.5), QPointF(key.right(), y + mapping_.row_height - 0.5));
        if (sequence.settings.highlight_scale) {
            const ScaleRowColors scale = scale_row_colors(colors);
            if (!in_scale(pitch)) {
                // Keys outside the scale are grayed out, so the scale's keys stand out as a pattern.
                QColor shade = colors.ruler_background;
                shade.setAlpha(colors.high_contrast ? 210 : 185);
                painter.fillRect(key, shade);
            } else {
                // Scale keys: a green strip matching their rows (stronger on the root) and the
                // note name, so the scale can be read off the keyboard.
                const double strip = std::round(key_width * 0.42);
                const QRectF tag(key.right() - strip, y, strip, mapping_.row_height - 1);
                painter.fillRect(tag, is_root(pitch) ? scale.root : scale.in);
                if (mapping_.row_height >= 9) {
                    painter.setPen(colors.dark || colors.high_contrast ? QColor(Qt::white) : QColor("#10301E"));
                    QFont name_font = painter.font();
                    name_font.setBold(is_root(pitch));
                    painter.setFont(name_font);
                    painter.drawText(tag, Qt::AlignCenter,
                                     QString::fromStdString(pitch_class_name(pitch % 12)));
                    name_font.setBold(false);
                    painter.setFont(name_font);
                }
            }
        }
        if (pitch % 12 == 0 && mapping_.row_height >= 7) {
            painter.setPen(colors.ruler_text);
            painter.drawText(QRectF(0, y, key_left - 4, mapping_.row_height), Qt::AlignVCenter | Qt::AlignRight,
                             QString::fromStdString(pitch_name(static_cast<std::uint8_t>(pitch))));
        }
    }
    // Right edge of the keyboard against the grid.
    painter.setPen(colors.bar_strong);
    painter.drawLine(QPointF(keyboard_width - 0.5, 0), QPointF(keyboard_width - 0.5, gutter.height()));
    painter.restore();
}

std::optional<TimeRange> PianoRollView::loop_range() const {
    const auto& sequence = controller_.active_sequence();
    if (!sequence.settings.loop_region)
        return std::nullopt;
    for (const auto& region : sequence.regions)
        if (region.id == *sequence.settings.loop_region)
            return TimeRange{region.start_tick, region.end_tick};
    return std::nullopt;
}

void PianoRollView::paint_ruler(QPainter& painter) {
    const QRectF ruler(keyboard_width, 0, width() - keyboard_width, ruler_height);
    const RollColors colors = roll_colors();
    painter.fillRect(QRectF(0, 0, width(), ruler_height), colors.ruler_background);
    painter.setPen(colors.c_line);
    painter.drawLine(QPointF(0, ruler_height - 0.5), QPointF(width(), ruler_height - 0.5));
    painter.save();
    painter.setClipRect(ruler);
    painter.translate(keyboard_width, 0);
    const auto& sequence = controller_.active_sequence();
    const bool loop_gesture = gesture_ == Gesture::loop_create || gesture_ == Gesture::loop_move ||
                              gesture_ == Gesture::loop_move_start || gesture_ == Gesture::loop_move_end;
    // Loop bracket and named regions.
    for (const auto& region : sequence.regions) {
        const bool loop = sequence.settings.loop_region && *sequence.settings.loop_region == region.id;
        if (loop && loop_gesture)
            continue; // drawn from the gesture preview below
        const double left = mapping_.tick_to_x(region.start_tick);
        const double right = mapping_.tick_to_x(region.end_tick);
        QColor color = loop ? colors.loop : QColor(160, 120, 220, 90);
        if (loop)
            color.setAlpha(170);
        painter.fillRect(QRectF(left, loop ? 0 : ruler_height - 6, right - left, 6), color);
        if (!loop && right - left > 40) {
            painter.setPen(colors.hint_text);
            painter.drawText(QRectF(left + 2, 0, right - left, ruler_height - 6), Qt::AlignLeft | Qt::AlignVCenter,
                             QString::fromStdString(region.name));
        }
    }
    if (loop_gesture && past_threshold_ && loop_preview_.end > loop_preview_.begin) {
        QColor color = colors.loop;
        color.setAlpha(220);
        const double left = mapping_.tick_to_x(loop_preview_.begin);
        painter.fillRect(QRectF(left, 0, mapping_.tick_to_x(loop_preview_.end) - left, 6), color);
    }
    const auto& meter = controller_.meter_map();
    const Tick last = mapping_.x_to_tick(ruler.width());
    const auto first = meter.position(std::max<Tick>(0, mapping_.origin_tick));
    painter.setPen(colors.ruler_text);
    double last_label_x = -1000;
    for (std::int64_t bar = first.bar; bar < first.bar + 100000; ++bar) {
        const Tick start = meter.bar_start(bar);
        if (start > last)
            break;
        const double x = mapping_.tick_to_x(start);
        painter.drawLine(QPointF(x, ruler_height - 10), QPointF(x, ruler_height));
        if (x - last_label_x >= 28) {
            painter.drawText(QPointF(x + 4, ruler_height - 9), QString::number(bar + 1)); // one-based bars
            last_label_x = x;
        }
    }
    {
        // Transport marker in the ruler: the playhead while playing, else the edit cursor.
        const double x = mapping_.tick_to_x(playhead_ ? *playhead_ : controller_.edit_cursor());
        QPolygonF marker;
        marker << QPointF(x - 5, 0) << QPointF(x + 5, 0) << QPointF(x, 8);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colors.playhead);
        painter.drawPolygon(marker);
    }
    painter.restore();
}

// --- Mouse contract (section 7.2) ----------------------------------------------------------------

bool PianoRollView::event(QEvent* event) {
    // Losing the mouse grab (a popup, a modal dialog) ends the gesture without committing it.
    if (event->type() == QEvent::UngrabMouse && gesture_ != Gesture::none)
        cancel_gesture();
    return QWidget::event(event);
}

void PianoRollView::mousePressEvent(QMouseEvent* event) {
    if (gesture_ != Gesture::none) {
        // A second button never restarts or corrupts a running gesture.
        event->accept();
        return;
    }
    setFocus(Qt::MouseFocusReason);
    press_point_ = event->position();
    current_point_ = press_point_;
    press_modifiers_ = event->modifiers();
    gesture_modifiers_ = press_modifiers_;
    gesture_button_ = event->button();
    past_threshold_ = false;
    shift_axis_locked_ = false;
    selection_at_press_ = controller_.selection();
    edit_cursor_at_press_ = controller_.edit_cursor();
    if (event->button() == Qt::MiddleButton) {
        gesture_ = Gesture::pan;
        pan_origin_ = mapping_;
        suspend_follow();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    const QPointF point = event->position();
    if (point.y() < ruler_height && point.x() >= keyboard_width) {
        const Tick tick = std::max<Tick>(0, mapping_.x_to_tick(point.x() - keyboard_width));
        if (point.y() < loop_strip_height && controller_.active_sequence().division.is_ppq()) {
            // Loop strip: drag to create the loop, or drag its edges or body (applied on release).
            gesture_ = Gesture::loop_create;
            const auto loop = loop_range();
            loop_at_press_ = loop.value_or(TimeRange{});
            loop_preview_ = loop_at_press_;
            if (loop) {
                const double left = keyboard_width + mapping_.tick_to_x(loop->begin);
                const double right = keyboard_width + mapping_.tick_to_x(loop->end);
                if (std::abs(point.x() - left) <= 5)
                    gesture_ = Gesture::loop_move_start;
                else if (std::abs(point.x() - right) <= 5)
                    gesture_ = Gesture::loop_move_end;
                else if (point.x() > left && point.x() < right)
                    gesture_ = Gesture::loop_move;
            }
            update();
            return;
        }
        controller_.set_edit_cursor(snap_nearest(tick, event->modifiers()));
        gesture_ = Gesture::scrub;
        return;
    }
    if (point.x() < keyboard_width && point.y() >= ruler_height) {
        const int pitch = std::clamp(pitch_at_y(point.y() - ruler_height), 0, 127);
        audition_pitch_ = pitch;
        emit audition_note_on(pitch, default_velocity());
        update();
        return;
    }
    const QPointF local = to_note_area(point);
    const Tick tick = std::max<Tick>(0, mapping_.x_to_tick(local.x()));
    const int pitch = std::clamp(pitch_at_y(local.y()), 0, 127);
    auto hit = hit_test(point);
    const auto& index = controller_.note_index();
    if (hit.note && !note_editable(index.notes()[*hit.note]) && tool_ != EditTool::erase && tool_ != EditTool::split)
        hit = {}; // other tracks' notes are read-only: a press there acts on empty space
    EditTool tool = tool_;
    if (tool == EditTool::draw && hit.note)
        tool = EditTool::select; // clicking a note in Draw selects, moves or resizes it
    switch (tool) {
    case EditTool::select: {
        if (hit.note) {
            const auto& note = index.notes()[*hit.note];
            handle_note_ = note;
            const bool control = (event->modifiers() & Qt::ControlModifier) != 0;
            const bool alt = (event->modifiers() & Qt::AltModifier) != 0;
            const bool shift = (event->modifiers() & Qt::ShiftModifier) != 0;
            // Ctrl+Alt on a body is the explicit end-resize for notes too narrow for a handle.
            HitZone zone = hit.zone;
            if (control && alt && zone == HitZone::body)
                zone = HitZone::right_edge;
            const bool edge = zone == HitZone::left_edge || zone == HitZone::right_edge;
            if (!controller_.selection().contains(note.on_id)) {
                if (shift || (edge && control)) {
                    // Shift extends; an edge press always resizes the grabbed note too.
                    auto selection = controller_.selection();
                    selection.insert(note.on_id);
                    controller_.set_selection(std::move(selection));
                } else if (!control) {
                    controller_.set_selection({note.on_id});
                }
            }
            gesture_ = Gesture::pending_select;
            if (zone == HitZone::right_edge)
                gesture_ = Gesture::resize_end;
            else if (zone == HitZone::left_edge)
                gesture_ = Gesture::resize_start;
            if (gesture_ != Gesture::pending_select)
                begin_drag_from_selection();
        } else {
            gesture_ = Gesture::rubber_band;
            if (!(event->modifiers() & Qt::ControlModifier))
                controller_.set_selection({});
        }
        break;
    }
    case EditTool::draw: {
        if (!controller_.active_track_index())
            return;
        const Tick start = snap_floor(tick, event->modifiers());
        NoteSpan preview;
        preview.start = start;
        preview.end = start + default_note_length();
        preview.pitch = static_cast<std::uint8_t>(pitch);
        preview.velocity = default_velocity();
        handle_note_ = preview;
        gesture_ = Gesture::draw;
        audition_pitch_ = pitch;
        emit audition_note_on(pitch, default_velocity());
        break;
    }
    case EditTool::erase: {
        gesture_ = Gesture::erase;
        erase_hits_.clear();
        last_erase_point_ = point;
        collect_erase_hits(point, point);
        break;
    }
    case EditTool::split: {
        if (!hit.note)
            return;
        const auto& note = index.notes()[*hit.note];
        if (!note_editable(note))
            return;
        const Tick at = snap_nearest(tick, event->modifiers());
        if (at <= note.start || at >= note.end) {
            emit status_message(tr("Split point must lie inside the note"));
            return;
        }
        auto split = split_note(controller_.document(), controller_.active_sequence().id, note.on_id, at);
        if (!split) {
            emit status_message(QString::fromStdString(split.error().message));
            return;
        }
        controller_.apply(split->changes);
        break;
    }
    case EditTool::audition: {
        audition_pitch_ = pitch;
        emit audition_note_on(pitch, default_velocity());
        break;
    }
    }
    update();
}

void PianoRollView::begin_drag_from_selection() {
    snapshot_ = DragSnapshot::capture(controller_.selected_notes());
    preview_move_ = {};
    preview_resize_delta_ = 0;
    reported_move_ = {0, 0};
}

void PianoRollView::collect_erase_hits(QPointF from, QPointF to) {
    // Every editable note the stroke segment crosses, not only those under sampled positions.
    const QPointF a = to_note_area(from);
    const QPointF b = to_note_area(to);
    const Tick first = mapping_.x_to_tick(std::min(a.x(), b.x())) - 1;
    const Tick last = mapping_.x_to_tick(std::max(a.x(), b.x())) + 1;
    const int high = std::clamp(pitch_at_y(std::min(a.y(), b.y())), 0, 127);
    const int low = std::clamp(pitch_at_y(std::max(a.y(), b.y())), 0, 127);
    const auto& index = controller_.note_index();
    for (const auto position : index.query(TimeRange{first, last + 1}, static_cast<std::uint8_t>(std::min(low, high)),
                                           static_cast<std::uint8_t>(std::max(low, high)))) {
        const auto& note = index.notes()[position];
        if (!note_editable(note) || erase_hits_.contains(note.on_id))
            continue;
        const double left = mapping_.tick_to_x(note.start);
        const double right = std::max(left + 2.0, mapping_.tick_to_x(note.end));
        const QRectF body(left - 1, row_y(note.pitch), right - left + 2, mapping_.row_height);
        if (segment_hits_rect(a, b, body))
            erase_hits_.insert(note.on_id);
    }
}

void PianoRollView::mouseMoveEvent(QMouseEvent* event) {
    const QPointF point = event->position();
    if (point.x() >= keyboard_width && point.y() >= ruler_height) {
        const QPointF local = to_note_area(point);
        emit hover_changed(mapping_.x_to_tick(local.x()), pitch_at_y(local.y()));
    }
    if (gesture_ == Gesture::none) {
        if (point.y() < loop_strip_height && point.x() >= keyboard_width) {
            setCursor(Qt::SizeHorCursor); // the loop strip
            return;
        }
        auto hit = hit_test(point);
        if (hit.note && !note_editable(controller_.note_index().notes()[*hit.note]))
            hit = {}; // read-only ghosts look like empty space
        const bool edge = hit.zone == HitZone::left_edge || hit.zone == HitZone::right_edge;
        switch (tool_) {
        case EditTool::select: setCursor(edge ? Qt::SizeHorCursor : hit.note ? Qt::SizeAllCursor : Qt::ArrowCursor); break;
        case EditTool::draw: // Draw on a note moves or resizes it (see mousePressEvent)
            setCursor(edge ? Qt::SizeHorCursor : hit.note ? Qt::SizeAllCursor : Qt::CrossCursor);
            break;
        case EditTool::erase: setCursor(hit.note ? Qt::PointingHandCursor : Qt::ForbiddenCursor); break;
        case EditTool::split: setCursor(hit.note ? Qt::SplitHCursor : Qt::ArrowCursor); break;
        case EditTool::audition: setCursor(Qt::PointingHandCursor); break;
        }
        return;
    }
    // The button that started the gesture is no longer down (its release went elsewhere): the
    // gesture is abandoned, never left running on button-less moves.
    if (event->spontaneous() && gesture_button_ != Qt::NoButton && !(event->buttons() & gesture_button_)) {
        cancel_gesture();
        return;
    }
    current_point_ = point;
    gesture_modifiers_ = event->modifiers();
    const QPointF delta = current_point_ - press_point_;
    if (!past_threshold_ && delta.manhattanLength() >= QApplication::startDragDistance())
        past_threshold_ = true; // stays true for the rest of the gesture
    if (gesture_ == Gesture::pending_select && past_threshold_) {
        gesture_ = (press_modifiers_ & Qt::ControlModifier) ? Gesture::duplicate_move : Gesture::move;
        if (gesture_ == Gesture::duplicate_move && handle_note_ && !controller_.selection().contains(handle_note_->on_id)) {
            auto selection = controller_.selection();
            selection.insert(handle_note_->on_id);
            controller_.set_selection(std::move(selection));
        }
        begin_drag_from_selection();
    }
    if (gesture_ == Gesture::erase) {
        collect_erase_hits(last_erase_point_, point);
        last_erase_point_ = point;
    }
    update_gesture_preview();
    // Auto-scroll near the viewport edges, only for drags that place something.
    const bool scrolls = past_threshold_ &&
                         (gesture_ == Gesture::move || gesture_ == Gesture::duplicate_move ||
                          gesture_ == Gesture::resize_end || gesture_ == Gesture::resize_start ||
                          gesture_ == Gesture::draw || gesture_ == Gesture::rubber_band);
    const QRectF area = note_area();
    const bool near_edge = point.x() < area.left() + 16 || point.x() > area.right() - 16 ||
                           point.y() < area.top() + 12 || point.y() > area.bottom() - 12;
    if (scrolls && near_edge)
        auto_scroll_timer_.start();
    else
        auto_scroll_timer_.stop();
    update();
}

void PianoRollView::update_gesture_preview() {
    const QPointF delta = current_point_ - press_point_;
    const Qt::KeyboardModifiers modifiers = gesture_modifiers_;
    const Tick raw_ticks = static_cast<Tick>(std::llround(delta.x() * mapping_.ticks_per_pixel));
    switch (gesture_) {
    case Gesture::pan: {
        mapping_ = pan_origin_;
        mapping_.origin_tick =
            std::max<Tick>(0, pan_origin_.origin_tick - static_cast<Tick>(delta.x() * mapping_.ticks_per_pixel));
        mapping_.top_pitch = clamp_top(step_rows(pan_origin_.top_pitch, static_cast<int>(delta.y() / mapping_.row_height)));
        emit mapping_changed();
        break;
    }
    case Gesture::scrub: {
        const Tick tick = std::max<Tick>(0, mapping_.x_to_tick(current_point_.x() - keyboard_width));
        controller_.set_edit_cursor(snap_nearest(tick, modifiers));
        break;
    }
    case Gesture::loop_create:
    case Gesture::loop_move:
    case Gesture::loop_move_start:
    case Gesture::loop_move_end: {
        const Tick press_tick = std::max<Tick>(0, mapping_.x_to_tick(press_point_.x() - keyboard_width));
        const Tick tick = std::max<Tick>(0, mapping_.x_to_tick(current_point_.x() - keyboard_width));
        const Tick snapped = snap_nearest(tick, modifiers);
        TimeRange range = loop_at_press_;
        if (gesture_ == Gesture::loop_create) {
            const Tick anchor = snap_nearest(press_tick, modifiers);
            range = TimeRange{std::min(anchor, snapped), std::max(anchor, snapped)};
        } else if (gesture_ == Gesture::loop_move_start) {
            range.begin = std::min(snapped, loop_at_press_.end - 1);
        } else if (gesture_ == Gesture::loop_move_end) {
            range.end = std::max(snapped, loop_at_press_.begin + 1);
        } else {
            const Tick begin = snap_nearest(std::max<Tick>(0, loop_at_press_.begin + (tick - press_tick)), modifiers);
            range = TimeRange{begin, begin + (loop_at_press_.end - loop_at_press_.begin)};
        }
        loop_preview_ = range;
        break;
    }
    case Gesture::move:
    case Gesture::duplicate_move: {
        // Pitch displacement from the rows under the pointer (folded rows skip hidden pitches).
        Tick ticks = raw_ticks;
        int pitch = pitch_at_y(current_point_.y() - ruler_height) - pitch_at_y(press_point_.y() - ruler_height);
        SnapMode snap = effective_snap(modifiers);
        if (modifiers & Qt::ShiftModifier) {
            // Lock to the first dominant axis after the threshold (section 7.2).
            if (!shift_axis_locked_ && past_threshold_) {
                shift_axis_locked_ = true;
                lock_to_time_axis_ = std::abs(delta.x()) >= std::abs(delta.y());
            }
            if (shift_axis_locked_) {
                if (lock_to_time_axis_) {
                    pitch = 0;
                } else {
                    ticks = 0;
                    snap = SnapMode::off; // a pitch-only move never shifts off-grid notes in time
                }
            }
        }
        const auto previous_pitch = preview_move_.delta_pitch;
        preview_move_ = compute_move(snapshot_, ticks, pitch, grid(), snap);
        if (handle_note_ && preview_move_.delta_pitch != previous_pitch) {
            release_audition();
            audition_pitch_ = handle_note_->pitch + preview_move_.delta_pitch;
            emit audition_note_on(*audition_pitch_, handle_note_->velocity);
        }
        // Report only when the preview changes, not on every repaint.
        const std::pair<Tick, int> move{preview_move_.delta_ticks, preview_move_.delta_pitch};
        if (move != reported_move_) {
            reported_move_ = move;
            emit status_message(tr("%1 %2 note(s) by %3 ticks, %4 semitones%5")
                                    .arg(gesture_ == Gesture::duplicate_move ? tr("Duplicate") : tr("Move"))
                                    .arg(snapshot_.notes.size())
                                    .arg(move.first)
                                    .arg(move.second)
                                    .arg(preview_move_.constrained ? tr(" (limited at the edge of the range)") : QString()));
        }
        break;
    }
    case Gesture::resize_end:
        if (handle_note_)
            preview_resize_delta_ = compute_end_delta(snapshot_, *handle_note_, raw_ticks, grid(), effective_snap(modifiers));
        break;
    case Gesture::resize_start:
        if (handle_note_)
            preview_resize_delta_ = compute_start_delta(snapshot_, *handle_note_, raw_ticks, grid(), effective_snap(modifiers));
        break;
    case Gesture::draw:
        if (handle_note_ && past_threshold_) {
            // Dragging sets the duration (never below one grid step while snapping); crossing
            // pitches previews but never sprays notes. A click keeps the default length.
            const Tick tick = mapping_.x_to_tick(to_note_area(current_point_).x());
            if (effective_snap(modifiers) == SnapMode::off)
                handle_note_->end = std::max(handle_note_->start + 1, tick);
            else
                handle_note_->end = std::max(grid().next(tick), grid().next(handle_note_->start));
        }
        break;
    case Gesture::erase:
    case Gesture::rubber_band:
    case Gesture::pending_select:
    case Gesture::none: break;
    }
}

void PianoRollView::auto_scroll_step() {
    if (gesture_ == Gesture::none) {
        auto_scroll_timer_.stop();
        return;
    }
    const QRectF area = note_area();
    double dx = 0;
    double dy = 0;
    if (current_point_.x() < area.left() + 16)
        dx = -(area.left() + 16 - current_point_.x());
    else if (current_point_.x() > area.right() - 16)
        dx = current_point_.x() - (area.right() - 16);
    if (current_point_.y() < area.top() + 12)
        dy = -(area.top() + 12 - current_point_.y());
    else if (current_point_.y() > area.bottom() - 12)
        dy = current_point_.y() - (area.bottom() - 12);
    // Accelerates smoothly with the distance past the edge.
    const Tick shift = static_cast<Tick>(dx * std::min(4.0, 0.2 + std::abs(dx) / 20.0) * mapping_.ticks_per_pixel);
    const Tick old_origin = mapping_.origin_tick;
    mapping_.origin_tick = std::max<Tick>(0, mapping_.origin_tick + shift);
    press_point_.rx() -= static_cast<double>(mapping_.origin_tick - old_origin) / mapping_.ticks_per_pixel;
    if (dy != 0) {
        const int old_top = mapping_.top_pitch;
        mapping_.top_pitch = clamp_top(step_rows(mapping_.top_pitch, dy < 0 ? 1 : -1));
        if (mapping_.top_pitch != old_top) // exactly one row, chromatic or folded
            press_point_.ry() += (dy < 0 ? 1 : -1) * mapping_.row_height;
    }
    suspend_follow();
    // The dragged notes follow the pointer while the view scrolls under it.
    update_gesture_preview();
    emit mapping_changed();
    update();
}

void PianoRollView::mouseReleaseEvent(QMouseEvent* event) {
    if (gesture_ == Gesture::none) {
        release_audition(); // keyboard gutter and audition tool
        update();
        return;
    }
    if (event->button() != gesture_button_)
        return; // another button's release never ends this gesture
    auto_scroll_timer_.stop();
    gesture_modifiers_ = event->modifiers();
    if (gesture_ == Gesture::pan) {
        gesture_ = Gesture::none;
        gesture_button_ = Qt::NoButton;
        setCursor(Qt::ArrowCursor);
        return;
    }
    commit_gesture();
}

void PianoRollView::commit_gesture() {
    const Gesture gesture = gesture_;
    gesture_ = Gesture::none;
    gesture_button_ = Qt::NoButton;
    release_audition();
    const auto& sequence = controller_.active_sequence();
    const auto& index = controller_.note_index();
    switch (gesture) {
    case Gesture::pending_select:
        // A click without drag: Ctrl toggles membership, Shift extends, a plain click keeps only that note.
        if (handle_note_) {
            auto selection = selection_at_press_;
            if (press_modifiers_ & Qt::ControlModifier) {
                if (selection.contains(handle_note_->on_id))
                    selection.erase(handle_note_->on_id);
                else
                    selection.insert(handle_note_->on_id);
            } else if (press_modifiers_ & Qt::ShiftModifier) {
                selection.insert(handle_note_->on_id);
            } else {
                selection = {handle_note_->on_id};
            }
            controller_.set_selection(std::move(selection));
        }
        break;
    case Gesture::rubber_band: {
        const QRectF band = QRectF(to_note_area(press_point_), to_note_area(current_point_)).normalized();
        const Tick first = mapping_.x_to_tick(band.left());
        const Tick last = mapping_.x_to_tick(band.right()) + 1;
        const int high = std::clamp(pitch_at_y(band.top()), 0, 127);
        const int low = std::clamp(pitch_at_y(band.bottom()), 0, 127);
        auto selection = (press_modifiers_ & Qt::ControlModifier) ? selection_at_press_ : std::set<EventId>{};
        for (const auto position : index.query(TimeRange{first, last}, static_cast<std::uint8_t>(low),
                                               static_cast<std::uint8_t>(high))) {
            const auto& note = index.notes()[position];
            if (!note_editable(note))
                continue;
            if (press_modifiers_ & Qt::ControlModifier && selection_at_press_.contains(note.on_id))
                selection.erase(note.on_id); // Ctrl toggles
            else
                selection.insert(note.on_id);
        }
        controller_.set_selection(std::move(selection));
        break;
    }
    case Gesture::move: {
        if (preview_move_.delta_ticks == 0 && preview_move_.delta_pitch == 0)
            break;
        std::vector<NoteUpdate> updates;
        for (const auto& note : snapshot_.notes)
            updates.push_back(NoteUpdate{note.on_id, note.start + preview_move_.delta_ticks,
                                         note.end + preview_move_.delta_ticks,
                                         static_cast<std::uint8_t>(note.pitch + preview_move_.delta_pitch),
                                         note.velocity, note.channel, note.release_velocity});
        auto changes = update_notes(controller_.document(), sequence.id, updates,
                                    updates.size() == 1 ? "Move note" : "Move notes");
        if (changes)
            controller_.apply(*changes);
        else
            emit status_message(QString::fromStdString(changes.error().message));
        break;
    }
    case Gesture::duplicate_move: {
        if (preview_move_.delta_ticks == 0 && preview_move_.delta_pitch == 0) {
            // Exact copies on top of the originals would be invisible and double the sound.
            controller_.set_selection(selection_at_press_);
            emit status_message(tr("Duplicate cancelled: the copy was not moved"));
            break;
        }
        std::vector<EventId> ids;
        for (const auto& note : snapshot_.notes)
            ids.push_back(note.on_id);
        auto duplicated = duplicate_notes(controller_.document(), sequence.id, ids, preview_move_.delta_ticks,
                                          preview_move_.delta_pitch, controller_.active_track(), "Duplicate notes");
        if (duplicated && controller_.apply(duplicated->changes))
            controller_.set_selection(std::set<EventId>(duplicated->created.begin(), duplicated->created.end()));
        else if (!duplicated)
            emit status_message(QString::fromStdString(duplicated.error().message));
        break;
    }
    case Gesture::resize_end:
    case Gesture::resize_start: {
        if (preview_resize_delta_ == 0)
            break;
        std::vector<NoteUpdate> updates;
        for (const auto& note : snapshot_.notes) {
            NoteUpdate update{note.on_id, note.start, note.end, note.pitch, note.velocity, note.channel,
                              note.release_velocity};
            if (gesture == Gesture::resize_end)
                update.end += preview_resize_delta_;
            else
                update.start += preview_resize_delta_;
            updates.push_back(update);
        }
        auto changes = update_notes(controller_.document(), sequence.id, updates,
                                    gesture == Gesture::resize_end ? "Resize note ends" : "Resize note starts");
        if (changes)
            controller_.apply(*changes);
        else
            emit status_message(QString::fromStdString(changes.error().message));
        break;
    }
    case Gesture::draw: {
        if (!handle_note_ || !controller_.active_track_index())
            break;
        const auto& note = *handle_note_;
        const NewNote created{controller_.active_track(), note.start, std::max(note.start + 1, note.end), note.pitch,
                              note.velocity, default_channel_for(controller_), 0};
        auto changes = insert_notes(controller_.document(), std::span(&created, 1), "Draw note");
        if (!changes) {
            emit status_message(QString::fromStdString(changes.error().message));
            break;
        }
        // The drawn note becomes the selection, so Delete, arrows and the velocity lane act on it.
        const auto ids = inserted_note_starts(*changes);
        if (controller_.apply(*changes))
            controller_.set_selection(std::set<EventId>(ids.begin(), ids.end()));
        break;
    }
    case Gesture::erase: {
        if (erase_hits_.empty())
            break;
        std::vector<EventId> ids(erase_hits_.begin(), erase_hits_.end());
        erase_hits_.clear();
        auto changes = delete_notes(controller_.document(), sequence.id, ids, "Erase notes");
        if (!changes) {
            emit status_message(QString::fromStdString(changes.error().message));
            break;
        }
        controller_.apply(*changes);
        auto selection = controller_.selection();
        for (const auto& id : ids)
            selection.erase(id);
        controller_.set_selection(std::move(selection));
        break;
    }
    case Gesture::loop_create:
    case Gesture::loop_move:
    case Gesture::loop_move_start:
    case Gesture::loop_move_end:
        if (!past_threshold_) {
            // A click in the loop strip moves the edit cursor like the rest of the ruler.
            const Tick tick = std::max<Tick>(0, mapping_.x_to_tick(press_point_.x() - keyboard_width));
            controller_.set_edit_cursor(snap_nearest(tick, press_modifiers_));
            emit seek_requested(controller_.edit_cursor());
        } else if (loop_preview_.end > loop_preview_.begin && (loop_preview_.begin != loop_at_press_.begin ||
                                                                 loop_preview_.end != loop_at_press_.end)) {
            if (set_loop_range(controller_, loop_preview_))
                emit status_message(tr("Loop set to %1 - %2")
                                        .arg(QString::fromStdString(controller_.meter_map().format(loop_preview_.begin)))
                                        .arg(QString::fromStdString(controller_.meter_map().format(loop_preview_.end))));
        }
        break;
    case Gesture::scrub:
        emit seek_requested(controller_.edit_cursor()); // playback follows a ruler click (ux-editing-8)
        break;
    case Gesture::none:
    case Gesture::pan: break;
    }
    handle_note_.reset();
    snapshot_ = {};
    preview_move_ = {};
    preview_resize_delta_ = 0;
    update();
}

void PianoRollView::cancel_gesture() {
    // Escape restores the exact state at gesture start; nothing reaches the undo history.
    if (gesture_ == Gesture::none)
        return;
    const Gesture gesture = gesture_;
    gesture_ = Gesture::none; // first: the selection and cursor restores below repaint
    gesture_button_ = Qt::NoButton;
    switch (gesture) {
    case Gesture::rubber_band:
    case Gesture::pending_select:
    case Gesture::move:
    case Gesture::duplicate_move:
    case Gesture::resize_end:
    case Gesture::resize_start:
        if (controller_.selection() != selection_at_press_)
            controller_.set_selection(selection_at_press_);
        break;
    case Gesture::scrub: controller_.set_edit_cursor(edit_cursor_at_press_); break;
    case Gesture::pan:
        mapping_ = pan_origin_;
        emit mapping_changed();
        setCursor(Qt::ArrowCursor);
        break;
    case Gesture::draw:
    case Gesture::erase:
    case Gesture::loop_create:
    case Gesture::loop_move:
    case Gesture::loop_move_start:
    case Gesture::loop_move_end:
    case Gesture::none: break;
    }
    auto_scroll_timer_.stop();
    release_audition();
    handle_note_.reset();
    snapshot_ = {};
    preview_move_ = {};
    preview_resize_delta_ = 0;
    erase_hits_.clear();
    update();
}

void PianoRollView::release_audition() {
    if (audition_pitch_) {
        emit audition_note_off(*audition_pitch_);
        audition_pitch_.reset();
    }
}

void PianoRollView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (tool_ != EditTool::select || event->button() != Qt::LeftButton)
        return;
    const QPointF point = event->position();
    if (!note_area().contains(point) || !controller_.active_track_index())
        return;
    if (const auto hit = hit_test(point); hit.note) {
        const auto& note = controller_.note_index().notes()[*hit.note];
        if (!note_editable(note)) {
            // Double-click on another track's note switches editing to that track.
            const auto& tracks = controller_.active_sequence().tracks;
            if (note.on_track < tracks.size()) {
                controller_.set_active_track(tracks[note.on_track].id);
                controller_.set_selection({note.on_id});
                emit status_message(tr("Now editing %1").arg(QString::fromStdString(tracks[note.on_track].name)));
            }
        }
        return;
    }
    gesture_ = Gesture::none;
    const QPointF local = to_note_area(point);
    const Tick tick = std::max<Tick>(0, mapping_.x_to_tick(local.x()));
    const Tick start = snap_floor(tick, event->modifiers());
    const int pitch = std::clamp(pitch_at_y(local.y()), 0, 127);
    const NewNote created{controller_.active_track(), start, start + default_note_length(),
                          static_cast<std::uint8_t>(pitch), default_velocity(), default_channel_for(controller_), 0};
    auto changes = insert_notes(controller_.document(), std::span(&created, 1), "Insert note");
    if (!changes) {
        emit status_message(QString::fromStdString(changes.error().message));
        return;
    }
    const auto ids = inserted_note_starts(*changes);
    if (controller_.apply(*changes))
        controller_.set_selection(std::set<EventId>(ids.begin(), ids.end()));
}

void PianoRollView::push_zoom_history(bool from_wheel) {
    // A wheel or pinch burst is one history step: wheel zooms within 500 ms coalesce.
    if (from_wheel && last_wheel_zoom_.isValid() && last_wheel_zoom_.elapsed() < 500) {
        last_wheel_zoom_.restart();
        return;
    }
    if (from_wheel)
        last_wheel_zoom_.restart();
    else
        last_wheel_zoom_.invalidate();
    zoom_history_.push_back(mapping_);
    if (zoom_history_.size() > zoom_history_limit)
        zoom_history_.erase(zoom_history_.begin());
}

void PianoRollView::wheelEvent(QWheelEvent* event) {
    const double steps = event->angleDelta().y() / 120.0;
    const QPointF local = to_note_area(event->position());
    if ((event->modifiers() & Qt::AltModifier) && !(event->modifiers() & Qt::ControlModifier)) {
        // Alt-wheel: taller or shorter piano rows around the row under the mouse. Qt reports
        // Alt-wheel as horizontal motion on Windows, so read whichever axis moved.
        const double alt_steps = steps != 0 ? steps : event->angleDelta().x() / 120.0;
        if (alt_steps != 0) {
            push_zoom_history(true);
            mapping_.zoom_pitch_around(std::clamp(local.y(), 0.0, note_area().height()), std::pow(1.15, alt_steps));
            mapping_.top_pitch = clamp_top(mapping_.top_pitch);
        }
    } else if (event->modifiers() & Qt::ControlModifier) {
        if (steps == 0) {
            event->accept();
            return;
        }
        push_zoom_history(true);
        if (event->modifiers() & Qt::ShiftModifier)
            mapping_.zoom_pitch_around(local.y(), std::pow(1.15, steps)); // Ctrl-Shift-wheel: pitch zoom
        else
            mapping_.zoom_time_around(std::max(0.0, local.x()), std::pow(1.25, steps)); // Ctrl-wheel: time zoom
    } else {
        // Shift turns vertical wheel motion into time; touchpads also send horizontal motion.
        // Fractions accumulate so small precision-touchpad steps still move the view.
        const bool shift = (event->modifiers() & Qt::ShiftModifier) != 0;
        const double horizontal = shift ? (steps != 0 ? steps : event->angleDelta().x() / 120.0)
                                        : event->angleDelta().x() / 120.0;
        const double vertical = shift ? 0.0 : steps;
        wheel_columns_ += horizontal * 80;
        const auto columns = static_cast<Tick>(wheel_columns_);
        wheel_columns_ -= static_cast<double>(columns);
        if (columns != 0)
            mapping_.origin_tick = std::max<Tick>(0, mapping_.origin_tick - static_cast<Tick>(static_cast<double>(columns) *
                                                                                              mapping_.ticks_per_pixel));
        wheel_rows_ += vertical * 3;
        const auto rows = static_cast<int>(wheel_rows_);
        wheel_rows_ -= rows;
        if (rows != 0)
            mapping_.top_pitch = clamp_top(step_rows(mapping_.top_pitch, rows));
        if (columns != 0 || rows != 0)
            suspend_follow();
    }
    emit mapping_changed();
    update();
    event->accept();
}

void PianoRollView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        if (gesture_ != Gesture::none)
            cancel_gesture();
        else if (!controller_.selection().empty())
            controller_.set_selection({}); // Escape clears the selection when nothing is in progress
        event->accept();
        return;
    }
    const auto modifiers = event->modifiers() & ~Qt::KeypadModifier;
    // Keyboard note entry (spec 29.3): Left/Right move the red transport line (window command),
    // Up/Down choose the entry pitch when nothing is selected (Shift: an octave), and Return or
    // Insert adds a note there and advances.
    if (controller_.selection().empty() && (modifiers & ~Qt::ShiftModifier) == Qt::NoModifier &&
        (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
        const bool up = event->key() == Qt::Key_Up;
        if (modifiers & Qt::ShiftModifier) {
            // An octave; with folded rows the nearest visible row at or below it.
            cursor_pitch_ = std::clamp(cursor_pitch_ + (up ? 12 : -12), 0, 127);
            cursor_pitch_ = step_rows(cursor_pitch_, 0);
        } else {
            cursor_pitch_ = step_rows(cursor_pitch_, up ? 1 : -1); // never lands on a hidden row
        }
        const double y = row_y(cursor_pitch_);
        if (y < 0 || y + mapping_.row_height > note_area().height()) {
            mapping_.top_pitch = clamp_top(step_rows(cursor_pitch_, visible_row_count() / 2));
            emit mapping_changed();
        }
        emit hover_changed(controller_.edit_cursor(), cursor_pitch_);
        update();
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Insert) &&
        modifiers == Qt::NoModifier && controller_.active_track_index() &&
        controller_.selection().empty()) { // step entry only without a selection
        const Tick start = controller_.edit_cursor();
        const NewNote created{controller_.active_track(), start, start + default_note_length(),
                              static_cast<std::uint8_t>(cursor_pitch_), default_velocity(),
                              default_channel_for(controller_), 0};
        auto changes = insert_notes(controller_.document(), std::span(&created, 1), "Insert note");
        if (!changes) {
            emit status_message(QString::fromStdString(changes.error().message));
        } else if (controller_.apply(*changes)) {
            controller_.set_edit_cursor(start + default_note_length()); // step entry: advance the cursor
            scroll_to_tick(controller_.edit_cursor());                  // and keep it on screen
            emit audition_note_on(cursor_pitch_, default_velocity());
            QTimer::singleShot(150, this, [this, pitch = cursor_pitch_] { emit audition_note_off(pitch); }); // audible
        }
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PianoRollView::focusOutEvent(QFocusEvent* event) {
    // Focus loss (Alt+Tab, a dialog) ends any gesture without committing it and releases
    // scrub/audition notes (section 7.3).
    cancel_gesture();
    release_audition();
    QWidget::focusOutEvent(event);
}

void PianoRollView::leaveEvent(QEvent* event) {
    emit hover_left();
    QWidget::leaveEvent(event);
}

void PianoRollView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    emit mapping_changed();
}

void PianoRollView::contextMenuEvent(QContextMenuEvent* event) {
    // A right press during a drag (Windows sends the menu on release) never opens a menu mid-gesture.
    if (gesture_ != Gesture::none) {
        event->accept();
        return;
    }
    // Right-click does not change an existing multi-note selection unnecessarily (section 7.2).
    const auto hit = hit_test(event->pos());
    if (hit.note) {
        const auto& note = controller_.note_index().notes()[*hit.note];
        if (note_editable(note) && !controller_.selection().contains(note.on_id))
            controller_.set_selection({note.on_id});
    }
    QMenu menu(this);
    // The window's commands, so shortcuts, enabled state and undo labels match the menus.
    bool any_command = false;
    for (const char* id : std::initializer_list<const char*>{"edit.cut", "edit.copy", "edit.paste", "edit.duplicate", "edit.delete", nullptr,
                           "edit.quantize_last", "edit.transpose_up", "edit.transpose_down", "edit.octave_up",
                           "edit.octave_down", nullptr}) {
        if (!id) {
            if (any_command)
                menu.addSeparator();
            continue;
        }
        if (auto* action = window()->findChild<QAction*>(QString::fromLatin1(id))) {
            menu.addAction(action);
            any_command = true;
        }
    }
    menu.addAction(tr("Select All"), this, &PianoRollView::select_all);
    menu.addAction(tr("Invert Selection"), this, &PianoRollView::invert_selection);
    menu.addAction(tr("Select Same Pitch"), this, &PianoRollView::select_same_pitch);
    menu.addAction(tr("Select Same Pitch Class"), this, &PianoRollView::select_same_pitch_class);
    menu.addAction(tr("Select Same Channel"), this, &PianoRollView::select_same_channel);
    menu.addAction(tr("Select Same Duration"), this, &PianoRollView::select_same_duration);
    menu.addAction(tr("Select Out-of-Scale Notes"), this, &PianoRollView::select_out_of_scale);
    menu.addSeparator();
    auto* loop = menu.addAction(tr("Loop Selection"), this, &PianoRollView::loop_selection);
    loop->setEnabled(!controller_.selection().empty());
    menu.addAction(tr("Fit Selection"), this, &PianoRollView::fit_selection_or_all);
    auto* ghosts = menu.addAction(tr("Show Other Tracks"));
    ghosts->setCheckable(true);
    ghosts->setChecked(controller_.show_ghost_tracks());
    connect(ghosts, &QAction::toggled, this, [this](bool on) { controller_.set_show_ghost_tracks(on); });
    menu.exec(event->globalPos());
}

// --- View and selection commands -----------------------------------------------------------------

void PianoRollView::fit_all() {
    push_zoom_history(false);
    const auto& notes = controller_.notes().notes;
    const Tick end = std::max<Tick>(controller_.active_sequence().effective_end(), 1);
    mapping_.origin_tick = 0;
    mapping_.ticks_per_pixel = std::max(0.05, static_cast<double>(end) / std::max(50.0, note_area().width() - 20));
    // Show every note: center the pitch range, shrinking rows (down to 8 px) when it does not fit.
    const double height = std::max(40.0, note_area().height());
    int low = 60;
    int high = 60;
    if (!notes.empty()) {
        low = 127;
        high = 0;
        for (const auto& note : notes) {
            low = std::min<int>(low, note.pitch);
            high = std::max<int>(high, note.pitch);
        }
        // Rows between 10 and 20 px: as tall as fits, so a short phrase is easy to edit.
        const int span = high - low + 7;
        mapping_.row_height = std::clamp(std::floor(height / span), 10.0, 20.0);
    } else {
        mapping_.row_height = 16.0;
    }
    const int rows = static_cast<int>(height / mapping_.row_height);
    mapping_.top_pitch = std::clamp((low + high) / 2 + rows / 2, std::min(127, rows - 1), 127);
    emit mapping_changed();
    update();
}

void PianoRollView::fit_selection_or_all() {
    const auto selected = controller_.selected_notes();
    if (selected.empty()) {
        fit_all();
        return;
    }
    push_zoom_history(false);
    const auto snapshot = DragSnapshot::capture(selected);
    const Tick span = std::max<Tick>(1, snapshot.latest_end - snapshot.earliest_start);
    mapping_.ticks_per_pixel = std::max(0.05, static_cast<double>(span) / std::max(50.0, note_area().width() - 40));
    mapping_.origin_tick = std::max<Tick>(0, snapshot.earliest_start - static_cast<Tick>(20 * mapping_.ticks_per_pixel));
    const int rows = static_cast<int>(note_area().height() / mapping_.row_height);
    mapping_.top_pitch = std::clamp((snapshot.lowest_pitch + snapshot.highest_pitch) / 2 + rows / 2, 0, 127);
    emit mapping_changed();
    update();
}

void PianoRollView::return_to_previous_zoom() {
    if (zoom_history_.empty())
        return;
    mapping_ = zoom_history_.back();
    zoom_history_.pop_back();
    last_wheel_zoom_.invalidate();
    emit mapping_changed();
    update();
}

void PianoRollView::zoom_time(double factor) {
    push_zoom_history(false);
    mapping_.zoom_time_around(note_area().width() / 2, factor);
    emit mapping_changed();
    update();
}

void PianoRollView::scroll_to_tick(Tick tick) {
    const Tick visible = mapping_.x_to_tick(note_area().width()) - mapping_.origin_tick;
    if (tick < mapping_.origin_tick || tick > mapping_.origin_tick + visible) {
        mapping_.origin_tick = std::max<Tick>(0, tick - visible / 4);
        emit mapping_changed();
        update();
    }
}

void PianoRollView::set_origin(Tick tick) {
    const Tick origin = std::max<Tick>(0, tick);
    if (origin == mapping_.origin_tick)
        return;
    mapping_.origin_tick = origin;
    suspend_follow(); // the scroll bar is a manual scroll
    emit mapping_changed(); // lanes convert x to ticks with this mapping
    update();
}

void PianoRollView::set_top_pitch(int pitch) {
    const int top = std::clamp(pitch, 0, 127);
    if (top == mapping_.top_pitch)
        return;
    mapping_.top_pitch = top;
    emit mapping_changed();
    update();
}

void PianoRollView::select_all() {
    std::set<EventId> selection;
    for (const auto& note : controller_.notes().notes)
        if (note_editable(note))
            selection.insert(note.on_id);
    controller_.set_selection(std::move(selection));
}

void PianoRollView::invert_selection() {
    std::set<EventId> selection;
    for (const auto& note : controller_.notes().notes)
        if (note_editable(note) && !controller_.selection().contains(note.on_id))
            selection.insert(note.on_id);
    controller_.set_selection(std::move(selection));
}

namespace {

/// One pass over the notes with a constant-time membership test built from the selection once
/// (a select-all of 100k notes must not compare every note with every selected note).
template <std::size_t Size, class Key>
std::set<EventId> select_matching(const DocumentController& controller, Key key,
                                  const std::function<bool(const NoteSpan&)>& editable) {
    std::bitset<Size> wanted;
    for (const auto& note : controller.selected_notes())
        wanted.set(static_cast<std::size_t>(key(note)));
    std::set<EventId> selection;
    if (wanted.none())
        return selection;
    for (const auto& note : controller.notes().notes)
        if (editable(note) && wanted.test(static_cast<std::size_t>(key(note))))
            selection.insert(selection.end(), note.on_id);
    return selection;
}

} // namespace

void PianoRollView::select_same_pitch() {
    const auto editable = [this](const NoteSpan& note) { return note_editable(note); };
    controller_.set_selection(select_matching<128>(controller_, [](const NoteSpan& note) { return note.pitch; }, editable));
}

void PianoRollView::select_same_pitch_class() {
    const auto editable = [this](const NoteSpan& note) { return note_editable(note); };
    controller_.set_selection(
        select_matching<12>(controller_, [](const NoteSpan& note) { return note.pitch % 12; }, editable));
}

void PianoRollView::select_same_channel() {
    const auto editable = [this](const NoteSpan& note) { return note_editable(note); };
    controller_.set_selection(
        select_matching<16>(controller_, [](const NoteSpan& note) { return note.channel & 0x0F; }, editable));
}

void PianoRollView::select_adjacent_note(int direction) {
    std::vector<const NoteSpan*> notes;
    for (const auto& note : controller_.notes().notes)
        if (note_editable(note))
            notes.push_back(&note);
    if (notes.empty())
        return;
    const auto before = [](const NoteSpan* a, const NoteSpan* b) {
        return a->start != b->start ? a->start < b->start : a->pitch < b->pitch;
    };
    std::ranges::sort(notes, before);
    const NoteSpan* chosen = nullptr;
    const auto& selection = controller_.selection();
    const auto anchor = std::ranges::find_if(notes, [&](const NoteSpan* note) { return selection.contains(note->on_id); });
    if (anchor != notes.end()) {
        const auto index = static_cast<std::ptrdiff_t>(anchor - notes.begin()) + direction;
        if (index < 0 || index >= static_cast<std::ptrdiff_t>(notes.size()))
            return; // at the first or last note: the selection stays
        chosen = notes[static_cast<std::size_t>(index)];
    } else {
        // Nothing selected: the first note at or after the edit cursor (or the last before it).
        const Tick cursor = controller_.edit_cursor();
        if (direction > 0) {
            const auto found = std::ranges::find_if(notes, [&](const NoteSpan* note) { return note->start >= cursor; });
            chosen = found != notes.end() ? *found : notes.back();
        } else {
            const auto found = std::ranges::find_if(notes | std::views::reverse,
                                                    [&](const NoteSpan* note) { return note->start < cursor; });
            chosen = found != (notes | std::views::reverse).end() ? *found : notes.front();
        }
    }
    controller_.set_selection({chosen->on_id});
    scroll_to_tick(chosen->start);
    cursor_pitch_ = chosen->pitch;
    emit audition_note_on(chosen->pitch, chosen->velocity);
    QTimer::singleShot(150, this, [this, pitch = static_cast<int>(chosen->pitch)] { emit audition_note_off(pitch); });
    update();
}

void PianoRollView::change_length_by_grid(int direction) {
    const auto selected = controller_.selected_notes();
    if (selected.empty())
        return;
    const Tick step = std::max<Tick>(1, grid().point(1) - grid().point(0));
    std::vector<NoteUpdate> updates;
    for (const auto& note : selected) {
        if (!note_editable(note) || !note.off_id)
            continue;
        const Tick end = std::max(note.start + 1, note.end + direction * step);
        if (end == note.end)
            continue;
        updates.push_back(NoteUpdate{note.on_id, note.start, end, note.pitch, note.velocity, note.channel, note.release_velocity});
    }
    if (updates.empty())
        return;
    auto changes = update_notes(controller_.document(), controller_.active_sequence().id, updates,
                                direction > 0 ? "Lengthen notes" : "Shorten notes");
    if (changes)
        controller_.apply(*changes);
    else
        emit status_message(QString::fromStdString(changes.error().message));
}

void PianoRollView::select_same_duration() {
    std::set<Tick> durations;
    for (const auto& note : controller_.selected_notes())
        durations.insert(note.duration());
    std::set<EventId> selection;
    if (!durations.empty())
        for (const auto& note : controller_.notes().notes)
            if (note_editable(note) && durations.contains(note.duration()))
                selection.insert(selection.end(), note.on_id);
    controller_.set_selection(std::move(selection));
}

void PianoRollView::select_out_of_scale() {
    std::set<EventId> selection;
    for (const auto& note : controller_.notes().notes)
        if (note_editable(note) && !in_scale(note.pitch))
            selection.insert(note.on_id);
    controller_.set_selection(std::move(selection));
}

void PianoRollView::loop_selection() {
    const auto selected = controller_.selected_notes();
    if (selected.empty()) {
        emit status_message(tr("Select notes first: Loop Selection loops their grid-aligned extent"));
        return;
    }
    if (!controller_.active_sequence().division.is_ppq()) {
        emit status_message(tr("Loops in bars need a PPQ (musical) time base"));
        return;
    }
    const auto range = grid_aligned_extent(selected, grid());
    if (set_loop_range(controller_, range))
        emit status_message(tr("Loop set to %1 - %2")
                                .arg(QString::fromStdString(controller_.meter_map().format(range.begin)))
                                .arg(QString::fromStdString(controller_.meter_map().format(range.end))));
}

// --- Panel with windowed scroll bars -------------------------------------------------------------

PianoRollPanel::PianoRollPanel(DocumentController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    view_ = new PianoRollView(controller, this);
    horizontal_ = new QScrollBar(Qt::Horizontal, this);
    vertical_ = new QScrollBar(Qt::Vertical, this);
    horizontal_->setRange(0, scrollbar_resolution);
    vertical_->setRange(0, 127);
    horizontal_->setAccessibleName(tr("Timeline position"));
    vertical_->setAccessibleName(tr("Pitch position"));
    auto* grid = new QVBoxLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(0);
    auto* row = new QHBoxLayout;
    row->setSpacing(0);
    row->addWidget(view_, 1);
    row->addWidget(vertical_);
    grid->addLayout(row, 1);
    grid->addWidget(horizontal_);
    connect(view_, &PianoRollView::mapping_changed, this, &PianoRollPanel::sync_scrollbars);
    connect(&controller_, &DocumentController::document_changed, this, &PianoRollPanel::sync_scrollbars);
    connect(&controller_, &DocumentController::view_state_changed, this, &PianoRollPanel::sync_scrollbars);
    connect(horizontal_, &QScrollBar::valueChanged, this, [this](int value) {
        if (syncing_)
            return;
        // Windowed mapping: the bar covers the song plus the visible window in 64-bit ticks.
        const Tick total = std::max<Tick>(controller_.active_sequence().effective_end() * 5 / 4, 1);
        view_->set_origin(static_cast<Tick>(static_cast<__int128>(total) * value / scrollbar_resolution));
    });
    connect(vertical_, &QScrollBar::valueChanged, this, [this](int value) {
        if (syncing_)
            return;
        view_->set_top_row(value); // rows, so Fold to Scale scrolls by visible rows
    });
    sync_scrollbars();
}

void PianoRollPanel::sync_scrollbars() {
    syncing_ = true;
    const Tick total = std::max<Tick>(controller_.active_sequence().effective_end() * 5 / 4, 1);
    const auto& mapping = view_->mapping();
    // Page and step sizes follow the visible window, so the thumb shows how much is on screen and
    // the arrows and trough move by a visible amount.
    const int page = static_cast<int>(std::clamp<__int128>(
        static_cast<__int128>(view_->visible_ticks()) * scrollbar_resolution / total, 1, scrollbar_resolution));
    horizontal_->setRange(0, std::max(0, scrollbar_resolution - page));
    horizontal_->setPageStep(page);
    horizontal_->setSingleStep(std::max(1, page / 8));
    const int total_rows = view_->row_count();
    const int rows = std::clamp(view_->visible_row_count(), 1, total_rows);
    vertical_->setRange(0, std::max(0, total_rows - rows));
    vertical_->setPageStep(rows);
    vertical_->setSingleStep(1);
    horizontal_->setValue(static_cast<int>(static_cast<__int128>(mapping.origin_tick) * scrollbar_resolution / total));
    vertical_->setValue(view_->top_row());
    syncing_ = false;
}

} // namespace mwb::ui
