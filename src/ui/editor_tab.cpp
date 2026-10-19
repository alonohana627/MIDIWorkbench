#include "mwb/ui/editor_tab.hpp"

#include "mwb/ui/icons.hpp"

#include "mwb/ui/document_controller.hpp"
#include "mwb/ui/drum_grid.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QKeyEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace mwb::ui {

namespace {

enum TrackColumn { column_name = 0, column_mute, column_solo, column_count };

QColor color_from_rgba(std::uint32_t rgba) {
    return QColor(static_cast<int>((rgba >> 24) & 0xFF), static_cast<int>((rgba >> 16) & 0xFF),
                  static_cast<int>((rgba >> 8) & 0xFF));
}

} // namespace

TrackListWidget::TrackListWidget(DocumentController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    // Type-2 files hold several sequences (spec 12: "a type-2 import displays a sequence
    // selector"); the selector is shown only when there is more than one.
    sequence_ = new QComboBox(this);
    sequence_->setAccessibleName(tr("Sequence"));
    sequence_->setToolTip(tr("This file holds several independent sequences; choose the one to show and edit"));
    connect(sequence_, &QComboBox::activated, this, [this](int index) {
        if (!rebuilding_ && index >= 0)
            controller_.set_active_sequence(static_cast<std::size_t>(index));
    });
    table_ = new QTableWidget(this);
    table_->installEventFilter(this);
    table_->setColumnCount(column_count);
    table_->setHorizontalHeaderLabels({tr("Track"), tr("M"), tr("S")});
    table_->horizontalHeader()->setSectionResizeMode(column_name, QHeaderView::Stretch);
    for (int column = column_mute; column < column_count; ++column)
        table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    // Typing never renames a track (letters and digits are editor commands); F2 or a
    // double-click does.
    table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    table_->setAccessibleName(tr("Tracks"));
    table_->setToolTip(tr("M mute, S solo. Recording goes to the selected track. Double-click a name to rename."));
    auto* buttons = new QHBoxLayout;
    table_->verticalHeader()->setDefaultSectionSize(28);
    const auto make_button = [&](IconId icon, const QString& tip, auto slot) {
        auto* button = new QToolButton(this);
        button->setIcon(make_icon(icon));
        button->setAutoRaise(true);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setMinimumSize(28, 28);
        connect(button, &QToolButton::clicked, this, slot);
        buttons->addWidget(button);
    };
    make_button(IconId::add, tr("Add track"), &TrackListWidget::add_track);
    make_button(IconId::duplicate, tr("Duplicate active track"), &TrackListWidget::duplicate_active_track);
    make_button(IconId::remove, tr("Delete active track"), &TrackListWidget::delete_active_track);
    delete_button_ = buttons->itemAt(buttons->count() - 1)->widget();
    make_button(IconId::up, tr("Move track up"), [this] { move_active_track(-1); });
    make_button(IconId::down, tr("Move track down"), [this] { move_active_track(1); });
    buttons->addStretch(1);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(sequence_);
    layout->addWidget(table_, 1);
    layout->addLayout(buttons);
    connect(&controller_, &DocumentController::document_changed, this, &TrackListWidget::rebuild);
    connect(&controller_, &DocumentController::view_state_changed, this, &TrackListWidget::rebuild);
    connect(table_, &QTableWidget::currentCellChanged, this, [this](int row, int column, int, int) {
        // Only the name column chooses the active (edited and recorded) track: clicking another
        // track's M or S mutes or solos it without switching the editor (ux-panels-6).
        if (rebuilding_ || row < 0 || column != column_name)
            return;
        const auto& tracks = controller_.active_sequence().tracks;
        if (static_cast<std::size_t>(row) < tracks.size())
            controller_.set_active_track(tracks[static_cast<std::size_t>(row)].id);
    });
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (rebuilding_)
            return;
        const auto& tracks = controller_.active_sequence().tracks;
        if (static_cast<std::size_t>(item->row()) >= tracks.size())
            return;
        const auto& track = tracks[static_cast<std::size_t>(item->row())];
        if (item->column() == column_name) {
            const std::string name = item->text().toStdString();
            if (name == track.name)
                return;
            ChangeBuilder builder(controller_.document(), "Rename track");
            Track updated = track_header(track); // properties only, no events
            updated.name = name;
            builder.set_track_properties(track.id, updated);
            controller_.apply(builder.finish());
        } else {
            set_track_flag(track.id, item->column(), item->checkState() == Qt::Checked);
        }
    });
    rebuild();
}


