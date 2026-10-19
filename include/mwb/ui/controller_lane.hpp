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
    /// Two short lines for the narrow lane margin ("CC 64" / "Sustain Pedal"); label() is the tooltip.
    [[nodiscard]] QString short_label() const;
    [[nodiscard]] QString detail_label() const;
    /// Largest stored value (127 or 16383 for pitch bend).
    [[nodiscard]] int maximum() const { return kind == Kind::pitch_bend ? 16383 : 127; }
};

/// Human name of a controller number, including reserved meanings (section 11.2).
[[nodiscard]] QString controller_name(int controller);

/// Value a channel resets to when nothing was sent (RP-015 "reset all controllers" defaults): the
/// boundary point written after a drawn curve on a lane that had no earlier event, so the drawn
/// value does not hold for the rest of the song (section 11.2). Bend 8192, pressure 0, CC 7 100,
/// CC 8/10 64, CC 11 127, other controllers 0.
[[nodiscard]] int controller_default_value(const LaneTarget& target);

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
    /// Channel of the active track: controller-type lanes follow it until the user picks a
    /// channel from the lane menu.
    void follow_track_channel(std::uint8_t channel);
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
    void focusOutEvent(QFocusEvent* event) override;
    bool event(QEvent* event) override;

private:
    struct Stroke {
        std::vector<std::pair<Tick, int>> points; ///< (tick, value) along the drag, in arrival order
        bool erase = false;
        Qt::MouseButton button = Qt::LeftButton;
        double press_x = 0;
        double max_x_distance = 0; ///< Largest horizontal distance from the press, in pixels.
    };
    /// The stroke with one value per tick (a later point replaces an earlier one at the same
    /// tick), sorted by tick: a vertical drag ends at the value released, not its minimum or maximum.
    [[nodiscard]] std::vector<std::pair<Tick, int>> stroke_by_tick() const;
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
    mutable std::size_t matching_sequence_ = SIZE_MAX;
    mutable LaneTarget matching_target_;
    bool follow_channel_ = true;
    /// Set when a right-drag erase ended: Windows delivers the context menu after the release.
    bool suppress_context_menu_ = false;
};

} // namespace mwb::ui
