#include "mwb/music/voice.hpp"

#include "mwb/core/notes.hpp"
#include "mwb/music/note_edit.hpp"

#include <algorithm>
#include <map>
#include <unordered_map>

namespace mwb::music {

std::string_view voice_profile_name(VoiceProfile profile) noexcept {
    switch (profile) {
    case VoiceProfile::monophonic_channel: return "monophonic_channel";
    case VoiceProfile::top_voice: return "top_voice";
    case VoiceProfile::bottom_voice: return "bottom_voice";
    case VoiceProfile::onset_chord: return "onset_chord";
    case VoiceProfile::explicit_selection: return "explicit_selection";
    }
    return "unknown";
}

Result<std::vector<VoiceStream>> extract_voices(const Sequence& sequence, VoiceProfile profile,
                                                const VoiceExtractionOptions& options) {
    if (profile == VoiceProfile::explicit_selection)
        return make_error(ErrorCode::invalid_argument, "use extract_selection for the explicit selection profile");
    Tick tolerance = 0;
    if (options.onset_tolerance) {
        if (*options.onset_tolerance < 0)
            return make_error(ErrorCode::invalid_argument, "onset tolerance must not be negative");
        tolerance = *options.onset_tolerance;
    } else if (sequence.division.is_ppq()) {
        tolerance = default_onset_tolerance(sequence.division.ppq);
    } else {
        return make_error(ErrorCode::invalid_argument,
                          "SMPTE sequences need an explicit absolute onset tolerance");
    }
    const NotePairing pairing = pair_notes(sequence);
    // Split by (track, channel); pairing order is stable event order.
    std::map<std::pair<std::size_t, std::uint8_t>, std::vector<std::uint32_t>> lines;
    for (std::uint32_t index = 0; index < pairing.notes.size(); ++index) {
        const auto& note = pairing.notes[index];
        if (sequence.tracks[note.on_track].is_drum && !options.include_drums)
            continue;
        lines[{note.on_track, note.channel}].push_back(index);
    }
    std::vector<VoiceStream> streams;
    for (auto& [key, members] : lines) {
        std::ranges::stable_sort(members, [&](std::uint32_t a, std::uint32_t b) {
            return pairing.notes[a].start < pairing.notes[b].start;
        });
        VoiceStream stream;
        stream.track_index = key.first;
        stream.channel = key.second;
        stream.profile = profile;
        stream.onset_tolerance = tolerance;
        const auto make_note = [&](std::uint32_t index) {
            const auto& span = pairing.notes[index];
            VoiceNote note;
            note.start = span.start;
            note.end = span.end;
            note.pitch = span.pitch;
            note.velocity = span.velocity;
            note.note_id = span.on_id;
            note.ordinal = index;
            note.chord_mask = static_cast<std::uint16_t>(1u << (span.pitch % 12));
            return note;
        };
        if (profile == VoiceProfile::monophonic_channel) {
            bool overlapping = false;
            for (std::size_t index = 1; index < members.size(); ++index)
                overlapping = overlapping || pairing.notes[members[index]].start < pairing.notes[members[index - 1]].end ||
                              pairing.notes[members[index]].start == pairing.notes[members[index - 1]].start;
            if (overlapping)
                continue;
            for (const auto index : members)
                stream.notes.push_back(make_note(index));
            streams.push_back(std::move(stream));
            continue;
        }
        std::size_t begin = 0;
        while (begin < members.size()) {
            const Tick group_start = pairing.notes[members[begin]].start;
            std::size_t end = begin;
            while (end < members.size() && pairing.notes[members[end]].start - group_start <= tolerance)
                ++end;
            std::uint32_t chosen = members[begin];
            std::uint16_t mask = 0;
            Tick latest_end = 0;
            for (std::size_t position = begin; position < end; ++position) {
                const auto index = members[position];
                const auto& span = pairing.notes[index];
                mask = static_cast<std::uint16_t>(mask | (1u << (span.pitch % 12)));
                latest_end = std::max(latest_end, span.end);
                const auto& best = pairing.notes[chosen];
                const bool better = profile == VoiceProfile::top_voice ? span.pitch > best.pitch
                                                                       : span.pitch < best.pitch;
                // Ties follow stable event order: the note earlier in the pairing wins.
                if (better || (span.pitch == best.pitch && index < chosen))
                    chosen = index;
            }
            VoiceNote note = make_note(chosen);
            if (profile == VoiceProfile::onset_chord) {
                note.start = group_start;
                note.end = latest_end;
                note.chord_mask = mask;
            }
            note.group_size = static_cast<std::uint8_t>(std::min<std::size_t>(end - begin, 255));
            stream.notes.push_back(note);
            begin = end;
        }
        streams.push_back(std::move(stream));
    }
    return streams;
}

Result<VoiceStream> extract_selection(const Sequence& sequence, std::span<const EventId> notes) {
    const NotePairing pairing = pair_notes(sequence);
    std::unordered_map<EventId, std::uint32_t, UuidHash> index_of;
    for (std::uint32_t index = 0; index < pairing.notes.size(); ++index)
        index_of.emplace(pairing.notes[index].on_id, index);
    VoiceStream stream;
    stream.profile = VoiceProfile::explicit_selection;
    for (std::size_t position = 0; position < notes.size(); ++position) {
        const auto found = index_of.find(notes[position]);
        if (found == index_of.end())
            return make_error(ErrorCode::not_found, "selected note does not exist", notes[position].to_string());
        const auto& span = pairing.notes[found->second];
        if (position == 0) {
            stream.track_index = span.on_track;
            stream.channel = span.channel;
        }
        VoiceNote note;
        note.start = span.start;
        note.end = span.end;
        note.pitch = span.pitch;
        note.velocity = span.velocity;
        note.note_id = span.on_id;
        note.ordinal = found->second;
        note.chord_mask = static_cast<std::uint16_t>(1u << (span.pitch % 12));
        stream.notes.push_back(note);
    }
    return stream;
}

} // namespace mwb::music
