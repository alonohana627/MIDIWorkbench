#pragma once

#include <QPointer>
#include <QWidget>

#include <array>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

class QFormLayout;
class QDoubleSpinBox;

namespace mwb::music {
struct TransformPreview;
}

namespace mwb::ui {

class DocumentController;

/// Right inspector (section 6.1): selection properties as numeric fields, track routing and
/// instrument, the scale panel (display separated from modification, section 9.1), chord insertion
/// and the transformation panel with preview, apply and cancel (section 10 lifecycle).
class InspectorPanel : public QWidget {
    Q_OBJECT

public:
    explicit InspectorPanel(QWidget* parent = nullptr);
    ~InspectorPanel() override;

    /// Shows the given document (or nothing).
    void set_controller(DocumentController* controller);
    /// Opens the transform panel on a tool ("quantize", ...).
    void focus_tool(const QString& tool_id);
    /// Applies the last used quantize settings to the selection (shortcut Q).
    void apply_last_quantize();

signals:
    void status_message(const QString& text);

private:
    void refresh();
    void refresh_selection();
    void apply_note_fields();
    void apply_track_fields();
    void apply_scale_fields();
    void apply_song_fields();
    /// Chooses a transform tool without moving keyboard focus.
    void select_tool(const QString& tool_id);
    void load_tool_defaults();
    /// Shows only the fields the chosen tool uses, with tool-specific labels.
    void update_tool_rows();
    /// Start and length readouts in bar.beat.tick next to the tick values.
    void update_position_suffixes();
    void update_parameters_from_form();
    void preview();
    void apply_preview();
    void cancel_preview();
    void insert_chord();
    void recognize_selection();

    QPointer<DocumentController> controller_;
    // Song (tempo, meter, loop length; section 5.1 step 2)
    QDoubleSpinBox* song_tempo_ = nullptr;
    QSpinBox* song_numerator_ = nullptr;
    QComboBox* song_denominator_ = nullptr;
    QSpinBox* song_loop_bars_ = nullptr;
    // Selection
    QLabel* selection_label_ = nullptr;
    QFormLayout* selection_form_ = nullptr;
    QSpinBox* start_ = nullptr;
    QSpinBox* length_ = nullptr;
    QSpinBox* pitch_ = nullptr;
    QSpinBox* velocity_ = nullptr;
    QSpinBox* channel_ = nullptr;
    QLabel* chord_label_ = nullptr;
    // Track
    QLineEdit* track_name_ = nullptr;
    QCheckBox* track_drum_ = nullptr;
    QComboBox* track_channel_override_ = nullptr;
    QComboBox* track_output_ = nullptr;
    QSpinBox* track_transpose_ = nullptr;
    QComboBox* track_preset_ = nullptr;
    QSpinBox* track_program_ = nullptr;
    // Scale
    QComboBox* scale_root_ = nullptr;
    QComboBox* scale_type_ = nullptr;
    QCheckBox* highlight_ = nullptr;
    QCheckBox* fold_ = nullptr;
    QComboBox* input_policy_ = nullptr;
    QComboBox* snap_ = nullptr;
    // Chords
    QComboBox* chord_type_ = nullptr;
    QComboBox* chord_root_ = nullptr;
    QSpinBox* chord_octave_ = nullptr;
    QSpinBox* chord_inversion_ = nullptr;
    QSpinBox* chord_strum_ = nullptr;
    // Transform
    QComboBox* tool_ = nullptr;
    QSpinBox* amount_ = nullptr;
    QSpinBox* strength_ = nullptr;
    QSpinBox* seed_ = nullptr;
    QComboBox* grid_ = nullptr;
    QPlainTextEdit* parameters_ = nullptr;
    QFormLayout* transform_form_ = nullptr;
    QCheckBox* advanced_ = nullptr;
    QLabel* report_ = nullptr;
    std::unique_ptr<music::TransformPreview> pending_;
    std::uint64_t pending_generation_ = 0;
    QString last_quantize_json_;
    bool updating_ = false;
    /// Values shown at the last refresh (start, length, pitch, velocity, channel).
    std::array<int, 5> shown_{};
};

} // namespace mwb::ui
