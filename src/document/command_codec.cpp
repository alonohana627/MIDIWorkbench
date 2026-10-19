#include "mwb/document/command_codec.hpp"

#include "mwb/core/utility.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace mwb::document {

namespace {

using nlohmann::json;

/// Evaluates a Result expression, propagating its error, else binds the value.
#define MWB_CODEC_ASSIGN(target, expression)                                                                          \
    auto target##_result_ = (expression);                                                                             \
    if (!target##_result_)                                                                                            \
        return std::unexpected(std::move(target##_result_).error());                                                  \
    auto target = std::move(*target##_result_)

// --- Encoding -----------------------------------------------------------------------------------

json bytes_value(std::span<const std::uint8_t> bytes) {
    return json::binary(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
}

json uuid_value(const Uuid& identity) { return bytes_value(identity.bytes()); }

json rational_value(const Rational& value) { return json::array({value.numerator, value.denominator}); }

json encode_event(const Event& event) {
    return json{
        {"id", uuid_value(event.id.uuid())},
        {"tick", event.tick},
        {"order", event.order_key},
        {"category", static_cast<std::uint8_t>(event.category)},
        {"code", event.code},
        {"flags", event.flags},
        {"payload", bytes_value(event.payload.bytes())},
    };
}

json encode_track(const Track& track) {
    json events = json::array();
    for (const auto& event : track.events)
        events.push_back(encode_event(event));
    const auto& routing = track.routing;
    const auto& instrument = track.instrument;
    return json{
        {"id", uuid_value(track.id.uuid())},
        {"name", track.name},
        {"color", track.color_rgba},
        {"end", track.end_tick},
        {"muted", track.muted},
        {"solo", track.solo},
        {"drum", track.is_drum},
        {"routing",
         json{
             {"output", static_cast<std::uint8_t>(routing.output)},
             {"endpoint", routing.endpoint_id ? json(*routing.endpoint_id) : json(nullptr)},
             {"channel", routing.channel_override ? json(*routing.channel_override) : json(nullptr)},
             {"transpose", routing.transpose},
             {"sysex", routing.send_sysex},
         }},
        {"instrument",
         json{
             {"backend", instrument.backend},
             {"bank_sha256", instrument.bank_sha256 ? json(*instrument.bank_sha256) : json(nullptr)},
             {"bank", instrument.bank},
             {"program", instrument.program},
             {"preset", instrument.synth_preset},
         }},
        {"events", std::move(events)},
    };
}

json encode_sequence(const Sequence& sequence) {
    json tracks = json::array();
    for (const auto& track : sequence.tracks)
        tracks.push_back(encode_track(track));
    json regions = json::array();
    for (const auto& region : sequence.regions) {
        json region_tracks = json::array();
        for (const auto& track : region.tracks)
            region_tracks.push_back(uuid_value(track.uuid()));
        regions.push_back(json{{"id", uuid_value(region.id.uuid())},
                               {"name", region.name},
                               {"start", region.start_tick},
                               {"end", region.end_tick},
                               {"tracks", std::move(region_tracks)},
                               {"loop", region.is_loop}});
    }
    json scales = json::array();
    for (const auto& region : sequence.scale_regions)
        scales.push_back(json{{"id", uuid_value(region.id.uuid())},
                              {"start", region.start_tick},
                              {"end", region.end_tick},
                              {"root", region.root_pc},
                              {"mask", region.scale_mask},
                              {"name", region.scale_name}});
    json recipes = json::array();
    for (const auto& recipe : sequence.recipes)
        recipes.push_back(json{{"id", uuid_value(recipe.id.uuid())},
                               {"tool", recipe.tool_id},
                               {"version", recipe.algorithm_version},
                               {"hash", bytes_value(recipe.source_semantic_hash)},
                               {"parameters", recipe.parameters_json},
                               {"targets", recipe.targets_json}});
    const auto& settings = sequence.settings;
    return json{
        {"id", uuid_value(sequence.id.uuid())},
        {"name", sequence.name},
        {"division",
         json{{"kind", static_cast<std::uint8_t>(sequence.division.kind)},
              {"ppq", sequence.division.ppq},
              {"smpte", static_cast<std::int32_t>(sequence.division.smpte_format)},
              {"tpf", sequence.division.ticks_per_frame}}},
        {"end", sequence.end_tick},
        {"root", sequence.root_pc},
        {"mask", sequence.scale_mask},
        {"settings",
         json{
             {"snap", rational_value(settings.snap)},
             {"loop", settings.loop_region ? uuid_value(settings.loop_region->uuid()) : json(nullptr)},
             {"highlight", settings.highlight_scale},
             {"fold", settings.fold_to_scale},
             {"policy", static_cast<std::uint8_t>(settings.input_policy)},
             {"tie_up", settings.nearest_tie_up},
             {"velocity", settings.default_velocity},
             {"length", rational_value(settings.default_note_length)},
         }},
        {"tracks", std::move(tracks)},
        {"regions", std::move(regions)},
        {"scales", std::move(scales)},
        {"recipes", std::move(recipes)},
    };
}

json encode_take(const RecordingTake& take) {
    json chunks = json::array();
    for (const auto& chunk : take.chunks)
        chunks.push_back(json{{"ordinal", chunk.ordinal}, {"first_ns", chunk.first_time_ns}, {"payload", bytes_value(chunk.payload)}});
    return json{{"id", uuid_value(take.id.uuid())},
                {"track", uuid_value(take.track.uuid())},
                {"name", take.name},
                {"created", take.created_ms},
                {"start", take.start_tick},
                {"end", take.end_tick},
                {"status", static_cast<std::uint8_t>(take.status)},
                {"capture", take.capture_json},
                {"chunks", std::move(chunks)}};
}

json encode_operation(const EditOperation& operation) {
    return std::visit(
        Overloaded{
            [](const EventInsert& insert) {
                return json{{"op", "event_insert"}, {"track", uuid_value(insert.track.uuid())},
                            {"event", encode_event(insert.event)}};
            },
            [](const EventRemove& remove) {
                return json{{"op", "event_remove"}, {"track", uuid_value(remove.track.uuid())},
                            {"event", encode_event(remove.event)}};
            },
            [](const EventReplace& replace) {
                return json{{"op", "event_replace"}, {"track", uuid_value(replace.track.uuid())},
                            {"before", encode_event(replace.before)}, {"after", encode_event(replace.after)}};
            },
            [](const TrackInsert& insert) {
                return json{{"op", "track_insert"}, {"sequence", uuid_value(insert.sequence.uuid())},
                            {"index", static_cast<std::uint64_t>(insert.index)}, {"track", encode_track(insert.track)}};
            },
            [](const TrackRemove& remove) {
                return json{{"op", "track_remove"}, {"sequence", uuid_value(remove.sequence.uuid())},
                            {"index", static_cast<std::uint64_t>(remove.index)}, {"track", encode_track(remove.track)}};
            },
            [](const TrackPropertiesChange& change) {
                return json{{"op", "track_properties"}, {"track", uuid_value(change.track.uuid())},
                            {"before", encode_track(change.before)}, {"after", encode_track(change.after)}};
            },
            [](const TrackMove& move) {
                return json{{"op", "track_move"}, {"sequence", uuid_value(move.sequence.uuid())},
                            {"from", static_cast<std::uint64_t>(move.from)}, {"to", static_cast<std::uint64_t>(move.to)}};
            },
            [](const SequencePropertiesChange& change) {
                return json{{"op", "sequence_properties"}, {"sequence", uuid_value(change.sequence.uuid())},
                            {"before", encode_sequence(change.before)}, {"after", encode_sequence(change.after)}};
            },
            [](const SequenceInsert& insert) {
                return json{{"op", "sequence_insert"}, {"index", static_cast<std::uint64_t>(insert.index)},
                            {"sequence", encode_sequence(insert.sequence)}};
            },
            [](const SequenceRemove& remove) {
                return json{{"op", "sequence_remove"}, {"index", static_cast<std::uint64_t>(remove.index)},
                            {"sequence", encode_sequence(remove.sequence)}};
            },
            [](const DocumentPropertiesChange& change) {
                return json{{"op", "document_properties"},
                            {"title_before", change.title_before},
                            {"title_after", change.title_after},
                            {"metadata_before", change.metadata_before},
                            {"metadata_after", change.metadata_after}};
            },
            [](const TakeInsert& insert) {
                return json{{"op", "take_insert"}, {"index", static_cast<std::uint64_t>(insert.index)},
                            {"take", encode_take(insert.take)}};
            },
            [](const TakeRemove& remove) {
                return json{{"op", "take_remove"}, {"index", static_cast<std::uint64_t>(remove.index)},
                            {"take", encode_take(remove.take)}};
            },
        },
        operation);
}

const char* kind_name(CommandKind kind) {
    switch (kind) {
    case CommandKind::edit: return "edit";
    case CommandKind::undo: return "undo";
    case CommandKind::redo: return "redo";
    }
    return "edit";
}

// --- Decoding -----------------------------------------------------------------------------------

std::unexpected<Error> malformed(std::string_view what) {
    return make_error(ErrorCode::invalid_data, "command envelope: " + std::string(what));
}

Result<const json*> member(const json& object, const char* key) {
    if (!object.is_object())
        return malformed(std::string("expected an object holding '") + key + "'");
    const auto found = object.find(key);
    if (found == object.end())
        return malformed(std::string("missing field '") + key + "'");
    return &*found;
}

Result<std::int64_t> as_signed(const json& value, std::int64_t minimum, std::int64_t maximum, const char* what) {
    std::int64_t result = 0;
    if (value.is_number_unsigned()) {
        const auto unsigned_value = value.get<std::uint64_t>();
        if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return malformed(std::string(what) + " is out of range");
        result = static_cast<std::int64_t>(unsigned_value);
    } else if (value.is_number_integer()) {
        result = value.get<std::int64_t>();
    } else {
        return malformed(std::string(what) + " is not an integer");
    }
    if (result < minimum || result > maximum)
        return malformed(std::string(what) + " is out of range");
    return result;
}

Result<std::uint64_t> as_unsigned(const json& value, std::uint64_t maximum, const char* what) {
    if (value.is_number_unsigned()) {
        const auto result = value.get<std::uint64_t>();
        if (result > maximum)
            return malformed(std::string(what) + " is out of range");
        return result;
    }
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0) {
        const auto result = static_cast<std::uint64_t>(value.get<std::int64_t>());
        if (result > maximum)
            return malformed(std::string(what) + " is out of range");
        return result;
    }
    return malformed(std::string(what) + " is not a non-negative integer");
}

Result<std::int64_t> signed_field(const json& object, const char* key, std::int64_t minimum, std::int64_t maximum) {
    MWB_CODEC_ASSIGN(value, member(object, key));
    return as_signed(*value, minimum, maximum, key);
}

Result<std::uint64_t> unsigned_field(const json& object, const char* key, std::uint64_t maximum) {
    MWB_CODEC_ASSIGN(value, member(object, key));
    return as_unsigned(*value, maximum, key);
}

Result<bool> bool_field(const json& object, const char* key) {
    MWB_CODEC_ASSIGN(value, member(object, key));
    if (!value->is_boolean())
        return malformed(std::string(key) + " is not a boolean");
    return value->get<bool>();
}

Result<std::string> string_field(const json& object, const char* key) {
    MWB_CODEC_ASSIGN(value, member(object, key));
    if (!value->is_string())
        return malformed(std::string(key) + " is not a string");
    return value->get<std::string>();
}

Result<std::optional<std::string>> optional_string_field(const json& object, const char* key) {
    MWB_CODEC_ASSIGN(value, member(object, key));
    if (value->is_null())
        return std::optional<std::string>();
    if (!value->is_string())
        return malformed(std::string(key) + " is not a string or null");
    return std::optional<std::string>(value->get<std::string>());
}

Result<std::vector<std::uint8_t>> as_bytes(const json& value, const char* what) {
    if (!value.is_binary())
        return malformed(std::string(what) + " is not a byte string");
    const auto& binary = value.get_binary();
    return std::vector<std::uint8_t>(binary.begin(), binary.end());
}

Result<std::vector<std::uint8_t>> bytes_field(const json& object, const char* key) {
    MWB_CODEC_ASSIGN(value, member(object, key));
    return as_bytes(*value, key);
}

Result<Uuid> as_uuid(const json& value, const char* what) {
    MWB_CODEC_ASSIGN(bytes, as_bytes(value, what));
    auto identity = Uuid::from_bytes(bytes);
    if (!identity)
        return malformed(std::string(what) + " is not a 16-byte identity");
    return *identity;
}

Result<Uuid> uuid_field(const json& object, const char* key) {
    MWB_CODEC_ASSIGN(value, member(object, key));
    return as_uuid(*value, key);
}

Result<const json*> array_field(const json& object, const char* key) {
    MWB_CODEC_ASSIGN(value, member(object, key));
    if (!value->is_array())
        return malformed(std::string(key) + " is not an array");
    return value;
}

Result<Rational> rational_field(const json& object, const char* key) {
    MWB_CODEC_ASSIGN(value, member(object, key));
    if (!value->is_array() || value->size() != 2)
        return malformed(std::string(key) + " is not a fraction");
    MWB_CODEC_ASSIGN(numerator, as_signed((*value)[0], std::numeric_limits<std::int64_t>::min(),
                                          std::numeric_limits<std::int64_t>::max(), key));
    MWB_CODEC_ASSIGN(denominator, as_signed((*value)[1], 1, std::numeric_limits<std::int64_t>::max(), key));
    return Rational{numerator, denominator};
}

constexpr std::int64_t int64_min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t int64_max = std::numeric_limits<std::int64_t>::max();

Result<Event> decode_event(const json& object) {
    Event event;
    MWB_CODEC_ASSIGN(identity, uuid_field(object, "id"));
    MWB_CODEC_ASSIGN(tick, signed_field(object, "tick", int64_min, int64_max));
    MWB_CODEC_ASSIGN(order, unsigned_field(object, "order", std::numeric_limits<std::uint64_t>::max()));
    MWB_CODEC_ASSIGN(category, unsigned_field(object, "category", 3));
    MWB_CODEC_ASSIGN(code, unsigned_field(object, "code", 255));
    MWB_CODEC_ASSIGN(flags, unsigned_field(object, "flags", 0xFFFFFFFFu));
    MWB_CODEC_ASSIGN(payload, bytes_field(object, "payload"));
    event.id = EventId(identity);
    event.tick = tick;
    event.order_key = order;
    event.category = static_cast<EventCategory>(category);
    event.code = static_cast<std::uint8_t>(code);
    event.flags = static_cast<std::uint32_t>(flags);
    event.payload = Payload(payload);
    return event;
}

Result<Track> decode_track(const json& object) {
    Track track;
    MWB_CODEC_ASSIGN(identity, uuid_field(object, "id"));
    MWB_CODEC_ASSIGN(name, string_field(object, "name"));
    MWB_CODEC_ASSIGN(color, unsigned_field(object, "color", 0xFFFFFFFFu));
    MWB_CODEC_ASSIGN(end, signed_field(object, "end", int64_min, int64_max));
    MWB_CODEC_ASSIGN(muted, bool_field(object, "muted"));
    MWB_CODEC_ASSIGN(solo, bool_field(object, "solo"));
    MWB_CODEC_ASSIGN(drum, bool_field(object, "drum"));
    track.id = TrackId(identity);
    track.name = std::move(name);
    track.color_rgba = static_cast<std::uint32_t>(color);
    track.end_tick = end;
    track.muted = muted;
    track.solo = solo;
    track.is_drum = drum;

    MWB_CODEC_ASSIGN(routing, member(object, "routing"));
    MWB_CODEC_ASSIGN(output, unsigned_field(*routing, "output", 2));
    MWB_CODEC_ASSIGN(endpoint, optional_string_field(*routing, "endpoint"));
    MWB_CODEC_ASSIGN(channel, member(*routing, "channel"));
    MWB_CODEC_ASSIGN(transpose, signed_field(*routing, "transpose", std::numeric_limits<std::int32_t>::min(),
                                             std::numeric_limits<std::int32_t>::max()));
    MWB_CODEC_ASSIGN(sysex, bool_field(*routing, "sysex"));
    track.routing.output = static_cast<OutputKind>(output);
    track.routing.endpoint_id = std::move(endpoint);
    if (!channel->is_null()) {
        MWB_CODEC_ASSIGN(channel_value, as_unsigned(*channel, 255, "channel"));
        track.routing.channel_override = static_cast<std::uint8_t>(channel_value);
    }
    track.routing.transpose = static_cast<std::int32_t>(transpose);
    track.routing.send_sysex = sysex;

    MWB_CODEC_ASSIGN(instrument, member(object, "instrument"));
    MWB_CODEC_ASSIGN(backend, string_field(*instrument, "backend"));
    MWB_CODEC_ASSIGN(bank_hash, optional_string_field(*instrument, "bank_sha256"));
    MWB_CODEC_ASSIGN(bank, unsigned_field(*instrument, "bank", 0xFFFF));
    MWB_CODEC_ASSIGN(program, unsigned_field(*instrument, "program", 255));
    MWB_CODEC_ASSIGN(preset, string_field(*instrument, "preset"));
    track.instrument.backend = std::move(backend);
    track.instrument.bank_sha256 = std::move(bank_hash);
    track.instrument.bank = static_cast<std::uint16_t>(bank);
    track.instrument.program = static_cast<std::uint8_t>(program);
    track.instrument.synth_preset = std::move(preset);

    MWB_CODEC_ASSIGN(events, array_field(object, "events"));
    track.events.reserve(events->size());
    for (const auto& item : *events) {
        MWB_CODEC_ASSIGN(event, decode_event(item));
        track.events.push_back(std::move(event));
    }
    return track;
}

Result<Sequence> decode_sequence(const json& object) {
    Sequence sequence;
    MWB_CODEC_ASSIGN(identity, uuid_field(object, "id"));
    MWB_CODEC_ASSIGN(name, string_field(object, "name"));
    sequence.id = SequenceId(identity);
    sequence.name = std::move(name);

    MWB_CODEC_ASSIGN(division, member(object, "division"));
    MWB_CODEC_ASSIGN(kind, unsigned_field(*division, "kind", 1));
    MWB_CODEC_ASSIGN(ppq, signed_field(*division, "ppq", std::numeric_limits<std::int32_t>::min(),
                                       std::numeric_limits<std::int32_t>::max()));
    MWB_CODEC_ASSIGN(smpte, signed_field(*division, "smpte", -30, -24));
    MWB_CODEC_ASSIGN(ticks_per_frame, signed_field(*division, "tpf", std::numeric_limits<std::int32_t>::min(),
                                                   std::numeric_limits<std::int32_t>::max()));
    if (smpte != -24 && smpte != -25 && smpte != -29 && smpte != -30)
        return malformed("SMPTE format is invalid");
    sequence.division.kind = static_cast<Division::Kind>(kind);
    sequence.division.ppq = static_cast<std::int32_t>(ppq);
    sequence.division.smpte_format = static_cast<SmpteFormat>(static_cast<std::int8_t>(smpte));
    sequence.division.ticks_per_frame = static_cast<std::int32_t>(ticks_per_frame);

    MWB_CODEC_ASSIGN(end, signed_field(object, "end", int64_min, int64_max));
    MWB_CODEC_ASSIGN(root, unsigned_field(object, "root", 255));
    MWB_CODEC_ASSIGN(mask, unsigned_field(object, "mask", 0xFFFF));
    sequence.end_tick = end;
    sequence.root_pc = static_cast<std::uint8_t>(root);
    sequence.scale_mask = static_cast<std::uint16_t>(mask);

    MWB_CODEC_ASSIGN(settings, member(object, "settings"));
    MWB_CODEC_ASSIGN(snap, rational_field(*settings, "snap"));
    MWB_CODEC_ASSIGN(loop, member(*settings, "loop"));
    MWB_CODEC_ASSIGN(highlight, bool_field(*settings, "highlight"));
    MWB_CODEC_ASSIGN(fold, bool_field(*settings, "fold"));
    MWB_CODEC_ASSIGN(policy, unsigned_field(*settings, "policy", 3));
    MWB_CODEC_ASSIGN(tie_up, bool_field(*settings, "tie_up"));
    MWB_CODEC_ASSIGN(velocity, unsigned_field(*settings, "velocity", 255));
    MWB_CODEC_ASSIGN(length, rational_field(*settings, "length"));
    sequence.settings.snap = snap;
    if (!loop->is_null()) {
        MWB_CODEC_ASSIGN(loop_id, as_uuid(*loop, "loop"));
        sequence.settings.loop_region = RegionId(loop_id);
    }
    sequence.settings.highlight_scale = highlight;
    sequence.settings.fold_to_scale = fold;
    sequence.settings.input_policy = static_cast<InputScalePolicy>(policy);
    sequence.settings.nearest_tie_up = tie_up;
    sequence.settings.default_velocity = static_cast<std::uint8_t>(velocity);
    sequence.settings.default_note_length = length;

    MWB_CODEC_ASSIGN(tracks, array_field(object, "tracks"));
    for (const auto& item : *tracks) {
        MWB_CODEC_ASSIGN(track, decode_track(item));
        sequence.tracks.push_back(std::move(track));
    }
    MWB_CODEC_ASSIGN(regions, array_field(object, "regions"));
    for (const auto& item : *regions) {
        Region region;
        MWB_CODEC_ASSIGN(region_id, uuid_field(item, "id"));
        MWB_CODEC_ASSIGN(region_name, string_field(item, "name"));
        MWB_CODEC_ASSIGN(start, signed_field(item, "start", int64_min, int64_max));
        MWB_CODEC_ASSIGN(region_end, signed_field(item, "end", int64_min, int64_max));
        MWB_CODEC_ASSIGN(region_tracks, array_field(item, "tracks"));
        MWB_CODEC_ASSIGN(is_loop, bool_field(item, "loop"));
        region.id = RegionId(region_id);
        region.name = std::move(region_name);
        region.start_tick = start;
        region.end_tick = region_end;
        for (const auto& track_value : *region_tracks) {
            MWB_CODEC_ASSIGN(track_id, as_uuid(track_value, "region track"));
            region.tracks.emplace_back(track_id);
        }
        region.is_loop = is_loop;
        sequence.regions.push_back(std::move(region));
    }
    MWB_CODEC_ASSIGN(scales, array_field(object, "scales"));
    for (const auto& item : *scales) {
        ScaleRegion region;
        MWB_CODEC_ASSIGN(region_id, uuid_field(item, "id"));
        MWB_CODEC_ASSIGN(start, signed_field(item, "start", int64_min, int64_max));
        MWB_CODEC_ASSIGN(region_end, signed_field(item, "end", int64_min, int64_max));
        MWB_CODEC_ASSIGN(region_root, unsigned_field(item, "root", 255));
        MWB_CODEC_ASSIGN(region_mask, unsigned_field(item, "mask", 0xFFFF));
        MWB_CODEC_ASSIGN(region_name, string_field(item, "name"));
        region.id = RegionId(region_id);
        region.start_tick = start;
        region.end_tick = region_end;
        region.root_pc = static_cast<std::uint8_t>(region_root);
        region.scale_mask = static_cast<std::uint16_t>(region_mask);
        region.scale_name = std::move(region_name);
        sequence.scale_regions.push_back(std::move(region));
    }
    MWB_CODEC_ASSIGN(recipes, array_field(object, "recipes"));
    for (const auto& item : *recipes) {
        Recipe recipe;
        MWB_CODEC_ASSIGN(recipe_id, uuid_field(item, "id"));
        MWB_CODEC_ASSIGN(tool, string_field(item, "tool"));
        MWB_CODEC_ASSIGN(version, signed_field(item, "version", std::numeric_limits<std::int32_t>::min(),
                                               std::numeric_limits<std::int32_t>::max()));
        MWB_CODEC_ASSIGN(hash, bytes_field(item, "hash"));
        MWB_CODEC_ASSIGN(parameters, string_field(item, "parameters"));
        MWB_CODEC_ASSIGN(targets, string_field(item, "targets"));
        if (hash.size() != recipe.source_semantic_hash.size())
            return malformed("recipe hash is not 32 bytes");
        recipe.id = RecipeId(recipe_id);
        recipe.tool_id = std::move(tool);
        recipe.algorithm_version = static_cast<std::int32_t>(version);
        std::copy(hash.begin(), hash.end(), recipe.source_semantic_hash.begin());
        recipe.parameters_json = std::move(parameters);
        recipe.targets_json = std::move(targets);
        sequence.recipes.push_back(std::move(recipe));
    }
    return sequence;
}

Result<RecordingTake> decode_take(const json& object) {
    constexpr auto int64_lowest = std::numeric_limits<std::int64_t>::min();
    constexpr auto int64_highest = std::numeric_limits<std::int64_t>::max();
    RecordingTake take;
    MWB_CODEC_ASSIGN(id, uuid_field(object, "id"));
    MWB_CODEC_ASSIGN(track, uuid_field(object, "track"));
    MWB_CODEC_ASSIGN(name, string_field(object, "name"));
    MWB_CODEC_ASSIGN(created, signed_field(object, "created", int64_lowest, int64_highest));
    MWB_CODEC_ASSIGN(start, signed_field(object, "start", 0, int64_highest));
    MWB_CODEC_ASSIGN(end, signed_field(object, "end", 0, int64_highest));
    MWB_CODEC_ASSIGN(status, unsigned_field(object, "status", 3));
    MWB_CODEC_ASSIGN(capture, string_field(object, "capture"));
    MWB_CODEC_ASSIGN(chunks, array_field(object, "chunks"));
    if (end < start)
        return malformed("take ends before it starts");
    take.id = TakeId(id);
    take.track = TrackId(track);
    take.name = std::move(name);
    take.created_ms = created;
    take.start_tick = start;
    take.end_tick = end;
    take.status = static_cast<TakeStatus>(status);
    take.capture_json = std::move(capture);
    for (const auto& item : *chunks) {
        if (!item.is_object())
            return malformed("take chunk is not an object");
        MWB_CODEC_ASSIGN(ordinal, signed_field(item, "ordinal", 0, std::numeric_limits<std::int32_t>::max()));
        MWB_CODEC_ASSIGN(first, signed_field(item, "first_ns", 0, int64_highest));
        MWB_CODEC_ASSIGN(payload, bytes_field(item, "payload"));
        take.chunks.push_back(TakeChunk{static_cast<std::int32_t>(ordinal), first, std::move(payload)});
    }
    return take;
}

Result<EditOperation> decode_operation(const json& object) {
    MWB_CODEC_ASSIGN(name, string_field(object, "op"));
    constexpr auto max_index = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
    if (name == "event_insert" || name == "event_remove") {
        MWB_CODEC_ASSIGN(track, uuid_field(object, "track"));
        MWB_CODEC_ASSIGN(event_value, member(object, "event"));
        MWB_CODEC_ASSIGN(event, decode_event(*event_value));
        if (name == "event_insert")
            return EditOperation(EventInsert{TrackId(track), std::move(event)});
        return EditOperation(EventRemove{TrackId(track), std::move(event)});
    }
    if (name == "event_replace") {
        MWB_CODEC_ASSIGN(track, uuid_field(object, "track"));
        MWB_CODEC_ASSIGN(before_value, member(object, "before"));
        MWB_CODEC_ASSIGN(after_value, member(object, "after"));
        MWB_CODEC_ASSIGN(before, decode_event(*before_value));
        MWB_CODEC_ASSIGN(after, decode_event(*after_value));
        if (before.id != after.id)
            return malformed("event replacement changes the event identity");
        return EditOperation(EventReplace{TrackId(track), std::move(before), std::move(after)});
    }
    if (name == "track_insert" || name == "track_remove") {
        MWB_CODEC_ASSIGN(sequence, uuid_field(object, "sequence"));
        MWB_CODEC_ASSIGN(index, unsigned_field(object, "index", max_index));
        MWB_CODEC_ASSIGN(track_value, member(object, "track"));
        MWB_CODEC_ASSIGN(track, decode_track(*track_value));
        if (name == "track_insert")
            return EditOperation(TrackInsert{SequenceId(sequence), static_cast<std::size_t>(index), std::move(track)});
        return EditOperation(TrackRemove{SequenceId(sequence), static_cast<std::size_t>(index), std::move(track)});
    }
    if (name == "track_properties") {
        MWB_CODEC_ASSIGN(track, uuid_field(object, "track"));
        MWB_CODEC_ASSIGN(before_value, member(object, "before"));
        MWB_CODEC_ASSIGN(after_value, member(object, "after"));
        MWB_CODEC_ASSIGN(before, decode_track(*before_value));
        MWB_CODEC_ASSIGN(after, decode_track(*after_value));
        return EditOperation(TrackPropertiesChange{TrackId(track), std::move(before), std::move(after)});
    }
    if (name == "track_move") {
        MWB_CODEC_ASSIGN(sequence, uuid_field(object, "sequence"));
        MWB_CODEC_ASSIGN(from, unsigned_field(object, "from", max_index));
        MWB_CODEC_ASSIGN(to, unsigned_field(object, "to", max_index));
        return EditOperation(
            TrackMove{SequenceId(sequence), static_cast<std::size_t>(from), static_cast<std::size_t>(to)});
    }
    if (name == "sequence_properties") {
        MWB_CODEC_ASSIGN(sequence, uuid_field(object, "sequence"));
        MWB_CODEC_ASSIGN(before_value, member(object, "before"));
        MWB_CODEC_ASSIGN(after_value, member(object, "after"));
        MWB_CODEC_ASSIGN(before, decode_sequence(*before_value));
        MWB_CODEC_ASSIGN(after, decode_sequence(*after_value));
        return EditOperation(SequencePropertiesChange{SequenceId(sequence), std::move(before), std::move(after)});
    }
    if (name == "sequence_insert" || name == "sequence_remove") {
        MWB_CODEC_ASSIGN(index, unsigned_field(object, "index", max_index));
        MWB_CODEC_ASSIGN(sequence_value, member(object, "sequence"));
        MWB_CODEC_ASSIGN(sequence, decode_sequence(*sequence_value));
        if (name == "sequence_insert")
            return EditOperation(SequenceInsert{static_cast<std::size_t>(index), std::move(sequence)});
        return EditOperation(SequenceRemove{static_cast<std::size_t>(index), std::move(sequence)});
    }
    if (name == "document_properties") {
        MWB_CODEC_ASSIGN(title_before, string_field(object, "title_before"));
        MWB_CODEC_ASSIGN(title_after, string_field(object, "title_after"));
        MWB_CODEC_ASSIGN(metadata_before, string_field(object, "metadata_before"));
        MWB_CODEC_ASSIGN(metadata_after, string_field(object, "metadata_after"));
        return EditOperation(DocumentPropertiesChange{std::move(title_before), std::move(title_after),
                                                      std::move(metadata_before), std::move(metadata_after)});
    }
    if (name == "take_insert" || name == "take_remove") {
        MWB_CODEC_ASSIGN(index, unsigned_field(object, "index", max_index));
        MWB_CODEC_ASSIGN(take_value, member(object, "take"));
        MWB_CODEC_ASSIGN(take, decode_take(*take_value));
        if (name == "take_insert")
            return EditOperation(TakeInsert{static_cast<std::size_t>(index), std::move(take)});
        return EditOperation(TakeRemove{static_cast<std::size_t>(index), std::move(take)});
    }
    return malformed("unknown operation '" + name + "'");
}

void add_sequence_entities(std::vector<Uuid>& entities, const Sequence& sequence) {
    entities.push_back(sequence.id.uuid());
    for (const auto& track : sequence.tracks) {
        entities.push_back(track.id.uuid());
        for (const auto& event : track.events)
            entities.push_back(event.id.uuid());
    }
}

void add_track_entities(std::vector<Uuid>& entities, const Track& track) {
    entities.push_back(track.id.uuid());
    for (const auto& event : track.events)
        entities.push_back(event.id.uuid());
}

Result<CommandEnvelope> decode_envelope(const json& root) {
    MWB_CODEC_ASSIGN(format, string_field(root, "format"));
    if (format != command_codec_format)
        return malformed("unknown format tag");
    MWB_CODEC_ASSIGN(version, signed_field(root, "version", int64_min, int64_max));
    if (version != command_codec_version)
        return make_error(ErrorCode::unsupported, "command envelope uses unsupported codec version " +
                                                      std::to_string(version));
    CommandEnvelope envelope;
    MWB_CODEC_ASSIGN(command_id, uuid_field(root, "command_id"));
    MWB_CODEC_ASSIGN(kind, string_field(root, "kind"));
    MWB_CODEC_ASSIGN(base_hash, bytes_field(root, "base_hash"));
    MWB_CODEC_ASSIGN(base_generation, unsigned_field(root, "base_generation", static_cast<std::uint64_t>(int64_max)));
    MWB_CODEC_ASSIGN(result_generation,
                     unsigned_field(root, "result_generation", static_cast<std::uint64_t>(int64_max)));
    if (kind == "edit")
        envelope.kind = CommandKind::edit;
    else if (kind == "undo")
        envelope.kind = CommandKind::undo;
    else if (kind == "redo")
        envelope.kind = CommandKind::redo;
    else
        return malformed("unknown command kind");
    if (base_hash.size() != envelope.base_semantic_hash.size())
        return malformed("base hash is not 32 bytes");
    if (result_generation <= base_generation)
        return malformed("result generation does not follow the base generation");
    envelope.command_id = command_id;
    std::copy(base_hash.begin(), base_hash.end(), envelope.base_semantic_hash.begin());
    envelope.base_generation = base_generation;
    envelope.result_generation = result_generation;

    MWB_CODEC_ASSIGN(changes, member(root, "changes"));
    MWB_CODEC_ASSIGN(label, string_field(*changes, "label"));
    MWB_CODEC_ASSIGN(operations, array_field(*changes, "operations"));
    envelope.changes.label = std::move(label);
    envelope.changes.operations.reserve(operations->size());
    for (const auto& item : *operations) {
        MWB_CODEC_ASSIGN(operation, decode_operation(item));
        envelope.changes.operations.push_back(std::move(operation));
    }

    MWB_CODEC_ASSIGN(affected, array_field(root, "affected"));
    std::vector<Uuid> stored;
    stored.reserve(affected->size());
    for (const auto& item : *affected) {
        MWB_CODEC_ASSIGN(identity, as_uuid(item, "affected entity"));
        stored.push_back(identity);
    }
    if (stored != affected_entities(envelope.changes))
        return malformed("affected entity list does not match the change set");
    return envelope;
}

} // namespace

std::vector<Uuid> affected_entities(const ChangeSet& changes) {
    std::vector<Uuid> entities;
    for (const auto& operation : changes.operations) {
        std::visit(Overloaded{
                       [&](const EventInsert& insert) {
                           entities.push_back(insert.track.uuid());
                           entities.push_back(insert.event.id.uuid());
                       },
                       [&](const EventRemove& remove) {
                           entities.push_back(remove.track.uuid());
                           entities.push_back(remove.event.id.uuid());
                       },
                       [&](const EventReplace& replace) {
                           entities.push_back(replace.track.uuid());
                           entities.push_back(replace.before.id.uuid());
                       },
                       [&](const TrackInsert& insert) {
                           entities.push_back(insert.sequence.uuid());
                           add_track_entities(entities, insert.track);
                       },
                       [&](const TrackRemove& remove) {
                           entities.push_back(remove.sequence.uuid());
                           add_track_entities(entities, remove.track);
                       },
                       [&](const TrackPropertiesChange& change) { entities.push_back(change.track.uuid()); },
                       [&](const TrackMove& move) { entities.push_back(move.sequence.uuid()); },
                       [&](const SequencePropertiesChange& change) { entities.push_back(change.sequence.uuid()); },
                       [&](const SequenceInsert& insert) { add_sequence_entities(entities, insert.sequence); },
                       [&](const SequenceRemove& remove) { add_sequence_entities(entities, remove.sequence); },
                       [](const DocumentPropertiesChange&) {},
                       [&](const TakeInsert& insert) {
                           entities.push_back(insert.take.id.uuid());
                           entities.push_back(insert.take.track.uuid());
                       },
                       [&](const TakeRemove& remove) {
                           entities.push_back(remove.take.id.uuid());
                           entities.push_back(remove.take.track.uuid());
                       },
                   },
                   operation);
    }
    std::sort(entities.begin(), entities.end());
    entities.erase(std::unique(entities.begin(), entities.end()), entities.end());
    return entities;
}

Result<std::vector<std::uint8_t>> encode_command(const CommandEnvelope& envelope) {
    if (envelope.result_generation <= envelope.base_generation)
        return make_error(ErrorCode::invalid_argument, "command result generation must exceed its base generation");
    try {
        json operations = json::array();
        for (const auto& operation : envelope.changes.operations)
            operations.push_back(encode_operation(operation));
        json affected = json::array();
        for (const auto& identity : affected_entities(envelope.changes))
            affected.push_back(uuid_value(identity));
        const json root{
            {"format", command_codec_format},
            {"version", command_codec_version},
            {"command_id", uuid_value(envelope.command_id)},
            {"kind", kind_name(envelope.kind)},
            {"base_hash", bytes_value(envelope.base_semantic_hash)},
            {"base_generation", envelope.base_generation},
            {"result_generation", envelope.result_generation},
            {"affected", std::move(affected)},
            {"changes", json{{"label", envelope.changes.label}, {"operations", std::move(operations)}}},
        };
        return json::to_cbor(root);
    } catch (const json::exception& exception) {
        return make_error(ErrorCode::invalid_argument, std::string("cannot encode command: ") + exception.what());
    } catch (const std::bad_alloc&) {
        return make_error(ErrorCode::limit_exceeded, "cannot encode command: out of memory");
    }
}

Result<CommandEnvelope> decode_command(std::span<const std::uint8_t> bytes) {
    try {
        const json root = json::from_cbor(bytes.begin(), bytes.end(), true, false);
        if (root.is_discarded())
            return malformed("not valid CBOR");
        return decode_envelope(root);
    } catch (const json::exception& exception) {
        return malformed(exception.what());
    } catch (const std::bad_alloc&) {
        return make_error(ErrorCode::limit_exceeded, "cannot decode command: out of memory");
    }
}

#undef MWB_CODEC_ASSIGN

} // namespace mwb::document
