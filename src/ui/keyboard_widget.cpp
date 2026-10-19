#include "mwb/ui/keyboard_widget.hpp"

#include "mwb/core/pitch.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>

namespace mwb::ui {

namespace {

bool black_key(int pitch) {
    switch (pitch % 12) {
    case 1:
    case 3:
    case 6:
    case 8:
    case 10: return true;
    default: return false;
    }
}

/// Index of a pitch among white keys (C=0, D=1, ... for octave 0).
int white_index(int pitch) {
    static constexpr int index_in_octave[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    return (pitch / 12) * 7 + index_in_octave[pitch % 12];
}

} // namespace

KeyboardWidget::KeyboardWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus); // selection or audition never steals editor focus
    setAccessibleName(tr("On-screen keyboard"));
    setAttribute(Qt::WA_Hover);
}

QSize KeyboardWidget::sizeHint() const { return {520, 72}; }

void KeyboardWidget::set_fixed_velocity(std::optional<int> velocity) { fixed_velocity_ = velocity; }

void KeyboardWidget::set_scale(int root_pitch_class, std::uint16_t absolute_mask, bool show) {
    root_ = root_pitch_class;
    scale_mask_ = absolute_mask;
    show_scale_ = show;
    update();
}

void KeyboardWidget::set_key_state(int pitch, KeySource source, bool down) {
    if (pitch < 0 || pitch > 127)
        return;
    const auto bit = static_cast<std::uint8_t>(1u << static_cast<unsigned>(source));
    auto& owners = owners_[static_cast<std::size_t>(pitch)];
    owners = down ? static_cast<std::uint8_t>(owners | bit) : static_cast<std::uint8_t>(owners & ~bit);
    update();
}

void KeyboardWidget::set_sustained(std::set<int> pitches) {
    sustained_ = std::move(pitches);
    update();
}

void KeyboardWidget::release_source(KeySource source) {
    const auto bit = static_cast<std::uint8_t>(1u << static_cast<unsigned>(source));
    for (std::size_t pitch = 0; pitch < owners_.size(); ++pitch) {
        if (owners_[pitch] & bit) {
            owners_[pitch] = static_cast<std::uint8_t>(owners_[pitch] & ~bit);
            if (source == KeySource::pointer)
                emit note_off(static_cast<int>(pitch));
        }
    }
    if (source == KeySource::pointer)
        pointer_key_.reset();
    update();
}

void KeyboardWidget::set_view_start(int pitch) {
    view_start_ = std::clamp(pitch, 0, 127 - 12);
    while (black_key(view_start_))
        --view_start_;
    update();
}

void KeyboardWidget::set_octave_offset(int offset) {
    octave_offset_ = offset;
    update();
}

double KeyboardWidget::white_key_width() const {
    return std::max(10.0, static_cast<double>(width()) / visible_white_keys_);
}

KeyboardWidget::KeyGeometry KeyboardWidget::key_geometry(int pitch) const {
    const double white = white_key_width();
    const double origin = white_index(view_start_) * white;
    if (!black_key(pitch))
        return {QRectF(white_index(pitch) * white - origin, 0, white, height()), false};
    const double center = white_index(pitch + 1) * white - origin; // boundary with the next white key
    return {QRectF(center - white * 0.32, 0, white * 0.64, height() * 0.62), true};
}

std::optional<int> KeyboardWidget::pitch_at(QPointF point) const {
    // Black keys sit on top of white keys.
    const int first = std::max(0, view_start_ - 1);
    const int last = std::min(127, view_start_ + visible_white_keys_ * 2 + 2);
    for (int pitch = first; pitch <= last; ++pitch)
        if (black_key(pitch) && key_geometry(pitch).rect.contains(point))
            return pitch;
    for (int pitch = first; pitch <= last; ++pitch)
        if (!black_key(pitch) && key_geometry(pitch).rect.contains(point))
            return pitch;
    return std::nullopt;
}

int KeyboardWidget::velocity_for(QPointF point, int pitch) const {
    if (fixed_velocity_)
        return *fixed_velocity_;
    // Position-sensitive strike: nearer the front edge (bottom) plays louder.
    const auto rect = key_geometry(pitch).rect;
    const double fraction = std::clamp((point.y() - rect.top()) / std::max(1.0, rect.height()), 0.0, 1.0);
    return std::clamp(static_cast<int>(20 + fraction * 107), 1, 127);
}

void KeyboardWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const int last = std::min(127, view_start_ + visible_white_keys_ * 2 + 2);
    const auto paint_key = [&](int pitch) {
        const auto geometry = key_geometry(pitch);
        const auto owners = owners_[static_cast<std::size_t>(pitch)];
        const bool in_scale = (scale_mask_ >> (pitch % 12)) & 1u;
        QColor fill = geometry.black ? QColor(35, 35, 38) : QColor(250, 250, 250);
        if (show_scale_ && !in_scale)
            fill = geometry.black ? QColor(70, 50, 50) : QColor(225, 210, 210);
        if (sustained_.contains(pitch))
            fill = QColor(150, 190, 255);
        if (owners != 0)
            fill = palette().color(QPalette::Highlight);
        painter.fillRect(geometry.rect, fill);
        painter.setPen(QColor(90, 90, 90));
        painter.drawRect(geometry.rect);
        if (show_scale_ && in_scale) {
            // Shape markers so scale members do not rely on color alone.
            const double size = std::min(8.0, geometry.rect.width() * 0.4);
            const QRectF marker(geometry.rect.center().x() - size / 2, geometry.rect.bottom() - size - 4, size, size);
            painter.setBrush(QColor(60, 140, 240));
            painter.setPen(Qt::NoPen);
            if (pitch % 12 == root_)
                painter.drawRect(marker);
            else
                painter.drawEllipse(marker);
            painter.setBrush(Qt::NoBrush);
        }
        if (!geometry.black && pitch % 12 == 0) {
            painter.setPen(Qt::black);
            painter.drawText(geometry.rect.adjusted(0, 0, 0, -14), Qt::AlignHCenter | Qt::AlignBottom,
                             QString::fromStdString(pitch_name(static_cast<std::uint8_t>(pitch), octave_offset_)));
        }
    };
    for (int pitch = view_start_; pitch <= last; ++pitch)
        if (!black_key(pitch))
            paint_key(pitch);
    for (int pitch = std::max(0, view_start_ - 1); pitch <= last; ++pitch)
        if (black_key(pitch))
            paint_key(pitch);
}

