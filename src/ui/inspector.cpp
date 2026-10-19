#include "mwb/ui/inspector.hpp"

#include "mwb/core/note_edit.hpp"
#include "mwb/core/pitch.hpp"
#include "mwb/music/chord.hpp"
#include "mwb/music/scale.hpp"
#include "mwb/music/transform.hpp"
#include "mwb/platform/midi_backend.hpp"
#include "mwb/ui/document_controller.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace mwb::ui {

namespace {

struct ToolEntry {
    const char* id;
    const char* label;
};

constexpr ToolEntry tool_entries[] = {
    {"quantize", "Quantize"},           {"humanize", "Humanize"},         {"transpose", "Transpose"},
    {"velocity", "Velocity"},           {"length", "Length"},             {"time_stretch", "Time stretch"},
    {"reverse", "Reverse"},             {"invert_pitch", "Invert pitch"}, {"rotate", "Rotate phrase"},
    {"repeat", "Repeat"},               {"strum", "Strum"},               {"arpeggiate", "Arpeggiate"},
    {"euclidean", "Euclidean rhythm"},  {"remove_duplicates", "Remove duplicates"},
    {"resolve_overlaps", "Resolve overlaps"}, {"join", "Join notes"},    {"conform_to_scale", "Conform to scale"},
};

const char* const snap_values[] = {"1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/128",
                                   "1/6", "1/12", "1/24", "1/48", "3/8", "3/16", "3/32", "1/20", "1/28"};
const char* const snap_labels[] = {"1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/128",
                                   "1/4 triplet", "1/8 triplet", "1/16 triplet", "1/32 triplet",
                                   "Dotted 1/4", "Dotted 1/8", "Dotted 1/16", "1/16 quintuplet", "1/16 septuplet"};

const char* const presets[] = {"classic", "saw-pad", "square-lead", "electric-piano", "bass"};

QSpinBox* spin(int low, int high, int value, QWidget* parent) {
    auto* box = new QSpinBox(parent);
    box->setRange(low, high);
    box->setValue(value);
    box->setMinimumHeight(24);
    return box;
}

/// Builds tool parameters for the quick fields; everything else stays at its default and can be
/// edited in the parameter JSON.
std::optional<music::TransformParameters> parameters_for(const QString& tool, int amount, int strength_percent,
                                                         std::uint64_t seed, Rational grid, const Sequence& sequence) {
    using namespace music;
    if (tool == "quantize") {
        QuantizeParameters parameters;
        parameters.grid.step = grid;
        parameters.strength = Rational{std::clamp(strength_percent, 0, 100), 100};
        return parameters;
    }
    if (tool == "humanize") {
        HumanizeParameters parameters;
        parameters.seed = seed;
        parameters.timing_range = std::max(0, amount);
        return parameters;
    }
    if (tool == "transpose") {
        TransposeParameters parameters;
        parameters.amount = amount;
        parameters.scale_root = sequence.root_pc;
        parameters.scale_mask = sequence.scale_mask;
        return parameters;
    }
    if (tool == "velocity") {
        VelocityParameters parameters;
        parameters.value = std::clamp(amount, 1, 127);
        return parameters;
    }
    if (tool == "length") {
        LengthParameters parameters;
        parameters.ticks = std::max(1, amount);
        return parameters;
    }
    if (tool == "time_stretch")
        return TimeStretchParameters{};
    if (tool == "reverse")
        return ReverseParameters{};
    if (tool == "invert_pitch") {
        InvertPitchParameters parameters;
        parameters.axis_pitch = std::clamp(amount, 0, 127);
        return parameters;
    }
    if (tool == "rotate") {
        RotateParameters parameters;
        parameters.shift = amount;
        return parameters;
    }
    if (tool == "repeat") {
        RepeatParameters parameters;
        parameters.count = std::clamp(amount, 1, 1024);
        return parameters;
    }
    if (tool == "strum") {
        StrumParameters parameters;
        parameters.spread = std::max(0, amount);
        parameters.seed = seed;
        return parameters;
    }
    if (tool == "arpeggiate") {
        ArpeggiateParameters parameters;
        parameters.rate = grid;
        parameters.seed = seed;
        return parameters;
    }
    if (tool == "euclidean") {
        EuclideanParameters parameters;
        parameters.pulses = std::clamp(amount, 0, parameters.steps);
        parameters.step_length = grid;
        return parameters;
    }
    if (tool == "remove_duplicates")
        return RemoveDuplicatesParameters{};
    if (tool == "resolve_overlaps")
        return ResolveOverlapsParameters{};
    if (tool == "join") {
        JoinParameters parameters;
        parameters.max_gap = std::max(0, amount);
        return parameters;
    }
    if (tool == "conform_to_scale") {
        ConformToScaleParameters parameters;
        parameters.scale_root = sequence.root_pc;
        parameters.scale_mask = sequence.scale_mask;
        return parameters;
    }
    return std::nullopt;
}

} // namespace

