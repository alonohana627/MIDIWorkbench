#include "mwb/playback/snapshot.hpp"

#include "mwb/core/notes.hpp"
#include "mwb/core/serialization.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <unordered_map>
#include <utility>

namespace mwb::playback {

namespace {

/// Process-wide counter giving every compiled snapshot a unique generation.
std::atomic<std::uint64_t> next_snapshot_generation{1};

/// Rounds a non-negative or negative exact fraction half away from zero.
std::int64_t round_half_away(__int128 numerator, __int128 denominator) noexcept {
    const bool negative = numerator < 0;
    const __int128 magnitude = negative ? -numerator : numerator;
    const __int128 rounded = (magnitude * 2 + denominator) / (denominator * 2);
    return static_cast<std::int64_t>(negative ? -rounded : rounded);
}

/// Stable merged-order key: (tick, track index, order key, sub-order).
struct SortKey {
    Tick tick = 0;
    std::uint32_t track = 0;
    std::uint64_t order = 0;
    std::uint32_t sub = 0;
    friend auto operator<=>(const SortKey&, const SortKey&) = default;
};

/// An event under construction together with its ordering key.
struct PendingEvent {
    SortKey key;
    ScheduledEvent event;
};

/// Identity hash of a route (output kind, endpoint, channel).
std::uint64_t route_key(OutputKind output, const std::string& endpoint, std::uint8_t channel) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(endpoint.size() + 2);
    bytes.push_back(static_cast<std::uint8_t>(output));
    bytes.push_back(channel);
    bytes.insert(bytes.end(), endpoint.begin(), endpoint.end());
    return xxh3_64(bytes);
}

/// Baseline controller state of a new route: the instrument's bank/program for internal and
/// SoundFont routes (GM2 rhythm bank 120 for drum routes); unknown for external hardware.
ChannelState baseline_for(const Route& route) {
    ChannelState state = ChannelState::defaults();
    if (route.output == OutputKind::external_midi)
        return state;
    state.program_known = true;
    state.program = static_cast<std::uint8_t>(route.instrument.program & 0x7Fu);
    state.bank_msb = route.drum ? std::uint8_t{120} : static_cast<std::uint8_t>((route.instrument.bank >> 7) & 0x7Fu);
    state.bank_lsb = static_cast<std::uint8_t>(route.instrument.bank & 0x7Fu);
    state.controllers[0] = state.bank_msb;
    state.controllers[32] = state.bank_lsb;
    return state;
}

/// Applies a transposition, returning nullopt when the pitch leaves 0..127.
std::optional<std::uint8_t> transposed(std::uint8_t pitch, std::int32_t semitones) {
    const std::int32_t value = static_cast<std::int32_t>(pitch) + semitones;
    if (value < 0 || value > 127)
        return std::nullopt;
    return static_cast<std::uint8_t>(value);
}

} // namespace

const char* chase_mode_label(ChaseMode mode) noexcept {
    switch (mode) {
    case ChaseMode::state:
        return "State chase";
    case ChaseMode::state_and_held_notes:
        return "State plus held-note chase (restarted notes lose their original envelope phase)";
    }
    return "State chase";
}

// --- TimeMapping ------------------------------------------------------------------------------

Result<TimeMapping> TimeMapping::make(TempoMap tempo_map, std::uint32_t sample_rate, Rational tempo_multiplier) {
    if (sample_rate == 0 || sample_rate > 768000)
        return make_error(ErrorCode::invalid_argument, "sample rate must be between 1 and 768000 Hz");
    if (tempo_multiplier.numerator <= 0 || tempo_multiplier.denominator <= 0)
        return make_error(ErrorCode::invalid_argument, "tempo multiplier must be positive");
    TimeMapping mapping;
    mapping.sample_rate_ = sample_rate;
    mapping.multiplier_ = tempo_multiplier;
    const Division& division = tempo_map.division();
    if (division.is_ppq()) {
        if (division.ppq <= 0)
            return make_error(ErrorCode::invalid_argument, "PPQ must be positive");
        mapping.denominator_ = static_cast<__int128>(1'000'000) * division.ppq * tempo_multiplier.numerator;
    } else {
        const Rational seconds_per_tick = division.smpte_seconds_per_tick();
        if (seconds_per_tick.numerator <= 0 || seconds_per_tick.denominator <= 0)
            return make_error(ErrorCode::invalid_argument, "invalid SMPTE division");
        mapping.denominator_ = seconds_per_tick.denominator;
    }
    mapping.tempo_map_ = std::move(tempo_map);
    return mapping;
}

