#pragma once

#include "mwb/core/change_set.hpp"
#include "mwb/core/error.hpp"
#include "mwb/core/notes.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

/// Direct note edits made by the editor (insert, move, resize, delete, duplicate). Each call
/// returns one ChangeSet (one undo entry); musical transformations live in the music module.
namespace mwb {

/// New values for one existing note, identified by its note-on event.
struct NoteUpdate {
    EventId note;
    Tick start = 0;
    Tick end = 0;
    std::uint8_t pitch = 60;
    std::uint8_t velocity = 100;
    std::uint8_t channel = 0;
    std::uint8_t release_velocity = 0;
};

/// A note to create on a track.
struct NewNote {
    TrackId track;
    Tick start = 0;
    Tick end = 1;
    std::uint8_t pitch = 60;
    std::uint8_t velocity = 100;
    std::uint8_t channel = 0;
    std::uint8_t release_velocity = 0;
};

/// Inserts notes; newly authored notes need positive length (section 16.4).
[[nodiscard]] Result<ChangeSet> insert_notes(const DocumentData& document, std::span<const NewNote> notes,
                                             std::string label = "Insert notes");

/// Applies new start/end/pitch/velocity/channel to existing notes. Both the note-on and its
/// paired note end are rewritten; links stay intact. Rejects negative times and pitches outside
/// 0..127 instead of clamping (section 7.3).
[[nodiscard]] Result<ChangeSet> update_notes(const DocumentData& document, SequenceId sequence,
                                             std::span<const NoteUpdate> updates, std::string label = "Edit notes");

/// Deletes notes (their note-on and paired note end).
[[nodiscard]] Result<ChangeSet> delete_notes(const DocumentData& document, SequenceId sequence,
                                             std::span<const EventId> notes, std::string label = "Delete notes");

/// Copies notes by `offset` ticks and `transpose` semitones onto `destination` (or their own track).
/// Returns the change set and the identities of the created notes.
/// Splits one note at `at` (start < at < end) into two notes as one change set: the original
/// note ends at `at` and a new note with the same pitch, velocity and channel covers [at, end).
/// Returns the new note's identity in `created`.
struct SplitNote {
    ChangeSet changes;
    EventId created;
};
[[nodiscard]] Result<SplitNote> split_note(const DocumentData& document, SequenceId sequence, EventId note, Tick at,
                                           std::string label = "Split note");

struct DuplicatedNotes {
    ChangeSet changes;
    std::vector<EventId> created;
};
[[nodiscard]] Result<DuplicatedNotes> duplicate_notes(const DocumentData& document, SequenceId sequence,
                                                      std::span<const EventId> notes, Tick offset, int transpose = 0,
                                                      std::optional<TrackId> destination = std::nullopt,
                                                      std::string label = "Duplicate notes");

/// Finds a note span by its identity in a pairing result.
[[nodiscard]] const NoteSpan* find_note(const NotePairing& pairing, EventId note);

} // namespace mwb
