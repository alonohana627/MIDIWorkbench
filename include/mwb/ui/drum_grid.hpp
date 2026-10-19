#pragma once

#include "mwb/core/time.hpp"

#include <QWidget>

#include <optional>
#include <set>
#include <vector>

namespace mwb::ui {

class DocumentController;

/// Conventional General MIDI percussion name for a pitch (35..81), or empty.
[[nodiscard]] QString gm_drum_name(int pitch);

/// Drum grid: a second view of the same note events (section 11.1). Named pitch rows, step
/// cells at the snap grid, velocity accents and ambiguous-cell markers when several or
/// off-grid notes share a cell. Clicking a step toggles a note of the configured length on the
/// active track as one command. Rows are renamed/reordered only for display, never transposed.
class DrumGridView : public QWidget {
    Q_OBJECT

public:
    explicit DrumGridView(DocumentController& controller, QWidget* parent = nullptr);

    /// Steps shown per bar view origin and step length come from the sequence snap grid.
    void set_origin(Tick tick);
    /// Velocity used for new steps; Shift-click uses the accent velocity.
    void set_velocities(int normal, int accent);
    [[nodiscard]] QSize sizeHint() const override;

signals:
    void open_in_piano_roll(Tick tick, int pitch);
    void status_message(const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;

private:
    [[nodiscard]] std::vector<int> rows() const;
    /// Toggles the step at (pitch, column): clears one note, adds one, or opens an ambiguous cell.
    void toggle_cell(int pitch, std::int64_t column, bool accent);
    [[nodiscard]] Tick step_ticks() const;
    [[nodiscard]] std::optional<std::pair<int, std::int64_t>> cell_at(QPointF point) const;

    DocumentController& controller_;
    Tick origin_ = 0;
    /// Keyboard cursor: row index into rows() and visible column.
    int cursor_row_ = 0;
    /// Document generation right after a click toggled a cell (undone by a double-click).
    std::optional<std::uint64_t> toggled_generation_;
    std::int64_t cursor_column_ = 0;
    int normal_velocity_ = 90;
    int accent_velocity_ = 120;
    static constexpr int name_width = 150;
    static constexpr int cell_width = 22;
    static constexpr int row_height = 20;
};

} // namespace mwb::ui