InspectorPanel::InspectorPanel(QWidget* parent) : QWidget(parent) {
    auto* content = new QWidget;
    auto* column = new QVBoxLayout(content);

    // --- Selection ------------------------------------------------------------------------------
    auto* selection_box = new QGroupBox(tr("Selection"), content);
    // The wrapped hint sits above the form (a spanning QFormLayout row miscomputes its height).
    auto* selection_column = new QVBoxLayout(selection_box);
    selection_label_ = new QLabel(tr("No notes selected"), selection_box);
    selection_label_->setWordWrap(true);
    selection_column->addWidget(selection_label_);
    auto* selection_form = new QFormLayout;
    selection_column->addLayout(selection_form);
    selection_form_ = selection_form;
    selection_form->setVerticalSpacing(6);
    start_ = spin(0, INT32_MAX, 0, selection_box);
    length_ = spin(1, INT32_MAX, 240, selection_box);
    pitch_ = spin(0, 127, 60, selection_box);
    velocity_ = spin(1, 127, 100, selection_box);
    channel_ = spin(1, 16, 1, selection_box); // displayed 1-16
    selection_form->addRow(tr("Start"), start_);
    selection_form->addRow(tr("Length"), length_);
    start_->setToolTip(tr("Start in ticks; bar.beat.tick shown in parentheses"));
    connect(start_, &QSpinBox::valueChanged, this, &InspectorPanel::update_position_suffixes);
    connect(length_, &QSpinBox::valueChanged, this, &InspectorPanel::update_position_suffixes);
    length_->setToolTip(tr("Length in ticks; beats shown in parentheses"));
    selection_form->addRow(tr("Pitch"), pitch_);
    selection_form->addRow(tr("Velocity"), velocity_);
    selection_form->addRow(tr("Channel"), channel_);
    chord_label_ = new QLabel(selection_box);
    chord_label_->setWordWrap(true);
    selection_form->addRow(tr("Chord"), chord_label_);
    for (auto* box : {start_, length_, pitch_, velocity_, channel_})
        connect(box, &QSpinBox::editingFinished, this, &InspectorPanel::apply_note_fields);
    column->addWidget(selection_box);

    // --- Song -----------------------------------------------------------------------------------
    auto* song_box = new QGroupBox(tr("Song"), content);
    auto* song_form = new QFormLayout(song_box);
    song_tempo_ = new QDoubleSpinBox(song_box);
    song_tempo_->setRange(5.0, 999.0);
    song_tempo_->setDecimals(2);
    song_tempo_->setSuffix(tr(" BPM"));
    song_tempo_->setKeyboardTracking(false);
    song_tempo_->setToolTip(tr("Tempo at the start of the song (writes a tempo event at bar 1)"));
    song_numerator_ = spin(1, 32, 4, song_box);
    song_numerator_->setKeyboardTracking(false);
    song_denominator_ = new QComboBox(song_box);
    for (const int denominator : {1, 2, 4, 8, 16, 32})
        song_denominator_->addItem(QString::number(denominator), denominator);
    auto* meter_row = new QHBoxLayout;
    meter_row->addWidget(song_numerator_);
    meter_row->addWidget(new QLabel("/", song_box));
    meter_row->addWidget(song_denominator_);
    song_loop_bars_ = spin(0, 999, 4, song_box);
    song_loop_bars_->setSpecialValueText(tr("No loop"));
    song_loop_bars_->setSuffix(tr(" bars"));
    song_loop_bars_->setKeyboardTracking(false);
    song_loop_bars_->setToolTip(tr("Length of the loop region from its start"));
    song_form->addRow(tr("Tempo"), song_tempo_);
    song_form->addRow(tr("Meter"), meter_row);
    song_form->addRow(tr("Loop length"), song_loop_bars_);
    connect(song_tempo_, &QDoubleSpinBox::valueChanged, this, &InspectorPanel::apply_song_fields);
    connect(song_numerator_, &QSpinBox::valueChanged, this, &InspectorPanel::apply_song_fields);
    connect(song_denominator_, &QComboBox::activated, this, &InspectorPanel::apply_song_fields);
    connect(song_loop_bars_, &QSpinBox::valueChanged, this, &InspectorPanel::apply_song_fields);
    column->addWidget(song_box);

    // --- Track ----------------------------------------------------------------------------------
    auto* track_box = new QGroupBox(tr("Track"), content);
    auto* track_form = new QFormLayout(track_box);
    track_name_ = new QLineEdit(track_box);
    track_drum_ = new QCheckBox(tr("Drum track (display hint)"), track_box);
    track_channel_override_ = new QComboBox(track_box);
    track_channel_override_->addItem(tr("Keep channels"));
    for (int channel = 1; channel <= 16; ++channel)
        track_channel_override_->addItem(tr("Force channel %1").arg(channel));
    track_channel_override_->setToolTip(tr("Play the track on its events' own channels, or force one channel"));
    track_transpose_ = spin(-48, 48, 0, track_box);
    track_preset_ = new QComboBox(track_box);
    for (const auto* preset : presets)
        track_preset_->addItem(QString::fromLatin1(preset));
    track_program_ = spin(1, 128, 1, track_box);
    track_form->addRow(tr("Name"), track_name_);
    track_form->addRow(track_drum_);
    track_output_ = new QComboBox(track_box);
    track_output_->setToolTip(tr("Internal synth, or an external MIDI port chosen explicitly (nothing is sent by default)"));
    track_output_->addItem(tr("Built-in synth"), QString());
    auto* refresh_ports = new QPushButton(tr("Refresh ports"), track_box);
    connect(refresh_ports, &QPushButton::clicked, this, [this] {
        auto backend = platform::create_midi_backend();
        if (!backend)
            return;
        auto outputs = (*backend)->enumerate_outputs();
        const QString current = track_output_->currentData().toString();
        track_output_->clear();
        track_output_->addItem(tr("Built-in synth"), QString());
        if (outputs)
            for (const auto& port : *outputs)
                track_output_->addItem(tr("MIDI out: %1").arg(QString::fromStdString(port.display_name)),
                                       QString::fromStdString(port.stable_id));
        track_output_->setCurrentIndex(std::max(0, track_output_->findData(current)));
    });
    connect(track_output_, &QComboBox::activated, this, &InspectorPanel::apply_track_fields);
    track_form->addRow(tr("Output"), track_output_);
    track_form->addRow(refresh_ports);
    track_form->addRow(tr("Routing"), track_channel_override_);
    track_form->addRow(tr("Playback transpose"), track_transpose_);
    track_form->addRow(tr("Built-in sound"), track_preset_);
    track_form->addRow(tr("Program (1-128)"), track_program_);
    connect(track_name_, &QLineEdit::editingFinished, this, &InspectorPanel::apply_track_fields);
    connect(track_drum_, &QCheckBox::toggled, this, &InspectorPanel::apply_track_fields);
    connect(track_channel_override_, &QComboBox::activated, this, &InspectorPanel::apply_track_fields);
    connect(track_transpose_, &QSpinBox::editingFinished, this, &InspectorPanel::apply_track_fields);
    connect(track_preset_, &QComboBox::activated, this, &InspectorPanel::apply_track_fields);
    connect(track_program_, &QSpinBox::editingFinished, this, &InspectorPanel::apply_track_fields);
    column->addWidget(track_box);

    // --- Scale ----------------------------------------------------------------------------------
    auto* scale_box = new QGroupBox(tr("Scale and grid"), content);
    auto* scale_form = new QFormLayout(scale_box);
    scale_root_ = new QComboBox(scale_box);
    for (int pitch_class = 0; pitch_class < 12; ++pitch_class)
        scale_root_->addItem(QString::fromStdString(pitch_class_name(pitch_class)));
    scale_type_ = new QComboBox(scale_box);
    for (const auto& definition : music::builtin_scales())
        scale_type_->addItem(QString::fromUtf8(definition.name.data(), static_cast<qsizetype>(definition.name.size())),
                             QString::fromUtf8(definition.id.data(), static_cast<qsizetype>(definition.id.size())));
    highlight_ = new QCheckBox(tr("Highlight scale rows (display only)"), scale_box);
    fold_ = new QCheckBox(tr("Fold to scale (display only)"), scale_box);
    input_policy_ = new QComboBox(scale_box);
    input_policy_->addItems({tr("Allow all input"), tr("Reject out-of-scale input"), tr("Nearest scale member"),
                             tr("Diatonic keyboard map")});
    snap_ = new QComboBox(scale_box);
    for (std::size_t index = 0; index < std::size(snap_values); ++index)
        snap_->addItem(tr(snap_labels[index]), QString::fromLatin1(snap_values[index]));
    scale_form->addRow(tr("Root"), scale_root_);
    scale_form->addRow(tr("Scale"), scale_type_);
    scale_form->addRow(highlight_);
    scale_form->addRow(fold_);
    scale_form->addRow(tr("New input"), input_policy_);
    scale_form->addRow(tr("Snap"), snap_);
    auto* scale_note = new QLabel(tr("Changing the scale never moves notes. Use Conform to scale below to change them."),
                                  scale_box);
    scale_note->setWordWrap(true);
    scale_form->addRow(scale_note);
    for (auto* combo : {scale_root_, scale_type_, input_policy_, snap_})
        connect(combo, &QComboBox::activated, this, &InspectorPanel::apply_scale_fields);
    connect(highlight_, &QCheckBox::toggled, this, &InspectorPanel::apply_scale_fields);
    connect(fold_, &QCheckBox::toggled, this, &InspectorPanel::apply_scale_fields);
    column->addWidget(scale_box);

    // --- Chords ---------------------------------------------------------------------------------
    auto* chord_box = new QGroupBox(tr("Insert chord at edit cursor"), content);
    auto* chord_form = new QFormLayout(chord_box);
    chord_root_ = new QComboBox(chord_box);
    for (int pitch_class = 0; pitch_class < 12; ++pitch_class)
        chord_root_->addItem(QString::fromStdString(pitch_class_name(pitch_class)));
    chord_octave_ = spin(-1, 9, 4, chord_box);
    chord_type_ = new QComboBox(chord_box);
    for (const auto& info : music::chord_types())
        chord_type_->addItem(QString::fromUtf8(info.display_name.data(), static_cast<qsizetype>(info.display_name.size())),
                             static_cast<int>(info.type));
    chord_inversion_ = spin(0, 6, 0, chord_box);
    chord_strum_ = spin(0, 960, 0, chord_box);
    auto* insert = new QPushButton(tr("Insert chord"), chord_box);
    chord_form->addRow(tr("Root"), chord_root_);
    chord_form->addRow(tr("Octave"), chord_octave_);
    chord_form->addRow(tr("Type"), chord_type_);
    chord_form->addRow(tr("Inversion"), chord_inversion_);
    chord_form->addRow(tr("Strum (ticks)"), chord_strum_);
    chord_form->addRow(insert);
    connect(insert, &QPushButton::clicked, this, &InspectorPanel::insert_chord);
    column->addWidget(chord_box);

    // --- Transformations ------------------------------------------------------------------------
    auto* transform_box = new QGroupBox(tr("Transform selection"), content);
    auto* transform_form = new QFormLayout(transform_box);
    transform_form_ = transform_form;
    tool_ = new QComboBox(transform_box);
    for (const auto& entry : tool_entries)
        tool_->addItem(tr(entry.label), QString::fromLatin1(entry.id));
    amount_ = spin(-100000, 100000, 0, transform_box);
    amount_->setToolTip(tr("Semitones, ticks, velocity, count or pulses depending on the tool"));
    strength_ = spin(0, 100, 100, transform_box);
    strength_->setSuffix(" %");
    seed_ = spin(0, INT32_MAX, 1, transform_box);
    grid_ = new QComboBox(transform_box);
    for (std::size_t index = 0; index < std::size(snap_values); ++index)
        grid_->addItem(tr(snap_labels[index]), QString::fromLatin1(snap_values[index]));
    grid_->setCurrentIndex(4);
    parameters_ = new QPlainTextEdit(transform_box);
    parameters_->setAccessibleName(tr("Tool parameters (JSON)"));
    parameters_->setMinimumHeight(110);
    report_ = new QLabel(transform_box);
    report_->setWordWrap(true);
    auto* buttons = new QHBoxLayout;
    auto* preview_button = new QPushButton(tr("Preview"), transform_box);
    auto* apply_button = new QPushButton(tr("Apply"), transform_box);
    auto* cancel_button = new QPushButton(tr("Cancel"), transform_box);
    buttons->addWidget(preview_button);
    buttons->addWidget(apply_button);
    buttons->addWidget(cancel_button);
    transform_form->addRow(tr("Tool"), tool_);
    transform_form->addRow(tr("Amount"), amount_);
    transform_form->addRow(tr("Strength"), strength_);
    transform_form->addRow(tr("Seed"), seed_);
    transform_form->addRow(tr("Grid / rate"), grid_);
    advanced_ = new QCheckBox(tr("Show all parameters (JSON)"), transform_box);
    advanced_->setToolTip(tr("Every option of the tool as editable JSON, also used by saved recipes"));
    transform_form->addRow(advanced_);
    transform_form->addRow(tr("Parameters"), parameters_);
    transform_form->setRowVisible(parameters_, false);
    connect(advanced_, &QCheckBox::toggled, this, [this](bool on) { transform_form_->setRowVisible(parameters_, on); });
    transform_form->addRow(buttons);
    transform_form->addRow(report_);
    connect(tool_, &QComboBox::activated, this, &InspectorPanel::load_tool_defaults);
    update_tool_rows();
    for (auto* box : {amount_, strength_, seed_})
        connect(box, &QSpinBox::valueChanged, this, &InspectorPanel::update_parameters_from_form);
    connect(grid_, &QComboBox::activated, this, &InspectorPanel::update_parameters_from_form);
    connect(preview_button, &QPushButton::clicked, this, &InspectorPanel::preview);
    connect(apply_button, &QPushButton::clicked, this, &InspectorPanel::apply_preview);
    connect(cancel_button, &QPushButton::clicked, this, &InspectorPanel::cancel_preview);
    // Tools used on every edit come right after the selection; settings below fold away.
    column->insertWidget(column->indexOf(selection_box) + 1, transform_box);
    // Settings sections fold under an arrow header (a button, announced as expandable), not a
    // checkable group box.
    const auto collapsible = [column](QGroupBox* box, const QString& key, bool expanded_by_default) {
        const QString title = box->title();
        box->setTitle(QString());
        auto* header = new QToolButton(box->parentWidget());
        header->setText(title);
        header->setCheckable(true);
        header->setObjectName("sectionHeader");
        header->setAutoRaise(true);
        header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QFont bold = header->font();
        bold.setWeight(QFont::DemiBold);
        header->setFont(bold);
        header->setAccessibleName(title);
        header->setAccessibleDescription(QObject::tr("Shows or hides the %1 section").arg(title));
        column->insertWidget(column->indexOf(box), header);
        const bool expanded = QSettings().value("inspector/expanded/" + key, expanded_by_default).toBool();
        const auto apply = [box, header](bool on) {
            header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
            header->setChecked(on);
            box->setVisible(on);
        };
        apply(expanded);
        QObject::connect(header, &QToolButton::toggled, box, [key, apply](bool on) {
            apply(on);
            QSettings().setValue("inspector/expanded/" + key, on);
        });
    };
    collapsible(song_box, "song", true);
    collapsible(track_box, "track", false);
    collapsible(scale_box, "scale", false);
    collapsible(chord_box, "chord", false);
    column->addStretch(1);

    for (auto* form : content->findChildren<QFormLayout*>()) {
        form->setRowWrapPolicy(QFormLayout::WrapLongRows); // narrow panels put long labels above fields
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    }
    for (auto* combo : content->findChildren<QComboBox*>())
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    // Width follows the dock (long rows wrap); the content never forces a wider minimum.
    content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* scroll = new QScrollArea(this);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // the panel reflows; it never scrolls sideways
    scroll->setFrameShape(QFrame::NoFrame);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
    setEnabled(false);
}

