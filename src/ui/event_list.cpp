#include "mwb/ui/event_list.hpp"

#include "mwb/core/change_set.hpp"
#include "mwb/core/hashing.hpp"
#include "mwb/core/pitch.hpp"
#include "mwb/core/text.hpp"
#include "mwb/core/validation.hpp"
#include "mwb/ui/controller_lane.hpp"
#include "mwb/ui/document_controller.hpp"

#include <QHeaderView>
#include <QItemSelectionModel>
#include <QAction>
#include <QLineEdit>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <tuple>

namespace mwb::ui {

QString event_type_name(const Event& event) {
    switch (event.category) {
    case EventCategory::channel:
        switch (event.message()) {
        case ChannelMessage::note_off: return QObject::tr("Note Off");
        case ChannelMessage::note_on: return event.data2() == 0 ? QObject::tr("Note Off (vel 0)") : QObject::tr("Note On");
        case ChannelMessage::poly_pressure: return QObject::tr("Poly Pressure");
        case ChannelMessage::control_change: return QObject::tr("Control Change");
        case ChannelMessage::program_change: return QObject::tr("Program Change");
        case ChannelMessage::channel_pressure: return QObject::tr("Channel Pressure");
        case ChannelMessage::pitch_bend: return QObject::tr("Pitch Bend");
        }
        break;
    case EventCategory::meta:
        switch (event.code) {
        case meta_type::text: return QObject::tr("Text");
        case meta_type::copyright: return QObject::tr("Copyright");
        case meta_type::track_name: return QObject::tr("Track Name");
        case meta_type::instrument_name: return QObject::tr("Instrument Name");
        case meta_type::lyric: return QObject::tr("Lyric");
        case meta_type::marker: return QObject::tr("Marker");
        case meta_type::cue_point: return QObject::tr("Cue Point");
        case meta_type::tempo: return QObject::tr("Tempo");
        case meta_type::time_signature: return QObject::tr("Time Signature");
        case meta_type::key_signature: return QObject::tr("Key Signature");
        case meta_type::port_prefix: return QObject::tr("Port");
        case meta_type::channel_prefix: return QObject::tr("Channel Prefix");
        case meta_type::smpte_offset: return QObject::tr("SMPTE Offset");
        case meta_type::sequencer_specific: return QObject::tr("Sequencer Specific");
        default: return QObject::tr("Meta 0x%1").arg(event.code, 2, 16, QLatin1Char('0'));
        }
    case EventCategory::sysex_f0: return QObject::tr("SysEx (F0)");
    case EventCategory::sysex_f7: return QObject::tr("SysEx (F7)");
    }
    return QObject::tr("Unknown");
}

QString describe_event(const Event& event) {
    const auto bytes = event.payload.bytes();
    switch (event.category) {
    case EventCategory::channel:
        switch (event.message()) {
        case ChannelMessage::note_off:
        case ChannelMessage::note_on:
            return QString("%1  vel %2").arg(QString::fromStdString(pitch_name(event.data1()))).arg(event.data2());
        case ChannelMessage::poly_pressure:
            return QString("%1  pressure %2").arg(QString::fromStdString(pitch_name(event.data1()))).arg(event.data2());
        case ChannelMessage::control_change:
            return QString("%1 = %2").arg(controller_name(event.data1())).arg(event.data2());
        case ChannelMessage::program_change: return QObject::tr("Program %1").arg(event.data1() + 1);
        case ChannelMessage::channel_pressure: return QObject::tr("Pressure %1").arg(event.data1());
        case ChannelMessage::pitch_bend: return QObject::tr("Bend %1").arg(event.pitch_bend_value() - 8192);
        }
        break;
    case EventCategory::meta:
        if (event.code == meta_type::tempo && bytes.size() == 3) {
            const int microseconds = (bytes[0] << 16) | (bytes[1] << 8) | bytes[2];
            if (microseconds > 0)
                return QObject::tr("%1 BPM (%2 µs/quarter)").arg(60'000'000.0 / microseconds, 0, 'f', 3).arg(microseconds);
        }
        if (event.code == meta_type::time_signature && bytes.size() == 4 && bytes[1] < 16)
            return QString("%1/%2").arg(bytes[0]).arg(1 << bytes[1]);
        if (event.code == meta_type::key_signature && bytes.size() == 2) {
            const auto sharps = static_cast<std::int8_t>(bytes[0]);
            return QObject::tr("%1 %2, %3").arg(std::abs(sharps)).arg(sharps >= 0 ? QObject::tr("sharps") : QObject::tr("flats"))
                .arg(bytes[1] ? QObject::tr("minor") : QObject::tr("major"));
        }
        if (event.code >= meta_type::text && event.code <= meta_type::device_name) {
            bool fallback = false;
            const auto text = decode_midi_text(bytes, &fallback);
            return QString::fromStdString(text) + (fallback ? QObject::tr("  [not UTF-8; shown as Windows-1252]") : QString());
        }
        return QObject::tr("%1 byte(s)").arg(bytes.size());
    case EventCategory::sysex_f0:
    case EventCategory::sysex_f7: return QObject::tr("%1 byte(s)").arg(bytes.size());
    }
    return {};
}

EventTableModel::EventTableModel(DocumentController& controller, QObject* parent)
    : QAbstractTableModel(parent), controller_(controller) {
    connect(&controller_, &DocumentController::document_changed, this, &EventTableModel::rebuild);
    // View state changes (scrubbing, zoom) fire constantly; only a sequence switch changes the rows.
    connect(&controller_, &DocumentController::view_state_changed, this, [this] {
        if (controller_.active_sequence_index() != built_sequence_)
            rebuild();
    });
    rebuild();
}

void EventTableModel::rebuild() {
    built_sequence_ = controller_.active_sequence_index();
    beginResetModel();
    rows_.clear();
    const auto& sequence = controller_.active_sequence();
    std::size_t total = 0;
    for (const auto& track : sequence.tracks)
        total += track.events.size();
    rows_.reserve(total);
    for (std::uint32_t track = 0; track < sequence.tracks.size(); ++track)
        for (std::uint32_t event = 0; event < sequence.tracks[track].events.size(); ++event)
            rows_.push_back(Row{track, event});
    // Merged stable order (tick, track, order key), the same order pairing and playback use.
    std::stable_sort(rows_.begin(), rows_.end(), [&](const Row& a, const Row& b) {
        const auto& left = sequence.tracks[a.track].events[a.event];
        const auto& right = sequence.tracks[b.track].events[b.event];
        return std::tie(left.tick, a.track, left.order_key) < std::tie(right.tick, b.track, right.order_key);
    });
    row_by_event_.clear();
    row_by_event_.reserve(rows_.size());
    for (std::size_t row = 0; row < rows_.size(); ++row)
        row_by_event_.emplace(sequence.tracks[rows_[row].track].events[rows_[row].event].id, static_cast<int>(row));
    endResetModel();
}

int EventTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int EventTableModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : column_count; }

