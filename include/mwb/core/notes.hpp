#pragma once

#include "mwb/core/model.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace mwb {

/// Note span flag bits.
namespace note_flag {
inline constexpr std::uint32_t zero_length = 1u << 0;   ///< Paired note with end == start.
inline constexpr std::uint32_t ambiguous = 1u << 1;     ///< Overlaps another unmatched note of the same key.
inline constexpr std::uint32_t unterminated = 1u << 2;  ///< No matching note end in the sequence.
inline constexpr std::uint32_t cross_track = 1u << 3;   ///< Note end lives in another track.
inline constexpr std::uint32_t muted = 1u << 4;         ///< Note-on carries the muted event flag.
} // namespace note_flag

/// A view of a note-on and its paired note end (section 2 "Note span"). The note's stable ID is
/// the identity of its note-on event.
struct NoteSpan {
    EventId on_id;
    std::optional<EventId> off_id;
    std::uint32_t on_track = 0;    ///< Index of the track owning the note-on (display owner).
    std::uint32_t off_track = 0;   ///< Index of the track owning the note end.
    Tick start = 0;
    Tick end = 0;                  ///< Equals the sequence end for unterminated notes.
    std::uint8_t pitch = 0;
    std::uint8_t channel = 0;
    std::uint8_t velocity = 0;
    std::uint8_t release_velocity = 0;
    std::uint8_t port = 0;
    std::uint32_t flags = 0;

    [[nodiscard]] Tick duration() const { return end - start; }
    [[nodiscard]] EventId id() const { return on_id; }
};

/// Result of deterministic FIFO note pairing (section 16.4).
struct NotePairing {
    std::vector<NoteSpan> notes;            ///< Ordered by (start, merged event order).
    std::vector<EventId> orphan_ends;       ///< Note ends with no open note-on.
    std::size_t ambiguous_count = 0;
};

/// Pairs note ends with the oldest unmatched note-on of the same port, channel and pitch over
/// the merged (tick, track index, order key) stream of the whole sequence. Links may cross tracks.
[[nodiscard]] NotePairing pair_notes(const Sequence& sequence);

/// Pairs notes of a single track only (used for track-local views and clipboard payloads).
[[nodiscard]] NotePairing pair_track_notes(const Track& track, std::uint32_t track_index = 0, Tick end_tick = 0);

/// Interval index over note spans: answers "notes intersecting [begin,end) with pitch in
/// [low,high]" including long notes that start before the window (section 18.3).
class NoteIndex {
public:
    NoteIndex() = default;
    explicit NoteIndex(std::vector<NoteSpan> notes);

    [[nodiscard]] const std::vector<NoteSpan>& notes() const { return notes_; }
    /// Indices into notes() intersecting the window. A zero-length note intersects when its
    /// start lies inside the window.
    [[nodiscard]] std::vector<std::size_t> query(TimeRange window, std::uint8_t low_pitch = 0,
                                                 std::uint8_t high_pitch = 127) const;
    /// Index of the note with the given note-on identity.
    [[nodiscard]] std::optional<std::size_t> find(EventId on_id) const;

private:
    static constexpr std::size_t chunk_size = 256;
    std::vector<NoteSpan> notes_;           ///< Sorted by start.
    std::vector<Tick> chunk_max_end_;       ///< Maximum end per chunk of chunk_size notes.
    std::vector<Tick> prefix_max_end_;      ///< Running maximum end over chunks.
};

} // namespace mwb
