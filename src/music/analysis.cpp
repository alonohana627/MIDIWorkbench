#include "mwb/music/analysis.hpp"

#include "mwb/core/tempo_map.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <tuple>

namespace mwb::music {

namespace {

/// Krumhansl-Kessler probe-tone profiles (Krumhansl 1990), indexed from the tonic.
constexpr std::array<double, 12> krumhansl_major{6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr std::array<double, 12> krumhansl_minor{6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

struct ModeDefinition {
    std::string_view name;
    std::array<int, 7> offsets;
};

/// Church modes for the MWB mode profiles v1. Rotations of the major profile would be identical
/// to the relative major, so these profiles emphasize the modal tonic: tonic 6.0, fifth 5.0,
/// third 4.5, other members 3.5, non-members 2.0.
constexpr std::array<ModeDefinition, 5> church_modes{{
    {"dorian", {0, 2, 3, 5, 7, 9, 10}},
    {"phrygian", {0, 1, 3, 5, 7, 8, 10}},
    {"lydian", {0, 2, 4, 6, 7, 9, 11}},
    {"mixolydian", {0, 2, 4, 5, 7, 9, 10}},
    {"locrian", {0, 1, 3, 5, 6, 8, 10}},
}};

/// Builds the MWB mode profile v1 weights for one church mode.
std::array<double, 12> mode_profile(const ModeDefinition& mode) {
    std::array<double, 12> profile{};
    profile.fill(2.0);
    for (const int offset : mode.offsets)
        profile[static_cast<std::size_t>(offset)] = 3.5;
    profile[0] = 6.0;
    const bool has_fifth = std::ranges::find(mode.offsets, 7) != mode.offsets.end();
    profile[has_fifth ? 7 : 6] = 5.0;
    const bool has_major_third = std::ranges::find(mode.offsets, 4) != mode.offsets.end();
    profile[has_major_third ? 4 : 3] = 4.5;
    return profile;
}

/// Pearson correlation of a histogram with a profile rotated so its tonic lies on `root`.
double correlation(const std::array<double, 12>& histogram, const std::array<double, 12>& profile, int root) {
    double mean_h = 0;
    double mean_p = 0;
    for (std::size_t index = 0; index < 12; ++index) {
        mean_h += histogram[index];
        mean_p += profile[index];
    }
    mean_h /= 12.0;
    mean_p /= 12.0;
    double covariance = 0;
    double variance_h = 0;
    double variance_p = 0;
    for (int pitch_class = 0; pitch_class < 12; ++pitch_class) {
        const double h = histogram[static_cast<std::size_t>(pitch_class)] - mean_h;
        const double p = profile[static_cast<std::size_t>(((pitch_class - root) % 12 + 12) % 12)] - mean_p;
        covariance += h * p;
        variance_h += h * h;
        variance_p += p * p;
    }
    if (variance_h <= 0 || variance_p <= 0)
        return 0;
    return covariance / std::sqrt(variance_h * variance_p);
}

/// Quarter-note BPM of a tempo in microseconds per quarter (display only).
double bpm(std::int64_t microseconds_per_quarter) {
    return 60'000'000.0 / static_cast<double>(microseconds_per_quarter);
}

/// Tonic pitch class of an SMF key signature (sharps/flats count and mode).
int key_signature_root(int sharps_flats, bool minor) {
    const int major_root = ((7 * sharps_flats) % 12 + 12) % 12;
    return minor ? (major_root + 9) % 12 : major_root;
}

} // namespace

std::size_t max_key_down_polyphony(std::span<const NoteSpan> notes) {
    // Event list: (tick, kind) with kind 0 = end, 1 = start, 2 = zero-length note at its tick.
    std::vector<std::pair<Tick, int>> points;
    points.reserve(notes.size() * 2);
    for (const auto& note : notes) {
        if (note.end <= note.start) {
            points.emplace_back(note.start, 2);
            continue;
        }
        points.emplace_back(note.start, 1);
        points.emplace_back(note.end, 0);
    }
    std::ranges::sort(points);
    std::size_t current = 0;
    std::size_t peak = 0;
    std::size_t index = 0;
    while (index < points.size()) {
        const Tick tick = points[index].first;
        std::size_t momentary = 0;
        for (; index < points.size() && points[index].first == tick; ++index) {
            switch (points[index].second) {
            case 0: --current; break;
            case 1: ++current; break;
            default: ++momentary; break;
            }
        }
        peak = std::max(peak, current + momentary);
    }
    return peak;
}

std::array<double, 12> pitch_class_histogram(std::span<const NoteSpan> notes, const KeyEstimationOptions& options) {
    std::array<double, 12> histogram{};
    const double ppq = options.ppq > 0 ? static_cast<double>(options.ppq) : 1.0;
    const double bound = options.duration_bound.to_double();
    for (const auto& note : notes) {
        double weight = std::min(static_cast<double>(note.end - note.start) / ppq, bound);
        if (weight <= 0)
            continue;
        if (options.weight_by_velocity)
            weight *= static_cast<double>(note.velocity) / 127.0;
        histogram[note.pitch % 12] += weight;
    }
    return histogram;
}

KeyEstimate estimate_key(std::span<const NoteSpan> notes, const KeyEstimationOptions& options) {
    KeyEstimate estimate;
    estimate.histogram = pitch_class_histogram(notes, options);
    const bool modes = options.profiles == KeyProfileSet::major_minor_and_modes;
    estimate.profile_name = modes ? "krumhansl_kessler+mwb_modes" : "krumhansl_kessler";
    estimate.profile_version = 1;

    struct Profile {
        std::string_view mode;
        std::array<double, 12> weights;
    };
    std::vector<Profile> profiles{{"major", krumhansl_major}, {"minor", krumhansl_minor}};
    if (modes)
        for (const auto& mode : church_modes)
            profiles.push_back({mode.name, mode_profile(mode)});

    std::vector<std::tuple<double, std::size_t, int>> scored; // correlation, profile, root
    for (std::size_t profile = 0; profile < profiles.size(); ++profile)
        for (int root = 0; root < 12; ++root)
            scored.emplace_back(correlation(estimate.histogram, profiles[profile].weights, root), profile, root);
    std::ranges::stable_sort(scored, [](const auto& a, const auto& b) {
        if (std::get<0>(a) != std::get<0>(b))
            return std::get<0>(a) > std::get<0>(b);
        return std::tie(std::get<1>(a), std::get<2>(a)) < std::tie(std::get<1>(b), std::get<2>(b));
    });
    for (std::size_t index = 0; index < scored.size() && index < options.alternatives + 1; ++index) {
        const auto& [value, profile, root] = scored[index];
        estimate.alternatives.push_back(KeyCandidate{root, std::string(profiles[profile].mode), value});
    }

    int distinct = 0;
    double total = 0;
    for (const double weight : estimate.histogram) {
        distinct += weight > 0 ? 1 : 0;
        total += weight;
    }
    const double best = std::get<0>(scored.front());
    estimate.margin = scored.size() > 1 ? best - std::get<0>(scored[1]) : best;
    if (notes.size() < options.minimum_notes || distinct < options.minimum_pitch_classes || total <= 0 ||
        best < options.minimum_correlation) {
        estimate.unknown = true;
        estimate.confidence = 0;
        return estimate;
    }
    estimate.unknown = false;
    estimate.best = estimate.alternatives.front();
    estimate.confidence =
        std::clamp(best, 0.0, 1.0) * (0.5 + 0.5 * std::clamp(estimate.margin / 0.1, 0.0, 1.0));
    return estimate;
}

std::string_view tempo_origin_name(TempoOrigin origin) noexcept {
    switch (origin) {
    case TempoOrigin::explicit_tempo: return "explicit";
    case TempoOrigin::default_tempo: return "default";
    case TempoOrigin::reference: return "reference";
    case TempoOrigin::unknown: return "unknown";
    }
    return "unknown";
}

std::string_view key_origin_name(KeyOrigin origin) noexcept {
    switch (origin) {
    case KeyOrigin::manual: return "manual";
    case KeyOrigin::signature: return "signature";
    case KeyOrigin::estimated: return "estimated";
    case KeyOrigin::unknown: return "unknown";
    }
    return "unknown";
}

std::string TrackSummary::program_json() const {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& program : programs) {
        list.push_back(nlohmann::json{
            {"channel", program.channel},
            {"program", program.program},
            {"bank_msb", program.bank_msb ? nlohmann::json(*program.bank_msb) : nlohmann::json(nullptr)},
            {"bank_lsb", program.bank_lsb ? nlohmann::json(*program.bank_lsb) : nlohmann::json(nullptr)},
            {"tick", program.tick}});
    }
    return list.dump();
}

std::string TrackSummary::feature_json() const {
    nlohmann::json features;
    features["version"] = analysis_algorithm_version;
    features["min_pitch"] = min_pitch ? nlohmann::json(*min_pitch) : nlohmann::json(nullptr);
    features["max_pitch"] = max_pitch ? nlohmann::json(*max_pitch) : nlohmann::json(nullptr);
    features["max_polyphony"] = max_polyphony;
    features["is_drum"] = is_drum;
    features["drum_candidate"] = drum_candidate;
    return features.dump();
}

std::string SequenceSummary::diagnostics_json() const {
    nlohmann::json root;
    root["version"] = analysis_version;
    nlohmann::json meter_list = nlohmann::json::array();
    for (const auto& meter : meters)
        meter_list.push_back({{"tick", meter.tick}, {"numerator", meter.numerator}, {"denominator", meter.denominator}});
    root["meters"] = std::move(meter_list);
    nlohmann::json key_list = nlohmann::json::array();
    for (const auto& key : key_signatures)
        key_list.push_back({{"tick", key.tick}, {"sharps_flats", key.sharps_flats}, {"minor", key.minor}});
    root["key_signatures"] = std::move(key_list);
    root["has_lyrics"] = has_lyrics;
    root["has_markers"] = has_markers;
    root["has_sysex"] = has_sysex;
    root["channel_mask"] = channel_mask;
    root["tempo_change_count"] = tempo_change_count;
    root["note_density"] = note_density;
    root["orphan_note_ends"] = orphan_note_ends;
    root["ambiguous_notes"] = ambiguous_notes;
    nlohmann::json estimate;
    estimate["profile"] = key_estimate.profile_name;
    estimate["profile_version"] = key_estimate.profile_version;
    estimate["unknown"] = key_estimate.unknown;
    estimate["confidence"] = key_estimate.confidence;
    estimate["margin"] = key_estimate.margin;
    nlohmann::json alternatives = nlohmann::json::array();
    for (const auto& candidate : key_estimate.alternatives)
        alternatives.push_back(
            {{"root", candidate.root_pitch_class}, {"mode", candidate.mode}, {"correlation", candidate.correlation}});
    estimate["alternatives"] = std::move(alternatives);
    root["key_estimate"] = std::move(estimate);
    root["warnings"] = warnings;
    return root.dump();
}

std::int64_t representative_tempo(const Sequence& sequence, Tick end_tick) {
    const TempoMap map = TempoMap::build(sequence);
    const auto& segments = map.segments();
    std::vector<std::pair<std::int64_t, Tick>> weighted; // microseconds per quarter, ticks
    for (std::size_t index = 0; index < segments.size(); ++index) {
        const Tick begin = std::max<Tick>(segments[index].tick, 0);
        const Tick end = index + 1 < segments.size() ? std::min(segments[index + 1].tick, end_tick) : end_tick;
        if (end > begin)
            weighted.emplace_back(segments[index].microseconds_per_quarter, end - begin);
    }
    if (weighted.empty())
        return map.tempo_at(0);
    // Ascending BPM = descending microseconds; the first value reaching half the weight wins, so
    // an exact tie resolves to the slower tempo.
    std::ranges::stable_sort(weighted, [](const auto& a, const auto& b) { return a.first > b.first; });
    Tick total = 0;
    for (const auto& [tempo, weight] : weighted)
        total += weight;
    Tick cumulative = 0;
    for (const auto& [tempo, weight] : weighted) {
        cumulative += weight;
        if (2 * cumulative >= total)
            return tempo;
    }
    return weighted.back().first;
}

SequenceSummary analyze_sequence(const Sequence& sequence, const AnalysisOptions& options) {
    SequenceSummary summary;
    const bool ppq = sequence.division.is_ppq();
    summary.division = ppq ? "ppq" : "smpte";
    const Tick end = sequence.effective_end();
    const TempoMap tempo_map = TempoMap::build(sequence);
    summary.duration_seconds = tempo_map.tick_to_seconds(end);
    summary.track_count = sequence.tracks.size();

    const NotePairing pairing = pair_notes(sequence);
    summary.note_count = pairing.notes.size();
    summary.orphan_note_ends = pairing.orphan_ends.size();
    summary.ambiguous_notes = pairing.ambiguous_count;
    summary.max_polyphony = max_key_down_polyphony(pairing.notes);

    // Tempo (section 27.1).
    bool has_tempo_events = false;
    for (const auto& track : sequence.tracks) {
        summary.event_count += track.events.size();
        for (const auto& event : track.events)
            has_tempo_events = has_tempo_events || event.is_meta(meta_type::tempo);
    }
    if (ppq) {
        summary.quarter_length = static_cast<double>(end) / static_cast<double>(sequence.division.ppq);
        std::int64_t slowest = tempo_map.tempo_at(0);
        std::int64_t fastest = slowest;
        const auto& segments = tempo_map.segments();
        for (std::size_t index = 0; index < segments.size(); ++index) {
            const bool reached = segments[index].tick < end || (index == 0);
            if (!reached)
                continue;
            slowest = std::max(slowest, segments[index].microseconds_per_quarter);
            fastest = std::min(fastest, segments[index].microseconds_per_quarter);
        }
        summary.tempo_min = bpm(slowest);
        summary.tempo_max = bpm(fastest);
        summary.tempo_representative = bpm(representative_tempo(sequence, end));
        summary.tempo_change_count = tempo_map.change_count();
        summary.tempo_origin =
            has_tempo_events && summary.note_count > 0 ? TempoOrigin::explicit_tempo : TempoOrigin::default_tempo;
        if (tempo_map.has_conflicts())
            summary.warnings.push_back("conflicting tempo events at the same tick; the last in merged order is used");
    } else {
        summary.tempo_origin = TempoOrigin::unknown;
    }

    // Per-track facts, pitch range, events of interest.
    std::vector<NoteSpan> key_notes;
    for (std::size_t track_index = 0; track_index < sequence.tracks.size(); ++track_index) {
        const auto& track = sequence.tracks[track_index];
        TrackSummary row;
        row.track_no = track_index;
        row.name = track.name;
        row.is_drum = track.is_drum;
        std::array<std::optional<std::uint8_t>, 16> bank_msb;
        std::array<std::optional<std::uint8_t>, 16> bank_lsb;
        for (const auto& event : track.events) {
            switch (event.category) {
            case EventCategory::channel: {
                row.channel_mask = static_cast<std::uint16_t>(row.channel_mask | (1u << event.channel()));
                if (event.message() == ChannelMessage::control_change && event.data1() == 0)
                    bank_msb[event.channel()] = event.data2();
                if (event.message() == ChannelMessage::control_change && event.data1() == 32)
                    bank_lsb[event.channel()] = event.data2();
                if (event.message() == ChannelMessage::program_change)
                    row.programs.push_back(ProgramUse{event.channel(), event.data1(), bank_msb[event.channel()],
                                                      bank_lsb[event.channel()], event.tick});
                break;
            }
            case EventCategory::meta:
                if (event.code == meta_type::lyric)
                    summary.has_lyrics = true;
                if (event.code == meta_type::marker)
                    summary.has_markers = true;
                if (event.code == meta_type::time_signature && event.payload.size() >= 2)
                    summary.meters.push_back(MeterChange{event.tick, event.payload[0], 1 << std::min<int>(event.payload[1], 30)});
                if (event.code == meta_type::key_signature && event.payload.size() >= 2)
                    summary.key_signatures.push_back(
                        KeySignature{event.tick, static_cast<std::int8_t>(event.payload[0]), event.payload[1] != 0});
                break;
            case EventCategory::sysex_f0:
            case EventCategory::sysex_f7: summary.has_sysex = true; break;
            }
        }
        std::vector<NoteSpan> track_notes;
        for (const auto& note : pairing.notes)
            if (note.on_track == track_index)
                track_notes.push_back(note);
        row.note_count = track_notes.size();
        bool all_channel_ten = !track_notes.empty();
        for (const auto& note : track_notes) {
            row.min_pitch = std::min(row.min_pitch.value_or(127), static_cast<int>(note.pitch));
            row.max_pitch = std::max(row.max_pitch.value_or(0), static_cast<int>(note.pitch));
            all_channel_ten = all_channel_ten && note.channel == 9;
            if (!track.is_drum || options.include_drums_in_key)
                key_notes.push_back(note);
        }
        row.max_polyphony = max_key_down_polyphony(track_notes);
        row.drum_candidate = track.is_drum || all_channel_ten;
        summary.has_drums = summary.has_drums || row.drum_candidate;
        summary.channel_mask = static_cast<std::uint16_t>(summary.channel_mask | row.channel_mask);
        if (row.min_pitch)
            summary.min_pitch = std::min(summary.min_pitch.value_or(127), *row.min_pitch);
        if (row.max_pitch)
            summary.max_pitch = std::max(summary.max_pitch.value_or(0), *row.max_pitch);
        summary.tracks.push_back(std::move(row));
    }
    std::ranges::stable_sort(summary.meters, [](const MeterChange& a, const MeterChange& b) { return a.tick < b.tick; });
    std::ranges::stable_sort(summary.key_signatures,
                             [](const KeySignature& a, const KeySignature& b) { return a.tick < b.tick; });
    if (summary.duration_seconds > 0)
        summary.note_density = static_cast<double>(summary.note_count) / summary.duration_seconds;
    if (summary.orphan_note_ends > 0)
        summary.warnings.push_back(std::format("{} note ends without a matching note start", summary.orphan_note_ends));
    if (summary.ambiguous_notes > 0)
        summary.warnings.push_back(std::format("{} notes overlap an unfinished note of the same key", summary.ambiguous_notes));

    // Key (section 27.2): manual > imported signature > estimate; the estimate is always kept.
    KeyEstimationOptions key_options = options.key;
    key_options.ppq = ppq ? sequence.division.ppq : 1;
    summary.key_estimate = estimate_key(key_notes, key_options);
    if (options.manual_key) {
        summary.key_origin = KeyOrigin::manual;
        summary.key_root = options.manual_key->first;
        summary.key_mode = options.manual_key->second;
    } else if (!summary.key_signatures.empty()) {
        const auto& signature = summary.key_signatures.front();
        summary.key_origin = KeyOrigin::signature;
        summary.key_root = key_signature_root(signature.sharps_flats, signature.minor);
        summary.key_mode = signature.minor ? "minor" : "major";
    } else if (!summary.key_estimate.unknown && summary.key_estimate.best) {
        summary.key_origin = KeyOrigin::estimated;
        summary.key_root = summary.key_estimate.best->root_pitch_class;
        summary.key_mode = summary.key_estimate.best->mode;
        summary.key_confidence = summary.key_estimate.confidence;
    } else {
        summary.key_origin = KeyOrigin::unknown;
    }
    return summary;
}

} // namespace mwb::music
