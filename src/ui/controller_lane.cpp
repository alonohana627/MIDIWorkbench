#include "mwb/ui/controller_lane.hpp"

#include "mwb/core/note_edit.hpp"
#include "mwb/ui/document_controller.hpp"
#include "mwb/ui/theme.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace mwb::ui {

QString controller_name(int controller) {
    switch (controller) {
    case 0: return QObject::tr("Bank Select (MSB)");
    case 1: return QObject::tr("Modulation");
    case 2: return QObject::tr("Breath");
    case 4: return QObject::tr("Foot Controller");
    case 5: return QObject::tr("Portamento Time");
    case 6: return QObject::tr("Data Entry (MSB)");
    case 7: return QObject::tr("Volume");
    case 8: return QObject::tr("Balance");
    case 10: return QObject::tr("Pan");
    case 11: return QObject::tr("Expression");
    case 32: return QObject::tr("Bank Select (LSB)");
    case 38: return QObject::tr("Data Entry (LSB)");
    case 64: return QObject::tr("Sustain Pedal");
    case 65: return QObject::tr("Portamento");
    case 66: return QObject::tr("Sostenuto");
    case 67: return QObject::tr("Soft Pedal");
    case 71: return QObject::tr("Resonance");
    case 74: return QObject::tr("Brightness");
    case 98: return QObject::tr("NRPN (LSB)");
    case 99: return QObject::tr("NRPN (MSB)");
    case 100: return QObject::tr("RPN (LSB)");
    case 101: return QObject::tr("RPN (MSB)");
    case 120: return QObject::tr("All Sound Off (reserved)");
    case 121: return QObject::tr("Reset All Controllers (reserved)");
    case 123: return QObject::tr("All Notes Off (reserved)");
    default: return QObject::tr("CC %1").arg(controller);
    }
}

int controller_default_value(const LaneTarget& target) {
    switch (target.kind) {
    case LaneTarget::Kind::pitch_bend: return 8192;
    case LaneTarget::Kind::channel_pressure: return 0;
    case LaneTarget::Kind::controller:
        switch (target.controller) {
        case 7: return 100;  // volume
        case 8:              // balance
        case 10: return 64;  // pan
        case 11: return 127; // expression
        default: return 0;
        }
    case LaneTarget::Kind::velocity:
    case LaneTarget::Kind::release_velocity: break;
    }
    return 0;
}

QString LaneTarget::short_label() const {
    switch (kind) {
    case Kind::velocity: return QObject::tr("Velocity");
    case Kind::release_velocity: return QObject::tr("Release");
    case Kind::controller: return QObject::tr("CC %1").arg(controller);
    case Kind::pitch_bend: return QObject::tr("Bend");
    case Kind::channel_pressure: return QObject::tr("Pressure");
    }
    return {};
}

QString LaneTarget::detail_label() const {
    switch (kind) {
    case Kind::velocity: return {};
    case Kind::release_velocity: return QObject::tr("velocity");
    case Kind::controller: return controller_name(controller);
    case Kind::pitch_bend:
    case Kind::channel_pressure: return QObject::tr("ch %1").arg(channel + 1);
    }
    return {};
}

QString LaneTarget::label() const {
    switch (kind) {
    case Kind::velocity: return QObject::tr("Velocity");
    case Kind::release_velocity: return QObject::tr("Release Velocity");
    case Kind::controller: return QObject::tr("CC %1 %2 (ch %3)").arg(controller).arg(controller_name(controller)).arg(channel + 1);
    case Kind::pitch_bend: return QObject::tr("Pitch Bend (ch %1)").arg(channel + 1);
    case Kind::channel_pressure: return QObject::tr("Channel Pressure (ch %1)").arg(channel + 1);
    }
    return {};
}

ControllerLane::ControllerLane(DocumentController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setFocusPolicy(Qt::StrongFocus); // Up/Down change the selected notes' velocity (keyboard access)
    setMouseTracking(true);          // value readout under the pointer
    setAccessibleName(tr("Controller lane"));
    connect(&controller_, &DocumentController::document_changed, this, qOverload<>(&QWidget::update));
    connect(&controller_, &DocumentController::selection_changed, this, qOverload<>(&QWidget::update));
}

QSize ControllerLane::sizeHint() const { return {400, 90}; }