InspectorPanel::~InspectorPanel() = default;

void InspectorPanel::set_controller(DocumentController* controller) {
    if (controller_)
        disconnect(controller_, nullptr, this, nullptr);
    controller_ = controller;
    pending_.reset();
    report_->clear();
    setEnabled(controller != nullptr);
    if (!controller)
        return;
    connect(controller, &DocumentController::selection_changed, this, &InspectorPanel::refresh_selection);
    connect(controller, &DocumentController::document_changed, this, &InspectorPanel::refresh);
    connect(controller, &DocumentController::view_state_changed, this, &InspectorPanel::refresh);
    refresh();
    load_tool_defaults();
}

void InspectorPanel::refresh() {
    if (!controller_)
        return;
    updating_ = true;
    const auto& sequence = controller_->active_sequence();
    {
        const auto& meter = controller_->meter_map();
        const auto& segment = meter.segment_at(0);
        song_tempo_->setValue(60'000'000.0 / static_cast<double>(controller_->tempo_map().tempo_at(0)));
        song_numerator_->setValue(segment.numerator);
        song_denominator_->setCurrentIndex(std::max(0, song_denominator_->findData(segment.denominator)));
        Tick loop_length = 0;
        if (sequence.settings.loop_region)
            for (const auto& region : sequence.regions)
                if (region.id == *sequence.settings.loop_region)
                    loop_length = region.end_tick - region.start_tick;
        const Tick bar = std::max<Tick>(1, meter.ticks_per_bar(segment));
        song_loop_bars_->setValue(static_cast<int>(std::clamp<Tick>((loop_length + bar - 1) / bar, 0, 999)));
        const bool ppq = sequence.division.is_ppq();
        for (QWidget* widget : std::initializer_list<QWidget*>{song_tempo_, song_numerator_, song_denominator_, song_loop_bars_})
            widget->setEnabled(ppq);
    }
    scale_root_->setCurrentIndex(sequence.root_pc);
    int scale_index = -1;
    for (int index = 0; index < scale_type_->count(); ++index) {
        auto scale = music::Scale::builtin(scale_type_->itemData(index).toString().toStdString(), sequence.root_pc);
        if (scale && scale->interval_mask() == sequence.scale_mask) {
            scale_index = index;
            break;
        }
    }
    if (scale_index >= 0)
        scale_type_->setCurrentIndex(scale_index);
    highlight_->setChecked(sequence.settings.highlight_scale);
    fold_->setChecked(sequence.settings.fold_to_scale);
    input_policy_->setCurrentIndex(static_cast<int>(sequence.settings.input_policy));
    const auto snap_index = snap_->findData(QString::fromStdString(sequence.settings.snap.to_string()));
    if (snap_index >= 0)
        snap_->setCurrentIndex(snap_index);
    if (const auto active = controller_->active_track_index()) {
        const auto& track = sequence.tracks[*active];
        track_name_->setText(QString::fromStdString(track.name));
        track_drum_->setChecked(track.is_drum);
        track_channel_override_->setCurrentIndex(track.routing.channel_override ? *track.routing.channel_override + 1 : 0);
        track_transpose_->setValue(track.routing.transpose);
        const int preset = track_preset_->findText(QString::fromStdString(track.instrument.synth_preset));
        track_preset_->setCurrentIndex(std::max(0, preset));
        track_program_->setValue(track.instrument.program + 1);
        const QString endpoint = track.routing.output == OutputKind::external_midi && track.routing.endpoint_id
                                     ? QString::fromStdString(*track.routing.endpoint_id)
                                     : QString();
        if (!endpoint.isEmpty() && track_output_->findData(endpoint) < 0)
            track_output_->addItem(tr("MIDI out: %1 (not connected)").arg(endpoint), endpoint); // unresolved, not substituted
        track_output_->setCurrentIndex(std::max(0, track_output_->findData(endpoint)));
    }
    updating_ = false;
    refresh_selection();
    if (pending_ && pending_generation_ != controller_->generation()) {
        pending_.reset(); // a stale preview is never applied
        report_->setText(tr("The document changed; preview again."));
    }
}

