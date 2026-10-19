#pragma once

#include "mwb/core/time.hpp"
#include "mwb/ui/editor_math.hpp"

#include <QWidget>

#include <optional>
#include <set>
#include <tuple>
#include <vector>

class QScrollBar;

namespace mwb::ui {

class DocumentController;

/// Conventional General MIDI percussion name for a pitch (35..81), or empty.
[[nodiscard]] QString gm_drum_name(int pitch);

/// Drum grid: a second view of the same note events (section 11.1). Named pitch rows, step
/// cells at the snap grid, velocity bars with accent marks and ambiguous-cell markers when several
/// or off-grid notes share a cell. Clicking a step toggles a note of the configured length on the
/// active track as one command; dragging along a row paints (or clears) every cell it crosses as
/// one command. Rows are renamed/reordered only for display, never transposed.
class DrumGridView : public QWidget {
    Q_OBJECT

public:
    explicit DrumGridView(DocumentController& controller, QWidget* parent = nullptr);

    /// Left edge of the grid (snapped to the step grid).
    void set_origin(Tick tick);
    [[nodiscard]] Tick origin() const { return origin_; }
    /// First visible row (rows scroll under the pinned bar-number header).
    void set_top_row(int row);
    [[nodiscard]] int top_row() const { return top_row_; }
    [[nodiscard]] int row_count() const;
    [[nodiscard]] int visible_row_count() const;
    [[nodiscard]] int visible_step_count() const;
    /// Playhead for display; while following, the grid pages so it stays visible.
    void set_playhead(std::optional<Tick> tick);
    [[nodiscard]] std::optional<Tick> playhead() const { return playhead_; }
    void set_follow_playhead(bool follow);
    [[nodiscard]] Grid step_grid() const;
    [[nodiscard]] QSize sizeHint() const override;

signals:
    void open_in_piano_roll(Tick tick, int pitch);
    /// A step was set: the hit is heard (ux-editing-21); the note-off follows 150 ms later.
    void audition_note_on(int pitch, int velocity);
    void audition_note_off(int pitch);
    void status_message(const QString& text);
    /// Origin, top row or size changed (scroll bars follow).
    void view_changed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

private:
    /// A press-drag along one row: set or clear every crossed cell, or toggle one hit's accent.
    struct PaintStroke {
        enum class Mode : std::uint8_t { set, clear, accent };
        int pitch = 0;
        Mode mode = Mode::set;
        bool accent = false;
        std::set<std::int64_t> steps; ///< Absolute step indices.
    };
    struct CellNotes {
        std::vector<EventId> notes;
        bool off_grid = false;
        int velocity = 0;
    };

    [[nodiscard]] const std::vector<int>& rows() const;
    [[nodiscard]] std::int64_t first_step() const;
    [[nodiscard]] std::int64_t step_at(Tick tick) const;
    [[nodiscard]] CellNotes notes_in_cell(int pitch, std::int64_t step) const;
    /// Applies a stroke as one command; returns true when the document changed.
    bool commit_stroke(const PaintStroke& stroke);
    /// Cell under a point as (pitch, absolute step index).
    [[nodiscard]] std::optional<std::pair<int, std::int64_t>> cell_at(QPointF point) const;
    void ensure_cursor_visible();
    void scroll_steps(std::int64_t steps);

    DocumentController& controller_;
    Tick origin_ = 0;
    int top_row_ = 0;
    /// Keyboard cursor: a pitch (stays on its drum when rows are added) and an absolute step.
    int cursor_pitch_ = 36;
    std::int64_t cursor_step_ = 0;
    std::optional<PaintStroke> stroke_;
    /// Ambiguous cell pressed: opened in the piano roll on release.
    std::optional<std::pair<Tick, int>> pending_open_;
    /// Document generation right after a click toggled a cell (undone by a double-click).
    std::optional<std::uint64_t> toggled_generation_;
    std::optional<Tick> playhead_;
    bool follow_playhead_ = true;
    bool follow_suspended_ = false;
    double wheel_rows_ = 0.0;
    double wheel_steps_ = 0.0;
    /// New steps use the sequence's new-note velocity (the Inspector's "Velocity" for new notes,
    /// uiB-19); Shift-click uses (or toggles) this accent velocity.
    int accent_velocity_ = 120;
    [[nodiscard]] int normal_velocity() const;
    mutable std::vector<int> rows_cache_;
    mutable std::optional<std::tuple<std::uint64_t, std::size_t, std::optional<std::size_t>>> rows_key_;
    static constexpr int name_width = 150;
    static constexpr int cell_width = 22;
    static constexpr int row_height = 20;
};

/// Drum grid with a horizontal (time) and a vertical (rows) scroll bar.
class DrumGridPanel : public QWidget {
    Q_OBJECT

public:
    explicit DrumGridPanel(DocumentController& controller, QWidget* parent = nullptr);
    [[nodiscard]] DrumGridView& view() { return *view_; }

private:
    void sync_scrollbars();

    DocumentController& controller_;
    DrumGridView* view_ = nullptr;
    QScrollBar* horizontal_ = nullptr;
    QScrollBar* vertical_ = nullptr;
    bool syncing_ = false;
};

} // namespace mwb::ui
