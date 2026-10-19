#include "mwb/ui/drag_export.hpp"

#include "mwb/app/document_io.hpp"
#include "mwb/core/ids.hpp"
#include "mwb/ui/document_controller.hpp"

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
        bool old = true;
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
}

void DragHandle::set_controller(DocumentController* controller) { controller_ = controller; }

QSize DragHandle::sizeHint() const {
    return {fontMetrics().horizontalAdvance(tr("⇲ Drag MIDI")) + 24, std::max(26, fontMetrics().height() + 10)};
}

QSize DragHandle::minimumSizeHint() const { return sizeHint(); } // never squeezed to unreadable text

void DragHandle::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setPen(palette().color(QPalette::Text));
    painter.setBrush(palette().color(QPalette::Button));
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 4, 4);
    painter.drawText(rect(), Qt::AlignCenter, tr("⇲ Drag MIDI"));
}

void DragHandle::mousePressEvent(QMouseEvent* event) {
    pressed_ = event->button() == Qt::LeftButton;
    press_ = event->position().toPoint();
}

void DragHandle::mouseMoveEvent(QMouseEvent* event) {
    if (!pressed_ || !controller_ || (event->position().toPoint() - press_).manhattanLength() < QApplication::startDragDistance())
        return;
    pressed_ = false;
    midi::ExportOptions options;
    options.mode = midi::ExportMode::compatibility; // canonical type 1 for DAWs
    options.sequence = controller_->active_sequence_index();
    const auto selected = controller_->selected_notes();
    std::string title = controller_->document().title;
    if (!selected.empty()) {
        Tick begin = selected.front().start;
        Tick end = selected.front().end;
        for (const auto& note : selected) {
            begin = std::min(begin, note.start);
            end = std::max(end, note.end);
        }
        options.range = TimeRange{begin, std::max(end, begin + 1)};
        options.tracks = {controller_->active_track()};
        title += " selection";
    }
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
