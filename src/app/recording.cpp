#include "mwb/app/recording.hpp"

#include "mwb/core/hashing.hpp"
#include "mwb/core/notes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <deque>
#include <map>

namespace mwb::app {

// --- Take chunk codec v1 -----------------------------------------------------------------------

namespace {

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}
void put64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}
std::uint64_t get(std::span<const std::uint8_t> bytes, std::size_t offset, int width) {
    std::uint64_t value = 0;
    for (int index = width - 1; index >= 0; --index)
        value = (value << 8) | bytes[offset + static_cast<std::size_t>(index)];
    return value;
}

constexpr std::size_t header_bytes = 4 + 4 + 4 + 8;

} // namespace

std::vector<std::uint8_t> encode_take_chunk(std::span<const CapturedMessage> messages) {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), {'M', 'W', 'T', 'C'});
    put32(out, 1); // codec version
    put32(out, static_cast<std::uint32_t>(messages.size()));
    const std::int64_t first = messages.empty() ? 0 : messages.front().time_ns;
    put64(out, static_cast<std::uint64_t>(first));
    for (const auto& message : messages) {
        put64(out, static_cast<std::uint64_t>(message.time_ns - first));
        out.push_back(message.port);
        out.push_back(message.size);
        for (std::uint8_t index = 0; index < message.size; ++index)
            out.push_back(message.bytes[index]);
    }
    return out;
}

Result<std::vector<CapturedMessage>> decode_take_chunk(std::span<const std::uint8_t> bytes) {
    const auto invalid = [](const char* what) { return make_error(ErrorCode::invalid_data, std::string("take chunk: ") + what); };
    if (bytes.size() < header_bytes || bytes[0] != 'M' || bytes[1] != 'W' || bytes[2] != 'T' || bytes[3] != 'C')
        return invalid("bad header");
    if (get(bytes, 4, 4) != 1)
        return make_error(ErrorCode::unsupported, "take chunk codec version is not supported");
    const auto count = get(bytes, 8, 4);
    const auto first = static_cast<std::int64_t>(get(bytes, 12, 8));
    if (first < 0 || count > bytes.size())
        return invalid("bad record count or origin");
    std::vector<CapturedMessage> messages;
    messages.reserve(static_cast<std::size_t>(count));
    std::size_t offset = header_bytes;
    std::uint64_t previous = 0;
    for (std::uint64_t index = 0; index < count; ++index) {
        if (offset + 10 > bytes.size())
            return invalid("truncated record");
        const auto relative = get(bytes, offset, 8);
        if (relative < previous || relative > static_cast<std::uint64_t>(INT64_MAX - first))
            return invalid("timestamps are not monotonic");
        previous = relative;
        CapturedMessage message;
        message.time_ns = first + static_cast<std::int64_t>(relative);
        message.port = bytes[offset + 8];
        message.size = bytes[offset + 9];
        offset += 10;
        if (message.size == 0 || message.size > 3 || offset + message.size > bytes.size())
            return invalid("bad message length");
        for (std::uint8_t byte = 0; byte < message.size; ++byte)
            message.bytes[byte] = bytes[offset + byte];
        offset += message.size;
        messages.push_back(message);
    }
    if (offset != bytes.size())
        return invalid("trailing bytes");
    return messages;
}

// --- Real-time take ----------------------------------------------------------------------------

TakeRecorder::TakeRecorder(TrackId track, RecordingOptions options, std::function<Tick(std::int64_t)> time_to_tick)
    : track_(track), options_(std::move(options)), time_to_tick_(std::move(time_to_tick)) {}

void TakeRecorder::add(const CapturedMessage& message) {
    // Mapped now, not at finish(): the transport clock that places the message (section 8.4
    // rule 2) is only valid while its play run is going, and finish() runs after the stop.
    ticks_.push_back(time_to_tick_(message.time_ns - options_.latency_compensation_ns));
    raw_.push_back(message);
}

void TakeRecorder::interrupt(std::string reason) { interruption_ = std::move(reason); }

