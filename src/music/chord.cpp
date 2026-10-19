#include "mwb/music/chord.hpp"

#include "mwb/core/checked.hpp"
#include "mwb/core/pitch.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <tuple>

namespace mwb::music {

namespace {

constexpr std::array<int, 3> major_intervals{0, 4, 7};
constexpr std::array<int, 3> minor_intervals{0, 3, 7};
constexpr std::array<int, 3> diminished_intervals{0, 3, 6};
constexpr std::array<int, 3> augmented_intervals{0, 4, 8};
constexpr std::array<int, 3> sus2_intervals{0, 2, 7};
constexpr std::array<int, 3> sus4_intervals{0, 5, 7};
constexpr std::array<int, 2> power_intervals{0, 7};
constexpr std::array<int, 4> major6_intervals{0, 4, 7, 9};
constexpr std::array<int, 4> minor6_intervals{0, 3, 7, 9};
constexpr std::array<int, 4> dominant7_intervals{0, 4, 7, 10};
constexpr std::array<int, 4> major7_intervals{0, 4, 7, 11};
constexpr std::array<int, 4> minor7_intervals{0, 3, 7, 10};
constexpr std::array<int, 4> minor_major7_intervals{0, 3, 7, 11};
constexpr std::array<int, 4> half_diminished7_intervals{0, 3, 6, 10};
constexpr std::array<int, 4> diminished7_intervals{0, 3, 6, 9};
constexpr std::array<int, 4> add9_intervals{0, 4, 7, 14};
constexpr std::array<int, 4> minor_add9_intervals{0, 3, 7, 14};

constexpr std::array<ChordTypeInfo, 17> type_table{{
    {ChordType::major, "major", "", "Major", major_intervals},
    {ChordType::minor, "minor", "m", "Minor", minor_intervals},
    {ChordType::diminished, "diminished", "dim", "Diminished", diminished_intervals},
    {ChordType::augmented, "augmented", "aug", "Augmented", augmented_intervals},
    {ChordType::suspended_second, "sus2", "sus2", "Suspended second", sus2_intervals},
    {ChordType::suspended_fourth, "sus4", "sus4", "Suspended fourth", sus4_intervals},
    {ChordType::power_fifth, "power_fifth", "5", "Power fifth", power_intervals},
    {ChordType::major_sixth, "major_sixth", "6", "Major sixth", major6_intervals},
    {ChordType::minor_sixth, "minor_sixth", "m6", "Minor sixth", minor6_intervals},
    {ChordType::dominant_seventh, "dominant_seventh", "7", "Dominant seventh", dominant7_intervals},
    {ChordType::major_seventh, "major_seventh", "maj7", "Major seventh", major7_intervals},
    {ChordType::minor_seventh, "minor_seventh", "m7", "Minor seventh", minor7_intervals},
    {ChordType::minor_major_seventh, "minor_major_seventh", "m(maj7)", "Minor major seventh", minor_major7_intervals},
    {ChordType::half_diminished_seventh, "half_diminished_seventh", "m7b5", "Half-diminished seventh",
     half_diminished7_intervals},
    {ChordType::diminished_seventh, "diminished_seventh", "dim7", "Diminished seventh", diminished7_intervals},
    {ChordType::added_ninth, "add9", "add9", "Added ninth", add9_intervals},
    {ChordType::minor_added_ninth, "minor_add9", "m(add9)", "Minor added ninth", minor_add9_intervals},
}};

/// Mathematical modulo with a non-negative result.
int positive_modulo(int value, int modulus) { return ((value % modulus) + modulus) % modulus; }

/// Absolute pitch-class mask of intervals above a root.
std::uint16_t reduced_mask(const std::vector<int>& intervals, int root) {
    std::uint16_t mask = 0;
    for (const int interval : intervals)
        mask = static_cast<std::uint16_t>(mask | (1u << positive_modulo(root + interval, 12)));
    return mask;
}

/// Seventh chords whose highest extension replaces the 7 in the symbol.
bool is_extensible_seventh(ChordType type) {
    return type == ChordType::dominant_seventh || type == ChordType::major_seventh ||
           type == ChordType::minor_seventh || type == ChordType::minor_major_seventh;
}

/// Removal priority when a voicing must have fewer tones: fifth first, then extensions from the
/// top, then the sixth, then remaining non-root tones from the top. Lower value = removed first.
int removal_rank(int interval) {
    if (interval == 7)
        return 0;
    if (interval > 12)
        return 1;
    if (interval == 9)
        return 2;
    if (interval == 0)
        return 9;
    if (interval == 3 || interval == 4)
        return 4;
    return 3;
}

/// "/X" slash suffix when the bass differs from the root.
std::string bass_suffix(std::optional<int> bass_pitch_class, int root_pitch_class) {
    if (!bass_pitch_class || *bass_pitch_class == root_pitch_class)
        return {};
    return "/" + pitch_class_name(*bass_pitch_class);
}

} // namespace

std::span<const ChordTypeInfo> chord_types() noexcept { return type_table; }

const ChordTypeInfo& chord_type_info(ChordType type) noexcept {
    return type_table[static_cast<std::size_t>(type)];
}

std::optional<ChordType> chord_type_from_id(std::string_view id) noexcept {
    for (const auto& info : type_table)
        if (info.id == id)
            return info.type;
    return std::nullopt;
}

std::vector<int> chord_intervals(ChordType type, std::uint8_t extensions) {
    const auto& info = chord_type_info(type);
    std::vector<int> intervals(info.intervals.begin(), info.intervals.end());
    if (extensions & chord_extension::ninth)
        intervals.push_back(14);
    if (extensions & chord_extension::eleventh)
        intervals.push_back(17);
    if (extensions & chord_extension::thirteenth)
        intervals.push_back(21);
    std::ranges::sort(intervals);
    const auto [first, last] = std::ranges::unique(intervals);
    intervals.erase(first, last);
    return intervals;
}

std::string chord_symbol_suffix(ChordType type, std::uint8_t extensions) {
    const auto& info = chord_type_info(type);
    std::string base(info.symbol);
    if (extensions == 0)
        return base;
    const std::string highest = (extensions & chord_extension::thirteenth) ? "13"
                                : (extensions & chord_extension::eleventh) ? "11"
                                                                          : "9";
    if (is_extensible_seventh(type)) {
        // The highest extension replaces the 7 in the symbol: C9, Cmaj13, Cm11, Cm(maj9).
        std::string symbol;
        switch (type) {
        case ChordType::dominant_seventh: symbol = highest; break;
        case ChordType::major_seventh: symbol = "maj" + highest; break;
        case ChordType::minor_seventh: symbol = "m" + highest; break;
        case ChordType::minor_major_seventh: symbol = "m(maj" + highest + ")"; break;
        case ChordType::major:
        case ChordType::minor:
        case ChordType::diminished:
        case ChordType::augmented:
        case ChordType::suspended_second:
        case ChordType::suspended_fourth:
        case ChordType::power_fifth:
        case ChordType::major_sixth:
        case ChordType::minor_sixth:
        case ChordType::half_diminished_seventh:
        case ChordType::diminished_seventh:
        case ChordType::added_ninth:
        case ChordType::minor_added_ninth: break;
        }
        // Lower extensions that are skipped are named explicitly ("13" implies 9 only when present).
        if ((extensions & chord_extension::thirteenth) && !(extensions & chord_extension::ninth))
            symbol += "(no9)";
        if ((extensions & chord_extension::thirteenth) && (extensions & chord_extension::eleventh))
            symbol += "(add11)";
        if ((extensions & chord_extension::eleventh) && !(extensions & chord_extension::ninth) &&
            !(extensions & chord_extension::thirteenth))
            symbol += "(no9)";
        return symbol;
    }
    std::string added;
    if (extensions & chord_extension::ninth)
        added += "add9";
    if (extensions & chord_extension::eleventh)
        added += added.empty() ? "add11" : ",add11";
    if (extensions & chord_extension::thirteenth)
        added += added.empty() ? "add13" : ",add13";
    return base + "(" + added + ")";
}

std::uint16_t pitch_class_mask(std::span<const int> pitches) noexcept {
    std::uint16_t mask = 0;
    for (const int pitch : pitches)
        mask = static_cast<std::uint16_t>(mask | (1u << positive_modulo(pitch, 12)));
    return mask;
}

Result<ChordVoicing> voice_chord(const ChordSpec& spec) {
    if (spec.root_pitch < -12 || spec.root_pitch > 139)
        return make_error(ErrorCode::invalid_argument, "chord root pitch is far outside the MIDI range");
    if (spec.low_limit > spec.high_limit)
        return make_error(ErrorCode::invalid_argument, "chord register floor lies above its ceiling");
    if (spec.bass_pitch_class && (*spec.bass_pitch_class < 0 || *spec.bass_pitch_class > 11))
        return make_error(ErrorCode::invalid_argument, "bass pitch class must lie in 0..11");
    if (spec.note_count < 0 || spec.note_count > 12)
        return make_error(ErrorCode::invalid_argument, "chord note count must lie in 0..12");
    std::vector<int> intervals = chord_intervals(spec.type, spec.extensions);
    const std::vector<int> natural = intervals;
    const int target_count = spec.note_count == 0 ? static_cast<int>(intervals.size()) : spec.note_count;
    while (static_cast<int>(intervals.size()) > target_count) {
        // Remove the lowest-ranked tone; among equal ranks remove the highest.
        std::size_t victim = 0;
        for (std::size_t index = 0; index < intervals.size(); ++index) {
            const int rank = removal_rank(intervals[index]);
            const int best = removal_rank(intervals[victim]);
            if (rank < best || (rank == best && intervals[index] > intervals[victim]))
                victim = index;
        }
        intervals.erase(intervals.begin() + static_cast<std::ptrdiff_t>(victim));
    }
    for (std::size_t doubling = 0; static_cast<int>(intervals.size()) < target_count; ++doubling) {
        const std::size_t source = doubling % natural.size();
        const int octave = 1 + static_cast<int>(doubling / natural.size());
        intervals.push_back(natural[source] + 12 * octave);
    }
    const int distinct_tones = static_cast<int>(intervals.size());
    if (spec.inversion < 0 || spec.inversion >= distinct_tones)
        return make_error(ErrorCode::invalid_argument, "chord inversion must be smaller than the tone count");

    std::vector<int> pitches;
    for (const int interval : intervals)
        pitches.push_back(spec.root_pitch + interval);
    std::ranges::sort(pitches);
    for (int step = 0; step < spec.inversion; ++step) {
        int moved = pitches.front() + 12;
        pitches.erase(pitches.begin());
        while (std::ranges::find(pitches, moved) != pitches.end())
            moved += 12;
        pitches.push_back(moved);
        std::ranges::sort(pitches);
    }
    switch (spec.spread) {
    case ChordSpread::close: break;
    case ChordSpread::drop_two:
        if (pitches.size() >= 2)
            pitches[pitches.size() - 2] -= 12;
        break;
    case ChordSpread::drop_three:
        if (pitches.size() >= 3)
            pitches[pitches.size() - 3] -= 12;
        break;
    case ChordSpread::open:
        for (std::size_t index = 1; index < pitches.size(); index += 2)
            pitches[index] += 12;
        break;
    }
    std::ranges::sort(pitches);
    const int root_pitch_class = positive_modulo(spec.root_pitch, 12);
    std::optional<int> bass;
    if (spec.bass_pitch_class) {
        const int lowest = pitches.front();
        if (positive_modulo(lowest, 12) != *spec.bass_pitch_class) {
            int candidate = lowest - 1;
            while (positive_modulo(candidate, 12) != *spec.bass_pitch_class)
                --candidate;
            pitches.insert(pitches.begin(), candidate);
        }
        bass = *spec.bass_pitch_class;
    } else if (positive_modulo(pitches.front(), 12) != root_pitch_class) {
        bass = positive_modulo(pitches.front(), 12);
    }

    ChordVoicing voicing;
    voicing.pitches = pitches;
    for (const int pitch : pitches)
        if (pitch < 0 || pitch > 127 || pitch < spec.low_limit || pitch > spec.high_limit)
            voicing.out_of_range.push_back(pitch);
    voicing.symbol = pitch_class_name(root_pitch_class) + chord_symbol_suffix(spec.type, spec.extensions) +
                     bass_suffix(bass, root_pitch_class);
    return voicing;
}

Result<std::vector<GeneratedNote>> render_chord(std::span<const int> pitches, const ChordRenderSpec& render) {
    if (render.duration < 1)
        return make_error(ErrorCode::invalid_argument, "chord duration must be at least one tick");
    if (render.start < 0 || render.strum < 0)
        return make_error(ErrorCode::invalid_argument, "chord start and strum must not be negative");
    if (render.velocity < 1 || render.velocity > 127)
        return make_error(ErrorCode::invalid_argument, "chord velocity must lie in 1..127");
    for (const int pitch : pitches)
        if (pitch < 0 || pitch > 127)
            return make_error(ErrorCode::invalid_argument, "chord contains a pitch outside 0..127");
    std::vector<int> ascending(pitches.begin(), pitches.end());
    std::ranges::sort(ascending);
    const auto count = static_cast<std::int64_t>(ascending.size());
    std::vector<GeneratedNote> notes;
    for (std::int64_t index = 0; index < count; ++index) {
        const std::int64_t order = render.direction == StrumDirection::up ? index : count - 1 - index;
        const Tick offset = count > 1 ? mul_div_round(render.strum, order, count - 1).value_or(0) : 0;
        GeneratedNote note;
        note.pitch = ascending[static_cast<std::size_t>(index)];
        note.start = render.start + offset;
        note.end = std::max(render.start + render.duration, note.start + 1);
        const auto ramp = count > 1 ? mul_div_round(render.accent, index, count - 1).value_or(0) : 0;
        std::int64_t velocity = render.velocity;
        switch (render.profile) {
        case VelocityProfile::flat: break;
        case VelocityProfile::accent_bass: velocity += index == 0 ? render.accent : 0; break;
        case VelocityProfile::accent_top: velocity += index == count - 1 ? render.accent : 0; break;
        case VelocityProfile::ramp_up: velocity += ramp; break;
        case VelocityProfile::ramp_down: velocity += render.accent - ramp; break;
        }
        note.velocity = static_cast<int>(std::clamp<std::int64_t>(velocity, 1, 127));
        notes.push_back(note);
    }
    std::ranges::stable_sort(notes, [](const GeneratedNote& a, const GeneratedNote& b) {
        return std::tie(a.start, a.pitch) < std::tie(b.start, b.pitch);
    });
    return notes;
}

std::vector<ChordCandidate> recognize_chord(std::uint16_t mask, std::optional<int> bass_pitch_class,
                                            const RecognitionOptions& options) {
    struct Template {
        ChordType type;
        std::uint8_t extensions;
        std::vector<int> intervals;
    };
    std::vector<Template> templates;
    for (const auto& info : type_table) {
        templates.push_back({info.type, 0, chord_intervals(info.type, 0)});
        if (options.include_extensions && is_extensible_seventh(info.type)) {
            for (const std::uint8_t extensions :
                 {chord_extension::ninth, static_cast<std::uint8_t>(chord_extension::ninth | chord_extension::eleventh),
                  static_cast<std::uint8_t>(chord_extension::ninth | chord_extension::thirteenth)})
                templates.push_back({info.type, extensions, chord_intervals(info.type, extensions)});
        }
    }
    const int sounding = std::popcount(static_cast<unsigned>(mask & 0x0FFF));
    struct Scored {
        ChordCandidate candidate;
        int score_tenths;
        int template_size;
    };
    std::vector<Scored> scored;
    if (sounding < 2)
        return {};
    for (int root = 0; root < 12; ++root) {
        for (const auto& entry : templates) {
            const std::uint16_t template_mask = reduced_mask(entry.intervals, root);
            const int size = std::popcount(static_cast<unsigned>(template_mask));
            const int matched = std::popcount(static_cast<unsigned>(template_mask & mask));
            const int extra = std::popcount(static_cast<unsigned>(mask & ~template_mask & 0x0FFF));
            const int missing = size - matched;
            if (matched < 2 || matched * 2 < size || missing > options.max_missing || extra > options.max_extra)
                continue;
            ChordCandidate candidate;
            candidate.root_pitch_class = root;
            candidate.type = entry.type;
            candidate.extensions = entry.extensions;
            int score = 20 * matched - 15 * extra - size;
            for (const int interval : entry.intervals) {
                const int pitch_class = positive_modulo(root + interval, 12);
                if (mask & (1u << pitch_class))
                    continue;
                const int reduced = interval % 12;
                if (std::ranges::find(candidate.missing_intervals, reduced) != candidate.missing_intervals.end())
                    continue;
                candidate.missing_intervals.push_back(reduced);
                score -= reduced == 7 ? 5 : (reduced == 0 ? 20 : 15);
            }
            for (int pitch_class = 0; pitch_class < 12; ++pitch_class)
                if ((mask & ~template_mask) & (1u << pitch_class))
                    candidate.extra_pitch_classes.push_back(pitch_class);
            if (bass_pitch_class && *bass_pitch_class == root)
                score += 5;
            if (bass_pitch_class && *bass_pitch_class != root)
                candidate.bass_pitch_class = *bass_pitch_class;
            candidate.score = static_cast<double>(score) / 10.0;
            candidate.symbol = pitch_class_name(root) + chord_symbol_suffix(entry.type, entry.extensions) +
                               bass_suffix(candidate.bass_pitch_class, root);
            scored.push_back({std::move(candidate), score, size});
        }
    }
    std::ranges::stable_sort(scored, [](const Scored& a, const Scored& b) {
        if (a.score_tenths != b.score_tenths)
            return a.score_tenths > b.score_tenths;
        if (a.template_size != b.template_size)
            return a.template_size < b.template_size;
        if (a.candidate.root_pitch_class != b.candidate.root_pitch_class)
            return a.candidate.root_pitch_class < b.candidate.root_pitch_class;
        return std::tie(a.candidate.type, a.candidate.extensions) < std::tie(b.candidate.type, b.candidate.extensions);
    });
    std::vector<ChordCandidate> result;
    for (auto& entry : scored) {
        if (result.size() >= options.max_results)
            break;
        result.push_back(std::move(entry.candidate));
    }
    return result;
}

std::vector<ChordSegment> analyze_chords(std::span<const NoteSpan> notes, Tick onset_tolerance,
                                         ChordSegmentPolicy policy, const RecognitionOptions& options) {
    std::vector<ChordSegment> segments;
    if (notes.empty())
        return segments;
    switch (policy) {
    case ChordSegmentPolicy::sounding_between_onsets: break;
    }
    std::vector<const NoteSpan*> ordered;
    Tick latest_end = 0;
    for (const auto& note : notes) {
        ordered.push_back(&note);
        latest_end = std::max(latest_end, note.end);
    }
    std::ranges::stable_sort(ordered, [](const NoteSpan* a, const NoteSpan* b) { return a->start < b->start; });
    std::vector<std::pair<Tick, Tick>> groups; // first and last onset of each group
    for (const NoteSpan* note : ordered) {
        if (groups.empty() || note->start - groups.back().first > onset_tolerance)
            groups.emplace_back(note->start, note->start);
        else
            groups.back().second = note->start;
    }
    for (std::size_t index = 0; index < groups.size(); ++index) {
        ChordSegment segment;
        segment.start = groups[index].first;
        segment.end = index + 1 < groups.size() ? groups[index + 1].first : latest_end;
        int lowest = 128;
        for (const NoteSpan* note : ordered) {
            if (note->start > groups[index].second)
                break;
            const bool sounding = note->end > segment.start || note->start >= segment.start;
            if (!sounding)
                continue;
            segment.pitch_class_mask = static_cast<std::uint16_t>(segment.pitch_class_mask | (1u << (note->pitch % 12)));
            lowest = std::min(lowest, static_cast<int>(note->pitch));
        }
        if (lowest < 128)
            segment.bass_pitch_class = lowest % 12;
        if (!segments.empty() && segments.back().pitch_class_mask == segment.pitch_class_mask &&
            segments.back().bass_pitch_class == segment.bass_pitch_class) {
            segments.back().end = segment.end;
            continue;
        }
        segments.push_back(std::move(segment));
    }
    for (auto& segment : segments)
        segment.alternatives = recognize_chord(segment.pitch_class_mask, segment.bass_pitch_class, options);
    return segments;
}

} // namespace mwb::music
