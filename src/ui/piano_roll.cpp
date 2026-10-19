#include "mwb/ui/piano_roll.hpp"

#include "mwb/ui/theme.hpp"

#include "mwb/core/pitch.hpp"
#include "mwb/music/scale.hpp"
#include "mwb/ui/document_controller.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>

namespace mwb::ui {

namespace {

constexpr double handle_zone = 6.0;          // DIP, section 7.2
constexpr std::size_t density_threshold = 30000;
constexpr int scrollbar_resolution = 100000;

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

} // namespace

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
    mapping_.row_height = 12.0;
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

void PianoRollView::set_snap_mode(SnapMode mode) { snap_mode_ = mode; }

void PianoRollView::set_grid_override(std::optional<Rational> fraction) {
    grid_override_ = fraction;
    update();
}

Tick PianoRollView::visible_ticks() const {
    return std::max<Tick>(1, mapping_.x_to_tick(note_area().width()) - mapping_.origin_tick);
}

int PianoRollView::visible_row_count() const {
    return std::max(1, static_cast<int>(note_area().height() / std::max(1.0, mapping_.row_height)));
}

std::uint8_t PianoRollView::default_channel() const {
    const auto active = controller_.active_track_index();
    if (!active)
        return 0;
    for (const auto& note : controller_.notes().notes)
        if (note.on_track == *active)
            return note.channel; // continue the track's own channel
    return controller_.active_sequence().tracks[*active].is_drum ? 9 : 0;
}

Grid PianoRollView::grid() const {
    const auto& sequence = controller_.active_sequence();
    const std::int32_t ppq = sequence.division.is_ppq() ? sequence.division.ppq : default_ppq;
    return Grid::make(grid_override_.value_or(sequence.settings.snap), ppq);
}

void PianoRollView::set_playhead(std::optional<Tick> tick) {
    if (playhead_ == tick)
        return;
    playhead_ = tick;
    update();
}

void PianoRollView::set_sounding_pitches(std::set<int> pitches) {
    sounding_ = std::move(pitches);
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

SnapMode PianoRollView::effective_snap(const QMouseEvent* event) const {
    if (event && (event->modifiers() & Qt::AltModifier))
        return SnapMode::off; // Alt bypasses time snapping during a drag (section 7.2)
    return snap_mode_;
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
    // Prefer editable notes and the most recently starting one under the pointer.
    std::optional<std::size_t> best;
    for (const auto position : candidates) {
        const auto& note = index.notes()[position];
        if (note.pitch != pitch)
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
    // If the note is too narrow for both zones, body selection wins (section 7.2).
    if (width_pixels >= 3 * handle_zone) {
        if (point.x() >= right - handle_zone)
            hit.zone = HitZone::right_edge;
        else if (point.x() <= left + handle_zone)
            hit.zone = HitZone::left_edge;
        else
            hit.zone = HitZone::body;
    } else {
        hit.zone = HitZone::body;
    }
    return hit;
}

// --- Rows (chromatic or folded) ------------------------------------------------------------------

void PianoRollView::rebuild_fold_rows() const {
    const auto& sequence = controller_.active_sequence();
    // Cached per document generation, sequence and scale: painting calls this for every row.
    const std::uint64_t key = (controller_.generation() << 20) ^ (controller_.active_sequence_index() << 16) ^
                              (static_cast<std::uint64_t>(sequence.scale_mask) << 4) ^ sequence.root_pc ^
                              (sequence.settings.fold_to_scale ? (1ull << 63) : 0);
    if (key == fold_key_ && fold_key_valid_)
        return;
    fold_key_ = key;
    fold_key_valid_ = true;
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

// --- Painting ------------------------------------------------------------------------------------

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
        painter.drawText(local, Qt::AlignCenter,
                         tr("Press 2 (Draw) and click to add notes  ·  Space plays\n"
                            "Ctrl+O opens a MIDI file  ·  drop .mid files here  ·  Ctrl+Shift+P finds any command"));
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
    const QColor root_tint = colors.root_tint;
    const QColor member_tint = colors.member_tint;
    for (const int pitch : visible_rows(area.height())) {
        const QRectF row(0, row_y(pitch), area.width(), mapping_.row_height);
        painter.fillRect(row, is_black_key(pitch) ? black : white);
        if (highlight && in_scale(pitch))
            painter.fillRect(row, is_root(pitch) ? root_tint : member_tint);
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
    if (snap.step() / mapping_.ticks_per_pixel >= 8.0) {
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
    if (visible.size() > density_threshold) {
        paint_density(painter, area, visible);
        return;
    }
    const auto& selection = controller_.selection();
    const RollColors colors = roll_colors();
    const bool highlight_scale = controller_.active_sequence().settings.highlight_scale;
    const bool hide_originals = gesture_ == Gesture::move || gesture_ == Gesture::resize_end ||
                                gesture_ == Gesture::resize_start;
    QFont label_font = font();
    label_font.setPointSizeF(std::max(6.0, mapping_.row_height * 0.55));
    painter.setFont(label_font);
    for (const auto position : visible) {
        const auto& note = index.notes()[position];
        const bool editable = note_editable(note);
        if (!editable && !controller_.show_ghost_tracks())
            continue;
        const bool selected = selection.contains(note.on_id);
        const double left = mapping_.tick_to_x(note.start);
        const double right = std::max(left + 2.0, mapping_.tick_to_x(note.end));
        const QRectF body(left, row_y(note.pitch) + 1, right - left, mapping_.row_height - 2);
        QColor fill = color_mode_ == NoteColorMode::velocity ? note_color(note) : note_fill(note_color(note), note.velocity);
        if (selected)
            fill = fill.lighter(135);
        if (!editable)
            fill.setAlpha(70); // read-only ghost
        if (selected && hide_originals)
            fill.setAlpha(60);
        if (note.flags & note_flag::muted)
            fill = fill.lighter(150);
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
            outline = QPen(QColor(230, 120, 30), 2, Qt::DashLine); // out of scale: shape, not only color
        }
        if (note.flags & (note_flag::unterminated | note_flag::zero_length))
            outline = QPen(QColor(210, 40, 40), 2, Qt::DotLine); // invalid/orphan-derived
        painter.setPen(outline);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(body, 2, 2);
        if (body.width() > 28 && mapping_.row_height >= 9) {
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
    // Occupancy summary: one column per pixel and pitch row, no per-note rectangles (section 7.4).
    const int columns = static_cast<int>(area.width());
    const int rows = 128;
    std::vector<std::uint16_t> occupancy(static_cast<std::size_t>(columns) * rows, 0);
    const auto& notes = controller_.note_index().notes();
    for (const auto position : visible) {
        const auto& note = notes[position];
        const int x0 = std::clamp(static_cast<int>(mapping_.tick_to_x(note.start)), 0, columns - 1);
        const int x1 = std::clamp(static_cast<int>(mapping_.tick_to_x(note.end)), x0, columns - 1);
        for (int x = x0; x <= x1; ++x) {
            auto& cell = occupancy[static_cast<std::size_t>(x) * rows + note.pitch];
            if (cell < 65535)
                ++cell;
        }
    }
    const QColor base = palette().color(QPalette::Highlight);
    for (int x = 0; x < columns; ++x) {
        for (int pitch = 0; pitch < rows; ++pitch) {
            const auto count = occupancy[static_cast<std::size_t>(x) * rows + static_cast<std::size_t>(pitch)];
            if (count == 0)
                continue;
            QColor color = base;
            color.setAlpha(std::min(255, 60 + count * 20));
            painter.fillRect(QRectF(x, row_y(pitch), 1, mapping_.row_height), color);
        }
    }
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(area.adjusted(6, 4, -6, -4), Qt::AlignTop | Qt::AlignRight,
                     tr("Density view: %1 notes — zoom in to edit").arg(visible.size()));
}

void PianoRollView::paint_drag_preview(QPainter& painter, const QRectF&) {
    if (gesture_ != Gesture::move && gesture_ != Gesture::duplicate_move && gesture_ != Gesture::resize_end &&
        gesture_ != Gesture::resize_start && gesture_ != Gesture::draw)
        return;
    QColor fill = palette().color(QPalette::Highlight);
    fill.setAlpha(170);
    painter.setPen(QPen(fill.darker(150), 1));
    painter.setBrush(fill);
    if (gesture_ == Gesture::draw && handle_note_) {
        const auto& note = *handle_note_;
        const double left = mapping_.tick_to_x(note.start);
        const double right = std::max(left + 2, mapping_.tick_to_x(note.end));
        painter.drawRect(QRectF(left, row_y(note.pitch) + 1, right - left, mapping_.row_height - 2));
        return;
    }
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
        const double left = mapping_.tick_to_x(start);
        const double right = std::max(left + 2, mapping_.tick_to_x(end));
        painter.drawRect(QRectF(left, row_y(pitch) + 1, right - left, mapping_.row_height - 2));
    }
    if (gesture_ == Gesture::move || gesture_ == Gesture::duplicate_move) {
        // Report only when the preview changes, not on every repaint.
        const std::pair<Tick, int> move{preview_move_.delta_ticks, preview_move_.delta_pitch};
        if (move != reported_move_) {
            reported_move_ = move;
            QMetaObject::invokeMethod(this, [this, count = snapshot_.notes.size(), move, constrained = preview_move_.constrained] {
                emit status_message(tr("Move %1 note(s) by %2 ticks, %3 semitones%4")
                                        .arg(count)
                                        .arg(move.first)
                                        .arg(move.second)
                                        .arg(constrained ? tr(" (limited at the edge of the range)") : QString()));
            }, Qt::QueuedConnection);
        }
    }
}

void PianoRollView::paint_keyboard(QPainter& painter) {
    const QRectF gutter(0, ruler_height, keyboard_width, height() - ruler_height);
    painter.save();
    painter.setClipRect(gutter);
    painter.translate(0, ruler_height);
    const auto& sequence = controller_.active_sequence();
    const RollColors colors = roll_colors();
    for (const int pitch : visible_rows(gutter.height())) {
        const QRectF key(0, row_y(pitch), keyboard_width - 1, mapping_.row_height);
        QColor color = is_black_key(pitch) ? colors.key_black : colors.key_white;
        if (sounding_.contains(pitch))
            color = palette().color(QPalette::Highlight);
        painter.fillRect(key, color);
        painter.setPen(colors.c_line);
        painter.drawLine(QPointF(key.left(), key.bottom() - 0.5), QPointF(key.right(), key.bottom() - 0.5)); // inside this key
        if (sequence.settings.highlight_scale && in_scale(pitch)) {
            // Shape marker in addition to color: filled square for the root, dot for members.
            painter.setBrush(palette().color(QPalette::Highlight));
            painter.setPen(Qt::NoPen);
            const double size = std::min(6.0, mapping_.row_height - 2);
            const QRectF marker(keyboard_width - size - 3, key.center().y() - size / 2, size, size);
            if (is_root(pitch))
                painter.drawRect(marker);
            else
                painter.drawEllipse(marker);
        }
        if (pitch % 12 == 0 && mapping_.row_height >= 8) {
            painter.setPen(colors.key_label);
            painter.drawText(key.adjusted(3, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft,
                             QString::fromStdString(pitch_name(static_cast<std::uint8_t>(pitch))));
        }
    }
    painter.restore();
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
    // Loop bracket and named regions.
    for (const auto& region : sequence.regions) {
        const double left = mapping_.tick_to_x(region.start_tick);
        const double right = mapping_.tick_to_x(region.end_tick);
        const bool loop = sequence.settings.loop_region && *sequence.settings.loop_region == region.id;
        QColor color = loop ? colors.loop : QColor(160, 120, 220, 90);
        if (loop)
            color.setAlpha(170);
        painter.fillRect(QRectF(left, loop ? 0 : ruler_height - 6, right - left, loop ? 6 : 6), color);
        if (!loop && right - left > 40) {
            painter.setPen(colors.hint_text);
            painter.drawText(QRectF(left + 2, 0, right - left, ruler_height - 6), Qt::AlignLeft | Qt::AlignVCenter,
                             QString::fromStdString(region.name));
        }
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

void PianoRollView::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    press_point_ = event->position();
    current_point_ = press_point_;
    press_modifiers_ = event->modifiers();
    shift_axis_locked_ = false;
    if (event->button() == Qt::MiddleButton) {
        gesture_ = Gesture::pan;
        pan_origin_ = mapping_;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    const QPointF point = event->position();
    if (point.y() < ruler_height && point.x() >= keyboard_width) {
        const Tick tick = mapping_.x_to_tick(point.x() - keyboard_width);
        controller_.set_edit_cursor(std::max<Tick>(0, effective_snap(event) == SnapMode::off ? tick : grid().nearest(tick)));
        gesture_ = Gesture::scrub;
        return;
    }
    if (point.x() < keyboard_width && point.y() >= ruler_height) {
        const int pitch = std::clamp(pitch_at_y(point.y() - ruler_height), 0, 127);
        audition_pitch_ = pitch;
        emit audition_note_on(pitch, default_velocity());
        return;
    }
    const QPointF local = to_note_area(point);
    const Tick tick = std::max<Tick>(0, mapping_.x_to_tick(local.x()));
    const int pitch = std::clamp(pitch_at_y(local.y()), 0, 127);
    const auto hit = hit_test(point);
    const auto& index = controller_.note_index();
    EditTool tool = tool_;
    if (tool == EditTool::draw && hit.note && note_editable(index.notes()[*hit.note]))
        tool = EditTool::select; // clicking a note in Draw selects, moves or resizes it
    switch (tool) {
    case EditTool::select: {
        selection_at_press_ = controller_.selection();
        if (hit.note) {
            const auto& note = index.notes()[*hit.note];
            if (!note_editable(note))
                return; // ghost tracks are read-only: no operation writes into them
            handle_note_ = note;
            if (!(event->modifiers() & Qt::ControlModifier) && !controller_.selection().contains(note.on_id))
                controller_.set_selection({note.on_id});
            gesture_ = Gesture::pending_select;
            if (hit.zone == HitZone::right_edge)
                gesture_ = Gesture::resize_end;
            else if (hit.zone == HitZone::left_edge)
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
        const Tick start = effective_snap(event) == SnapMode::off ? tick : grid().floor(tick);
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
        if (hit.note && note_editable(index.notes()[*hit.note]))
            erase_hits_.insert(index.notes()[*hit.note].on_id);
        break;
    }
    case EditTool::split: {
        if (!hit.note)
            return;
        const auto& note = index.notes()[*hit.note];
        if (!note_editable(note))
            return;
        const Tick at = effective_snap(event) == SnapMode::off ? tick : grid().nearest(tick);
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
}

void PianoRollView::mouseMoveEvent(QMouseEvent* event) {
    const QPointF point = event->position();
    if (point.x() >= keyboard_width && point.y() >= ruler_height) {
        const QPointF local = to_note_area(point);
        emit hover_changed(mapping_.x_to_tick(local.x()), pitch_at_y(local.y()));
    }
    if (gesture_ == Gesture::none) {
        const auto hit = hit_test(point);
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
    current_point_ = point;
    const QPointF delta = current_point_ - press_point_;
    const bool past_threshold = delta.manhattanLength() >= QApplication::startDragDistance();
    if (gesture_ == Gesture::pan) {
        mapping_ = pan_origin_;
        mapping_.origin_tick = std::max<Tick>(0, pan_origin_.origin_tick - static_cast<Tick>(delta.x() * mapping_.ticks_per_pixel));
        mapping_.top_pitch = std::clamp(pan_origin_.top_pitch + static_cast<int>(delta.y() / mapping_.row_height), 0, 127);
        emit mapping_changed();
        update();
        return;
    }
    if (gesture_ == Gesture::scrub) {
        const Tick tick = mapping_.x_to_tick(point.x() - keyboard_width);
        controller_.set_edit_cursor(std::max<Tick>(0, effective_snap(event) == SnapMode::off ? tick : grid().nearest(tick)));
        return;
    }
    if (gesture_ == Gesture::pending_select && past_threshold) {
        gesture_ = (press_modifiers_ & Qt::ControlModifier) ? Gesture::duplicate_move : Gesture::move;
        if (gesture_ == Gesture::duplicate_move && handle_note_ && !controller_.selection().contains(handle_note_->on_id)) {
            auto selection = controller_.selection();
            selection.insert(handle_note_->on_id);
            controller_.set_selection(std::move(selection));
        }
        begin_drag_from_selection();
    }
    const Tick raw_ticks = static_cast<Tick>(std::llround(delta.x() * mapping_.ticks_per_pixel));
    // Pitch displacement from the rows under the pointer (folded rows skip hidden pitches).
    const int raw_pitch = pitch_at_y(current_point_.y() - ruler_height) - pitch_at_y(press_point_.y() - ruler_height);
    switch (gesture_) {
    case Gesture::move:
    case Gesture::duplicate_move: {
        Tick ticks = raw_ticks;
        int pitch = raw_pitch;
        if (event->modifiers() & Qt::ShiftModifier) {
            // Lock to the first dominant axis after the threshold (section 7.2).
            if (!shift_axis_locked_ && past_threshold) {
                shift_axis_locked_ = true;
                lock_to_time_axis_ = std::abs(delta.x()) >= std::abs(delta.y());
            }
            if (shift_axis_locked_) {
                if (lock_to_time_axis_)
                    pitch = 0;
                else
                    ticks = 0;
            }
        }
        const auto previous_pitch = preview_move_.delta_pitch;
        preview_move_ = compute_move(snapshot_, ticks, pitch, grid(), effective_snap(event));
        if (handle_note_ && preview_move_.delta_pitch != previous_pitch) {
            release_audition();
            audition_pitch_ = handle_note_->pitch + preview_move_.delta_pitch;
            emit audition_note_on(*audition_pitch_, handle_note_->velocity);
        }
        break;
    }
    case Gesture::resize_end:
        if (handle_note_)
            preview_resize_delta_ = compute_end_delta(snapshot_, *handle_note_, raw_ticks, grid(), effective_snap(event));
        break;
    case Gesture::resize_start:
        if (handle_note_)
            preview_resize_delta_ = compute_start_delta(snapshot_, *handle_note_, raw_ticks, grid(), effective_snap(event));
        break;
    case Gesture::draw:
        if (handle_note_) {
            // Dragging sets the duration; crossing pitches previews but never sprays notes.
            const QPointF local = to_note_area(point);
            const Tick tick = mapping_.x_to_tick(local.x());
            Tick end = effective_snap(event) == SnapMode::off ? tick : grid().next(tick);
            handle_note_->end = std::max(handle_note_->start + 1, end);
        }
        break;
    case Gesture::erase: {
        const auto hit = hit_test(point);
        if (hit.note) {
            const auto& note = controller_.note_index().notes()[*hit.note];
            if (note_editable(note))
                erase_hits_.insert(note.on_id);
        }
        break;
    }
    case Gesture::rubber_band:
    case Gesture::pending_select:
    case Gesture::none:
    case Gesture::pan:
    case Gesture::scrub: break;
    }
    // Auto-scroll near the viewport edges.
    const QRectF area = note_area();
    const bool near_edge = point.x() < area.left() + 16 || point.x() > area.right() - 16 ||
                           point.y() < area.top() + 12 || point.y() > area.bottom() - 12;
    if (near_edge && gesture_ != Gesture::erase)
        auto_scroll_timer_.start();
    else
        auto_scroll_timer_.stop();
    update();
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
        const int rows = dy < 0 ? 1 : -1;
        const int old_top = mapping_.top_pitch;
        mapping_.top_pitch = std::clamp(mapping_.top_pitch + rows, 0, 127);
        press_point_.ry() += (mapping_.top_pitch - old_top) * mapping_.row_height;
    }
    emit mapping_changed();
    update();
}

void PianoRollView::mouseReleaseEvent(QMouseEvent* event) {
    auto_scroll_timer_.stop();
    if (event->button() == Qt::MiddleButton && gesture_ == Gesture::pan) {
        gesture_ = Gesture::none;
        setCursor(Qt::ArrowCursor);
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    commit_gesture(event);
}

void PianoRollView::commit_gesture([[maybe_unused]] QMouseEvent* event) {
    const Gesture gesture = gesture_;
    gesture_ = Gesture::none;
    release_audition();
    const auto& sequence = controller_.active_sequence();
    const auto& index = controller_.note_index();
    switch (gesture) {
    case Gesture::pending_select:
        // A click without drag: Ctrl toggles membership; a plain click keeps only that note.
        if (handle_note_) {
            auto selection = selection_at_press_;
            if (press_modifiers_ & Qt::ControlModifier) {
                if (selection.contains(handle_note_->on_id))
                    selection.erase(handle_note_->on_id);
                else
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
                              note.velocity, default_channel(), 0};
        auto changes = insert_notes(controller_.document(), std::span(&created, 1), "Draw note");
        if (changes)
            controller_.apply(*changes);
        break;
    }
    case Gesture::erase: {
        if (erase_hits_.empty())
            break;
        std::vector<EventId> ids(erase_hits_.begin(), erase_hits_.end());
        auto changes = delete_notes(controller_.document(), sequence.id, ids, "Erase notes");
        if (changes)
            controller_.apply(*changes);
        auto selection = controller_.selection();
        for (const auto& id : ids)
            selection.erase(id);
        controller_.set_selection(std::move(selection));
        break;
    }
    case Gesture::none:
    case Gesture::pan:
    case Gesture::scrub: break;
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
    if (gesture_ == Gesture::rubber_band || gesture_ == Gesture::pending_select)
        controller_.set_selection(selection_at_press_);
    if (gesture_ == Gesture::pan)
        mapping_ = pan_origin_;
    gesture_ = Gesture::none;
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
    if (!note_area().contains(point) || hit_test(point).note || !controller_.active_track_index())
        return;
    gesture_ = Gesture::none;
    const QPointF local = to_note_area(point);
    const Tick tick = std::max<Tick>(0, mapping_.x_to_tick(local.x()));
    const Tick start = effective_snap(event) == SnapMode::off ? tick : grid().floor(tick);
    const int pitch = std::clamp(pitch_at_y(local.y()), 0, 127);
    const NewNote created{controller_.active_track(), start, start + default_note_length(),
                          static_cast<std::uint8_t>(pitch), default_velocity(), default_channel(), 0};
    auto changes = insert_notes(controller_.document(), std::span(&created, 1), "Insert note");
    if (changes && controller_.apply(*changes)) {
        const auto& notes = controller_.notes().notes;
        for (const auto& note : notes)
            if (note.start == start && note.pitch == pitch && note_editable(note))
                controller_.set_selection({note.on_id});
    }
}

void PianoRollView::wheelEvent(QWheelEvent* event) {
    const double steps = event->angleDelta().y() / 120.0;
    const QPointF local = to_note_area(event->position());
    if (event->modifiers() & Qt::ControlModifier) {
        zoom_history_.push_back(mapping_);
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
            mapping_.top_pitch = std::clamp(mapping_.top_pitch + rows, std::min(127, visible_row_count() - 1), 127);
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
    // Keyboard note entry (spec 29.3): Left/Right move the red transport line (window command),
    // Up/Down choose the entry pitch when nothing is selected (Shift: an octave), and Return or
    // Insert adds a note there and advances.
    if (controller_.selection().empty() && (event->modifiers() & ~Qt::ShiftModifier) == Qt::NoModifier &&
        (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
        const int step = (event->modifiers() & Qt::ShiftModifier) ? 12 : 1;
        cursor_pitch_ = std::clamp(cursor_pitch_ + (event->key() == Qt::Key_Up ? step : -step), 0, 127);
        if (cursor_pitch_ > mapping_.top_pitch || cursor_pitch_ <= mapping_.top_pitch - visible_row_count()) {
            mapping_.top_pitch = std::clamp(cursor_pitch_ + visible_row_count() / 2, 0, 127);
            emit mapping_changed();
        }
        emit hover_changed(controller_.edit_cursor(), cursor_pitch_);
        update();
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Insert) &&
        controller_.active_track_index() && controller_.selection().empty()) { // step entry only without a selection
        const Tick start = controller_.edit_cursor();
        const NewNote created{controller_.active_track(), start, start + default_note_length(),
                              static_cast<std::uint8_t>(cursor_pitch_), default_velocity(), default_channel(), 0};
        if (auto changes = insert_notes(controller_.document(), std::span(&created, 1), "Insert note")) {
            if (controller_.apply(*changes))
                controller_.set_edit_cursor(start + default_note_length()); // step entry: advance the cursor
        }
        emit audition_note_on(cursor_pitch_, default_velocity());
        QTimer::singleShot(150, this, [this, pitch = cursor_pitch_] { emit audition_note_off(pitch); }); // audible
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PianoRollView::focusOutEvent(QFocusEvent* event) {
    release_audition(); // scrub/audition notes end when focus is lost (section 7.3)
    QWidget::focusOutEvent(event);
}

void PianoRollView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    emit mapping_changed();
}

void PianoRollView::contextMenuEvent(QContextMenuEvent* event) {
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
    menu.addAction(tr("Select Out-of-Scale Notes"), this, &PianoRollView::select_out_of_scale);
    menu.addSeparator();
    menu.addAction(tr("Fit Selection"), this, &PianoRollView::fit_selection_or_all);
    menu.exec(event->globalPos());
}

// --- View and selection commands -----------------------------------------------------------------

void PianoRollView::fit_all() {
    zoom_history_.push_back(mapping_);
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
        // Rows between 8 and 16 px: as tall as fits, so a short phrase is easy to edit.
        const int span = high - low + 7;
        mapping_.row_height = std::clamp(std::floor(height / span), 8.0, 16.0);
    } else {
        mapping_.row_height = 14.0;
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
    zoom_history_.push_back(mapping_);
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
    emit mapping_changed();
    update();
}

void PianoRollView::zoom_time(double factor) {
    zoom_history_.push_back(mapping_);
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

template <class Predicate>
std::set<EventId> select_matching(const DocumentController& controller, Predicate predicate,
                                  const std::function<bool(const NoteSpan&)>& editable) {
    std::set<EventId> selection;
    const auto selected = controller.selected_notes();
    for (const auto& note : controller.notes().notes)
        if (editable(note) && predicate(note, selected))
            selection.insert(note.on_id);
    return selection;
}

} // namespace

void PianoRollView::select_same_pitch() {
    const auto editable = [this](const NoteSpan& note) { return note_editable(note); };
    controller_.set_selection(select_matching(
        controller_,
        [](const NoteSpan& note, const std::vector<NoteSpan>& selected) {
            return std::any_of(selected.begin(), selected.end(), [&](const NoteSpan& s) { return s.pitch == note.pitch; });
        },
        editable));
}

void PianoRollView::select_same_pitch_class() {
    const auto editable = [this](const NoteSpan& note) { return note_editable(note); };
    controller_.set_selection(select_matching(
        controller_,
        [](const NoteSpan& note, const std::vector<NoteSpan>& selected) {
            return std::any_of(selected.begin(), selected.end(),
                               [&](const NoteSpan& s) { return s.pitch % 12 == note.pitch % 12; });
        },
        editable));
}

void PianoRollView::select_same_channel() {
    const auto editable = [this](const NoteSpan& note) { return note_editable(note); };
    controller_.set_selection(select_matching(
        controller_,
        [](const NoteSpan& note, const std::vector<NoteSpan>& selected) {
            return std::any_of(selected.begin(), selected.end(),
                               [&](const NoteSpan& s) { return s.channel == note.channel; });
        },
        editable));
}

void PianoRollView::select_out_of_scale() {
    std::set<EventId> selection;
    for (const auto& note : controller_.notes().notes)
        if (note_editable(note) && !in_scale(note.pitch))
            selection.insert(note.on_id);
    controller_.set_selection(std::move(selection));
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
        view_->set_top_pitch(127 - value);
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
    const int rows = std::clamp(view_->visible_row_count(), 1, 128);
    vertical_->setRange(0, std::max(0, 128 - rows));
    vertical_->setPageStep(rows);
    vertical_->setSingleStep(1);
    horizontal_->setValue(static_cast<int>(static_cast<__int128>(mapping.origin_tick) * scrollbar_resolution / total));
    vertical_->setValue(127 - mapping.top_pitch);
    syncing_ = false;
}

} // namespace mwb::ui
