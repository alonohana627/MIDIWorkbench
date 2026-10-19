#include "mwb/core/notes.hpp"

#include <algorithm>
#include <deque>
#include <tuple>
#include <unordered_map>

namespace mwb {

namespace {

struct StreamItem {
    Tick tick;
    std::uint32_t track;
    std::uint64_t order;
    const Event* event;
    std::uint8_t port;
};

/// Builds the deterministic merged stream (tick, track index, order key) with the effective
/// port of every event (port-prefix meta 0x21 applies to later events of its track).
std::vector<StreamItem> merged_stream(const Sequence& sequence) {
    std::vector<StreamItem> stream;
    std::size_t total = 0;
    for (const auto& track : sequence.tracks)
        total += track.events.size();
    stream.reserve(total);
    for (std::size_t track_index = 0; track_index < sequence.tracks.size(); ++track_index) {
        std::uint8_t port = 0;
        for (const auto& event : sequence.tracks[track_index].events) {
            if (event.is_meta(meta_type::port_prefix) && event.payload.size() == 1)
                port = event.payload[0];
            if (event.is_channel())
                stream.push_back({event.tick, static_cast<std::uint32_t>(track_index), event.order_key, &event, port});
        }
    }
    std::stable_sort(stream.begin(), stream.end(), [](const StreamItem& a, const StreamItem& b) {
        return std::tie(a.tick, a.track, a.order) < std::tie(b.tick, b.track, b.order);
    });
    return stream;
}

std::uint32_t note_key(std::uint8_t port, std::uint8_t channel, std::uint8_t pitch) {
    return (static_cast<std::uint32_t>(port) << 16) | (static_cast<std::uint32_t>(channel) << 8) | pitch;
}

NotePairing pair_stream(const std::vector<StreamItem>& stream, Tick end_tick) {
    NotePairing pairing;
    std::unordered_map<std::uint32_t, std::deque<std::size_t>> open;
    for (const auto& item : stream) {
        const Event& event = *item.event;
        if (event.is_note_start()) {
            NoteSpan span;
            span.on_id = event.id;
            span.on_track = item.track;
            span.off_track = item.track;
            span.start = event.tick;
            span.end = event.tick;
            span.pitch = event.data1();
            span.channel = event.channel();
            span.velocity = event.data2();
            span.port = item.port;
            if (event.flags & event_flag::muted)
                span.flags |= note_flag::muted;
            auto& queue = open[note_key(item.port, span.channel, span.pitch)];
            if (!queue.empty()) {
                span.flags |= note_flag::ambiguous;
                pairing.notes[queue.back()].flags |= note_flag::ambiguous;
            }
            queue.push_back(pairing.notes.size());
            pairing.notes.push_back(span);
        } else if (event.is_note_end()) {
            auto found = open.find(note_key(item.port, event.channel(), event.data1()));
            if (found == open.end() || found->second.empty()) {
                pairing.orphan_ends.push_back(event.id);
                continue;
            }
            NoteSpan& span = pairing.notes[found->second.front()];
            found->second.pop_front();
            span.off_id = event.id;
            span.off_track = item.track;
            span.end = event.tick;
            span.release_velocity = event.message() == ChannelMessage::note_off ? event.data2() : 0;
            if (span.end == span.start)
                span.flags |= note_flag::zero_length;
            if (span.off_track != span.on_track)
                span.flags |= note_flag::cross_track;
        }
    }
    for (auto& [key, queue] : open) {
        for (const auto index : queue) {
            NoteSpan& span = pairing.notes[index];
            span.flags |= note_flag::unterminated;
            span.end = std::max(end_tick, span.start);
        }
    }
    for (const auto& span : pairing.notes)
        if (span.flags & note_flag::ambiguous)
            ++pairing.ambiguous_count;
    return pairing;
}

} // namespace

NotePairing pair_notes(const Sequence& sequence) {
    return pair_stream(merged_stream(sequence), sequence.effective_end());
}

NotePairing pair_track_notes(const Track& track, std::uint32_t track_index, Tick end_tick) {
    std::vector<StreamItem> stream;
    stream.reserve(track.events.size());
    std::uint8_t port = 0;
    for (const auto& event : track.events) {
        if (event.is_meta(meta_type::port_prefix) && event.payload.size() == 1)
            port = event.payload[0];
        if (event.is_channel())
            stream.push_back({event.tick, track_index, event.order_key, &event, port});
    }
    return pair_stream(stream, std::max({end_tick, track.end_tick, track.last_event_tick()}));
}

// --- NoteIndex --------------------------------------------------------------------------------

NoteIndex::NoteIndex(std::vector<NoteSpan> notes) : notes_(std::move(notes)) {
    std::stable_sort(notes_.begin(), notes_.end(),
                     [](const NoteSpan& a, const NoteSpan& b) { return a.start < b.start; });
    const std::size_t chunks = (notes_.size() + chunk_size - 1) / chunk_size;
    chunk_max_end_.assign(chunks, 0);
    for (std::size_t index = 0; index < notes_.size(); ++index) {
        auto& slot = chunk_max_end_[index / chunk_size];
        slot = std::max(slot, notes_[index].end);
    }
    prefix_max_end_.resize(chunks);
    Tick running = 0;
    for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
        running = std::max(running, chunk_max_end_[chunk]);
        prefix_max_end_[chunk] = running;
    }
}

std::vector<std::size_t> NoteIndex::query(TimeRange window, std::uint8_t low_pitch, std::uint8_t high_pitch) const {
    std::vector<std::size_t> result;
    if (notes_.empty())
        return result;
    // Notes starting at or after window.end cannot intersect.
    const auto upper = std::lower_bound(notes_.begin(), notes_.end(), window.end,
                                        [](const NoteSpan& span, Tick value) { return span.start < value; });
    const std::size_t limit = static_cast<std::size_t>(upper - notes_.begin());
    // Skip leading chunks whose running maximum end is before the window: no note there can reach it.
    std::size_t first_chunk = static_cast<std::size_t>(
        std::lower_bound(prefix_max_end_.begin(), prefix_max_end_.end(), window.begin) - prefix_max_end_.begin());
    // prefix_max_end_ is non-decreasing; chunks before first_chunk all end before window.begin.
    for (std::size_t chunk = first_chunk; chunk * chunk_size < limit; ++chunk) {
        if (chunk_max_end_[chunk] <= window.begin && notes_[chunk * chunk_size].start < window.begin)
            continue;
        const std::size_t end = std::min(limit, (chunk + 1) * chunk_size);
        for (std::size_t index = chunk * chunk_size; index < end; ++index) {
            const auto& span = notes_[index];
            if (span.pitch < low_pitch || span.pitch > high_pitch)
                continue;
            const bool intersects = span.end > window.begin || (span.end == span.start && span.start >= window.begin);
            if (intersects)
                result.push_back(index);
        }
    }
    return result;
}

std::optional<std::size_t> NoteIndex::find(EventId on_id) const {
    for (std::size_t index = 0; index < notes_.size(); ++index)
        if (notes_[index].on_id == on_id)
            return index;
    return std::nullopt;
}

} // namespace mwb
