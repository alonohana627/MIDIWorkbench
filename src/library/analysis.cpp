// Catalog writers' derived data (sections 26.3, 27.1, 27.3). Musical analysis (summaries, key
// estimation, polyphony, fingerprints, voice extraction, motif tokens) comes from the music
// module; this file maps it to catalog rows and adds the search envelope fields the query
// compiler reads (tempos, meter names, event classes) plus feature vectors.

#include "mwb/library/analysis.hpp"

#include "mwb/core/notes.hpp"
#include "mwb/core/tempo_map.hpp"
#include "mwb/core/text.hpp"
#include "mwb/music/analysis.hpp"
#include "mwb/music/fingerprint.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <set>

namespace mwb::library {

namespace {

using Json = nlohmann::json;

constexpr std::uint8_t drum_channel = 9;

/// Merged view of every event with its track index, in (tick, track, order) order.
struct MergedEvent {
    const Event* event;
    std::uint32_t track;
};

std::vector<MergedEvent> merged_events(const Sequence& sequence) {
    std::vector<MergedEvent> merged;
    for (std::size_t track = 0; track < sequence.tracks.size(); ++track)
        for (const auto& event : sequence.tracks[track].events)
            merged.push_back(MergedEvent{&event, static_cast<std::uint32_t>(track)});
    std::ranges::stable_sort(merged, [](const MergedEvent& left, const MergedEvent& right) {
        if (left.event->tick != right.event->tick)
            return left.event->tick < right.event->tick;
        if (left.track != right.track)
            return left.track < right.track;
        return left.event->order_key < right.event->order_key;
    });
    return merged;
}

void append_text(std::string& text, bool& truncated, const std::string& piece) {
    if (piece.empty() || truncated)
        return;
    if (text.size() + piece.size() + 1 > searchable_text_limit) {
        truncated = true;
        return;
    }
    if (!text.empty())
        text.push_back('\n');
    text += piece;
}

/// Sequence end used for tempo segment weighting (matches the music module's basic analysis).
Tick analysis_end(const Sequence& sequence) {
    Tick end = sequence.effective_end();
    for (const auto& track : sequence.tracks)
        end = std::max(end, track.last_event_tick());
    return end;
}

/// Distinct tempo values (quarter-note BPM) of segments that sound before the sequence end.
std::vector<double> segment_tempos(const Sequence& sequence) {
    if (!sequence.division.is_ppq())
        return {};
    const auto tempo_map = TempoMap::build(sequence);
    const auto end = analysis_end(sequence);
    std::set<double> tempos;
    const auto& segments = tempo_map.segments();
    for (std::size_t index = 0; index < segments.size(); ++index)
        if (index == 0 || segments[index].tick < end)
            tempos.insert(60'000'000.0 / static_cast<double>(segments[index].microseconds_per_quarter));
    return std::vector<double>(tempos.begin(), tempos.end());
}

SequenceSummary summarize_sequence(const Sequence& sequence, std::uint32_t sequence_no, std::string& text,
                                   bool& text_truncated) {
    const auto analyzed = music::analyze_sequence(sequence);
    SequenceSummary summary;
    summary.sequence_no = sequence_no;
    summary.ppq = analyzed.division != "smpte";
    summary.duration_seconds = std::max(0.0, analyzed.duration_seconds);
    summary.quarter_length = analyzed.quarter_length;
    summary.tempo_min = analyzed.tempo_min;
    summary.tempo_max = analyzed.tempo_max;
    summary.tempo_representative = analyzed.tempo_representative;
    summary.tempo_origin = std::string(music::tempo_origin_name(analyzed.tempo_origin));
    summary.key_root = analyzed.key_root;
    summary.key_mode = analyzed.key_mode;
    summary.key_origin = std::string(music::key_origin_name(analyzed.key_origin));
    summary.key_confidence = analyzed.key_confidence;
    summary.track_count = static_cast<std::int64_t>(analyzed.track_count);
    summary.event_count = static_cast<std::int64_t>(analyzed.event_count);
    summary.note_count = static_cast<std::int64_t>(analyzed.note_count);
    summary.min_pitch = analyzed.min_pitch;
    summary.max_pitch = analyzed.max_pitch;
    summary.max_polyphony = static_cast<std::int64_t>(analyzed.max_polyphony);
    summary.has_drums = analyzed.has_drums;
    summary.tempo_change_count = static_cast<std::int64_t>(analyzed.tempo_change_count);
    summary.tempos = segment_tempos(sequence);
    for (const auto& meter : analyzed.meters) {
        const auto name = std::to_string(meter.numerator) + "/" + std::to_string(meter.denominator);
        if (std::ranges::find(summary.meters, name) == summary.meters.end())
            summary.meters.push_back(name);
    }
    if (summary.meters.empty() && summary.ppq)
        summary.meters.push_back("4/4"); // Effective default meter (section 17.3).
    for (const auto& track : analyzed.tracks) {
        TrackSummary row;
        row.track_no = static_cast<std::uint32_t>(track.track_no);
        row.name = track.name;
        row.channel_mask = track.channel_mask;
        for (const auto& program : track.programs)
            if (std::ranges::find(row.programs, program.program) == row.programs.end())
                row.programs.push_back(program.program);
        row.program_json = track.program_json();
        row.note_count = static_cast<std::int64_t>(track.note_count);
        row.feature_json = track.feature_json();
        summary.tracks.push_back(std::move(row));
    }

    // Event-class presence for `has:` and the embedded text for full-text search.
    std::set<std::string> classes;
    if (analyzed.has_lyrics)
        classes.insert("lyrics");
    if (analyzed.has_markers)
        classes.insert("markers");
    if (analyzed.has_sysex)
        classes.insert("sysex");
    if (!analyzed.key_signatures.empty())
        classes.insert("key_signatures");
    if (analyzed.meters.size() > 1)
        classes.insert("meter_changes");
    if (analyzed.tempo_change_count > 0)
        classes.insert("tempo_changes");
    for (const auto& [event, track_index] : merged_events(sequence)) {
        if (event->is_channel()) {
            switch (event->message()) {
            case ChannelMessage::program_change:
                classes.insert("program_changes");
                break;
            case ChannelMessage::control_change:
                classes.insert("controllers");
                break;
            case ChannelMessage::pitch_bend:
                classes.insert("bend");
                break;
            case ChannelMessage::poly_pressure:
            case ChannelMessage::channel_pressure:
                classes.insert("aftertouch");
                break;
            case ChannelMessage::note_on:
            case ChannelMessage::note_off:
                break;
            }
            continue;
        }
        if (event->category != EventCategory::meta)
            continue;
        switch (event->code) {
        case meta_type::text:
        case meta_type::copyright:
        case meta_type::instrument_name:
        case meta_type::program_name:
            classes.insert("text");
            append_text(text, text_truncated, decode_midi_text(event->payload.bytes()));
            break;
        case meta_type::lyric:
        case meta_type::marker:
        case meta_type::cue_point:
            append_text(text, text_truncated, decode_midi_text(event->payload.bytes()));
            break;
        default:
            break;
        }
    }
    for (const auto& track : summary.tracks)
        append_text(text, text_truncated, track.name);
    summary.event_classes.assign(classes.begin(), classes.end());

    // The music module's versioned envelope plus the fields the search compiler reads.
    auto envelope = Json::parse(analyzed.diagnostics_json(), nullptr, false);
    if (!envelope.is_object())
        envelope = Json::object();
    envelope["tempos"] = summary.tempos;
    envelope["meter_names"] = summary.meters;
    envelope["event_classes"] = summary.event_classes;
    summary.diagnostics_json = envelope.dump();
    return summary;
}

} // namespace

search::SequenceFacts SequenceSummary::facts() const {
    search::SequenceFacts facts;
    facts.tempo_representative = tempo_representative;
    facts.tempo_min = tempo_min;
    facts.tempo_max = tempo_max;
    facts.tempos = tempos;
    facts.tempo_origin = tempo_origin;
    facts.key_root = key_root;
    facts.key_mode = key_mode;
    facts.key_origin = key_origin;
    facts.key_confidence = key_confidence;
    facts.quarter_length = quarter_length;
    facts.duration_seconds = duration_seconds;
    facts.meters = meters;
    facts.track_count = track_count;
    facts.note_count = note_count;
    facts.min_pitch = min_pitch;
    facts.max_pitch = max_pitch;
    facts.has_drums = has_drums;
    facts.event_classes = event_classes;
    for (const auto& track : tracks)
        facts.tracks.push_back(search::TrackFacts{track.channel_mask, track.programs});
    return facts;
}

std::string program_json_for(const std::vector<std::uint8_t>& programs, std::uint8_t channel) {
    Json list = Json::array();
    for (const auto program : programs)
        list.push_back(Json{{"channel", channel}, {"program", program}, {"bank_msb", nullptr}, {"bank_lsb", nullptr},
                            {"tick", 0}});
    return list.dump();
}

std::string summary_diagnostics_json(const SequenceSummary& summary) {
    Json object;
    object["version"] = 1;
    object["tempos"] = summary.tempos;
    object["meter_names"] = summary.meters;
    object["event_classes"] = summary.event_classes;
    object["tempo_change_count"] = summary.tempo_change_count;
    return object.dump();
}

DocumentAnalysis analyze_document(const DocumentData& document, const std::vector<Diagnostic>& diagnostics) {
    DocumentAnalysis analysis;
    for (const auto& diagnostic : diagnostics)
        if (diagnostic.severity != Severity::info)
            ++analysis.warning_count;
    for (std::size_t index = 0; index < document.sequences.size(); ++index)
        analysis.sequences.push_back(summarize_sequence(document.sequences[index], static_cast<std::uint32_t>(index),
                                                        analysis.searchable_text, analysis.searchable_text_truncated));
    return analysis;
}

std::vector<Fingerprint> compute_fingerprints(const DocumentData& document, const Sha256Digest& object_sha256) {
    std::vector<Fingerprint> result;
    result.push_back(Fingerprint{std::string(music::fingerprint_profile_name(music::FingerprintProfile::exact_object)),
                                 object_sha256});
    for (const auto profile : {music::FingerprintProfile::exact_event_content, music::FingerprintProfile::exact_notes,
                               music::FingerprintProfile::transposition_invariant,
                               music::FingerprintProfile::rhythm_shape}) {
        const auto fingerprint = music::fingerprint_document(document, profile);
        result.push_back(Fingerprint{std::string(music::fingerprint_profile_name(profile)), fingerprint.sha256});
    }
    return result;
}

const std::vector<std::string>& feature_dimension_names() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> list;
        for (int pitch_class = 0; pitch_class < 12; ++pitch_class)
            list.push_back("pitch_class_" + std::to_string(pitch_class));
        for (int interval = -6; interval <= 6; ++interval)
            list.push_back("interval_" + std::to_string(interval));
        for (const char* name : {"note_density", "mean_velocity", "max_polyphony", "pitch_range", "mean_pitch",
                                 "drum_fraction", "tempo"})
            list.push_back(name);
        return list;
    }();
    return names;
}

