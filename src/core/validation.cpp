#include "mwb/core/validation.hpp"

#include "mwb/core/notes.hpp"

#include <algorithm>
#include <unordered_set>

namespace mwb {

const char* severity_name(Severity severity) {
    switch (severity) {
    case Severity::info: return "info";
    case Severity::warning: return "warning";
    case Severity::error: return "error";
    }
    return "error";
}

bool event_is_well_formed(const Event& event, std::string* reason) {
    const auto fail = [&](const char* text) {
        if (reason)
            *reason = text;
        return false;
    };
    switch (event.category) {
    case EventCategory::channel: {
        if (event.code < 0x80 || event.code > 0xEF)
            return fail("channel status byte out of range");
        if (event.payload.size() != channel_data_length(event.code))
            return fail("channel message has the wrong number of data bytes");
        for (const auto byte : event.payload.bytes())
            if (byte > 0x7F)
                return fail("channel data byte above 127");
        return true;
    }
    case EventCategory::meta: {
        if (event.code > 0x7F || event.code == meta_type::end_of_track)
            return fail("meta type is not storable");
        const auto size = event.payload.size();
        switch (event.code) {
        case meta_type::tempo:
            if (size != 3)
                return fail("tempo meta must have three bytes");
            if (event.payload[0] == 0 && event.payload[1] == 0 && event.payload[2] == 0)
                return fail("tempo must be nonzero");
            return true;
        case meta_type::time_signature:
            if (size != 4)
                return fail("time signature meta must have four bytes");
            if (event.payload[0] == 0)
                return fail("time signature numerator must be positive");
            return true;
        case meta_type::key_signature: {
            if (size != 2)
                return fail("key signature meta must have two bytes");
            const auto sharps = static_cast<std::int8_t>(event.payload[0]);
            if (sharps < -7 || sharps > 7 || event.payload[1] > 1)
                return fail("key signature out of range");
            return true;
        }
        case meta_type::channel_prefix:
        case meta_type::port_prefix:
            if (size != 1)
                return fail("prefix meta must have one byte");
            return true;
        case meta_type::smpte_offset:
            if (size != 5)
                return fail("SMPTE offset meta must have five bytes");
            return true;
        default: return true;
        }
    }
    case EventCategory::sysex_f0:
        if (event.code != 0xF0)
            return fail("F0 packet code mismatch");
        return true;
    case EventCategory::sysex_f7:
        if (event.code != 0xF7)
            return fail("F7 packet code mismatch");
        return true;
    }
    return fail("unknown category");
}

std::vector<Diagnostic> validate_document(const DocumentData& document, const DocumentLimits& limits) {
    std::vector<Diagnostic> diagnostics;
    const auto add = [&](Severity severity, std::string code, std::string message, int sequence = -1, int track = -1,
                         std::int64_t ordinal = -1) {
        diagnostics.push_back(Diagnostic{severity, std::move(code), std::move(message), sequence, track, ordinal});
    };
    if (document.sequences.empty())
        add(Severity::error, "document.no_sequence", "an editable document needs at least one sequence");
    if (document.sequences.size() > limits.max_sequences)
        add(Severity::error, "limit.sequences", "too many independent sequences");

    std::size_t total_events = 0;
    std::unordered_set<Uuid, UuidHash> identities;
    const auto unique_id = [&](const Uuid& id) { return identities.insert(id).second; };

    for (std::size_t sequence_index = 0; sequence_index < document.sequences.size(); ++sequence_index) {
        const auto& sequence = document.sequences[sequence_index];
        const int s = static_cast<int>(sequence_index);
        if (!unique_id(sequence.id.uuid()))
            add(Severity::error, "identity.duplicate", "duplicate sequence identity", s);
        if (auto status = sequence.division.validate(); !status)
            add(Severity::error, "sequence.division", status.error().message, s);
        if (sequence.root_pc > 11)
            add(Severity::error, "sequence.root", "root pitch class must be 0..11", s);
        if (sequence.scale_mask == 0 || sequence.scale_mask > 0x0FFF || (sequence.scale_mask & 1) == 0)
            add(Severity::error, "sequence.scale", "scale mask must include the root and fit in 12 bits", s);
        if (sequence.tracks.empty())
            add(Severity::error, "sequence.no_track", "an editable sequence needs at least one track", s);
        if (sequence.tracks.size() > limits.max_tracks_per_sequence)
            add(Severity::error, "limit.tracks", "too many tracks in one sequence", s);
        if (sequence.end_tick < 0)
            add(Severity::error, "sequence.end", "sequence end must not be negative", s);

        std::unordered_set<Uuid, UuidHash> sequence_tracks;
        for (std::size_t track_index = 0; track_index < sequence.tracks.size(); ++track_index) {
            const auto& track = sequence.tracks[track_index];
            const int t = static_cast<int>(track_index);
            sequence_tracks.insert(track.id.uuid());
            if (!unique_id(track.id.uuid()))
                add(Severity::error, "identity.duplicate", "duplicate track identity", s, t);
            total_events += track.events.size();
            for (std::size_t index = 0; index < track.events.size(); ++index) {
                const auto& event = track.events[index];
                const auto ordinal = static_cast<std::int64_t>(index);
                if (event.tick < 0)
                    add(Severity::error, "event.negative_time", "event time is negative", s, t, ordinal);
                if (index > 0) {
                    const auto& previous = track.events[index - 1];
                    if (previous.tick > event.tick || (previous.tick == event.tick && previous.order_key >= event.order_key))
                        add(Severity::error, "event.order", "events are not in strict (tick, order) order", s, t, ordinal);
                }
                if (event.payload.size() > limits.max_payload_bytes)
                    add(Severity::error, "limit.payload", "event payload exceeds the size limit", s, t, ordinal);
                if (!unique_id(event.id.uuid()))
                    add(Severity::error, "identity.duplicate", "duplicate event identity", s, t, ordinal);
                std::string reason;
                if (!event_is_well_formed(event, &reason)) {
                    const bool preserved = (event.flags & event_flag::malformed) != 0;
                    add(preserved ? Severity::warning : Severity::error, "event.malformed", reason, s, t, ordinal);
                }
            }
            if (track.end_tick < track.last_event_tick())
                add(Severity::error, "track.end", "track end tick is before its last event", s, t);
        }
        for (const auto& region : sequence.regions) {
            if (region.end_tick <= region.start_tick || region.start_tick < 0)
                add(Severity::error, "region.range", "region must have positive length", s);
            for (const auto& track : region.tracks)
                if (!sequence_tracks.contains(track.uuid()))
                    add(Severity::error, "region.track", "region refers to a track of another sequence", s);
        }
        auto scale_regions = sequence.scale_regions;
        std::sort(scale_regions.begin(), scale_regions.end(),
                  [](const ScaleRegion& a, const ScaleRegion& b) { return a.start_tick < b.start_tick; });
        for (std::size_t index = 0; index < scale_regions.size(); ++index) {
            const auto& region = scale_regions[index];
            if (region.end_tick <= region.start_tick)
                add(Severity::error, "scale_region.range", "scale region must have positive length", s);
            if ((region.scale_mask & 1) == 0 || region.scale_mask > 0x0FFF || region.root_pc > 11)
                add(Severity::error, "scale_region.scale", "scale region mask or root is invalid", s);
            if (index > 0 && scale_regions[index - 1].end_tick > region.start_tick)
                add(Severity::error, "scale_region.overlap", "scale regions overlap", s);
        }
        const auto pairing = pair_notes(sequence);
        if (!pairing.orphan_ends.empty())
            add(Severity::info, "notes.orphan_end",
                std::to_string(pairing.orphan_ends.size()) + " note end(s) without a matching note start", s);
        if (pairing.ambiguous_count > 0)
            add(Severity::info, "notes.ambiguous",
                std::to_string(pairing.ambiguous_count) + " overlapping same-key notes paired first-in-first-out", s);
    }
    if (total_events > limits.max_events_per_document)
        add(Severity::warning, "limit.events", "document exceeds the editable event limit; open for inspection only");
    return diagnostics;
}

bool has_errors(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& diagnostic) { return diagnostic.severity == Severity::error; });
}

} // namespace mwb
