#include "mwb/ui/theme.hpp"

#include "mwb/ui/icons.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QStyleOption>
#include <QTabBar>

#include <algorithm>

namespace mwb::ui {

namespace {

Theme g_theme = Theme::system;
Theme g_resolved = Theme::dark;
bool g_follow_connected = false;

Theme resolve(Theme theme) {
    if (theme != Theme::system)
        return theme;
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Light ? Theme::light : Theme::dark;
}

struct PaletteSpec {
    const char* window;
    const char* text;
    const char* base;
    const char* alternate_base;
    const char* button;
    const char* mid;
    const char* midlight;
    const char* dark;
    const char* highlight;
    const char* highlighted_text;
    const char* placeholder;
    const char* disabled_text;
    const char* tooltip_base;
    const char* tooltip_text;
    const char* link;
};

constexpr PaletteSpec dark_spec{"#1E1F22", "#E4E5E8", "#17181B", "#202226", "#2A2C31", "#3A3D43", "#2F3237", "#111214",
                                "#2F6FDB", "#FFFFFF", "#8B8E96", "#6B6E75", "#2A2C31", "#E4E5E8", "#6AA6FF"};
constexpr PaletteSpec light_spec{"#F3F4F6", "#1B1C1F", "#FFFFFF", "#F7F8FA", "#FBFBFC", "#C9CCD2", "#E3E5E9", "#A0A4AC",
                                 "#2563EB", "#FFFFFF", "#6B6F78", "#A3A6AD", "#1B1C1F", "#FFFFFF", "#1D4ED8"};
constexpr PaletteSpec contrast_spec{"#000000", "#FFFFFF", "#000000", "#0F0F0F", "#000000", "#FFFFFF", "#FFFFFF", "#FFFFFF",
                                    "#FFD400", "#000000", "#C0C0C0", "#8A8A8A", "#000000", "#FFFFFF", "#00FFFF"};

/// Boundary color of fields and check boxes: at least 3:1 against both the window and the field
/// fill (WCAG 1.4.11). Fusion's own outline is the window color darkened, about 1.1:1 in dark.
QColor control_edge() {
    switch (g_resolved) {
    case Theme::light: return QColor("#858A93"); // 3.15:1 on the window, 3.6:1 on white
    case Theme::high_contrast: return QColor("#FFFFFF");
    case Theme::system:
    case Theme::dark: break;
    }
    return QColor("#8B8E96"); // 4.6:1 on the dark window, 4.9:1 on the dark field
}

/// Fusion with check indicators and focus frames that stay visible in every theme. Indicators are
/// painted here (not with a style-sheet image) so no image resource is needed, and the checked
/// state is a glyph, readable on a selected (highlighted) row as well.
class WorkbenchStyle final : public QProxyStyle {
public:
    WorkbenchStyle() : QProxyStyle(QStyleFactory::create("Fusion")) {}

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget) const override {
        if (element == PE_IndicatorCheckBox || element == PE_IndicatorItemViewItemCheck) {
            draw_check_indicator(option, painter);
            return;
        }
        if (element == PE_FrameFocusRect) {
            draw_focus_frame(option, painter, widget);
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    void drawComplexControl(ComplexControl control, const QStyleOptionComplex* option, QPainter* painter,
                            const QWidget* widget) const override {
        QProxyStyle::drawComplexControl(control, option, painter, widget);
        // Spin boxes keep Fusion's arrow buttons (a style-sheet border would remove them) and get
        // the same 3:1 field edge as line edits and combos, drawn over Fusion's faint frame
        // (ux-visual-2). Focus keeps the highlight color.
        if (control == CC_SpinBox && g_resolved != Theme::high_contrast) {
            const auto* spin = qstyleoption_cast<const QStyleOptionSpinBox*>(option);
            if (!spin || !spin->frame)
                return;
            const bool focused = (option->state & State_HasFocus) != 0;
            const bool enabled = (option->state & State_Enabled) != 0;
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(QPen(focused ? option->palette.color(QPalette::Highlight)
                                         : enabled ? control_edge() : option->palette.color(QPalette::Disabled, QPalette::WindowText),
                                 1.0));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5), 2.0, 2.0);
            painter->restore();
        }
    }

