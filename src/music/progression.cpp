#include "mwb/music/progression.hpp"

#include "mwb/core/checked.hpp"
#include "mwb/music/json_fields.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace mwb::music {

namespace {

/// Safety bound on enumerated voicings per step before ranking.
constexpr std::size_t enumeration_bound = 200000;

/// Mathematical modulo with a non-negative result.
int positive_modulo(int value, int modulus) { return ((value % modulus) + modulus) % modulus; }

/// Removal priority used when there are fewer voices than chord tones (fifth first, then
/// extensions from the top, then the sixth); the root, third and seventh are kept longest.
int omission_rank(int interval) {
    if (interval == 7)
        return 0;
    if (interval > 12)
        return 1;
    if (interval == 9)
        return 2;
    if (interval == 0)
        return 9;
    if (interval == 3 || interval == 4)
        return 5;
    return 3;
}

struct StepPlan {
    std::vector<int> chord_pitch_classes;     ///< Distinct chord pitch classes in interval order.
    std::vector<int> required_pitch_classes;  ///< Pitch classes every candidate must contain.
    std::optional<int> bass_pitch_class;      ///< Required pitch class of the lowest voice.
};

/// Validates a step and derives its chord tones, required tones for `voice_count` voices and bass constraint.
[[nodiscard]] Result<StepPlan> plan_step(const ProgressionStep& step, int voice_count) {
    if (step.root_pitch_class < 0 || step.root_pitch_class > 11)
        return make_error(ErrorCode::invalid_argument, "progression root pitch class must lie in 0..11");
    if (step.duration < 1)
        return make_error(ErrorCode::invalid_argument, "progression step duration must be at least one tick");
    if (step.velocity < 1 || step.velocity > 127)
        return make_error(ErrorCode::invalid_argument, "progression velocity must lie in 1..127");
    if (step.gate.denominator <= 0 || step.gate.numerator <= 0 || step.gate.numerator > step.gate.denominator)
        return make_error(ErrorCode::invalid_argument, "progression gate must lie in (0, 1]");
    StepPlan plan;
    std::vector<int> intervals;
    for (const int interval : chord_intervals(step.type, step.extensions)) {
        const int pitch_class = positive_modulo(step.root_pitch_class + interval, 12);
        if (std::ranges::find(plan.chord_pitch_classes, pitch_class) == plan.chord_pitch_classes.end()) {
            plan.chord_pitch_classes.push_back(pitch_class);
            intervals.push_back(interval);
        }
    }
    std::vector<std::size_t> kept(intervals.size());
    for (std::size_t index = 0; index < kept.size(); ++index)
        kept[index] = index;
    while (static_cast<int>(kept.size()) > voice_count) {
        std::size_t victim = 0;
        for (std::size_t index = 0; index < kept.size(); ++index) {
            const int rank = omission_rank(intervals[kept[index]]);
            const int best = omission_rank(intervals[kept[victim]]);
            if (rank < best || (rank == best && intervals[kept[index]] > intervals[kept[victim]]))
                victim = index;
        }
        kept.erase(kept.begin() + static_cast<std::ptrdiff_t>(victim));
    }
    for (const auto index : kept)
        plan.required_pitch_classes.push_back(plan.chord_pitch_classes[index]);
    if (step.bass_pitch_class) {
        if (*step.bass_pitch_class < 0 || *step.bass_pitch_class > 11)
            return make_error(ErrorCode::invalid_argument, "progression bass pitch class must lie in 0..11");
        plan.bass_pitch_class = *step.bass_pitch_class;
    } else if (step.inversion) {
        if (*step.inversion < 0 || *step.inversion >= static_cast<int>(plan.chord_pitch_classes.size()))
            return make_error(ErrorCode::invalid_argument, "progression inversion exceeds the chord tones");
        plan.bass_pitch_class = plan.chord_pitch_classes[static_cast<std::size_t>(*step.inversion)];
    }
    if (plan.bass_pitch_class) {
        // A required bass tone replaces the least important required tone when voices are scarce.
        const auto bass = *plan.bass_pitch_class;
        auto& required = plan.required_pitch_classes;
        if (std::ranges::find(required, bass) == required.end() && static_cast<int>(required.size()) >= voice_count &&
            !required.empty())
            required.pop_back();
    }
    return plan;
}

/// Weighted semitones by which a voicing leaves the user register.
std::int64_t register_cost(const std::vector<int>& voicing, const VoiceLeadingOptions& options) {
    std::int64_t outside = 0;
    for (const int pitch : voicing) {
        if (pitch < options.low_pitch)
            outside += options.low_pitch - pitch;
        if (pitch > options.high_pitch)
            outside += pitch - options.high_pitch;
    }
    return outside * options.weights.register_violation;
}

/// Movement, crossing and common-tone terms between two voicings of equal size.
std::int64_t transition_cost(const std::vector<int>& from, const std::vector<int>& to,
                             const VoiceLeadingOptions& options) {
    std::int64_t movement = 0;
    std::int64_t crossings = 0;
    std::int64_t common = 0;
    for (std::size_t voice = 0; voice < to.size(); ++voice) {
        movement += std::abs(to[voice] - from[voice]);
        if (to[voice] == from[voice])
            ++common;
        if (voice + 1 < to.size()) {
            if (to[voice] > from[voice + 1])
                ++crossings;
            if (to[voice + 1] < from[voice])
                ++crossings;
        }
    }
    std::int64_t cost = movement * options.weights.movement + crossings * options.weights.crossing;
    if (options.retain_common_tones)
        cost -= common * options.weights.common_tone;
    return cost;
}

/// Enumerates ascending voicings: the lowest voice carries the bass pitch class when one is
/// required, upper adjacent voices are at most an octave apart, and every required pitch class
/// is present.
std::vector<std::vector<int>> enumerate_voicings(const StepPlan& plan, int voice_count,
                                                 const VoiceLeadingOptions& options, bool& overflow) {
    const int low = std::max(0, options.low_pitch - options.register_tolerance);
    const int high = std::min(127, options.high_pitch + options.register_tolerance);
    std::vector<int> chord_pitches;
    std::vector<int> bass_pitches;
    for (int pitch = low; pitch <= high; ++pitch) {
        const int pitch_class = pitch % 12;
        const bool chord_tone =
            std::ranges::find(plan.chord_pitch_classes, pitch_class) != plan.chord_pitch_classes.end();
        if (chord_tone)
            chord_pitches.push_back(pitch);
        if (plan.bass_pitch_class ? pitch_class == *plan.bass_pitch_class : chord_tone)
            bass_pitches.push_back(pitch);
    }
    std::vector<std::vector<int>> result;
    std::vector<int> current;
    const auto covers = [&](const std::vector<int>& voicing) {
        for (const int pitch_class : plan.required_pitch_classes) {
            bool found = false;
            for (const int pitch : voicing)
                found = found || pitch % 12 == pitch_class;
            if (!found)
                return false;
        }
        return true;
    };
    const auto extend = [&](auto&& self, std::size_t next_index) -> void {
        if (overflow)
            return;
        if (static_cast<int>(current.size()) == voice_count) {
            if (covers(current)) {
                result.push_back(current);
                if (result.size() >= enumeration_bound)
                    overflow = true;
            }
            return;
        }
        for (std::size_t index = next_index; index < chord_pitches.size(); ++index) {
            const int pitch = chord_pitches[index];
            if (current.size() >= 2 && pitch - current.back() > 12)
                break;
            current.push_back(pitch);
            self(self, index + 1);
            current.pop_back();
        }
    };
    for (const int bass : bass_pitches) {
        current.assign(1, bass);
        const auto first_upper = std::ranges::upper_bound(chord_pitches, bass) - chord_pitches.begin();
        extend(extend, static_cast<std::size_t>(first_upper));
    }
    return result;
}

} // namespace