void ControllerLane::set_target(LaneTarget target) {
    target_ = target;
    setAccessibleName(target.label());
    setToolTip(target.label()); // the margin shows a shortened label
    update();
}

void ControllerLane::follow_track_channel(std::uint8_t channel) {
    if (!follow_channel_ || channel == target_.channel || stroke_)
        return;
    LaneTarget target = target_;
    target.channel = channel;
    set_target(target);
}

void ControllerLane::set_mapping(const ViewMapping& mapping, int left_margin, Grid grid) {
    mapping_ = mapping;
    left_margin_ = left_margin;
    grid_ = grid;
    update();
}

int ControllerLane::value_at_y(double y) const {
    const double usable = std::max(1.0, height() - 4.0);
    const double fraction = std::clamp(1.0 - (y - 2.0) / usable, 0.0, 1.0);
    const int value = static_cast<int>(std::lround(fraction * target_.maximum()));
    if (target_.kind == LaneTarget::Kind::velocity)
        return std::clamp(value, 1, 127); // sounding note-ons stay 1..127
    return value;
}

double ControllerLane::y_for_value(int value) const {
    const double usable = std::max(1.0, height() - 4.0);
    return 2.0 + usable * (1.0 - static_cast<double>(value) / target_.maximum());
}

bool ControllerLane::matches(const Event& event) const {
    if (!event.is_channel() || event.channel() != target_.channel)
        return false;
    switch (target_.kind) {
    case LaneTarget::Kind::controller:
        return event.message() == ChannelMessage::control_change && event.data1() == target_.controller;
    case LaneTarget::Kind::pitch_bend: return event.message() == ChannelMessage::pitch_bend;
    case LaneTarget::Kind::channel_pressure: return event.message() == ChannelMessage::channel_pressure;
    case LaneTarget::Kind::velocity:
    case LaneTarget::Kind::release_velocity: return false;
    }
    return false;
}

int ControllerLane::event_value(const Event& event) const {
    return target_.kind == LaneTarget::Kind::pitch_bend   ? event.pitch_bend_value()
           : target_.kind == LaneTarget::Kind::controller ? event.data2()
                                                          : event.data1();
}

const std::vector<std::size_t>& ControllerLane::matching_positions() const {
    const auto active = controller_.active_track_index();
    const bool same_target = matching_target_.kind == target_.kind && matching_target_.channel == target_.channel &&
                             matching_target_.controller == target_.controller;
    if (matching_generation_ != controller_.generation() || matching_track_ != active ||
        matching_sequence_ != controller_.active_sequence_index() || !same_target) {
        matching_.clear();
        if (active) {
            const auto& events = controller_.active_sequence().tracks[*active].events;
            for (std::size_t position = 0; position < events.size(); ++position)
                if (matches(events[position]))
                    matching_.push_back(position);
        }
        matching_generation_ = controller_.generation();
        matching_track_ = active;
        matching_sequence_ = controller_.active_sequence_index();
        matching_target_ = target_;
    }
    return matching_;
}

