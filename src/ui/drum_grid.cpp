#include "mwb/ui/drum_grid.hpp"

#include "mwb/ui/theme.hpp"

#include "mwb/core/checked.hpp"
#include "mwb/core/note_edit.hpp"
#include "mwb/core/pitch.hpp"
#include "mwb/ui/document_controller.hpp"
#include "mwb/ui/piano_roll.hpp"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>

namespace mwb::ui {

QString gm_drum_name(int pitch) {
    static const char* const names[] = {
        "Acoustic Bass Drum", "Bass Drum 1", "Side Stick", "Acoustic Snare", "Hand Clap", "Electric Snare",
        "Low Floor Tom", "Closed Hi-Hat", "High Floor Tom", "Pedal Hi-Hat", "Low Tom", "Open Hi-Hat",
        "Low-Mid Tom", "Hi-Mid Tom", "Crash Cymbal 1", "High Tom", "Ride Cymbal 1", "Chinese Cymbal",
        "Ride Bell", "Tambourine", "Splash Cymbal", "Cowbell", "Crash Cymbal 2", "Vibraslap",
        "Ride Cymbal 2", "Hi Bongo", "Low Bongo", "Mute Hi Conga", "Open Hi Conga", "Low Conga",
        "High Timbale", "Low Timbale", "High Agogo", "Low Agogo", "Cabasa", "Maracas",
        "Short Whistle", "Long Whistle", "Short Guiro", "Long Guiro", "Claves", "Hi Wood Block",
        "Low Wood Block", "Mute Cuica", "Open Cuica", "Mute Triangle", "Open Triangle"};
    if (pitch < 35 || pitch > 81)
        return {};
    return QObject::tr(names[pitch - 35]);
}

DrumGridView::DrumGridView(DocumentController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
    setAccessibleName(tr("Drum grid"));
    connect(&controller_, &DocumentController::document_changed, this, [this] {
        set_top_row(top_row_); // rows may have been added or removed
        update();
    });
    connect(&controller_, &DocumentController::view_state_changed, this, [this] {
        set_top_row(top_row_);
        update();
    });
    connect(&controller_, &DocumentController::edit_cursor_changed, this, qOverload<>(&QWidget::update));
}

QSize DrumGridView::sizeHint() const { return {name_width + 32 * cell_width, 16 * row_height}; }

Grid DrumGridView::step_grid() const {
    const auto& sequence = controller_.active_sequence();
    return Grid::make(sequence.settings.snap, sequence.division.is_ppq() ? sequence.division.ppq : default_ppq);
}

std::int64_t DrumGridView::step_at(Tick tick) const {
    // Index of the grid point at or before `tick`; columns are derived from the origin, never
    // accumulated, so inexact grids (triplets at odd PPQ) do not drift.
    const Grid grid = step_grid();
    std::int64_t index =
        mul_div_floor(tick, grid.fraction.denominator, 4 * static_cast<std::int64_t>(grid.ppq) * grid.fraction.numerator)
            .value_or(0);
    while (index > 0 && grid.point(index) > tick)
        --index;
    while (grid.point(index + 1) <= tick && grid.point(index + 1) > grid.point(index))
        ++index;
    return index;
}

std::int64_t DrumGridView::first_step() const { return step_at(origin_); }

void DrumGridView::set_origin(Tick tick) {
    const Tick origin = step_grid().floor(std::max<Tick>(0, tick));
    if (origin == origin_)
        return;
    origin_ = origin;
    if (playhead_)
        follow_suspended_ = true; // a manual scroll while playing keeps the user's view
    emit view_changed();
    update();
}

void DrumGridView::scroll_steps(std::int64_t steps) {
    if (steps == 0)
        return;
    const auto target = std::max<std::int64_t>(0, first_step() + steps);
    set_origin(step_grid().point(target));
}

int DrumGridView::row_count() const { return static_cast<int>(rows().size()); }

int DrumGridView::visible_row_count() const { return std::max(1, (height() - row_height) / row_height); }

int DrumGridView::visible_step_count() const { return std::max(1, (width() - name_width) / cell_width); }

void DrumGridView::set_top_row(int row) {
    const int top = std::clamp(row, 0, std::max(0, row_count() - visible_row_count()));
    if (top == top_row_)
        return;
    top_row_ = top;
    emit view_changed();
    update();
}

void DrumGridView::set_velocities(int normal, int accent) {
    normal_velocity_ = std::clamp(normal, 1, 127);
    accent_velocity_ = std::clamp(accent, 1, 127);
}

void DrumGridView::set_follow_playhead(bool follow) {
    follow_playhead_ = follow;
    follow_suspended_ = false;
}

void DrumGridView::set_playhead(std::optional<Tick> tick) {
    if (playhead_ == tick)
        return;
    playhead_ = tick;
    if (!tick)
        follow_suspended_ = false; // the next Play follows again
    if (tick && follow_playhead_ && !follow_suspended_ && !stroke_) {
        // Page by whole screens' worth of steps when the playhead leaves the visible columns.
        const auto first = first_step();
        const auto steps = static_cast<std::int64_t>(visible_step_count());
        const auto at = step_at(*tick);
        if (at < first || at > first + steps * 9 / 10) {
            origin_ = step_grid().point(std::max<std::int64_t>(0, at - steps / 10));
            emit view_changed();
        }
    }
    update();
}

const std::vector<int>& DrumGridView::rows() const {
    // Default kit rows plus any other pitches used on the active track, highest first; cached per
    // generation and active track (paint, clicks and keys all ask).
    const auto active = controller_.active_track_index();
    const auto key = std::tuple{controller_.generation(), controller_.active_sequence_index(), active};
    if (rows_key_ && *rows_key_ == key)
        return rows_cache_;
    std::set<int> pitches = {35, 36, 37, 38, 39, 40, 42, 44, 46, 41, 45, 48, 49, 51};
    if (active)
        for (const auto& note : controller_.notes().notes)
            if (note.on_track == *active)
                pitches.insert(note.pitch);
    rows_cache_.assign(pitches.rbegin(), pitches.rend());
    rows_key_ = key;
    return rows_cache_;
}

std::optional<std::pair<int, std::int64_t>> DrumGridView::cell_at(QPointF point) const {
    if (point.x() < name_width || point.y() < row_height)
        return std::nullopt;
    const auto& row_list = rows();
    const auto row = static_cast<std::size_t>(top_row_) + static_cast<std::size_t>((point.y() - row_height) / row_height);
    if (row >= row_list.size())
        return std::nullopt;
    const auto column = static_cast<std::int64_t>((point.x() - name_width) / cell_width);
    return std::pair{row_list[row], first_step() + column};
}

DrumGridView::CellNotes DrumGridView::notes_in_cell(int pitch, std::int64_t step) const {
    CellNotes cell;
    const auto active = controller_.active_track_index();
    if (!active || pitch < 0 || pitch > 127)
        return cell;
    const Grid grid = step_grid();
    const Tick start = grid.point(step);
    const Tick end = std::max(start + 1, grid.point(step + 1));
    const auto& index = controller_.note_index();
    for (const auto position : index.query(TimeRange{start, end}, static_cast<std::uint8_t>(pitch),
                                           static_cast<std::uint8_t>(pitch))) {
        const auto& note = index.notes()[position];
        if (note.on_track != *active || note.pitch != pitch || note.start < start || note.start >= end)
            continue;
        cell.notes.push_back(note.on_id);
        cell.velocity = std::max<int>(cell.velocity, note.velocity);
        if (note.start != start)
            cell.off_grid = true;
    }
    return cell;
}

void DrumGridView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const RollColors colors = roll_colors();
    painter.fillRect(rect(), palette().color(QPalette::Base));
    const auto& row_list = rows();
    const Grid grid = step_grid();
    const auto first = first_step();
    const int steps_visible = visible_step_count();
    const Tick window_begin = grid.point(first);
    const Tick window_end = grid.point(first + steps_visible);
    const auto active = controller_.active_track_index();
    const auto& meter = controller_.meter_map();
    const auto x_for = [&](Tick tick) {
        return name_width + static_cast<double>(tick - window_begin) / std::max(1.0, grid.step()) * cell_width;
    };

