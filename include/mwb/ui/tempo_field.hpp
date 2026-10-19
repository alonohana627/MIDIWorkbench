#pragma once

#include <QDoubleSpinBox>
#include <QTimer>

namespace mwb::ui {

/// Toolbar BPM field: shows the song tempo at the play position and edits it. Typing (Enter),
/// the arrow keys, the mouse wheel (once the field has focus, see MainWindow's wheel guard) and a
/// vertical drag on the text (1 BPM per 4 px, Shift 0.1 BPM) all change the value; a burst of
/// changes is committed as one edit: a drag on release, wheel and arrow steps after 400 ms
/// without a change, typing on Enter or focus loss. Display updates never overwrite a value the
/// user is still editing.
class TempoField : public QDoubleSpinBox {
    Q_OBJECT

public:
    explicit TempoField(QWidget* parent = nullptr);

    /// Shows `bpm` unless an edit is in progress (pending commit, drag, or unfinished typing).
    void show_tempo(double bpm);
    /// True while an edit has not been committed yet.
    [[nodiscard]] bool editing() const;
    /// Commits a pending edit now (also called by the idle timer and on release).
    void commit_now();

    static constexpr int idle_commit_ms = 400;
    static constexpr int drag_pixels_per_step = 4;

signals:
    /// One committed edit; the receiver writes the document (one undo entry).
    void tempo_committed(double bpm);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void value_edited();

    QTimer idle_;
    bool syncing_ = false;   ///< show_tempo() is setting the value.
    bool pending_ = false;   ///< A changed value waits for its commit.
    bool pressed_ = false;   ///< Left button down on the text, drag not decided yet.
    bool dragging_ = false;
    int press_y_ = 0;
    double press_value_ = 0;
};

} // namespace mwb::ui