void ControllerLane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Base));
    painter.fillRect(QRect(0, 0, left_margin_, height()), palette().color(QPalette::Window));
    {
        // Short label in the middle of the margin (the scale sits in its top and bottom corners);
        // long controller names are elided, the full label is the tooltip.
        painter.setPen(palette().color(QPalette::Text));
        const QFontMetrics metrics = fontMetrics();
        const int text_width = std::max(8, left_margin_ - 8);
        QString text = metrics.elidedText(target_.short_label(), Qt::ElideRight, text_width);
        if (const QString detail = target_.detail_label(); !detail.isEmpty())
            text += QLatin1Char('\n') + metrics.elidedText(detail, Qt::ElideRight, text_width);
        painter.drawText(QRect(4, 14, left_margin_ - 6, std::max(14, height() - 28)), Qt::AlignVCenter | Qt::AlignLeft, text);
    }
    painter.save();
    painter.setClipRect(QRect(left_margin_, 0, width() - left_margin_, height()));
    painter.translate(left_margin_, 0);
    const double area_width = width() - left_margin_;
    const Tick first = mapping_.origin_tick;
    const Tick last = mapping_.x_to_tick(area_width) + 1;
    const RollColors colors = roll_colors();
    // Bars and beats line up with the piano roll above.
    {
        const auto& meter = controller_.meter_map();
        const auto position = meter.position(std::max<Tick>(0, first));
        for (std::int64_t bar = position.bar; bar < position.bar + 100000; ++bar) {
            const Tick start = meter.bar_start(bar);
            if (start > last)
                break;
            const auto& segment = meter.segment_at(start);
            const Tick beat = meter.ticks_per_beat(segment);
            if (beat > 0 && static_cast<double>(beat) / mapping_.ticks_per_pixel >= 8.0) {
                painter.setPen(QPen(colors.beat, 1));
                for (Tick tick = start + beat; tick < start + meter.ticks_per_bar(segment); tick += beat)
                    painter.drawLine(QPointF(mapping_.tick_to_x(tick), 0), QPointF(mapping_.tick_to_x(tick), height()));
            }
            painter.setPen(QPen(colors.bar, 1));
            painter.drawLine(QPointF(mapping_.tick_to_x(start), 0), QPointF(mapping_.tick_to_x(start), height()));
        }
    }
    painter.setPen(QPen(palette().color(QPalette::Midlight), 1, Qt::DotLine));
    const double middle = y_for_value(target_.kind == LaneTarget::Kind::pitch_bend ? 8192 : 64);
    painter.drawLine(QPointF(0, middle), QPointF(area_width, middle));

    const auto active = controller_.active_track_index();
    if (target_.kind == LaneTarget::Kind::velocity || target_.kind == LaneTarget::Kind::release_velocity) {
        const auto& index = controller_.note_index();
        const auto& selection = controller_.selection();
        const auto visible = index.query(TimeRange{first, last});
        // Level of detail: with more notes than pixel columns, one stem per column (its highest
        // value; selected wins) instead of one per note.
        const auto columns = static_cast<std::size_t>(std::max(1.0, area_width));
        if (visible.size() > columns) {
            std::vector<int> highest(columns, -1);
            std::vector<char> marked(columns, 0);
            for (const auto position : visible) {
                const auto& note = index.notes()[position];
                if (!active || note.on_track != *active)
                    continue;
                const double x = mapping_.tick_to_x(note.start);
                if (x < 0 || x >= area_width)
                    continue;
                const auto column = static_cast<std::size_t>(x);
                const int value = target_.kind == LaneTarget::Kind::velocity ? note.velocity : note.release_velocity;
                highest[column] = std::max(highest[column], value);
                if (selection.contains(note.on_id))
                    marked[column] = 1;
            }
            for (std::size_t column = 0; column < columns; ++column) {
                if (highest[column] < 0)
                    continue;
                painter.setPen(QPen(marked[column] ? palette().color(QPalette::WindowText) : palette().color(QPalette::Highlight), 1));
                const double x = static_cast<double>(column) + 0.5;
                painter.drawLine(QPointF(x, height()), QPointF(x, y_for_value(highest[column])));
            }
        } else {
            for (const auto position : visible) {
                const auto& note = index.notes()[position];
                if (!active || note.on_track != *active)
                    continue;
                const int value = target_.kind == LaneTarget::Kind::velocity ? note.velocity : note.release_velocity;
                const double x = mapping_.tick_to_x(note.start);
                const bool selected = selection.contains(note.on_id);
                painter.setPen(QPen(selected ? palette().color(QPalette::WindowText) : palette().color(QPalette::Highlight), selected ? 3 : 2));
                painter.drawLine(QPointF(x, height()), QPointF(x, y_for_value(value)));
                painter.drawEllipse(QPointF(x, y_for_value(value)), 2.5, 2.5);
            }
        }
    } else if (active) {
        const auto& track = controller_.active_sequence().tracks[*active];
        QPolygonF steps;
        int previous = -1;
        const auto value_of = [&](const Event& event) {
            return target_.kind == LaneTarget::Kind::pitch_bend ? event.pitch_bend_value()
                   : target_.kind == LaneTarget::Kind::controller ? event.data2()
                                                                   : event.data1();
        };
        // Only matching events in the visible range are visited (cached positions, binary search):
        // the value in effect at the left edge comes from the previous matching event.
        const auto& positions = matching_positions();
        auto begin = std::lower_bound(positions.begin(), positions.end(), first,
                                      [&](std::size_t position, Tick tick) { return track.events[position].tick < tick; });
        if (begin != positions.begin()) {
            previous = value_of(track.events[*std::prev(begin)]);
            steps << QPointF(0, y_for_value(previous));
        }
        auto end_visible = std::upper_bound(begin, positions.end(), last,
                                                  [&](Tick tick, std::size_t position) { return tick < track.events[position].tick; });
        if (static_cast<double>(end_visible - begin) > area_width) {
            // Level of detail: dense data draws one min-max bar per pixel column.
            const auto columns = static_cast<std::size_t>(std::max(1.0, area_width));
            std::vector<int> low(columns, INT32_MAX);
            std::vector<int> high(columns, -1);
            for (auto iterator = begin; iterator != end_visible; ++iterator) {
                const auto& event = track.events[*iterator];
                const double x = mapping_.tick_to_x(event.tick);
                if (x < 0 || x >= area_width)
                    continue;
                const auto column = static_cast<std::size_t>(x);
                const int value = value_of(event);
                low[column] = std::min(low[column], value);
                high[column] = std::max(high[column], value);
            }
            painter.setPen(QPen(colors.loop, 1));
            for (std::size_t column = 0; column < columns; ++column)
                if (high[column] >= 0) {
                    const double x = static_cast<double>(column) + 0.5;
                    painter.drawLine(QPointF(x, y_for_value(low[column]) + 0.5), QPointF(x, y_for_value(high[column]) - 0.5));
                }
            end_visible = begin; // the per-event step line below is skipped
            steps.clear();
            previous = -1;
        }
        for (auto iterator = begin; iterator != end_visible; ++iterator) {
            const auto& event = track.events[*iterator];
            const int value = target_.kind == LaneTarget::Kind::pitch_bend ? event.pitch_bend_value()
                              : target_.kind == LaneTarget::Kind::controller ? event.data2()
                                                                              : event.data1();
            const double x = mapping_.tick_to_x(event.tick);
            if (previous >= 0)
                steps << QPointF(x, y_for_value(previous));
            steps << QPointF(x, y_for_value(value));
            previous = value;
            painter.setBrush(colors.loop);
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(QPointF(x, y_for_value(value)), 3, 3);
        }
        if (previous >= 0)
            steps << QPointF(area_width, y_for_value(previous));
        painter.setPen(QPen(colors.loop, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(steps);
    }
    if (stroke_ && stroke_->points.size() > 1) {
        painter.setPen(QPen(stroke_->erase ? danger_color() : palette().color(QPalette::Highlight), 2));
        QPolygonF line;
        for (const auto& [tick, value] : stroke_by_tick())
            line << QPointF(mapping_.tick_to_x(tick), stroke_->erase ? height() / 2.0 : y_for_value(value));
        painter.drawPolyline(line);
    }
    // Value readout: while drawing, the value under the pen; otherwise under the pointer.
    const std::optional<std::pair<Tick, int>> readout =
        stroke_ && !stroke_->points.empty() && !stroke_->erase ? std::optional(stroke_->points.back()) : hover_;
    if (readout) {
        const QString text = QString::number(readout->second);
        const QPointF anchor(mapping_.tick_to_x(readout->first) + 6, std::clamp(y_for_value(readout->second) - 4, 12.0, height() - 4.0));
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(anchor, text);
    }
    painter.restore();
    // Scale inside the margin, right-aligned against the plot, so it never covers the first stems.
    QFont small = font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() - 1));
    painter.setFont(small);
    painter.setPen(palette().color(QPalette::PlaceholderText));
    const int maximum = target_.maximum();
    if (height() >= 48) {
        painter.drawText(QRectF(2, 1, left_margin_ - 5, 14), Qt::AlignRight | Qt::AlignTop, QString::number(maximum));
        painter.drawText(QRectF(2, height() - 15, left_margin_ - 5, 14), Qt::AlignRight | Qt::AlignBottom, QString::number(0));
    }
    if (hasFocus()) {
        painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(rect()).adjusted(1, 1, -1, -1));
    }
}