    // Notes of the active track per (pitch, column): count, max velocity, off-grid flag.
    struct Cell {
        int count = 0;
        int velocity = 0;
        bool off_grid = false;
    };
    std::map<std::pair<int, std::int64_t>, Cell> cells;
    const auto& index = controller_.note_index();
    if (active)
        for (const auto position : index.query(TimeRange{window_begin, window_end})) {
            const auto& note = index.notes()[position];
            if (note.on_track != *active || note.start < window_begin || note.start >= window_end)
                continue;
            const auto step = step_at(note.start);
            auto& cell = cells[{note.pitch, step - first}];
            ++cell.count;
            cell.velocity = std::max<int>(cell.velocity, note.velocity);
            if (grid.point(step) != note.start)
                cell.off_grid = true;
        }
    // Header: bar numbers (pinned while rows scroll).
    painter.fillRect(QRect(0, 0, width(), row_height), colors.ruler_background);
    painter.setPen(colors.ruler_text);
    for (int column = 0; column < steps_visible; ++column) {
        const auto position = meter.position(grid.point(first + column));
        if (position.beat == 0 && position.tick_in_beat == 0)
            painter.drawText(QRect(name_width + column * cell_width + 2, 0, cell_width * 4, row_height),
                             Qt::AlignLeft | Qt::AlignVCenter, QString::number(position.bar + 1));
    }
    painter.save();
    painter.setClipRect(QRect(0, row_height, width(), height() - row_height));
    // Playhead column under everything else.
    const auto playhead_column = playhead_ && *playhead_ >= window_begin && *playhead_ < window_end
                                     ? std::optional<std::int64_t>(step_at(*playhead_) - first)
                                     : std::nullopt;
    if (playhead_column) {
        QColor column_fill = palette().color(QPalette::Highlight);
        column_fill.setAlpha(45);
        painter.fillRect(QRect(name_width + static_cast<int>(*playhead_column) * cell_width, row_height, cell_width,
                               height() - row_height),
                         column_fill);
    }
    const int last_row = std::min(static_cast<int>(row_list.size()), top_row_ + visible_row_count() + 1);
    const QColor accent_mark = palette().color(QPalette::Text);
    for (int row = top_row_; row < last_row; ++row) {
        const int pitch = row_list[static_cast<std::size_t>(row)];
        const int y = row_height * (row - top_row_ + 1);
        painter.fillRect(QRect(0, y, name_width, row_height), palette().color(QPalette::Window));
        painter.setPen(palette().color(QPalette::Text));
        const QString name = gm_drum_name(pitch);
        painter.drawText(QRect(4, y, name_width - 8, row_height), Qt::AlignVCenter | Qt::AlignLeft,
                         QString("%1  %2").arg(pitch).arg(name.isEmpty() ? QString::fromStdString(pitch_name(static_cast<std::uint8_t>(pitch))) : name));
        for (int column = 0; column < steps_visible; ++column) {
            const QRect cell_rect(name_width + column * cell_width + 1, y + 1, cell_width - 2, row_height - 2);
            painter.setPen(palette().color(QPalette::Mid));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(cell_rect);
            const bool pending = stroke_ && stroke_->pitch == pitch && stroke_->steps.contains(first + column);
            const auto found = cells.find({pitch, column});
            if (found != cells.end()) {
                const auto& cell = found->second;
                // Velocity as bar height (readable without color), accents marked on top.
                const int usable = cell_rect.height() - 4;
                const int bar = std::max(2, static_cast<int>(std::lround(usable * cell.velocity / 127.0)));
                const QRect hit(cell_rect.left() + 2, cell_rect.bottom() - 1 - bar, cell_rect.width() - 4, bar);
                QColor fill = note_fill(palette().color(QPalette::Highlight), cell.velocity);
                if (pending && stroke_->mode == PaintStroke::Mode::clear)
                    fill.setAlpha(60); // will be cleared on release
                painter.fillRect(hit, fill);
                painter.setPen(QPen(colors.bar_strong, 1));
                painter.drawRect(hit);
                if (cell.velocity >= accent_velocity_)
                    painter.fillRect(QRect(cell_rect.left() + 2, cell_rect.top() + 2, cell_rect.width() - 4, 2), accent_mark);
                if (cell.count > 1 || cell.off_grid) {
                    // Ambiguous: several or off-grid notes share this cell; a click opens it in the piano roll.
                    painter.setPen(QPen(QColor(230, 120, 30), 2));
                    painter.drawLine(cell_rect.topLeft() + QPoint(3, 3), cell_rect.bottomRight() - QPoint(3, 3));
                }
            } else if (pending && stroke_->mode == PaintStroke::Mode::set) {
                QColor fill = palette().color(QPalette::Highlight);
                fill.setAlpha(110);
                painter.fillRect(cell_rect.adjusted(2, 2, -2, -2), fill);
            }
        }
    }
    // Beat and bar lines over the cells (the bar line is stronger, so bars are easy to count).
    for (int column = 0; column <= steps_visible; ++column) {
        const auto position = meter.position(grid.point(first + column));
        if (position.tick_in_beat != 0)
            continue;
        const bool bar = position.beat == 0;
        painter.setPen(QPen(bar ? colors.bar_strong : colors.beat, bar ? 1.5 : 1));
        const double x = name_width + column * cell_width;
        painter.drawLine(QPointF(x, row_height), QPointF(x, height()));
    }
    // Edit cursor (where playback starts), the playhead and the keyboard cell cursor.
    const Tick cursor = controller_.edit_cursor();
    if (cursor >= window_begin && cursor <= window_end) {
        const double x = x_for(cursor);
        painter.setPen(playhead_ ? QPen(colors.cursor, 1, Qt::DashLine) : QPen(colors.playhead, 2));
        painter.drawLine(QPointF(x, row_height), QPointF(x, height()));
    }
    if (playhead_column) {
        const double x = x_for(*playhead_);
        painter.setPen(QPen(colors.playhead, 2));
        painter.drawLine(QPointF(x, row_height), QPointF(x, height()));
    }
    if (hasFocus() && !row_list.empty()) {
        const auto found = std::find(row_list.begin(), row_list.end(), cursor_pitch_);
        const int row = found == row_list.end() ? 0 : static_cast<int>(found - row_list.begin());
        const auto column = cursor_step_ - first;
        if (row >= top_row_ && column >= 0 && column < steps_visible) {
            const QRect focus(name_width + static_cast<int>(column) * cell_width, row_height * (row - top_row_ + 1),
                              cell_width, row_height);
            painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(focus.adjusted(1, 1, -1, -1));
        }
    }
    painter.restore();
}