void InspectorPanel::refresh_selection() {
    if (!controller_)
        return;
    updating_ = true;
    const auto selected = controller_->selected_notes();
    selection_label_->setText(selected.empty()
                                  ? tr("Select notes to edit their start, length, pitch and velocity. "
                                       "With nothing selected, the tools below act on the active track.")
                                  : tr("%n note(s) selected", nullptr, static_cast<int>(selected.size())));
    // No fake values when nothing is selected: the numeric rows are hidden.
    for (auto* box : {start_, length_, pitch_, velocity_, channel_})
        selection_form_->setRowVisible(box, !selected.empty());
    selection_form_->setRowVisible(chord_label_, selected.size() >= 2);
    const bool single = selected.size() == 1;
    for (auto* box : {start_, length_, pitch_, channel_})
        box->setEnabled(single);
    velocity_->setEnabled(!selected.empty());
    if (single) {
        const auto& note = selected.front();
        start_->setValue(static_cast<int>(std::min<Tick>(note.start, INT32_MAX)));
        length_->setValue(static_cast<int>(std::clamp<Tick>(note.duration(), 1, INT32_MAX)));
        pitch_->setValue(note.pitch);
        velocity_->setValue(note.velocity);
        channel_->setValue(note.channel + 1);
        pitch_->setSuffix(QString("  (%1)").arg(QString::fromStdString(pitch_name(note.pitch))));
    } else if (!selected.empty()) {
        velocity_->setValue(selected.front().velocity);
    }
    update_position_suffixes();
    shown_ = {start_->value(), length_->value(), pitch_->value(), velocity_->value(), channel_->value()};
    updating_ = false;
    recognize_selection();
}