    QIcon standardIcon(StandardPixmap icon, const QStyleOption* option, const QWidget* widget) const override {
        // Fusion's toolbar overflow glyph is a few grey pixels; users must see that more controls exist.
        if (icon == SP_ToolBarHorizontalExtensionButton || icon == SP_ToolBarVerticalExtensionButton)
            return make_icon(IconId::more);
        return QProxyStyle::standardIcon(icon, option, widget);
    }

private:
    static void draw_check_indicator(const QStyleOption* option, QPainter* painter) {
        const QPalette& palette = option->palette;
        const bool enabled = (option->state & State_Enabled) != 0;
        const bool on = (option->state & State_On) != 0;
        const bool partial = (option->state & State_NoChange) != 0;
        const bool high_contrast = g_resolved == Theme::high_contrast;
        const QRectF box = QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        QColor edge = enabled ? control_edge() : palette.color(QPalette::Disabled, QPalette::WindowText);
        QColor fill = enabled ? palette.color(QPalette::Base) : palette.color(QPalette::Window);
        QColor mark = palette.color(QPalette::HighlightedText);
        if ((on || partial) && enabled) {
            if (high_contrast) {
                fill = QColor("#000000"); // a yellow glyph on black reads on black and on yellow rows
                mark = QColor("#FFD400");
            } else {
                fill = palette.color(QPalette::Highlight);
                edge = (option->state & State_Selected) ? palette.color(QPalette::HighlightedText) : fill;
            }
        } else if (!enabled) {
            mark = palette.color(QPalette::Disabled, QPalette::WindowText);
        }
        painter->setPen(QPen(edge, high_contrast ? 2.0 : 1.0));
        painter->setBrush(fill);
        painter->drawRoundedRect(box, 2.5, 2.5);
        const QRectF r = box.adjusted(box.width() * 0.2, box.height() * 0.2, -box.width() * 0.2, -box.height() * 0.2);
        painter->setPen(QPen(mark, std::max(1.6, box.width() / 7.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        if (partial) {
            painter->drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        } else if (on) {
            QPainterPath check;
            check.moveTo(r.left(), r.top() + r.height() * 0.55);
            check.lineTo(r.left() + r.width() * 0.38, r.bottom());
            check.lineTo(r.right(), r.top() + r.height() * 0.08);
            painter->drawPath(check);
        }
        painter->restore();
    }

    static void draw_focus_frame(const QStyleOption* option, QPainter* painter, const QWidget* widget) {
        // Highlight against the window: 3.5:1 dark, 4.7:1 light, 14.7:1 high contrast. On a
        // highlighted background (a selected item) the highlighted text color is used instead.
        QColor color = option->palette.color(QPalette::Highlight);
        if (const auto* focus = qstyleoption_cast<const QStyleOptionFocusRect*>(option);
            focus && focus->backgroundColor == color)
            color = option->palette.color(QPalette::HighlightedText);
        // Item views and tab bars frame text tightly: a 1px frame there, 2px around controls.
        const bool thin = qobject_cast<const QAbstractItemView*>(widget) != nullptr || qobject_cast<const QTabBar*>(widget) != nullptr;
        const qreal width = thin ? 1.0 : 2.0;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(color, width));
        painter->setBrush(Qt::NoBrush);
        const qreal inset = width / 2.0;
        painter->drawRoundedRect(QRectF(option->rect).adjusted(inset, inset, -inset, -inset), 3, 3);
        painter->restore();
    }
};

} // namespace

QString theme_name(Theme theme) {
    switch (theme) {
    case Theme::system: return "system";
    case Theme::dark: return "dark";
    case Theme::light: return "light";
    case Theme::high_contrast: return "high_contrast";
    }
    return "system";
}

Theme theme_from_name(const QString& name) {
    if (name == "dark")
        return Theme::dark;
    if (name == "light")
        return Theme::light;
    if (name == "high_contrast")
        return Theme::high_contrast;
    return Theme::system;
}

void apply_base_font(QApplication& application) {
    QFont font = QApplication::font();
    font.setFamilies({"Segoe UI Variable Text", "Segoe UI", "Tahoma", "Noto Sans", "DejaVu Sans"});
    const qreal size = font.pointSizeF() > 0 ? font.pointSizeF() : 9.0;
    font.setPointSizeF(size + 1.0); // a little larger than the platform default, not a lot
    application.setFont(font);
}

QPalette make_palette(Theme theme) {
    const PaletteSpec& spec = resolve(theme) == Theme::light           ? light_spec
                              : resolve(theme) == Theme::high_contrast ? contrast_spec
                                                                       : dark_spec;
    QPalette palette;
    const auto set = [&](QPalette::ColorRole role, const char* color) { palette.setColor(role, QColor(color)); };
    set(QPalette::Window, spec.window);
    set(QPalette::WindowText, spec.text);
    set(QPalette::Text, spec.text);
    set(QPalette::ButtonText, spec.text);
    set(QPalette::BrightText, spec.text);
    set(QPalette::Base, spec.base);
    set(QPalette::AlternateBase, spec.alternate_base);
    set(QPalette::Button, spec.button);
    set(QPalette::Mid, spec.mid);
    set(QPalette::Midlight, spec.midlight);
    set(QPalette::Dark, spec.dark);
    set(QPalette::Light, spec.midlight);
    set(QPalette::Shadow, spec.dark);
    set(QPalette::Highlight, spec.highlight);
    set(QPalette::HighlightedText, spec.highlighted_text);
    set(QPalette::PlaceholderText, spec.placeholder);
    set(QPalette::ToolTipBase, spec.tooltip_base);
    set(QPalette::ToolTipText, spec.tooltip_text);
    set(QPalette::Link, spec.link);
    set(QPalette::LinkVisited, spec.link);
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, QColor(spec.disabled_text));
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(spec.mid));
    return palette;
}