bool ControllerLane::event(QEvent* event) {
    // A focused velocity lane keeps Up/Down (nudge velocity) instead of the window's transpose.
    if (event->type() == QEvent::ShortcutOverride && target_.kind == LaneTarget::Kind::velocity) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) &&
            (key->modifiers() & ~Qt::ShiftModifier) == Qt::NoModifier) {
            event->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

void ControllerLane::contextMenuEvent(QContextMenuEvent* event) {
    // Windows delivers the menu after the right-button release: an erase drag never opens it, and
    // neither does a right press during a draw stroke.
    if (std::exchange(suppress_context_menu_, false) || stroke_) {
        event->accept();
        return;
    }
    // Change what this lane shows, or remove it.
    QMenu menu(this);
    auto* show = menu.addMenu(tr("Show"));
    const std::uint8_t channel = target_.channel;
    const auto add_target = [&](const QString& text, LaneTarget target) {
        auto* action = show->addAction(text, this, [this, target] { set_target(target); });
        action->setCheckable(true);
        action->setChecked(target.kind == target_.kind &&
                           (target.kind != LaneTarget::Kind::controller || target.controller == target_.controller));
    };
    add_target(tr("Velocity"), LaneTarget{LaneTarget::Kind::velocity, 0, channel});
    add_target(tr("Release Velocity"), LaneTarget{LaneTarget::Kind::release_velocity, 0, channel});
    show->addSeparator();
    for (const auto& [number, name] : {std::pair{1, tr("Modulation (CC 1)")}, std::pair{7, tr("Volume (CC 7)")},
                                       std::pair{10, tr("Pan (CC 10)")}, std::pair{11, tr("Expression (CC 11)")},
                                       std::pair{64, tr("Sustain Pedal (CC 64)")}})
        add_target(name, LaneTarget{LaneTarget::Kind::controller, static_cast<std::uint8_t>(number), channel});
    show->addAction(tr("Other Controller…"), this, [this, channel] {
        bool accepted = false;
        const int number = QInputDialog::getInt(this, tr("Controller"), tr("CC number"), target_.controller, 0, 127, 1, &accepted);
        if (accepted)
            set_target(LaneTarget{LaneTarget::Kind::controller, static_cast<std::uint8_t>(number), channel});
    });
    show->addSeparator();
    add_target(tr("Pitch Bend"), LaneTarget{LaneTarget::Kind::pitch_bend, 0, channel});
    add_target(tr("Channel Pressure"), LaneTarget{LaneTarget::Kind::channel_pressure, 0, channel});
    auto* channels = menu.addMenu(tr("Channel"));
    for (int index = 0; index < 16; ++index) {
        auto* action = channels->addAction(tr("Channel %1").arg(index + 1), this, [this, index] {
            LaneTarget target = target_;
            target.channel = static_cast<std::uint8_t>(index);
            follow_channel_ = false; // an explicit choice stays when the active track changes
            set_target(target);
        });
        action->setCheckable(true);
        action->setChecked(index == target_.channel);
    }
    menu.addSeparator();
    menu.addAction(tr("Remove Lane"), this, [this] { emit remove_requested(this); });
    menu.exec(event->globalPos());
}

void ControllerLane::leaveEvent(QEvent* event) {
    hover_.reset();
    update();
    QWidget::leaveEvent(event);
}

void ControllerLane::nudge_selected_velocity(int delta) {
    const auto selected = controller_.selected_notes();
    if (selected.empty() || target_.kind != LaneTarget::Kind::velocity)
        return;
    std::vector<NoteUpdate> updates;
    for (const auto& note : selected)
        updates.push_back(NoteUpdate{note.on_id, note.start, note.end, note.pitch,
                                     static_cast<std::uint8_t>(std::clamp(note.velocity + delta, 1, 127)), note.channel,
                                     note.release_velocity});
    if (auto changes = update_notes(controller_.document(), controller_.active_sequence().id, updates, "Change velocity"))
        controller_.apply(*changes);
}

void ControllerLane::mousePressEvent(QMouseEvent* event) {
    // Left draws, right erases; other buttons (middle pans the roll) and a second button during a
    // stroke never start or replace one.
    if (stroke_ || event->position().x() < left_margin_ ||
        (event->button() != Qt::LeftButton && event->button() != Qt::RightButton))
        return;
    suppress_context_menu_ = false;
    stroke_ = Stroke{};
    stroke_->erase = event->button() == Qt::RightButton; // right-drag erases a range
    stroke_->button = event->button();
    stroke_->press_x = event->position().x();
    const double x = event->position().x() - left_margin_;
    stroke_->points.emplace_back(std::max<Tick>(0, mapping_.x_to_tick(x)), value_at_y(event->position().y()));
    update();
}

void ControllerLane::mouseMoveEvent(QMouseEvent* event) {
    if (!stroke_) {
        if (event->position().x() >= left_margin_) {
            hover_ = std::pair{std::max<Tick>(0, mapping_.x_to_tick(event->position().x() - left_margin_)),
                               value_at_y(event->position().y())};
            update();
        }
        return;
    }
    if (event->spontaneous() && !(event->buttons() & stroke_->button)) {
        stroke_.reset(); // the release went elsewhere: abandon the stroke
        update();
        return;
    }
    const double x = event->position().x() - left_margin_;
    const Tick tick = std::max<Tick>(0, mapping_.x_to_tick(x));
    stroke_->points.emplace_back(tick, value_at_y(event->position().y()));
    stroke_->max_x_distance = std::max(stroke_->max_x_distance, std::abs(event->position().x() - stroke_->press_x));
    update();
}

void ControllerLane::focusOutEvent(QFocusEvent* event) {
    if (stroke_) {
        stroke_.reset(); // a stroke never survives focus loss (Alt+Tab, a dialog)
        update();
    }
    QWidget::focusOutEvent(event);
}

void ControllerLane::mouseReleaseEvent(QMouseEvent* event) {
    if (!stroke_ || event->button() != stroke_->button)
        return;
    if (stroke_->erase) {
        // A right-click (no drag) is for the context menu; erasing needs a right-drag.
        if (stroke_->max_x_distance < QApplication::startDragDistance()) {
            stroke_.reset();
            update();
            return;
        }
        suppress_context_menu_ = true;
    }
    if (target_.kind == LaneTarget::Kind::velocity || target_.kind == LaneTarget::Kind::release_velocity)
        commit_velocity_stroke();
    else
        commit_controller_stroke();
    stroke_.reset();
    update();
}

void ControllerLane::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && stroke_) {
        stroke_.reset(); // cancel restores the state at gesture start
        update();
        return;
    }
    if ((event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) && target_.kind == LaneTarget::Kind::velocity) {
        const int step = event->modifiers() & Qt::ShiftModifier ? 10 : 1;
        nudge_selected_velocity(event->key() == Qt::Key_Up ? step : -step);
        return;
    }
    QWidget::keyPressEvent(event);
}

