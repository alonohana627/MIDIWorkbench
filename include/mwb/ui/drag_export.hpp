#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/model.hpp"
#include "mwb/midi/codec.hpp"

#include <QPointer>
#include <QWidget>

#include <filesystem>
#include <span>
#include <string>

namespace mwb::ui {

class DocumentController;

/// Writes `bytes` as a complete, closed .mid file in the drag cache before any drag starts; drop
/// targets may read it asynchronously long after the drag returns (section 14.3). The name keeps
/// the user's title where Windows allows it.
[[nodiscard]] Result<std::filesystem::path> write_drag_file(const std::filesystem::path& cache, const std::string& title,
                                                            std::span<const std::uint8_t> bytes);

/// Removes drag-cache files older than 24 hours (never anything outside the cache directory).
void clean_drag_cache(const std::filesystem::path& cache);

/// Starts a Windows file drag (OLE file drop through Qt) of an existing file.
void start_file_drag(QWidget* source, const std::filesystem::path& file);

/// What "Drag MIDI" exports: the active sequence for a compatibility (canonical type 1) file, or,
/// with notes selected, exactly those notes (and the active track's controller state) in their
/// time span. `title` receives the file title.
[[nodiscard]] midi::ExportOptions drag_export_options(const DocumentController& controller, std::string* title);

/// Toolbar handle that drags the active document, or its selected notes, out as a .mid file to a
/// DAW or Explorer.
class DragHandle : public QWidget {
    Q_OBJECT

public:
    DragHandle(std::filesystem::path cache, QWidget* parent = nullptr);
    void set_controller(DocumentController* controller);
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

signals:
    void status_message(const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    std::filesystem::path cache_;
    QPointer<DocumentController> controller_; ///< Cleared if the document closes first (uiB-39).
    QPoint press_;
    bool pressed_ = false;
};

} // namespace mwb::ui
