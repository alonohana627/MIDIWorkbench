#include "mwb/ui/library_panel.hpp"

#include "mwb/ui/task_thread.hpp"

#include "mwb/core/pitch.hpp"
#include "mwb/library/library.hpp"
#include "mwb/midi/codec.hpp"
#include "mwb/ui/drag_export.hpp"

#include <QMimeData>
#include <QUrl>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QListWidget>
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
#include <QMenu>
#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
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
    if (!row || role != Qt::DisplayRole)
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
    if (const auto* row = row_at(index.row()); row && drag_files_.contains(row->asset_id))
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
        const auto found = drag_files_.find(row->asset_id);
        if (found != drag_files_.end()) {
            const auto url = QUrl::fromLocalFile(found->second);
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
    auto* open_button = new QPushButton(tr("Library…"), this);
    open_button->setToolTip(tr("Open or create a managed library folder"));
    import_button_ = new QPushButton(tr("Add Folder…"), this);
    import_button_->setToolTip(tr("Import a folder; browsing continues while it imports"));
    import_button_->setEnabled(false);
    cancel_import_button_ = new QPushButton(tr("Cancel Import"), this);
    cancel_import_button_->setToolTip(tr("Stop the running import; files already imported stay"));
    cancel_import_button_->hide();
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
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(20);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setAccessibleName(tr("Library results"));
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->setDragEnabled(true);
    table_->setDragDropMode(QAbstractItemView::DragOnly);
    status_ = new QLabel(tr("No library open"), this);
    status_->setWordWrap(true);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(open_button);
    buttons->addWidget(import_button_);
    buttons->addWidget(cancel_import_button_);
    // Page 1: the open library. Page 0: what a library is and how to start one.
    auto* browse = new QWidget(this);
    auto* layout = new QVBoxLayout(browse);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    layout->addLayout(buttons);
    layout->addWidget(search_);
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
                                      "and melody, and keeps every saved revision."),
                                   empty);
    explanation->setWordWrap(true);
    explanation->setForegroundRole(QPalette::PlaceholderText);
    auto* start_button = new QPushButton(tr("Create or Open Library…"), empty);
    start_button->setDefault(true);
    connect(start_button, &QPushButton::clicked, this, &LibraryPanel::choose_library);
    empty_status_ = new QLabel(empty);
    empty_status_->setWordWrap(true);
    empty_layout->addWidget(heading);
    empty_layout->addWidget(explanation);
    empty_layout->addWidget(start_button, 0, Qt::AlignLeft);
    empty_layout->addWidget(empty_status_);
    empty_layout->addStretch(1);

    pages_ = new QStackedWidget(this);
    pages_->addWidget(empty);
    pages_->addWidget(browse);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(pages_);
    debounce_.setSingleShot(true);
    debounce_.setInterval(120); // typing debounce (section 33.3)
    connect(&debounce_, &QTimer::timeout, this, &LibraryPanel::run_search);
    connect(search_, &QLineEdit::textChanged, this, [this] { debounce_.start(); });
    connect(open_button, &QPushButton::clicked, this, &LibraryPanel::choose_library);
    connect(import_button_, &QPushButton::clicked, this, &LibraryPanel::import_folder);
    connect(cancel_import_button_, &QPushButton::clicked, this, [this] {
        if (import_stop_)
            import_stop_->request_stop();
        status_->setText(tr("Cancelling the import…"));
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] { prepare_drag_file(); });
    // Single click selects silently; double-click or Enter opens (section 6.2).
    connect(table_, &QTableView::activated, this, [this] { open_selected(); });
    connect(table_, &QTableView::customContextMenuRequested, this, [this](const QPoint& point) {
        const QModelIndex clicked = table_->indexAt(point);
        if (!clicked.isValid())
            return; // no item under the pointer: nothing to act on
        table_->setCurrentIndex(clicked);
        QMenu menu(this);
        menu.addAction(tr("Open"), this, &LibraryPanel::open_selected);
        menu.addAction(tr("Revisions…"), this, &LibraryPanel::show_revisions);
        menu.addAction(tr("Toggle Favorite"), this, [this] { set_metadata(0); });
        menu.addAction(tr("Add Tag…"), this, [this] {
            bool accepted = false;
            const QString tag = QInputDialog::getText(this, tr("Add tag"), tr("Tag"), QLineEdit::Normal, {}, &accepted);
            if (accepted && !tag.trimmed().isEmpty())
                run_metadata_task([tag = tag.trimmed().toStdString()](library::Library& library, std::int64_t asset) {
                    return library.add_tag(asset, tag);
                });
        });
        menu.addAction(tr("Remove Tag…"), this, [this] {
            bool accepted = false;
            const QString tag = QInputDialog::getText(this, tr("Remove tag"), tr("Tag"), QLineEdit::Normal, {}, &accepted);
            if (accepted && !tag.trimmed().isEmpty())
                run_metadata_task([tag = tag.trimmed().toStdString()](library::Library& library, std::int64_t asset) {
                    return library.remove_tag(asset, tag);
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
        menu.addAction(tr("Move to Trash…"), this, [this] {
            const LibraryRow* row = model_->row_at(table_->currentIndex().row());
            if (!row ||
                QMessageBox::question(this, tr("Move to Trash"),
                                      tr("Move \"%1\" to the trash? Re-importing the file brings it back.").arg(row->title)) !=
                    QMessageBox::Yes)
                return;
            run_metadata_task([](library::Library& library, std::int64_t asset) {
                return library.trash(std::vector<std::int64_t>{asset}); // a metadata state; restorable
            });
        });
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
    const QString last = QSettings().value("library/last").toString();
    if (!last.isEmpty())
        open_library(std::filesystem::path(last.toStdWString()), false);
}

LibraryPanel::~LibraryPanel() {
    // Background work ends with the panel. Searches and the import see their stop tokens; queued
    // user writes (saves, tags, ratings, restores) still run before the write thread ends.
    if (search_stop_)
        search_stop_->request_stop();
    if (import_stop_)
        import_stop_->request_stop();
    import_thread_.reset();
    read_thread_.reset();
    browse_thread_.reset();
    write_thread_.reset(); // drains
}

void LibraryPanel::focus_search() {
    search_->setFocus();
    search_->selectAll();
}

void LibraryPanel::choose_library() {
    const QString directory = QFileDialog::getExistingDirectory(this, tr("Choose or create a library folder"));
    if (directory.isEmpty())
        return;
    const std::filesystem::path root(directory.toStdWString());
    std::error_code error;
    const bool exists = std::filesystem::exists(root / "library.sqlite", error);
    open_library(root, !exists);
}

void LibraryPanel::open_library(const std::filesystem::path& root, bool create) {
    status_->setText(tr("Opening library…"));
    QPointer<LibraryPanel> guard(this);
    write_thread_->post([guard, root, create](std::stop_token) {
        auto opened = create ? library::Library::create(root) : library::Library::open(root);
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, root, opened = std::make_shared<decltype(opened)>(std::move(opened))]() mutable {
                if (!guard)
                    return;
                if (!*opened) {
                    const QString message =
                        tr("Library unavailable: %1").arg(QString::fromStdString((*opened).error().describe()));
                    guard->status_->setText(message);
                    guard->empty_status_->setText(message);
                    return;
                }
                guard->library_ = std::shared_ptr<library::Library>(std::move(**opened));
                guard->pages_->setCurrentIndex(1);
                emit guard->opened(); // after library_ is set, so listeners see the new library
                QSettings().setValue("library/last", QString::fromStdWString(root.wstring()));
                guard->import_button_->setEnabled(true);
                guard->run_search();
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::import_folder() {
    if (!library_)
        return;
    const QString directory = QFileDialog::getExistingDirectory(this, tr("Add a folder of MIDI files"));
    if (directory.isEmpty())
        return;
    status_->setText(tr("Importing %1… (browsing stays available)").arg(directory));
    QPointer<LibraryPanel> guard(this);
    auto library = library_;
    const std::filesystem::path source(directory.toStdWString());
    import_stop_ = std::make_shared<std::stop_source>();
    import_button_->setEnabled(false);
    cancel_import_button_->show();
    import_thread_->post([guard, library, source, parser = import_parser_ ? import_parser_ : parser_,
                         cancel = import_stop_](std::stop_token thread_stop) {
        // Cancel button or panel shutdown, whichever comes first.
        std::stop_callback forward(thread_stop, [cancel] { cancel->request_stop(); });
        const auto stop = cancel->get_token();
        library::ImportOptions options;
        options.parser = parser; // parsing runs in the isolated worker process (section 28.2)
        auto last_report = std::make_shared<std::chrono::steady_clock::time_point>();
        options.progress = [guard, last_report](std::size_t done, std::size_t found) {
            const auto now = std::chrono::steady_clock::now();
            if (now - *last_report < std::chrono::milliseconds(200) && done != found)
                return; // at most five updates a second
            *last_report = now;
            QMetaObject::invokeMethod(
                qApp,
                [guard, done, found] {
                    if (guard)
                        guard->status_->setText(tr("Importing: %1 of %2 files… (browsing stays available)").arg(done).arg(found));
                },
                Qt::QueuedConnection);
        };
        auto report = library->import_paths({source}, options, stop);
        // Analysis and motif indexing follow automatically in the desktop app (section 26.6).
        std::optional<library::AnalyzeReport> analysis;
        if (report) {
            if (auto analyzed = library->analyze({}, stop))
                analysis = *analyzed;
        }
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
            if (analysis)
                text += tr("; analyzed %1").arg(analysis->revisions_analyzed);
        }
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, text] {
                if (!guard)
                    return;
                guard->import_button_->setEnabled(true);
                guard->cancel_import_button_->hide();
                guard->status_->setText(text);
                emit guard->status_message(text);
                guard->run_search();
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

void LibraryPanel::run_search() {
    if (!library_)
        return;
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
        std::vector<std::pair<std::int64_t, QString>> hits; // asset, description
        if (similar) {
            search::ApproximateQuery query;
            query.notes = notes;
            auto result = library->similar_search(query, stop);
            if (!result) {
                error = QString::fromStdString(result.error().describe());
            } else {
                label = QString::fromStdString(result->label); // always labeled approximate with its budget
                for (const auto& match : result->matches)
                    hits.emplace_back(match.stream.asset_id,
                                      tr("score %1 (pitch %2, rhythm %3), transpose %4, ticks %5-%6")
                                          .arg(match.components.score, 0, 'f', 1)
                                          .arg(match.components.pitch, 0, 'f', 2)
                                          .arg(match.components.rhythm, 0, 'f', 2)
                                          .arg(match.components.transpose)
                                          .arg(match.start_tick)
                                          .arg(match.end_tick));
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
                    hits.emplace_back(occurrence.stream.asset_id, tr("transpose %1, ticks %2-%3, track %4")
                                                                      .arg(occurrence.transpose)
                                                                      .arg(occurrence.start_tick)
                                                                      .arg(occurrence.end_tick)
                                                                      .arg(occurrence.stream.track_no + 1));
            }
        }
        if (error.isEmpty()) {
            std::vector<std::int64_t> ids;
            for (const auto& hit : hits)
                ids.push_back(hit.first);
            if (auto browse = library->browse_rows_for(ids)) {
                for (const auto& hit : hits)
                    for (const auto& row : *browse)
                        if (row.id == hit.first) {
                            auto library_row = make_row(row, row.text[0].empty() ? row.sort_title : row.text[0]);
                            library_row.status = hit.second;
                            rows.push_back(std::move(library_row));
                        }
            }
        }
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, generation, rows = std::move(rows), error, label]() mutable {
                if (!guard)
                    return;
                guard->deliver_results(generation, std::move(rows), error);
                if (error.isEmpty())
                    guard->status_->setText(guard->status_->text() + "  [" + label + "]");
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::save_document(std::shared_ptr<const DocumentData> document, std::optional<std::int64_t> asset_id,
                                 std::optional<std::int64_t> expected_head, QString title, bool phrase, QObject* origin) {
    if (!library_) {
        emit status_message(tr("Open a library first (Library… in the library panel)"));
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
                guard->run_search();
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
        auto* item = new QListWidgetItem(tr("#%1  %2  %3  %4 bytes%5%6")
                                             .arg(revision.ordinal)
                                             .arg(QDateTime::fromMSecsSinceEpoch(revision.created_ms).toString())
                                             .arg(origin)
                                             .arg(revision.byte_count)
                                             .arg(revision.pinned ? tr("  pinned") : QString())
                                             .arg(head && *head == revision.id ? tr("  (current)") : QString()),
                                         list);
        item->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(revision.id));
    }
    layout->addWidget(list);
    auto* buttons = new QDialogButtonBox(&dialog);
    auto* restore = buttons->addButton(tr("Restore as New Head"), QDialogButtonBox::AcceptRole);
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
                guard->run_search();
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::prepare_drag_file() {
    const LibraryRow* row = model_->row_at(table_->currentIndex().row());
    if (!row || !library_ || drag_cache_.empty())
        return;
    QPointer<LibraryPanel> guard(this);
    auto library = library_;
    const auto asset_id = row->asset_id;
    const std::string title = row->title.toStdString();
    const auto cache = drag_cache_;
    // The file is written and closed in the background before any drag can start. Only the latest
    // row is prepared: moving through the list does not queue a parse per row.
    const std::uint64_t generation = ++*drag_latest_;
    browse_thread_->post([guard, library, asset_id, title, cache, parser = parser_, generation,
                          latest = drag_latest_](std::stop_token) {
        if (generation != latest->load())
            return;
        auto asset = library->asset(asset_id);
        if (!asset || !*asset || !(*asset)->current_revision_id)
            return;
        auto document = library->open_revision(*(*asset)->current_revision_id, parser);
        if (!document)
            return;
        midi::ExportOptions options;
        options.mode = midi::ExportMode::compatibility;
        auto bytes = midi::export_midi(*document, options);
        if (!bytes)
            return;
        auto file = write_drag_file(cache, title, *bytes);
        if (!file)
            return;
        const QString path = QString::fromStdWString(file->wstring());
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, asset_id, path] {
                if (guard)
                    guard->model_->set_drag_file(asset_id, path);
            },
            Qt::QueuedConnection);
    });
}

void LibraryPanel::run_metadata_task(std::function<Status(library::Library&, std::int64_t)> task) {
    const LibraryRow* row = model_->row_at(table_->currentIndex().row());
    if (!row || !library_)
        return;
    const auto asset_id = row->asset_id;
    auto library = library_;
    QPointer<LibraryPanel> guard(this);
    write_thread_->post([guard, library, asset_id, task = std::move(task)](std::stop_token) {
        const Status status = task(*library, asset_id);
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, status] {
                if (!guard)
                    return;
                if (!status)
                    emit guard->status_message(tr("Not saved: %1").arg(QString::fromStdString(status.error().describe())));
                guard->run_search();
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
    const int count = model_->rowCount({});
    // Pages are fetched as the list scrolls; the count says when more exist (no full count query).
    status_->setText(count == 0 && current_query_.empty()
                         ? tr("This library is empty. Use Add Folder… to import MIDI files.")
                     : count == 0 ? tr("No match (items still being indexed are not searched musically)")
                     : next_page_ ? tr("%1+ results (scroll for more)").arg(count)
                                  : tr("%n result(s)", nullptr, count));
}

void LibraryPanel::open_selected() {
    const auto index = table_->currentIndex();
    const LibraryRow* row = model_->row_at(index.row());
    if (!row || !library_)
        return;
    QPointer<LibraryPanel> guard(this);
    auto library = library_;
    const auto asset_id = row->asset_id;
    browse_thread_->post([guard, library, asset_id, parser = parser_](std::stop_token) {
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
            [guard, document = std::move(document), error, asset_id, revision_id]() mutable {
                if (!guard)
                    return;
                if (document)
                    emit guard->open_requested(std::move(*document), asset_id, revision_id); // a working document
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
    const auto asset_id = row->asset_id;
    const bool favorite = !row->favorite;
    auto library = library_;
    QPointer<LibraryPanel> guard(this);
    write_thread_->post([guard, library, asset_id, action, favorite](std::stop_token) {
        const Status status = action == 0 ? library->set_favorite(asset_id, favorite)
                                          : library->set_rating(asset_id, action - 10);
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, status] {
                if (!guard)
                    return;
                if (!status)
                    emit guard->status_message(tr("Not saved: %1").arg(QString::fromStdString(status.error().describe())));
                guard->run_search();
            },
            Qt::QueuedConnection);
    });
}

} // namespace mwb::ui