std::vector<std::pair<Tick, int>> ControllerLane::stroke_by_tick() const {
    std::map<Tick, int> latest;
    if (stroke_)
        for (const auto& [tick, value] : stroke_->points)
            latest[tick] = value; // arrival order: a later point at the same tick wins
    return {latest.begin(), latest.end()};
}

void ControllerLane::commit_velocity_stroke() {
    if (stroke_->points.empty() || stroke_->erase)
        return;
    const auto points = stroke_by_tick();
    // A drag that stays on one stem (horizontal travel under the drag distance) sets the value
    // released, the same number the readout showed.
    const bool vertical = stroke_->max_x_distance < QApplication::startDragDistance();
    const int released = stroke_->points.back().second;
    const Tick first = points.front().first;
    const Tick last = points.back().first;
    const Tick slack = static_cast<Tick>(4 * mapping_.ticks_per_pixel);
    const auto active = controller_.active_track_index();
    if (!active)
        return;
    const auto value_at = [&](Tick tick) {
        if (vertical)
            return released;
        // Linear interpolation along the drag path.
        auto upper = std::lower_bound(points.begin(), points.end(), std::pair<Tick, int>{tick, INT32_MIN});
        if (upper == points.end())
            return points.back().second;
        if (upper == points.begin())
            return upper->second;
        auto lower = std::prev(upper);
        if (upper->first == lower->first)
            return upper->second;
        const double fraction = static_cast<double>(tick - lower->first) / static_cast<double>(upper->first - lower->first);
        return static_cast<int>(std::lround(lower->second + fraction * (upper->second - lower->second)));
    };
    const auto& selection = controller_.selection();
    std::vector<NoteUpdate> updates;
    for (const auto& note : controller_.notes().notes) {
        if (note.on_track != *active || note.start < first - slack || note.start > last + slack)
            continue;
        if (!selection.empty() && !selection.contains(note.on_id))
            continue;
        NoteUpdate update{note.on_id, note.start, note.end, note.pitch, note.velocity, note.channel, note.release_velocity};
        const int value = value_at(note.start);
        if (target_.kind == LaneTarget::Kind::velocity)
            update.velocity = static_cast<std::uint8_t>(std::clamp(value, 1, 127));
        else
            update.release_velocity = static_cast<std::uint8_t>(std::clamp(value, 0, 127));
        updates.push_back(update);
    }
    if (updates.empty())
        return;
    auto changes = update_notes(controller_.document(), controller_.active_sequence().id, updates,
                                target_.kind == LaneTarget::Kind::velocity ? "Draw velocities" : "Draw release velocities");
    if (changes)
        controller_.apply(*changes);
    else
        emit status_message(QString::fromStdString(changes.error().message));
}

