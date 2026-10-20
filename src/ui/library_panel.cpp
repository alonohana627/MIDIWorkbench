#include "mwb/ui/library_panel.hpp"

#include "mwb/ui/task_thread.hpp"

#include "mwb/core/pitch.hpp"
#include "mwb/library/library.hpp"
#include "mwb/library/storage.hpp"
#include "mwb/midi/codec.hpp"
#include "mwb/ui/drag_export.hpp"

#include <QMimeData>
#include <QUrl>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QStackedWidget>

#include <chrono>
#include <QInputDialog>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QAction>
#include <QMenu>
#include <QSignalBlocker>
#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTableView>
#include <QVBoxLayout>

#include <condition_variable>
#include <deque>
#include <mutex>

namespace mwb::ui {

namespace {

enum LibraryColumn { column_title = 0, column_tags, column_rating, column_tempo, column_key, column_length, column_status, library_column_count };

LibraryRow make_row(const search::BrowseRow& browse, const std::string& title);

/// Runs one page of a text search and resolves its display rows.
struct PageResult {
    std::vector<LibraryRow> rows;
    std::optional<search::PageCursor> next;
    QString error;
};

PageResult load_page(library::Library& library, const std::string& query, std::optional<search::PageCursor> after,
                     std::stop_token stop) {
    PageResult result;
    library::SearchRequest request;
    request.query_text = query;
    request.prefix_last_word = true; // "lea" already finds "lead" while typing (code-library-28)
    request.after = std::move(after);
    auto page = library.search(request, stop);
    if (!page) {
        result.error = QString::fromStdString(page.error().describe());
        return result;
    }
    result.next = page->next;
    std::vector<std::int64_t> ids;
    for (const auto& row : page->rows)
        ids.push_back(row.id);
    if (auto browse = library.browse_rows_for(ids)) {
        for (const auto id : ids) // keep the search order
            for (const auto& browse_row : *browse)
                if (browse_row.id == id) {
                    std::string title = browse_row.text[0].empty() ? browse_row.sort_title : browse_row.text[0];
                    result.rows.push_back(make_row(browse_row, title));
                }
    }
    return result;
}

LibraryRow make_row(const search::BrowseRow& browse, const std::string& title) {
    LibraryRow row;
    row.asset_id = browse.id;
    row.title = QString::fromStdString(title);
    QStringList tags;
    for (const auto& tag : browse.tags)
        tags << QString::fromStdString(tag);
    row.tags = tags.join(", ");
    row.rating = browse.rating;
    row.favorite = browse.favorite;
    row.modified_ms = browse.modified_ms;
    if (!browse.sequences.empty()) {
        const auto& facts = browse.sequences.front();
        if (facts.tempo_representative)
            row.tempo = QString::number(*facts.tempo_representative, 'f', 1) +
                        (facts.tempo_origin == "default" ? QObject::tr(" (default)") : QString());
        if (facts.key_root && facts.key_mode)
            row.key = QString::fromStdString(pitch_class_name(*facts.key_root) + " " + *facts.key_mode) +
                      (facts.key_origin == "estimated" ? QObject::tr(" (est.)") : QString());
        if (facts.quarter_length)
            row.length = QObject::tr("%1 qn").arg(*facts.quarter_length, 0, 'f', 1);
        else
            row.length = QObject::tr("%1 s").arg(facts.duration_seconds, 0, 'f', 1);
    }
    switch (browse.analysis) {
    case search::AnalysisState::indexing: row.status = QObject::tr("indexing"); break;
    case search::AnalysisState::ready: row.status = QObject::tr("ready"); break;
    case search::AnalysisState::warning: row.status = QObject::tr("warning"); break;
    case search::AnalysisState::error: row.status = QObject::tr("error"); break;
    }
    if (browse.source_missing)
        row.status += QObject::tr(", source missing");
    return row;
}

} // namespace

int LibraryResultModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int LibraryResultModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : library_column_count; }

QVariant LibraryResultModel::data(const QModelIndex& index, int role) const {
    const LibraryRow* row = row_at(index.row());
    if (!row)
        return {};
    if (role == Qt::ToolTipRole) {
        // The title may be elided in a narrow dock; hidden columns are summarized here too.
        QStringList lines{row->title};
        if (!row->tags.isEmpty())
            lines << tr("Tags: %1").arg(row->tags);
        if (!row->status.isEmpty())
            lines << row->status;
        return lines.join('\n');
    }
    if (role != Qt::DisplayRole)
        return {};
    switch (index.column()) {
    case column_title: return (row->favorite ? QStringLiteral("★ ") : QString()) + row->title;
    case column_tags: return row->tags;
    case column_rating: return row->rating > 0 ? QString(row->rating, QChar(0x2605)) : QString();
    case column_tempo: return row->tempo;
    case column_key: return row->key;
    case column_length: return row->length;
    case column_status: return row->status;
    default: return {};
    }
}

QVariant LibraryResultModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QAbstractTableModel::headerData(section, orientation, role);
    switch (section) {
    case column_title: return tr("Title");
    case column_tags: return tr("Tags");
    case column_rating: return tr("Rating");
    case column_tempo: return tr("BPM");
    case column_key: return tr("Key");
    case column_length: return tr("Length");
    case column_status: return tr("Status");
    default: return {};
    }
}

void LibraryResultModel::set_rows(std::vector<LibraryRow> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

void LibraryResultModel::replace_row(std::int64_t asset_id, std::optional<LibraryRow> row) {
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        if (rows_[index].asset_id != asset_id)
            continue;
        const int position = static_cast<int>(index);
        if (!row) {
            beginRemoveRows({}, position, position);
            rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(index));
            endRemoveRows();
            drag_files_.erase(asset_id);
            return;
        }
        const QString status = rows_[index].status; // keep a melody-search description
        const auto match = rows_[index].match;
        if (rows_[index].modified_ms != row->modified_ms)
            drag_files_.erase(asset_id); // a new revision: the prepared file is stale
        rows_[index] = std::move(*row);
        if (match) { // a melody hit keeps its match and its description
            rows_[index].match = match;
            rows_[index].status = status;
        } else if (rows_[index].status.isEmpty()) {
            rows_[index].status = status;
        }
        emit dataChanged(this->index(position, 0), this->index(position, columnCount({}) - 1));
        return;
    }
}

void LibraryResultModel::insert_row(int position, LibraryRow row) {
    position = std::clamp(position, 0, static_cast<int>(rows_.size()));
    beginInsertRows({}, position, position);
    rows_.insert(rows_.begin() + position, std::move(row));
    endInsertRows();
}

int LibraryResultModel::position_of(std::int64_t asset_id) const {
    for (std::size_t index = 0; index < rows_.size(); ++index)
        if (rows_[index].asset_id == asset_id)
            return static_cast<int>(index);
    return -1;
}

void LibraryResultModel::set_drag_file(std::int64_t asset_id, std::int64_t modified_ms, QString path) {
    const int position = position_of(asset_id);
    if (position < 0 || rows_[static_cast<std::size_t>(position)].modified_ms != modified_ms)
        return; // prepared from a revision the row no longer shows
    drag_files_[asset_id] = {modified_ms, std::move(path)};
    emit dataChanged(index(position, 0), index(position, columnCount({}) - 1)); // drag now enabled
}

const QString* LibraryResultModel::drag_file_for(const LibraryRow& row) const {
    const auto found = drag_files_.find(row.asset_id);
    return found != drag_files_.end() && found->second.first == row.modified_ms ? &found->second.second : nullptr;
}

bool LibraryResultModel::contains_asset(std::int64_t asset_id) const {
    return std::ranges::any_of(rows_, [asset_id](const LibraryRow& row) { return row.asset_id == asset_id; });
}

void LibraryResultModel::append_rows(std::vector<LibraryRow> rows) {
    if (rows.empty())
        return;
    beginInsertRows({}, static_cast<int>(rows_.size()), static_cast<int>(rows_.size() + rows.size() - 1));
    for (auto& row : rows)
        rows_.push_back(std::move(row));
    endInsertRows();
}

Qt::ItemFlags LibraryResultModel::flags(const QModelIndex& index) const {
    auto result = QAbstractTableModel::flags(index);
    if (const auto* row = row_at(index.row()); row && drag_file_for(*row))
        result |= Qt::ItemIsDragEnabled;
    return result;
}

QStringList LibraryResultModel::mimeTypes() const { return {QStringLiteral("text/uri-list")}; }

QMimeData* LibraryResultModel::mimeData(const QModelIndexList& indexes) const {
    QList<QUrl> urls;
    for (const auto& index : indexes) {
        const auto* row = row_at(index.row());
        if (!row)
            continue;
        if (const QString* file = drag_file_for(*row)) {
            const auto url = QUrl::fromLocalFile(*file);
            if (!urls.contains(url))
                urls << url;
        }
    }
    if (urls.isEmpty())
        return nullptr;
    auto* mime = new QMimeData;
    mime->setUrls(urls); // a real file drop for DAWs and Explorer (section 14.3)
    return mime;
}