bool TrackListWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) && key->modifiers() == Qt::NoModifier) {
            delete_active_track(); // undoable; the last track is kept (a rename editor has its own focus)
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TrackListWidget::rebuild() {
    rebuilding_ = true;
    {
        const auto& sequences = controller_.document().sequences;
        sequence_->clear();
        for (std::size_t index = 0; index < sequences.size(); ++index) {
            const auto& name = sequences[index].name;
            sequence_->addItem(name.empty() ? tr("Sequence %1").arg(index + 1)
                                            : tr("Sequence %1: %2").arg(index + 1).arg(QString::fromStdString(name)));
        }
        sequence_->setCurrentIndex(static_cast<int>(controller_.active_sequence_index()));
        sequence_->setVisible(sequences.size() > 1);
    }
    const auto& tracks = controller_.active_sequence().tracks;
    if (delete_button_) {
        delete_button_->setEnabled(tracks.size() > 1); // a sequence keeps at least one track
        delete_button_->setToolTip(tracks.size() > 1 ? tr("Delete active track") : tr("The last track cannot be deleted"));
    }
    const int current_column = table_->currentColumn();
    table_->setRowCount(static_cast<int>(tracks.size()));
    const auto active = controller_.active_track_index();
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        const auto& track = tracks[index];
        const int row = static_cast<int>(index);
        auto* name = new QTableWidgetItem(QString::fromStdString(track.name));
        if (active && *active == index) { // the edited track reads bold even when another row is current
            QFont bold = name->font();
            bold.setBold(true);
            name->setFont(bold);
        }
        name->setData(Qt::DecorationRole, color_from_rgba(track.color_rgba));
        name->setToolTip(track.is_drum ? tr("%1 (drum track)").arg(QString::fromStdString(track.name))
                                       : QString::fromStdString(track.name));
        table_->setItem(row, column_name, name);
        const auto check = [&](int column, bool value, const QString& tip) {
            auto* item = new QTableWidgetItem;
            item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            item->setCheckState(value ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(tip);
            item->setData(Qt::AccessibleTextRole, tip); // screen readers say what the box does
            table_->setItem(row, column, item);
        };
        const QString track_name = QString::fromStdString(track.name);
        check(column_mute, track.muted, tr("Mute %1").arg(track_name));
        check(column_solo, track.solo, tr("Solo %1").arg(track_name));
    }
    // Keep the focused column (Mute or Solo stays current after it was toggled from the keyboard).
    if (active)
        table_->setCurrentCell(static_cast<int>(*active), std::max(current_column, 0));
    rebuilding_ = false;
}

void TrackListWidget::set_track_flag(TrackId track_id, int column, bool value) {
    const auto index = controller_.active_sequence().track_index(track_id);
    if (!index)
        return;
    Track updated = track_header(controller_.active_sequence().tracks[*index]); // properties only, no events
    if (column == column_mute)
        updated.muted = value;
    else if (column == column_solo)
        updated.solo = value;
    ChangeBuilder builder(controller_.document(), column == column_mute ? "Mute track" : "Solo track");
    builder.set_track_properties(track_id, updated);
    controller_.apply(builder.finish());
}

void TrackListWidget::add_track() {
    const auto& sequence = controller_.active_sequence();
    Track track;
    track.id = TrackId::generate();
    track.name = "Track " + std::to_string(sequence.tracks.size() + 1);
    const QColor color = QColor::fromHsv(static_cast<int>((sequence.tracks.size() * 47) % 360), 150, 220);
    track.color_rgba = ((static_cast<std::uint32_t>(color.rgb()) & 0xFFFFFFu) << 8) | 0xFFu;
    track.end_tick = sequence.end_tick;
    ChangeBuilder builder(controller_.document(), "Add track");
    builder.append(TrackInsert{sequence.id, sequence.tracks.size(), track});
    if (controller_.apply(builder.finish()))
        controller_.set_active_track(track.id);
}

void TrackListWidget::delete_active_track() {
    const auto& sequence = controller_.active_sequence();
    const auto index = controller_.active_track_index();
    if (!index || sequence.tracks.size() <= 1)
        return; // an editable sequence keeps at least one track
    ChangeBuilder builder(controller_.document(), "Delete track");
    builder.append(TrackRemove{sequence.id, *index, sequence.tracks[*index]});
    controller_.apply(builder.finish());
}

void TrackListWidget::duplicate_active_track() {
    const auto& sequence = controller_.active_sequence();
    const auto index = controller_.active_track_index();
    if (!index)
        return;
    Track copy = sequence.tracks[*index];
    copy.id = TrackId::generate();
    copy.name += " copy";
    for (auto& event : copy.events)
        event.id = EventId::generate(); // a duplicate has its own identities
    ChangeBuilder builder(controller_.document(), "Duplicate track");
    builder.append(TrackInsert{sequence.id, *index + 1, std::move(copy)});
    controller_.apply(builder.finish());
}

void TrackListWidget::move_active_track(int direction) {
    const auto& sequence = controller_.active_sequence();
    const auto index = controller_.active_track_index();
    if (!index)
        return;
    const auto target = static_cast<std::ptrdiff_t>(*index) + direction;
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(sequence.tracks.size()))
        return;
    ChangeBuilder builder(controller_.document(), "Move track");
    builder.append(TrackMove{sequence.id, *index, static_cast<std::size_t>(target), sequence.tracks[*index].id});
    controller_.apply(builder.finish());
}

// --- EditorTab -----------------------------------------------------------------------------------

