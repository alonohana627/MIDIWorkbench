#pragma once

#include <QColor>
#include <QFont>
#include <QPalette>
#include <QString>

class QApplication;

namespace mwb::ui {

/// Application themes (section 29.3: dark, light and high contrast). `system` follows Windows.
enum class Theme { system, dark, light, high_contrast };

[[nodiscard]] QString theme_name(Theme theme);
[[nodiscard]] Theme theme_from_name(const QString& name);

/// Sets the base font: Segoe UI family list and one point larger than the platform default,
/// plus `size_step` points (the View > Font Size choice), so every widget inherits it. Can be
/// called again at run time; the platform size is read only the first time.
void apply_base_font(QApplication& application, int size_step = 0);
/// The font size step in effect (0 = normal).
[[nodiscard]] int font_size_step();

/// Applies Fusion, the theme palette and the small shared stylesheet; remembers the theme so
/// custom-painted widgets can read roll_colors(). `system` resolves to dark or light and follows
/// later Windows color-scheme changes.
void apply_theme(Theme theme);
[[nodiscard]] Theme current_theme();
/// The concrete theme in effect (never `system`).
[[nodiscard]] Theme resolved_theme();

[[nodiscard]] QPalette make_palette(Theme theme);
[[nodiscard]] QString base_stylesheet();
/// Monospace font for position and cursor readouts.
[[nodiscard]] QFont mono_font();
/// Color for destructive or warning accents (record, errors, clipping).
[[nodiscard]] QColor danger_color();
/// Color for cautions that are not errors (out-of-scale notes, unsaved warnings); 3:1 or better on
/// the theme's grid and window.
[[nodiscard]] QColor warning_color();
/// Translucent color for regions (markers, loop and selection ranges) on the ruler and grid.
[[nodiscard]] QColor region_color();

/// Every color the piano roll, keyboard gutter, ruler and lanes paint with, for the active theme.
struct RollColors {
    QColor white_row;
    QColor black_row;
    QColor c_line;
    QColor snap;
    QColor beat;
    QColor bar;
    QColor bar_strong;
    QColor root_tint;
    QColor member_tint;
    QColor loop;
    QColor cursor;
    QColor playhead;
    QColor key_white;
    QColor key_black;
    QColor key_label;
    QColor ruler_background;
    QColor ruler_text;
    QColor hint_text;
    QColor selected_outline;
    QColor selected_inner;
    bool dark = true;
    bool high_contrast = false;
};

[[nodiscard]] RollColors roll_colors();

/// Note fill from a hue source color and a velocity (velocity drives lightness).
[[nodiscard]] QColor note_fill(const QColor& base, int velocity);

} // namespace mwb::ui