const Event* EventTableModel::event_at(int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size())
        return nullptr;
    const auto& entry = rows_[static_cast<std::size_t>(row)];
    const auto& sequence = controller_.active_sequence();
    if (entry.track >= sequence.tracks.size() || entry.event >= sequence.tracks[entry.track].events.size())
        return nullptr;
    return &sequence.tracks[entry.track].events[entry.event];
}

TrackId EventTableModel::track_at(int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size())
        return {};
    return controller_.active_sequence().tracks[rows_[static_cast<std::size_t>(row)].track].id;
}

int EventTableModel::row_of(EventId id) const {
    const auto found = row_by_event_.find(id);
    return found == row_by_event_.end() ? -1 : found->second;
}

QVariant EventTableModel::data(const QModelIndex& index, int role) const {
    const Event* event = event_at(index.row());
    if (!event)
        return {};
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
        case column_tick: return QVariant::fromValue<qlonglong>(event->tick);
        case column_delta: {
            const Event* previous = event_at(index.row() - 1);
            return QVariant::fromValue<qlonglong>(previous ? event->tick - previous->tick : event->tick);
        }
        case column_position: return QString::fromStdString(controller_.meter_map().format(event->tick));
        case column_track: {
            const auto& entry = rows_[static_cast<std::size_t>(index.row())];
            const auto& name = controller_.active_sequence().tracks[entry.track].name;
            return QString("%1 %2").arg(entry.track + 1).arg(QString::fromStdString(name));
        }
        case column_channel: return event->is_channel() ? QVariant(event->channel() + 1) : QVariant(); // display 1-16
        case column_type: return event_type_name(*event);
        case column_data: return describe_event(*event);
        case column_raw: {
            const auto bytes = event->payload.bytes();
            const auto shown = bytes.first(std::min<std::size_t>(bytes.size(), 24));
            QString text = QString::fromStdString(to_hex(shown));
            for (int position = static_cast<int>(text.size()) - 2; position > 0; position -= 2)
                text.insert(position, ' ');
            if (bytes.size() > 24)
                text += QStringLiteral(" …");
            return QString("%1 | %2").arg(event->code, 2, 16, QLatin1Char('0')).arg(text);
        }
        case column_warning: {
            std::string reason;
            if (!event_is_well_formed(*event, &reason))
                return QString::fromStdString(reason);
            if (event->flags & event_flag::synthetic_closure)
                return tr("synthetic note end");
            return {};
        }
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole && index.column() == column_warning)
        return data(index, Qt::DisplayRole);
    if (role == Qt::TextAlignmentRole && (index.column() == column_tick || index.column() == column_delta || index.column() == column_channel))
        return int(Qt::AlignRight | Qt::AlignVCenter);
    return {};
}

