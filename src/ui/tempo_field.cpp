#include "mwb/ui/tempo_field.hpp"

#include <QLineEdit>
#include <QMouseEvent>

#include <cstdlib>

namespace mwb::ui {

TempoField::TempoField(QWidget* parent) : QDoubleSpinBox(parent) {
    setRange(5.0, 999.0);
    setDecimals(1);
    setSingleStep(1.0);
    setSuffix(tr(" BPM"));
    setValue(120.0);
    setKeyboardTracking(false); // typing commits on Enter, not per keystroke
    setAccelerated(true);
    setAccessibleName(tr("Tempo"));
    setToolTip(tr("Song tempo at the play position. Type a value, use the arrows or the wheel, or drag up or down "
                  "(Shift: 0.1 BPM steps). Without tempo changes this sets the tempo of the whole song; with "
                  "tempo changes, the one in effect here. Meter, loop and all tempo settings are in the "
                  "Inspector's Song section."));
    idle_.setSingleShot(true);
    idle_.setInterval(idle_commit_ms);
    connect(&idle_, &QTimer::timeout, this, &TempoField::commit_now);
    connect(this, &QDoubleSpinBox::valueChanged, this, &TempoField::value_edited);
    connect(this, &QAbstractSpinBox::editingFinished, this, &TempoField::commit_now);
    lineEdit()->installEventFilter(this);
}

void TempoField::show_tempo(double bpm) {
    if (editing())
        return;
    syncing_ = true;
    setValue(bpm);
    syncing_ = false;
}

bool TempoField::editing() const {
    return pending_ || dragging_ || pressed_ || (hasFocus() && lineEdit()->isModified());
}

void TempoField::commit_now() {
    idle_.stop();
    if (!pending_ || dragging_)
        return;
    pending_ = false;
    emit tempo_committed(value());
}

void TempoField::value_edited() {
    if (syncing_)
        return;
    pending_ = true;
    if (!dragging_)
        idle_.start(); // wheel and arrow steps: one edit per burst
}

bool TempoField::eventFilter(QObject* watched, QEvent* event) {
    if (watched != lineEdit())
        return QDoubleSpinBox::eventFilter(watched, event);
    const auto type = event->type();
    if (type == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && isEnabled()) {
            pressed_ = true;
            press_y_ = qRound(mouse->position().y());
            press_value_ = value();
        }
        // The press still reaches the text, so a click places the cursor as usual.
    } else if (type == QEvent::MouseMove) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const int rise = press_y_ - qRound(mouse->position().y()); // up increases the tempo
        if (pressed_ && (mouse->buttons() & Qt::LeftButton) && (dragging_ || std::abs(rise) >= drag_pixels_per_step)) {
            if (!dragging_) {
                dragging_ = true;
                lineEdit()->deselect();
                lineEdit()->setCursor(Qt::SizeVerCursor);
            }
            const double step = (mouse->modifiers() & Qt::ShiftModifier) ? 0.1 : 1.0;
            setValue(press_value_ + step * (rise / drag_pixels_per_step));
            return true; // no text selection while dragging
        }
    } else if (type == QEvent::MouseButtonRelease) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            pressed_ = false;
            if (dragging_) {
                dragging_ = false;
                lineEdit()->unsetCursor();
                commit_now(); // a drag is one edit
                return true;
            }
        }
    }
    return QDoubleSpinBox::eventFilter(watched, event);
}

} // namespace mwb::ui
