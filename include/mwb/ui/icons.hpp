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
    loop,
    metronome,
    library,
    inspector,
    add,
    remove,
    duplicate,
    up,
    down,
};

/// Paints the icon at 16, 20, 24 and 32 px for the current palette; rebuild after a theme change.
[[nodiscard]] QIcon make_icon(IconId id);

} // namespace mwb::ui