const LibraryRow* LibraryResultModel::row_at(int row) const {
    return row >= 0 && static_cast<std::size_t>(row) < rows_.size() ? &rows_[static_cast<std::size_t>(row)] : nullptr;
}

LibraryPanel::LibraryPanel(QWidget* parent) : QWidget(parent) {
    // New and Open are separate (ux-panels-8): a library is its own folder; choosing a folder of
    // MIDI files offers to create a library next to it and import them.
    auto* open_button = new QPushButton(tr("Library"), this);
    open_button->setToolTip(tr("Create a new library or open an existing one"));
    auto* library_menu = new QMenu(open_button);
    library_menu->addAction(tr("New Library…"), this, &LibraryPanel::new_library);
    library_menu->addAction(tr("Open Library…"), this, &LibraryPanel::choose_library);
    open_button->setMenu(library_menu);
    trash_button_ = new QPushButton(tr("Trash"), this);
    trash_button_->setCheckable(true);
    trash_button_->setToolTip(tr("Show the items moved to the trash (restore or delete permanently)"));
    trash_button_->setEnabled(false);
    auto* refresh_button = new QPushButton(tr("Refresh"), this);
    refresh_button->setToolTip(tr("Run the search again (new and changed items appear)"));
    import_button_ = new QPushButton(tr("Add Folder…"), this);
    import_button_->setToolTip(tr("Import a folder; browsing continues while it imports"));
    import_button_->setEnabled(false);
    cancel_import_button_ = new QPushButton(tr("Cancel Import"), this);
    cancel_import_button_->setToolTip(tr("Stop the running import; files already imported stay"));
    cancel_import_button_->hide();
    import_progress_ = new QProgressBar(this);
    import_progress_->setAccessibleName(tr("Import progress"));
    import_progress_->setTextVisible(true);
    import_progress_->setFormat(tr("%v of %m files"));
    import_progress_->hide();
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search: tag:bass bpm:120..130 key:\"A minor\" text:\"lead\""));
    search_->setClearButtonEnabled(true);
    search_->setAccessibleName(tr("Library search"));
    model_ = new LibraryResultModel(this);
    table_ = new QTableView(this);
    table_->setModel(model_);
    // Keyset paging: the next page loads when the list is scrolled near its end.
    connect(table_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (value >= table_->verticalScrollBar()->maximum() - 4)
            fetch_more();
    });
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection); // actions apply to every selected row
    table_->setTextElideMode(Qt::ElideMiddle); // "Bass groove … take 3" keeps both ends
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(24);
    configure_columns();
    table_->setAccessibleName(tr("Library results"));
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->setDragEnabled(true);
    table_->setDragDropMode(QAbstractItemView::DragOnly);
    status_ = new QLabel(tr("No library open"), this);
    status_->setWordWrap(true);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(open_button);
    buttons->addWidget(import_button_);
    buttons->addWidget(import_progress_, 1);
    buttons->addWidget(cancel_import_button_);
    buttons->addWidget(trash_button_);
    buttons->addWidget(refresh_button);
    buttons->addStretch(1);
    // Shown while the list holds melody hits or the trash, so the user knows (and can leave).
    mode_bar_ = new QWidget(this);
    auto* mode_layout = new QHBoxLayout(mode_bar_);
    mode_layout->setContentsMargins(0, 0, 0, 0);
    mode_label_ = new QLabel(mode_bar_);
    QFont mode_font = mode_label_->font();
    mode_font.setWeight(QFont::DemiBold);
    mode_label_->setFont(mode_font);
    auto* back = new QPushButton(tr("Back to Search"), mode_bar_);
    mode_layout->addWidget(mode_label_, 1);
    mode_layout->addWidget(back);
    mode_bar_->hide();
    connect(back, &QPushButton::clicked, this, [this] {
        trash_button_->setChecked(false);
        run_search();
    });
    connect(trash_button_, &QPushButton::toggled, this, &LibraryPanel::show_trash);
    connect(refresh_button, &QPushButton::clicked, this, [this] { refresh_results(); });
    // Page 1: the open library. Page 0: what a library is and how to start one.
    auto* browse = new QWidget(this);
    auto* layout = new QVBoxLayout(browse);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    layout->addLayout(buttons);
    layout->addWidget(search_);
    layout->addWidget(mode_bar_);
    layout->addWidget(table_, 1);
    layout->addWidget(status_);

    auto* empty = new QWidget(this);
    auto* empty_layout = new QVBoxLayout(empty);
    empty_layout->setContentsMargins(16, 24, 16, 16);
    empty_layout->setSpacing(10);
    auto* heading = new QLabel(tr("No library yet"), empty);
    QFont heading_font = heading->font();
    heading_font.setPointSizeF(heading_font.pointSizeF() + 2);
    heading_font.setWeight(QFont::DemiBold);
    heading->setFont(heading_font);
    auto* explanation = new QLabel(tr("A library indexes your MIDI files so you can search them by tag, key, tempo "
                                      "and melody, and keeps every saved revision. It is a new folder of its own: "
                                      "importing copies your files into it and never changes the originals."),
                                   empty);
    explanation->setWordWrap(true);
    explanation->setForegroundRole(QPalette::PlaceholderText);
    auto* start_button = new QPushButton(tr("New Library…"), empty);
    start_button->setDefault(true);
    connect(start_button, &QPushButton::clicked, this, &LibraryPanel::new_library);
    auto* open_existing = new QPushButton(tr("Open Library…"), empty);
    connect(open_existing, &QPushButton::clicked, this, &LibraryPanel::choose_library);
    empty_status_ = new QLabel(empty);
    empty_status_->setWordWrap(true);
    empty_layout->addWidget(heading);
    empty_layout->addWidget(explanation);
    auto* start_buttons = new QHBoxLayout;
    start_buttons->addWidget(start_button);
    start_buttons->addWidget(open_existing);
    start_buttons->addStretch(1);
    empty_layout->addLayout(start_buttons);
    empty_layout->addWidget(empty_status_);
    empty_layout->addStretch(1);

    pages_ = new QStackedWidget(this);
    pages_->addWidget(empty);
    pages_->addWidget(browse);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(pages_);
    watch_flush_.setSingleShot(true);
    watch_flush_.setInterval(1000);
    connect(&watch_flush_, &QTimer::timeout, this, &LibraryPanel::apply_watch_hints);
    change_poll_.setInterval(2000);
    connect(&change_poll_, &QTimer::timeout, this, &LibraryPanel::poll_changes);
    debounce_.setSingleShot(true);
    debounce_.setInterval(120); // typing debounce (section 33.3)
    connect(&debounce_, &QTimer::timeout, this, [this] { run_search(); });
    connect(search_, &QLineEdit::textChanged, this, [this] { debounce_.start(); });
    connect(import_button_, &QPushButton::clicked, this, &LibraryPanel::import_folder);
    connect(cancel_import_button_, &QPushButton::clicked, this, [this] {
        if (import_stop_)
            import_stop_->request_stop();
        status_->setText(tr("Cancelling the import…"));
    });
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] { prepare_drag_file(); });
    // Listen before opening; insert into the open document (spec 5.2, 6.2).
    auto* preview_action = new QAction(tr("Preview"), table_);
    preview_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    preview_action->setShortcutContext(Qt::WidgetShortcut);
    connect(preview_action, &QAction::triggered, this, [this] {
        load_current([](LibraryPanel& panel, DocumentData document, const LibraryRow& row, std::int64_t) {
            emit panel.preview_requested(std::move(document), row.title, row.match);
        });
    });
    table_->addAction(preview_action);
    // Single click selects silently; double-click or Enter opens (section 6.2).
    connect(table_, &QTableView::activated, this, [this] { open_selected(); });
    connect(table_, &QTableView::customContextMenuRequested, this, [this](const QPoint& point) {
        const QModelIndex clicked = table_->indexAt(point);
        if (!clicked.isValid())
            return; // no item under the pointer: nothing to act on
        if (!table_->selectionModel()->isRowSelected(clicked.row(), {}))
            table_->setCurrentIndex(clicked); // a right-click outside the selection selects that row
        QMenu menu(this);
        if (mode_ == ResultMode::trash) {
            menu.addAction(tr("Restore"), this, &LibraryPanel::restore_selected_from_trash);
            menu.addAction(tr("Delete Permanently…"), this, &LibraryPanel::purge_selected);
            menu.exec(table_->viewport()->mapToGlobal(point));
            return;
        }
        menu.addAction(tr("Open"), this, &LibraryPanel::open_selected);
        menu.addAction(tr("Preview\tCtrl+Enter"), this, [this] {
            load_current([](LibraryPanel& panel, DocumentData document, const LibraryRow& row, std::int64_t) {
                emit panel.preview_requested(std::move(document), row.title, row.match);
            });
        });
        menu.addAction(tr("Insert at Edit Cursor"), this, [this] {
            load_current([](LibraryPanel& panel, DocumentData document, const LibraryRow& row, std::int64_t) {
                emit panel.insert_requested(std::move(document), row.title);
            });
        });
        menu.addSeparator();
        menu.addAction(tr("Revisions…"), this, &LibraryPanel::show_revisions);
        menu.addAction(tr("Toggle Favorite"), this, [this] { set_metadata(0); });
        menu.addAction(tr("Add Tag…"), this, [this] {
            bool accepted = false;
            const QString tag = QInputDialog::getText(this, tr("Add tag"), tr("Tag"), QLineEdit::Normal, {}, &accepted);
            if (accepted && !tag.trimmed().isEmpty())
                run_metadata_task([tag = tag.trimmed().toStdString()](library::Library& library, std::int64_t asset) {
                    return library.add_tag(asset, tag);
                }, [tag = tag.trimmed()](LibraryRow& row) {
                    if (!row.tags.split(", ", Qt::SkipEmptyParts).contains(tag, Qt::CaseInsensitive))
                        row.tags = row.tags.isEmpty() ? tag : row.tags + ", " + tag;
                });
        });
        menu.addAction(tr("Remove Tag…"), this, [this] {
            bool accepted = false;
            // The tags the selected items have, to pick from instead of retyping (ux-panels-33).
            QStringList present;
            for (const auto asset : selected_assets())
                if (const int position = model_->position_of(asset); position >= 0)
                    if (const auto* row = model_->row_at(position))
                        for (const auto& tag : row->tags.split(", ", Qt::SkipEmptyParts))
                            if (!present.contains(tag, Qt::CaseInsensitive))
                                present << tag;
            if (present.isEmpty()) {
                emit status_message(tr("The selected items have no tags"));
                return;
            }
            const QString tag = QInputDialog::getItem(this, tr("Remove tag"), tr("Tag"), present, 0, false, &accepted);
            if (accepted && !tag.trimmed().isEmpty())
                run_metadata_task([tag = tag.trimmed().toStdString()](library::Library& library, std::int64_t asset) {
                    return library.remove_tag(asset, tag);
                }, [tag = tag.trimmed()](LibraryRow& row) {
                    QStringList tags = row.tags.split(", ", Qt::SkipEmptyParts);
                    tags.removeIf([&](const QString& existing) { return existing.compare(tag, Qt::CaseInsensitive) == 0; });
                    row.tags = tags.join(", ");
                });
        });
        menu.addAction(tr("Set Musical Role…"), this, [this] {
            bool accepted = false;
            const QString role = QInputDialog::getItem(this, tr("Musical role"), tr("Role"),
                                                       {"melody", "bass", "chords", "drums", "arpeggio", "full song", "unspecified"},
                                                       0, true, &accepted);
            if (accepted)
                run_metadata_task([role = role.toStdString()](library::Library& library, std::int64_t asset) {
                    return library.set_role(asset, role);
                });
        });
        menu.addAction(tr("Move to Trash…"), this, &LibraryPanel::trash_selected);
        auto* rating_menu = menu.addMenu(tr("Rating"));
        const LibraryRow* current = model_->row_at(table_->currentIndex().row());
        for (int rating = 0; rating <= 5; ++rating) {
            auto* action = rating_menu->addAction(rating == 0 ? tr("No rating") : QString(rating, QChar(0x2605)), this,
                                                  [this, rating] { set_metadata(10 + rating); });
            action->setCheckable(true);
            action->setChecked(current && current->rating == rating);
        }
        menu.exec(table_->viewport()->mapToGlobal(point));
    });
    table_->installEventFilter(this);
    // Reached with Tab, pressed with Return or Space; a click leaves the focus in the editor.
    for (auto* button : findChildren<QPushButton*>())
        button->setFocusPolicy(Qt::TabFocus);
    const QString last = QSettings().value("library/last").toString();
    if (!last.isEmpty())
        open_library(std::filesystem::path(last.toStdWString()), false); // not interactive: the layout stays as saved
}

