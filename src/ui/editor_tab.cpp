#include "mwb/ui/editor_tab.hpp"

#include "mwb/ui/icons.hpp"

#include "mwb/ui/document_controller.hpp"
#include "mwb/ui/drum_grid.hpp"
#include "mwb/ui/instrument_menu.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QMenu>
#include <QComboBox>
#include <QKeyEvent>
#include <QHBoxLayout>
#include <QApplication>
#include <QPixmap>
#include <QPainter>
#include <QIcon>
#include <QHeaderView>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace mwb::ui {

namespace {

// The Instrument column comes last so the M and S cells keep their places (and shortcuts).
enum TrackColumn { column_name = 0, column_mute, column_solo, column_instrument, column_count };

/// Draws a menu cell: its text (elided) and a "\u25BE" at the right edge, so the Instrument
/// cell reads as something that opens a menu (review_10). The arrow is never elided away and
/// uses the cell's text color, so every theme draws it as readable as the text.
class MenuCellDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem item(option);
        initStyleOption(&item, index);
        const QString text = item.text;
        item.text.clear(); // background, selection and focus only; the text is drawn below
        const QWidget* widget = item.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);
        QRect text_rect = style->subElementRect(QStyle::SE_ItemViewItemText, &item, widget);
        const int arrow_width = arrow_space(item.fontMetrics);
        text_rect.setRight(text_rect.right() - arrow_width);
        text_rect.setLeft(std::max(text_rect.left(), item.rect.left() + 4)); // the padding of a text cell
        const bool selected = item.state.testFlag(QStyle::State_Selected);
        painter->save();
        painter->setFont(item.font);
        painter->setPen(item.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeft,
                          item.fontMetrics.elidedText(text, Qt::ElideRight, std::max(0, text_rect.width())));
        painter->drawText(QRect(text_rect.right() + 1, item.rect.top(), arrow_width, item.rect.height()), Qt::AlignCenter,
                          QStringLiteral("\u25BE"));
        painter->restore();
    }

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.rwidth() += arrow_space(option.fontMetrics);
        return size;
    }

    [[nodiscard]] static int arrow_space(const QFontMetrics& metrics) {
        return metrics.horizontalAdvance(QStringLiteral("\u25BE")) + 12; // the arrow, its gaps and the left padding
    }
};

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
    table_->setHorizontalHeaderLabels({tr("Track"), tr("M"), tr("S"), tr("Instrument")});
    // Name and instrument share the width; a narrow list elides both (the tooltip has the rest).
    table_->horizontalHeader()->setSectionResizeMode(column_name, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(column_instrument, QHeaderView::Stretch);
    for (int column = column_mute; column <= column_solo; ++column)
        table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    table_->setTextElideMode(Qt::ElideRight);
    table_->setItemDelegateForColumn(column_instrument, new MenuCellDelegate(table_));
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    // Typing never renames a track (letters and digits are editor commands); F2 or a
    // double-click does.
    table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    table_->setAccessibleName(tr("Tracks"));
    table_->setToolTip(tr("M mute, S solo. Recording goes to the selected track. Double-click a name to rename; "
                          "click an instrument (or press Enter on it) to choose another."));
    auto* buttons = new QHBoxLayout;
    table_->verticalHeader()->setDefaultSectionSize(28);
    const auto make_button = [&](IconId icon, const QString& tip, auto slot) {
        auto* button = new QToolButton(this);
        button->setIcon(make_icon(icon));
        button->setAutoRaise(true);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setMinimumSize(26, 26);
        connect(button, &QToolButton::clicked, this, slot);
        buttons->addWidget(button);
    };
    make_button(IconId::add, tr("Add track"), &TrackListWidget::add_track);
    make_button(IconId::duplicate, tr("Duplicate active track"), &TrackListWidget::duplicate_active_track);
    make_button(IconId::remove, tr("Delete active track"), &TrackListWidget::delete_active_track);
    delete_button_ = buttons->itemAt(buttons->count() - 1)->widget();
    make_button(IconId::up, tr("Move track up"), [this] { move_active_track(-1); });
    make_button(IconId::down, tr("Move track down"), [this] { move_active_track(1); });
    // Track color: the button shows the active track's color and opens a picker; the track
    // table's context menu and a click on a row's swatch open it too.
    color_button_ = new QToolButton(this);
    color_button_->setAutoRaise(true);
    color_button_->setToolTip(tr("Track color..."));
    color_button_->setAccessibleName(tr("Track color"));
    color_button_->setMinimumSize(26, 26);
    connect(color_button_, &QToolButton::clicked, this, &TrackListWidget::choose_active_track_color);
    buttons->addWidget(color_button_);
    buttons->addStretch(1);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        const int row = table_->rowAt(point.y());
        if (row < 0)
            return;
        table_->setCurrentCell(row, column_name);
        QMenu menu(this);
        menu.addAction(tr("Track Color..."), this, &TrackListWidget::choose_active_track_color);
        menu.addAction(tr("Rename"), this, [this, row] { table_->editItem(table_->item(row, column_name)); });
        const auto& tracks = controller_.active_sequence().tracks;
        if (static_cast<std::size_t>(row) < tracks.size()) {
            const TrackId id = tracks[static_cast<std::size_t>(row)].id;
            auto* instruments = menu.addMenu(tr("Instrument"));
            populate_instrument_menu(*instruments, music::effective_instrument(tracks[static_cast<std::size_t>(row)]),
                                     [this, id](std::optional<std::uint8_t> program) {
                                         emit status_message(choose_track_instrument(controller_, id, program));
                                     });
        }
        menu.exec(table_->viewport()->mapToGlobal(point));
    });
    // Return, Enter and keyboard Space on the Instrument cell arrive as activated() (see
    // pressable_cell_role and the window's key routing); F2 is handled in eventFilter.
    connect(table_, &QAbstractItemView::activated, this, [this](const QModelIndex& index) {
        if (index.column() == column_instrument)
            open_instrument_menu(index.row());
    });
    connect(table_, &QTableWidget::cellClicked, this, [this](int row, int column) {
        if (column == column_instrument) {
            open_instrument_menu(row);
            return;
        }
        if (column != column_name)
            return;
        // The swatch sits at the start of the name cell; a click on it picks the color.
        const QRect cell = table_->visualItemRect(table_->item(row, column));
        const QPoint at = table_->viewport()->mapFromGlobal(QCursor::pos());
        if (at.x() - cell.left() < 20)
            choose_active_track_color();
    });
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
            const std::string name = item->text().trimmed().toStdString();
            if (name.empty()) {
                // A track always has a name (ux-panels-44): the old one comes back.
                rebuilding_ = true;
                item->setText(QString::fromStdString(track.name));
                rebuilding_ = false;
                emit status_message(tr("A track needs a name"));
                return;
            }
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
        const bool press = key->key() == Qt::Key_F2 || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
        if (press && key->modifiers() == Qt::NoModifier && table_->currentColumn() == column_instrument) {
            open_instrument_menu(table_->currentRow()); // F2 means "change this", as it renames a name
            return true;
        }
        if ((key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) && key->modifiers() == Qt::NoModifier) {
            // The highlighted row is what Delete means (it may differ from the edited track after
            // an M/S click): make it the active track first (review_4).
            const auto& tracks = controller_.active_sequence().tracks;
            if (const int row = table_->currentRow(); row >= 0 && static_cast<std::size_t>(row) < tracks.size())
                controller_.set_active_track(tracks[static_cast<std::size_t>(row)].id);
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
    // Items are updated in place, never recreated: a note drag (every edit fires this) costs a
    // few comparisons, and the item whose itemChanged led here is never deleted under its own
    // handler. A name being edited is left alone (uiB-35).
    auto* focus = QApplication::focusWidget();
    const bool editing_name = focus && table_->viewport()->isAncestorOf(focus) && table_->currentColumn() == column_name;
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        const auto& track = tracks[index];
        const int row = static_cast<int>(index);
        auto* name = table_->item(row, column_name);
        if (!name) {
            name = new QTableWidgetItem;
            table_->setItem(row, column_name, name);
        }
        const QString track_name = QString::fromStdString(track.name);
        if (name->text() != track_name && !(editing_name && table_->currentRow() == row))
            name->setText(track_name);
        QFont font = name->font();
        const bool bold = active && *active == index; // the edited track reads bold even when another row is current
        if (font.bold() != bold) {
            font.setBold(bold);
            name->setFont(font);
        }
        // The swatch has a border in the base color, so it stays visible on the selection
        // highlight (ux-visual-29).
        const QColor color = color_from_rgba(track.color_rgba);
        if (name->data(Qt::UserRole + 7).value<QColor>() != color) {
            QPixmap swatch(12, 12);
            swatch.fill(table_->palette().color(QPalette::Base));
            QPainter painter(&swatch);
            painter.fillRect(QRect(1, 1, 10, 10), color);
            painter.end();
            name->setData(Qt::DecorationRole, QIcon(swatch));
            name->setData(Qt::UserRole + 7, color);
        }
        const QString tip = track.is_drum ? tr("%1 (drum track)").arg(track_name) : track_name;
        if (name->toolTip() != tip)
            name->setToolTip(tip);
        const auto check = [&](int column, bool value, const QString& check_tip) {
            auto* item = table_->item(row, column);
            if (!item) {
                item = new QTableWidgetItem;
                item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                // Always set, also when unchecked: an item without check-state data draws no box
                // and ignores clicks (review_10 I2). checkState() reads Unchecked either way, so the
                // compare below cannot catch it.
                item->setCheckState(value ? Qt::Checked : Qt::Unchecked);
                table_->setItem(row, column, item);
            }
            const auto state = value ? Qt::Checked : Qt::Unchecked;
            if (item->checkState() != state)
                item->setCheckState(state);
            if (item->toolTip() != check_tip) {
                item->setToolTip(check_tip);
                item->setData(Qt::AccessibleTextRole, check_tip); // screen readers say what the box does
            }
        };
        check(column_mute, track.muted, tr("Mute %1").arg(track_name));
        check(column_solo, track.solo, tr("Solo %1").arg(track_name));
        auto* instrument = table_->item(row, column_instrument);
        if (!instrument) {
            instrument = new QTableWidgetItem;
            instrument->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable); // chosen from a menu, never typed
            instrument->setData(pressable_cell_role, true);
            table_->setItem(row, column_instrument, instrument);
        }
        const auto effective = music::effective_instrument(track);
        const QString label = QString::fromStdString(music::instrument_label(track));
        if (instrument->text() != label)
            instrument->setText(label);
        const QString instrument_tip =
            (effective.drums ? tr("%1 plays drums (channel 10)").arg(track_name)
                             : tr("%1 plays %2 (General MIDI program %3)").arg(track_name, label).arg(effective.program + 1)) +
            tr(". Click or press Enter to choose another instrument.");
        if (instrument->toolTip() != instrument_tip) {
            instrument->setToolTip(instrument_tip);
            instrument->setData(Qt::AccessibleTextRole, tr("Instrument of %1: %2").arg(track_name, label));
        }
    }
    if (color_button_) {
        // The color button shows the active track's color as a swatch.
        if (active && *active < tracks.size()) {
            QPixmap swatch(16, 16);
            swatch.fill(Qt::transparent);
            QPainter painter(&swatch);
            painter.setPen(palette().color(QPalette::WindowText));
            painter.setBrush(color_from_rgba(tracks[*active].color_rgba));
            painter.drawRoundedRect(QRectF(1.5, 1.5, 13, 13), 3, 3);
            painter.end();
            color_button_->setIcon(QIcon(swatch));
        }
        color_button_->setEnabled(active.has_value());
    }
    // Keep the focused column (Mute or Solo stays current after it was toggled from the keyboard).
    if (active)
        table_->setCurrentCell(static_cast<int>(*active), std::max(current_column, 0));
    rebuilding_ = false;
}

