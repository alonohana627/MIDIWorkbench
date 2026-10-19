#pragma once

#include "mwb/core/note_edit.hpp"
#include "mwb/music/scale.hpp"
#include "mwb/ui/editor_math.hpp"

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <array>
#include <optional>
#include <set>
#include <vector>

class QScrollBar;

namespace mwb::ui {

class DocumentController;

/// Editing tools of section 7.1.
enum class EditTool : std::uint8_t { select, draw, erase, split, audition };

/// Note color modes of section 7.1.
enum class NoteColorMode : std::uint8_t { track, channel, velocity, scale_degree };

/// Channel for new notes on the active track: the track's existing notes decide, then the drum
/// flag (channel 10 is a convention, not a rule, section 11.1). Shared by the piano roll, the drum
/// grid and the controller lanes so every editor writes the same channel.
[[nodiscard]] std::uint8_t default_channel_for(const DocumentController& controller);

/// Sets the active sequence's loop to `range` as one undoable command ("Set loop"), reusing the
/// loop region (or a switched-off loop region) when one exists. Returns false when nothing changed
/// or the range is empty.
bool set_loop_range(DocumentController& controller, TimeRange range);

/// Custom-painted piano roll (sections 7.1-7.4): pinned keyboard and ruler, notes from the
/// interval index of the visible window only, density summaries when zoomed far out, and the
/// mouse contract of section 7.2. It never edits the document directly: every committed gesture
/// becomes one command through the DocumentController (one undo entry per gesture).
class PianoRollView : public QWidget {
    Q_OBJECT

public:
    explicit PianoRollView(DocumentController& controller, QWidget* parent = nullptr);

    void set_tool(EditTool tool);
    [[nodiscard]] EditTool tool() const { return tool_; }
    void set_color_mode(NoteColorMode mode);
    /// Absolute, relative or off (section 7.3); Alt during a drag still bypasses snapping.
    void set_snap_mode(SnapMode mode);
    [[nodiscard]] SnapMode snap_mode() const { return snap_mode_; }
    /// Grid used for snapping (from the sequence settings unless overridden).
    [[nodiscard]] Grid grid() const;
    void set_grid_override(std::optional<Rational> fraction);
    [[nodiscard]] const ViewMapping& mapping() const { return mapping_; }
    /// Playhead position for display (sampled from the transport at 30-60 Hz). While following,
    /// the view pages so the playhead stays visible.
    void set_playhead(std::optional<Tick> tick);
    [[nodiscard]] std::optional<Tick> playhead() const { return playhead_; }
    /// Follow the playhead while playing (default on). Scrolling by hand during playback pauses
    /// following until the transport stops.
    void set_follow_playhead(bool follow);
    [[nodiscard]] bool follow_playhead() const { return follow_playhead_; }
    /// Pitches currently sounding (highlighted keys and notes).
    void set_sounding_pitches(std::set<int> pitches);
    /// Result of a transform preview drawn as outlined ghosts over the notes; shown only while the
    /// document is still at `generation` (an empty vector clears it).
    void set_preview_notes(std::vector<NoteSpan> notes, std::uint64_t generation);

    /// View commands of section 7.4.
    void fit_selection_or_all();
    void fit_all();
    void return_to_previous_zoom();
    void zoom_time(double factor);
    void scroll_to_tick(Tick tick);
    /// Sets the left edge without recording zoom history (scroll bar).
    void set_origin(Tick tick);
    /// Sets the top visible pitch row (scroll bar).
    void set_top_pitch(int pitch);
    /// Row-based vertical position: 128 chromatic rows, or the folded rows (Fold to Scale).
    [[nodiscard]] int row_count() const;
    [[nodiscard]] int top_row() const;
    void set_top_row(int row);
    /// Selection commands of section 7.1.
    void select_all();
    void invert_selection();
    void select_same_pitch();
    void select_same_pitch_class();
    void select_same_channel();
    void select_same_duration();
    void select_out_of_scale();
    /// Loop Selection: sets the loop to the grid-aligned extent of the selected notes.
    void loop_selection();

