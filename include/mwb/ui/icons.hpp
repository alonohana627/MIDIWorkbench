#pragma once

#include <QIcon>

namespace mwb::ui {

/// Toolbar and panel icons, painted with QPainter in the palette's text color (no SVG module).
enum class IconId {
    new_document,
    open,
    save,
    undo,
    redo,
    select,
    draw,
    erase,
    split,
    audition,
    play,
    stop,
    rewind,
    record,
    step_record,
    loop,
    metronome,
    library,
    inspector,
    add,
    remove,
    duplicate,
    up,
    down,
    close,
    pause,
    more,
};

/// A painted icon. Every pixmap is drawn on request with the palette in effect at that moment, at
/// the requested size and device pixel ratio, so icons follow theme changes (including Windows
/// light/dark switches) and monitor scale changes without being rebuilt. Disabled icons use the
/// disabled text color in both check states; checked and selected icons use the highlighted text.
[[nodiscard]] QIcon make_icon(IconId id);

} // namespace mwb::ui
