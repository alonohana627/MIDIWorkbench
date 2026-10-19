#pragma once

#include "mwb/core/model.hpp"

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QWidget>

#include <unordered_map>
#include <vector>

class QLineEdit;
class QTableView;

namespace mwb::ui {

class DocumentController;

/// Flattened, virtualized view of every event of the active sequence (section 11.3): absolute
/// tick, delta, musical position, track, channel, type, decoded data, raw payload and warnings.
/// Rows refer to events by (track index, event index) and are rebuilt per document generation.
class EventTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { column_tick, column_delta, column_position, column_track, column_channel, column_type, column_data, column_raw, column_warning, column_count };

    explicit EventTableModel(DocumentController& controller, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    /// Editing tick and data bytes validates lengths and legal ranges and becomes one command.
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;

    /// Event behind a row.
    [[nodiscard]] const Event* event_at(int row) const;
    [[nodiscard]] TrackId track_at(int row) const;
    /// Row of an event identity, or -1.
    [[nodiscard]] int row_of(EventId id) const;

private:
    void rebuild();

    struct Row {
        std::uint32_t track = 0;
        std::uint32_t event = 0;
    };
    DocumentController& controller_;
    std::vector<Row> rows_;
    std::unordered_map<EventId, int, UuidHash> row_by_event_;    std::size_t built_sequence_ = static_cast<std::size_t>(-1);
};

/// Event list panel: filter field plus a table synchronized with the piano roll selection.
class EventListPanel : public QWidget {
    Q_OBJECT

public:
    explicit EventListPanel(DocumentController& controller, QWidget* parent = nullptr);

private:
    void sync_from_selection();
    void sync_to_selection();

    DocumentController& controller_;
    EventTableModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    QTableView* table_ = nullptr;
    QLineEdit* filter_ = nullptr;
    bool syncing_ = false;
};

/// Human description of an event's data ("C4 vel 100", "CC 7 Volume = 90", "Tempo 120.000 BPM").
[[nodiscard]] QString describe_event(const Event& event);
/// Short type name ("Note On", "Control Change", "Tempo", ...).
[[nodiscard]] QString event_type_name(const Event& event);

} // namespace mwb::ui
