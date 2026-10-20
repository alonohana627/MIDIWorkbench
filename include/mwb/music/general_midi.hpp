#pragma once

#include "mwb/core/change_set.hpp"
#include "mwb/core/error.hpp"
#include "mwb/core/ids.hpp"
#include "mwb/core/model.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/// General MIDI Level 1 instrument names and the track instrument edit (choosing what a track
/// plays). Programs are 0-based here, as in the MIDI byte; the UI shows them by name.
namespace mwb::music {

inline constexpr int gm_program_count = 128;
inline constexpr int gm_family_count = 16;
inline constexpr int gm_family_size = 8;
/// MIDI channel 10 (0-based 9): percussion in General MIDI.
inline constexpr std::uint8_t gm_drum_channel = 9;

/// GM instrument name, "Acoustic Grand Piano" .. "Gunshot". The program is masked to 0..127.
[[nodiscard]] std::string_view gm_program_name(std::uint8_t program) noexcept;
/// GM family name ("Piano", "Chromatic Percussion", ... "Sound Effects"); 0..15, else "".
[[nodiscard]] std::string_view gm_family_name(int family) noexcept;
/// Family of a program: eight consecutive programs per family.
[[nodiscard]] constexpr int gm_family_of(std::uint8_t program) noexcept { return (program & 0x7F) / gm_family_size; }
/// Program with exactly this GM name, if any.
[[nodiscard]] std::optional<std::uint8_t> gm_program_named(std::string_view name) noexcept;

/// What a track plays, as the track list shows it.
struct TrackInstrument {
    bool drums = false;           ///< A drum track (channel 10 percussion); `program` is then unused.
    std::uint8_t program = 0;     ///< 0..127.
    bool from_event = false;      ///< Taken from a Program Change before the first note (imported files).
    friend bool operator==(const TrackInstrument&, const TrackInstrument&) = default;
};

/// The effective instrument: drums for a drum track; otherwise the first Program Change on the
/// track before its first note, when there is one (that is what a player hears first), else the
/// track's instrument setting.
[[nodiscard]] TrackInstrument effective_instrument(const Track& track);
/// "Drums" or the GM name of the effective program.
[[nodiscard]] std::string instrument_label(const Track& track);

/// Result of choosing an instrument: the edit and what it had to move.
struct InstrumentEdit {
    ChangeSet changes;                         ///< One undo entry; empty when nothing changes.
    std::optional<std::uint8_t> moved_to;      ///< Channel the track's channel-10 events moved to.
    std::size_t moved_events = 0;              ///< How many channel events moved there.
};

/// Chooses a track's instrument as one undoable edit.
/// - A program (0..127): sets instrument.program and `synth_preset` (the built-in synth's timbre
///   for it, which the caller supplies), clears the drum flag and drops a channel-10 override.
///   Channel 10 is percussion in General MIDI, so the track's events on it move to a free channel
///   (the lowest one no other track of the sequence uses, never 10) in the same edit. Then on every
///   channel the track's notes use (or its channel override) the first Program Change before the
///   first note on that channel is updated, or one is inserted at tick 0, so an exported file
///   carries the instrument. No Program Change is ever written on channel 10.
/// - std::nullopt (drums): sets the drum flag and channel override 10; no Program Change.
/// The change set is empty when the track already has that instrument.
[[nodiscard]] Result<InstrumentEdit> set_track_instrument(const DocumentData& document, TrackId track,
                                                          std::optional<std::uint8_t> program,
                                                          std::string synth_preset);

} // namespace mwb::music