    /// Width of the pinned keyboard and height of the ruler in device-independent pixels.
    static constexpr int keyboard_width = 56;
    static constexpr int ruler_height = 26;
    /// Height of the loop strip at the top of the ruler (drag there to set the loop).
    static constexpr int loop_strip_height = 8;

    /// Ticks and pitch rows currently visible (for scroll bar page sizes).
    [[nodiscard]] Tick visible_ticks() const;
    [[nodiscard]] int visible_row_count() const;

signals:
    void audition_note_on(int pitch, int velocity);
    void audition_note_off(int pitch);
    void hover_changed(Tick tick, int pitch);
    /// The pointer left the editor (pointer readouts should clear).
    void hover_left();
    void status_message(const QString& text);
    void mapping_changed();
    /// The edit tool changed (toolbar and B toggle stay in sync).
    void tool_changed(mwb::ui::EditTool tool);
    /// A ruler click or scrub ended at `tick` (the new edit cursor): a rolling transport jumps there.
    void seek_requested(mwb::Tick tick);

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
    void contextMenuEvent(QContextMenuEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool event(QEvent* event) override;

private:
    enum class Gesture : std::uint8_t {
        none, pending_select, rubber_band, move, duplicate_move, resize_end, resize_start, draw, erase, pan, scrub,
        loop_create, loop_move_start, loop_move_end, loop_move
    };
    enum class HitZone : std::uint8_t { none, body, left_edge, right_edge };
    struct Hit {
        std::optional<std::size_t> note; ///< Index into the controller's note index.
        HitZone zone = HitZone::none;
    };
    struct FoldKey {
        std::uint64_t generation = 0;
        std::size_t sequence = 0;
        std::uint16_t mask = 0;
        std::uint8_t root = 0;
        bool fold = false;
        friend bool operator==(const FoldKey&, const FoldKey&) = default;
    };

    [[nodiscard]] QRectF note_area() const;
    [[nodiscard]] QPointF to_note_area(QPointF widget_point) const;
    [[nodiscard]] Hit hit_test(QPointF widget_point) const;
    [[nodiscard]] bool note_editable(const NoteSpan& note) const;
    [[nodiscard]] Tick default_note_length() const;
    [[nodiscard]] std::uint8_t default_velocity() const;
    [[nodiscard]] SnapMode effective_snap(Qt::KeyboardModifiers modifiers) const;
    [[nodiscard]] Tick snap_floor(Tick tick, Qt::KeyboardModifiers modifiers) const;
    [[nodiscard]] Tick snap_nearest(Tick tick, Qt::KeyboardModifiers modifiers) const;
    void begin_drag_from_selection();
    /// Recomputes the preview of the running gesture from the press point, the current point and
    /// the last modifiers (mouse moves and auto-scroll both call it).
    void update_gesture_preview();
    void commit_gesture();
    void cancel_gesture();
    void auto_scroll_step();
    void push_zoom_history(bool from_wheel);
    void suspend_follow();
    void update_playhead_area(std::optional<Tick> tick);
    void collect_erase_hits(QPointF from, QPointF to);
    [[nodiscard]] std::optional<TimeRange> loop_range() const;
    [[nodiscard]] QString empty_hint() const;
    void paint_rows(QPainter& painter, const QRectF& area);
    void paint_grid(QPainter& painter, const QRectF& area);
    void paint_notes(QPainter& painter, const QRectF& area);
    void paint_density(QPainter& painter, const QRectF& area, const std::vector<std::size_t>& visible);
    void paint_keyboard(QPainter& painter);
    void paint_ruler(QPainter& painter);
    void paint_drag_preview(QPainter& painter, const QRectF& area);
    void paint_transform_preview(QPainter& painter, const QRectF& area);
    [[nodiscard]] QColor note_color(const NoteSpan& note) const;
    [[nodiscard]] bool in_scale(int pitch) const;
    /// Row mapping that honors Fold to Scale: scale members plus every used pitch stay visible.
    [[nodiscard]] double row_y(int pitch) const;
    /// row_y for every pitch, computed once per paint (fold mode makes row_y O(rows)).
    [[nodiscard]] std::array<double, 128> row_table() const;
    [[nodiscard]] int pitch_at_y(double y) const;
    [[nodiscard]] std::vector<int> visible_rows(double height) const;
    void rebuild_fold_rows() const;
    [[nodiscard]] std::size_t top_row_index() const;
    /// Moves `rows` visible rows up (positive) or down from `pitch`, through the folded rows when
    /// Fold to Scale is on.
    [[nodiscard]] int step_rows(int pitch, int rows) const;
    /// Clamps a top pitch so the last row still reaches the bottom of the view.
    [[nodiscard]] int clamp_top(int pitch) const;
    [[nodiscard]] bool is_root(int pitch) const;
    void release_audition();

    DocumentController& controller_;
    QTimer auto_scroll_timer_;
    ViewMapping mapping_;
    mutable std::vector<int> fold_rows_; ///< Descending visible pitches when folded; empty when chromatic.
    mutable std::optional<FoldKey> fold_key_;
    mutable std::optional<music::Scale> scale_cache_;
    std::vector<ViewMapping> zoom_history_;
    QElapsedTimer last_wheel_zoom_;
    EditTool tool_ = EditTool::select;
    NoteColorMode color_mode_ = NoteColorMode::track;
    SnapMode snap_mode_ = SnapMode::absolute;
    std::optional<Rational> grid_override_;
    std::optional<Tick> playhead_;
    bool follow_playhead_ = true;
    bool follow_suspended_ = false;
    std::set<int> sounding_;
    std::vector<NoteSpan> preview_notes_;
    std::uint64_t preview_generation_ = 0;

    // Gesture state; every frame is derived from the snapshot captured at gesture start.
    Gesture gesture_ = Gesture::none;
    Qt::MouseButton gesture_button_ = Qt::NoButton;
    QPointF press_point_;
    QPointF current_point_;
    QPointF last_erase_point_;
    Qt::KeyboardModifiers press_modifiers_;
    Qt::KeyboardModifiers gesture_modifiers_; ///< Modifiers of the latest mouse event.
    bool past_threshold_ = false;
    DragSnapshot snapshot_;
    std::optional<NoteSpan> handle_note_;
    std::set<EventId> selection_at_press_;
    Tick edit_cursor_at_press_ = 0;
    std::set<EventId> erase_hits_;
    MoveResult preview_move_;
    Tick preview_resize_delta_ = 0;
    TimeRange loop_at_press_;
    TimeRange loop_preview_;
    std::optional<int> audition_pitch_;
    /// Keyboard note-entry pitch (Up/Down with nothing selected; Return inserts a note there).
    int cursor_pitch_ = 60;
    /// Fractional wheel steps from precision touchpads, accumulated until a whole row or column.
    double wheel_rows_ = 0.0;
    double wheel_columns_ = 0.0;
    /// Last move preview reported in the status bar (to report only changes).
    std::pair<Tick, int> reported_move_{0, 0};
    ViewMapping pan_origin_;
    bool shift_axis_locked_ = false;
    bool lock_to_time_axis_ = false;
};

/// Piano roll plus its scroll bars with a windowed mapping of 64-bit time onto Qt's int range.
class PianoRollPanel : public QWidget {
    Q_OBJECT

public:
    explicit PianoRollPanel(DocumentController& controller, QWidget* parent = nullptr);
    [[nodiscard]] PianoRollView& view() { return *view_; }

private:
    void sync_scrollbars();

    DocumentController& controller_;
    PianoRollView* view_ = nullptr;
    QScrollBar* horizontal_ = nullptr;
    QScrollBar* vertical_ = nullptr;
    bool syncing_ = false;
};

} // namespace mwb::ui
