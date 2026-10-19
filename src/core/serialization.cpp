#include "mwb/core/serialization.hpp"

#include <nlohmann/json.hpp>

namespace mwb {

using nlohmann::json;

namespace {

Result<json> parse_object(const std::string& text, const char* what) {
    json value = json::parse(text, nullptr, false);
    if (value.is_discarded() || !value.is_object())
        return make_error(ErrorCode::invalid_data, std::string(what) + " is not a JSON object");
    const auto version = value.find("version");
    if (version == value.end() || !version->is_number_integer() || version->get<int>() != 1)
        return make_error(ErrorCode::unsupported, std::string(what) + " has an unsupported version");
    return value;
}

template <class T>
T value_or(const json& object, const char* key, T fallback) {
    const auto found = object.find(key);
    if (found == object.end() || found->is_null())
        return fallback;
    if constexpr (std::is_same_v<T, bool>) {
        return found->is_boolean() ? found->get<bool>() : fallback;
    } else if constexpr (std::is_integral_v<T>) {
        return found->is_number_integer() ? static_cast<T>(found->get<std::int64_t>()) : fallback;
    } else {
        return found->is_string() ? found->get<std::string>() : fallback;
    }
}

const char* policy_name(InputScalePolicy policy) {
    switch (policy) {
    case InputScalePolicy::allow_all: return "allow_all";
    case InputScalePolicy::reject_out_of_scale: return "reject";
    case InputScalePolicy::nearest_member: return "nearest";
    case InputScalePolicy::diatonic_keyboard: return "diatonic";
    }
    return "allow_all";
}

InputScalePolicy policy_from(const std::string& name) {
    if (name == "reject")
        return InputScalePolicy::reject_out_of_scale;
    if (name == "nearest")
        return InputScalePolicy::nearest_member;
    if (name == "diatonic")
        return InputScalePolicy::diatonic_keyboard;
    return InputScalePolicy::allow_all;
}

const char* output_name(OutputKind kind) {
    switch (kind) {
    case OutputKind::internal_synth: return "internal";
    case OutputKind::soundfont: return "soundfont";
    case OutputKind::external_midi: return "external";
    }
    return "internal";
}

} // namespace

std::string sequence_settings_to_json(const SequenceSettings& settings) {
    json value = {
        {"version", 1},
        {"snap", settings.snap.to_string()},
        {"loop_region", settings.loop_region ? json(settings.loop_region->to_string()) : json(nullptr)},
        {"highlight_scale", settings.highlight_scale},
        {"fold_to_scale", settings.fold_to_scale},
        {"input_scale_policy", policy_name(settings.input_policy)},
        {"nearest_tie_up", settings.nearest_tie_up},
        {"default_velocity", settings.default_velocity},
        {"default_note_length", settings.default_note_length.to_string()},
    };
    return value.dump();
}

Result<SequenceSettings> sequence_settings_from_json(const std::string& text) {
    auto parsed = parse_object(text, "sequence settings");
    if (!parsed)
        return std::unexpected(parsed.error());
    const json& value = *parsed;
    SequenceSettings settings;
    if (auto snap = Rational::parse(value_or<std::string>(value, "snap", "1/16")); snap && snap->numerator > 0)
        settings.snap = *snap;
    if (auto length = Rational::parse(value_or<std::string>(value, "default_note_length", "1/16"));
        length && length->numerator > 0)
        settings.default_note_length = *length;
    const std::string loop = value_or<std::string>(value, "loop_region", "");
    if (!loop.empty()) {
        auto id = Uuid::parse(loop);
        if (!id)
            return std::unexpected(id.error());
        settings.loop_region = RegionId(*id);
    }
    settings.highlight_scale = value_or(value, "highlight_scale", false);
    settings.fold_to_scale = value_or(value, "fold_to_scale", false);
    settings.input_policy = policy_from(value_or<std::string>(value, "input_scale_policy", "allow_all"));
    settings.nearest_tie_up = value_or(value, "nearest_tie_up", false);
    settings.default_velocity = static_cast<std::uint8_t>(std::clamp(value_or<int>(value, "default_velocity", 100), 1, 127));
    return settings;
}

std::string routing_to_json(const RoutingSettings& routing) {
    json value = {
        {"version", 1},
        {"output", output_name(routing.output)},
        {"endpoint", routing.endpoint_id ? json(*routing.endpoint_id) : json(nullptr)},
        {"channel_override", routing.channel_override ? json(*routing.channel_override) : json(nullptr)},
        {"transpose", routing.transpose},
        {"send_sysex", routing.send_sysex},
    };
    return value.dump();
}

Result<RoutingSettings> routing_from_json(const std::string& text) {
    auto parsed = parse_object(text, "track routing");
    if (!parsed)
        return std::unexpected(parsed.error());
    const json& value = *parsed;
    RoutingSettings routing;
    const std::string output = value_or<std::string>(value, "output", "internal");
    routing.output = output == "soundfont"  ? OutputKind::soundfont
                     : output == "external" ? OutputKind::external_midi
                                            : OutputKind::internal_synth;
    const std::string endpoint = value_or<std::string>(value, "endpoint", "");
    if (!endpoint.empty())
        routing.endpoint_id = endpoint;
    const int channel = value_or<int>(value, "channel_override", -1);
    if (channel >= 0 && channel <= 15)
        routing.channel_override = static_cast<std::uint8_t>(channel);
    routing.transpose = std::clamp(value_or<int>(value, "transpose", 0), -127, 127);
    routing.send_sysex = value_or(value, "send_sysex", false);
    return routing;
}

std::string instrument_to_json(const InstrumentSettings& instrument) {
    json value = {
        {"version", 1},
        {"backend", instrument.backend},
        {"bank_sha256", instrument.bank_sha256 ? json(*instrument.bank_sha256) : json(nullptr)},
        {"bank", instrument.bank},
        {"program", instrument.program},
        {"synth_preset", instrument.synth_preset},
    };
    return value.dump();
}

Result<InstrumentSettings> instrument_from_json(const std::string& text) {
    auto parsed = parse_object(text, "track instrument");
    if (!parsed)
        return std::unexpected(parsed.error());
    const json& value = *parsed;
    InstrumentSettings instrument;
    instrument.backend = value_or<std::string>(value, "backend", "builtin") == "sf2" ? "sf2" : "builtin";
    const std::string bank = value_or<std::string>(value, "bank_sha256", "");
    if (!bank.empty())
        instrument.bank_sha256 = bank;
    instrument.bank = static_cast<std::uint16_t>(std::clamp(value_or<int>(value, "bank", 0), 0, 16383));
    instrument.program = static_cast<std::uint8_t>(std::clamp(value_or<int>(value, "program", 0), 0, 127));
    instrument.synth_preset = value_or<std::string>(value, "synth_preset", "classic");
    return instrument;
}

std::string region_tracks_to_json(const std::vector<TrackId>& tracks) {
    json value = json::array();
    for (const auto& track : tracks)
        value.push_back(track.to_string());
    return value.dump();
}

Result<std::vector<TrackId>> region_tracks_from_json(const std::string& text) {
    json value = json::parse(text, nullptr, false);
    if (value.is_discarded() || !value.is_array())
        return make_error(ErrorCode::invalid_data, "region tracks must be a JSON array");
    std::vector<TrackId> tracks;
    for (const auto& item : value) {
        if (!item.is_string())
            return make_error(ErrorCode::invalid_data, "region track entry must be a string");
        auto id = Uuid::parse(item.get<std::string>());
        if (!id)
            return std::unexpected(id.error());
        tracks.emplace_back(*id);
    }
    return tracks;
}

// --- Semantic hash ------------------------------------------------------------------------------

namespace {

void hash_uuid(Sha256Builder& hasher, const Uuid& id) { hasher.update(std::span<const std::uint8_t>(id.bytes())); }

void hash_event(Sha256Builder& hasher, const Event& event) {
    hash_uuid(hasher, event.id.uuid());
    hasher.update_i64(event.tick);
    hasher.update_u64(event.order_key);
    hasher.update_u8(static_cast<std::uint8_t>(event.category));
    hasher.update_u8(event.code);
    hasher.update_u32(event.flags);
    hasher.update_u64(event.payload.size());
    hasher.update(event.payload.bytes());
}

void hash_sequence(Sha256Builder& hasher, const Sequence& sequence) {
    hash_uuid(hasher, sequence.id.uuid());
    hasher.update_string(sequence.name);
    hasher.update_u8(static_cast<std::uint8_t>(sequence.division.kind));
    hasher.update_i64(sequence.division.ppq);
    hasher.update_i64(static_cast<std::int64_t>(sequence.division.smpte_format));
    hasher.update_i64(sequence.division.ticks_per_frame);
    hasher.update_i64(sequence.end_tick);
    hasher.update_u8(sequence.root_pc);
    hasher.update_u32(sequence.scale_mask);
    hasher.update_string(sequence_settings_to_json(sequence.settings));
    hasher.update_u64(sequence.tracks.size());
    for (const auto& track : sequence.tracks) {
        hash_uuid(hasher, track.id.uuid());
        hasher.update_string(track.name);
        hasher.update_u32(track.color_rgba);
        hasher.update_i64(track.end_tick);
        hasher.update_u8(static_cast<std::uint8_t>((track.muted ? 1 : 0) | (track.solo ? 2 : 0) | (track.is_drum ? 4 : 0)));
        hasher.update_string(routing_to_json(track.routing));
        hasher.update_string(instrument_to_json(track.instrument));
        hasher.update_u64(track.events.size());
        for (const auto& event : track.events)
            hash_event(hasher, event);
    }
    hasher.update_u64(sequence.regions.size());
    for (const auto& region : sequence.regions) {
        hash_uuid(hasher, region.id.uuid());
        hasher.update_string(region.name);
        hasher.update_i64(region.start_tick);
        hasher.update_i64(region.end_tick);
        hasher.update_string(region_tracks_to_json(region.tracks));
        hasher.update_u8(region.is_loop ? 1 : 0);
    }
    hasher.update_u64(sequence.scale_regions.size());
    for (const auto& region : sequence.scale_regions) {
        hash_uuid(hasher, region.id.uuid());
        hasher.update_i64(region.start_tick);
        hasher.update_i64(region.end_tick);
        hasher.update_u8(region.root_pc);
        hasher.update_u32(region.scale_mask);
        hasher.update_string(region.scale_name);
    }
    hasher.update_u64(sequence.recipes.size());
    for (const auto& recipe : sequence.recipes) {
        hash_uuid(hasher, recipe.id.uuid());
        hasher.update_string(recipe.tool_id);
        hasher.update_i64(recipe.algorithm_version);
        hasher.update(std::span<const std::uint8_t>(recipe.source_semantic_hash));
        hasher.update_string(recipe.parameters_json);
        hasher.update_string(recipe.targets_json);
    }
}

} // namespace

Sha256Digest semantic_hash(const Sequence& sequence) {
    Sha256Builder hasher;
    hasher.update_string("mwb.sequence.v1");
    hash_sequence(hasher, sequence);
    return hasher.finish();
}

Sha256Digest semantic_hash(const DocumentData& document) {
    Sha256Builder hasher;
    hasher.update_string("mwb.document.v1");
    hash_uuid(hasher, document.id.uuid());
    hasher.update_string(document.title);
    hasher.update_string(document.metadata_json);
    hasher.update_u64(document.sequences.size());
    for (const auto& sequence : document.sequences)
        hash_sequence(hasher, sequence);
    hasher.update_u64(document.takes.size());
    for (const auto& take : document.takes) {
        hash_uuid(hasher, take.id.uuid());
        hash_uuid(hasher, take.track.uuid());
        hasher.update_string(take.name);
        hasher.update_i64(take.start_tick);
        hasher.update_i64(take.end_tick);
        hasher.update_u8(static_cast<std::uint8_t>(take.status));
        hasher.update_string(take.capture_json);
        hasher.update_u64(take.chunks.size());
        for (const auto& chunk : take.chunks) {
            hasher.update_i64(chunk.ordinal);
            hasher.update_i64(chunk.first_time_ns);
            hasher.update_u64(chunk.payload.size());
            hasher.update(chunk.payload);
        }
    }
    hasher.update_u64(document.settings.size());
    for (const auto& [key, value] : document.settings) {
        hasher.update_string(key);
        hasher.update_string(value);
    }
    return hasher.finish();
}

} // namespace mwb
