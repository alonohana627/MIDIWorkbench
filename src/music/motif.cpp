#include "mwb/music/motif.hpp"

#include <array>
#include <utility>

namespace mwb::music {

namespace {

/// Canonical ratios as (numerator, denominator), ascending; bin index - 5 is the token.
constexpr std::array<std::pair<std::int64_t, std::int64_t>, 11> canonical_ratios{
    {{1, 4}, {1, 3}, {1, 2}, {2, 3}, {3, 4}, {1, 1}, {4, 3}, {3, 2}, {2, 1}, {3, 1}, {4, 1}}};

} // namespace

std::string_view token_family_name(TokenFamily family) noexcept {
    switch (family) {
    case TokenFamily::pitch: return "pitch";
    case TokenFamily::interval: return "interval";
    case TokenFamily::rhythm: return "rhythm";
    case TokenFamily::contour: return "contour";
    case TokenFamily::chord: return "chord";
    }
    return "pitch";
}

int rhythm_ratio_bin(Tick previous_interval, Tick next_interval) noexcept {
    if (previous_interval <= 0 || next_interval <= 0)
        return 0;
    // ratio r = next / previous. r lies above the boundary between canonical ratios a and b
    // (geometric mean sqrt(a*b)) exactly when r^2 > a*b, i.e. next^2 * a.den * b.den >
    // previous^2 * a.num * b.num.
    const __int128 next_squared = static_cast<__int128>(next_interval) * next_interval;
    const __int128 previous_squared = static_cast<__int128>(previous_interval) * previous_interval;
    std::size_t bin = 0;
    for (std::size_t index = 0; index + 1 < canonical_ratios.size(); ++index) {
        const auto [a_num, a_den] = canonical_ratios[index];
        const auto [b_num, b_den] = canonical_ratios[index + 1];
        if (next_squared * a_den * b_den > previous_squared * a_num * b_num)
            bin = index + 1;
        else
            break;
    }
    return static_cast<int>(bin) - 5;
}

int contour_token(int interval, bool size_bins) noexcept {
    const int sign = interval > 0 ? 1 : (interval < 0 ? -1 : 0);
    if (!size_bins || sign == 0)
        return sign;
    const int size = interval * sign;
    const int bin = size <= 2 ? 1 : (size <= 4 ? 2 : 3);
    return sign * bin;
}

MotifTokens make_motif_tokens(const VoiceStream& stream, const MotifTokenOptions& options) {
    MotifTokens tokens;
    const auto& notes = stream.notes;
    for (std::size_t index = 0; index < notes.size(); ++index) {
        tokens.pitch.push_back(notes[index].pitch);
        tokens.onsets.push_back(notes[index].start);
        int mask = notes[index].chord_mask;
        if (options.normalize_chord_root) {
            const int shift = ((notes[index].pitch % 12) + 12) % 12;
            mask = ((mask >> shift) | (mask << (12 - shift))) & 0x0FFF;
        }
        tokens.chord.push_back(mask);
        if (index + 1 < notes.size()) {
            const int interval = notes[index + 1].pitch - notes[index].pitch;
            tokens.interval.push_back(interval);
            tokens.contour.push_back(contour_token(interval, options.contour_size_bins));
        }
        if (index + 2 < notes.size()) {
            const Tick first = notes[index + 1].start - notes[index].start;
            const Tick second = notes[index + 2].start - notes[index + 1].start;
            if (first > 0 && second > 0)
                tokens.rhythm.push_back(rhythm_ratio_bin(first, second));
        }
    }
    return tokens;
}

Result<std::vector<std::pair<VoiceStream, MotifTokens>>> extract_motif_tokens(const Sequence& sequence,
                                                                            VoiceProfile profile,
                                                                            const VoiceExtractionOptions& extraction,
                                                                            const MotifTokenOptions& options) {
    auto streams = extract_voices(sequence, profile, extraction);
    if (!streams)
        return std::unexpected(streams.error());
    std::vector<std::pair<VoiceStream, MotifTokens>> result;
    for (auto& stream : *streams) {
        auto tokens = make_motif_tokens(stream, options);
        result.emplace_back(std::move(stream), std::move(tokens));
    }
    return result;
}

} // namespace mwb::music
