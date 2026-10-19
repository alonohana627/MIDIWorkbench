#pragma once

#include <QPointer>
#include <QWidget>

class QComboBox;
class QToolButton;

namespace mwb::ui {

class DocumentController;

/// Compact scale controls in the document tab row, always in view (the Inspector keeps the full
/// set): Scale on/off (highlight in-scale rows and mark notes outside the scale), the root and
/// scale type, Fold (show only scale rows and rows with notes) and "Select outside" to act on
/// the notes outside the scale. Every change is one undoable sequence-settings edit.
class ScaleBar : public QWidget {
    Q_OBJECT

public:
    explicit ScaleBar(QWidget* parent = nullptr);

    /// Follows the active editor; nullptr disables the bar.
    void set_controller(DocumentController* controller);

    [[nodiscard]] QToolButton* scale_button() const { return scale_; }
    [[nodiscard]] QToolButton* fold_button() const { return fold_; }
    [[nodiscard]] QComboBox* root_box() const { return root_; }
    [[nodiscard]] QComboBox* type_box() const { return type_; }
    [[nodiscard]] QToolButton* select_button() const { return select_; }

signals:
    /// "Select outside" was pressed; the window selects the active roll's out-of-scale notes.
    void select_outside_requested();

private:
    void refresh();
    void apply();
    /// Turning Scale on while the song is chromatic picks the estimated key (major/minor or a
    /// mode the scale list knows), or C major when the notes are too few to tell.
    void choose_scale_from_notes();

    QPointer<DocumentController> controller_;
    QToolButton* scale_ = nullptr;
    QComboBox* root_ = nullptr;
    QComboBox* type_ = nullptr;
    QToolButton* fold_ = nullptr;
    QToolButton* select_ = nullptr;
    bool updating_ = false;
};

} // namespace mwb::ui