void TrackListWidget::open_instrument_menu(int row) {
    const auto& tracks = controller_.active_sequence().tracks;
    if (row < 0 || static_cast<std::size_t>(row) >= tracks.size())
        return;
    const Track& track = tracks[static_cast<std::size_t>(row)];
    const TrackId id = track.id;
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("instrumentMenu"));
    menu.setTitle(tr("Instrument of %1").arg(QString::fromStdString(track.name)));
    populate_instrument_menu(menu, music::effective_instrument(track), [this, id](std::optional<std::uint8_t> program) {
        emit status_message(choose_track_instrument(controller_, id, program));
    });
    table_->setCurrentCell(row, column_instrument);
    QRect cell = table_->visualRect(table_->model()->index(row, column_instrument));
    if (cell.isEmpty())
        cell = QRect(table_->viewport()->rect().topLeft(), QSize(1, 1));
    menu.exec(table_->viewport()->mapToGlobal(cell.bottomLeft()));
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

bool TrackListWidget::set_track_color(TrackId track_id, const QColor& color) {
    const auto index = controller_.active_sequence().track_index(track_id);
    if (!index || !color.isValid())
        return false;
    Track updated = track_header(controller_.active_sequence().tracks[*index]); // properties only, no events
    updated.color_rgba = ((static_cast<std::uint32_t>(color.rgb()) & 0xFFFFFFu) << 8) | 0xFFu;
    if (updated.color_rgba == controller_.active_sequence().tracks[*index].color_rgba)
        return true;
    ChangeBuilder builder(controller_.document(), "Change track color");
    builder.set_track_properties(track_id, updated);
    return controller_.apply(builder.finish());
}

