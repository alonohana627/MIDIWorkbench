#include "mwb/ui/icons.hpp"

#include "mwb/ui/theme.hpp"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace mwb::ui {

namespace {

/// Draws one icon in a 24x24 design grid, scaled to the pixmap.
void paint_icon(QPainter& painter, IconId id, const QColor& ink) {
    QPen pen(ink, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const auto fill = [&](const QPainterPath& path, const QColor& color) {
        painter.save();
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPath(path);
        painter.restore();
    };
    switch (id) {
    case IconId::new_document: {
        QPainterPath page;
        page.moveTo(6, 3);
        page.lineTo(14, 3);
        page.lineTo(19, 8);
        page.lineTo(19, 21);
        page.lineTo(6, 21);
        page.closeSubpath();
        painter.drawPath(page);
        painter.drawLine(QPointF(14, 3), QPointF(14, 8));
        painter.drawLine(QPointF(14, 8), QPointF(19, 8));
        break;
    }
    case IconId::open: {
        QPainterPath folder;
        folder.moveTo(3, 6);
        folder.lineTo(9, 6);
        folder.lineTo(11, 8);
        folder.lineTo(20, 8);
        folder.lineTo(20, 19);
        folder.lineTo(3, 19);
        folder.closeSubpath();
        painter.drawPath(folder);
        break;
    }
    case IconId::save:
        painter.drawRoundedRect(QRectF(4, 4, 16, 16), 2, 2);
        painter.drawRect(QRectF(8, 4, 8, 5));
        painter.drawRect(QRectF(7, 13, 10, 7));
        break;
    case IconId::undo:
    case IconId::redo: {
        painter.save();
        if (id == IconId::redo) {
            painter.translate(24, 0);
            painter.scale(-1, 1);
        }
        QPainterPath arc;
        arc.moveTo(5, 10);
        arc.lineTo(15, 10);
        arc.cubicTo(21, 10, 21, 19, 15, 19);
        arc.lineTo(9, 19);
        painter.drawPath(arc);
        painter.drawLine(QPointF(5, 10), QPointF(9, 6));
        painter.drawLine(QPointF(5, 10), QPointF(9, 14));
        painter.restore();
        break;
    }
    case IconId::select: {
        QPainterPath arrow;
        arrow.moveTo(6, 3);
        arrow.lineTo(6, 19);
        arrow.lineTo(10, 15);
        arrow.lineTo(13, 21);
        arrow.lineTo(15.5, 20);
        arrow.lineTo(12.5, 14);
        arrow.lineTo(18, 14);
        arrow.closeSubpath();
        painter.drawPath(arrow);
        break;
    }
    case IconId::draw: {
        QPainterPath pencil;
        pencil.moveTo(4, 20);
        pencil.lineTo(5, 15);
        pencil.lineTo(16, 4);
        pencil.lineTo(20, 8);
        pencil.lineTo(9, 19);
        pencil.closeSubpath();
        painter.drawPath(pencil);
        painter.drawLine(QPointF(13.5, 6.5), QPointF(17.5, 10.5));
        break;
    }
    case IconId::erase: {
        painter.save();
        painter.translate(12, 12);
        painter.rotate(-40);
        painter.drawRoundedRect(QRectF(-8, -5, 16, 10), 2, 2);
        painter.drawLine(QPointF(-1, -5), QPointF(-1, 5));
        painter.restore();
        painter.drawLine(QPointF(5, 21), QPointF(19, 21));
        break;
    }
    case IconId::split:
        painter.drawRoundedRect(QRectF(3, 8, 7, 8), 1.5, 1.5);
        painter.drawRoundedRect(QRectF(14, 8, 7, 8), 1.5, 1.5);
        pen.setStyle(Qt::DashLine);
        pen.setWidthF(1.5);
        painter.setPen(pen);
        painter.drawLine(QPointF(12, 3), QPointF(12, 21));
        break;
    case IconId::audition: {
        QPainterPath speaker;
        speaker.moveTo(4, 9);
        speaker.lineTo(8, 9);
        speaker.lineTo(13, 4);
        speaker.lineTo(13, 20);
        speaker.lineTo(8, 15);
        speaker.lineTo(4, 15);
        speaker.closeSubpath();
        painter.drawPath(speaker);
        painter.drawArc(QRectF(12, 8, 6, 8), -60 * 16, 120 * 16);
        painter.drawArc(QRectF(12, 5, 10, 14), -60 * 16, 120 * 16);
        break;
    }
    case IconId::play: {
        QPainterPath triangle;
        triangle.moveTo(7, 4);
        triangle.lineTo(20, 12);
        triangle.lineTo(7, 20);
        triangle.closeSubpath();
        fill(triangle, ink);
        break;
    }
    case IconId::stop: {
        QPainterPath square;
        square.addRoundedRect(QRectF(6, 6, 12, 12), 1.5, 1.5);
        fill(square, ink);
        break;
    }
    case IconId::rewind: {
        QPainterPath shape;
        shape.addRect(QRectF(5, 5, 2.5, 14));
        shape.moveTo(19, 5);
        shape.lineTo(9, 12);
        shape.lineTo(19, 19);
        shape.closeSubpath();
        fill(shape, ink);
        break;
    }
    case IconId::record: {
        QPainterPath dot;
        dot.addEllipse(QPointF(12, 12), 7, 7);
        fill(dot, danger_color());
        break;
    }
    case IconId::loop: {
        painter.drawArc(QRectF(4, 6, 16, 12), 30 * 16, 150 * 16);
        painter.drawArc(QRectF(4, 6, 16, 12), 210 * 16, 150 * 16);
        painter.drawLine(QPointF(18.9, 9), QPointF(19.5, 5.5));
        painter.drawLine(QPointF(18.9, 9), QPointF(15.5, 8.5));
        painter.drawLine(QPointF(5.1, 15), QPointF(4.5, 18.5));
        painter.drawLine(QPointF(5.1, 15), QPointF(8.5, 15.5));
        break;
    }
    case IconId::metronome: {
        QPainterPath body;
        body.moveTo(9, 3);
        body.lineTo(15, 3);
        body.lineTo(19, 21);
        body.lineTo(5, 21);
        body.closeSubpath();
        painter.drawPath(body);
        painter.drawLine(QPointF(12, 17), QPointF(17, 7));
        painter.drawLine(QPointF(7, 17), QPointF(17, 17));
        break;
    }
    case IconId::library:
        painter.drawRect(QRectF(4, 5, 4, 15));
        painter.drawRect(QRectF(10, 5, 4, 15));
        painter.save();
        painter.translate(17, 5);
        painter.rotate(15);
        painter.drawRect(QRectF(0, 0, 4, 15));
        painter.restore();
        break;
    case IconId::inspector:
        for (int row = 0; row < 3; ++row) {
            const double y = 6 + row * 6;
            painter.drawLine(QPointF(4, y), QPointF(20, y));
            const double x = row == 0 ? 15 : row == 1 ? 8 : 13;
            QPainterPath knob;
            knob.addEllipse(QPointF(x, y), 2.5, 2.5);
            fill(knob, ink);
        }
        break;
    case IconId::add:
        painter.drawLine(QPointF(12, 5), QPointF(12, 19));
        painter.drawLine(QPointF(5, 12), QPointF(19, 12));
        break;
    case IconId::remove:
        painter.drawLine(QPointF(5, 12), QPointF(19, 12));
        break;
    case IconId::duplicate:
        painter.drawRoundedRect(QRectF(4, 8, 11, 12), 1.5, 1.5);
        painter.drawRoundedRect(QRectF(9, 4, 11, 12), 1.5, 1.5);
        break;
    case IconId::pause: {
        QPainterPath bars;
        bars.addRoundedRect(QRectF(6, 5, 4, 14), 1, 1);
        bars.addRoundedRect(QRectF(14, 5, 4, 14), 1, 1);
        fill(bars, ink);
        break;
    }
    case IconId::close:
        pen.setWidthF(1.8);
        painter.setPen(pen);
        painter.drawLine(QPointF(7, 7), QPointF(17, 17));
        painter.drawLine(QPointF(17, 7), QPointF(7, 17));
        break;
    case IconId::up:
    case IconId::down: {
        painter.save();
        if (id == IconId::down) {
            painter.translate(0, 24);
            painter.scale(1, -1);
        }
        painter.drawLine(QPointF(12, 19), QPointF(12, 5));
        painter.drawLine(QPointF(12, 5), QPointF(6, 11));
        painter.drawLine(QPointF(12, 5), QPointF(18, 11));
        painter.restore();
        break;
    }
    }
}

QPixmap render(IconId id, int size, const QColor& ink, qreal ratio) {
    QPixmap pixmap(QSize(size, size) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(size / 24.0, size / 24.0);
    paint_icon(painter, id, ink);
    return pixmap;
}

} // namespace

QIcon make_icon(IconId id) {
    const QPalette palette = QApplication::palette();
    const qreal ratio = qApp ? qApp->devicePixelRatio() : 1.0;
    QIcon icon;
    for (const int size : {16, 20, 24, 32}) {
        icon.addPixmap(render(id, size, palette.color(QPalette::WindowText), ratio), QIcon::Normal, QIcon::Off);
        icon.addPixmap(render(id, size, palette.color(QPalette::HighlightedText), ratio), QIcon::Normal, QIcon::On);
        icon.addPixmap(render(id, size, palette.color(QPalette::Disabled, QPalette::WindowText), ratio), QIcon::Disabled,
                       QIcon::Off);
    }
    return icon;
}

} // namespace mwb::ui
