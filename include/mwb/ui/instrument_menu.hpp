#pragma once

#include "mwb/core/ids.hpp"
#include "mwb/music/general_midi.hpp"

#include <QString>

#include <cstdint>
#include <functional>
#include <optional>

class QMenu;

namespace mwb::ui {

/// Item data role marking a table cell that Return, Enter and keyboard Space press like a button
/// (the view's activated() signal), as they toggle a check cell. The window's key routing reads
/// it, so those keys reach the cell instead of a window shortcut.
inline constexpr int pressable_cell_role = 0x0100 + 40; // Qt::UserRole + 40

class DocumentController;

/// Fills `menu` with the General MIDI instruments: one submenu per family (8 instruments each)
/// and "Drums (channel 10)". The current instrument is checked and its family submenu is the
/// menu's active entry, so the keyboard starts next to it. `chosen` receives a program, or
/// std::nullopt for drums. The track list, its context menu and the Inspector share this menu.
void populate_instrument_menu(QMenu& menu, const music::TrackInstrument& current,
                              const std::function<void(std::optional<std::uint8_t>)>& chosen);

/// Gives a track an instrument as one undoable edit (music::set_track_instrument with the
/// built-in synth's timbre for the program, so it sounds different at once). Returns the status
/// line saying what changed (or why nothing did).
QString choose_track_instrument(DocumentController& controller, TrackId track, std::optional<std::uint8_t> program);

} // namespace mwb::ui