LibraryPanel::~LibraryPanel() {
    // Background work ends with the panel. Searches and the import see their stop tokens; queued
    // user writes (saves, tags, ratings, restores) still run before the write thread ends.
    if (search_stop_)
        search_stop_->request_stop();
    if (import_stop_)
        import_stop_->request_stop();
    watcher_.reset(); // no more hints
    import_thread_.reset();
    jobs_.reset(); // cancels upkeep jobs (the index build stops at its next page) and waits
    read_thread_.reset();
    browse_thread_.reset();
    write_thread_.reset(); // drains
}

void LibraryPanel::trash_selected() {
    if (mode_ == ResultMode::trash) {
        purge_selected(); // Delete in the trash view deletes permanently (with its own question)
        return;
    }
    const auto assets = selected_assets();
    if (assets.empty())
        return;
    const LibraryRow* current = model_->row_at(table_->currentIndex().row());
    const QString question = assets.size() == 1 && current
                                 ? tr("Move \"%1\" to the trash? You can restore it from Trash.").arg(current->title)
                                 : tr("Move %n items to the trash? You can restore them from Trash.", nullptr,
                                      static_cast<int>(assets.size()));
    if (QMessageBox::question(this, tr("Move to Trash"), question) != QMessageBox::Yes)
        return;
    run_metadata_task([](library::Library& library, std::int64_t asset) {
        return library.trash(std::vector<std::int64_t>{asset}); // a metadata state; restorable
    }, {}, true);
}

