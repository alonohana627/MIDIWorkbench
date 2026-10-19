#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/model.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace mwb {

/// Inserts one event into a track (the event carries its order key).
struct EventInsert {
    TrackId track;
    Event event;
};

/// Removes one event; the full copy makes the operation invertible.
struct EventRemove {
    TrackId track;
    Event event;
};

/// Replaces an event in place (same identity), possibly moving it in time.
struct EventReplace {
    TrackId track;
    Event before;
    Event after;
};

/// Inserts a complete track (with events) at a position of a sequence.
struct TrackInsert {
    SequenceId sequence;
    std::size_t index = 0;
    Track track;
};

/// Removes a complete track (with events).
struct TrackRemove {
    SequenceId sequence;
    std::size_t index = 0;
    Track track;
};

/// Changes track properties; `before`/`after` carry no events.
struct TrackPropertiesChange {
    TrackId track;
    Track before;
    Track after;
};

/// Moves a track to another position inside its sequence.
struct TrackMove {
    SequenceId sequence;
    std::size_t from = 0;
    std::size_t to = 0;
};

/// Changes sequence properties (division, scale, settings, regions, recipes); `before`/`after`
/// carry no tracks.
struct SequencePropertiesChange {
    SequenceId sequence;
    Sequence before;
    Sequence after;
};

/// Inserts or removes a whole sequence.
struct SequenceInsert {
    std::size_t index = 0;
    Sequence sequence;
};
struct SequenceRemove {
    std::size_t index = 0;
    Sequence sequence;
};

/// Inserts or removes a recording take with its raw capture evidence (section 8.4).
struct TakeInsert {
    std::size_t index = 0;
    RecordingTake take;
};
struct TakeRemove {
    std::size_t index = 0;
    RecordingTake take;
};

/// Changes document title and metadata.
struct DocumentPropertiesChange {
    std::string title_before;
    std::string title_after;
    std::string metadata_before;
    std::string metadata_after;
};

/// One reversible edit operation.
using EditOperation =
    std::variant<EventInsert, EventRemove, EventReplace, TrackInsert, TrackRemove, TrackPropertiesChange,
                 TrackMove, SequencePropertiesChange, SequenceInsert, SequenceRemove, DocumentPropertiesChange,
                 TakeInsert, TakeRemove>;

/// A complete command delta: validates and commits completely or leaves the document unchanged
/// (invariant 8, section 18.2). One ChangeSet is one undo entry.
struct ChangeSet {
    std::string label;
    std::vector<EditOperation> operations;

    [[nodiscard]] bool empty() const { return operations.empty(); }
    /// Approximate memory held by this change set, used for the undo budget.
    [[nodiscard]] std::size_t memory_estimate() const;
};

/// Applies every operation in order. On failure the operations already applied are reverted and
/// the error names the failing operation.
[[nodiscard]] Status apply_change_set(DocumentData& document, const ChangeSet& changes);

/// Inverse change set: applying it after `changes` restores the previous state exactly.
[[nodiscard]] ChangeSet invert(const ChangeSet& changes);

/// Order key placement for newly inserted events at a tick.
enum class OrderPlacement {
    after_existing,  ///< After every event at the same tick.
    before_existing, ///< Before every event at the same tick.
    before_note_starts, ///< After non-note-start events but before the first sounding note-on.
};

/// Builds change sets from intuitive edits. Touched tracks are copied on first touch, edited
/// freely, and the result is diffed by event identity in finish(). Track end ticks are extended
/// automatically to cover moved or inserted events (invariant 4).
/// A batch of edits to one track, applied by ChangeBuilder::edit_track in one pass.
struct BulkTrackEdit {
    /// Content replacements that keep the event's tick and order position (tick fields ignored).
    std::unordered_map<EventId, Event, UuidHash> replace_in_place;
    /// Events to remove.
    std::unordered_set<EventId, UuidHash> remove;
    /// Events to insert, each with its placement inside its tick. An event that moves to another
    /// tick is removed and inserted again with the same identity.
    std::vector<std::pair<Event, OrderPlacement>> insert;
};

class ChangeBuilder {
public:
    ChangeBuilder(const DocumentData& document, std::string label);

    /// Inserts a new event; assigns its order key. Returns its identity.
    EventId insert(TrackId track, Event event, OrderPlacement placement = OrderPlacement::after_existing);
    /// Removes an event by identity; returns false when it does not exist.
    bool remove(TrackId track, EventId event);
    /// Replaces an existing event's content (tick, code, payload, flags). A tick change places
    /// the event at the end of the destination tick unless `placement` says otherwise.
    bool replace(TrackId track, EventId event, Event updated,
                 OrderPlacement placement = OrderPlacement::after_existing);
    /// Adds a note as a note-on/note-off pair. Note-offs are ordered before note starts at their
    /// tick so a note ending where a same-pitch note begins stays well formed (section 16.6).
    EventId add_note(TrackId track, Tick start, Tick end, std::uint8_t channel, std::uint8_t pitch,
                     std::uint8_t velocity, std::uint8_t release_velocity = 0);
    /// Replaces a track's whole event list (bulk transformations). Events are stably sorted by
    /// (tick, order key); tick groups with duplicate order keys are renumbered in their current
    /// order. Events with a nil identity receive a new one.
    void set_events(TrackId track, std::vector<Event> events);
    /// Applies many replacements, removals and insertions to one track in O((n + k) log k).
    /// Order inside a tick: before_existing insertions (reverse list order), existing events up to
    /// the first note start, before_note_starts insertions, the remaining existing events, then
    /// after_existing insertions (list order). Existing events keep their relative order and keys;
    /// only tick groups that receive insertions can get new order keys. Returns false when the
    /// track does not exist.
    bool edit_track(TrackId track, BulkTrackEdit edit);
    /// Current working copy of a track (after edits made through this builder).
    [[nodiscard]] const Track* working_track(TrackId track);
    /// Records track property changes (events in `updated` are ignored).
    void set_track_properties(TrackId track, const Track& updated);
    /// Records sequence property changes (tracks in `updated` are ignored).
    void set_sequence_properties(SequenceId sequence, const Sequence& updated);
    /// Appends an explicit structural operation.
    void append(EditOperation operation);

    /// Produces the change set; the builder must not be used afterwards.
    [[nodiscard]] ChangeSet finish();

private:
    struct WorkingTrack {
        TrackLocation location;
        Track track;
    };
    WorkingTrack* touch(TrackId track);
    std::uint64_t allocate_order_key(Track& track, Tick tick, OrderPlacement placement,
                                     std::size_t& insert_position);

    const DocumentData& document_;
    std::string label_;
    std::vector<WorkingTrack> working_;
    std::vector<EditOperation> explicit_operations_;
    std::vector<std::pair<SequenceId, Sequence>> sequence_updates_;
    std::vector<std::pair<TrackId, Track>> track_updates_;
};

/// Locates an event in a sorted track by tick/order key hint, verifying identity.
[[nodiscard]] std::optional<std::size_t> locate_event(const Track& track, const Event& hint);
/// Locates an event by identity with a linear scan.
[[nodiscard]] std::optional<std::size_t> find_event(const Track& track, EventId id);

} // namespace mwb
