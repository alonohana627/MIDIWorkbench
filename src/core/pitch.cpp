#include "mwb/core/pitch.hpp"

#include <charconv>

namespace mwb {

namespace {
constexpr const char* sharp_names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
constexpr const char* flat_names[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
} // namespace

std::string pitch_class_name(int pitch_class, bool prefer_flats) {
    const int normalized = ((pitch_class % 12) + 12) % 12;
    return prefer_flats ? flat_names[normalized] : sharp_names[normalized];
}

std::string pitch_name(std::uint8_t pitch, int octave_offset, bool prefer_flats) {
    const int octave = pitch / 12 + octave_offset;
    return pitch_class_name(pitch % 12, prefer_flats) + std::to_string(octave);
}

std::optional<int> parse_pitch_class(std::string_view text) {
    if (text.empty())
        return std::nullopt;
    int base = 0;
    switch (text[0]) {
    case 'C': case 'c': base = 0; break;
    case 'D': case 'd': base = 2; break;
    case 'E': case 'e': base = 4; break;
    case 'F': case 'f': base = 5; break;
    case 'G': case 'g': base = 7; break;
    case 'A': case 'a': base = 9; break;
    case 'B': case 'b': base = 11; break;
    default: return std::nullopt;
    }
    int accidental = 0;
    for (std::size_t index = 1; index < text.size(); ++index) {
        if (text[index] == '#')
            ++accidental;
        else if (text[index] == 'b')
            --accidental;
        else
            return std::nullopt;
    }
    return ((base + accidental) % 12 + 12) % 12;
}

std::optional<std::uint8_t> parse_pitch(std::string_view text, int octave_offset) {
    if (text.empty())
        return std::nullopt;
    if (text[0] >= '0' && text[0] <= '9') {
        int value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || value < 0 || value > 127)
            return std::nullopt;
        return static_cast<std::uint8_t>(value);
    }
    std::size_t split = 1;
    while (split < text.size() && (text[split] == '#' || text[split] == 'b'))
        ++split;
    const auto pitch_class = parse_pitch_class(text.substr(0, split));
    if (!pitch_class || split >= text.size())
        return std::nullopt;
    int octave = 0;
    const auto octave_text = text.substr(split);
    const auto [end, error] = std::from_chars(octave_text.data(), octave_text.data() + octave_text.size(), octave);
    if (error != std::errc{} || end != octave_text.data() + octave_text.size())
        return std::nullopt;
    // Accidentals may cross the octave boundary (B#3 == C4, Cb4 == B3).
    int base = 0;
    switch (text[0]) {
    case 'C': case 'c': base = 0; break;
    case 'D': case 'd': base = 2; break;
    case 'E': case 'e': base = 4; break;
    case 'F': case 'f': base = 5; break;
    case 'G': case 'g': base = 7; break;
    case 'A': case 'a': base = 9; break;
    default: base = 11; break;
    }
    int accidental = 0;
    for (std::size_t index = 1; index < split; ++index)
        accidental += text[index] == '#' ? 1 : -1;
    const int value = (octave - octave_offset) * 12 + base + accidental;
    if (value < 0 || value > 127)
        return std::nullopt;
    return static_cast<std::uint8_t>(value);
}

} // namespace mwb