void LibraryPanel::show_trash(bool on) {
    if (!library_)
        return;
    if (!on) {
        if (mode_ == ResultMode::trash)
            run_search();
        return;
    }
    set_mode(ResultMode::trash);
    const std::uint64_t generation = ++*generation_;
    restart_search_cancellation();
    next_page_.reset();
    QPointer<LibraryPanel> guard(this);
    auto library = library_;
    read_thread_->post([guard, library, generation, latest = generation_](std::stop_token) {
        if (generation != latest->load())
            return;
        std::vector<LibraryRow> rows;
        QString error;
        if (auto trashed = library->list_trash()) {
            if (auto browse = library->browse_rows_for(*trashed))
                for (const auto id : *trashed)
                    for (const auto& row : *browse)
                        if (row.id == id)
                            rows.push_back(make_row(row, row.text[0].empty() ? row.sort_title : row.text[0]));
        } else {
            error = QString::fromStdString(trashed.error().describe());
        }
        QMetaObject::invokeMethod(
            qApp,
            [guard, generation, rows = std::move(rows), error]() mutable {
                if (guard)
                    guard->deliver_results(generation, std::move(rows), error);
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::restore_selected_from_trash() {
    const auto assets = selected_assets();
    if (assets.empty() || !library_)
        return;
    run_metadata_task([](library::Library& library, std::int64_t asset) {
        return library.restore_from_trash(std::vector<std::int64_t>{asset}); // identity and tags come back
    }, {}, true);
    emit status_message(tr("Restored %n item(s) from the trash", nullptr, static_cast<int>(assets.size())));
}

void LibraryPanel::purge_selected() {
    const auto assets = selected_assets();
    if (assets.empty() || !library_)
        return;
    if (QMessageBox::warning(this, tr("Delete Permanently"),
                             tr("Permanently delete %n item(s) and all their revisions? This cannot be undone.", nullptr,
                                static_cast<int>(assets.size())),
                             QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Discard)
        return;
    run_metadata_task([](library::Library& library, std::int64_t asset) -> Status {
        auto purged = library.purge(std::vector<std::int64_t>{asset});
        if (!purged)
            return std::unexpected(std::move(purged).error());
        return ok();
    }, {}, true);
}

std::vector<std::int64_t> LibraryPanel::selected_assets() const {
    std::vector<std::int64_t> assets;
    for (const auto& index : table_->selectionModel()->selectedRows())
        if (const auto* row = model_->row_at(index.row()))
            assets.push_back(row->asset_id);
    if (assets.empty())
        if (const auto* row = model_->row_at(table_->currentIndex().row()))
            assets.push_back(row->asset_id);
    return assets;
}

void LibraryPanel::set_mode(ResultMode mode) {
    mode_ = mode;
    mode_label_->setText(mode == ResultMode::melody ? tr("Melody search results") : tr("Trash"));
    mode_bar_->setVisible(mode != ResultMode::text);
    apply_column_visibility();
    if (mode != ResultMode::trash && trash_button_->isChecked()) {
        const QSignalBlocker blocker(trash_button_);
        trash_button_->setChecked(false);
    }
}

void LibraryPanel::apply_column_visibility() {
    auto* header = table_->horizontalHeader();
    const QStringList hidden =
        QSettings().value("library/hidden_columns", QStringList{QString::number(column_tags), QString::number(column_status)})
            .toStringList();
    for (int column = column_title + 1; column < library_column_count; ++column)
        header->setSectionHidden(column, hidden.contains(QString::number(column)));
    // A melody hit's Status says where it matched (transposition, ticks, track): two hits in one
    // file differ only there, so melody results show Title and Status only, and the match
    // details fit the default 350 px dock instead of being clipped behind BPM, Key and Length
    // (review_5, review_6). Back to Search restores the saved choice.
    if (mode_ == ResultMode::melody)
        for (int column = column_title + 1; column < library_column_count; ++column)
            header->setSectionHidden(column, column != column_status);
}

void LibraryPanel::configure_columns() {
    // Title takes the room; the other columns fit their content (ux-panels-9). Tags and Status
    // start hidden in the narrow dock; the header menu shows or hides any column.
    auto* header = table_->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setSectionResizeMode(column_title, QHeaderView::Stretch);
    apply_column_visibility();
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header, &QHeaderView::customContextMenuRequested, this, [this, header](const QPoint& point) {
        QMenu menu(this);
        for (int column = column_title + 1; column < library_column_count; ++column) {
            auto* action = menu.addAction(model_->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString());
            action->setCheckable(true);
            action->setChecked(!header->isSectionHidden(column));
            connect(action, &QAction::toggled, this, [header, column](bool shown) {
                header->setSectionHidden(column, !shown);
                QStringList hidden_columns;
                for (int index = column_title + 1; index < library_column_count; ++index)
                    if (header->isSectionHidden(index))
                        hidden_columns << QString::number(index);
                QSettings().setValue("library/hidden_columns", hidden_columns);
            });
        }
        menu.exec(header->mapToGlobal(point));
    });
}

bool LibraryPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) && key->modifiers() == Qt::NoModifier) {
            trash_selected();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void LibraryPanel::focus_search() {
    search_->setFocus();
    search_->selectAll();
}

void LibraryPanel::choose_library() {
    const QString directory = QFileDialog::getExistingDirectory(this, tr("Open a library folder"));
    if (directory.isEmpty())
        return;
    const std::filesystem::path root(directory.toStdWString());
    std::error_code error;
    if (std::filesystem::exists(root / "library.sqlite", error)) {
        open_library(root, false, true);
        return;
    }
    // Folder litter the system adds (desktop.ini, Thumbs.db, .DS_Store, dot files) does not make
    // a folder "not empty" (review_5).
    bool has_files = false;
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        const QString name = QString::fromStdWString(it->path().filename().wstring()).toLower();
        if (name.startsWith('.') || name == "desktop.ini" || name == "thumbs.db")
            continue;
        has_files = true;
        break;
    }
    if (!has_files) {
        open_library(root, true, true); // an empty folder becomes a new library
        return;
    }
    // A folder of MIDI files is not a library: offer one next to it and import the files (the
    // originals are only read).
    // Next to the folder; for a drive root (no folder name) inside it (the import skips the
    // library's own folder).
    const std::filesystem::path suggested = root.has_filename() && root.filename() != root.root_path()
                                                ? root.parent_path() / (root.filename().wstring() + L" Library")
                                                : root / L"MIDI Workbench Library";
    const auto answer = QMessageBox::question(
        this, tr("Not a library"),
        tr("\"%1\" is not a library. Create a library in \"%2\" and import the MIDI files from \"%1\"?")
            .arg(QString::fromStdWString(root.wstring()), QString::fromStdWString(suggested.wstring())));
    if (answer != QMessageBox::Yes)
        return;
    open_library(suggested, !std::filesystem::exists(suggested / "library.sqlite", error), true, root);
}

void LibraryPanel::new_library() {
    const QString parent = QFileDialog::getExistingDirectory(this, tr("Where should the new library folder go?"));
    if (parent.isEmpty())
        return;
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("New Library"), tr("Library folder name"), QLineEdit::Normal,
                                               tr("MIDI Workbench Library"), &accepted)
                             .trimmed();
    if (!accepted || name.isEmpty())
        return;
    open_library(std::filesystem::path(parent.toStdWString()) / name.toStdWString(), true, true);
}

void LibraryPanel::open_library(const std::filesystem::path& root, bool create, bool interactive,
                                std::optional<std::filesystem::path> import_after) {
    status_->setText(tr("Opening library…"));
    QPointer<LibraryPanel> guard(this);
    write_thread_->post([guard, root, create, interactive, import_after](std::stop_token) {
        auto opened = create ? library::Library::create(root) : library::Library::open(root);
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, root, interactive, import_after, opened = std::make_shared<decltype(opened)>(std::move(opened))]() mutable {
                if (!guard)
                    return;
                if (!*opened) {
                    const QString message =
                        tr("Library unavailable: %1").arg(QString::fromStdString((*opened).error().describe()));
                    guard->status_->setText(message);
                    guard->empty_status_->setText(message);
                    // Also said when the library remembered from last time is gone (unplugged
                    // drive, moved folder): the dock may be hidden (ux-panels-34).
                    emit guard->status_message(interactive ? message
                                                           : message + tr(" — use Open Library… to locate it"));
                    return;
                }
                guard->library_ = std::shared_ptr<library::Library>(std::move(**opened));
                // A search that finds the outbox reset drops the index; rebuild it in the background.
                guard->library_->set_browse_index_dropped_listener([guard] {
                    QMetaObject::invokeMethod(
                        qApp,
                        [guard] {
                            if (guard)
                                guard->schedule_browse_index(false);
                        },
                        Qt::QueuedConnection);
                });
                guard->model_->clear_drag_files(); // asset ids repeat across libraries
                guard->change_revision_.reset();
                guard->change_poll_.start();
                guard->pages_->setCurrentIndex(1);
                emit guard->opened(interactive); // after library_ is set, so listeners see the new library
                QSettings().setValue("library/last", QString::fromStdWString(root.wstring()));
                guard->import_button_->setEnabled(true);
                guard->trash_button_->setEnabled(true);
                guard->run_search();
                guard->schedule_browse_index(true);
                // The previous library's folders are no longer followed.
                guard->watcher_.reset();
                guard->watched_roots_.clear();
                guard->pending_hints_.clear();
                guard->start_watching();
                if (import_after)
                    guard->import_path(*import_after);
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::import_folder() {
    if (!library_)
        return;
    const QString directory = QFileDialog::getExistingDirectory(this, tr("Add a folder of MIDI files"));
    if (!directory.isEmpty())
        import_path(std::filesystem::path(directory.toStdWString()));
}

void LibraryPanel::import_path(const std::filesystem::path& source) {
    if (!library_)
        return;
    status_->setText(tr("Importing %1… (browsing stays available)").arg(QString::fromStdWString(source.wstring())));
    QPointer<LibraryPanel> guard(this);
    auto library = library_;
    import_stop_ = std::make_shared<std::stop_source>();
    import_button_->setEnabled(false);
    cancel_import_button_->show();
    import_progress_->setRange(0, 0); // busy until the first file count arrives
    import_progress_->show();
    import_thread_->post([guard, library, source, parser = import_parser_ ? import_parser_ : parser_,
                         cancel = import_stop_](std::stop_token thread_stop) {
        // Cancel button or panel shutdown, whichever comes first.
        std::stop_callback forward(thread_stop, [cancel] { cancel->request_stop(); });
        const auto stop = cancel->get_token();
        library::ImportOptions options;
        options.parser = parser; // parsing runs in the isolated worker process (section 28.2)
        std::error_code folder_error;
        options.watch = std::filesystem::is_directory(source, folder_error); // folders are followed (section 22.4)
        auto last_report = std::make_shared<std::chrono::steady_clock::time_point>();
        options.progress = [guard, last_report](std::size_t done, std::size_t found) {
            const auto now = std::chrono::steady_clock::now();
            if (now - *last_report < std::chrono::milliseconds(200) && done != found)
                return; // at most five updates a second
            *last_report = now;
            QMetaObject::invokeMethod(
                qApp,
                [guard, done, found] {
                    if (!guard)
                        return;
                    guard->status_->setText(tr("Importing: %1 of %2 files… (browsing stays available)").arg(done).arg(found));
                    guard->import_progress_->setRange(0, static_cast<int>(std::max<std::size_t>(found, 1)));
                    guard->import_progress_->setValue(static_cast<int>(done));
                },
                Qt::QueuedConnection);
        };
        auto report = library->import_paths({source}, options, stop);
        // Analysis and motif indexing follow automatically in the desktop app (section 26.6).
        std::optional<library::AnalyzeReport> analysis;
        if (report && !stop.stop_requested()) {
            // Analysis re-reads the same untrusted bytes: it goes through the same isolated parser
            // as the import, never this process (section 28.2).
            library::AnalyzeOptions analyze;
            analyze.parser = parser;
            // The analysis phase reports its own progress instead of a frozen "Importing: N of N"
            // (ux-panels-31).
            auto last_analysis_report = std::make_shared<std::chrono::steady_clock::time_point>();
            analyze.progress = [guard, last_analysis_report](std::size_t done) {
                const auto now = std::chrono::steady_clock::now();
                if (now - *last_analysis_report < std::chrono::milliseconds(200))
                    return;
                *last_analysis_report = now;
                QMetaObject::invokeMethod(
                    qApp,
                    [guard, done] {
                        if (guard)
                            guard->status_->setText(tr("Indexing for melody search: %1 item(s)… (browsing stays available)").arg(done));
                    },
                    Qt::QueuedConnection);
            };
            if (auto analyzed = library->analyze(analyze, stop))
                analysis = *analyzed;
        }
        const bool cancelled = stop.stop_requested() && !thread_stop.stop_requested();
        QString text;
        if (!report) {
            text = tr("Import failed: %1").arg(QString::fromStdString(report.error().describe()));
        } else {
            const auto& counts = report->counts;
            text = tr("Imported %1, duplicates %2, unsupported %3, failed %4, changed during read %5 (of %6 found)")
                       .arg(counts.imported)
                       .arg(counts.duplicate)
                       .arg(counts.unsupported)
                       .arg(counts.failed)
                       .arg(counts.changed_during_read)
                       .arg(counts.discovered);
            if (cancelled)
                text = tr("Import cancelled. %1").arg(text);
            if (analysis)
                text += tr("; analyzed %1").arg(analysis->revisions_analyzed);
        }
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, text, cancelled] {
                if (!guard)
                    return;
                guard->import_button_->setEnabled(true);
                guard->cancel_import_button_->hide();
                guard->import_progress_->hide();
                guard->status_->setText(text);
                emit guard->status_message(text);
                guard->refresh_results();
                guard->start_watching(); // a newly imported folder is followed from now on
                if (cancelled)
                    guard->schedule_analysis(); // files imported before the cancel are still indexed
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::schedule_browse_index(bool collect_garbage) {
    if (!library_)
        return;
    auto library = library_;
    std::ignore = jobs_->submit("Library upkeep", jobs::JobPriority::maintenance, [library, collect_garbage](jobs::JobRecord& job) -> Status {
        // Failures only mean searches keep using SQL paging; nothing to report to the user.
        const auto stop = job.stop_token();
        std::ignore = library->build_browse_index(stop);
        if (collect_garbage && !stop.stop_requested())
            std::ignore = library->collect_garbage(); // default 30-day grace: only long-unreferenced objects
        return stop.stop_requested() ? make_error(ErrorCode::cancelled, "library upkeep cancelled") : ok();
    });
}

void LibraryPanel::start_watching() {
    if (!library_)
        return;
    QPointer<LibraryPanel> guard(this);
    auto library = library_;
    std::ignore = jobs_->submit("Watched folders", jobs::JobPriority::maintenance, [guard, library](jobs::JobRecord&) -> Status {
        auto roots = library->source_roots();
        if (!roots)
            return std::unexpected(roots.error());
        std::vector<std::filesystem::path> watched;
        for (const auto& root : *roots) {
            std::error_code error;
            if (root.watched && std::filesystem::is_directory(root.path, error))
                watched.push_back(root.path); // a disconnected drive is simply not watched now
        }
        // Weak: a queued call must not keep a closed library (and its writer lock) alive.
        QMetaObject::invokeMethod(
            qApp,
            [guard, weak = std::weak_ptr<library::Library>(library), watched = std::move(watched)] {
                const auto library = weak.lock();
                if (!guard || !library || guard->library_ != library)
                    return; // closed, or another library was opened meanwhile
                // The same folders are already watched (an import finished in a folder that was
                // watched from its start): keep that watcher. Replacing it would drop the changes
                // it is still batching, an edit made right after the import (seen as a flaky
                // watched-folder test in batch 10).
                if (guard->watcher_ && guard->watched_roots_ == watched)
                    return;
                const bool replacing = guard->watcher_ != nullptr && !guard->watched_roots_.empty();
                guard->watcher_.reset();
                guard->watched_roots_.clear();
                if (watched.empty())
                    return;
                if (replacing) {
                    // Changes the old watcher had not reported yet are lost with it: reconcile the
                    // folders once (unchanged files are skipped by their content hash).
                    platform::FileChangeHint rescan;
                    rescan.kind = platform::FileChangeKind::rescan_required;
                    guard->pending_hints_.push_back(std::move(rescan));
                    guard->watch_flush_.start();
                }
                auto created = platform::DirectoryWatcher::create(
                    platform::FileWatcherSettings{}, [guard](std::vector<platform::FileChangeHint> hints) {
                        // On the watcher's thread: hand the batch to the GUI thread.
                        QMetaObject::invokeMethod(
                            qApp,
                            [guard, hints = std::move(hints)]() mutable {
                                if (!guard)
                                    return;
                                guard->pending_hints_.insert(guard->pending_hints_.end(), hints.begin(), hints.end());
                                guard->watch_flush_.start(); // restarts: one batch per quiet second
                            },
                            Qt::QueuedConnection);
                    });
                if (!created)
                    return; // watching is a convenience; imports still work
                // The library may live inside a watched folder: its own files are never sources.
                (*created)->exclude(library->root());
                for (const auto& root : watched)
                    if ((*created)->add_directory(root))
                        guard->watched_roots_.push_back(root);
                guard->watcher_ = std::move(*created);
            },
            Qt::QueuedConnection);
        return ok();
    });
}

void LibraryPanel::apply_watch_hints() {
    if (!library_ || pending_hints_.empty())
        return;
    const auto hints = std::exchange(pending_hints_, {});
    // Group by the watched folder each path belongs to (the deepest one).
    struct Work {
        std::vector<std::filesystem::path> changed;
        std::vector<std::filesystem::path> missing;
        bool rescan = false;
    };
    std::map<std::filesystem::path, Work> work;
    const auto root_of = [this](const std::filesystem::path& path) -> std::optional<std::filesystem::path> {
        std::optional<std::filesystem::path> best;
        for (const auto& root : watched_roots_) {
            // Case-insensitive and normalized on Windows: the watcher may report a folder spelled
            // differently from the stored one (review_7).
            if (library::relative_inside(path, root) && (!best || root.native().size() > best->native().size()))
                best = root;
        }
        return best;
    };
    for (const auto& hint : hints) {
        if (hint.kind == platform::FileChangeKind::rescan_required) {
            for (const auto& root : watched_roots_)
                work[root].rescan = true;
            continue;
        }
        if (hint.kind == platform::FileChangeKind::renamed && !hint.previous_path.empty())
            if (const auto root = root_of(hint.previous_path))
                work[*root].missing.push_back(hint.previous_path);
        const auto root = root_of(hint.path);
        if (!root)
            continue;
        std::error_code error;
        if (std::filesystem::is_regular_file(hint.path, error))
            work[*root].changed.push_back(hint.path);
        else if (!std::filesystem::exists(hint.path, error))
            work[*root].missing.push_back(hint.path);
        // A changed directory itself carries no file to import; its files have their own hints.
    }
    QPointer<LibraryPanel> guard(this);
    auto library = library_;
    import_thread_->post([guard, library, work = std::move(work),
                          parser = import_parser_ ? import_parser_ : parser_](std::stop_token stop) {
        std::size_t updated = 0;
        std::size_t missing = 0;
        for (const auto& [root, items] : work) {
            library::ImportOptions options;
            options.parser = parser; // the isolated worker, as for every import (section 28.2)
            options.root = root;     // update the existing locations, never register a new folder
            std::vector<std::filesystem::path> sources = items.changed;
            if (items.rescan)
                sources = {root}; // notifications were lost: reconcile the whole folder
            std::ranges::sort(sources);
            sources.erase(std::ranges::unique(sources).begin(), sources.end());
            if (!sources.empty())
                if (auto report = library->import_paths(sources, options, stop))
                    updated += report->counts.imported + report->counts.source_changed;
            if (!items.missing.empty())
                if (auto marked = library->mark_sources_missing(root, items.missing))
                    missing += *marked;
        }
        if (updated > 0) {
            library::AnalyzeOptions analyze;
            analyze.parser = parser;
            std::ignore = library->analyze(analyze, stop); // melody search follows the new revisions
        }
        QMetaObject::invokeMethod(
            qApp,
            [guard, updated, missing] {
                if (!guard || (updated == 0 && missing == 0))
                    return;
                emit guard->status_message(tr("Watched folders: %1 item(s) updated, %2 source file(s) missing")
                                               .arg(updated)
                                               .arg(missing));
                guard->refresh_results();
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::schedule_analysis() {
    if (!library_)
        return;
    // Idempotent: analyze() skips revisions already indexed. Runs on the import thread with the
    // isolated parser, so melody search finds saved items without waiting for an import (ux-panels-14).
    QPointer<LibraryPanel> guard(this);
    auto library = library_;
    // Reported in the window's status bar: the panel's own line keeps describing the results.
    emit status_message(tr("Indexing for melody search…"));
    import_thread_->post([guard, library, parser = import_parser_ ? import_parser_ : parser_](std::stop_token stop) {
        library::AnalyzeOptions analyze;
        analyze.parser = parser;
        auto analyzed = library->analyze(analyze, stop);
        const QString text = analyzed ? tr("Indexed %n item(s) for melody search", nullptr,
                                           static_cast<int>(analyzed->revisions_analyzed))
                                      : tr("Indexing failed: %1").arg(QString::fromStdString(analyzed.error().describe()));
        QMetaObject::invokeMethod(
            qApp,
            [guard, text] {
                if (!guard)
                    return;
                emit guard->status_message(text);
                guard->refresh_visible(); // status column: indexing -> ready
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::refresh_visible() {
    for (int row = 0; row < model_->rowCount({}); ++row)
        if (const auto* entry = model_->row_at(row))
            refresh_row(entry->asset_id, false);
}

void LibraryPanel::refresh_results() {
    if (mode_ == ResultMode::text)
        run_search(true);
    else if (mode_ == ResultMode::trash)
        show_trash(true);
    else
        refresh_visible();
}

void LibraryPanel::load_current(
    std::function<void(LibraryPanel&, DocumentData, const LibraryRow&, std::int64_t revision)> use) {
    const LibraryRow* current = model_->row_at(table_->currentIndex().row());
    if (!current || !library_)
        return;
    const LibraryRow row = *current;
    QPointer<LibraryPanel> guard(this);
    auto library = library_;
    browse_thread_->post([guard, library, row, parser = parser_, use = std::move(use)](std::stop_token) {
        std::optional<DocumentData> document;
        QString error;
        std::int64_t revision_id = 0;
        auto asset = library->asset(row.asset_id);
        if (asset && *asset && (*asset)->current_revision_id) {
            revision_id = *(*asset)->current_revision_id;
            if (auto opened = library->open_revision(revision_id, parser))
                document = std::move(*opened);
            else
                error = QString::fromStdString(opened.error().describe());
        } else {
            error = tr("This item has no readable revision");
        }
        QMetaObject::invokeMethod(
            qApp,
            [guard, document = std::move(document), error, row, revision_id, use]() mutable {
                if (!guard)
                    return;
                if (document)
                    use(*guard, std::move(*document), row, revision_id);
                else
                    emit guard->status_message(error);
            },
            Qt::QueuedConnection);
    });
}

std::stop_token LibraryPanel::restart_search_cancellation() {
    if (search_stop_)
        search_stop_->request_stop(); // the running search ends at its next check (section 20.4)
    search_stop_ = std::make_shared<std::stop_source>();
    return search_stop_->get_token();
}

void LibraryPanel::run_search(bool keep_position) {
    if (!library_)
        return;
    set_mode(ResultMode::text);
    // A refresh (save, import, external change) keeps the current item and scroll position; a
    // new query starts at the top (section 19.4).
    keep_asset_.reset();
    keep_scroll_ = 0;
    if (keep_position) {
        if (const auto* row = model_->row_at(table_->currentIndex().row()))
            keep_asset_ = row->asset_id;
        keep_scroll_ = table_->verticalScrollBar()->value();
    }
    const std::uint64_t generation = ++*generation_;
    const auto stop = restart_search_cancellation();
    QPointer<LibraryPanel> guard(this);
    auto library = library_;
    current_query_ = search_->text().trimmed().toStdString();
    next_page_.reset();
    fetching_more_ = false;
    read_thread_->post([guard, library, query = current_query_, generation, latest = generation_, stop](std::stop_token) {
        if (generation != latest->load() || stop.stop_requested())
            return; // a newer search was typed while this one waited in the queue
        auto page = std::make_shared<PageResult>(load_page(*library, query, std::nullopt, stop));
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, generation, page]() mutable {
                if (guard)
                    guard->deliver_results(generation, std::move(page->rows), page->error, page->next);
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::fetch_more() {
    if (!library_ || !next_page_ || fetching_more_)
        return;
    fetching_more_ = true;
    const std::uint64_t generation = generation_->load();
    const auto stop = search_stop_ ? search_stop_->get_token() : std::stop_token{};
    QPointer<LibraryPanel> guard(this);
    auto library = library_;
    read_thread_->post([guard, library, query = current_query_, after = *next_page_, generation, latest = generation_,
                        stop](std::stop_token) {
        if (generation != latest->load() || stop.stop_requested())
            return;
        auto page = std::make_shared<PageResult>(load_page(*library, query, after, stop));
        QMetaObject::invokeMethod(
            qApp,
            [guard, generation, page]() mutable {
                if (guard)
                    guard->deliver_results(generation, std::move(page->rows), page->error, page->next, true);
            },
            Qt::QueuedConnection);
    }, [guard] {
        // A thrown error ends this page load; scrolling can ask again (code-uiA-23).
        QMetaObject::invokeMethod(
            qApp,
            [guard] {
                if (guard)
                    guard->fetching_more_ = false;
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::search_melody(std::vector<std::pair<Tick, std::uint8_t>> onsets_and_pitches,
                                 std::vector<Tick> durations, bool similar) {
    if (!library_) {
        emit status_message(tr("Open a library first"));
        return;
    }
    if (onsets_and_pitches.size() < (similar ? 4u : 1u)) {
        emit status_message(similar ? tr("Select at least four notes to find similar phrases")
                                    : tr("Select the notes of the melody to find"));
        return;
    }
    std::vector<search::QueryNote> notes;
    for (std::size_t index = 0; index < onsets_and_pitches.size(); ++index)
        notes.push_back(search::QueryNote{onsets_and_pitches[index].first, std::max<Tick>(1, durations[index]),
                                          onsets_and_pitches[index].second, 100});
    const std::uint64_t generation = ++*generation_;
    const auto search_stop = restart_search_cancellation();
    next_page_.reset();
    keep_asset_.reset();
    set_mode(ResultMode::melody);
    status_->setText(similar ? tr("Searching similar phrases (approximate search)") : tr("Searching the melody"));
    QPointer<LibraryPanel> guard(this);
    auto library = library_;
    read_thread_->post([guard, library, notes = std::move(notes), similar, generation, latest = generation_,
                        stop = search_stop](std::stop_token) {
        if (generation != latest->load() || stop.stop_requested())
            return;
        std::vector<LibraryRow> rows;
        QString error;
        QString label;
        struct Hit {
            std::int64_t asset = 0;
            QString description;
            TimeRange range;
        };
        std::vector<Hit> hits;
        if (similar) {
            search::ApproximateQuery query;
            query.notes = notes;
            auto result = library->similar_search(query, stop);
            if (!result) {
                error = QString::fromStdString(result.error().describe());
            } else {
                label = QString::fromStdString(result->label); // always labeled approximate with its budget
                for (const auto& match : result->matches)
                    hits.push_back(Hit{match.stream.asset_id,
                                       tr("score %1 (pitch %2, rhythm %3), transpose %4, ticks %5-%6")
                                           .arg(match.components.score, 0, 'f', 1)
                                           .arg(match.components.pitch, 0, 'f', 2)
                                           .arg(match.components.rhythm, 0, 'f', 2)
                                           .arg(match.components.transpose)
                                           .arg(match.start_tick)
                                           .arg(match.end_tick),
                                       TimeRange{match.start_tick, std::max(match.end_tick, match.start_tick + 1)}});
            }
        } else {
            search::ExactQuery query;
            query.notes = notes;
            query.mode = search::MatchMode::transposed;
            auto result = library->motif_search(query, stop);
            if (!result) {
                error = QString::fromStdString(result.error().describe());
            } else {
                label = result->partial ? tr("exact search (partial: %1)").arg(QString::fromStdString(result->partial_reason))
                                        : tr("exact search");
                for (const auto& occurrence : result->occurrences)
                    hits.push_back(Hit{occurrence.stream.asset_id, tr("transpose %1, ticks %2-%3, track %4")
                                                                       .arg(occurrence.transpose)
                                                                       .arg(occurrence.start_tick)
                                                                       .arg(occurrence.end_tick)
                                                                       .arg(occurrence.stream.track_no + 1),
                                       TimeRange{occurrence.start_tick,
                                                 std::max(occurrence.end_tick, occurrence.start_tick + 1)}});
            }
        }
        if (error.isEmpty()) {
            std::vector<std::int64_t> ids;
            for (const auto& hit : hits)
                ids.push_back(hit.asset);
            if (auto browse = library->browse_rows_for(ids)) {
                for (const auto& hit : hits)
                    for (const auto& row : *browse)
                        if (row.id == hit.asset) {
                            auto library_row = make_row(row, row.text[0].empty() ? row.sort_title : row.text[0]);
                            library_row.status = hit.description;
                            library_row.match = hit.range;
                            rows.push_back(std::move(library_row));
                        }
            }
        }
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, generation, rows = std::move(rows), error, label]() mutable {
                if (!guard)
                    return;
                const bool current = generation == guard->generation_->load();
                guard->deliver_results(generation, std::move(rows), error);
                if (current && error.isEmpty()) // never onto a newer search's status
                    guard->status_->setText(guard->status_->text() + "  [" + label + "]");
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::save_document(std::shared_ptr<const DocumentData> document, std::optional<std::int64_t> asset_id,
                                 std::optional<std::int64_t> expected_head, QString title, bool phrase, QObject* origin) {
    if (!library_) {
        emit status_message(tr("Open a library first (Open Library… in the library panel)"));
        return;
    }
    QPointer<LibraryPanel> guard(this);
    auto library = library_;
    QPointer<QObject> origin_guard(origin);
    write_thread_->post([guard, library, document = std::move(document), asset_id, expected_head, title, phrase,
                        origin_guard](std::stop_token) {
        Result<std::int64_t> revision = make_error(ErrorCode::internal, "not saved");
        std::int64_t asset = asset_id.value_or(0);
        if (asset_id) {
            library::CommitRequest request;
            request.asset_id = *asset_id;
            request.expected_head = expected_head; // never last-writer-wins (section 22.3)
            request.origin = library::RevisionOrigin::edit;
            request.label = "Saved from the editor";
            revision = library->save_document_revision(request, *document);
        } else {
            auto created = library->create_asset(*document, title.toStdString(),
                                                 phrase ? library::AssetKind::phrase : library::AssetKind::song,
                                                 phrase ? library::RevisionOrigin::extract : library::RevisionOrigin::edit);
            if (created) {
                asset = *created;
                auto info = library->asset(asset);
                revision = info && *info && (*info)->current_revision_id
                               ? Result<std::int64_t>(*(*info)->current_revision_id)
                               : Result<std::int64_t>(make_error(ErrorCode::internal, "new item has no revision"));
            } else {
                revision = std::unexpected(created.error());
            }
        }
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, revision, asset, phrase, origin_guard] {
                if (!guard)
                    return;
                if (!revision) {
                    const auto& error = revision.error();
                    emit guard->status_message(error.code == ErrorCode::conflict
                                                   ? tr("The library item changed since it was opened; the new content was kept "
                                                        "as an object. Save as a new item or reopen it.")
                                                   : tr("Not saved to the library: %1").arg(QString::fromStdString(error.describe())));
                    return;
                }
                emit guard->saved(asset, *revision, phrase, origin_guard.data());
                emit guard->status_message(tr("Saved to the library as a new revision"));
                guard->refresh_results();
                guard->schedule_analysis(); // saved items are found by melody search too
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::show_revisions() {
    const LibraryRow* row = model_->row_at(table_->currentIndex().row());
    if (!row || !library_)
        return;
    const auto asset_id = row->asset_id;
    const QString title = row->title;
    auto library = library_;
    QPointer<LibraryPanel> guard(this);
    browse_thread_->post([guard, library, asset_id, title](std::stop_token) {
        auto revisions = library->revisions(asset_id);
        auto asset = library->asset(asset_id);
        const std::optional<std::int64_t> head = asset && *asset ? (*asset)->current_revision_id : std::nullopt;
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, asset_id, title, head, revisions = std::make_shared<decltype(revisions)>(std::move(revisions))] {
                if (guard)
                    guard->present_revisions(asset_id, title, head, *revisions);
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::present_revisions(std::int64_t asset_id, const QString& title, std::optional<std::int64_t> head,
                                     const Result<std::vector<library::RevisionInfo>>& revisions) {
    if (!revisions) {
        emit status_message(QString::fromStdString(revisions.error().describe()));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Revisions of %1").arg(title));
    auto* layout = new QVBoxLayout(&dialog);
    auto* list = new QListWidget(&dialog);
    for (const auto& revision : *revisions) {
        const QString origin = QString::fromUtf8(library::revision_origin_name(revision.origin).data());
        // Local date format and the revision's own label ("source change candidate", a save
        // label) so a revision can be recognized (ux-panels-32).
        const QString when = QLocale().toString(QDateTime::fromMSecsSinceEpoch(revision.created_ms), QLocale::ShortFormat);
        const QString label = revision.label.empty() ? QString() : QStringLiteral("  \u201C%1\u201D").arg(QString::fromStdString(revision.label));
        auto* item = new QListWidgetItem(tr("#%1  %2  %3%4  %5 bytes%6%7")
                                             .arg(revision.ordinal)
                                             .arg(when)
                                             .arg(origin)
                                             .arg(label)
                                             .arg(revision.byte_count)
                                             .arg(revision.pinned ? tr("  pinned") : QString())
                                             .arg(head && *head == revision.id ? tr("  (current)") : QString()),
                                         list);
        item->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(revision.id));
        item->setData(Qt::UserRole + 1, revision.pinned);
    }
    layout->addWidget(list);
    auto* buttons = new QDialogButtonBox(&dialog);
    auto* restore = buttons->addButton(tr("Restore as New Head"), QDialogButtonBox::AcceptRole);
    // Pinning keeps a revision from ever being cleaned up (spec 24.3).
    auto* pin = buttons->addButton(tr("Pin / Unpin"), QDialogButtonBox::ActionRole);
    connect(pin, &QPushButton::clicked, &dialog, [this, list] {
        auto* item = list->currentItem();
        if (!item || !library_)
            return;
        const auto revision_id = item->data(Qt::UserRole).toLongLong();
        const bool pinned = !item->data(Qt::UserRole + 1).toBool();
        // Shown at once; written on the write thread like every catalog change (section 19.4).
        item->setData(Qt::UserRole + 1, pinned);
        QString text = item->text().remove(tr("  pinned"));
        if (pinned) {
            const qsizetype current = text.indexOf(tr("  (current)"));
            text.insert(current < 0 ? text.size() : current, tr("  pinned"));
        }
        item->setText(text);
        QPointer<LibraryPanel> guard(this);
        auto library = library_;
        write_thread_->post([guard, library, revision_id, pinned](std::stop_token) {
            auto changed = library->set_revision_pinned(revision_id, pinned);
            if (changed)
                return;
            const QString message = QString::fromStdString(changed.error().describe());
            QMetaObject::invokeMethod(
                qApp,
                [guard, message] {
                    if (guard)
                        emit guard->status_message(tr("Not pinned: %1").arg(message));
                },
                Qt::QueuedConnection);
        });
    });
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(restore, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || !list->currentItem() || !library_)
        return;
    const auto revision_id = list->currentItem()->data(Qt::UserRole).toLongLong();
    auto library = library_;
    QPointer<LibraryPanel> guard(this);
    // Restore creates a new head referencing the old content; history is kept (section 24.3).
    write_thread_->post([guard, library, asset_id, revision_id, head](std::stop_token) {
        auto restored = library->restore_revision(asset_id, revision_id, head);
        const QString message = restored ? tr("Restored revision as the new head")
                                         : tr("Not restored: %1").arg(QString::fromStdString(restored.error().describe()));
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, message] {
                if (!guard)
                    return;
                emit guard->status_message(message);
                guard->refresh_results();
                guard->schedule_analysis();
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::prepare_drag_file() {
    if (!library_ || drag_cache_.empty() || mode_ == ResultMode::trash)
        return;
    // Every selected row (a few at most) gets its file, so a multi-row drag drops all of them.
    std::vector<LibraryRow> rows;
    for (const auto asset : selected_assets())
        if (const int position = model_->position_of(asset); position >= 0 && rows.size() < 16)
            rows.push_back(*model_->row_at(position));
    if (rows.empty())
        return;
    QPointer<LibraryPanel> guard(this);
    auto library = library_;
    const auto cache = drag_cache_;
    // The files are written and closed in the background before any drag can start. Only the
    // latest selection is prepared: moving through the list does not queue a parse per row.
    const std::uint64_t generation = ++*drag_latest_;
    browse_thread_->post([guard, library, rows, cache, parser = parser_, generation, latest = drag_latest_](std::stop_token) {
        for (const auto& row : rows) {
            if (generation != latest->load())
                return;
            auto asset = library->asset(row.asset_id);
            if (!asset || !*asset || !(*asset)->current_revision_id)
                continue;
            auto document = library->open_revision(*(*asset)->current_revision_id, parser);
            if (!document)
                continue;
            midi::ExportOptions options;
            options.mode = midi::ExportMode::compatibility;
            auto bytes = midi::export_midi(*document, options);
            if (!bytes)
                continue;
            auto file = write_drag_file(cache, row.title.toStdString(), *bytes);
            if (!file)
                continue;
            const QString path = QString::fromStdWString(file->wstring());
            QMetaObject::invokeMethod(
                qApp, // context outlives the task; the lambda checks the guard
                [guard, asset_id = row.asset_id, modified = row.modified_ms, path] {
                    if (guard) // ignored when the row shows a newer revision by now
                        guard->model_->set_drag_file(asset_id, modified, path);
                },
                Qt::QueuedConnection);
        }
    });
}

void LibraryPanel::run_metadata_task(std::function<Status(library::Library&, std::int64_t)> task,
                                     std::function<void(LibraryRow&)> optimistic, bool removes_row) {
    const auto assets = selected_assets();
    if (assets.empty() || !library_)
        return;
    // Shown at once; the previous rows (with their positions) put things back if the library
    // refuses (code-uiA-7).
    struct Previous {
        int position = 0;
        LibraryRow row;
    };
    std::vector<Previous> previous;
    for (const auto asset : assets) {
        const int position = model_->position_of(asset);
        if (position < 0)
            continue;
        previous.push_back({position, *model_->row_at(position)});
        if (removes_row) {
            model_->replace_row(asset, std::nullopt);
        } else if (optimistic) {
            LibraryRow changed = previous.back().row;
            optimistic(changed);
            model_->replace_row(asset, changed);
        }
    }
    auto library = library_;
    QPointer<LibraryPanel> guard(this);
    write_thread_->post([guard, library, assets, task = std::move(task), removes_row, previous](std::stop_token) {
        std::vector<std::int64_t> failed;
        QString error;
        for (const auto asset : assets)
            if (const Status status = task(*library, asset); !status) {
                failed.push_back(asset);
                error = QString::fromStdString(status.error().describe());
            }
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, assets, failed, error, removes_row, previous] {
                if (!guard)
                    return;
                if (!failed.empty())
                    emit guard->status_message(tr("Not saved for %n item(s): %1", nullptr, static_cast<int>(failed.size()))
                                                   .arg(error));
                // Positions were recorded while removing one after another, so put rows back in
                // reverse removal order to get the original order (review_5).
                for (auto it = previous.rbegin(); it != previous.rend(); ++it) {
                    const auto& old = *it;
                    const bool refused = std::ranges::find(failed, old.row.asset_id) != failed.end();
                    if (refused && removes_row)
                        guard->model_->insert_row(old.position, old.row);
                    else if (refused)
                        guard->model_->replace_row(old.row.asset_id, old.row);
                    else if (!removes_row)
                        guard->refresh_row(old.row.asset_id, false); // the stored values (e.g. normalized tags)
                }
                // Items came back or went (restore, trash): a text search shows the library as
                // it is now, at the same place (a search started meanwhile may not have seen it).
                if (removes_row && failed.size() < assets.size() && guard->mode_ == ResultMode::text)
                    guard->run_search(true);
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::deliver_results(std::uint64_t generation, std::vector<LibraryRow> rows, QString error,
                                   std::optional<search::PageCursor> next, bool append) {
    if (generation != generation_->load())
        return; // obsolete generation: dropped (section 20.4)
    fetching_more_ = false;
    if (!error.isEmpty()) {
        // Previous results stay visible until a valid query is ready (section 25.2).
        status_->setText(tr("Query problem: %1").arg(error));
        return;
    }
    if (append)
        model_->append_rows(std::move(rows));
    else
        model_->set_rows(std::move(rows));
    next_page_ = std::move(next);
    if (!append && keep_asset_) {
        // A refresh keeps the item the user was on and where the list was scrolled.
        if (const int position = model_->position_of(*keep_asset_); position >= 0)
            table_->setCurrentIndex(model_->index(position, 0));
        table_->verticalScrollBar()->setValue(keep_scroll_);
        keep_asset_.reset();
    }
    const int count = model_->rowCount({});
    // Pages are fetched as the list scrolls; the count says when more exist (no full count query).
    QString text;
    if (count == 0) {
        text = mode_ == ResultMode::melody ? tr("No phrase matches the selected melody (items still being indexed are "
                                                "not searched)")
               : mode_ == ResultMode::trash ? tr("The trash is empty")
               : current_query_.empty()     ? tr("This library is empty. Use Add Folder… to import MIDI files.")
                                            : tr("No match (items still being indexed are not searched musically)");
    } else {
        text = next_page_ ? tr("%1+ results (scroll for more)").arg(count) : tr("%n result(s)", nullptr, count);
    }
    status_->setText(text);
}

void LibraryPanel::open_selected() {
    const auto index = table_->currentIndex();
    const LibraryRow* row = model_->row_at(index.row());
    if (!row || !library_)
        return;
    QPointer<LibraryPanel> guard(this);
    auto library = library_;
    const auto asset_id = row->asset_id;
    const auto reveal = row->match;
    browse_thread_->post([guard, library, asset_id, reveal, parser = parser_](std::stop_token) {
        std::optional<DocumentData> document;
        QString error;
        std::int64_t revision_id = 0;
        auto asset = library->asset(asset_id);
        if (asset && *asset && (*asset)->current_revision_id) {
            revision_id = *(*asset)->current_revision_id;
            auto opened = library->open_revision(revision_id, parser);
            if (opened)
                document = std::move(*opened);
            else
                error = QString::fromStdString(opened.error().describe());
        } else {
            error = tr("This item has no readable revision");
        }
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, document = std::move(document), error, asset_id, revision_id, reveal]() mutable {
                if (!guard)
                    return;
                if (document) // a working document; a melody result opens at its match
                    emit guard->open_requested(std::move(*document), asset_id, revision_id, reveal);
                else
                    emit guard->status_message(error);
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::set_metadata(int action) {
    const LibraryRow* row = model_->row_at(table_->currentIndex().row());
    if (!row || !library_)
        return;
    if (action == 0) {
        const bool favorite = !row->favorite; // one new value for every selected item
        run_metadata_task([favorite](library::Library& library, std::int64_t asset) { return library.set_favorite(asset, favorite); },
                          [favorite](LibraryRow& changed) { changed.favorite = favorite; });
        return;
    }
    const int rating = action - 10;
    run_metadata_task([rating](library::Library& library, std::int64_t asset) { return library.set_rating(asset, rating); },
                      [rating](LibraryRow& changed) { changed.rating = rating; });
}

void LibraryPanel::poll_changes() {
    if (!library_ || polling_changes_)
        return;
    polling_changes_ = true;
    QPointer<LibraryPanel> guard(this);
    auto library = library_;
    const auto since = change_revision_.value_or(0);
    const bool first = !change_revision_;
    browse_thread_->post([guard, library, since, first](std::stop_token) {
        auto changes = std::make_shared<Result<library::ChangeSet>>(library->changes_since(since));
        QMetaObject::invokeMethod(
            qApp,
            [guard, changes, first] {
                if (!guard)
                    return;
                guard->polling_changes_ = false;
                if (!*changes)
                    return;
                const auto& batch = **changes;
                guard->change_revision_ = batch.latest_revision;
                if (first)
                    return; // the first poll only learns where the outbox stands
                if (batch.full_reload)
                    guard->schedule_browse_index(false); // searches use SQL until it is rebuilt
                if (batch.full_reload || batch.structure_changed) {
                    guard->refresh_results(); // text results keep their item and scroll position
                    return;
                }
                std::size_t unseen = 0;
                for (const auto asset : batch.asset_ids) {
                    if (guard->model_->contains_asset(asset))
                        guard->refresh_row(asset, false);
                    else
                        ++unseen;
                }
                // New items never reset the list under the user (no refresh storm during an
                // import, code-uiA-6): the status offers Refresh instead.
                if (unseen > 0 && guard->mode_ == ResultMode::text && !guard->cancel_import_button_->isVisible())
                    guard->status_->setText(
                        tr("%n new or changed item(s) are not in this list yet; press Refresh to show them", nullptr,
                           static_cast<int>(unseen)));
            },
            Qt::QueuedConnection);
    }, [guard] {
        // A thrown storage error ends this poll, not polling for good (code-uiA-23).
        QMetaObject::invokeMethod(
            qApp,
            [guard] {
                if (guard)
                    guard->polling_changes_ = false;
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::refresh_row(std::int64_t asset_id, bool removed) {
    if (removed || !library_) {
        model_->replace_row(asset_id, std::nullopt);
        return;
    }
    QPointer<LibraryPanel> guard(this);
    auto library = library_;
    browse_thread_->post([guard, library, asset_id](std::stop_token) {
        std::optional<LibraryRow> row;
        if (auto browse = library->browse_rows_for({asset_id}); browse && !browse->empty()) {
            const auto& found = browse->front();
            row = make_row(found, found.text[0].empty() ? found.sort_title : found.text[0]);
        }
        QMetaObject::invokeMethod(
            qApp,
            [guard, asset_id, row] {
                if (!guard || !row)
                    return;
                guard->model_->replace_row(asset_id, row);
                // A new revision dropped the prepared drag file: prepare it again if selected.
                if (std::ranges::contains(guard->selected_assets(), asset_id))
                    guard->prepare_drag_file();
            },
            Qt::QueuedConnection);
    });
}

} // namespace mwb::ui