TimeMapping::ExactFrame TimeMapping::exact_frame(Tick tick) const noexcept {
    const Division& division = tempo_map_.division();
    if (division.is_ppq()) {
        return ExactFrame{tempo_map_.elapsed_scaled(tick) * sample_rate_ * multiplier_.denominator, denominator_};
    }
    const Rational seconds_per_tick = division.smpte_seconds_per_tick();
    return ExactFrame{static_cast<__int128>(tick) * seconds_per_tick.numerator * sample_rate_, denominator_};
}

std::int64_t TimeMapping::frame_at(Tick tick) const noexcept {
    const ExactFrame exact = exact_frame(tick);
    return round_half_away(exact.numerator, exact.denominator);
}

Tick TimeMapping::first_tick_at_or_after_frame(std::int64_t frame) const noexcept {
    if (frame <= frame_at(0))
        return 0;
    Tick low = 0;   // frame_at(low) < frame
    Tick high = 1;  // grows until frame_at(high) >= frame
    while (frame_at(high) < frame) {
        low = high;
        if (high > (std::numeric_limits<Tick>::max() >> 2))
            return high;
        high *= 2;
    }
    while (high - low > 1) {
        const Tick middle = low + (high - low) / 2;
        if (frame_at(middle) < frame)
            low = middle;
        else
            high = middle;
    }
    return high;
}

TimeMapping::ExactFrame TimeMapping::frames_for_ticks_at(Tick at, Tick ticks) const noexcept {
    const Division& division = tempo_map_.division();
    if (division.is_ppq()) {
        const __int128 tempo = tempo_map_.tempo_at(std::max<Tick>(at, 0));
        return ExactFrame{static_cast<__int128>(ticks) * tempo * sample_rate_ * multiplier_.denominator, denominator_};
    }
    const Rational seconds_per_tick = division.smpte_seconds_per_tick();
    return ExactFrame{static_cast<__int128>(ticks) * seconds_per_tick.numerator * sample_rate_, denominator_};
}

Tick TimeMapping::ticks_for_frames_at(Tick at, std::int64_t frames) const noexcept {
    const ExactFrame one_tick = frames_for_ticks_at(at, 1);
    if (one_tick.numerator <= 0)
        return 0;
    // ticks = frames / (one_tick.numerator / one_tick.denominator), rounded toward zero.
    return static_cast<Tick>(static_cast<__int128>(frames) * one_tick.denominator / one_tick.numerator);
}

// --- PlaybackSnapshot -------------------------------------------------------------------------

std::size_t PlaybackSnapshot::first_event_at_or_after(Tick tick) const noexcept {
    const auto found = std::lower_bound(events.begin(), events.end(), tick,
                                        [](const ScheduledEvent& event, Tick value) { return event.tick < value; });
    return static_cast<std::size_t>(found - events.begin());
}

std::uint32_t PlaybackSnapshot::find_note(EventId owner) const noexcept {
    const auto found = std::lower_bound(notes_by_owner.begin(), notes_by_owner.end(), owner,
                                        [](const auto& entry, const EventId& value) { return entry.first < value; });
    if (found == notes_by_owner.end() || found->first != owner)
        return no_note;
    return found->second;
}

