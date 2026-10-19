#include "mwb/ui/theme.hpp"

#include <QApplication>
#include <QGuiApplication>
#include <QStyleFactory>
#include <QStyleHints>

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
        "QToolButton { padding: 4px; border-radius: 4px; }"
        "QToolButton:checked { background: palette(highlight); color: palette(highlighted-text); }"
        "QDockWidget::title { padding: 6px 8px; background: palette(window); }"
        "QStatusBar { border-top: 1px solid palette(mid); }"
        "QStatusBar::item { border: 0; }"
        "QGroupBox { font-weight: 600; margin-top: 14px; }"
        "QTabBar::tab { padding: 6px 14px; }"
        "QLineEdit:focus, QAbstractSpinBox:focus, QComboBox:focus, QTableView:focus, QListView:focus"
        " { border: 2px solid palette(highlight); border-radius: 3px; }"
        "QLabel#transport\\.position { padding: 2px 8px; border: 1px solid palette(mid); border-radius: 3px; }");
}

void apply_theme(Theme theme) {
    g_theme = theme;
    g_resolved = resolve(theme);
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QApplication::setPalette(make_palette(g_resolved));
    if (auto* application = qobject_cast<QApplication*>(QCoreApplication::instance()))
        application->setStyleSheet(base_stylesheet() +
                                   (g_resolved == Theme::high_contrast
                                        ? QStringLiteral(
                                              // Fusion draws these outlines too faintly on pure black.
                                              "QCheckBox::indicator, QTableView::indicator { width: 13px; height: 13px;"
                                              " border: 2px solid #FFFFFF; background: #000000; }"
                                              "QCheckBox::indicator:checked, QTableView::indicator:checked { background: #FFD400; }"
                                              "QLineEdit, QAbstractSpinBox, QComboBox, QPushButton { border: 1px solid #FFFFFF; }")
                                        : QString()));
    if (!g_follow_connected) {
        g_follow_connected = true;
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
        colors = RollColors{QColor("#000000"), QColor("#0A0A0A"), QColor("#FFFFFF"), QColor("#303030"), QColor("#707070"),
                            QColor("#FFFFFF"), QColor("#FFFFFF"), accent, accent, QColor("#00FF7F"), QColor("#FFFFFF"),
                            danger_color(), QColor("#FFFFFF"), QColor("#000000"), QColor("#000000"), QColor("#000000"),
                            QColor("#FFFFFF"), QColor("#C0C0C0"), QColor("#FFD400"), QColor("#000000"), true, true};
        colors.root_tint.setAlpha(80);
        colors.member_tint.setAlpha(36);
    } else {
        colors = RollColors{QColor("#1D1F23"), QColor("#18191C"), QColor("#3A3D43"), QColor("#24262A"), QColor("#30333A"),
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
    double lightness = dark ? 0.38 + 0.30 * v : 0.62 - 0.22 * v;
    if (g_resolved == Theme::high_contrast)
        lightness = 0.70 + 0.10 * v; // bright fills with black labels on the black grid
    QColor color = base.toHsl();
    const float hue = color.hslHueF() < 0 ? 0.6f : color.hslHueF();
    return QColor::fromHslF(hue, 0.65f, static_cast<float>(lightness));
}

} // namespace mwb::ui
