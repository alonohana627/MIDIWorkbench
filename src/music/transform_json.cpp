#include "mwb/music/transform.hpp"

#include "mwb/core/random.hpp"
#include "mwb/core/serialization.hpp"
#include "mwb/core/utility.hpp"
#include "mwb/music/json_fields.hpp"
#include "mwb/music/progression.hpp"

#include <array>
#include <charconv>

namespace mwb::music {

namespace {

using nlohmann::json;
template <class Enumeration, std::size_t Count>
using NameTable = std::array<std::pair<std::string_view, Enumeration>, Count>;

constexpr NameTable<QuantizeTarget, 3> quantize_targets{{{"starts", QuantizeTarget::starts},
                                                         {"ends", QuantizeTarget::ends},
                                                         {"starts_and_ends", QuantizeTarget::starts_and_ends}}};
constexpr NameTable<RandomDistribution, 2> distributions{
    {{"uniform", RandomDistribution::uniform}, {"triangular", RandomDistribution::triangular}}};
constexpr NameTable<TransposeMode, 3> transpose_modes{{{"semitones", TransposeMode::semitones},
                                                       {"octaves", TransposeMode::octaves},
                                                       {"scale_degrees", TransposeMode::scale_degrees}}};
constexpr NameTable<OutOfScalePolicy, 2> out_of_scale_policies{
    {{"preserve_chromatic_offset", OutOfScalePolicy::preserve_chromatic_offset},
     {"conform_first", OutOfScalePolicy::conform_first}}};
constexpr NameTable<NearestTie, 2> ties{{{"downward", NearestTie::downward}, {"upward", NearestTie::upward}}};
constexpr NameTable<RangePolicy, 2> range_policies{{{"reject", RangePolicy::reject}, {"constrain", RangePolicy::constrain}}};
constexpr NameTable<OutOfRangeAction, 2> out_of_range_actions{
    {{"reject", OutOfRangeAction::reject}, {"skip", OutOfRangeAction::skip}}};
constexpr NameTable<VelocityMode, 6> velocity_modes{{{"set", VelocityMode::set},
                                                     {"add", VelocityMode::add},
                                                     {"scale", VelocityMode::scale},
                                                     {"ramp", VelocityMode::ramp},
                                                     {"curve", VelocityMode::curve},
                                                     {"compress", VelocityMode::compress}}};
constexpr NameTable<VelocityTarget, 2> velocity_targets{
    {{"note_on", VelocityTarget::note_on}, {"release", VelocityTarget::release}}};
constexpr NameTable<LengthMode, 5> length_modes{{{"set", LengthMode::set},
                                                 {"multiply", LengthMode::multiply},
                                                 {"add", LengthMode::add},
                                                 {"legato_to_next", LengthMode::legato_to_next},
                                                 {"gap", LengthMode::gap}}};
constexpr NameTable<ControllerReversePolicy, 1> controller_policies{
    {{"leave_unchanged", ControllerReversePolicy::leave_unchanged}}};
constexpr NameTable<StrumOrder, 4> strum_orders{{{"up", StrumOrder::up},
                                                 {"down", StrumOrder::down},
                                                 {"alternate", StrumOrder::alternate},
                                                 {"random", StrumOrder::random}}};
constexpr NameTable<ArpeggioPattern, 5> arpeggio_patterns{{{"up", ArpeggioPattern::up},
                                                           {"down", ArpeggioPattern::down},
                                                           {"up_down", ArpeggioPattern::up_down},
                                                           {"as_played", ArpeggioPattern::as_played},
                                                           {"random", ArpeggioPattern::random}}};
constexpr NameTable<DuplicatePolicy, 3> duplicate_policies{{{"exact", DuplicatePolicy::exact},
                                                            {"ignore_velocity", DuplicatePolicy::ignore_velocity},
                                                            {"same_onset", DuplicatePolicy::same_onset}}};
constexpr NameTable<OverlapAction, 3> overlap_actions{{{"trim_earlier", OverlapAction::trim_earlier},
                                                       {"merge", OverlapAction::merge},
                                                       {"leave", OverlapAction::leave}}};

template <class Enumeration, std::size_t Count>
std::string name_of(Enumeration value, const NameTable<Enumeration, Count>& table) {
    return std::string(enumeration_name<Enumeration>(value, std::span<const std::pair<std::string_view, Enumeration>>(table)));
}

template <class Enumeration, std::size_t Count>
void read_enum(FieldReader& reader, std::string_view key, Enumeration& out, const NameTable<Enumeration, Count>& table) {
    reader.enumeration<Enumeration>(key, out, std::span<const std::pair<std::string_view, Enumeration>>(table));
}

json grid_json(const GridSpec& grid) {
    return json{{"step", grid.step.to_string()}, {"swing", grid.swing.to_string()}, {"origin", grid.origin}};
}

void read_grid(FieldReader& reader, std::string_view key, GridSpec& grid) {
    if (const auto* object = reader.object(key)) {
        FieldReader nested(*object, std::string(key));
        nested.rational("step", grid.step);
        nested.rational("swing", grid.swing);
        nested.integer("origin", grid.origin);
        if (!nested.status())
            reader.fail(key, nested.status().error().message);
    }
}

json optional_json(const std::optional<Tick>& value) { return value ? json(*value) : json(nullptr); }

void read_optional_tick(FieldReader& reader, std::string_view key, std::optional<Tick>& out) {
    if (!reader.present(key))
        return;
    if (reader.is_null(key)) {
        out.reset();
        return;
    }
    Tick value = 0;
    reader.integer(key, value);
    if (reader.status())
        out = value;
}

// --- Per-tool parameter forms ------------------------------------------------------------------

json write(const QuantizeParameters& p) {
    return json{{"grid", grid_json(p.grid)},
                {"strength", p.strength.to_string()},
                {"target", name_of(p.target, quantize_targets)},
                {"preserve_duration", p.preserve_duration},
                {"max_movement", optional_json(p.max_movement)}};
}
void read(FieldReader& r, QuantizeParameters& p) {
    read_grid(r, "grid", p.grid);
    r.rational("strength", p.strength);
    read_enum(r, "target", p.target, quantize_targets);
    r.boolean("preserve_duration", p.preserve_duration);
    read_optional_tick(r, "max_movement", p.max_movement);
}

json write(const HumanizeParameters& p) {
    return json{{"timing_range", p.timing_range},
                {"velocity_range", p.velocity_range},
                {"duration_range", p.duration_range},
                {"distribution", name_of(p.distribution, distributions)},
                {"keep_chord_groups", p.keep_chord_groups},
                {"prevent_order_inversions", p.prevent_order_inversions},
                {"chord_tolerance", optional_json(p.chord_tolerance)}};
}
void read(FieldReader& r, HumanizeParameters& p) {
    r.integer("timing_range", p.timing_range);
    r.integer("velocity_range", p.velocity_range);
    r.integer("duration_range", p.duration_range);
    read_enum(r, "distribution", p.distribution, distributions);
    r.boolean("keep_chord_groups", p.keep_chord_groups);
    r.boolean("prevent_order_inversions", p.prevent_order_inversions);
    read_optional_tick(r, "chord_tolerance", p.chord_tolerance);
}

json write(const TransposeParameters& p) {
    return json{{"mode", name_of(p.mode, transpose_modes)},
                {"amount", p.amount},
                {"scale_root", p.scale_root},
                {"scale_mask", p.scale_mask},
                {"out_of_scale", name_of(p.out_of_scale, out_of_scale_policies)},
                {"tie", name_of(p.tie, ties)},
                {"exclude_drums", p.exclude_drums},
                {"range_policy", name_of(p.range_policy, range_policies)}};
}
void read(FieldReader& r, TransposeParameters& p) {
    read_enum(r, "mode", p.mode, transpose_modes);
    r.integer("amount", p.amount);
    r.integer("scale_root", p.scale_root);
    r.integer("scale_mask", p.scale_mask);
    read_enum(r, "out_of_scale", p.out_of_scale, out_of_scale_policies);
    read_enum(r, "tie", p.tie, ties);
    r.boolean("exclude_drums", p.exclude_drums);
    read_enum(r, "range_policy", p.range_policy, range_policies);
}

json write(const MoveParameters& p) {
    return json{{"ticks", p.ticks}, {"semitones", p.semitones}, {"range_policy", name_of(p.range_policy, range_policies)}};
}
void read(FieldReader& r, MoveParameters& p) {
    r.integer("ticks", p.ticks);
    r.integer("semitones", p.semitones);
    read_enum(r, "range_policy", p.range_policy, range_policies);
}

json write(const VelocityParameters& p) {
    return json{{"mode", name_of(p.mode, velocity_modes)},
                {"target", name_of(p.target, velocity_targets)},
                {"value", p.value},
                {"factor", p.factor.to_string()},
                {"ramp_start", p.ramp_start},
                {"ramp_end", p.ramp_end},
                {"curve", p.curve},
                {"range_low", p.range_low},
                {"range_high", p.range_high}};
}
void read(FieldReader& r, VelocityParameters& p) {
    read_enum(r, "mode", p.mode, velocity_modes);
    read_enum(r, "target", p.target, velocity_targets);
    r.integer("value", p.value);
    r.rational("factor", p.factor);
    r.integer("ramp_start", p.ramp_start);
    r.integer("ramp_end", p.ramp_end);
    r.integer("curve", p.curve);
    r.integer("range_low", p.range_low);
    r.integer("range_high", p.range_high);
}

json write(const LengthParameters& p) {
    return json{{"mode", name_of(p.mode, length_modes)}, {"ticks", p.ticks}, {"factor", p.factor.to_string()}};
}
void read(FieldReader& r, LengthParameters& p) {
    read_enum(r, "mode", p.mode, length_modes);
    r.integer("ticks", p.ticks);
    r.rational("factor", p.factor);
}

json write(const TimeStretchParameters& p) {
    return json{{"factor", p.factor.to_string()}, {"anchor", p.anchor}, {"event_classes", p.event_classes}};
}
void read(FieldReader& r, TimeStretchParameters& p) {
    r.rational("factor", p.factor);
    r.integer("anchor", p.anchor);
    r.integer("event_classes", p.event_classes);
}

json range_json(const std::optional<TimeRange>& range) {
    return range ? json{{"begin", range->begin}, {"end", range->end}} : json(nullptr);
}
void read_range(FieldReader& r, std::string_view key, std::optional<TimeRange>& out) {
    if (!r.present(key))
        return;
    if (r.is_null(key)) {
        out.reset();
        return;
    }
    const json* object = r.object(key);
    if (!object)
        return;
    FieldReader nested(*object, std::string(key));
    TimeRange range;
    nested.integer("begin", range.begin);
    nested.integer("end", range.end);
    if (!nested.status()) {
        r.fail(key, nested.status().error().message);
        return;
    }
    out = range;
}

json write(const ReverseParameters& p) {
    return json{{"range", range_json(p.range)}, {"controllers", name_of(p.controllers, controller_policies)}};
}
void read(FieldReader& r, ReverseParameters& p) {
    read_range(r, "range", p.range);
    read_enum(r, "controllers", p.controllers, controller_policies);
}

json write(const InvertPitchParameters& p) {
    return json{{"axis_pitch", p.axis_pitch},
                {"axis_between_keys", p.axis_between_keys},
                {"out_of_range", name_of(p.out_of_range, out_of_range_actions)}};
}
void read(FieldReader& r, InvertPitchParameters& p) {
    r.integer("axis_pitch", p.axis_pitch);
    r.boolean("axis_between_keys", p.axis_between_keys);
    read_enum(r, "out_of_range", p.out_of_range, out_of_range_actions);
}

json write(const RotateParameters& p) { return json{{"range", range_json(p.range)}, {"shift", p.shift}}; }
void read(FieldReader& r, RotateParameters& p) {
    std::optional<TimeRange> range = p.range;
    read_range(r, "range", range);
    if (range)
        p.range = *range;
    r.integer("shift", p.shift);
}

json write(const RepeatParameters& p) {
    return json{{"count", p.count},
                {"length", p.length},
                {"pitch_step", p.pitch_step},
                {"velocity_step", p.velocity_step},
                {"out_of_range", name_of(p.out_of_range, out_of_range_actions)}};
}
void read(FieldReader& r, RepeatParameters& p) {
    r.integer("count", p.count);
    r.integer("length", p.length);
    r.integer("pitch_step", p.pitch_step);
    r.integer("velocity_step", p.velocity_step);
    read_enum(r, "out_of_range", p.out_of_range, out_of_range_actions);
}

json write(const StrumParameters& p) {
    return json{{"order", name_of(p.order, strum_orders)},
                {"spread", p.spread},
                {"preserve_chord_grouping", p.preserve_chord_grouping},
                {"chord_tolerance", optional_json(p.chord_tolerance)}};
}
void read(FieldReader& r, StrumParameters& p) {
    read_enum(r, "order", p.order, strum_orders);
    r.integer("spread", p.spread);
    r.boolean("preserve_chord_grouping", p.preserve_chord_grouping);
    read_optional_tick(r, "chord_tolerance", p.chord_tolerance);
}

json write(const ArpeggiateParameters& p) {
    return json{{"pattern", name_of(p.pattern, arpeggio_patterns)},
                {"rate", p.rate.to_string()},
                {"octaves", p.octaves},
                {"gate", p.gate.to_string()},
                {"swing", p.swing.to_string()},
                {"chord_tolerance", optional_json(p.chord_tolerance)}};
}
void read(FieldReader& r, ArpeggiateParameters& p) {
    read_enum(r, "pattern", p.pattern, arpeggio_patterns);
    r.rational("rate", p.rate);
    r.integer("octaves", p.octaves);
    r.rational("gate", p.gate);
    r.rational("swing", p.swing);
    read_optional_tick(r, "chord_tolerance", p.chord_tolerance);
}

json write(const EuclideanParameters& p) {
    return json{{"steps", p.steps},
                {"pulses", p.pulses},
                {"rotation", p.rotation},
                {"pitch", p.pitch},
                {"channel", p.channel},
                {"step_length", p.step_length.to_string()},
                {"start", p.start},
                {"cycles", p.cycles},
                {"gate", p.gate.to_string()},
                {"velocity", p.velocity},
                {"accent_velocity", p.accent_velocity},
                {"accent_pulses", p.accent_pulses}};
}
void read(FieldReader& r, EuclideanParameters& p) {
    r.integer("steps", p.steps);
    r.integer("pulses", p.pulses);
    r.integer("rotation", p.rotation);
    r.integer("pitch", p.pitch);
    r.integer("channel", p.channel);
    r.rational("step_length", p.step_length);
    r.integer("start", p.start);
    r.integer("cycles", p.cycles);
    r.rational("gate", p.gate);
    r.integer("velocity", p.velocity);
    r.integer("accent_velocity", p.accent_velocity);
    r.integer_list("accent_pulses", p.accent_pulses);
}

json write(const GrooveApplyParameters& p) {
    return json{{"groove", json::parse(groove_to_json(p.groove), nullptr, false)},
                {"target_grid", grid_json(p.target_grid)},
                {"strength", p.strength.to_string()},
                {"velocity_strength", p.velocity_strength.to_string()},
                {"preserve_duration", p.preserve_duration}};
}
void read(FieldReader& r, GrooveApplyParameters& p) {
    if (const auto* groove = r.object("groove")) {
        auto parsed = groove_from_json(groove->dump());
        if (!parsed)
            r.fail("groove", parsed.error().message);
        else
            p.groove = *parsed;
    }
    read_grid(r, "target_grid", p.target_grid);
    r.rational("strength", p.strength);
    r.rational("velocity_strength", p.velocity_strength);
    r.boolean("preserve_duration", p.preserve_duration);
}

json write(const RemoveDuplicatesParameters& p) {
    return json{{"policy", name_of(p.policy, duplicate_policies)}, {"across_tracks", p.across_tracks}};
}
void read(FieldReader& r, RemoveDuplicatesParameters& p) {
    read_enum(r, "policy", p.policy, duplicate_policies);
    r.boolean("across_tracks", p.across_tracks);
}

json write(const ResolveOverlapsParameters& p) { return json{{"action", name_of(p.action, overlap_actions)}}; }
void read(FieldReader& r, ResolveOverlapsParameters& p) { read_enum(r, "action", p.action, overlap_actions); }

json write(const SplitParameters& p) { return json{{"points", p.points}}; }
void read(FieldReader& r, SplitParameters& p) { r.integer_list("points", p.points); }

json write(const JoinParameters& p) { return json{{"max_gap", p.max_gap}}; }
void read(FieldReader& r, JoinParameters& p) { r.integer("max_gap", p.max_gap); }

json write(const ConformToScaleParameters& p) {
    return json{{"scale_root", p.scale_root},
                {"scale_mask", p.scale_mask},
                {"tie", name_of(p.tie, ties)},
                {"exclude_drums", p.exclude_drums}};
}
void read(FieldReader& r, ConformToScaleParameters& p) {
    r.integer("scale_root", p.scale_root);
    r.integer("scale_mask", p.scale_mask);
    read_enum(r, "tie", p.tie, ties);
    r.boolean("exclude_drums", p.exclude_drums);
}

/// Tool identifiers in variant index order.
constexpr std::array<std::string_view, std::variant_size_v<TransformParameters>> tool_names{
    "quantize",     "humanize", "transpose",  "move",      "velocity",          "length",
    "time_stretch", "reverse",  "invert_pitch", "rotate",  "repeat",            "strum",
    "arpeggiate",   "euclidean", "groove_apply", "remove_duplicates", "resolve_overlaps", "split",
    "join",         "conform_to_scale"};

std::optional<std::uint64_t> seed_of(const TransformParameters& parameters) {
    return std::visit(Overloaded{[](const HumanizeParameters& p) -> std::optional<std::uint64_t> { return p.seed; },
                                 [](const StrumParameters& p) -> std::optional<std::uint64_t> { return p.seed; },
                                 [](const ArpeggiateParameters& p) -> std::optional<std::uint64_t> { return p.seed; },
                                 [](const auto&) -> std::optional<std::uint64_t> { return std::nullopt; }},
                      parameters);
}

void set_seed(TransformParameters& parameters, std::uint64_t seed) {
    std::visit(Overloaded{[&](HumanizeParameters& p) { p.seed = seed; }, [&](StrumParameters& p) { p.seed = seed; },
                          [&](ArpeggiateParameters& p) { p.seed = seed; }, [](auto&) {}},
               parameters);
}

/// Default-constructs the alternative at `index`.
template <std::size_t Index = 0>
TransformParameters make_alternative(std::size_t index) {
    if constexpr (Index + 1 < std::variant_size_v<TransformParameters>) {
        if (index != Index)
            return make_alternative<Index + 1>(index);
    }
    return TransformParameters{std::in_place_index<Index>};
}

} // namespace

std::string_view tool_id(const TransformParameters& parameters) noexcept { return tool_names[parameters.index()]; }

int algorithm_version(const TransformParameters& parameters) noexcept {
    std::ignore = parameters; // Every tool is at algorithm version 1.
    return 1;
}

bool uses_random_seed(const TransformParameters& parameters) noexcept { return seed_of(parameters).has_value(); }

Result<TransformPreview> preview_transform(const DocumentData& document, const TargetSelection& selection,
                                           const TransformParameters& parameters) {
    return std::visit(
        Overloaded{
            [&](const QuantizeParameters& p) { return quantize(document, selection, p); },
            [&](const HumanizeParameters& p) { return humanize(document, selection, p); },
            [&](const TransposeParameters& p) { return transpose(document, selection, p); },
            [&](const MoveParameters& p) { return move_notes(document, selection, p); },
            [&](const VelocityParameters& p) { return change_velocity(document, selection, p); },
            [&](const LengthParameters& p) { return change_length(document, selection, p); },
            [&](const TimeStretchParameters& p) { return time_stretch(document, selection, p); },
            [&](const ReverseParameters& p) { return reverse(document, selection, p); },
            [&](const InvertPitchParameters& p) { return invert_pitch(document, selection, p); },
            [&](const RotateParameters& p) { return rotate(document, selection, p); },
            [&](const RepeatParameters& p) { return repeat(document, selection, p); },
            [&](const StrumParameters& p) { return strum(document, selection, p); },
            [&](const ArpeggiateParameters& p) { return arpeggiate(document, selection, p); },
            [&](const EuclideanParameters& p) { return euclidean_rhythm(document, selection, p); },
            [&](const GrooveApplyParameters& p) { return apply_groove(document, selection, p); },
            [&](const RemoveDuplicatesParameters& p) { return remove_duplicates(document, selection, p); },
            [&](const ResolveOverlapsParameters& p) { return resolve_overlaps(document, selection, p); },
            [&](const SplitParameters& p) { return split_notes(document, selection, p); },
            [&](const JoinParameters& p) { return join_notes(document, selection, p); },
            [&](const ConformToScaleParameters& p) { return conform_to_scale(document, selection, p); },
        },
        parameters);
}

std::string parameters_to_json(const TransformParameters& parameters) {
    json root;
    root["format"] = "mwb.transform";
    root["tool"] = std::string(tool_id(parameters));
    root["version"] = algorithm_version(parameters);
    root["parameters"] = std::visit([](const auto& p) { return write(p); }, parameters);
    if (const auto seed = seed_of(parameters)) {
        root["prng"] = json{{"name", DeterministicRandom::algorithm_name},
                            {"version", DeterministicRandom::algorithm_version},
                            {"seed", std::to_string(*seed)}};
    }
    return root.dump();
}

Result<TransformParameters> parameters_from_json(std::string_view text) {
    auto parsed = parse_json_object(text);
    if (!parsed)
        return std::unexpected(parsed.error());
    FieldReader reader(*parsed, "transform");
    std::string format;
    std::string tool;
    int version = 0;
    reader.text("format", format);
    reader.text("tool", tool);
    reader.integer("version", version);
    MWB_TRY(reader.status());
    if (format != "mwb.transform")
        return make_error(ErrorCode::invalid_data, "not a transformation parameter document");
    std::optional<std::size_t> index;
    for (std::size_t candidate = 0; candidate < tool_names.size(); ++candidate)
        if (tool_names[candidate] == tool)
            index = candidate;
    if (!index)
        return make_error(ErrorCode::unsupported, "unknown transformation tool", tool);
    TransformParameters parameters = make_alternative(*index);
    if (version != algorithm_version(parameters))
        return make_error(ErrorCode::unsupported, "unsupported transformation algorithm version", tool);
    if (const auto* fields = reader.object("parameters")) {
        FieldReader field_reader(*fields, tool);
        std::visit([&](auto& p) { read(field_reader, p); }, parameters);
        MWB_TRY(field_reader.status());
    }
    MWB_TRY(reader.status());
    if (uses_random_seed(parameters)) {
        const auto* prng = reader.object("prng");
        MWB_TRY(reader.status());
        if (!prng)
            return make_error(ErrorCode::invalid_data, "seeded transformation lacks its PRNG description", tool);
        FieldReader prng_reader(*prng, "prng");
        std::string name;
        int prng_version = 0;
        std::string seed_text;
        prng_reader.text("name", name);
        prng_reader.integer("version", prng_version);
        prng_reader.text("seed", seed_text);
        MWB_TRY(prng_reader.status());
        if (name != DeterministicRandom::algorithm_name || prng_version != DeterministicRandom::algorithm_version)
            return make_error(ErrorCode::unsupported, "recipe uses a different PRNG algorithm or version", name);
        std::uint64_t seed = 0;
        const auto [end, error] = std::from_chars(seed_text.data(), seed_text.data() + seed_text.size(), seed);
        if (error != std::errc{} || end != seed_text.data() + seed_text.size() || seed_text.empty())
            return make_error(ErrorCode::invalid_data, "malformed PRNG seed", seed_text);
        set_seed(parameters, seed);
    }
    return parameters;
}

Result<Recipe> make_recipe(const DocumentData& document, const TargetSelection& selection,
                           const TransformParameters& parameters) {
    const auto index = document.sequence_index(selection.sequence);
    if (!index)
        return make_error(ErrorCode::not_found, "the target sequence does not exist");
    Recipe recipe;
    recipe.id = RecipeId::generate();
    recipe.tool_id = std::string(tool_id(parameters));
    recipe.algorithm_version = algorithm_version(parameters);
    recipe.source_semantic_hash = semantic_hash(document.sequences[*index]);
    recipe.parameters_json = parameters_to_json(parameters);
    recipe.targets_json = selection_to_json(selection);
    return recipe;
}

namespace {

/// Replays a stored "progression" recipe (insert_progression): its parameters hold the start,
/// channel and the progression with its voice-leading options; its target is the track
/// (code-core-15).
Result<TransformPreview> replay_progression(const DocumentData& document, const Recipe& recipe, bool allow_changed_source) {
    if (recipe.algorithm_version != voice_leading_algorithm_version)
        return make_error(ErrorCode::unsupported, "unsupported voice-leading algorithm version");
    const auto parameters = nlohmann::json::parse(recipe.parameters_json, nullptr, false);
    const auto targets = nlohmann::json::parse(recipe.targets_json, nullptr, false);
    if (!parameters.is_object() || !targets.is_object() || !parameters.contains("progression") ||
        !targets.contains("track") || !targets["track"].is_string())
        return make_error(ErrorCode::invalid_data, "progression recipe is incomplete");
    Tick start = 0;
    int channel = 0;
    FieldReader reader(parameters, "progression recipe");
    reader.integer("start", start);
    reader.integer("channel", channel);
    MWB_TRY(reader.status());
    if (start < 0 || channel < 0 || channel > 15)
        return make_error(ErrorCode::invalid_data, "progression recipe start or channel out of range");
    auto progression = progression_from_json(parameters["progression"].dump());
    if (!progression)
        return std::unexpected(progression.error());
    const auto parsed_track = Uuid::parse(targets["track"].get<std::string>());
    if (!parsed_track)
        return make_error(ErrorCode::invalid_data, "progression recipe names no valid track");
    const TrackId track(*parsed_track);
    const auto location = find_track(document, track);
    if (!location)
        return make_error(ErrorCode::not_found, "the recipe's track does not exist");
    if (!allow_changed_source && semantic_hash(document.sequences[location->sequence]) != recipe.source_semantic_hash)
        return make_error(ErrorCode::conflict, "the source sequence changed since the recipe was recorded");
    return insert_progression(document, track, progression->first, progression->second, start,
                              static_cast<std::uint8_t>(channel));
}

} // namespace

Result<TransformPreview> replay_recipe(const DocumentData& document, const Recipe& recipe, bool allow_changed_source) {
    if (recipe.tool_id == "progression")
        return replay_progression(document, recipe, allow_changed_source);
    auto parameters = parameters_from_json(recipe.parameters_json);
    if (!parameters)
        return std::unexpected(parameters.error());
    if (tool_id(*parameters) != recipe.tool_id || algorithm_version(*parameters) != recipe.algorithm_version)
        return make_error(ErrorCode::invalid_data, "recipe tool or version does not match its parameters");
    auto selection = selection_from_json(recipe.targets_json);
    if (!selection)
        return std::unexpected(selection.error());
    const auto index = document.sequence_index(selection->sequence);
    if (!index)
        return make_error(ErrorCode::not_found, "the recipe's sequence does not exist");
    if (!allow_changed_source && semantic_hash(document.sequences[*index]) != recipe.source_semantic_hash)
        return make_error(ErrorCode::conflict, "the source sequence changed since the recipe was recorded");
    return preview_transform(document, *selection, *parameters);
}

} // namespace mwb::music
