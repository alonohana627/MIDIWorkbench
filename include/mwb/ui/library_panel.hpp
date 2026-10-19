#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/model.hpp"
#include "mwb/library/library.hpp"
#include "mwb/midi/codec.hpp"
#include "mwb/search/sql.hpp"
#include "mwb/ui/task_thread.hpp"

#include <QAbstractTableModel>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <functional>
#include <map>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;

class QStackedWidget;

namespace mwb::library {
class Library;
}

namespace mwb::ui {

/// One result row shown in the library browser.
struct LibraryRow {
    std::int64_t asset_id = 0;
    QString title;
    QString tags;
    int rating = 0;
    bool favorite = false;
    QString tempo;
    QString key;
    QString length;
    QString status;
};

/// Virtualized result table model fed by pages of keyset-paginated results.
class LibraryResultModel : public QAbstractTableModel {
    Q_OBJECT

public:
    using QAbstractTableModel::QAbstractTableModel;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void set_rows(std::vector<LibraryRow> rows);
    void append_rows(std::vector<LibraryRow> rows);
    /// Replaces the row of `asset_id` (keeps position, selection and paging), or removes it.
    void replace_row(std::int64_t asset_id, std::optional<LibraryRow> row);
    [[nodiscard]] bool contains_asset(std::int64_t asset_id) const;
    [[nodiscard]] const LibraryRow* row_at(int row) const;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] QStringList mimeTypes() const override;
    [[nodiscard]] QMimeData* mimeData(const QModelIndexList& indexes) const override;
    /// A complete .mid prepared in the drag cache for an asset (created before any drag starts).
    void set_drag_file(std::int64_t asset_id, QString path) { drag_files_[asset_id] = std::move(path); }

private:
    std::vector<LibraryRow> rows_;
    std::map<std::int64_t, QString> drag_files_;
};

/// Left library dock (sections 5.2, 6.2, 24, 25): open or create a library, import folders in the
/// background while browsing, search with the query language (debounced 120 ms, obsolete
/// generations dropped), browse pages, and open results. Selection alone is silent; Enter or
/// double-click opens. The GUI thread never waits for SQLite: queries run on a worker thread.
class LibraryPanel : public QWidget {
    Q_OBJECT

public:
    explicit LibraryPanel(QWidget* parent = nullptr);
    ~LibraryPanel() override;

    void focus_search();
    /// Directory for temporary .mid files dragged out to DAWs.
    void set_drag_cache(std::filesystem::path cache) { drag_cache_ = std::move(cache); }
    /// Parser for opening items, and a separate one for imports, so an import never delays an open
    /// (the application passes two isolated worker processes).
    void set_parser(midi::MidiParser parser) { parser_ = std::move(parser); }
    void set_import_parser(midi::MidiParser parser) { import_parser_ = std::move(parser); }
    /// Musical content search with notes taken from the editor selection (section 5.3):
    /// transposition-invariant exact matches, or similar phrases with explained scores.
    void search_melody(std::vector<std::pair<Tick, std::uint8_t>> onsets_and_pitches, std::vector<Tick> durations,
                       bool similar);
    /// Saves a document snapshot: a new revision of `asset_id` (checked against `expected_head`)
    /// or, without an asset, a new library item. Emits saved() or a status message.
    void save_document(std::shared_ptr<const DocumentData> document, std::optional<std::int64_t> asset_id,
                       std::optional<std::int64_t> expected_head, QString title, bool phrase, QObject* origin = nullptr);
    /// Shows the revision history of the selected result with Restore (section 24.3).
    void show_revisions();
    /// Opens (or creates) the library at `root` without blocking the GUI.
    void open_library(const std::filesystem::path& root, bool create);
    [[nodiscard]] bool is_open() const { return library_ != nullptr; }

signals:
    void open_requested(DocumentData document, qint64 asset_id, qint64 revision_id);
    /// `origin` is the object passed to save_document (it may have been destroyed: compare only).
    void saved(qint64 asset_id, qint64 revision_id, bool phrase, QObject* origin);
    void status_message(const QString& text);
    /// A library was opened or created (the main window then shows the panel).
    void opened();

private:
    void choose_library();
    void present_revisions(std::int64_t asset_id, const QString& title, std::optional<std::int64_t> head,
                           const Result<std::vector<library::RevisionInfo>>& revisions);
    void import_folder();
    void run_search();
    void open_selected();
    void deliver_results(std::uint64_t generation, std::vector<LibraryRow> rows, QString error,
                         std::optional<search::PageCursor> next = std::nullopt, bool append = false);
    /// Loads the next page of the current text search (keyset cursor) when scrolled to the end.
    void fetch_more();
    /// Cancels in-flight searches (a newer one replaces them) and returns the new request's token.
    std::stop_token restart_search_cancellation();
    void set_metadata(int column_kind);
    void prepare_drag_file();
    /// Runs one metadata change on the library thread, then refreshes the results.
    void run_metadata_task(std::function<Status(library::Library&, std::int64_t)> task, bool removes_row = false);
    /// Re-reads one item's display row after a metadata change (no re-search).
    void refresh_row(std::int64_t asset_id, bool removed);
    /// Reads the library's change outbox (section 19.4) and refreshes changed rows in place, so
    /// edits from the command line or another window appear without re-running the search.
    void poll_changes();
    std::filesystem::path drag_cache_;

    std::shared_ptr<library::Library> library_;
    LibraryResultModel* model_ = nullptr;
    QLineEdit* search_ = nullptr;
    QTableView* table_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* import_button_ = nullptr;
    QPushButton* cancel_import_button_ = nullptr;
    std::shared_ptr<std::stop_source> import_stop_;
    QTimer debounce_;
    /// Latest search generation; shared with queued tasks so obsolete searches are skipped.
    std::shared_ptr<std::atomic<std::uint64_t>> generation_ = std::make_shared<std::atomic<std::uint64_t>>(0);
    std::vector<std::jthread> workers_;
    QStackedWidget* pages_ = nullptr;
    QLabel* empty_status_ = nullptr;
    midi::MidiParser parser_;
    midi::MidiParser import_parser_;
    std::shared_ptr<std::stop_source> search_stop_;
    QTimer change_poll_;
    std::optional<std::int64_t> change_revision_;
    bool polling_changes_ = false;
    std::optional<search::PageCursor> next_page_;
    std::string current_query_;
    bool fetching_more_ = false;
    /// Owned background threads: searches; opens, revisions and drag files; user writes (drained on
    /// close); imports (so a save never waits behind a whole import).
    std::unique_ptr<TaskThread> read_thread_ = std::make_unique<TaskThread>();
    std::unique_ptr<TaskThread> browse_thread_ = std::make_unique<TaskThread>();
    std::unique_ptr<TaskThread> write_thread_ = std::make_unique<TaskThread>(true);
    std::unique_ptr<TaskThread> import_thread_ = std::make_unique<TaskThread>();
    std::shared_ptr<std::atomic<std::uint64_t>> drag_latest_ = std::make_shared<std::atomic<std::uint64_t>>(0);
};

} // namespace mwb::ui
