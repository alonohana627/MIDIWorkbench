#include "mwb/midi/codec.hpp"

#include "mwb/core/notes.hpp"

#include <libremidi/writer.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <sstream>
#include <tuple>

namespace mwb::midi {

std::string ExportReport::to_json() const {
    nlohmann::json value = {{"version", 1},
                            {"format", format},
                            {"track_count", track_count},
                            {"events_written", events_written},
                            {"events_omitted", events_omitted},
                            {"notes_trimmed", notes_trimmed}};
    auto list = nlohmann::json::array();
    for (const auto& diagnostic : diagnostics)
        list.push_back({{"severity", severity_name(diagnostic.severity)},
                        {"code", diagnostic.code},
                        {"message", diagnostic.message}});
    value["diagnostics"] = std::move(list);
    return value.dump();
}

namespace {

constexpr Tick max_delta = 0x0FFFFFFF; // SMF variable-length quantity range

/// One output track before serialization: event copies with absolute ticks in output order.
struct OutputTrack {
    std::vector<Event> events;
    Tick end_tick = 0;
};

bool is_global_meta(const Event& event) {
    return event.category == EventCategory::meta &&
           (event.code == meta_type::tempo || event.code == meta_type::time_signature ||
            event.code == meta_type::key_signature || event.code == meta_type::smpte_offset);
}

/// Audible-track rule of section 12: any soloed track is audible unless muted; no solo means all
/// unmuted tracks. An explicit track subset is applied first.
std::vector<std::size_t> selected_tracks(const Sequence& sequence, const ExportOptions& options) {
    const bool any_solo = std::any_of(sequence.tracks.begin(), sequence.tracks.end(),
                                      [](const Track& track) { return track.solo; });
    std::vector<std::size_t> indices;
    for (std::size_t index = 0; index < sequence.tracks.size(); ++index) {
        const auto& track = sequence.tracks[index];
        if (!options.tracks.empty() &&
            std::find(options.tracks.begin(), options.tracks.end(), track.id) == options.tracks.end())
            continue;
        if (options.audible_tracks_only && (track.muted || (any_solo && !track.solo)))
            continue;
        indices.push_back(index);
    }
    return indices;
}

void append_vlq(std::vector<std::uint8_t>& out, std::uint32_t value) {
    std::uint8_t groups[5];
    int count = 0;
    do {
        groups[count++] = static_cast<std::uint8_t>(value & 0x7F);
        value >>= 7;
    } while (value != 0 && count < 5);
    for (int index = count - 1; index >= 0; --index)
        out.push_back(static_cast<std::uint8_t>(groups[index] | (index > 0 ? 0x80 : 0x00)));
}

/// Converts a domain event into the byte form libremidi's writer expects.
libremidi::message to_message(const Event& event) {
    libremidi::message message;
    switch (event.category) {
    case EventCategory::channel:
        message.bytes.push_back(event.code);
        break;
    case EventCategory::meta: {
        message.bytes.push_back(0xFF);
        message.bytes.push_back(event.code);
        std::vector<std::uint8_t> length;
        append_vlq(length, static_cast<std::uint32_t>(event.payload.size()));
        message.bytes.insert(message.bytes.end(), length.begin(), length.end());
        break;
    }
    case EventCategory::sysex_f0:
    case EventCategory::sysex_f7:
        message.bytes.push_back(event.code); // the writer adds the VLQ length after it
        break;
    }
    for (const auto byte : event.payload.bytes())
        message.bytes.push_back(byte);
    return message;
}

/// Export-selected-range for one track (section 16.6): notes crossing the boundaries are trimmed,
/// controller state at the range start is initialized at tick 0 in a defined order (bank select,
/// program, other controllers, pressure, bend), events are rebased to tick zero and the track
/// ends at the range end.
OutputTrack range_track(const Track& track, std::uint32_t track_index, TimeRange range, bool carry_globals,
                        ExportReport& report) {
    OutputTrack output;
    output.end_tick = range.length();
    std::map<std::pair<int, int>, const Event*> controllers;
    std::map<int, const Event*> programs;
    std::map<int, const Event*> pressures;
    std::map<int, const Event*> bends;
    std::map<std::uint8_t, const Event*> globals;
    for (const auto& event : track.events) {
        if (event.tick >= range.begin)
            break;
        if (event.is_channel()) {
            switch (event.message()) {
            case ChannelMessage::control_change: controllers[{event.channel(), event.data1()}] = &event; break;
            case ChannelMessage::program_change: programs[event.channel()] = &event; break;
            case ChannelMessage::channel_pressure: pressures[event.channel()] = &event; break;
            case ChannelMessage::pitch_bend: bends[event.channel()] = &event; break;
            case ChannelMessage::note_off:
            case ChannelMessage::note_on:
            case ChannelMessage::poly_pressure: break;
            }
        } else if (carry_globals && is_global_meta(event)) {
            globals[event.code] = &event;
        }
    }
    const auto add_copy = [&](const Event& source, Tick tick) {
        Event copy = source;
        copy.tick = tick;
        output.events.push_back(std::move(copy));
    };
    for (const auto& [type, event] : globals)
        add_copy(*event, 0);
    for (const auto& [key, event] : controllers)
        if (key.second == 0 || key.second == 32)
            add_copy(*event, 0);
    for (const auto& [channel, event] : programs)
        add_copy(*event, 0);
    for (const auto& [key, event] : controllers)
        if (key.second != 0 && key.second != 32)
            add_copy(*event, 0);
    for (const auto& [channel, event] : pressures)
        add_copy(*event, 0);
    for (const auto& [channel, event] : bends)
        add_copy(*event, 0);

    const auto pairing = pair_track_notes(track, track_index, track.end_tick);
    std::map<EventId, const NoteSpan*> by_on;
    for (const auto& note : pairing.notes)
        by_on[note.on_id] = &note;
    std::vector<Event> body;
    for (const auto& event : track.events) {
        if (event.is_note_start()) {
            const auto found = by_on.find(event.id);
            if (found == by_on.end())
                continue;
            const NoteSpan& note = *found->second;
            const Tick start = std::max(note.start, range.begin);
            const Tick end = std::min(note.end, range.end);
            const bool zero_length_inside = note.start == note.end && range.contains(note.start);
            if (end <= start && !zero_length_inside)
                continue;
            if (start != note.start || end != note.end)
                ++report.notes_trimmed;
            Event on = event;
            on.tick = start - range.begin;
            body.push_back(std::move(on));
            Event off = events::note_off(end - range.begin, note.channel, note.pitch, note.release_velocity);
            body.push_back(std::move(off));
        } else if (event.is_note_end()) {
            continue; // emitted with its paired note start; orphans are outside any exported note
        } else if (range.contains(event.tick)) {
            Event copy = event;
            copy.tick -= range.begin;
            body.push_back(std::move(copy));
        }
    }
    // Stable by tick; a note end created for a note keeps its relative place after starts at the
    // same tick only when it belongs to a later note, which the stable order of creation preserves.
    std::stable_sort(body.begin(), body.end(), [](const Event& a, const Event& b) { return a.tick < b.tick; });
    for (auto& event : body)
        output.events.push_back(std::move(event));
    return output;
}

OutputTrack plain_track(const Track& track, bool skip_globals) {
    OutputTrack output;
    output.end_tick = std::max(track.end_tick, track.last_event_tick());
    output.events.reserve(track.events.size());
    for (const auto& event : track.events) {
        if (skip_globals && is_global_meta(event))
            continue;
        output.events.push_back(event);
    }
    return output;
}

/// Canonical-profile repairs: drop malformed events and close unterminated notes at the track end.
void repair_track(OutputTrack& output, ExportReport& report) {
    std::size_t dropped = 0;
    std::erase_if(output.events, [&](const Event& event) {
        if ((event.flags & event_flag::malformed) == 0)
            return false;
        ++dropped;
        return true;
    });
    report.events_omitted += dropped;
    Track temporary;
    temporary.events = output.events;
    const auto pairing = pair_track_notes(temporary, 0, output.end_tick);
    std::size_t closed = 0;
    for (const auto& note : pairing.notes) {
        if (note.flags & note_flag::unterminated) {
            output.events.push_back(events::note_off(output.end_tick, note.channel, note.pitch, 0));
            ++closed;
        }
    }
    if (closed > 0) {
        std::stable_sort(output.events.begin(), output.events.end(),
                         [](const Event& a, const Event& b) { return a.tick < b.tick; });
        report.diagnostics.push_back(Diagnostic{Severity::warning, "repair.closed_notes",
                                                std::to_string(closed) + " unterminated note(s) closed at the track end"});
    }
}

/// Builds the output tracks of one sequence.
Result<std::vector<OutputTrack>> sequence_tracks(const Sequence& sequence, const ExportOptions& options, int format,
                                                 ExportReport& report) {
    const auto chosen = selected_tracks(sequence, options);
    if (chosen.empty())
        return make_error(ErrorCode::invalid_argument, "no track is selected for export");
    const bool compatibility = options.mode == ExportMode::compatibility;
    const bool conductor = compatibility && options.profile == CompatibilityProfile::canonical_type1 && format == 1;
    if (options.range && options.range->empty())
        return make_error(ErrorCode::invalid_argument, "export range is empty");

    std::vector<OutputTrack> outputs;
    if (conductor) {
        // Canonical profile: global tempo/meter/key events on a conductor track (section 16.6).
        std::vector<std::tuple<Tick, std::size_t, std::uint64_t, const Event*>> globals;
        for (const auto index : chosen)
            for (const auto& event : sequence.tracks[index].events)
                if (is_global_meta(event))
                    globals.emplace_back(event.tick, index, event.order_key, &event);
        std::stable_sort(globals.begin(), globals.end(), [](const auto& a, const auto& b) {
            return std::tie(std::get<0>(a), std::get<1>(a), std::get<2>(a)) <
                   std::tie(std::get<0>(b), std::get<1>(b), std::get<2>(b));
        });
        OutputTrack conductor_track;
        if (options.range) {
            std::map<std::uint8_t, const Event*> initial;
            for (const auto& item : globals)
                if (std::get<0>(item) < options.range->begin)
                    initial[std::get<3>(item)->code] = std::get<3>(item);
            for (const auto& [code, event] : initial) {
                Event copy = *event;
                copy.tick = 0;
                conductor_track.events.push_back(std::move(copy));
            }
            for (const auto& item : globals)
                if (options.range->contains(std::get<0>(item))) {
                    Event copy = *std::get<3>(item);
                    copy.tick -= options.range->begin;
                    conductor_track.events.push_back(std::move(copy));
                }
        } else {
            for (const auto& item : globals)
                conductor_track.events.push_back(*std::get<3>(item));
        }
        outputs.push_back(std::move(conductor_track));
    }
    for (const auto index : chosen) {
        const auto& track = sequence.tracks[index];
        if (options.range)
            outputs.push_back(range_track(track, static_cast<std::uint32_t>(index), *options.range, !conductor, report));
        else
            outputs.push_back(plain_track(track, conductor));
    }
    if (conductor) {
        Tick end = 0;
        for (std::size_t index = 1; index < outputs.size(); ++index)
            end = std::max(end, outputs[index].end_tick);
        outputs[0].end_tick = std::max(end, outputs[0].events.empty() ? Tick{0} : outputs[0].events.back().tick);
    }
    if (compatibility) {
        for (auto& output : outputs)
            repair_track(output, report);
        if (report.events_omitted > 0)
            report.diagnostics.push_back(Diagnostic{Severity::warning, "repair.dropped_malformed",
                                                    std::to_string(report.events_omitted) +
                                                        " malformed event(s) omitted"});
    } else {
        for (const auto& output : outputs)
            for (const auto& event : output.events)
                if (event.flags & event_flag::malformed)
                    return make_error(ErrorCode::invalid_data,
                                      "the document contains malformed preserved events; use Original bytes or a "
                                      "compatibility (repaired) export");
    }
    if (format == 0) {
        // Merge tracks in stable (tick, track, position) order.
        OutputTrack merged;
        std::vector<std::tuple<Tick, std::size_t, std::size_t>> order;
        for (std::size_t track_index = 0; track_index < outputs.size(); ++track_index) {
            merged.end_tick = std::max(merged.end_tick, outputs[track_index].end_tick);
            for (std::size_t position = 0; position < outputs[track_index].events.size(); ++position)
                order.emplace_back(outputs[track_index].events[position].tick, track_index, position);
        }
        std::sort(order.begin(), order.end());
        merged.events.reserve(order.size());
        for (const auto& [tick, track_index, position] : order)
            merged.events.push_back(outputs[track_index].events[position]);
        outputs.clear();
        outputs.push_back(std::move(merged));
    }
    return outputs;
}

} // namespace

Result<std::vector<std::uint8_t>> export_midi(const DocumentData& document, const ExportOptions& options,
                                              ExportReport* report_out) {
    ExportReport report;
    if (options.mode == ExportMode::original) {
        if (document.sources.empty() || !document.sources.front().bytes)
            return make_error(ErrorCode::not_found, "this document has no original imported bytes");
        if (report_out)
            *report_out = report;
        return *document.sources.front().bytes;
    }
    if (document.sequences.empty())
        return make_error(ErrorCode::invalid_data, "document has no sequence to export");

    std::vector<std::size_t> sequence_indices;
    int format = options.smf_format;
    if (options.sequence) {
        if (*options.sequence >= document.sequences.size())
            return make_error(ErrorCode::invalid_argument, "sequence index is out of range");
        sequence_indices.push_back(*options.sequence);
    } else if (format == 2 || (format < 0 && document.sequences.size() > 1)) {
        for (std::size_t index = 0; index < document.sequences.size(); ++index)
            sequence_indices.push_back(index);
        format = 2;
    } else {
        sequence_indices.push_back(0);
    }
    if (options.mode == ExportMode::compatibility && options.profile == CompatibilityProfile::type0)
        format = 0;
    if (format < 0)
        format = 1;
    if (format == 2 && sequence_indices.size() > 1) {
        const auto& division = document.sequences[sequence_indices.front()].division;
        for (const auto index : sequence_indices)
            if (!(document.sequences[index].division == division))
                return make_error(ErrorCode::invalid_data,
                                  "type 2 export needs the same division in every sequence; export separate files");
    }

    std::vector<OutputTrack> outputs;
    for (const auto sequence_index : sequence_indices) {
        auto tracks = sequence_tracks(document.sequences[sequence_index], options, format, report);
        if (!tracks)
            return std::unexpected(tracks.error());
        for (auto& track : *tracks)
            outputs.push_back(std::move(track));
    }
    if (format == 0 && outputs.size() != 1)
        return make_error(ErrorCode::invalid_argument, "type 0 holds exactly one track");

    const auto& division = document.sequences[sequence_indices.front()].division;
    libremidi::writer writer;
    writer.format = format;
    if (division.is_ppq())
        writer.ticksPerQuarterNote = static_cast<std::uint16_t>(division.ppq);
    else
        writer.rawDivision = division.smf_word();
    for (std::size_t track_index = 0; track_index < outputs.size(); ++track_index) {
        const auto& output = outputs[track_index];
        writer.tracks.emplace_back();
        auto& destination = writer.tracks.back();
        destination.reserve(output.events.size() + 1);
        Tick previous = 0;
        for (const auto& event : output.events) {
            const Tick delta = event.tick - previous;
            if (delta < 0)
                return make_error(ErrorCode::internal, "export events are not in time order");
            if (delta > max_delta)
                return make_error(ErrorCode::invalid_data,
                                  "a time gap exceeds the SMF variable-length range; shorten the gap before export");
            destination.push_back(
                libremidi::track_event{static_cast<int>(delta), static_cast<int>(track_index), to_message(event)});
            previous = event.tick;
            ++report.events_written;
        }
        const Tick end_delta = std::max<Tick>(0, output.end_tick - previous);
        if (end_delta > max_delta)
            return make_error(ErrorCode::invalid_data, "track end is too far after its last event for SMF");
        destination.push_back(libremidi::track_event{static_cast<int>(end_delta), static_cast<int>(track_index),
                                                     libremidi::meta_events::end_of_track()});
    }
    report.format = format;
    report.track_count = static_cast<int>(outputs.size());
    std::ostringstream stream(std::ios::binary);
    try {
        writer.write(stream);
    } catch (const std::exception& error) {
        return make_error(ErrorCode::internal, std::string("MIDI writer failed: ") + error.what());
    }
    const std::string text = stream.str();
    if (report_out)
        *report_out = report;
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

} // namespace mwb::midi