void DrumGridView::mousePressEvent(QMouseEvent* event) {
    if (stroke_ || event->button() != Qt::LeftButton)
        return; // a second button never restarts a running stroke
    pending_open_.reset();
    toggled_generation_.reset(); // a double-click only takes back the click it belongs to
    const auto cell = cell_at(event->position());
    if (!cell)
        return;
    setFocus(Qt::MouseFocusReason);
    cursor_pitch_ = cell->first;
    cursor_step_ = cell->second;
    const auto existing = notes_in_cell(cell->first, cell->second);
    if (existing.notes.size() > 1 || existing.off_grid) {
        // Several or off-grid notes: never deleted from here; the release opens them in the piano roll.
        pending_open_ = std::pair{step_grid().point(cell->second), cell->first};
        update();
        return;
    }
    PaintStroke stroke;
    stroke.pitch = cell->first;
    stroke.steps.insert(cell->second);
    const bool shift = (event->modifiers() & Qt::ShiftModifier) != 0;
    if (existing.notes.empty()) {
        stroke.mode = PaintStroke::Mode::set;
        stroke.accent = shift;
    } else {
        stroke.mode = shift ? PaintStroke::Mode::accent : PaintStroke::Mode::clear;
    }
    stroke_ = std::move(stroke);
    update();
}

