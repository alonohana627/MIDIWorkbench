#include "mwb/ui/scale_bar.hpp"

#include "mwb/core/change_set.hpp"
#include "mwb/core/pitch.hpp"
#include "mwb/music/analysis.hpp"
#include "mwb/music/scale.hpp"
#include "mwb/ui/document_controller.hpp"

#include <algorithm>
#include <vector>

#include <QComboBox>
#include <QHBoxLayout>
#include <QToolButton>

namespace mwb::ui {

ScaleBar::ScaleBar(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 6, 0);
    layout->setSpacing(4);
    const auto make_toggle = [this](const QString& text, const QString& tip) {
        auto* button = new QToolButton(this);
        button->setText(text);
        button->setCheckable(true);
        button->setToolTip(tip);
        button->setAccessibleName(text);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        return button;
    };
    scale_ = make_toggle(tr("Scale"), tr("Highlight the scale's rows and mark notes outside the scale"));
    root_ = new QComboBox(this);
    root_->setAccessibleName(tr("Scale root"));
    root_->setToolTip(tr("Scale root"));
    for (int pitch_class = 0; pitch_class < 12; ++pitch_class)
        root_->addItem(QString::fromStdString(pitch_class_name(pitch_class)));
    type_ = new QComboBox(this);
    type_->setAccessibleName(tr("Scale type"));
    type_->setToolTip(tr("Scale type"));
    for (const auto& definition : music::builtin_scales())
        type_->addItem(QString::fromUtf8(definition.name.data(), static_cast<qsizetype>(definition.name.size())),
                       QString::fromUtf8(definition.id.data(), static_cast<qsizetype>(definition.id.size())));
    type_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    type_->setMinimumContentsLength(12);
    fold_ = make_toggle(tr("Fold"), tr("Show only the scale's rows and rows that hold notes"));
    select_ = new QToolButton(this);
    select_->setText(tr("Select outside"));
    select_->setToolTip(tr("Select the active track's notes that are outside the scale"));
    select_->setAccessibleName(tr("Select notes outside the scale"));
    select_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    layout->addWidget(scale_);
    layout->addWidget(root_);
    layout->addWidget(type_);
    layout->addWidget(fold_);
    layout->addWidget(select_);
    connect(scale_, &QToolButton::toggled, this, [this](bool on) {
        if (on && !updating_ && type_->currentData().toString() == QLatin1String("chromatic"))
            choose_scale_from_notes(); // every note is in a chromatic scale: nothing would show
        apply();
    });
    connect(fold_, &QToolButton::toggled, this, &ScaleBar::apply);
    connect(root_, &QComboBox::activated, this, &ScaleBar::apply);
    connect(type_, &QComboBox::activated, this, &ScaleBar::apply);
    connect(select_, &QToolButton::clicked, this, &ScaleBar::select_outside_requested);
    set_controller(nullptr);
}

void ScaleBar::set_controller(DocumentController* controller) {
    if (controller_)
        disconnect(controller_, nullptr, this, nullptr);
    controller_ = controller;
    if (controller_)
        connect(controller_, &DocumentController::document_changed, this, &ScaleBar::refresh);
    refresh();
}

void ScaleBar::refresh() {
    const bool enabled = controller_ != nullptr;
    for (QWidget* widget : std::initializer_list<QWidget*>{scale_, root_, type_, fold_, select_})
        widget->setEnabled(enabled);
    if (!controller_)
        return;
    updating_ = true;
    const auto& sequence = controller_->active_sequence();
    scale_->setChecked(sequence.settings.highlight_scale);
    fold_->setChecked(sequence.settings.fold_to_scale);
    root_->setCurrentIndex(sequence.root_pc % 12);
    // A custom mask no built-in scale matches keeps the current choice in view.
    for (int index = 0; index < type_->count(); ++index) {
        auto scale = music::Scale::builtin(type_->itemData(index).toString().toStdString(), sequence.root_pc);
        if (scale && scale->interval_mask() == sequence.scale_mask) {
            type_->setCurrentIndex(index);
            break;
        }
    }
    // Selecting outside the scale only makes sense while the scale is in use.
    select_->setEnabled(sequence.settings.highlight_scale || sequence.settings.fold_to_scale);
    updating_ = false;
}

void ScaleBar::choose_scale_from_notes() {
    if (!controller_)
        return;
    const auto& sequence = controller_->active_sequence();
    std::vector<NoteSpan> pitched;
    for (const auto& note : controller_->notes().notes)
        if (note.on_track < sequence.tracks.size() && !sequence.tracks[note.on_track].is_drum)
            pitched.push_back(note);
    int root = 0;
    QString type = QStringLiteral("major");
    const auto estimate = music::estimate_key(pitched);
    if (!estimate.unknown && estimate.best) {
        root = estimate.best->root_pitch_class;
        const std::string mode = estimate.best->mode == "minor" ? std::string("natural_minor") : estimate.best->mode;
        if (music::find_builtin_scale(mode))
            type = QString::fromStdString(mode);
    }
    root_->setCurrentIndex(std::clamp(root, 0, 11));
    if (const int index = type_->findData(type); index >= 0)
        type_->setCurrentIndex(index);
}

void ScaleBar::apply() {
    if (updating_ || !controller_)
        return;
    const auto& sequence = controller_->active_sequence();
    auto scale = music::Scale::builtin(type_->currentData().toString().toStdString(), root_->currentIndex());
    if (!scale)
        return;
    Sequence updated = sequence_header(sequence); // properties only, no events
    updated.root_pc = static_cast<std::uint8_t>(scale->root_pitch_class());
    updated.scale_mask = scale->interval_mask();
    updated.settings.highlight_scale = scale_->isChecked();
    updated.settings.fold_to_scale = fold_->isChecked();
    if (updated.root_pc == sequence.root_pc && updated.scale_mask == sequence.scale_mask &&
        updated.settings == sequence.settings) {
        refresh();
        return;
    }
    ChangeBuilder builder(controller_->document(), "Change scale");
    builder.set_sequence_properties(sequence.id, updated);
    controller_->apply(builder.finish()); // display settings only; no event changes (section 9.1)
    refresh();
}

} // namespace mwb::ui
