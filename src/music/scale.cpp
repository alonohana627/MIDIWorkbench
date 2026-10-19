#include "mwb/music/scale.hpp"

#include "mwb/core/pitch.hpp"
#include "mwb/music/chord.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <format>

namespace mwb::music {

namespace {

/// Interval mask from semitone offsets (compile time).
constexpr std::uint16_t mask_of(std::initializer_list<int> offsets) {
    std::uint16_t mask = 0;
    for (const int offset : offsets)
        mask = static_cast<std::uint16_t>(mask | (1u << offset));
    return mask;
}

/// The section 9.2 table. Identifiers are stable storage keys.
constexpr std::array<ScaleDefinition, 19> scale_table{{
    {"major", "Major / Ionian", mask_of({0, 2, 4, 5, 7, 9, 11})},
    {"natural_minor", "Natural minor / Aeolian", mask_of({0, 2, 3, 5, 7, 8, 10})},
    {"harmonic_minor", "Harmonic minor", mask_of({0, 2, 3, 5, 7, 8, 11})},
    {"melodic_minor", "Melodic minor ascending / jazz minor", mask_of({0, 2, 3, 5, 7, 9, 11})},
    {"dorian", "Dorian", mask_of({0, 2, 3, 5, 7, 9, 10})},
    {"phrygian", "Phrygian", mask_of({0, 1, 3, 5, 7, 8, 10})},
    {"lydian", "Lydian", mask_of({0, 2, 4, 6, 7, 9, 11})},
    {"mixolydian", "Mixolydian", mask_of({0, 2, 4, 5, 7, 9, 10})},
    {"locrian", "Locrian", mask_of({0, 1, 3, 5, 6, 8, 10})},
    {"phrygian_dominant", "Phrygian dominant", mask_of({0, 1, 4, 5, 7, 8, 10})},
    {"double_harmonic_major", "Double harmonic major", mask_of({0, 1, 4, 5, 7, 8, 11})},
    {"hungarian_minor", "Hungarian minor", mask_of({0, 2, 3, 6, 7, 8, 11})},
    {"major_pentatonic", "Major pentatonic", mask_of({0, 2, 4, 7, 9})},
    {"minor_pentatonic", "Minor pentatonic", mask_of({0, 3, 5, 7, 10})},
    {"blues_hexatonic", "Blues hexatonic", mask_of({0, 3, 5, 6, 7, 10})},
    {"whole_tone", "Whole tone", mask_of({0, 2, 4, 6, 8, 10})},
    {"diminished_half_whole", "Diminished half whole", mask_of({0, 1, 3, 4, 6, 7, 9, 10})},
    {"diminished_whole_half", "Diminished whole half", mask_of({0, 2, 3, 5, 6, 8, 9, 11})},
    {"chromatic", "Chromatic", 0x0FFF},
}};

/// Floor division for a positive divisor.
int floor_divide(int numerator, int divisor) {
    int quotient = numerator / divisor;
    if (numerator % divisor != 0 && numerator < 0)
        --quotient;
    return quotient;
}

/// Floor division for a positive divisor (64-bit).
std::int64_t floor_divide64(std::int64_t numerator, std::int64_t divisor) {
    std::int64_t quotient = numerator / divisor;
    if (numerator % divisor != 0 && numerator < 0)
        --quotient;
    return quotient;
}

/// Mathematical modulo with a non-negative result.
int positive_modulo(int value, int modulus) { return ((value % modulus) + modulus) % modulus; }

constexpr std::array<std::string_view, 7> upper_numerals{"I", "II", "III", "IV", "V", "VI", "VII"};
constexpr std::array<std::string_view, 7> lower_numerals{"i", "ii", "iii", "iv", "v", "vi", "vii"};
constexpr std::array<std::string_view, 7> solfege{"do", "re", "mi", "fa", "sol", "la", "ti"};

/// Roman numeral for a recognized diatonic chord quality (heptatonic scales only).
std::string roman_numeral(int degree, const std::vector<int>& intervals) {
    const auto index = static_cast<std::size_t>(degree);
    const bool has_major_third = std::ranges::find(intervals, 4) != intervals.end();
    const bool has_fifth = std::ranges::find(intervals, 7) != intervals.end();
    const bool has_diminished_fifth = std::ranges::find(intervals, 6) != intervals.end();
    const bool has_augmented_fifth = std::ranges::find(intervals, 8) != intervals.end();
    const bool has_minor_seventh = std::ranges::find(intervals, 10) != intervals.end();
    const bool has_major_seventh = std::ranges::find(intervals, 11) != intervals.end();
    const bool has_diminished_seventh = intervals.size() > 3 && std::ranges::find(intervals, 9) != intervals.end();
    std::string numeral(has_major_third ? upper_numerals[index] : lower_numerals[index]);
    if (has_diminished_fifth && !has_fifth) {
        if (has_minor_seventh)
            return numeral + "ø7";          // half-diminished seventh
        if (has_diminished_seventh)
            return numeral + "°7";          // diminished seventh
        return numeral + "°";
    }
    if (has_augmented_fifth && !has_fifth)
        numeral += "+";
    if (has_major_seventh)
        numeral += "maj7";
    else if (has_minor_seventh)
        numeral += "7";
    return numeral;
}

} // namespace

std::span<const ScaleDefinition> builtin_scales() noexcept { return scale_table; }

std::optional<ScaleDefinition> find_builtin_scale(std::string_view id) noexcept {
    for (const auto& definition : scale_table)
        if (definition.id == id)
            return definition;
    return std::nullopt;
}

Result<std::uint16_t> interval_mask_from_offsets(std::span<const int> offsets) {
    std::uint16_t mask = 0;
    for (const int offset : offsets) {
        if (offset < 0 || offset > 11)
            return make_error(ErrorCode::invalid_argument, "scale offsets must lie in 0..11");
        mask = static_cast<std::uint16_t>(mask | (1u << offset));
    }
    return mask;
}

Scale::Scale() : root_(0), mask_(0x0FFF), id_("chromatic"), name_("Chromatic") {}

Scale::Scale(int root, std::uint16_t mask, std::string id, std::string name)
    : root_(root), mask_(mask), id_(std::move(id)), name_(std::move(name)) {}

Result<Scale> Scale::builtin(std::string_view id, int root_pitch_class) {
    const auto definition = find_builtin_scale(id);
    if (!definition)
        return make_error(ErrorCode::not_found, "unknown scale identifier", std::string(id));
    if (root_pitch_class < 0 || root_pitch_class > 11)
        return make_error(ErrorCode::invalid_argument, "scale root pitch class must lie in 0..11");
    return Scale(root_pitch_class, definition->interval_mask, std::string(definition->id),
                 std::string(definition->name));
}

Result<Scale> Scale::custom(int root_pitch_class, std::uint16_t interval_mask, std::string name) {
    if (root_pitch_class < 0 || root_pitch_class > 11)
        return make_error(ErrorCode::invalid_argument, "scale root pitch class must lie in 0..11");
    if ((interval_mask & ~0x0FFFu) != 0)
        return make_error(ErrorCode::invalid_argument, "scale mask uses only twelve bits");
    if ((interval_mask & 1u) == 0)
        return make_error(ErrorCode::invalid_argument, "a custom scale must contain its root");
    return Scale(root_pitch_class, interval_mask, "custom", std::move(name));
}

int Scale::size() const noexcept { return std::popcount(static_cast<unsigned>(mask_)); }

std::vector<int> Scale::offsets() const {
    std::vector<int> result;
    for (int offset = 0; offset < 12; ++offset)
        if (mask_ & (1u << offset))
            result.push_back(offset);
    return result;
}

bool Scale::contains(int midi_pitch) const noexcept {
    // Section 9.2: relative_pc = ((midi_note % 12) - root_pc + 12) % 12, made safe for negatives.
    const int relative = positive_modulo(positive_modulo(midi_pitch, 12) - root_ + 12, 12);
    return (mask_ & (1u << relative)) != 0;
}

std::uint16_t Scale::absolute_pitch_class_mask() const noexcept {
    std::uint16_t mask = 0;
    for (int pitch_class = 0; pitch_class < 12; ++pitch_class)
        if (contains(pitch_class))
            mask = static_cast<std::uint16_t>(mask | (1u << pitch_class));
    return mask;
}

int Scale::nearest_member(int midi_pitch, NearestTie tie) const noexcept {
    if (contains(midi_pitch))
        return midi_pitch;
    for (int distance = 1; distance <= 12; ++distance) {
        const bool lower = contains(midi_pitch - distance);
        const bool upper = contains(midi_pitch + distance);
        if (lower && upper)
            return tie == NearestTie::upward ? midi_pitch + distance : midi_pitch - distance;
        if (lower)
            return midi_pitch - distance;
        if (upper)
            return midi_pitch + distance;
    }
    return midi_pitch; // Unreachable: the root is always a member.
}

std::optional<std::int64_t> Scale::member_ordinal(int midi_pitch) const noexcept {
    if (!contains(midi_pitch))
        return std::nullopt;
    const int relative = midi_pitch - root_;
    const int octave = floor_divide(relative, 12);
    const int offset = relative - octave * 12;
    int index = 0;
    for (int bit = 0; bit < offset; ++bit)
        if (mask_ & (1u << bit))
            ++index;
    return static_cast<std::int64_t>(octave) * size() + index;
}

int Scale::pitch_of_ordinal(std::int64_t ordinal) const noexcept {
    const int count = size();
    const std::int64_t octave = floor_divide64(ordinal, count);
    const auto index = static_cast<int>(ordinal - octave * count);
    int seen = 0;
    int offset = 0;
    for (int bit = 0; bit < 12; ++bit) {
        if (mask_ & (1u << bit)) {
            if (seen == index) {
                offset = bit;
                break;
            }
            ++seen;
        }
    }
    return root_ + static_cast<int>(octave) * 12 + offset;
}

int Scale::transpose_by_degrees(int midi_pitch, int degrees, OutOfScalePolicy policy, NearestTie tie) const noexcept {
    if (const auto ordinal = member_ordinal(midi_pitch))
        return pitch_of_ordinal(*ordinal + degrees);
    if (policy == OutOfScalePolicy::conform_first) {
        const int member = nearest_member(midi_pitch, tie);
        return pitch_of_ordinal(*member_ordinal(member) + degrees);
    }
    int lower = midi_pitch - 1;
    while (!contains(lower))
        --lower;
    const int chromatic_offset = midi_pitch - lower;
    return pitch_of_ordinal(*member_ordinal(lower) + degrees) + chromatic_offset;
}

Result<std::vector<DiatonicChord>> diatonic_chords(const Scale& scale, int note_count) {
    if (note_count < 2 || note_count > 7)
        return make_error(ErrorCode::invalid_argument, "stacked chords need two to seven tones");
    const int count = scale.size();
    if (count < 3)
        return make_error(ErrorCode::invalid_argument, "stacking every other degree needs at least three members");
    std::vector<DiatonicChord> chords;
    for (int degree = 0; degree < count; ++degree) {
        DiatonicChord chord;
        chord.degree = degree;
        const int root_pitch = scale.pitch_of_ordinal(degree);
        for (int tone = 0; tone < note_count; ++tone) {
            const int pitch = scale.pitch_of_ordinal(degree + 2 * tone);
            chord.pitch_classes.push_back(positive_modulo(pitch, 12));
            chord.intervals.push_back(pitch - root_pitch);
        }
        std::vector<int> reduced;
        for (const int interval : chord.intervals)
            reduced.push_back(interval % 12);
        for (const auto& info : chord_types()) {
            std::vector<int> template_reduced;
            for (const int interval : info.intervals)
                template_reduced.push_back(interval % 12);
            auto sorted_chord = reduced;
            std::ranges::sort(sorted_chord);
            std::ranges::sort(template_reduced);
            if (sorted_chord == template_reduced && info.intervals.size() == chord.intervals.size()) {
                chord.quality = std::string(info.symbol.empty() ? "maj" : info.symbol);
                break;
            }
        }
        const std::string root_name = pitch_class_name(positive_modulo(root_pitch, 12));
        if (scale.is_heptatonic()) {
            chord.roman_numeral = roman_numeral(degree, reduced);
            chord.label = chord.quality.empty()
                              ? std::format("{} ({})", chord.roman_numeral, root_name)
                              : std::format("{} ({}{})", chord.roman_numeral, root_name,
                                            chord.quality == "maj" ? "" : chord.quality);
        } else {
            chord.label = std::format("stack every other scale degree from degree {} ({})", degree + 1, root_name);
        }
        chords.push_back(std::move(chord));
    }
    return chords;
}

std::string degree_label(const Scale& scale, int midi_pitch, bool use_solfege) {
    const auto ordinal = scale.member_ordinal(midi_pitch);
    if (!ordinal)
        return {};
    const auto degree = static_cast<std::size_t>(((*ordinal % scale.size()) + scale.size()) % scale.size());
    if (use_solfege && scale.is_heptatonic())
        return std::string(solfege[degree]);
    return std::to_string(degree + 1);
}

} // namespace mwb::music