void DrumGridView::mouseMoveEvent(QMouseEvent* event) {
    if (!stroke_)
        return;
    if (event->spontaneous() && !(event->buttons() & Qt::LeftButton)) {
        stroke_.reset(); // the release went elsewhere: abandon the stroke
        update();
        return;
    }
    if (stroke_->mode == PaintStroke::Mode::accent || event->position().x() < name_width)
        return;
    // Painting stays on the pressed row; every crossed column joins the stroke.
    const auto column = static_cast<std::int64_t>((event->position().x() - name_width) / cell_width);
    const auto step = first_step() + std::clamp<std::int64_t>(column, 0, visible_step_count() - 1);
    if (stroke_->steps.insert(step).second)
        update();
}

void DrumGridView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    if (pending_open_) {
        const auto [tick, pitch] = *pending_open_;
        pending_open_.reset();
        emit status_message(tr("Several or off-grid notes share this cell; opened in the piano roll"));
        emit open_in_piano_roll(tick, pitch);
        return;
    }
    if (!stroke_)
        return;
    const PaintStroke stroke = *stroke_;
    stroke_.reset();
    if (commit_stroke(stroke) && stroke.steps.size() == 1 && stroke.mode != PaintStroke::Mode::accent)
        toggled_generation_ = controller_.generation();
    controller_.set_edit_cursor(step_grid().point(cursor_step_));
    update();
}

