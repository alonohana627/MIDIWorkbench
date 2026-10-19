#include "mwb/ui/drum_grid.hpp"

#include "mwb/core/note_edit.hpp"
#include "mwb/core/pitch.hpp"
#include "mwb/ui/document_controller.hpp"
#include "mwb/ui/editor_math.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
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
    setAccessibleName(tr("Drum grid"));
    connect(&controller_, &DocumentController::document_changed, this, qOverload<>(&QWidget::update));
    connect(&controller_, &DocumentController::view_state_changed, this, qOverload<>(&QWidget::update));
    connect(&controller_, &DocumentController::edit_cursor_changed, this, qOverload<>(&QWidget::update));
}

QSize DrumGridView::sizeHint() const { return {name_width + 32 * cell_width, 16 * row_height}; }

void DrumGridView::set_origin(Tick tick) {
    origin_ = std::max<Tick>(0, tick);
    update();
}

void DrumGridView::set_velocities(int normal, int accent) {
    normal_velocity_ = std::clamp(normal, 1, 127);
    accent_velocity_ = std::clamp(accent, 1, 127);
}

Tick DrumGridView::step_ticks() const {
    const auto& sequence = controller_.active_sequence();
    const auto grid = Grid::make(sequence.settings.snap, sequence.division.is_ppq() ? sequence.division.ppq : default_ppq);
    return std::max<Tick>(1, grid.point(1));
}

std::vector<int> DrumGridView::rows() const {
    // Default kit rows plus any other pitches used on the active track, highest first.
    std::set<int> pitches = {35, 36, 37, 38, 39, 40, 42, 44, 46, 41, 45, 48, 49, 51};
    const auto active = controller_.active_track_index();
    for (const auto& note : controller_.notes().notes)
        if (active && note.on_track == *active)
            pitches.insert(note.pitch);
    return std::vector<int>(pitches.rbegin(), pitches.rend());
}

std::optional<std::pair<int, std::int64_t>> DrumGridView::cell_at(QPointF point) const {
    if (point.x() < name_width || point.y() < row_height)
        return std::nullopt;
    const auto row_list = rows();
    const auto row = static_cast<std::size_t>((point.y() - row_height) / row_height);
    if (row >= row_list.size())
        return std::nullopt;
    const auto step = static_cast<std::int64_t>((point.x() - name_width) / cell_width);
    return std::pair{row_list[row], step};
}