void InspectorPanel::recognize_selection() {
    const auto selected = controller_ ? controller_->selected_notes() : std::vector<NoteSpan>{};
    if (selected.size() < 2) {
        chord_label_->clear();
        return;
    }
    std::vector<int> pitches;
    for (const auto& note : selected)
        pitches.push_back(note.pitch);
    // Advisory recognition: alternatives are listed, notes are never rewritten (section 9.4).
    const auto candidates = music::recognize_chord(music::pitch_class_mask(pitches),
                                                   *std::min_element(pitches.begin(), pitches.end()) % 12);
    QStringList names;
    for (std::size_t index = 0; index < candidates.size() && index < 4; ++index)
        names << QString::fromStdString(candidates[index].symbol);
    chord_label_->setText(names.isEmpty() ? tr("No chord match") : names.join(" / "));
}

void InspectorPanel::apply_note_fields() {
    if (updating_ || !controller_)
        return;
    const auto selected = controller_->selected_notes();
    if (selected.empty())
        return;
    // editingFinished also fires on focus loss: only fields whose value differs from what was shown
    // are applied, so focusing Velocity on a mixed selection does not flatten it.
    const std::array<int, 5> now{start_->value(), length_->value(), pitch_->value(), velocity_->value(), channel_->value()};
    if (now == shown_)
        return;
    const bool single = selected.size() == 1;
    std::vector<NoteUpdate> updates;
    for (const auto& note : selected) {
        NoteUpdate update{note.on_id, note.start, note.end, note.pitch, note.velocity, note.channel, note.release_velocity};
        if (single) {
            if (now[0] != shown_[0] || now[1] != shown_[1]) {
                update.start = now[0];
                update.end = update.start + now[1];
            }
            if (now[2] != shown_[2])
                update.pitch = static_cast<std::uint8_t>(now[2]);
            if (now[4] != shown_[4])
                update.channel = static_cast<std::uint8_t>(now[4] - 1);
        }
        if (now[3] != shown_[3])
            update.velocity = static_cast<std::uint8_t>(now[3]);
        updates.push_back(update);
    }
    shown_ = now;
    auto changes = update_notes(controller_->document(), controller_->active_sequence().id, updates, "Edit note properties");
    if (changes)
        controller_->apply(*changes);
    else
        emit status_message(QString::fromStdString(changes.error().message));
}

