#pragma once

#include "mwb/core/error.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mwb::music {

/// Shipped scale definition (specification section 9.2): a stable identifier, a display name and
/// a 12-bit interval mask relative to the root (bit n set = n semitones above the root).
struct ScaleDefinition {
    std::string_view id;
    std::string_view name;
    std::uint16_t interval_mask = 0;
};

/// The nineteen shipped scales of section 9.2, in the order of the specification table.
/// The identifiers are stable and stored in documents and recipes; never rename them.
[[nodiscard]] std::span<const ScaleDefinition> builtin_scales() noexcept;

/// Looks up a shipped scale by its stable identifier.
[[nodiscard]] std::optional<ScaleDefinition> find_builtin_scale(std::string_view id) noexcept;

/// Builds an interval mask from semitone offsets 0..11 above the root.
[[nodiscard]] Result<std::uint16_t> interval_mask_from_offsets(std::span<const int> offsets);

/// Direction used when a pitch lies exactly between two scale members (section 9.3).
enum class NearestTie : std::uint8_t {
    downward, ///< Default: the lower member wins.
    upward,   ///< User choice: the higher member wins.
};

/// What diatonic transposition does with a source pitch that is not a scale member (section 9.3).
enum class OutOfScalePolicy : std::uint8_t {
    preserve_chromatic_offset, ///< Move the lower neighbouring degree and keep the semitone offset.
    conform_first,             ///< Map to the nearest member (with the tie rule) and then move.
};

/// A pitch-class scale: root pitch class 0..11 (0 = C) plus a 12-bit interval mask relative to
/// the root. The root is always a member. Display-only data (name, identifier) never affects
/// membership. Scales are immutable values.
class Scale {
public:
    /// Chromatic scale on C.
    Scale();

    /// Shipped scale by identifier on the given root.
    [[nodiscard]] static Result<Scale> builtin(std::string_view id, int root_pitch_class);
    /// Custom scale of one to twelve pitch classes; the root bit (bit 0) must be set.
    [[nodiscard]] static Result<Scale> custom(int root_pitch_class, std::uint16_t interval_mask,
                                              std::string name = "Custom");

    [[nodiscard]] int root_pitch_class() const noexcept { return root_; }
    [[nodiscard]] std::uint16_t interval_mask() const noexcept { return mask_; }
    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    /// Number of members per octave.
    [[nodiscard]] int size() const noexcept;
    /// Ordered semitone offsets above the root (degree 0 is always 0).
    [[nodiscard]] std::vector<int> offsets() const;
    /// Membership by the formula of section 9.2 applied to a MIDI pitch (any integer, so values
    /// produced during arithmetic can be tested before range checks).
    [[nodiscard]] bool contains(int midi_pitch) const noexcept;
    /// Mask of absolute pitch classes (bit 0 = C) contained in this scale.
    [[nodiscard]] std::uint16_t absolute_pitch_class_mask() const noexcept;

    /// Nearest scale member to `midi_pitch`; members return themselves. Ties follow `tie`.
    [[nodiscard]] int nearest_member(int midi_pitch, NearestTie tie = NearestTie::downward) const noexcept;

    /// Degree position of a member pitch counted from the root of octave 0 (C-1 region): every
    /// member pitch has a unique ordinal and consecutive members differ by one. Returns nullopt
    /// for non-members.
    [[nodiscard]] std::optional<std::int64_t> member_ordinal(int midi_pitch) const noexcept;
    /// Inverse of member_ordinal.
    [[nodiscard]] int pitch_of_ordinal(std::int64_t ordinal) const noexcept;

    /// Moves a pitch by `degrees` ordered scale degrees across octaves (section 9.3). Out-of-scale
    /// pitches follow `policy`. The result may lie outside 0..127; callers range-check it.
    [[nodiscard]] int transpose_by_degrees(int midi_pitch, int degrees, OutOfScalePolicy policy,
                                           NearestTie tie = NearestTie::downward) const noexcept;

    /// True when the scale has seven members (Roman-numeral harmony may be labeled).
    [[nodiscard]] bool is_heptatonic() const noexcept { return size() == 7; }

    friend bool operator==(const Scale& left, const Scale& right) noexcept {
        return left.root_ == right.root_ && left.mask_ == right.mask_;
    }

private:
    Scale(int root, std::uint16_t mask, std::string id, std::string name);

    int root_ = 0;
    std::uint16_t mask_ = 0x0FFF;
    std::string id_;
    std::string name_;
};

/// A chord built by stacking every other scale degree from one scale degree (section 9.4).
struct DiatonicChord {
    int degree = 0;                      ///< Zero-based scale degree of the chord root.
    std::vector<int> pitch_classes;      ///< Absolute pitch classes from the root upward.
    std::vector<int> intervals;          ///< Semitones above the chord root, ascending.
    std::string quality;                 ///< Recognized quality ("maj", "m", "dim", "7", ...) or "".
    std::string roman_numeral;           ///< Only for heptatonic scales; empty otherwise.
    std::string label;                   ///< Display label such as "ii (Dm)" or "degree 3 stack".
};

/// Diatonic triads (`note_count` 3) or sevenths (`note_count` 4) on every degree of the scale by
/// stacking every other scale degree. Roman numerals are assigned only for heptatonic scales;
/// other scales are labeled "stack every other scale degree".
[[nodiscard]] Result<std::vector<DiatonicChord>> diatonic_chords(const Scale& scale, int note_count);

/// Degree label used by the scale panel: "1".."n" or solfege syllables for heptatonic scales.
[[nodiscard]] std::string degree_label(const Scale& scale, int midi_pitch, bool use_solfege = false);

} // namespace mwb::music
