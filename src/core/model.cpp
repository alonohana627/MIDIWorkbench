#include "mwb/core/model.hpp"

#include <algorithm>
#include <chrono>

namespace mwb {

std::optional<std::size_t> Sequence::track_index(TrackId track) const {
    for (std::size_t index = 0; index < tracks.size(); ++index)
        if (tracks[index].id == track)
            return index;
    return std::nullopt;
}

Track track_header(const Track& track) {
    Track header;
    header.id = track.id;
    header.name = track.name;
    header.color_rgba = track.color_rgba;
    header.end_tick = track.end_tick;
    header.muted = track.muted;
    header.solo = track.solo;
    header.is_drum = track.is_drum;
    header.routing = track.routing;
    header.instrument = track.instrument;
    return header;
}

Sequence sequence_header(const Sequence& sequence) {
    Sequence header;
    header.id = sequence.id;
    header.name = sequence.name;
    header.division = sequence.division;
    header.end_tick = sequence.end_tick;
    header.root_pc = sequence.root_pc;
    header.scale_mask = sequence.scale_mask;
    header.settings = sequence.settings;
    header.regions = sequence.regions;
    header.scale_regions = sequence.scale_regions;
    header.recipes = sequence.recipes;
    return header;
}

Tick Sequence::effective_end() const {
    Tick end = end_tick;
    for (const auto& track : tracks)
        end = std::max({end, track.end_tick, track.last_event_tick()});
    for (const auto& region : regions)
        end = std::max(end, region.end_tick);
    return end;
}

std::optional<std::size_t> DocumentData::sequence_index(SequenceId sequence) const {
    for (std::size_t index = 0; index < sequences.size(); ++index)
        if (sequences[index].id == sequence)
            return index;
    return std::nullopt;
}

std::optional<TrackLocation> find_track(const DocumentData& document, TrackId track) {
    for (std::size_t sequence = 0; sequence < document.sequences.size(); ++sequence) {
        if (const auto index = document.sequences[sequence].track_index(track))
            return TrackLocation{sequence, *index};
    }
    return std::nullopt;
}

std::int64_t now_utc_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

DocumentData make_blank_document(std::string title) {
    DocumentData document;
    document.id = DocumentId::generate();
    document.title = std::move(title);
    document.created_ms = now_utc_ms();
    document.modified_ms = document.created_ms;

    Sequence sequence;
    sequence.id = SequenceId::generate();
    sequence.name = "Sequence 1";
    sequence.division = Division::make_ppq(default_ppq);
    const Tick bar = 4 * static_cast<Tick>(default_ppq);
    sequence.end_tick = 4 * bar;

    Track track;
    track.id = TrackId::generate();
    track.name = "Track 1";
    track.end_tick = sequence.end_tick;
    Event tempo = events::tempo(0, static_cast<std::uint32_t>(default_microseconds_per_quarter));
    tempo.order_key = order_key_spacing;
    Event meter = events::time_signature(0, 4, 2);
    meter.order_key = 2 * order_key_spacing;
    track.events.push_back(std::move(tempo));
    track.events.push_back(std::move(meter));
    sequence.tracks.push_back(std::move(track));

    Region loop;
    loop.id = RegionId::generate();
    loop.name = "Loop";
    loop.start_tick = 0;
    loop.end_tick = 4 * bar;
    loop.is_loop = true;
    sequence.settings.loop_region = loop.id;
    sequence.regions.push_back(std::move(loop));

    document.sequences.push_back(std::move(sequence));
    return document;
}

} // namespace mwb