void TrackListWidget::choose_active_track_color() {
    const auto index = controller_.active_track_index();
    if (!index)
        return;
    const auto& track = controller_.active_sequence().tracks[*index];
    const TrackId id = track.id;
    const QColor chosen = QColorDialog::getColor(color_from_rgba(track.color_rgba), this,
                                                 tr("Color of %1").arg(QString::fromStdString(track.name)));
    if (chosen.isValid())
        set_track_color(id, chosen);
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
    connect(track_list_, &TrackListWidget::status_message, this, &EditorTab::status_message);
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
    // Wide enough for a name, M, S and an instrument name ("Acoustic Grand Piano") side by side.
    horizontal->setSizes({std::max(340, track_list_->sizeHint().width()), 1000});
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
        // Lanes that follow the track show the active track's channel (ux-editing-31).
        const auto channel = default_channel_for(*controller_);
        for (auto* lane : lanes_)
            lane->follow_track_channel(channel);
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
    const bool changed = showing_drum_grid() != show;
    center_->setCurrentWidget(show ? static_cast<QWidget*>(drum_grid_) : piano_roll_);
    lanes_container_->setVisible(!show); // lanes follow the piano roll's time axis, not the grid's
    if (!changed)
        return;
    // The grid draws no selection: edit keys must never act on notes the user cannot see, and
    // the keyboard goes to the view that is shown (ux-editing-22).
    if (show)
        controller_->set_selection({});
    if (isVisible())
        (show ? static_cast<QWidget*>(&drum_grid_->view()) : static_cast<QWidget*>(&piano_roll_->view()))->setFocus();
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