QString base_stylesheet() {
    return QStringLiteral(
        "QToolBar { border: 0; spacing: 2px; padding: 3px 6px; }"
        "QToolBar::separator { width: 1px; margin: 4px 6px; background: palette(mid); }"
        "QToolBarExtension { min-width: 20px; }"
        // A transparent 2px border is reserved so the focus border does not shift the icon.
        "QToolButton { padding: 2px; border: 2px solid transparent; border-radius: 4px; }"
        "QToolButton:focus { border-color: palette(highlight); }"
        "QToolButton:checked { background: palette(highlight); color: palette(highlighted-text); }"
        "QToolButton:checked:focus { border-color: palette(highlighted-text); }"
        "QToolButton:checked:disabled { background: transparent; color: palette(placeholder-text); }"
        "QToolButton#sectionHeader, QToolButton#sectionHeader:checked { background: transparent; color: palette(window-text);"
        " text-align: left; padding: 2px 0px; }"
        "QToolButton#sectionHeader:focus { border-color: palette(highlight); }"
        "QDockWidget::title { padding: 6px 8px; background: palette(window); }"
        "QStatusBar { border-top: 1px solid palette(mid); }"
        "QStatusBar::item { border: 0; }"
        "QGroupBox { font-weight: 600; margin-top: 14px; }"
        "QGroupBox#sectionBody { margin-top: 0; }" // the body under a folding section header
        "QTabBar::tab { padding: 6px 14px; }"
        "QLineEdit:focus, QAbstractSpinBox:focus, QComboBox:focus, QTableView:focus, QListView:focus, QTreeView:focus"
        " { border: 2px solid palette(highlight); border-radius: 3px; }"
        "QLabel#transport\\.position { padding: 2px 8px; border: 1px solid palette(mid); border-radius: 3px; }");
}