void InspectorPanel::apply_track_fields() {
    if (updating_ || !controller_)
        return;
    const auto active = controller_->active_track_index();
    if (!active)
        return;
    Track updated = track_header(controller_->active_sequence().tracks[*active]); // properties only, no events
    updated.name = track_name_->text().toStdString();
    updated.is_drum = track_drum_->isChecked();
    const int override_index = track_channel_override_->currentIndex();
    updated.routing.channel_override =
        override_index > 0 ? std::optional<std::uint8_t>(static_cast<std::uint8_t>(override_index - 1)) : std::nullopt;
    updated.routing.transpose = track_transpose_->value();
    updated.instrument.synth_preset = track_preset_->currentText().toStdString();
    updated.instrument.program = static_cast<std::uint8_t>(track_program_->value() - 1);
    const QString endpoint = track_output_->currentData().toString();
    updated.routing.output = endpoint.isEmpty() ? OutputKind::internal_synth : OutputKind::external_midi;
    updated.routing.endpoint_id = endpoint.isEmpty() ? std::nullopt : std::optional<std::string>(endpoint.toStdString());
    ChangeBuilder builder(controller_->document(), "Edit track");
    builder.set_track_properties(updated.id, updated);
    controller_->apply(builder.finish());
}

void InspectorPanel::apply_song_fields() {
    if (updating_ || !controller_ || !controller_->active_sequence().division.is_ppq())
        return;
    const auto& sequence = controller_->active_sequence();
    const auto tempo = static_cast<std::uint32_t>(std::llround(60'000'000.0 / song_tempo_->value()));
    const auto numerator = static_cast<std::uint8_t>(song_numerator_->value());
    const int denominator = song_denominator_->currentData().toInt();
    std::uint8_t power = 0;
    while ((1 << power) < denominator)
        ++power;
    ChangeBuilder builder(controller_->document(), "Edit tempo, meter or loop");
    // Replace the tempo and time signature in effect at bar 1, or add them to the first track.
    const auto replace_or_insert = [&](std::uint8_t type, const Event& replacement) {
        for (const auto& track : sequence.tracks)
            for (const auto& event : track.events) {
                if (event.tick > 0)
                    break;
                if (event.is_meta(type)) {
                    if (!(event.payload == replacement.payload)) {
                        Event updated = event;
                        updated.payload = replacement.payload;
                        builder.replace(track.id, event.id, updated);
                    }
                    return;
                }
            }
        if (!sequence.tracks.empty())
            std::ignore = builder.insert(sequence.tracks.front().id, replacement, OrderPlacement::before_existing);
    };
    replace_or_insert(meta_type::tempo, events::tempo(0, tempo));
    replace_or_insert(meta_type::time_signature, events::time_signature(0, numerator, power));
    // Loop length in bars of the new meter, from the loop start (a loop region is created if none).
    const Tick bar = std::max<Tick>(1, static_cast<Tick>(numerator) * 4 * sequence.division.ppq / denominator);
    const Tick length = bar * song_loop_bars_->value();
    Sequence updated = sequence_header(sequence); // properties only, no events
    bool found = false;
    if (song_loop_bars_->value() == 0) {
        updated.settings.loop_region.reset(); // "No loop": the region stays, it just stops looping
        found = true;
    } else if (updated.settings.loop_region)
        for (auto& region : updated.regions)
            if (region.id == *updated.settings.loop_region) {
                region.end_tick = region.start_tick + length;
                found = true;
            }
    if (!found) // reuse a loop region that was switched off, if any
        for (auto& region : updated.regions)
            if (region.is_loop) {
                region.end_tick = region.start_tick + length;
                updated.settings.loop_region = region.id;
                found = true;
                break;
            }
    if (!found) {
        Region loop;
        loop.id = RegionId::generate();
        loop.name = "Loop";
        loop.start_tick = 0;
        loop.end_tick = length;
        loop.is_loop = true;
        updated.settings.loop_region = loop.id;
        updated.regions.push_back(std::move(loop));
    }
    builder.set_sequence_properties(sequence.id, updated);
    auto changes = builder.finish();
    if (!changes.empty())
        controller_->apply(changes);
}