bool DrumGridView::commit_stroke(const PaintStroke& stroke) {
    const auto active = controller_.active_track_index();
    if (!active) {
        emit status_message(tr("Add a track first: drum steps are written to the active track"));
        return false;
    }
    const auto& sequence = controller_.active_sequence();
    const Grid grid = step_grid();
    const bool several = stroke.steps.size() > 1;
    Result<ChangeSet> changes = make_error(ErrorCode::invalid_data, "nothing to change");
    switch (stroke.mode) {
    case PaintStroke::Mode::set: {
        // New steps use the configured note length, at most one step long, on the track's channel.
        const Tick configured =
            Grid::make(sequence.settings.default_note_length, grid.ppq).point(1);
        const std::uint8_t channel = default_channel_for(controller_);
        const int velocity = stroke.accent ? accent_velocity_ : normal_velocity_;
        std::vector<NewNote> notes;
        for (const auto step : stroke.steps) {
            if (!notes_in_cell(stroke.pitch, step).notes.empty())
                continue; // painting over a set cell leaves it as it is
            const Tick start = grid.point(step);
            const Tick length = std::clamp<Tick>(configured, 1, std::max<Tick>(1, grid.point(step + 1) - start));
            notes.push_back(NewNote{sequence.tracks[*active].id, start, start + length,
                                    static_cast<std::uint8_t>(stroke.pitch), static_cast<std::uint8_t>(velocity), channel, 0});
        }
        if (notes.empty())
            return false;
        changes = insert_notes(controller_.document(), notes, several ? "Set drum steps" : "Set drum step");
        break;
    }
    case PaintStroke::Mode::clear: {
        std::vector<EventId> ids;
        for (const auto step : stroke.steps) {
            const auto cell = notes_in_cell(stroke.pitch, step);
            if (cell.notes.size() == 1 && !cell.off_grid)
                ids.push_back(cell.notes.front()); // ambiguous cells are never cleared from here
        }
        if (ids.empty())
            return false;
        changes = delete_notes(controller_.document(), sequence.id, ids, several ? "Clear drum steps" : "Clear drum step");
        break;
    }
    case PaintStroke::Mode::accent: {
        // Shift-click on a hit toggles it between the normal and the accent velocity.
        const auto cell = notes_in_cell(stroke.pitch, *stroke.steps.begin());
        if (cell.notes.size() != 1)
            return false;
        const auto position = controller_.note_index().find(cell.notes.front());
        if (!position)
            return false;
        const auto& note = controller_.note_index().notes()[*position];
        const int velocity = note.velocity >= accent_velocity_ ? normal_velocity_ : accent_velocity_;
        const NoteUpdate update{note.on_id, note.start, note.end, note.pitch, static_cast<std::uint8_t>(velocity),
                                note.channel, note.release_velocity};
        changes = update_notes(controller_.document(), sequence.id, std::span(&update, 1), "Accent drum step");
        break;
    }
    }
    if (!changes) {
        emit status_message(QString::fromStdString(changes.error().message));
        return false;
    }
    QString error;
    if (!controller_.apply(*changes, &error)) {
        if (!error.isEmpty())
            emit status_message(error);
        return false;
    }
    return true;
}