std::vector<std::uint8_t> FeatureVector::payload() const {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(values.size() * 4);
    for (const float value : values) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (int shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>((bits >> shift) & 0xFF));
    }
    return bytes;
}

Result<std::vector<FeatureVector>> compute_feature_vectors(const DocumentData& document) {
    std::vector<FeatureVector> vectors;
    for (std::size_t index = 0; index < document.sequences.size(); ++index) {
        const auto& sequence = document.sequences[index];
        const auto summary = music::analyze_sequence(sequence);
        const auto notes = pair_notes(sequence).notes;
        FeatureVector vector;
        vector.sequence_no = static_cast<std::uint32_t>(index);
        vector.values.assign(feature_dimension, 0.0f);
        double total_duration = 0.0;
        double velocity_sum = 0.0;
        double pitch_sum = 0.0;
        std::size_t drums = 0;
        for (const auto& note : notes) {
            const auto weight = static_cast<double>(std::max<Tick>(1, note.duration()));
            vector.values[note.pitch % 12] += static_cast<float>(weight);
            total_duration += weight;
            velocity_sum += note.velocity;
            pitch_sum += note.pitch;
            if (note.channel == drum_channel)
                ++drums;
        }
        if (total_duration > 0)
            for (std::size_t pitch_class = 0; pitch_class < 12; ++pitch_class)
                vector.values[pitch_class] = static_cast<float>(vector.values[pitch_class] / total_duration);
        auto streams = search::extract_motif_streams(sequence, static_cast<std::uint32_t>(index));
        if (!streams)
            return std::unexpected(std::move(streams).error());
        std::size_t interval_total = 0;
        for (const auto& stream : *streams) {
            for (std::size_t note = 1; note < stream.notes.size(); ++note) {
                const auto interval =
                    std::clamp(static_cast<int>(stream.notes[note].pitch) - stream.notes[note - 1].pitch, -6, 6);
                vector.values[static_cast<std::size_t>(12 + interval + 6)] += 1.0f;
                ++interval_total;
            }
        }
        if (interval_total > 0)
            for (std::size_t bin = 12; bin < 25; ++bin)
                vector.values[bin] /= static_cast<float>(interval_total);
        const auto count = static_cast<double>(notes.size());
        if (count > 0) {
            vector.values[25] = static_cast<float>(std::min(summary.note_density / 16.0, 1.0));
            vector.values[26] = static_cast<float>(velocity_sum / count / 127.0);
            vector.values[28] = static_cast<float>((summary.max_pitch.value_or(0) - summary.min_pitch.value_or(0)) / 127.0);
            vector.values[29] = static_cast<float>(pitch_sum / count / 127.0);
            vector.values[30] = static_cast<float>(static_cast<double>(drums) / count);
        }
        vector.values[27] = static_cast<float>(std::min(static_cast<double>(summary.max_polyphony) / 16.0, 1.0));
        if (summary.tempo_representative)
            vector.values[31] = static_cast<float>(std::clamp((*summary.tempo_representative - 40.0) / 200.0, 0.0, 1.0));
        for (auto& value : vector.values)
            if (!std::isfinite(value))
                value = 0.0f;
        vectors.push_back(std::move(vector));
    }
    return vectors;
}

Result<std::vector<search::MotifStream>> compute_motif_streams(const DocumentData& document) {
    std::vector<search::MotifStream> streams;
    for (std::size_t index = 0; index < document.sequences.size(); ++index) {
        auto extracted = search::extract_motif_streams(document.sequences[index], static_cast<std::uint32_t>(index));
        if (!extracted)
            return std::unexpected(std::move(extracted).error());
        for (auto& stream : *extracted)
            streams.push_back(std::move(stream));
    }
    return streams;
}

} // namespace mwb::library