Result<VoiceLeadingResult> voice_lead(std::span<const ProgressionStep> steps, const VoiceLeadingOptions& options) {
    if (steps.empty())
        return make_error(ErrorCode::invalid_argument, "a progression needs at least one step");
    if (options.low_pitch < 0 || options.high_pitch > 127 || options.low_pitch > options.high_pitch)
        return make_error(ErrorCode::invalid_argument, "voice-leading register must lie inside 0..127");
    if (options.register_tolerance < 0 || options.register_tolerance > 24)
        return make_error(ErrorCode::invalid_argument, "register tolerance must lie in 0..24");
    if (options.candidate_limit == 0)
        return make_error(ErrorCode::invalid_argument, "candidate limit must be positive");
    if (options.voice_count < 0 || options.voice_count > 8)
        return make_error(ErrorCode::invalid_argument, "voice count must lie in 0..8");
    int voice_count = options.voice_count;
    if (voice_count == 0) {
        for (const auto& step : steps) {
            std::vector<int> distinct;
            for (const int interval : chord_intervals(step.type, step.extensions))
                if (std::ranges::find(distinct, interval % 12) == distinct.end())
                    distinct.push_back(interval % 12);
            voice_count = std::max(voice_count, std::max(3, static_cast<int>(distinct.size())));
        }
        voice_count = std::min(voice_count, 8);
    }
    if (!options.start_voicing.empty() && static_cast<int>(options.start_voicing.size()) != voice_count)
        return make_error(ErrorCode::invalid_argument, "start voicing must have one pitch per voice");

    VoiceLeadingResult result;
    result.weights = options.weights;
    result.candidate_limit = options.candidate_limit;
    std::vector<std::vector<std::vector<int>>> candidates;
    const int center_twice = options.low_pitch + options.high_pitch;
    for (const auto& step : steps) {
        auto plan = plan_step(step, voice_count);
        if (!plan)
            return std::unexpected(plan.error());
        bool overflow = false;
        auto voicings = enumerate_voicings(*plan, voice_count, options, overflow);
        if (voicings.empty())
            return make_error(ErrorCode::invalid_argument,
                              "no voicing of a progression chord fits the register and voice count");
        result.enumerated_counts.push_back(voicings.size());
        // Static ranking for the candidate limit: register cost, then distance of the voicing's
        // centre from the register centre, then enumeration order (stable sort).
        std::ranges::stable_sort(voicings, [&](const std::vector<int>& a, const std::vector<int>& b) {
            const auto cost_a = register_cost(a, options);
            const auto cost_b = register_cost(b, options);
            if (cost_a != cost_b)
                return cost_a < cost_b;
            const auto centre = [&](const std::vector<int>& voicing) {
                return std::abs(voicing.front() + voicing.back() - center_twice);
            };
            return centre(a) < centre(b);
        });
        if (voicings.size() > options.candidate_limit || overflow) {
            voicings.resize(std::min(voicings.size(), options.candidate_limit));
            result.truncated = true;
        }
        result.kept_counts.push_back(voicings.size());
        candidates.push_back(std::move(voicings));
    }

    // Dynamic programming over steps; ties keep the earlier candidate.
    constexpr std::int64_t unreachable = std::numeric_limits<std::int64_t>::max();
    std::vector<std::vector<std::int64_t>> best(candidates.size());
    std::vector<std::vector<std::size_t>> previous(candidates.size());
    for (std::size_t step = 0; step < candidates.size(); ++step) {
        best[step].assign(candidates[step].size(), unreachable);
        previous[step].assign(candidates[step].size(), 0);
        for (std::size_t index = 0; index < candidates[step].size(); ++index) {
            const auto& candidate = candidates[step][index];
            const std::int64_t own = register_cost(candidate, options);
            if (step == 0) {
                best[step][index] = own + (options.start_voicing.empty()
                                               ? 0
                                               : transition_cost(options.start_voicing, candidate, options));
                continue;
            }
            for (std::size_t from = 0; from < candidates[step - 1].size(); ++from) {
                const std::int64_t total =
                    best[step - 1][from] + own + transition_cost(candidates[step - 1][from], candidate, options);
                if (total < best[step][index]) {
                    best[step][index] = total;
                    previous[step][index] = from;
                }
            }
        }
    }
    const auto& last = best.back();
    std::size_t chosen = static_cast<std::size_t>(std::ranges::min_element(last) - last.begin());
    result.total_cost = last[chosen];
    std::vector<std::size_t> path(candidates.size());
    for (std::size_t step = candidates.size(); step-- > 0;) {
        path[step] = chosen;
        chosen = previous[step][chosen];
    }
    for (std::size_t step = 0; step < candidates.size(); ++step) {
        result.voicings.push_back(candidates[step][path[step]]);
        result.step_costs.push_back(step == 0 ? best[0][path[0]] : best[step][path[step]] - best[step - 1][path[step - 1]]);
    }
    return result;
}

