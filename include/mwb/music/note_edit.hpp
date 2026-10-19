#pragma once

#include "mwb/core/change_set.hpp"
#include "mwb/core/error.hpp"
#include "mwb/core/model.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/core/time.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mwb::music {

/// What a transformation operates on (specification section 10): one sequence, a track subset,
/// an optional explicit note selection and an optional time range. Selection never depends on
/// view state; it is plain data so it can be stored in a recipe's targets_json.
struct TargetSelection {
    SequenceId sequence;
    std::vector<TrackId> tracks;     ///< Empty = every track of the sequence (note owner = note-on track).
    std::vector<EventId> notes;      ///< Selected note IDs (note-on identities); empty = every note.
    std::optional<TimeRange> range;  ///< When set, only notes whose start lies in [begin, end).
    friend bool operator==(const TargetSelection&, const TargetSelection&) = default;
};

/// Counts reported by every transformation preview (sections 10 and 10.2: clipped or skipped
/// operations appear in the preview report).
struct TransformReport {
    std::string tool;                     ///< Stable tool identifier, e.g. "quantize".
    std::size_t targeted = 0;             ///< Notes (or events) the operation looked at.
    std::size_t changed = 0;              ///< Existing notes/events whose content changed.
    std::size_t created = 0;              ///< New notes/events.
    std::size_t removed = 0;              ///< Notes/events removed.
    std::size_t clipped = 0;              ///< Values limited by a stated rule (velocity 1..127, one-tick floor).
    std::size_t skipped = 0;              ///< Targets left unchanged by a stated rule (out of range, unterminated).
    std::size_t collisions = 0;           ///< New same-pitch overlaps or equal onsets on one track/channel.
    std::size_t ambiguous = 0;            ///< Cases left for the user to decide.
    Tick max_movement = 0;                ///< Largest absolute onset displacement in ticks.
    Rational max_rounding_error{0, 1};    ///< Largest |exact - rounded| tick error.
    std::vector<std::string> messages;    ///< Human-readable findings for the preview.
};

/// A transformation preview: the complete undoable change set plus its report. Applying is
/// apply_change_set (one undo entry); cancelling is discarding the preview.
struct TransformPreview {
    ChangeSet changes;
    TransformReport report;
    std::vector<EventId> created_notes;   ///< Note-on identities of created notes, in creation order.
};

/// Values of one note as a transformation sees them.
struct NoteValues {
    Tick start = 0;
    Tick end = 0;
    int pitch = 60;
    int velocity = 100;
    int release_velocity = 0;
    std::uint8_t channel = 0;
    friend bool operator==(const NoteValues&, const NoteValues&) = default;
};

/// Pure editing session over an immutable document snapshot. It pairs the notes of one
/// sequence, lets a transformation update, remove or add notes and other events by identity,
/// and produces one ChangeSet. Events whose time or pitch changed are re-ordered within their
/// tick so that note ends precede note starts (section 16.6); untouched tick groups keep their
/// order keys. The document must outlive the editor.
class NoteEditor {
public:
    /// Opens the sequence named by `sequence`.
    [[nodiscard]] static Result<NoteEditor> open(const DocumentData& document, SequenceId sequence);

    [[nodiscard]] const DocumentData& document() const noexcept { return *document_; }
    [[nodiscard]] const Sequence& sequence() const noexcept { return document_->sequences[sequence_index_]; }
    [[nodiscard]] std::int32_t ppq() const noexcept;
    /// Notes of the original sequence (pair_notes order).
    [[nodiscard]] const std::vector<NoteSpan>& notes() const noexcept { return pairing_.notes; }

    /// Indices into notes() matching the selection, ordered by (start, track, pitch, note order).
    /// Fails when the selection names unknown tracks or notes.
    [[nodiscard]] Result<std::vector<std::size_t>> select(const TargetSelection& selection) const;

    /// Current values of a note (after earlier updates in this session).
    [[nodiscard]] NoteValues values(std::size_t note) const;
    /// Original values of a note.
    [[nodiscard]] NoteValues original_values(std::size_t note) const;
    /// True for notes without a paired end; their end cannot be changed.
    [[nodiscard]] bool is_unterminated(std::size_t note) const;
    /// True when the note ends with a note-off message, which can carry a release velocity.
    [[nodiscard]] bool has_release_velocity(std::size_t note) const;