namespace {

/// Field boundaries for dark and light (WCAG 1.4.11: 3:1). Fusion draws them in the window color
/// darkened, which is about 1.1:1 in dark and 2.1:1 in light. Combo arrows stay Fusion's.
QString field_stylesheet(const QString& edge) {
    // Spin boxes keep Fusion's frame: a style-sheet border there drops Fusion's arrow buttons.
    return QStringLiteral(
               "QLineEdit, QComboBox { border: 1px solid %1; border-radius: 3px; padding: 2px 4px; }"
               "QLineEdit, QComboBox:editable { background: palette(base); }"
               "QComboBox:!editable { background: palette(button); }"
               "QLineEdit:focus, QComboBox:focus { padding: 1px 3px; }")
        .arg(edge);
}

} // namespace

void apply_theme(Theme theme) {
    g_theme = theme;
    g_resolved = resolve(theme);
    QApplication::setStyle(new WorkbenchStyle); // the application owns the style
    QApplication::setPalette(make_palette(g_resolved));
    const QString theme_rules =
        g_resolved == Theme::high_contrast
            ? QStringLiteral(
                  // Fusion draws these outlines too faintly on pure black. Check boxes are painted
                  // by WorkbenchStyle (white frame, yellow check on black).
                  "QLineEdit, QAbstractSpinBox, QComboBox, QPushButton { border: 1px solid #FFFFFF; }"
                  "QPushButton { padding: 3px 10px; }"
                  "QPushButton:focus { border: 2px solid #FFD400; padding: 2px 9px; }"
                  "QScrollBar::handle { background: #FFFFFF; border: 1px solid #000000;"
                  " min-width: 24px; min-height: 24px; }"
                  "QScrollBar { background: #000000; border: 1px solid #FFFFFF; }"
                  "QSplitter::handle { background: #FFFFFF; }"
                  "QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { width: 16px;"
                  " background: #000000; border-left: 1px solid #FFFFFF; }"
                  "QAbstractSpinBox::up-arrow { width: 0; height: 0; border-left: 4px solid transparent;"
                  " border-right: 4px solid transparent; border-bottom: 5px solid #FFFFFF; }"
                  "QAbstractSpinBox::down-arrow { width: 0; height: 0; border-left: 4px solid transparent;"
                  " border-right: 4px solid transparent; border-top: 5px solid #FFFFFF; }"
                  "QLineEdit, QAbstractSpinBox, QComboBox { padding: 1px 4px; }"
                  "QLineEdit:focus, QAbstractSpinBox:focus, QComboBox:focus { border: 2px solid #FFD400; padding: 0px 3px; }"
                  // Fusion frames menus and tooltips in the window color darkened: black on black.
                  "QMenu { border: 1px solid #FFFFFF; }"
                  "QMenu::separator { height: 1px; background: #FFFFFF; margin: 4px 8px; }"
                  "QToolTip { border: 1px solid #FFFFFF; }"
                  "QMainWindow::separator { background: #FFFFFF; width: 2px; height: 2px; }")
            : field_stylesheet(control_edge().name());
    if (auto* application = qobject_cast<QApplication*>(QCoreApplication::instance()))
        application->setStyleSheet(base_stylesheet() + theme_rules);
    if (!g_follow_connected) {
        g_follow_connected = true;
        // Icons are painted on demand from the palette (icons.cpp), so a Windows light/dark switch
        // only needs the palette and style sheet re-applied; widgets repaint on the palette change.
        QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, qApp, [] {
            if (g_theme == Theme::system)
                apply_theme(Theme::system);
        });
    }
}

Theme current_theme() { return g_theme; }

Theme resolved_theme() { return g_resolved; }

QFont mono_font() {
    QFont font = QApplication::font();
    font.setFamilies({"Cascadia Mono", "Consolas", "DejaVu Sans Mono", "Courier New"});
    font.setStyleHint(QFont::Monospace);
    return font;
}