Result<RecordedTake> TakeRecorder::finish(const DocumentData& document, Tick stop_tick, std::string take_name) {
    const auto location = find_track(document, track_);
    if (!location)
        return make_error(ErrorCode::not_found, "the armed track no longer exists");
    RecordedTake result;
    ChangeBuilder builder(document, "Record take");

    const auto accepted_channel = [&](std::uint8_t channel) {
        return channel < 8 ? ((options_.channel_filter_mask_low >> channel) & 1u) != 0
                           : ((options_.channel_filter_mask_high >> (channel - 8)) & 1u) != 0;
    };
    // Loop takes: map every tick into the loop; remember the pass so the last complete pass is kept.
    const auto fold = [&](Tick tick, std::int64_t& pass) {
        if (!options_.loop || options_.loop->length() <= 0) {
            pass = 0;
            return tick;
        }
        const Tick length = options_.loop->length();
        const Tick offset = tick - options_.loop->begin;
        pass = offset < 0 ? 0 : offset / length;
        return offset < 0 ? tick : options_.loop->begin + offset % length;
    };
    struct OpenNote {
        Tick start = 0;
        std::uint8_t velocity = 0;
        std::int64_t pass = 0;
    };
    std::map<std::pair<int, int>, std::deque<OpenNote>> open;
    struct PendingNote {
        Tick start = 0;
        Tick end = 0;
        std::uint8_t channel = 0;
        std::uint8_t pitch = 0;
        std::uint8_t velocity = 0;
        std::uint8_t release = 0;
        bool synthetic_end = false;
        std::int64_t pass = 0;
    };
    std::vector<PendingNote> notes;
    struct PendingControl {
        Event event;
        std::int64_t pass = 0;
    };
    std::vector<PendingControl> controls;
    const TimeRange window = options_.punch.value_or(TimeRange{options_.start_tick, std::max(stop_tick, options_.start_tick) + 1});
    std::int64_t last_pass = 0;

    const auto close = [&](std::pair<int, int> key, Tick tick, std::uint8_t release, bool synthetic, std::int64_t pass) {
        auto& queue = open[key];
        if (queue.empty())
            return false;
        OpenNote started = queue.front();
        queue.pop_front();
        // A note held across the loop end is split into two take-local spans (section 8.4 rule 6).
        if (options_.loop && pass > started.pass) {
            notes.push_back({started.start, options_.loop->end, static_cast<std::uint8_t>(key.first),
                             static_cast<std::uint8_t>(key.second), started.velocity, 0, true, started.pass});
            // Held through whole passes in between: each of them sounds the note for the full
            // loop, so a composed middle pass keeps it (code-runtime-28).
            for (auto middle = started.pass + 1; middle < pass; ++middle)
                notes.push_back({options_.loop->begin, options_.loop->end, static_cast<std::uint8_t>(key.first),
                                 static_cast<std::uint8_t>(key.second), started.velocity, 0, true, middle});
            started.start = options_.loop->begin;
            started.pass = pass;
            ++result.synthetic_closures;
        }
        Tick start = started.start;
        Tick end = std::max(tick, start);
        // Punch boundaries: notes begun before punch-in start at the boundary only when held-note
        // capture is on; notes running past punch-out end at the boundary.
        if (start < window.begin) {
            if (!options_.capture_held_notes || end <= window.begin)
                return true;
            start = window.begin;
        }
        if (start >= window.end)
            return true;
        if (end > window.end) {
            end = window.end;
            synthetic = true;
        }
        if (end == start)
            end = start + 1; // recorded notes keep a positive length
        notes.push_back({start, end, static_cast<std::uint8_t>(key.first), static_cast<std::uint8_t>(key.second),
                         started.velocity, release, synthetic, started.pass});
        if (synthetic)
            ++result.synthetic_closures;
        return true;
    };

    std::map<std::pair<std::uint8_t, std::uint8_t>, std::uint8_t> carried; // (channel, controller) -> value
    for (std::size_t index = 0; index < raw_.size(); ++index) {
        const CapturedMessage& message = raw_[index];
        if (message.size == 0)
            continue;
        const std::uint8_t status = message.bytes[0];
        if (status < 0x80 || status > 0xEF)
            continue;
        const std::uint8_t channel = status & 0x0F;
        if (!accepted_channel(channel))
            continue;
        std::int64_t pass = 0;
        // Not clamped at 0: a message during a count-in from tick 0 maps before the take start
        // and is handled by the punch-in rules below, never moved onto tick 0.
        const Tick tick = fold(ticks_[index], pass);
        last_pass = std::max(last_pass, pass);
        const std::uint8_t kind = status & 0xF0;
        if (kind == 0x90 || kind == 0x80) {
            const int pitch = message.bytes[1] + options_.transpose;
            if (pitch < 0 || pitch > 127)
                continue;
            const auto key = std::pair{static_cast<int>(channel), pitch};
            if (kind == 0x90 && message.bytes[2] > 0) {
                open[key].push_back({tick, message.bytes[2], pass});
            } else {
                // Physical note-off time is recorded independently of sustain (section 8.4 rule 3).
                std::ignore = close(key, tick, kind == 0x80 ? message.bytes[2] : 0, false, pass); // orphan offs are kept raw only
            }
            continue;
        }
        if (tick < window.begin) {
            // With held-note capture, the controller state of the count-in (a sustain pedal held
            // into the take) is carried to the punch-in like the held notes (code-runtime-27).
            if (options_.capture_held_notes && kind == 0xB0)
                carried[{channel, message.bytes[1]}] = message.bytes[2];
            continue;
        }
        if (tick >= window.end)
            continue;
        Event event;
        event.id = EventId::generate();
        event.tick = tick;
        event.category = EventCategory::channel;
        event.code = status;
        event.payload = channel_data_length(status) == 1 ? Payload{message.bytes[1]} : Payload{message.bytes[1], message.bytes[2]};
        if (kind == 0xA0) {
            // Polyphonic pressure follows its note through the input transpose (code-runtime-29).
            const int pitch = message.bytes[1] + options_.transpose;
            if (pitch < 0 || pitch > 127)
                continue;
            event.payload = Payload{static_cast<std::uint8_t>(pitch), message.bytes[2]};
        }
        controls.push_back({std::move(event), pass});
    }
    if (!carried.empty()) {
        std::int64_t start_pass = 0;
        const Tick start = fold(window.begin, start_pass);
        for (const auto& [slot, value] : carried) {
            Event event;
            event.id = EventId::generate();
            event.tick = start;
            event.category = EventCategory::channel;
            event.code = static_cast<std::uint8_t>(0xB0 | slot.first);
            event.payload = Payload{slot.second, value};
            controls.push_back({std::move(event), start_pass});
        }
    }
    // Still-held notes close at the stop position, labeled synthetic (section 8.4 rule 7).
    std::int64_t stop_pass = 0;
    const Tick stop_folded = fold(stop_tick, stop_pass);
    for (auto& [key, queue] : open)
        while (!queue.empty())
            std::ignore = close(key, std::max(stop_folded, queue.front().start), 0, true, std::max(stop_pass, queue.front().pass)); // held notes

    // Loop takes keep the last complete pass (the one before the pass in progress at stop) in the
    // composed result; a stop during the first pass keeps that pass. Every pass stays in the raw
    // take evidence (section 5.4).
    // A punch-in while looping starts in a later pass: never compose a pass the take did not see.
    std::int64_t first_pass = options_.first_pass.value_or(0);
    if (options_.loop && !options_.first_pass && !ticks_.empty())
        std::ignore = fold(ticks_.front(), first_pass);
    const std::int64_t kept_pass =
        options_.loop ? std::max(first_pass, std::max<std::int64_t>(0, stop_pass - 1)) : 0;
    if (options_.loop && last_pass > 0)
        result.messages.push_back("loop take: " + std::to_string(last_pass + 1) + " passes captured; pass " +
                                  std::to_string(kept_pass + 1) +
                                  " (the last complete one) is composed, all passes stay in the raw take");

    if (options_.replace_range && options_.punch) {
        // Replace-range removes existing events only inside the punch interval, after acceptance.
        const auto& track = document.sequences[location->sequence].tracks[location->track];
        const auto pairing = pair_track_notes(track);
        for (const auto& note : pairing.notes) {
            if (options_.punch->contains(note.start)) {
                builder.remove(track_, note.on_id);
                if (note.off_id)
                    builder.remove(track_, *note.off_id);
            }
        }
        for (const auto& event : track.events)
            if (event.is_channel() && !event.is_note_start() && !event.is_note_end() && options_.punch->contains(event.tick))
                builder.remove(track_, event.id);
    }
    for (const auto& note : notes) {
        if (options_.loop && note.pass != kept_pass)
            continue;
        builder.insert(track_, events::note_on(note.start, note.channel, note.pitch, note.velocity));
        Event off = events::note_off(note.end, note.channel, note.pitch, note.release);
        if (note.synthetic_end)
            off.flags |= event_flag::synthetic_closure;
        builder.insert(track_, std::move(off), OrderPlacement::before_note_starts);
        ++result.notes;
    }
    for (auto& control : controls) {
        if (options_.loop && control.pass != kept_pass)
            continue;
        builder.insert(track_, std::move(control.event), OrderPlacement::before_note_starts);
        ++result.controllers;
    }

    // Raw evidence in bounded chunks (section 21.2), with capture metadata (capture_json v1).
    RecordingTake& take = result.take;
    take.id = take_id_; // the identity capture chunks were journaled under
    take.track = track_;
    take.name = std::move(take_name);
    take.created_ms = now_utc_ms();
    // A take never starts before tick 0 (a stop during a count-in from bar 1 maps before it).
    take.start_tick = std::max<Tick>(0, options_.start_tick);
    take.end_tick = std::max(take.start_tick, stop_tick);
    take.status = interruption_ ? TakeStatus::interrupted : TakeStatus::accepted;
    for (std::size_t begin = 0, ordinal = 0; begin < raw_.size(); begin += take_chunk_messages, ++ordinal) {
        const auto count = std::min(take_chunk_messages, raw_.size() - begin);
        const std::span<const CapturedMessage> slice(raw_.data() + begin, count);
        take.chunks.push_back(TakeChunk{static_cast<std::int32_t>(ordinal), std::max<std::int64_t>(0, slice.front().time_ns),
                                        encode_take_chunk(slice)});
    }
    result.lost_events = lost_;
    result.complete = lost_ == 0 && !interruption_;
    nlohmann::json capture = {
        {"version", 1},
        {"input", raw_.empty() ? "" : "port 0"},
        {"timestamp_quality", raw_.empty() ? "arrival_estimated" : raw_.front().quality},
        {"clock_origin_ns", raw_.empty() ? 0 : raw_.front().time_ns},
        {"latency_compensation_ns", options_.latency_compensation_ns},
        {"raw_division", "nanoseconds"},
        {"lost_events", lost_},
        {"boundary_policy", options_.loop ? "split_at_loop_end" : "close_at_stop"},
        {"complete", result.complete},
    };
    if (interruption_)
        capture["interruption"] = *interruption_;
    take.capture_json = capture.dump();
    if (!result.complete)
        result.messages.push_back(lost_ > 0 ? "input overflow: " + std::to_string(lost_) + " event(s) lost; the take is incomplete"
                                            : "recording was interrupted: " + interruption_.value_or(""));
    builder.append(TakeInsert{document.takes.size(), take});
    result.changes = builder.finish();
    return result;
}