bool DrumGridView::event(QEvent* event) {
    // Arrow keys and Return operate the grid while it has focus (not the window's commands).
    if (event->type() == QEvent::ShortcutOverride) {
        const auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Return:
        case Qt::Key_Enter:
            event->accept();
            return true;
        default: break;
        }
    }
    if (event->type() == QEvent::UngrabMouse && stroke_) {
        stroke_.reset(); // a popup or dialog took the mouse: nothing is committed
        update();
    }
    return QWidget::event(event);
}

void DrumGridView::ensure_cursor_visible() {
    const auto& row_list = rows();
    const auto found = std::find(row_list.begin(), row_list.end(), cursor_pitch_);
    if (found != row_list.end()) {
        const int row = static_cast<int>(found - row_list.begin());
        if (row < top_row_)
            set_top_row(row);
        else if (row >= top_row_ + visible_row_count())
            set_top_row(row - visible_row_count() + 1);
    }
    const auto first = first_step();
    const auto steps = static_cast<std::int64_t>(visible_step_count());
    if (cursor_step_ < first)
        set_origin(step_grid().point(cursor_step_));
    else if (cursor_step_ >= first + steps)
        set_origin(step_grid().point(cursor_step_ - steps + 1));
}

void DrumGridView::keyPressEvent(QKeyEvent* event) {
    const auto& row_list = rows();
    if (row_list.empty()) {
        QWidget::keyPressEvent(event);
        return;
    }
    auto found = std::find(row_list.begin(), row_list.end(), cursor_pitch_);
    if (found == row_list.end()) {
        found = row_list.begin();
        cursor_pitch_ = *found;
    }
    switch (event->key()) {
    case Qt::Key_Up:
        if (found != row_list.begin())
            cursor_pitch_ = *std::prev(found);
        break;
    case Qt::Key_Down:
        if (std::next(found) != row_list.end())
            cursor_pitch_ = *std::next(found);
        break;
    case Qt::Key_Left: cursor_step_ = std::max<std::int64_t>(0, cursor_step_ - 1); break;
    case Qt::Key_Right: ++cursor_step_; break;
    case Qt::Key_Return: // Enter toggles (Space stays Play/Pause)
    case Qt::Key_Enter: {
        const auto cell = notes_in_cell(cursor_pitch_, cursor_step_);
        if (cell.notes.size() > 1 || cell.off_grid) {
            emit open_in_piano_roll(step_grid().point(cursor_step_), cursor_pitch_);
            break;
        }
        const bool shift = (event->modifiers() & Qt::ShiftModifier) != 0; // Shift: accent
        PaintStroke stroke;
        stroke.pitch = cursor_pitch_;
        stroke.steps.insert(cursor_step_);
        stroke.mode = cell.notes.empty() ? PaintStroke::Mode::set
                      : shift            ? PaintStroke::Mode::accent
                                         : PaintStroke::Mode::clear;
        stroke.accent = shift;
        commit_stroke(stroke);
        break;
    }
    default: QWidget::keyPressEvent(event); return;
    }
    ensure_cursor_visible();
    controller_.set_edit_cursor(step_grid().point(cursor_step_));
    update();
    event->accept();
}

void DrumGridView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    // The first click of a double-click toggled the cell; take back exactly that toggle (never
    // an unrelated earlier edit) before opening the cell in the piano roll.
    const QString undo = controller_.undo_text();
    if (toggled_generation_ && *toggled_generation_ == controller_.generation() &&
        (undo == QStringLiteral("Set drum step") || undo == QStringLiteral("Clear drum step")))
        std::ignore = controller_.undo();
    toggled_generation_.reset();
    stroke_.reset();
    pending_open_.reset();
    if (const auto cell = cell_at(event->position()))
        emit open_in_piano_roll(step_grid().point(cell->second), cell->first);
}

