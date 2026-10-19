#pragma once

#include "mwb/ui/editor_math.hpp"

#include <QWidget>

#include <optional>
#include <vector>

namespace mwb::ui {

class DocumentController;

/// What a lane shows and edits (section 11.2).
struct LaneTarget {
    enum class Kind : std::uint8_t { velocity, release_velocity, controller, pitch_bend, channel_pressure };
    Kind kind = Kind::velocity;
    std::uint8_t controller = 1; ///< CC number for Kind::controller.
    std::uint8_t channel = 0;    ///< Target channel for controller-type lanes.

    [[nodiscard]] QString label() const;
    /// Largest stored value (127 or 16383 for pitch bend).
    [[nodiscard]] int maximum() const { return kind == Kind::pitch_bend ? 16383 : 127; }
};

/// Human name of a controller number, including reserved meanings (section 11.2).
[[nodiscard]] QString controller_name(int controller);

/// A resizable lane under the piano roll sharing its horizontal mapping. Velocity lanes edit the
/// note-ons of the active track (selected notes when there is a selection); controller lanes draw
/// with a pencil that replaces events only in the swept range and adds a boundary point so playback
/// after the range is unchanged. Each drag is one undo entry.
class ControllerLane : public QWidget {
    Q_OBJECT

public:
    ControllerLane(DocumentController& controller, QWidget* parent = nullptr);

    void set_target(LaneTarget target);
    [[nodiscard]] const LaneTarget& target() const { return target_; }
    /// Horizontal mapping shared with the piano roll; `left_margin` aligns with its keyboard.
    void set_mapping(const ViewMapping& mapping, int left_margin, Grid grid);
    [[nodiscard]] QSize sizeHint() const override;

signals:
    void status_message(const QString& text);
    /// The user chose "Remove Lane" from the lane's context menu.
    void remove_requested(mwb::ui::ControllerLane* lane);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool event(QEvent* event) override;

private:
    struct Stroke {
        std::vector<std::pair<Tick, int>> points; ///< (tick, value) along the drag
        bool erase = false;
    };
    [[nodiscard]] int value_at_y(double y) const;
    [[nodiscard]] double y_for_value(int value) const;
    [[nodiscard]] bool matches(const Event& event) const;
    void commit_velocity_stroke();
    void commit_controller_stroke();
    /// Positions of the active track's events this lane shows, rebuilt once per document change.
    const std::vector<std::size_t>& matching_positions() const;
    [[nodiscard]] int event_value(const Event& event) const;
    void nudge_selected_velocity(int delta);

    DocumentController& controller_;
    LaneTarget target_;
    ViewMapping mapping_;
    Grid grid_;
    int left_margin_ = 56;
    std::optional<Stroke> stroke_;
    std::optional<std::pair<Tick, int>> hover_; ///< Pointer position as (tick, value) for the readout.
    mutable std::vector<std::size_t> matching_;
    mutable std::uint64_t matching_generation_ = UINT64_MAX;
    mutable std::optional<std::size_t> matching_track_;
    mutable LaneTarget matching_target_;
};

} // namespace mwb::ui