// --- Step recording ----------------------------------------------------------------------------

StepRecorder::StepRecorder(TrackId track, Tick cursor, Settings settings)
    : track_(track), cursor_(std::max<Tick>(0, cursor)), settings_(settings) {}

bool StepRecorder::set_cursor(Tick cursor) {
    cursor = std::max<Tick>(0, cursor);
    if (!held_.empty() || cursor == cursor_)
        return false;
    cursor_ = cursor;
    return true;
}

void StepRecorder::key_down(std::uint8_t pitch, std::uint8_t velocity) {
    if (held_.contains(pitch))
        return; // auto-repeat or pressure never re-adds a held key
    held_.insert(pitch);
    chord_.emplace_back(pitch, settings_.use_incoming_velocity ? std::max<std::uint8_t>(1, velocity) : settings_.fixed_velocity);
}

std::optional<ChangeSet> StepRecorder::key_up(const DocumentData& document, std::uint8_t pitch) {
    if (!held_.erase(pitch) || !held_.empty() || chord_.empty())
        return std::nullopt;
    ChangeBuilder builder(document, chord_.size() == 1 ? "Step note" : "Step chord");
    Step step;
    step.start = cursor_;
    step.length = settings_.note_length;
    for (const auto& [chord_pitch, velocity] : chord_)
        step.notes.push_back(builder.add_note(track_, cursor_, cursor_ + std::max<Tick>(1, settings_.note_length),
                                              settings_.channel, chord_pitch, velocity));
    chord_.clear();
    cursor_ += settings_.advance;
    steps_.push_back(std::move(step));
    return builder.finish();
}