QColor danger_color() {
    if (g_resolved == Theme::light)
        return QColor("#D92D20");
    if (g_resolved == Theme::high_contrast)
        return QColor("#FF5050");
    return QColor("#F0524B");
}

QColor warning_color() {
    if (g_resolved == Theme::light)
        return QColor("#B45309");
    if (g_resolved == Theme::high_contrast)
        return QColor("#FFB000");
    return QColor("#F59E0B");
}

QColor region_color() {
    QColor color = g_resolved == Theme::light           ? QColor("#7C3AED")
                   : g_resolved == Theme::high_contrast ? QColor("#FF66FF")
                                                        : QColor("#A78BFA");
    color.setAlpha(g_resolved == Theme::high_contrast ? 200 : 140);
    return color;
}

RollColors roll_colors() {
    RollColors colors;
    const QColor accent = QApplication::palette().color(QPalette::Highlight);
    if (g_resolved == Theme::light) {
        colors = RollColors{QColor("#FFFFFF"), QColor("#F1F2F5"), QColor("#C3C7CE"), QColor("#EDEEF1"), QColor("#D9DCE1"),
                            QColor("#9AA0AA"), QColor("#7D838E"), accent, accent, QColor("#2E9E6B"), QColor("#6B6F78"),
                            danger_color(), QColor("#FFFFFF"), QColor("#2B2D31"), QColor("#1B1C1F"), QColor("#F3F4F6"),
                            QColor("#1B1C1F"), QColor("#6B6F78"), QColor("#1B1C1F"), QColor("#FFFFFF"), false, false};
        colors.root_tint.setAlpha(46);
        colors.member_tint.setAlpha(18);
    } else if (g_resolved == Theme::high_contrast) {
        colors = RollColors{QColor("#000000"), QColor("#1A1A1A"), QColor("#FFFFFF"), QColor("#303030"), QColor("#707070"),
                            QColor("#FFFFFF"), QColor("#FFFFFF"), accent, accent, QColor("#00FF7F"), QColor("#FFFFFF"),
                            danger_color(), QColor("#FFFFFF"), QColor("#000000"), QColor("#000000"), QColor("#000000"),
                            QColor("#FFFFFF"), QColor("#C0C0C0"), QColor("#FFD400"), QColor("#000000"), true, true};
        colors.root_tint.setAlpha(80);
        colors.member_tint.setAlpha(36);
    } else {
        colors = RollColors{QColor("#23252A"), QColor("#17181B"), QColor("#4A4E56"), QColor("#24262A"), QColor("#30333A"),
                            QColor("#50555E"), QColor("#6A707B"), accent, accent, QColor("#3FB37F"), QColor("#9AA0AA"),
                            danger_color(), QColor("#C9CBD0"), QColor("#2B2D31"), QColor("#1B1C1F"), QColor("#1E1F22"),
                            QColor("#C9CBD0"), QColor("#8B8E96"), QColor("#FFFFFF"), QColor("#17181B"), true, false};
        colors.root_tint.setAlpha(56);
        colors.member_tint.setAlpha(22);
    }
    return colors;
}

QColor note_fill(const QColor& base, int velocity) {
    const double v = std::clamp(velocity, 1, 127) / 127.0;
    const bool dark = g_resolved != Theme::light;
    // Dark floor 0.46: even velocity 1 stays at 3:1 against the grid rows (quiet ghost notes).
    double lightness = dark ? 0.46 + 0.24 * v : 0.62 - 0.22 * v;
    if (g_resolved == Theme::high_contrast)
        lightness = 0.70 + 0.10 * v; // bright fills with black labels on the black grid
    QColor color = base.toHsl();
    const float hue = color.hslHueF() < 0 ? 0.6f : color.hslHueF();
    return QColor::fromHslF(hue, 0.65f, static_cast<float>(lightness));
}

} // namespace mwb::ui
