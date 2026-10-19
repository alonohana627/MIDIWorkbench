#include "mwb/ui/drag_export.hpp"

#include "mwb/app/document_io.hpp"
#include "mwb/core/ids.hpp"
#include "mwb/ui/document_controller.hpp"

#include <cctype>
#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QUrl>

#include <chrono>

namespace mwb::ui {

namespace {

/// Replaces characters Windows forbids in file names; the visible title stays intact elsewhere.
std::string safe_file_stem(const std::string& title) {
    std::string stem;
    for (const char character : title) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 32 || character == '<' || character == '>' || character == ':' || character == '"' ||
            character == '/' || character == '\\' || character == '|' || character == '?' || character == '*')
            stem.push_back('_');
        else
            stem.push_back(character);
    }
    while (!stem.empty() && (stem.back() == '.' || stem.back() == ' '))
        stem.pop_back();
    if (stem.empty())
        stem = "MIDI";
    // Windows reserves CON, PRN, AUX, NUL, COM1-9 and LPT1-9 as names (also with an extension):
    // a document titled so would make the drag file fail (ux-panels-45).
    {
        std::string base = stem.substr(0, stem.find('.'));
        for (auto& character : base)
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        const bool device = base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
                            (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) &&
                             base[3] >= '1' && base[3] <= '9');
        if (device)
            stem.insert(base.size(), "_");
    }
    if (stem.size() > 80) {
        std::size_t length = 80;
        // Never cut inside a UTF-8 sequence (a split character makes an invalid path).
        while (length > 0 && (static_cast<unsigned char>(stem[length]) & 0xC0) == 0x80)
            --length;
        stem.resize(length);
    }
    return stem;
}

} // namespace

Result<std::filesystem::path> write_drag_file(const std::filesystem::path& cache, const std::string& title,
                                              std::span<const std::uint8_t> bytes) {
    std::error_code error;
    std::filesystem::create_directories(cache, error);
    // A unique subdirectory keeps the plain title as the file name the DAW will show.
    const auto directory = cache / Uuid::generate().to_string().substr(0, 8);
    std::filesystem::create_directories(directory, error);
    if (error)
        return make_error(ErrorCode::io, "could not create the drag cache: " + error.message());
    const auto path = directory / app::path_from_utf8(safe_file_stem(title) + ".mid");
    MWB_TRY(app::write_file_atomically(path, bytes, false));
    return path;
}

void clean_drag_cache(const std::filesystem::path& cache) {
    std::error_code error;
    if (!std::filesystem::is_directory(cache, error))
        return;
    const auto limit = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
    for (const auto& entry : std::filesystem::directory_iterator(cache, error)) {
        if (!entry.is_directory(error) || entry.is_symlink(error))
            continue;
        // A folder made just now (a drag being prepared while this runs in the background) is
        // never old, even before its file is written.
        bool old = entry.last_write_time(error) <= limit;
        for (const auto& file : std::filesystem::directory_iterator(entry.path(), error))
            if (file.last_write_time(error) > limit)
                old = false;
        if (old)
            std::filesystem::remove_all(entry.path(), error);
    }
}

void start_file_drag(QWidget* source, const std::filesystem::path& file) {
    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(QString::fromStdWString(file.wstring()))});
    auto* drag = new QDrag(source);
    drag->setMimeData(mime);
    drag->exec(Qt::CopyAction); // the file stays in the cache for later asynchronous reads
}

DragHandle::DragHandle(std::filesystem::path cache, QWidget* parent) : QWidget(parent), cache_(std::move(cache)) {
    setToolTip(tr("Drag the document (or the selected notes) into a DAW or folder as a .mid file"));
    setAccessibleName(tr("Drag MIDI out"));
    setCursor(Qt::OpenHandCursor);
    setAttribute(Qt::WA_Hover); // a hover fill says it can be grabbed (ux-visual-23)
}

void DragHandle::set_controller(DocumentController* controller) { controller_ = controller; }

QSize DragHandle::sizeHint() const {
    return {fontMetrics().horizontalAdvance(tr("⇲ Drag MIDI")) + 24, std::max(26, fontMetrics().height() + 10)};
}

QSize DragHandle::minimumSizeHint() const { return sizeHint(); } // never squeezed to unreadable text

void DragHandle::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    // A quiet border (it is a handle, not the primary button) and a hover fill; the label is the
    // normal button text, readable in every theme (final review N1: the border grey was 1.3:1).
    painter.setPen(palette().color(QPalette::Mid));
    painter.setBrush(palette().color(QPalette::Button));
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 4, 4);
    if (underMouse()) {
        // Hover: a thin accent line under the label instead of a fill that could cost contrast.
        painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
        painter.drawLine(QPoint(8, height() - 4), QPoint(width() - 8, height() - 4));
    }
    painter.setPen(palette().color(QPalette::ButtonText));
    painter.drawText(rect(), Qt::AlignCenter, tr("⇲ Drag MIDI"));
}

midi::ExportOptions drag_export_options(const DocumentController& controller, std::string* title) {
    midi::ExportOptions options;
    options.mode = midi::ExportMode::compatibility; // canonical type 1 for DAWs
    options.sequence = controller.active_sequence_index();
    const auto selected = controller.selected_notes();
    if (title)
        *title = controller.document().title;
    if (!selected.empty()) {
        Tick begin = selected.front().start;
        Tick end = selected.front().end;
        for (const auto& note : selected) {
            begin = std::min(begin, note.start);
            end = std::max(end, note.end);
        }
        options.range = TimeRange{begin, std::max(end, begin + 1)};
        options.tracks = {controller.active_track()};
        // Exactly the selected notes, not every note of the track in that time span (uiB-18).
        options.notes.reserve(selected.size());
        for (const auto& note : selected)
            options.notes.push_back(note.on_id);
        if (title)
            *title += " selection";
    }
    return options;
}

void DragHandle::mousePressEvent(QMouseEvent* event) {
    pressed_ = event->button() == Qt::LeftButton;
    press_ = event->position().toPoint();
}

void DragHandle::mouseReleaseEvent(QMouseEvent* event) {
    // A click without a drag: say how the handle is used (ux-panels-45).
    if (pressed_ && event->button() == Qt::LeftButton)
        emit status_message(tr("Drag this onto a DAW track or a folder; from the keyboard use File > Export MIDI…"));
    pressed_ = false;
}

void DragHandle::mouseMoveEvent(QMouseEvent* event) {
    if (!pressed_ || !controller_ || (event->position().toPoint() - press_).manhattanLength() < QApplication::startDragDistance())
        return;
    pressed_ = false;
    std::string title;
    const midi::ExportOptions options = drag_export_options(*controller_, &title);
    auto bytes = midi::export_midi(controller_->document(), options);
    if (!bytes) {
        emit status_message(QString::fromStdString(bytes.error().describe()));
        return;
    }
    // The file is created and closed before the drag starts (section 14.3).
    auto file = write_drag_file(cache_, title, *bytes);
    if (!file) {
        emit status_message(QString::fromStdString(file.error().describe()));
        return;
    }
    start_file_drag(this, *file);
}

} // namespace mwb::ui