void ControllerLane::commit_controller_stroke() {
    if (stroke_->points.empty())
        return;
    const auto active = controller_.active_track_index();
    if (!active)
        return;
    const auto points = stroke_by_tick();
    const Tick first = grid_.floor(points.front().first);
    const Tick last = std::max(grid_.next(points.back().first), first + 1);
    const auto& track = controller_.active_sequence().tracks[*active];
    // Value in effect at the range end before the edit: restored afterwards when needed so
    // playback after the range is unchanged (section 11.2).
    std::optional<int> value_after_range;
    bool has_event_at_end = false;
    for (const auto& event : track.events) {
        if (!matches(event))
            continue;
        if (event.tick == last)
            has_event_at_end = true;
        if (event.tick > last)
            break;
        value_after_range = target_.kind == LaneTarget::Kind::pitch_bend ? event.pitch_bend_value()
                            : target_.kind == LaneTarget::Kind::controller ? event.data2()
                                                                            : event.data1();
    }
    // One bulk edit: removals and insertions in a single pass over the track.
    BulkTrackEdit bulk;
    for (const auto& event : track.events)
        if (matches(event) && event.tick >= first && event.tick < last)
            bulk.remove.insert(event.id);
    const auto make_event = [&](Tick tick, int value) {
        switch (target_.kind) {
        case LaneTarget::Kind::pitch_bend: return events::pitch_bend(tick, target_.channel, value);
        case LaneTarget::Kind::channel_pressure:
            return events::channel_pressure(tick, target_.channel, static_cast<std::uint8_t>(value));
        case LaneTarget::Kind::controller:
        case LaneTarget::Kind::velocity:
        case LaneTarget::Kind::release_velocity: break;
        }
        return events::control_change(tick, target_.channel, target_.controller, static_cast<std::uint8_t>(value));
    };
    int last_written = -1;
    if (!stroke_->erase) {
        // Sample the drawn line (linear between the stroke's points, as the preview shows it) at
        // every grid point; skip repeated values (discrete MIDI 1.0 events). The sustain pedal is
        // a switch: drawn values snap to off/on so a curve never becomes accidental pedal flutter
        // (section 11.2).
        const bool sustain_lane = target_.kind == LaneTarget::Kind::controller && target_.controller == 64;
        const auto value_at = [&](Tick tick) {
            const auto upper = std::lower_bound(points.begin(), points.end(), tick,
                                                [](const std::pair<Tick, int>& point, Tick value) { return point.first < value; });
            if (upper == points.end())
                return points.back().second;
            if (upper == points.begin() || upper->first == tick)
                return upper->second;
            const auto lower = std::prev(upper);
            const double fraction = static_cast<double>(tick - lower->first) / static_cast<double>(upper->first - lower->first);
            return static_cast<int>(std::lround(lower->second + fraction * (upper->second - lower->second)));
        };
        const Tick stroke_last = points.back().first;
        for (Tick tick = first; tick < last; tick = grid_.next(tick)) {
            if (tick > stroke_last && tick != first)
                break;
            int value = std::clamp(value_at(tick), 0, target_.maximum());
            if (sustain_lane)
                value = value >= 64 ? 127 : 0;
            if (value == last_written)
                continue;
            bulk.insert.emplace_back(make_event(tick, value), OrderPlacement::before_note_starts);
            last_written = value;
        }
    }
    // Playback after the range stays as it was: the value in effect before the edit, or the
    // channel's reset value when the lane had no earlier event (a fresh bend must not stay bent).
    const int restore = value_after_range.value_or(controller_default_value(target_));
    const bool wrote = last_written >= 0 || !bulk.remove.empty();
    if (wrote && !has_event_at_end && restore != last_written)
        bulk.insert.emplace_back(make_event(last, restore), OrderPlacement::before_note_starts);
    ChangeBuilder builder(controller_.document(), stroke_->erase ? "Erase controller range" : "Draw controller curve");
    std::ignore = builder.edit_track(track.id, std::move(bulk));
    controller_.apply(builder.finish());
}

} // namespace mwb::ui