void DrumGridView::wheelEvent(QWheelEvent* event) {
    // Plain wheel scrolls rows, Shift+wheel (or a horizontal touchpad swipe) scrolls time.
    // Fractions accumulate, so precision touchpads move smoothly instead of a page per event.
    const QPoint delta = event->angleDelta();
    if (delta.isNull() || (event->modifiers() & Qt::ControlModifier)) {
        event->accept();
        return;
    }
    const bool shift = (event->modifiers() & Qt::ShiftModifier) != 0;
    const double vertical = shift ? 0.0 : delta.y() / 120.0;
    const double horizontal = shift ? (delta.y() != 0 ? delta.y() : delta.x()) / 120.0 : delta.x() / 120.0;
    wheel_rows_ -= vertical * 3;
    wheel_steps_ -= horizontal * 4;
    const auto rows = static_cast<int>(wheel_rows_);
    const auto steps = static_cast<std::int64_t>(wheel_steps_);
    wheel_rows_ -= rows;
    wheel_steps_ -= static_cast<double>(steps);
    if (rows != 0)
        set_top_row(top_row_ + rows);
    scroll_steps(steps);
    event->accept();
}

void DrumGridView::focusOutEvent(QFocusEvent* event) {
    if (stroke_ || pending_open_) {
        stroke_.reset(); // a stroke never survives focus loss
        pending_open_.reset();
        update();
    }
    QWidget::focusOutEvent(event);
}

void DrumGridView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    set_top_row(top_row_);
    emit view_changed();
}

// --- Panel with scroll bars ----------------------------------------------------------------------

DrumGridPanel::DrumGridPanel(DocumentController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    view_ = new DrumGridView(controller, this);
    horizontal_ = new QScrollBar(Qt::Horizontal, this);
    vertical_ = new QScrollBar(Qt::Vertical, this);
    horizontal_->setAccessibleName(tr("Drum grid time position"));
    vertical_->setAccessibleName(tr("Drum grid rows"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* row = new QHBoxLayout;
    row->setSpacing(0);
    row->addWidget(view_, 1);
    row->addWidget(vertical_);
    layout->addLayout(row, 1);
    layout->addWidget(horizontal_);
    setFocusProxy(view_);
    connect(view_, &DrumGridView::view_changed, this, &DrumGridPanel::sync_scrollbars);
    connect(&controller_, &DocumentController::document_changed, this, &DrumGridPanel::sync_scrollbars);
    connect(&controller_, &DocumentController::view_state_changed, this, &DrumGridPanel::sync_scrollbars);
    connect(horizontal_, &QScrollBar::valueChanged, this, [this](int value) {
        if (!syncing_)
            view_->set_origin(view_->step_grid().point(value));
    });
    connect(vertical_, &QScrollBar::valueChanged, this, [this](int value) {
        if (!syncing_)
            view_->set_top_row(value);
    });
    sync_scrollbars();
}

void DrumGridPanel::sync_scrollbars() {
    syncing_ = true;
    // Time in steps: the song plus a quarter more, so steps can be added after the end.
    const Grid grid = view_->step_grid();
    const Tick end = std::max<Tick>(controller_.active_sequence().effective_end() * 5 / 4, grid.point(64));
    const auto step = std::max(1.0, grid.step());
    const auto total = static_cast<std::int64_t>(std::ceil(static_cast<double>(end) / step));
    const int visible = view_->visible_step_count();
    const auto first = static_cast<std::int64_t>(std::llround(static_cast<double>(view_->origin()) / step));
    const auto maximum = std::clamp<std::int64_t>(std::max(total, first + visible) - visible, 0, INT_MAX / 2);
    horizontal_->setRange(0, static_cast<int>(maximum));
    horizontal_->setPageStep(visible);
    horizontal_->setSingleStep(1);
    horizontal_->setValue(static_cast<int>(std::clamp<std::int64_t>(first, 0, maximum)));
    const int rows = view_->row_count();
    const int visible_rows = view_->visible_row_count();
    vertical_->setRange(0, std::max(0, rows - visible_rows));
    vertical_->setPageStep(visible_rows);
    vertical_->setSingleStep(1);
    vertical_->setValue(view_->top_row());
    vertical_->setVisible(rows > visible_rows);
    syncing_ = false;
}

} // namespace mwb::ui
