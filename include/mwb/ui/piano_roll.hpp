#pragma once

#include "mwb/core/note_edit.hpp"
#include "mwb/music/scale.hpp"
#include "mwb/ui/editor_math.hpp"

#include <QPointer>
#include <QTimer>
#include <QWidget>

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
    void set_snap_mode(SnapMode mode);
    [[nodiscard]] SnapMode snap_mode() const { return snap_mode_; }
    /// Grid used for snapping (from the sequence settings unless overridden).
    [[nodiscard]] Grid grid() const;
    void set_grid_override(std::optional<Rational> fraction);
    [[nodiscard]] const ViewMapping& mapping() const { return mapping_; }
    /// Playhead position for display (sampled from the transport at 30-60 Hz).
    void set_playhead(std::optional<Tick> tick);
    /// Pitches currently sounding (highlighted keys).
    void set_sounding_pitches(std::set<int> pitches);

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
    /// Selection commands of section 7.1.
    void select_all();
    void invert_selection();
    void select_same_pitch();
    void select_same_pitch_class();
    void select_same_channel();
    void select_out_of_scale();

    /// Width of the pinned keyboard and height of the ruler in device-independent pixels.
    static constexpr int keyboard_width = 56;
    static constexpr int ruler_height = 26;

    /// Ticks and pitch rows currently visible (for scroll bar page sizes).
    [[nodiscard]] Tick visible_ticks() const;
    [[nodiscard]] int visible_row_count() const;

signals:
    void audition_note_on(int pitch, int velocity);
    void audition_note_off(int pitch);
    void hover_changed(Tick tick, int pitch);
    void status_message(const QString& text);
    void mapping_changed();
    /// The edit tool changed (toolbar and B toggle stay in sync).
    void tool_changed(mwb::ui::EditTool tool);

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

private:
    enum class Gesture : std::uint8_t {
        none, pending_select, rubber_band, move, duplicate_move, resize_end, resize_start, draw, erase, pan, scrub
    };
    enum class HitZone : std::uint8_t { none, body, left_edge, right_edge };
    struct Hit {
        std::optional<std::size_t> note; ///< Index into the controller's note index.
        HitZone zone = HitZone::none;
    };

    [[nodiscard]] QRectF note_area() const;
    [[nodiscard]] QPointF to_note_area(QPointF widget_point) const;
    [[nodiscard]] Hit hit_test(QPointF widget_point) const;
    [[nodiscard]] bool note_editable(const NoteSpan& note) const;
    [[nodiscard]] Tick default_note_length() const;
    [[nodiscard]] std::uint8_t default_velocity() const;
    /// Channel for new notes: the active track's existing notes decide; drum tracks use channel 10.
    [[nodiscard]] std::uint8_t default_channel() const;
    [[nodiscard]] SnapMode effective_snap(const QMouseEvent* event) const;
    void begin_drag_from_selection();
    void commit_gesture(QMouseEvent* event);
    void cancel_gesture();
    void update_scrollbars();
    void auto_scroll_step();
    void paint_rows(QPainter& painter, const QRectF& area);
    void paint_grid(QPainter& painter, const QRectF& area);
    void paint_notes(QPainter& painter, const QRectF& area);
    void paint_density(QPainter& painter, const QRectF& area, const std::vector<std::size_t>& visible);
    void paint_keyboard(QPainter& painter);
    void paint_ruler(QPainter& painter);
    void paint_drag_preview(QPainter& painter, const QRectF& area);
    [[nodiscard]] QColor note_color(const NoteSpan& note) const;
    [[nodiscard]] bool in_scale(int pitch) const;
    /// Row mapping that honors Fold to Scale: scale members plus every used pitch stay visible.
    [[nodiscard]] double row_y(int pitch) const;
    [[nodiscard]] int pitch_at_y(double y) const;
    [[nodiscard]] std::vector<int> visible_rows(double height) const;
    void rebuild_fold_rows() const;
    [[nodiscard]] std::size_t top_row_index() const;
    [[nodiscard]] bool is_root(int pitch) const;
    void release_audition();

    DocumentController& controller_;
    QScrollBar* horizontal_bar_ = nullptr;
    QScrollBar* vertical_bar_ = nullptr;
    QTimer auto_scroll_timer_;
    ViewMapping mapping_;
    mutable std::vector<int> fold_rows_; ///< Descending visible pitches when folded; empty when chromatic.
    mutable std::uint64_t fold_key_ = 0;
    mutable bool fold_key_valid_ = false;
    mutable std::optional<music::Scale> scale_cache_;
    std::vector<ViewMapping> zoom_history_;
    EditTool tool_ = EditTool::select;
    NoteColorMode color_mode_ = NoteColorMode::track;
    SnapMode snap_mode_ = SnapMode::absolute;
    std::optional<Rational> grid_override_;
    std::optional<Tick> playhead_;
    std::set<int> sounding_;

    // Gesture state; every frame is derived from the snapshot captured at gesture start.
    Gesture gesture_ = Gesture::none;
    QPointF press_point_;
    QPointF current_point_;
    Qt::KeyboardModifiers press_modifiers_;
    DragSnapshot snapshot_;
    std::optional<NoteSpan> handle_note_;
    std::set<EventId> selection_at_press_;
    std::set<EventId> erase_hits_;
    MoveResult preview_move_;
    Tick preview_resize_delta_ = 0;
    std::optional<int> audition_pitch_;
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