void PlaybackSnapshot::state_before(std::size_t event_index, std::span<ChannelState> states) const noexcept {
    const std::size_t route_count = std::min(states.size(), routes.size());
    event_index = std::min(event_index, events.size());
    const auto found = std::upper_bound(checkpoint_events.begin(), checkpoint_events.end(), event_index);
    const std::size_t checkpoint =
        found == checkpoint_events.begin() ? 0 : static_cast<std::size_t>(found - checkpoint_events.begin() - 1);
    std::size_t start = 0;
    if (checkpoint < checkpoint_events.size() && checkpoint_states.size() >= (checkpoint + 1) * routes.size()) {
        start = checkpoint_events[checkpoint];
        for (std::size_t route = 0; route < route_count; ++route)
            states[route] = checkpoint_states[checkpoint * routes.size() + route];
    } else {
        for (std::size_t route = 0; route < route_count; ++route)
            states[route] = baseline_states[route];
    }
    for (std::size_t index = start; index < event_index; ++index) {
        const ScheduledEvent& event = events[index];
        if (event.kind != RealtimeEventKind::midi || event.role != NoteRole::none || event.route >= route_count)
            continue;
        states[event.route].apply(event.bytes[0], event.bytes[1], event.bytes[2]);
    }
}

const ClickSegment* PlaybackSnapshot::click_segment_at(Tick tick) const noexcept {
    if (click_segments.empty())
        return nullptr;
    const auto after = std::upper_bound(click_segments.begin(), click_segments.end(), tick,
                                        [](Tick value, const ClickSegment& segment) { return value < segment.start; });
    return after == click_segments.begin() ? &click_segments.front() : &*std::prev(after);
}

std::vector<bool> audible_tracks(const Sequence& sequence, bool master_mute) {
    std::vector<bool> audible(sequence.tracks.size(), false);
    if (master_mute)
        return audible;
    const bool any_solo = std::ranges::any_of(sequence.tracks, [](const Track& track) { return track.solo; });
    for (std::size_t index = 0; index < sequence.tracks.size(); ++index) {
        const Track& track = sequence.tracks[index];
        audible[index] = !track.muted && (!any_solo || track.solo);
    }
    return audible;
}

// --- Compilation ------------------------------------------------------------------------------