Result<std::vector<GeneratedNote>> render_progression(std::span<const ProgressionStep> steps,
                                                      const VoiceLeadingResult& voicing, Tick start) {
    if (voicing.voicings.size() != steps.size())
        return make_error(ErrorCode::invalid_argument, "voicing result does not match the progression");
    if (start < 0)
        return make_error(ErrorCode::invalid_argument, "progression start must not be negative");
    std::vector<GeneratedNote> notes;
    Tick cursor = start;
    for (std::size_t index = 0; index < steps.size(); ++index) {
        const auto& step = steps[index];
        const auto length = mul_div_round(step.duration, step.gate.numerator, step.gate.denominator);
        if (!length)
            return make_error(ErrorCode::limit_exceeded, "progression step length overflows");
        for (const int pitch : voicing.voicings[index]) {
            if (pitch < 0 || pitch > 127)
                return make_error(ErrorCode::invalid_argument, "voicing pitch outside 0..127");
            notes.push_back(GeneratedNote{cursor, cursor + std::max<Tick>(1, *length), pitch, step.velocity});
        }
        const auto next = checked_add(cursor, step.duration);
        if (!next)
            return make_error(ErrorCode::limit_exceeded, "progression end overflows");
        cursor = *next;
    }
    return notes;
}

std::string progression_to_json(std::span<const ProgressionStep> steps, const VoiceLeadingOptions& options) {
    nlohmann::json root;
    root["format"] = "mwb.progression";
    root["version"] = voice_leading_algorithm_version;
    nlohmann::json list = nlohmann::json::array();
    for (const auto& step : steps) {
        nlohmann::json item;
        item["root"] = step.root_pitch_class;
        item["type"] = std::string(chord_type_info(step.type).id);
        item["extensions"] = step.extensions;
        item["duration"] = step.duration;
        item["inversion"] = step.inversion ? nlohmann::json(*step.inversion) : nlohmann::json(nullptr);
        item["bass"] = step.bass_pitch_class ? nlohmann::json(*step.bass_pitch_class) : nlohmann::json(nullptr);
        item["velocity"] = step.velocity;
        item["gate"] = step.gate.to_string();
        list.push_back(std::move(item));
    }
    root["steps"] = std::move(list);
    nlohmann::json settings;
    settings["low_pitch"] = options.low_pitch;
    settings["high_pitch"] = options.high_pitch;
    settings["register_tolerance"] = options.register_tolerance;
    settings["voice_count"] = options.voice_count;
    settings["candidate_limit"] = options.candidate_limit;
    settings["retain_common_tones"] = options.retain_common_tones;
    settings["weight_movement"] = options.weights.movement;
    settings["weight_crossing"] = options.weights.crossing;
    settings["weight_register"] = options.weights.register_violation;
    settings["weight_common_tone"] = options.weights.common_tone;
    settings["start_voicing"] = options.start_voicing;
    root["options"] = std::move(settings);
    return root.dump();
}

