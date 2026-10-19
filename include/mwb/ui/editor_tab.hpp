#pragma once

#include "mwb/ui/controller_lane.hpp"
#include "mwb/ui/keyboard_widget.hpp"
#include "mwb/ui/piano_roll.hpp"

#include <QWidget>

#include <memory>
#include <vector>

class QComboBox;
class QSplitter;
class QStackedWidget;
class QTableWidget;
class QToolButton;

namespace mwb::ui {

class DocumentController;
class DrumGridView;
class DrumGridPanel;

/// Track list beside the editor: name, mute, solo and color per track (section 12). All are
/// document edits (undoable); recording goes to the selected (active) track.
class TrackListWidget : public QWidget {
    Q_OBJECT

public:
    explicit TrackListWidget(DocumentController& controller, QWidget* parent = nullptr);

private:
    void rebuild();
    void set_track_flag(TrackId track, int column, bool value);
    void add_track();
    void delete_active_track();
    void duplicate_active_track();
    void move_active_track(int direction);

    DocumentController& controller_;
    QTableWidget* table_ = nullptr;
    bool rebuilding_ = false;
    QWidget* delete_button_ = nullptr;
};

/// One document's center and bottom editor: track list, piano roll or drum grid, controller
/// lanes and the on-screen keyboard.
class EditorTab : public QWidget {
    Q_OBJECT

public:
    explicit EditorTab(std::unique_ptr<DocumentController> controller, QWidget* parent = nullptr);
    ~EditorTab() override;

    [[nodiscard]] DocumentController& controller() { return *controller_; }
    [[nodiscard]] PianoRollView& piano_roll() { return piano_roll_->view(); }
    [[nodiscard]] DrumGridView& drum_grid();
    /// Transport playhead for this document's editors (contract C1): forwards to the piano roll
    /// and the drum grid, which page to follow it while following is on.
    void set_playhead(std::optional<Tick> tick);
    void set_follow_playhead(bool follow);
    /// Loop Selection command: loops the grid-aligned extent of the selected notes.
    void loop_selection() { piano_roll().loop_selection(); }
    [[nodiscard]] KeyboardWidget& keyboard() { return *keyboard_; }
    void set_keyboard_visible(bool visible) { keyboard_->setVisible(visible); }
    [[nodiscard]] TrackListWidget& track_list() { return *track_list_; }
    /// Shows the drum grid instead of the piano roll (both are views of the same notes).
    void show_drum_grid(bool show);
    [[nodiscard]] bool showing_drum_grid() const;
    /// Adds a controller lane (velocity, release velocity, CC, bend, pressure).
    void add_lane(LaneTarget target);
    void remove_last_lane();
    void remove_lane(ControllerLane* lane);

signals:
    /// Messages from the drum grid and the lanes, for the status bar.
    void status_message(const QString& text);

private:
    void sync_lanes();

    std::unique_ptr<DocumentController> controller_;
    bool last_track_was_drum_ = false;
    TrackListWidget* track_list_ = nullptr;
    QStackedWidget* center_ = nullptr;
    PianoRollPanel* piano_roll_ = nullptr;
    DrumGridPanel* drum_grid_ = nullptr;
    QWidget* lanes_container_ = nullptr;
    std::vector<ControllerLane*> lanes_;
    KeyboardWidget* keyboard_ = nullptr;
};

} // namespace mwb::ui