void InspectorPanel::apply_scale_fields() {
    if (updating_ || !controller_)
        return;
    const auto& sequence = controller_->active_sequence();
    auto scale = music::Scale::builtin(scale_type_->currentData().toString().toStdString(), scale_root_->currentIndex());
    if (!scale)
        return;
    Sequence updated = sequence_header(sequence); // properties only, no events
    updated.root_pc = static_cast<std::uint8_t>(scale->root_pitch_class());
    updated.scale_mask = scale->interval_mask();
    updated.settings.highlight_scale = highlight_->isChecked();
    updated.settings.fold_to_scale = fold_->isChecked();
    updated.settings.input_policy = static_cast<InputScalePolicy>(input_policy_->currentIndex());
    if (auto snap = Rational::parse(snap_->currentData().toString().toStdString()))
        updated.settings.snap = *snap;
    ChangeBuilder builder(controller_->document(), "Change scale or grid");
    builder.set_sequence_properties(sequence.id, updated);
    controller_->apply(builder.finish()); // display settings only; no event changes (section 9.1)
}

void InspectorPanel::load_tool_defaults() {
    const QString tool = tool_->currentData().toString();
    updating_ = true;
    if (tool == "transpose")
        amount_->setValue(12);
    else if (tool == "velocity")
        amount_->setValue(100);
    else if (tool == "humanize")
        amount_->setValue(10);
    else if (tool == "length")
        amount_->setValue(240);
    else if (tool == "invert_pitch")
        amount_->setValue(60);
    else if (tool == "repeat")
        amount_->setValue(1);
    else if (tool == "strum")
        amount_->setValue(60);
    else if (tool == "euclidean")
        amount_->setValue(5);
    else
        amount_->setValue(0);
    updating_ = false;
    update_tool_rows();
    update_parameters_from_form();
}

void InspectorPanel::update_tool_rows() {
    // Tool -> label of the Amount field (empty: not used), and whether strength, seed and grid apply.
    struct Rows {
        const char* tool;
        const char* amount;
        bool strength;
        bool seed;
        const char* grid;
    };
    static constexpr Rows rows[] = {
        {"quantize", "", true, false, "Grid"},           {"humanize", "Timing ± (ticks)", false, true, ""},
        {"transpose", "Semitones", false, false, ""},     {"velocity", "Velocity (1-127)", false, false, ""},
        {"length", "Length (ticks)", false, false, ""},   {"time_stretch", "", false, false, ""},
        {"reverse", "", false, false, ""},                {"invert_pitch", "Axis pitch (0-127)", false, false, ""},
        {"rotate", "Shift (notes)", false, false, ""},    {"repeat", "Copies", false, false, ""},
        {"strum", "Spread (ticks)", false, true, ""},     {"arpeggiate", "", false, true, "Rate"},
        {"euclidean", "Pulses", false, false, "Step"},    {"remove_duplicates", "", false, false, ""},
        {"resolve_overlaps", "", false, false, ""},       {"join", "Max gap (ticks)", false, false, ""},
        {"conform_to_scale", "", false, false, ""},
    };
    const QString tool = tool_->currentData().toString();
    for (const auto& row : rows) {
        if (tool != QLatin1String(row.tool))
            continue;
        const QString amount = tr(row.amount);
        transform_form_->setRowVisible(amount_, !amount.isEmpty());
        if (auto* label = qobject_cast<QLabel*>(transform_form_->labelForField(amount_)))
            label->setText(amount);
        transform_form_->setRowVisible(strength_, row.strength);
        transform_form_->setRowVisible(seed_, row.seed);
        const QString grid = tr(row.grid);
        transform_form_->setRowVisible(grid_, !grid.isEmpty());
        if (auto* label = qobject_cast<QLabel*>(transform_form_->labelForField(grid_)))
            label->setText(grid);
    }
}

