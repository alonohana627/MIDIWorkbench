#include "mwb/music/ppq.hpp"

#include "mwb/music/grid.hpp"

#include <deque>
#include <format>
#include <map>
#include <unordered_map>

namespace mwb::music {

namespace {

[[nodiscard]] Status validate_ppq(std::int32_t ppq) {
    if (ppq < 1 || ppq > 32767)
        return make_error(ErrorCode::invalid_argument, "PPQ must lie in 1..32767");
    return ok();
}

/// Converts one tick with its rounding error.
[[nodiscard]] Result<std::pair<Tick, Rational>> convert_tick(Tick tick, std::int32_t source_ppq, std::int32_t destination_ppq,
                                               Tick source_anchor, Tick destination_anchor) {
    auto exact = make_rational((static_cast<__int128>(tick) - source_anchor) * destination_ppq +
                                   static_cast<__int128>(destination_anchor) * source_ppq,
                               source_ppq);
    if (!exact)
        return std::unexpected(exact.error());
    const Tick rounded = round_rational(*exact);
    if (rounded < 0)
        return make_error(ErrorCode::invalid_argument, "converted events would lie before tick 0");
    return std::pair{rounded, rounding_error(*exact, rounded)};
}

/// Tracks merged source ticks per destination tick.
class CollisionCounter {
public:
    void add(Tick source, Tick destination) {
        const auto [found, inserted] = first_source_.emplace(destination, source);
        if (!inserted && found->second != source)
            ++collisions_;
    }
    [[nodiscard]] std::size_t count() const { return collisions_; }

private:
    std::unordered_map<Tick, Tick> first_source_;
    std::size_t collisions_ = 0;
};

} // namespace

Result<PpqConversion> convert_events_ppq(std::span<const Event> events, std::int32_t source_ppq,
                                         std::int32_t destination_ppq, Tick source_anchor, Tick destination_anchor) {
    MWB_TRY(validate_ppq(source_ppq));
    MWB_TRY(validate_ppq(destination_ppq));
    PpqConversion conversion;
    CollisionCounter collisions;
    std::map<std::pair<int, int>, std::deque<std::pair<Tick, Tick>>> open_notes;
    for (const auto& event : events) {
        auto converted = convert_tick(event.tick, source_ppq, destination_ppq, source_anchor, destination_anchor);
        if (!converted)
            return std::unexpected(converted.error());
        Event copy = event;
        copy.tick = converted->first;
        if (compare_rational(converted->second, conversion.max_error) > 0)
            conversion.max_error = converted->second;
        collisions.add(event.tick, copy.tick);
        if (event.is_note_start()) {
            open_notes[{event.channel(), event.data1()}].emplace_back(event.tick, copy.tick);
        } else if (event.is_note_end()) {
            auto& queue = open_notes[{event.channel(), event.data1()}];
            if (!queue.empty()) {
                const auto [source_start, destination_start] = queue.front();
                queue.pop_front();
                if (event.tick > source_start && copy.tick <= destination_start)
                    ++conversion.collapsed_notes;
            }
        }
        conversion.events.push_back(std::move(copy));
    }
    conversion.collisions = collisions.count();
    return conversion;
}

Result<TransformPreview> change_sequence_ppq(const DocumentData& document, SequenceId sequence_id,
                                             std::int32_t new_ppq) {
    MWB_TRY(validate_ppq(new_ppq));
    auto editor = NoteEditor::open(document, sequence_id);
    if (!editor)
        return std::unexpected(editor.error());
    const Sequence& sequence = editor->sequence();
    if (!sequence.division.is_ppq())
        return make_error(ErrorCode::unsupported, "SMPTE sequences need a tempo-map conversion, not a PPQ change");
    const std::int32_t old_ppq = sequence.division.ppq;
    TransformReport report;
    report.tool = "change_ppq";
    const auto convert = [&](Tick tick) -> Result<Tick> {
        auto converted = convert_tick(tick, old_ppq, new_ppq, 0, 0);
        if (!converted)
            return std::unexpected(converted.error());
        if (compare_rational(converted->second, report.max_rounding_error) > 0)
            report.max_rounding_error = converted->second;
        return converted->first;
    };

    std::unordered_map<EventId, Tick, UuidHash> new_ticks;
    for (const auto& track : sequence.tracks) {
        CollisionCounter collisions;
        for (const auto& event : track.events) {
            auto tick = convert(event.tick);
            if (!tick)
                return std::unexpected(tick.error());
            new_ticks.emplace(event.id, *tick);
            collisions.add(event.tick, *tick);
            ++report.targeted;
        }
        report.collisions += collisions.count();
    }
    // Enforce the one-tick floor for notes that had a positive length.
    for (const auto& note : editor->notes()) {
        if (!note.off_id || note.end <= note.start)
            continue;
        const Tick start = new_ticks.at(note.on_id);
        Tick& end = new_ticks.at(*note.off_id);
        if (end <= start) {
            end = start + 1;
            ++report.clipped;
        }
    }
    for (std::size_t track = 0; track < sequence.tracks.size(); ++track) {
        const auto& events = sequence.tracks[track].events;
        for (std::size_t index = 0; index < events.size(); ++index) {
            const Tick tick = new_ticks.at(events[index].id);
            if (tick != events[index].tick) {
                editor->retime_event(track, index, tick);
                ++report.changed;
            }
        }
        Track header = sequence.tracks[track];
        auto end = convert(header.end_tick);
        if (!end)
            return std::unexpected(end.error());
        header.end_tick = *end;
        editor->set_track_header(track, std::move(header));
    }
    Sequence header = sequence;
    header.division.ppq = new_ppq;
    auto end = convert(header.end_tick);
    if (!end)
        return std::unexpected(end.error());
    header.end_tick = *end;
    std::size_t floored_regions = 0;
    for (auto& region : header.regions) {
        auto start = convert(region.start_tick);
        auto finish = convert(region.end_tick);
        if (!start || !finish)
            return make_error(ErrorCode::limit_exceeded, "region conversion overflows");
        region.start_tick = *start;
        region.end_tick = std::max(*finish, *start + 1);
        floored_regions += *finish < *start + 1 ? 1 : 0;
    }
    for (auto& region : header.scale_regions) {
        auto start = convert(region.start_tick);
        auto finish = convert(region.end_tick);
        if (!start || !finish)
            return make_error(ErrorCode::limit_exceeded, "scale region conversion overflows");
        region.start_tick = *start;
        region.end_tick = std::max(*finish, *start + 1);
        floored_regions += *finish < *start + 1 ? 1 : 0;
    }
    report.clipped += floored_regions;
    editor->set_sequence_header(std::move(header));
    editor->set_chronological_order(true);
    report.messages.push_back(std::format("PPQ {} -> {}: maximum timing error {} ticks, {} merged ticks, {} floors applied",
                                          old_ppq, new_ppq, report.max_rounding_error.to_string(), report.collisions,
                                          report.clipped));
    TransformPreview preview;
    preview.report = std::move(report);
    preview.changes = editor->finish("Change PPQ");
    return preview;
}

} // namespace mwb::music