void DrumGridView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Base));
    const auto row_list = rows();
    const Tick step = step_ticks();
    const int steps_visible = std::max(1, (width() - name_width) / cell_width);
    const auto active = controller_.active_track_index();
    const auto& meter = controller_.meter_map();

    // Notes of the active track per (pitch, step): count, max velocity, off-grid flag.
    struct Cell {
        int count = 0;
        int velocity = 0;
        bool off_grid = false;
    };
    std::map<std::pair<int, std::int64_t>, Cell> cells;
    const TimeRange window{origin_, origin_ + step * steps_visible};
    const auto& index = controller_.note_index();
    for (const auto position : index.query(window)) {
        const auto& note = index.notes()[position];
        if (!active || note.on_track != *active || note.start < window.begin)
            continue;
        const auto column = (note.start - origin_) / step;
        auto& cell = cells[{note.pitch, column}];
        ++cell.count;
        cell.velocity = std::max<int>(cell.velocity, note.velocity);
        if ((note.start - origin_) % step != 0)
            cell.off_grid = true;
    }
    painter.setPen(palette().color(QPalette::Text));
    for (int column = 0; column < steps_visible; ++column) {
        const Tick tick = origin_ + column * step;
        const auto position = meter.position(tick);
        if (position.beat == 0 && position.tick_in_beat == 0)
            painter.drawText(QRect(name_width + column * cell_width, 0, cell_width * 4, row_height),
                             Qt::AlignLeft | Qt::AlignVCenter, QString::number(position.bar + 1));
    }
    for (std::size_t row = 0; row < row_list.size(); ++row) {
        const int pitch = row_list[row];
        const int y = row_height * static_cast<int>(row + 1);
        painter.fillRect(QRect(0, y, name_width, row_height), palette().color(QPalette::Window));
        painter.setPen(palette().color(QPalette::Text));
        const QString name = gm_drum_name(pitch);
        painter.drawText(QRect(4, y, name_width - 8, row_height), Qt::AlignVCenter | Qt::AlignLeft,
                         QString("%1  %2").arg(pitch).arg(name.isEmpty() ? QString::fromStdString(pitch_name(static_cast<std::uint8_t>(pitch))) : name));
        for (int column = 0; column < steps_visible; ++column) {
            const QRect cell_rect(name_width + column * cell_width + 1, y + 1, cell_width - 2, row_height - 2);
            const Tick tick = origin_ + column * step;
            const auto beat_position = meter.position(tick);
            QColor background = beat_position.tick_in_beat == 0 ? palette().color(QPalette::AlternateBase)
                                                                 : palette().color(QPalette::Base);
            painter.fillRect(cell_rect, background);
            painter.setPen(palette().color(QPalette::Mid));
            painter.drawRect(cell_rect);
            const auto found = cells.find({pitch, column});
            if (found == cells.end())
                continue;
            const auto& cell = found->second;
            QColor fill = QColor::fromHsv(210, 60 + cell.velocity, 240 - cell.velocity / 2);
            painter.fillRect(cell_rect.adjusted(2, 2, -2, -2), fill);
            if (cell.count > 1 || cell.off_grid) {
                // Ambiguous: several or off-grid notes share this cell; open it in the piano roll.
                painter.setPen(QPen(QColor(230, 120, 30), 2));
                painter.drawLine(cell_rect.topLeft() + QPoint(3, 3), cell_rect.bottomRight() - QPoint(3, 3));
            }
        }
    }
}

void DrumGridView::mousePressEvent(QMouseEvent* event) {
    const auto cell = cell_at(event->position());
    const auto active = controller_.active_track_index();
    if (!cell || !active || event->button() != Qt::LeftButton)
        return;
    const auto [pitch, column] = *cell;
    const Tick step = step_ticks();
    const Tick start = origin_ + column * step;
    std::vector<EventId> existing;
    for (const auto& note : controller_.notes().notes)
        if (note.on_track == *active && note.pitch == pitch && note.start >= start && note.start < start + step)
            existing.push_back(note.on_id);
    const auto& sequence = controller_.active_sequence();
    if (existing.size() > 1) {
        emit status_message(tr("Several notes share this cell; opened in the piano roll"));
        emit open_in_piano_roll(start, pitch);
        return;
    }
    if (existing.size() == 1) {
        auto changes = delete_notes(controller_.document(), sequence.id, existing, "Clear drum step");
        if (changes)
            controller_.apply(*changes);
        return;
    }
    const int velocity = (event->modifiers() & Qt::ShiftModifier) ? accent_velocity_ : normal_velocity_;
    const NewNote note{sequence.tracks[*active].id, start, start + std::max<Tick>(1, step / 2),
                       static_cast<std::uint8_t>(pitch), static_cast<std::uint8_t>(velocity),
                       sequence.tracks[*active].is_drum ? std::uint8_t{9} : std::uint8_t{0}, 0};
    auto changes = insert_notes(controller_.document(), std::span(&note, 1), "Set drum step");
    if (changes)
        controller_.apply(*changes);
}

void DrumGridView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (const auto cell = cell_at(event->position()))
        emit open_in_piano_roll(origin_ + cell->second * step_ticks(), cell->first);
}

void DrumGridView::wheelEvent(QWheelEvent* event) {
    const Tick step = step_ticks();
    set_origin(origin_ + (event->angleDelta().y() > 0 ? -4 : 4) * step);
    event->accept();
}

} // namespace mwb::ui
