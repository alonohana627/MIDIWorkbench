#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/event.hpp"
#include "mwb/core/model.hpp"
#include "mwb/core/time.hpp"
#include "mwb/music/note_edit.hpp"

#include <cstdint>
#include <span>
#include <vector>

/// Cross-resolution operations (specification section 17.5).
namespace mwb::music {

/// Result of converting events between PPQ resolutions.
struct PpqConversion {
    std::vector<Event> events;          ///< Same order and identities as the input; ticks converted.
    Rational max_error{0, 1};           ///< Largest |exact - rounded| in destination ticks.
    std::size_t collisions = 0;         ///< Events whose distinct source tick merged into an occupied destination tick.
    std::size_t collapsed_notes = 0;    ///< Paired notes (same channel and pitch) whose positive length became zero.
};

/// Converts PPQ events for pasting: destination_tick = destination_anchor +
/// round((source_tick - source_anchor) * destination_ppq / source_ppq), every event computed
/// independently from its exact original position, half away from zero. Fails when a result
/// would be negative.
[[nodiscard]] Result<PpqConversion> convert_events_ppq(std::span<const Event> events, std::int32_t source_ppq,
                                                       std::int32_t destination_ppq, Tick source_anchor = 0,
                                                       Tick destination_anchor = 0);

/// Changes a sequence's PPQ as one explicit undoable operation: every event class in every
/// track, track and sequence end ticks, regions and scale regions are converted together.
/// Positive note lengths that would round to zero are kept at one tick (reported as clipped);
/// regions keep at least one tick. Rejects SMPTE sequences (convert with a tempo map instead).
[[nodiscard]] Result<TransformPreview> change_sequence_ppq(const DocumentData& document, SequenceId sequence,
                                                           std::int32_t new_ppq);

} // namespace mwb::music