QVariant EventTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QAbstractTableModel::headerData(section, orientation, role);
    switch (section) {
    case column_tick: return tr("Tick");
    case column_delta: return tr("Delta");
    case column_position: return tr("Position");
    case column_track: return tr("Track");
    case column_channel: return tr("Ch");
    case column_type: return tr("Type");
    case column_data: return tr("Data");
    case column_raw: return tr("Raw");
    case column_warning: return tr("Warnings");
    default: return {};
    }
}

Qt::ItemFlags EventTableModel::flags(const QModelIndex& index) const {
    auto result = QAbstractTableModel::flags(index);
    const Event* event = event_at(index.row());
    if (event && (index.column() == column_tick || (event->is_channel() && index.column() == column_raw)))
        result |= Qt::ItemIsEditable;
    return result;
}

bool EventTableModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    const Event* current = event_at(index.row());
    if (!current || role != Qt::EditRole)
        return false;
    Event updated = *current;
    if (index.column() == column_tick) {
        bool ok_number = false;
        const auto tick_value = value.toLongLong(&ok_number);
        if (!ok_number || tick_value < 0)
            return false;
        updated.tick = tick_value;
    } else if (index.column() == column_raw) {
        // Accept "status | data bytes" or just data bytes in hex; validate before committing.
        QString text = value.toString();
        const auto bar = text.indexOf('|');
        if (bar >= 0) {
            bool status_ok = false;
            const int status = text.left(bar).trimmed().toInt(&status_ok, 16);
            if (!status_ok || status < 0x80 || status > 0xEF)
                return false;
            updated.code = static_cast<std::uint8_t>(status);
            text = text.mid(bar + 1);
        }
        std::vector<std::uint8_t> bytes;
        for (const auto& part : text.split(' ', Qt::SkipEmptyParts)) {
            bool byte_ok = false;
            const int byte = part.toInt(&byte_ok, 16);
            if (!byte_ok || byte < 0 || byte > 0x7F)
                return false;
            bytes.push_back(static_cast<std::uint8_t>(byte));
        }
        updated.payload = Payload(bytes);
        std::string reason;
        if (!event_is_well_formed(updated, &reason)) {
            emit controller_.edit_rejected(tr("Raw edit rejected: %1").arg(QString::fromStdString(reason)));
            return false;
        }
    } else {
        return false;
    }
    ChangeBuilder builder(controller_.document(), "Edit event");
    builder.replace(track_at(index.row()), current->id, updated);
    // Raw note edits rebuild pairing on the next generation; the event list shows the new links.
    return controller_.apply(builder.finish());
}

EventListPanel::EventListPanel(DocumentController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    model_ = new EventTableModel(controller, this);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterKeyColumn(-1);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText(tr("Filter events (type, data, track)…"));
    filter_->setClearButtonEnabled(true);
    filter_->setAccessibleName(tr("Event filter"));
    table_ = new QTableView(this);
    table_->setModel(proxy_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->verticalHeader()->setDefaultSectionSize(20);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setAccessibleName(tr("Event list"));
    table_->setSortingEnabled(false);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->addWidget(filter_);
    layout->addWidget(table_, 1);
    connect(filter_, &QLineEdit::textChanged, proxy_, &QSortFilterProxyModel::setFilterFixedString);
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &EventListPanel::sync_to_selection);
    connect(&controller_, &DocumentController::selection_changed, this, &EventListPanel::sync_from_selection);
}

void EventListPanel::sync_to_selection() {
    if (syncing_)
        return;
    std::set<EventId> notes;
    const auto active = controller_.active_track();
    for (const auto& index : table_->selectionModel()->selectedRows()) {
        const int row = proxy_->mapToSource(index).row();
        const Event* event = model_->event_at(row);
        if (event && event->is_note_start() && model_->track_at(row) == active)
            notes.insert(event->id);
    }
    syncing_ = true;
    controller_.set_selection(std::move(notes));
    syncing_ = false;
}

void EventListPanel::sync_from_selection() {
    if (syncing_ || !isVisible())
        return;
    syncing_ = true;
    table_->clearSelection();
    int first = -1;
    std::size_t synced = 0;
    for (const auto& id : controller_.selection()) {
        const int row = model_->row_of(id);
        if (row < 0)
            continue;
        const auto index = proxy_->mapFromSource(model_->index(row, 0));
        table_->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        if (first < 0 || index.row() < first)
            first = index.row();
        if (++synced >= 2000)
            break; // bounded synchronization for huge selections
    }
    if (first >= 0)
        table_->scrollTo(proxy_->index(first, 0));
    syncing_ = false;
}

} // namespace mwb::ui
