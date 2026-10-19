#pragma once

#include "mwb/playback/channel_state.hpp"
#include "mwb/playback/realtime_event.hpp"
#include "mwb/playback/snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mwb::playback {

/// One note the scheduler has started and therefore must end (section 13.6).
struct OwnedNote {
    std::uint32_t note = no_note;   ///< Index into the current snapshot's notes.
    std::uint32_t route = no_route; ///< Route index in the current snapshot.
    std::uint8_t channel = 0;
    std::uint8_t pitch = 0;
};

/// Fixed-capacity registry of sounding notes owned by one scheduler. Storage is allocated once in
/// the constructor; every other member is real-time safe. Stop, seek, mute, loop and route changes
/// end exactly the notes recorded here, so no note can be left stuck.
class OwnedNoteRegistry {
public:
    explicit OwnedNoteRegistry(std::size_t capacity);

    /// Records a started note; false when the registry is full (the note must not be started).
    [[nodiscard]] bool add(const OwnedNote& note) noexcept;
    /// Removes the entry of `note` and returns it, or nullopt when that note is not owned.
    [[nodiscard]] std::optional<OwnedNote> remove_note(std::uint32_t note) noexcept;
    /// Currently owned notes in insertion-independent order.
    [[nodiscard]] std::span<const OwnedNote> active() const noexcept { return {entries_.data(), size_}; }
    /// Mutable view used for reconciliation after a snapshot swap.
    [[nodiscard]] std::span<OwnedNote> active_mutable() noexcept { return {entries_.data(), size_}; }
    /// Removes the entry at `position` of active().
    void erase_at(std::size_t position) noexcept;
    void clear() noexcept { size_ = 0; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t capacity() const noexcept { return entries_.size(); }

private:
    std::vector<OwnedNote> entries_;
    std::size_t size_ = 0;
};

/// Scheduler configuration fixed at construction (allocation happens only there).
struct SchedulerSettings {
    ChaseMode chase = ChaseMode::state;
    bool metronome = false;               ///< Emit metronome click events.
    bool loop_enabled = true;             ///< Honour the snapshot's loop range.
    std::size_t owned_note_capacity = 2048;
    std::size_t route_capacity = 256;     ///< Must cover PlaybackOptions::max_routes.
};

/// The scheduler's mapping from absolute frames to the snapshot timeline, published by the audio
/// engine so other threads can place input events on the transport clock (recording, section 8.4
/// rule 2). `passes` counts loop wraps since the last start and is not reset when a snapshot swap
/// re-anchors the loop, so loop passes stay distinguishable across edits.
struct SchedulerClock {
    std::int64_t anchor = 0;     ///< Absolute frame of song frame 0 (or of loop iteration 0).
    std::int64_t iteration = 0;  ///< Loop iteration relative to `anchor`.
    std::int64_t passes = 0;     ///< Loop wraps since playback started.
    bool looping = false;
    bool playing = false;
};

/// Linear transport tick heard at `absolute_frame` under `clock`: the snapshot tick, plus
/// `pass * loop length` for loop passes after the first so later passes are not folded onto the
/// first one (the recorder folds them, section 8.4 rule 6). Frames before the snapshot origin (a
/// count-in before tick 0) give negative ticks at the tempo in effect at tick 0.
[[nodiscard]] Tick linear_tick_at(const PlaybackSnapshot& snapshot, const SchedulerClock& clock,
                                  std::int64_t absolute_frame) noexcept;

/// Summary of one processed block.
struct BlockResult {
    std::size_t events = 0;      ///< Events delivered to the sink (including cleanup and chase).
    std::size_t loop_wraps = 0;  ///< Loop boundaries crossed inside the block.
    bool reached_end = false;    ///< Non-looping playback passed the snapshot end.
};

/// Real-time event scheduler (section 13.4). The device frame clock is the master: the scheduler
/// counts absolute frames and, for each audio block [frame_start, frame_end), delivers the events
/// whose absolute frame lies inside the block with exact offsets. An event exactly at the block
/// end belongs to the next block. Loop iterations start at anchor + round(k * exact_length), so
/// the rational remainder is carried and long loops never drift.
///
/// Every member except the constructor is noexcept and allocation free; it is driven from one
/// thread (the audio thread, or the offline renderer). Commands emit their cleanup and chase
/// messages at `offset` within the current block (normally 0, before process_block()).
class Scheduler {
public:
    explicit Scheduler(SchedulerSettings settings = {});

    /// Installs a snapshot at a safe block boundary. While playing, notes whose owner identity no
    /// longer exists (or changed route/pitch) are released through the old routes, surviving
    /// notes are re-indexed, and playback continues at the first unplayed tick. Returns false (and
    /// keeps the old snapshot) when the snapshot has more routes than the configured capacity.
    [[nodiscard]] bool set_snapshot(const PlaybackSnapshot* snapshot, IEventSink& sink) noexcept;
    /// Starts playback at `tick` with controller chase (and held-note chase when configured).
    /// With `count_in_bars`, that many bars of metronome clicks (in the meter and tempo at `tick`)
    /// play first and the song starts after them, so a count-in also works from tick 0 (section
    /// 13.3 CountingIn). Count-in clicks sound whether or not the metronome is enabled.
    void start(Tick tick, IEventSink& sink, std::uint32_t offset = 0, std::uint32_t count_in_bars = 0) noexcept;
    /// Seek: increments the epoch, releases owned notes and sustain, restores channel state and
    /// restarts at `tick` when playing; when stopped it only moves the position.
    void seek(Tick tick, IEventSink& sink, std::uint32_t offset = 0) noexcept;
    /// Stops playback: owned note-offs plus sustain-off, position retained.
    void stop(IEventSink& sink, std::uint32_t offset = 0) noexcept;
    /// Panic: stop cleanup plus all-notes-off, all-sound-off and bend reset on every route used
    /// since the snapshot was installed. Never sends System Reset.
    void panic(IEventSink& sink, std::uint32_t offset = 0) noexcept;
    /// Processes one block of `frame_count` frames and advances the absolute frame counter.
    BlockResult process_block(std::uint32_t frame_count, IEventSink& sink) noexcept;