Result<std::pair<std::vector<ProgressionStep>, VoiceLeadingOptions>> progression_from_json(std::string_view json) {
    auto parsed = parse_json_object(json);
    if (!parsed)
        return std::unexpected(parsed.error());
    FieldReader reader(*parsed, "progression");
    std::string format;
    int version = 0;
    reader.text("format", format);
    reader.integer("version", version);
    if (reader.status() && (format != "mwb.progression" || version != voice_leading_algorithm_version))
        return make_error(ErrorCode::unsupported, "unsupported progression format or version");
    std::vector<ProgressionStep> steps;
    if (const auto* list = reader.array("steps")) {
        for (const auto& item : *list) {
            FieldReader step_reader(item, "progression.steps");
            ProgressionStep step;
            step_reader.integer("root", step.root_pitch_class);
            std::string type = std::string(chord_type_info(step.type).id);
            step_reader.text("type", type);
            const auto parsed_type = chord_type_from_id(type);
            if (!parsed_type)
                step_reader.fail("type", "unknown chord type");
            else
                step.type = *parsed_type;
            step_reader.integer("extensions", step.extensions);
            step_reader.integer("duration", step.duration);
            step_reader.optional_integer("inversion", step.inversion);
            step_reader.optional_integer("bass", step.bass_pitch_class);
            step_reader.integer("velocity", step.velocity);
            step_reader.rational("gate", step.gate);
            MWB_TRY(step_reader.status());
            steps.push_back(step);
        }
    }
    VoiceLeadingOptions options;
    if (const auto* settings = reader.object("options")) {
        FieldReader options_reader(*settings, "progression.options");
        options_reader.integer("low_pitch", options.low_pitch);
        options_reader.integer("high_pitch", options.high_pitch);
        options_reader.integer("register_tolerance", options.register_tolerance);
        options_reader.integer("voice_count", options.voice_count);
        options_reader.integer("candidate_limit", options.candidate_limit);
        options_reader.boolean("retain_common_tones", options.retain_common_tones);
        options_reader.integer("weight_movement", options.weights.movement);
        options_reader.integer("weight_crossing", options.weights.crossing);
        options_reader.integer("weight_register", options.weights.register_violation);
        options_reader.integer("weight_common_tone", options.weights.common_tone);
        options_reader.integer_list("start_voicing", options.start_voicing);
        MWB_TRY(options_reader.status());
    }
    MWB_TRY(reader.status());
    return std::pair{std::move(steps), std::move(options)};
}

} // namespace mwb::music