void StepRecorder::rest() {
    cursor_ += settings_.advance;
    steps_.push_back(Step{cursor_ - settings_.advance, {}, 0});
}

std::optional<ChangeSet> StepRecorder::tie(const DocumentData& document) {
    if (steps_.empty() || steps_.back().notes.empty())
        return std::nullopt;
    auto& step = steps_.back();
    const auto location = find_track(document, track_);
    if (!location)
        return std::nullopt;
    const auto& track = document.sequences[location->sequence].tracks[location->track];
    const auto pairing = pair_track_notes(track);
    ChangeBuilder builder(document, "Tie step");
    for (const auto& id : step.notes) {
        for (const auto& note : pairing.notes) {
            if (note.on_id != id || !note.off_id)
                continue;
            const auto position = find_event(track, *note.off_id);
            if (!position)
                continue;
            Event off = track.events[*position];
            off.tick += settings_.advance;
            builder.replace(track_, off.id, off, OrderPlacement::before_note_starts);
        }
    }
    step.length += settings_.advance;
    cursor_ += settings_.advance;
    return builder.finish();
}

std::optional<ChangeSet> StepRecorder::back(const DocumentData& document) {
    if (steps_.empty())
        return std::nullopt;
    const Step step = steps_.back();
    steps_.pop_back();
    cursor_ = step.start;
    if (step.notes.empty())
        return std::nullopt;
    const auto location = find_track(document, track_);
    if (!location)
        return std::nullopt;
    const auto& track = document.sequences[location->sequence].tracks[location->track];
    const auto pairing = pair_track_notes(track);
    ChangeBuilder builder(document, "Remove last step");
    for (const auto& id : step.notes)
        for (const auto& note : pairing.notes)
            if (note.on_id == id) {
                builder.remove(track_, note.on_id);
                if (note.off_id)
                    builder.remove(track_, *note.off_id);
            }
    return builder.finish();
}

} // namespace mwb::app
