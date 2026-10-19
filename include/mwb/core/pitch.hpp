#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mwb {

/// Note name with octave; with the default offset -1, MIDI 60 is "C4" (section 8.1: the
/// displayed octave convention is configurable without changing pitches).
[[nodiscard]] std::string pitch_name(std::uint8_t pitch, int octave_offset = -1, bool prefer_flats = false);
/// Pitch class name such as "C#" or "Db".
[[nodiscard]] std::string pitch_class_name(int pitch_class, bool prefer_flats = false);
/// Parses "C4", "F#3", "Eb5" or "60" into a MIDI pitch.
[[nodiscard]] std::optional<std::uint8_t> parse_pitch(std::string_view text, int octave_offset = -1);
/// Parses "C", "F#" or "Bb" into a pitch class 0..11.
[[nodiscard]] std::optional<int> parse_pitch_class(std::string_view text);

} // namespace mwb