void KeyboardWidget::press_pointer_key(int pitch, int velocity) {
    pointer_key_ = pitch;
    set_key_state(pitch, KeySource::pointer, true);
    emit note_on(pitch, velocity);
}

void KeyboardWidget::release_pointer_key() {
    if (!pointer_key_)
        return;
    const int pitch = *pointer_key_;
    pointer_key_.reset();
    set_key_state(pitch, KeySource::pointer, false);
    emit note_off(pitch);
}

void KeyboardWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    if (const auto pitch = pitch_at(event->position()))
        press_pointer_key(*pitch, velocity_for(event->position(), *pitch));
}

void KeyboardWidget::mouseMoveEvent(QMouseEvent* event) {
    const auto pitch = pitch_at(event->position());
    if (pitch != hover_key_) {
        hover_key_ = pitch;
        if (pitch)
            setToolTip(QString("%1 (MIDI %2)")
                           .arg(QString::fromStdString(pitch_name(static_cast<std::uint8_t>(*pitch), octave_offset_)))
                           .arg(*pitch)); // name and number on hover (section 8.1)
    }
    if ((event->buttons() & Qt::LeftButton) && pitch && pointer_key_ && *pitch != *pointer_key_) {
        release_pointer_key(); // glissando: release the old key before starting the new one
        press_pointer_key(*pitch, velocity_for(event->position(), *pitch));
    }
}

void KeyboardWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        release_pointer_key();
}

void KeyboardWidget::wheelEvent(QWheelEvent* event) {
    const int steps = event->angleDelta().y() > 0 ? -7 : 7;
    set_view_start(view_start_ + steps);
    event->accept();
}

void KeyboardWidget::leaveEvent(QEvent* event) {
    hover_key_.reset();
    QWidget::leaveEvent(event);
}

bool KeyboardWidget::event(QEvent* event) {
    if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate)
        release_source(KeySource::pointer); // losing capture releases pointer-owned notes
    return QWidget::event(event);
}

// --- Computer keyboard ---------------------------------------------------------------------------

ComputerKeyboardMap ComputerKeyboardMap::defaults() {
    ComputerKeyboardMap map;
    const int keys[] = {Qt::Key_A, Qt::Key_W, Qt::Key_S, Qt::Key_E, Qt::Key_D, Qt::Key_F, Qt::Key_T,
                        Qt::Key_G, Qt::Key_Y, Qt::Key_H, Qt::Key_U, Qt::Key_J, Qt::Key_K};
    for (int semitone = 0; semitone < 13; ++semitone)
        map.semitone_by_key[keys[semitone]] = semitone;
    map.octave_key_down = Qt::Key_Z;
    map.octave_key_up = Qt::Key_X;
    map.velocity_key_down = Qt::Key_C;
    map.velocity_key_up = Qt::Key_V;
    return map;
}

ComputerKeyboardPlayer::ComputerKeyboardPlayer(QObject* parent) : QObject(parent) {}

void ComputerKeyboardPlayer::set_enabled(bool enabled) {
    if (enabled == enabled_)
        return;
    if (!enabled)
        release_all();
    enabled_ = enabled;
    emit state_changed(octave_root_, velocity_);
}

bool ComputerKeyboardPlayer::handle_key(QKeyEvent* event, bool pressed) {
    if (!enabled_ || (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))
        return false;
    const int key = event->key();
    if (pressed && event->isAutoRepeat())
        return map_.semitone_by_key.contains(key); // auto-repeat never retriggers notes
    if (key == map_.octave_key_down || key == map_.octave_key_up) {
        if (pressed) {
            octave_root_ = std::clamp(octave_root_ + (key == map_.octave_key_up ? 12 : -12), 0, 120);
            emit state_changed(octave_root_, velocity_);
        }
        return true;
    }
    if (key == map_.velocity_key_down || key == map_.velocity_key_up) {
        if (pressed) {
            velocity_ = std::clamp(velocity_ + (key == map_.velocity_key_up ? 10 : -10), 1, 127);
            emit state_changed(octave_root_, velocity_);
        }
        return true;
    }
    const auto found = map_.semitone_by_key.find(key);
    if (found == map_.semitone_by_key.end())
        return false;
    if (pressed) {
        if (held_.contains(key))
            return true;
        const int pitch = octave_root_ + found->second;
        if (pitch < 0 || pitch > 127)
            return true;
        held_[key] = pitch; // release uses the note captured here
        emit note_on(pitch, velocity_);
    } else if (event->isAutoRepeat()) {
        return true;
    } else if (auto held = held_.find(key); held != held_.end()) {
        emit note_off(held->second);
        held_.erase(held);
    }
    return true;
}

void ComputerKeyboardPlayer::release_all() {
    for (const auto& [key, pitch] : held_)
        emit note_off(pitch);
    held_.clear();
}

} // namespace mwb::ui