void InspectorPanel::update_position_suffixes() {
    if (!controller_)
        return;
    // Musicians read positions as bar.beat.tick; the tick value stays editable.
    const auto& meter = controller_->meter_map();
    start_->setSuffix(QStringLiteral("  (%1)").arg(QString::fromStdString(meter.format(start_->value()))));
    const Tick beat = std::max<Tick>(1, meter.ticks_per_beat(meter.segment_at(start_->value())));
    const double beats = static_cast<double>(length_->value()) / static_cast<double>(beat);
    length_->setSuffix(tr("  (%1 beats)").arg(beats, 0, 'g', 3));
}

void InspectorPanel::update_parameters_from_form() {
    if (updating_ || !controller_)
        return;
    const auto grid = Rational::parse(grid_->currentData().toString().toStdString()).value_or(Rational{1, 16});
    const auto parameters = parameters_for(tool_->currentData().toString(), amount_->value(), strength_->value(),
                                           static_cast<std::uint64_t>(seed_->value()), grid,
                                           controller_->active_sequence());
    if (parameters)
        parameters_->setPlainText(QString::fromStdString(music::parameters_to_json(*parameters)));
    pending_.reset();
    report_->clear();
}

void InspectorPanel::preview() {
    if (!controller_)
        return;
    auto parameters = music::parameters_from_json(parameters_->toPlainText().toStdString());
    if (!parameters) {
        report_->setText(tr("Parameters are invalid: %1").arg(QString::fromStdString(parameters.error().message)));
        return;
    }
    music::TargetSelection selection;
    selection.sequence = controller_->active_sequence().id;
    selection.tracks = {controller_->active_track()};
    for (const auto& id : controller_->selection())
        selection.notes.push_back(id);
    auto result = music::preview_transform(controller_->document(), selection, *parameters);
    if (!result) {
        report_->setText(QString::fromStdString(result.error().describe()));
        pending_.reset();
        return;
    }
    const auto& report = result->report;
    QStringList lines;
    lines << tr("%1 targeted, %2 changed, %3 created, %4 removed").arg(report.targeted).arg(report.changed)
                 .arg(report.created).arg(report.removed);
    if (report.clipped || report.skipped || report.collisions || report.ambiguous)
        lines << tr("%1 clipped, %2 skipped, %3 collisions, %4 ambiguous").arg(report.clipped).arg(report.skipped)
                     .arg(report.collisions).arg(report.ambiguous);
    if (report.max_movement)
        lines << tr("Largest move: %1 ticks").arg(report.max_movement);
    for (const auto& message : report.messages)
        lines << QString::fromStdString(message);
    report_->setText(lines.join('\n'));
    pending_ = std::make_unique<music::TransformPreview>(std::move(*result));
    pending_generation_ = controller_->generation();
    if (tool_->currentData().toString() == "quantize")
        last_quantize_json_ = parameters_->toPlainText();
}

void InspectorPanel::apply_preview() {
    if (!controller_)
        return;
    if (!pending_ || pending_generation_ != controller_->generation())
        preview();
    if (!pending_)
        return;
    const auto created = pending_->created_notes;
    if (controller_->apply(pending_->changes) && !created.empty())
        controller_->set_selection(std::set<EventId>(created.begin(), created.end()));
    pending_.reset();
}

void InspectorPanel::cancel_preview() {
    pending_.reset(); // cancel leaves the document unchanged
    report_->setText(tr("Cancelled"));
}

void InspectorPanel::select_tool(const QString& tool_id) {
    const int index = tool_->findData(tool_id);
    if (index >= 0 && index != tool_->currentIndex()) {
        tool_->setCurrentIndex(index);
        load_tool_defaults();
    }
}

void InspectorPanel::focus_tool(const QString& tool_id) {
    select_tool(tool_id);
    tool_->setFocus();
}

void InspectorPanel::apply_last_quantize() {
    if (!controller_)
        return;
    if (controller_->selection().empty()) {
        emit status_message(tr("Select notes to quantize (Q applies the last quantize settings)"));
        return;
    }
    // Keyboard focus stays where it is; the user's chosen tool is restored afterwards.
    const int previous_tool = tool_->currentIndex();
    const QString previous_parameters = parameters_->toPlainText();
    select_tool("quantize");
    if (!last_quantize_json_.isEmpty())
        parameters_->setPlainText(last_quantize_json_);
    preview();
    apply_preview();
    if (previous_tool != tool_->currentIndex()) {
        tool_->setCurrentIndex(previous_tool);
        parameters_->setPlainText(previous_parameters);
    }
}

void InspectorPanel::insert_chord() {
    if (!controller_ || !controller_->active_track_index())
        return;
    music::ChordSpec spec;
    spec.root_pitch = (chord_octave_->value() + 1) * 12 + chord_root_->currentIndex();
    spec.type = static_cast<music::ChordType>(chord_type_->currentData().toInt());
    spec.inversion = chord_inversion_->value();
    music::ChordRenderSpec render;
    render.start = controller_->edit_cursor();
    const auto& sequence = controller_->active_sequence();
    render.duration = sequence.division.is_ppq() ? sequence.division.ppq : 960;
    render.strum = chord_strum_->value();
    render.velocity = sequence.settings.default_velocity;
    auto result = music::insert_chord(controller_->document(), controller_->active_track(), spec, render);
    if (!result) {
        emit status_message(QString::fromStdString(result.error().describe()));
        return;
    }
    if (controller_->apply(result->changes))
        controller_->set_selection(std::set<EventId>(result->created_notes.begin(), result->created_notes.end()));
}

} // namespace mwb::ui
