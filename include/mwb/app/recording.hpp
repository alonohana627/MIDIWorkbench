#pragma once

#include "mwb/core/change_set.hpp"
#include "mwb/core/error.hpp"
#include "mwb/core/model.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

/// Real-time and step recording (specification sections 8.4 and 8.5). The logic here is free of
/// Qt and devices: the GUI feeds timestamped messages and receives undoable change sets plus the
/// raw take evidence.
namespace mwb::app {

/// One captured input message with its original timestamp (section 8.4 rule 1).
struct CapturedMessage {
    std::int64_t time_ns = 0;            ///< Capture-clock nanoseconds (monotonic).
    std::uint8_t port = 0;               ///< Input port ordinal within the take.
    std::uint8_t size = 0;
    std::array<std::uint8_t, 3> bytes{};
    std::string quality = "arrival_estimated";
};

/// Take chunk codec v1 (section 23.1): little-endian header "MWTC", version 1, record count,
/// first time; then records of (u64 relative nanoseconds, u8 port, u8 length, bytes). Decoding
/// validates lengths, counts and monotonic times.
[[nodiscard]] std::vector<std::uint8_t> encode_take_chunk(std::span<const CapturedMessage> messages);
[[nodiscard]] Result<std::vector<CapturedMessage>> decode_take_chunk(std::span<const std::uint8_t> bytes);
/// Maximum messages per chunk so a long take is stored in bounded pieces (section 21.2).
inline constexpr std::size_t take_chunk_messages = 4096;

/// Recording options of section 8.4.
struct RecordingOptions {
    Tick start_tick = 0;
    std::optional<TimeRange> punch;        ///< Replace/punch interval; events outside are not kept.
    bool replace_range = false;            ///< Remove existing events inside the punch after acceptance.
    bool capture_held_notes = false;       ///< Notes begun before punch-in start at the boundary.
    std::optional<TimeRange> loop;         ///< Loop takes: notes held across the end are split.
    /// Loop pass (counted by the transport since playback started) in which capture began, for a
    /// punch-in while looping: the composed pass is never earlier. Unset: the pass of the first
    /// captured message.
    std::optional<std::int64_t> first_pass;
    std::int64_t latency_compensation_ns = 0; ///< Measured latency, applied once.
    std::uint8_t channel_filter_mask_low = 0xFF;  ///< Channels 1-8 accepted (bit per channel).
    std::uint8_t channel_filter_mask_high = 0xFF; ///< Channels 9-16 accepted.
    int transpose = 0;                     ///< Input transpose (applied to notes, recorded raw untouched).
};

/// Result of one take: the change set that commits it (one undo entry), the stored take with
/// raw evidence, and a report (section 8.4 rule 9).
struct RecordedTake {
    ChangeSet changes;
    RecordingTake take;
    std::size_t notes = 0;
    std::size_t controllers = 0;
    std::size_t synthetic_closures = 0;   ///< Notes still held at stop or split at a loop end.
    std::size_t lost_events = 0;          ///< Overflow reported by the input; take not complete.
    bool complete = true;
    std::vector<std::string> messages;
};

/// Captures a real-time take. Each message is mapped to a tick by `time_to_tick` (the transport
/// clock and tempo map) when it is added; the raw messages are kept unchanged as evidence. Ticks
/// may be negative (a count-in before tick 0); they are linear, so loop passes after the first
/// lie after the loop end and are folded by RecordingOptions::loop.
class TakeRecorder {
public:
    TakeRecorder(TrackId track, RecordingOptions options, std::function<Tick(std::int64_t)> time_to_tick);

    /// Adds one message (note on/off, CC including sustain, bend, channel and poly pressure).
    void add(const CapturedMessage& message);
    /// Reports input overflow so the take is not labeled complete.
    void report_lost(std::size_t count) { lost_ += count; }
    /// Marks an interruption (device unplugged) at the last known position.
    void interrupt(std::string reason);
    /// Finishes at `stop_tick`: still-held notes close there with synthetic note-offs.
    [[nodiscard]] Result<RecordedTake> finish(const DocumentData& document, Tick stop_tick, std::string take_name);

    [[nodiscard]] std::size_t captured() const { return raw_.size(); }
    /// Raw messages captured so far (for journaling full chunks while recording, section 21.2).
    [[nodiscard]] std::span<const CapturedMessage> raw() const { return raw_; }
    /// Identity the finished take will carry, fixed when recording starts so capture chunks
    /// journaled during the take and the final TakeInsert refer to the same take.
    [[nodiscard]] const TakeId& take_id() const { return take_id_; }

private:
    TrackId track_;
    TakeId take_id_ = TakeId::generate();
    RecordingOptions options_;
    std::function<Tick(std::int64_t)> time_to_tick_;
    std::vector<CapturedMessage> raw_;
    std::vector<Tick> ticks_; ///< Transport tick of each raw_ message (latency compensated).
    std::size_t lost_ = 0;
    std::optional<std::string> interruption_;
};

/// Step recording (section 8.5): the edit cursor is the insertion point; a chord is captured from
/// the first note-on until every physically held key is released; rest, tie and back are explicit
/// steps; each committed step is one undo entry.
class StepRecorder {
public:
    struct Settings {
        Tick note_length = 240;
        Tick advance = 240;
        bool use_incoming_velocity = true;
        std::uint8_t fixed_velocity = 100;
        std::uint8_t channel = 0;
    };

    StepRecorder(TrackId track, Tick cursor, Settings settings);

    /// A key went down. Returns nothing; the chord commits when all keys are up.
    void key_down(std::uint8_t pitch, std::uint8_t velocity);
    /// A key went up; when it was the last held key, returns the committed step's change set
    /// (inserted notes) and advances the cursor. Auto-repeat or pressure never advances.
    [[nodiscard]] std::optional<ChangeSet> key_up(const DocumentData& document, std::uint8_t pitch);
    /// Advances without inserting.
    void rest();
    /// Extends the previous step's notes by one advance; returns the change set.
    [[nodiscard]] std::optional<ChangeSet> tie(const DocumentData& document);
    /// Removes the previous step's notes and moves the cursor back; returns the change set.
    [[nodiscard]] std::optional<ChangeSet> back(const DocumentData& document);

    [[nodiscard]] Tick cursor() const { return cursor_; }
    /// Moves the insertion point (the user moved the edit cursor, section 8.5). Ignored while a
    /// chord is being held, so a step always lands where it began; returns whether it moved.
    bool set_cursor(Tick cursor);
    [[nodiscard]] const std::set<std::uint8_t>& held() const { return held_; }

private:
    struct Step {
        Tick start = 0;
        std::vector<EventId> notes;
        Tick length = 0;
    };
    TrackId track_;
    Tick cursor_ = 0;
    Settings settings_;
    std::set<std::uint8_t> held_;
    std::vector<std::pair<std::uint8_t, std::uint8_t>> chord_; ///< (pitch, velocity) in press order.
    std::vector<Step> steps_;
};

} // namespace mwb::app