    /// Updates a note's events. The end of an unterminated note is ignored. Release velocity is
    /// stored only when the note ends with a note-off message (velocity-zero note-ons cannot hold it).
    void update(std::size_t note, const NoteValues& values);
    /// Removes both events of a note.
    void remove(std::size_t note);
    /// Adds a new note to a track of the sequence (index into sequence().tracks). Returns its ID.
    EventId add(std::size_t track_index, const NoteValues& values);
    /// Changes the tick of a non-note event (time stretch, PPQ change).
    void retime_event(std::size_t track_index, std::size_t event_index, Tick tick);
    /// When enabled, events of a merged tick group keep their original chronological order
    /// (original tick, then original order key) instead of the note-end-first rule. Used by
    /// whole-sequence conversions such as a PPQ change.
    void set_chronological_order(bool enabled) noexcept { chronological_ = enabled; }
    /// Records new track properties (events ignored) to be emitted by finish().
    void set_track_header(std::size_t track_index, Track header);
    /// Records new sequence properties (tracks ignored) to be emitted by finish().
    void set_sequence_header(Sequence header);

    /// Number of same-track, same-channel, same-pitch adjacent overlaps (or equal onsets) after
    /// the edits that did not exist before, restricted to keys touched by this session.
    [[nodiscard]] std::size_t new_collisions() const;

    /// Produces the change set. The editor must not be used afterwards.
    [[nodiscard]] ChangeSet finish(std::string label);

private:
    /// Copy of an event with its edit state and original ordering.
    struct WorkingEvent {
        Event event;
        bool changed = false;
        bool removed = false;
        Tick original_tick = 0;
        std::uint64_t original_order = 0;
        std::uint64_t creation = 0;   ///< Creation order for new events (0 for existing).
    };
    /// Position of an original event in the sequence.
    struct EventLocation {
        std::size_t track = 0;
        std::size_t index = 0;
    };

    NoteEditor() = default;
    /// Working copy of a track's events, created on first touch (indices match the original).
    std::vector<WorkingEvent>& working(std::size_t track_index);
    /// Working copy of an original event, or nullptr for unknown identities.
    WorkingEvent* working_event(EventId id);
    /// Current state of an original event without creating a working copy.
    [[nodiscard]] const Event* current_event(EventId id) const;

    const DocumentData* document_ = nullptr;
    std::size_t sequence_index_ = 0;
    NotePairing pairing_;
    std::unordered_map<EventId, EventLocation, UuidHash> locations_;
    std::unordered_map<std::size_t, std::vector<WorkingEvent>> working_;
    std::unordered_map<EventId, std::size_t, UuidHash> note_index_;
    std::vector<std::pair<std::size_t, NoteValues>> added_notes_;
    std::vector<bool> note_touched_;
    std::vector<bool> note_removed_;
    std::uint64_t creation_counter_ = 0;
    bool chronological_ = false;
    std::vector<std::pair<std::size_t, Track>> track_headers_;
    std::optional<Sequence> sequence_header_;
};

/// Serializes a selection to the recipe targets_json form (version 1).
[[nodiscard]] std::string selection_to_json(const TargetSelection& selection);
/// Parses a selection written by selection_to_json.
[[nodiscard]] Result<TargetSelection> selection_from_json(std::string_view json);

} // namespace mwb::music

namespace mwb::music {

/// Default onset-group tolerance: 1/96 quarter note at the sequence PPQ, rounded half away from
/// zero (section 26.2); 0 for SMPTE sequences, which must state an absolute tolerance instead.
[[nodiscard]] Tick default_onset_tolerance(std::int32_t ppq) noexcept;

/// Groups selected notes into onset groups per owning track: notes on the same track whose
/// starts lie within `tolerance` ticks of the group's first start form one group. Groups are
/// ordered by (first start, track); members keep the order of `notes`.
[[nodiscard]] std::vector<std::vector<std::size_t>> onset_groups(const NoteEditor& editor,
                                                                 const std::vector<std::size_t>& notes,
                                                                 Tick tolerance);

/// Finishes an editor into a preview: counts new collisions into the report and builds the
/// change set with the given label.
[[nodiscard]] TransformPreview finish_preview(NoteEditor& editor, TransformReport report, std::string label);

} // namespace mwb::music

namespace mwb::music {

/// Common start of every selection-based transformation: the opened editor, the selected note
/// indices (ordered by start, track, pitch) and a report pre-filled with the tool name and the
/// targeted count.
struct PreparedTransform {
    NoteEditor editor;
    std::vector<std::size_t> selected;
    TransformReport report;
};

/// Opens the editor and resolves the selection. `require_ppq` rejects SMPTE sequences for tools
/// that need a musical grid (section 17.4: convert explicitly first).
[[nodiscard]] Result<PreparedTransform> prepare_transform(const DocumentData& document,
                                                          const TargetSelection& selection, std::string_view tool,
                                                          bool require_ppq);

} // namespace mwb::music