    [[nodiscard]] bool playing() const noexcept { return playing_; }
    [[nodiscard]] const PlaybackSnapshot* snapshot() const noexcept { return snapshot_; }
    /// Absolute frame of the next block start (frames processed since construction).
    [[nodiscard]] std::int64_t absolute_frame() const noexcept { return absolute_frame_; }
    /// Song frame (snapshot timeline) at the next block start.
    [[nodiscard]] std::int64_t song_frame() const noexcept;
    /// First tick not yet played (the transport position).
    [[nodiscard]] Tick position_tick() const noexcept;
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
    /// True while count-in clicks play before the song starts.
    [[nodiscard]] bool in_count_in() const noexcept { return playing_ && absolute_frame_ < count_in_end_; }
    /// Current frame-to-timeline mapping (see SchedulerClock).
    [[nodiscard]] SchedulerClock clock() const noexcept {
        return SchedulerClock{anchor_, iteration_, passes_, looping_, playing_};
    }
    [[nodiscard]] std::int64_t loop_iteration() const noexcept { return iteration_; }
    [[nodiscard]] bool looping() const noexcept { return looping_; }
    /// Absolute frame where loop iteration `iteration` starts (meaningful while looping).
    [[nodiscard]] std::int64_t loop_iteration_start(std::int64_t iteration) const noexcept;
    [[nodiscard]] const OwnedNoteRegistry& owned_notes() const noexcept { return registry_; }
    [[nodiscard]] std::size_t dropped_note_ons() const noexcept { return dropped_note_ons_; }
    [[nodiscard]] const SchedulerSettings& settings() const noexcept { return settings_; }
    /// Enables or disables metronome clicks (real-time safe).
    void set_metronome(bool enabled) noexcept { settings_.metronome = enabled; }

private:
    void emit_event(IEventSink& sink, const RealtimeMidiEvent& event) noexcept;
    void emit_message(IEventSink& sink, std::uint32_t route, std::uint8_t status, std::uint8_t data1,
                      std::uint8_t data2, std::uint32_t offset) noexcept;
    void dispatch(IEventSink& sink, std::size_t index, std::uint32_t offset) noexcept;
    void release_owned(IEventSink& sink, std::uint32_t offset) noexcept;
    void sustain_off(IEventSink& sink, std::uint32_t offset) noexcept;
    void chase_to(std::size_t event_index, Tick tick, IEventSink& sink, std::uint32_t offset) noexcept;
    void chase_differences(IEventSink& sink) noexcept;
    void setup_count_in(Tick tick, std::uint32_t bars, std::int64_t begin) noexcept;
    [[nodiscard]] std::int64_t count_in_click_frame(std::uint32_t beat) const noexcept;
    void click_seek(Tick tick) noexcept;
    void click_advance() noexcept;
    [[nodiscard]] bool click_pending() const noexcept;
    [[nodiscard]] Tick click_tick() const noexcept;
    [[nodiscard]] std::int64_t click_absolute_frame() const noexcept;
    void emit_click(IEventSink& sink, std::uint32_t accent, std::uint32_t offset) noexcept;
    void anchor_at(Tick tick, std::int64_t absolute) noexcept;
    [[nodiscard]] std::int64_t event_absolute_frame(std::size_t index) const noexcept;
    [[nodiscard]] std::int64_t song_frame_at(std::int64_t absolute) const noexcept;

    SchedulerSettings settings_;
    OwnedNoteRegistry registry_;
    std::vector<ChannelState> live_;     ///< State last sent per route of the current snapshot.
    std::vector<ChannelState> scratch_;  ///< Chase target / remapping workspace.
    std::vector<std::uint8_t> route_used_;
    std::vector<std::uint8_t> scratch_used_;
    std::array<ShortMessage, max_chase_messages> chase_buffer_{};
    std::size_t emitted_ = 0;

    const PlaybackSnapshot* snapshot_ = nullptr;
    bool playing_ = false;
    bool looping_ = false;
    std::int64_t absolute_frame_ = 0;
    std::int64_t anchor_ = 0;          ///< Absolute frame of loop iteration 0 start (or of song frame 0).
    std::int64_t iteration_ = 0;
    std::size_t cursor_ = 0;
    std::size_t iteration_end_ = 0;
    Tick stopped_tick_ = 0;
    std::uint64_t epoch_ = 0;
    std::size_t dropped_note_ons_ = 0;
    std::int64_t passes_ = 0;

    // Lazily generated metronome clicks: the next beat of the snapshot's beat grid.
    std::size_t click_segment_ = 0;
    std::int64_t click_beat_ = 0;
    bool clicks_done_ = true;

    // Count-in: `count_in_beats_` clicks from `count_in_start_`, each beat lasting
    // count_in_beat_numerator_ / count_in_beat_denominator_ frames; the song starts at count_in_end_.
    std::int64_t count_in_start_ = 0;
    std::int64_t count_in_end_ = 0;
    __int128 count_in_beat_numerator_ = 0;
    __int128 count_in_beat_denominator_ = 1;
    std::uint32_t count_in_beats_ = 0;
    std::uint32_t count_in_next_ = 0;
    ClickSegment count_in_grid_;
    Tick count_in_tick_ = 0;
};

} // namespace mwb::playback