Result<std::unique_ptr<const PlaybackSnapshot>> compile_playback(const DocumentData& document,
                                                                const PlaybackOptions& options,
                                                                std::stop_token stop) {
    if (document.sequences.empty())
        return make_error(ErrorCode::invalid_argument, "the document has no sequence to play");
    const Sequence* sequence = &document.sequences.front();
    if (!options.sequence.is_nil()) {
        const auto index = document.sequence_index(options.sequence);
        if (!index)
            return make_error(ErrorCode::not_found, "the sequence to play does not exist");
        sequence = &document.sequences[*index];
    }
    if (options.max_routes == 0 || options.checkpoint_interval == 0 || options.preflight_block_frames == 0)
        return make_error(ErrorCode::invalid_argument, "route limit, checkpoint interval and block size must be positive");
    if (options.session.loop && (options.session.loop->begin < 0 || options.session.loop->empty()))
        return make_error(ErrorCode::invalid_argument, "a loop range needs 0 <= begin < end");

    auto mapping = TimeMapping::make(TempoMap::build(*sequence), options.sample_rate, options.session.tempo_multiplier);
    if (!mapping)
        return std::unexpected(std::move(mapping).error());

    auto snapshot = std::make_unique<PlaybackSnapshot>();
    snapshot->document_generation = document.generation;
    snapshot->snapshot_generation = next_snapshot_generation.fetch_add(1, std::memory_order_relaxed);
    snapshot->sequence = sequence->id;
    snapshot->source_semantic_hash = semantic_hash(*sequence);
    snapshot->session = options.session;
    snapshot->mapping = std::move(*mapping);
    SnapshotStatistics& statistics = snapshot->statistics;

    std::vector<bool> audible = audible_tracks(*sequence, options.master_mute);
    if (options.include_muted_tracks && !options.master_mute)
        std::ranges::fill(audible, true);
    statistics.audible_tracks = static_cast<std::size_t>(std::ranges::count(audible, true));

    // Route resolution with a per-track, per-channel cache.
    constexpr std::uint32_t unresolved = no_route;
    std::vector<std::array<std::uint32_t, 16>> route_cache(sequence->tracks.size());
    for (auto& cache : route_cache)
        cache.fill(unresolved);
    std::string route_error;
    auto resolve_route = [&](std::uint32_t track_index, std::uint8_t channel) -> std::uint32_t {
        std::uint32_t& cached = route_cache[track_index][channel & 0x0Fu];
        if (cached != unresolved)
            return cached;
        const Track& track = sequence->tracks[track_index];
        const auto effective = static_cast<std::uint8_t>(track.routing.channel_override.value_or(channel) & 0x0Fu);
        const std::string endpoint = track.routing.output == OutputKind::external_midi
                                         ? track.routing.endpoint_id.value_or(std::string{})
                                         : std::string{};
        const std::uint64_t key = route_key(track.routing.output, endpoint, effective);
        for (std::uint32_t index = 0; index < snapshot->routes.size(); ++index) {
            Route& existing = snapshot->routes[index];
            if (existing.key == key && existing.output == track.routing.output && existing.endpoint == endpoint &&
                existing.channel == effective) {
                if (existing.first_track != track_index && !(existing.instrument == track.instrument))
                    ++statistics.route_conflicts;
                existing.drum = existing.drum || track.is_drum;
                cached = index;
                return index;
            }
        }
        if (snapshot->routes.size() >= options.max_routes) {
            route_error = std::format("more than {} distinct routes", options.max_routes);
            return unresolved;
        }
        Route route;
        route.output = track.routing.output;
        route.endpoint = endpoint;
        route.channel = effective;
        route.drum = track.is_drum || effective == 9;
        route.instrument = track.instrument;
        route.key = key;
        route.first_track = track_index;
        snapshot->routes.push_back(std::move(route));
        cached = static_cast<std::uint32_t>(snapshot->routes.size() - 1);
        return cached;
    };

    std::vector<PendingEvent> pending;
    std::size_t processed = 0;
    auto check_stop = [&]() { return (++processed & 0xFFFu) == 0 && stop.stop_requested(); };

    // Order keys of note starts and ends, needed to place synthesized note-offs in the merged order.
    std::unordered_map<EventId, std::pair<std::uint32_t, std::uint64_t>, UuidHash> end_order;
    for (std::uint32_t track_index = 0; track_index < sequence->tracks.size(); ++track_index)
        for (const Event& event : sequence->tracks[track_index].events)
            if (event.is_note_end() || event.is_note_start())
                end_order.emplace(event.id, std::make_pair(track_index, event.order_key));

    // Notes from deterministic FIFO pairing.
    const NotePairing pairing = pair_notes(*sequence);
    statistics.orphan_note_ends = pairing.orphan_ends.size();
    for (const NoteSpan& span : pairing.notes) {
        if (check_stop())
            return make_error(ErrorCode::cancelled, "snapshot compilation was cancelled");
        if (span.on_track >= sequence->tracks.size() || !audible[span.on_track])
            continue;
        if ((span.flags & note_flag::muted) != 0) {
            ++statistics.muted_notes;
            continue;
        }
        const Track& track = sequence->tracks[span.on_track];
        const std::uint32_t route_index = resolve_route(span.on_track, span.channel);
        if (route_index == unresolved)
            return make_error(ErrorCode::limit_exceeded, "too many playback routes", route_error);
        const Route& route = snapshot->routes[route_index];
        const std::int32_t shift = track.routing.transpose + (route.drum ? 0 : options.session.preview_transpose);
        const auto pitch = transposed(span.pitch, shift);
        if (!pitch) {
            ++statistics.notes_out_of_range;
            continue;
        }
        std::uint64_t on_order = 0;
        if (const auto found = end_order.find(span.on_id); found != end_order.end())
            on_order = found->second.second;

        NoteBoundary note;
        note.owner = span.on_id;
        note.route = route_index;
        note.channel = route.channel;
        note.pitch = *pitch;
        note.velocity = span.velocity;
        note.release_velocity = span.release_velocity;
        note.start_tick = span.start;
        note.end_tick = span.end;
        const auto note_index = static_cast<std::uint32_t>(snapshot->notes.size());
        snapshot->notes.push_back(note);

        PendingEvent on;
        on.key = SortKey{span.start, span.on_track, on_order, 0};
        on.event.tick = span.start;
        on.event.route = route_index;
        on.event.note = note_index;
        on.event.role = NoteRole::note_on;
        on.event.size = 3;
        on.event.bytes = {static_cast<std::uint8_t>(0x90u | route.channel), *pitch, span.velocity};
        pending.push_back(on);

        PendingEvent off;
        SortKey off_key{span.end, std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint64_t>::max(), 0};
        if (span.off_id) {
            if (const auto found = end_order.find(*span.off_id); found != end_order.end())
                off_key = SortKey{span.end, found->second.first, found->second.second, 0};
        }
        off.key = off_key;
        off.event.tick = span.end;
        off.event.route = route_index;
        off.event.note = note_index;
        off.event.role = NoteRole::note_off;
        off.event.size = 3;
        off.event.bytes = {static_cast<std::uint8_t>(0x80u | route.channel), *pitch, span.release_velocity};
        pending.push_back(off);
    }
    statistics.notes = snapshot->notes.size();

    // Controllers, programs, bend, pressure and tempo.
    for (std::uint32_t track_index = 0; track_index < sequence->tracks.size(); ++track_index) {
        const Track& track = sequence->tracks[track_index];
        for (const Event& event : track.events) {
            if (check_stop())
                return make_error(ErrorCode::cancelled, "snapshot compilation was cancelled");
            if (event.is_meta(meta_type::tempo)) {
                if (event.payload.size() != 3)
                    continue;
                PendingEvent tempo;
                tempo.key = SortKey{event.tick, track_index, event.order_key, 0};
                tempo.event.tick = event.tick;
                tempo.event.kind = RealtimeEventKind::tempo;
                tempo.event.value = (static_cast<std::uint32_t>(event.payload[0]) << 16) |
                                    (static_cast<std::uint32_t>(event.payload[1]) << 8) | event.payload[2];
                pending.push_back(tempo);
                continue;
            }
            if (event.category == EventCategory::sysex_f0 || event.category == EventCategory::sysex_f7) {
                if (audible[track_index])
                    ++statistics.skipped_sysex;
                continue;
            }
            if (!audible[track_index] || !event.is_channel())
                continue;
            const ChannelMessage message = event.message();
            if (message == ChannelMessage::note_on || message == ChannelMessage::note_off)
                continue; // Notes come from the pairing above.
            const std::uint32_t route_index = resolve_route(track_index, event.channel());
            if (route_index == unresolved)
                return make_error(ErrorCode::limit_exceeded, "too many playback routes", route_error);
            const Route& route = snapshot->routes[route_index];
            std::uint8_t data1 = event.data1();
            if (message == ChannelMessage::poly_pressure) {
                const std::int32_t shift =
                    track.routing.transpose + (route.drum ? 0 : options.session.preview_transpose);
                const auto pitch = transposed(data1, shift);
                if (!pitch)
                    continue;
                data1 = *pitch;
            }
            PendingEvent pending_event;
            pending_event.key = SortKey{event.tick, track_index, event.order_key, 0};
            pending_event.event.tick = event.tick;
            pending_event.event.route = route_index;
            pending_event.event.size = static_cast<std::uint8_t>(1 + channel_data_length(event.code));
            pending_event.event.bytes = {static_cast<std::uint8_t>((event.code & 0xF0u) | route.channel), data1,
                                         pending_event.event.size == 3 ? event.data2() : std::uint8_t{0}};
            pending.push_back(pending_event);
        }
    }

    Tick end_tick = std::max(sequence->effective_end(), options.minimum_end_tick);
    for (const PendingEvent& event : pending)
        end_tick = std::max(end_tick, event.key.tick);

    // Metronome beat grid from the meter map (PPQ sequences only). Only one entry per meter
    // segment is stored; the scheduler generates the clicks while playing, so the cost does not
    // grow with the length of the timeline.
    if (options.metronome_clicks && sequence->division.is_ppq()) {
        const MeterMap meters = MeterMap::build(*sequence);
        const auto& segments = meters.segments();
        for (std::size_t index = 0; index < segments.size(); ++index) {
            const MeterSegment& segment = segments[index];
            const Tick segment_end = index + 1 < segments.size() ? segments[index + 1].tick : end_tick;
            const Tick beat_length = meters.ticks_per_beat(segment);
            if (beat_length <= 0 || segment.numerator <= 0 || segment_end <= segment.tick)
                continue;
            ClickSegment grid;
            grid.start = segment.tick;
            grid.end = segment_end;
            grid.beat_length = beat_length;
            grid.numerator = segment.numerator;
            grid.compound = segment.denominator == 8 && segment.numerator % 3 == 0 && segment.numerator > 3;
            snapshot->click_segments.push_back(grid);
        }
    }

    std::ranges::stable_sort(pending, [](const PendingEvent& left, const PendingEvent& right) {
        return left.key < right.key;
    });
    snapshot->events.reserve(pending.size());
    for (PendingEvent& event : pending) {
        event.event.frame = snapshot->mapping.frame_at(event.event.tick);
        snapshot->events.push_back(event.event);
    }
    pending.clear();
    pending.shrink_to_fit();

    for (NoteBoundary& note : snapshot->notes) {
        note.start_frame = snapshot->mapping.frame_at(note.start_tick);
        note.end_frame = snapshot->mapping.frame_at(note.end_tick);
    }
    snapshot->notes_by_owner.reserve(snapshot->notes.size());
    for (std::uint32_t index = 0; index < snapshot->notes.size(); ++index)
        snapshot->notes_by_owner.emplace_back(snapshot->notes[index].owner, index);
    std::ranges::sort(snapshot->notes_by_owner, [](const auto& left, const auto& right) { return left.first < right.first; });

    // Controller checkpoints keyed by route.
    snapshot->baseline_states.reserve(snapshot->routes.size());
    for (const Route& route : snapshot->routes)
        snapshot->baseline_states.push_back(baseline_for(route));
    std::vector<ChannelState> current = snapshot->baseline_states;
    for (std::size_t index = 0; index < snapshot->events.size(); ++index) {
        if (index % options.checkpoint_interval == 0) {
            snapshot->checkpoint_events.push_back(index);
            snapshot->checkpoint_states.insert(snapshot->checkpoint_states.end(), current.begin(), current.end());
        }
        const ScheduledEvent& event = snapshot->events[index];
        if (event.kind == RealtimeEventKind::midi && event.role == NoteRole::none && event.route < current.size())
            current[event.route].apply(event.bytes[0], event.bytes[1], event.bytes[2]);
    }

    snapshot->end_tick = end_tick;
    snapshot->end_frame = snapshot->mapping.frame_at(end_tick);

    if (options.session.loop) {
        LoopInfo loop;
        loop.start_tick = options.session.loop->begin;
        loop.end_tick = options.session.loop->end;
        loop.start_frame = snapshot->mapping.frame_at(loop.start_tick);
        loop.end_frame = snapshot->mapping.frame_at(loop.end_tick);
        const auto start_exact = snapshot->mapping.exact_frame(loop.start_tick);
        const auto end_exact = snapshot->mapping.exact_frame(loop.end_tick);
        loop.length_numerator = end_exact.numerator - start_exact.numerator;
        loop.length_denominator = end_exact.denominator;
        if (loop.length_numerator <= 0)
            return make_error(ErrorCode::invalid_argument, "the loop range has no duration");
        loop.first_event = snapshot->first_event_at_or_after(loop.start_tick);
        loop.end_event = snapshot->first_event_at_or_after(loop.end_tick);
        snapshot->loop = loop;
    }

    // Real-time preflight: maximum events inside any window of one block (section 20.2).
    std::size_t peak = 0;
    std::size_t window_start = 0;
    for (std::size_t index = 0; index < snapshot->events.size(); ++index) {
        while (snapshot->events[index].frame - snapshot->events[window_start].frame >=
               static_cast<std::int64_t>(options.preflight_block_frames))
            ++window_start;
        peak = std::max(peak, index - window_start + 1);
    }
    statistics.peak_events_per_block = peak;
    if (peak > options.max_events_per_block) {
        snapshot->live_playback_allowed = false;
        snapshot->live_refusal = std::format(
            "{} events fall inside one audio block of {} frames, above the real-time budget of {} events per "
            "block. Live playback is refused; the sequence can still be inspected, exported or rendered offline.",
            peak, options.preflight_block_frames, options.max_events_per_block);
    }
    return std::unique_ptr<const PlaybackSnapshot>(std::move(snapshot));
}

} // namespace mwb::playback