EditorTab::EditorTab(std::unique_ptr<DocumentController> controller, QWidget* parent)
    : QWidget(parent), controller_(std::move(controller)) {
    controller_->setParent(nullptr);
    track_list_ = new TrackListWidget(*controller_, this);
    piano_roll_ = new PianoRollPanel(*controller_, this);
    drum_grid_ = new DrumGridPanel(*controller_, this);
    center_ = new QStackedWidget(this);
    center_->addWidget(piano_roll_);
    center_->addWidget(drum_grid_);
    lanes_container_ = new QWidget(this);
    auto* lanes_layout = new QVBoxLayout(lanes_container_);
    lanes_layout->setContentsMargins(0, 0, 0, 0);
    lanes_layout->setSpacing(1);
    keyboard_ = new KeyboardWidget(this);
    keyboard_->setMinimumHeight(56);

    auto* vertical = new QSplitter(Qt::Vertical, this);
    vertical->addWidget(center_);
    vertical->addWidget(lanes_container_);
    vertical->addWidget(keyboard_);
    vertical->setStretchFactor(0, 1);
    vertical->setStretchFactor(1, 0);
    vertical->setStretchFactor(2, 0);
    vertical->setCollapsible(0, false);
    vertical->setSizes({640, 80, 64});
    keyboard_->hide(); // shown with playing mode or View > On-screen Keyboard
    auto* horizontal = new QSplitter(Qt::Horizontal, this);
    horizontal->addWidget(track_list_);
    horizontal->addWidget(vertical);
    horizontal->setStretchFactor(0, 0);
    horizontal->setStretchFactor(1, 1);
    horizontal->setSizes({140, 1000});
    horizontal->setCollapsible(0, true); // drag the divider left to hide the track list
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(horizontal);

    add_lane(LaneTarget{LaneTarget::Kind::velocity, 0, 0});
    // Tab goes from the track list (and its buttons) straight to the editor surface.
    QWidget::setTabOrder(track_list_, &piano_roll_->view());
    connect(&piano_roll_->view(), &PianoRollView::mapping_changed, this, &EditorTab::sync_lanes);
    connect(controller_.get(), &DocumentController::document_changed, this, &EditorTab::sync_lanes);
    connect(&drum_grid_->view(), &DrumGridView::status_message, this, &EditorTab::status_message);
    connect(&drum_grid_->view(), &DrumGridView::open_in_piano_roll, this, [this](Tick tick, int) {
        show_drum_grid(false);
        piano_roll_->view().scroll_to_tick(tick);
    });
    // The grid follows the active track: drum tracks open in the grid, others in the piano roll.
    const auto follow_track = [this] {
        const auto& sequence = controller_->active_sequence();
        const auto active = controller_->active_track_index();
        const bool drum = active && sequence.tracks[*active].is_drum;
        if (drum != last_track_was_drum_) {
            last_track_was_drum_ = drum;
            show_drum_grid(drum);
        }
    };
    connect(controller_.get(), &DocumentController::view_state_changed, this, follow_track);
    connect(controller_.get(), &DocumentController::document_changed, this, follow_track);
    follow_track();
}

EditorTab::~EditorTab() {
    // Child widgets reference the controller; destroy them first.
    const auto children = findChildren<QWidget*>(Qt::FindDirectChildrenOnly);
    for (auto* child : children)
        delete child;
}

void EditorTab::show_drum_grid(bool show) {
    center_->setCurrentWidget(show ? static_cast<QWidget*>(drum_grid_) : piano_roll_);
    lanes_container_->setVisible(!show); // lanes follow the piano roll's time axis, not the grid's
}

DrumGridView& EditorTab::drum_grid() { return drum_grid_->view(); }

void EditorTab::set_playhead(std::optional<Tick> tick) {
    piano_roll().set_playhead(tick);
    drum_grid_->view().set_playhead(tick);
}

void EditorTab::set_follow_playhead(bool follow) {
    piano_roll().set_follow_playhead(follow);
    drum_grid_->view().set_follow_playhead(follow);
}

bool EditorTab::showing_drum_grid() const { return center_->currentWidget() == drum_grid_; }

void EditorTab::add_lane(LaneTarget target) {
    auto* lane = new ControllerLane(*controller_, lanes_container_);
    lane->set_target(target);
    lane->setMinimumHeight(60);
    lanes_container_->layout()->addWidget(lane);
    lanes_.push_back(lane);
    connect(lane, &ControllerLane::remove_requested, this, &EditorTab::remove_lane);
    connect(lane, &ControllerLane::status_message, this, &EditorTab::status_message);
    sync_lanes();
}

void EditorTab::remove_lane(ControllerLane* lane) {
    const auto found = std::find(lanes_.begin(), lanes_.end(), lane);
    if (found == lanes_.end())
        return;
    lanes_.erase(found);
    lane->deleteLater(); // the lane's own context menu is still on the stack
}

void EditorTab::remove_last_lane() {
    if (lanes_.empty())
        return;
    delete lanes_.back();
    lanes_.pop_back();
}

void EditorTab::sync_lanes() {
    auto& view = piano_roll_->view();
    for (auto* lane : lanes_)
        lane->set_mapping(view.mapping(), PianoRollView::keyboard_width, view.grid());
}

} // namespace mwb::ui
