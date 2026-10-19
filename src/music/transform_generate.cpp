#include "mwb/music/transform.hpp"

#include "mwb/core/checked.hpp"
#include "mwb/core/random.hpp"
#include "mwb/core/serialization.hpp"
#include "mwb/music/json_fields.hpp"

#include <algorithm>
#include <format>
#include <map>

namespace mwb::music {

namespace {

constexpr std::int64_t max_generated_steps = 65536;

/// Mathematical modulo with a non-negative result.
std::int64_t positive_modulo(std::int64_t value, std::int64_t modulus) { return ((value % modulus) + modulus) % modulus; }

/// Gate fractions lie in (0, 1].
bool valid_gate(const Rational& gate) {
    return gate.denominator > 0 && gate.numerator > 0 && gate.numerator <= gate.denominator;
}

/// round(length * gate) for an exact step length, at least one tick.
Tick gated_length(const Rational& step, const Rational& gate) {
    const __int128 numerator = static_cast<__int128>(step.numerator) * gate.numerator;
    const __int128 denominator = static_cast<__int128>(step.denominator) * gate.denominator;
    auto exact = make_rational(numerator, denominator);
    return exact ? std::max<Tick>(1, round_rational(*exact)) : 1;
}

/// Exact difference of two fractions.
Rational subtract(const Rational& left, const Rational& right) {
    const __int128 numerator = static_cast<__int128>(left.numerator) * right.denominator -
                               static_cast<__int128>(right.numerator) * left.denominator;
    auto value = make_rational(numerator, static_cast<__int128>(left.denominator) * right.denominator);
    return value ? *value : Rational{0, 1};
}

/// True for fractions in [0, 1] with a positive denominator.
bool unit_fraction(const Rational& value) {
    return value.denominator > 0 && value.numerator >= 0 && value.numerator <= value.denominator;
}

/// Range checks for a generated note before insertion.
[[nodiscard]] Status validate_note(const GeneratedNote& note) {
    if (note.pitch < 0 || note.pitch > 127)
        return make_error(ErrorCode::invalid_argument, "generated pitch outside 0..127");
    if (note.velocity < 1 || note.velocity > 127)
        return make_error(ErrorCode::invalid_argument, "generated velocity outside 1..127");
    if (note.start < 0 || note.end <= note.start)
        return make_error(ErrorCode::invalid_argument, "generated notes need a non-negative start and positive length");
    return ok();
}


} // namespace

// --- Strum -----------------------------------------------------------------------------------------

Result<TransformPreview> strum(const DocumentData& document, const TargetSelection& selection,
                               const StrumParameters& parameters) {
    if (parameters.spread < 0)
        return make_error(ErrorCode::invalid_argument, "strum spread must not be negative");
    if (parameters.chord_tolerance && *parameters.chord_tolerance < 0)
        return make_error(ErrorCode::invalid_argument, "chord tolerance must not be negative");
    auto prepared = prepare_transform(document, selection, "strum", false);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    DeterministicRandom random(parameters.seed);
    const auto groups =
        onset_groups(editor, selected, parameters.chord_tolerance.value_or(default_onset_tolerance(editor.ppq())));
    for (std::size_t group_index = 0; group_index < groups.size(); ++group_index) {
        std::vector<std::size_t> order = groups[group_index];
        if (order.size() < 2)
            continue;
        std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
            return editor.values(a).pitch < editor.values(b).pitch;
        });
        bool descending = parameters.order == StrumOrder::down;
        if (parameters.order == StrumOrder::alternate)
            descending = group_index % 2 == 1;
        if (descending)
            std::ranges::reverse(order);
        if (parameters.order == StrumOrder::random) {
            for (std::size_t index = order.size() - 1; index > 0; --index) {
                const auto other = random.bounded(static_cast<std::uint32_t>(index + 1));
                std::swap(order[index], order[other]);
            }
        }
        Tick group_start = editor.values(order.front()).start;
        for (const auto note : order)
            group_start = std::min(group_start, editor.values(note).start);
        const auto count = static_cast<std::int64_t>(order.size());
        for (std::int64_t position = 0; position < count; ++position) {
            const auto note = order[static_cast<std::size_t>(position)];
            const NoteValues before = editor.values(note);
            NoteValues after = before;
            after.start = group_start + mul_div_round(parameters.spread, position, count - 1).value_or(0);
            if (!editor.is_unterminated(note)) {
                if (!parameters.preserve_chord_grouping)
                    after.end = before.end + (after.start - before.start);
                if (after.end <= after.start) {
                    after.end = after.start + 1;
                    ++report.clipped;
                }
            }
            report.max_movement = std::max(report.max_movement, std::abs(after.start - before.start));
            if (after != before) {
                editor.update(note, after);
                ++report.changed;
            }
        }
    }
    return finish_preview(editor, std::move(report), "Strum");
}

// --- Arpeggiate --------------------------------------------------------------------------------------

Result<TransformPreview> arpeggiate(const DocumentData& document, const TargetSelection& selection,
                                    const ArpeggiateParameters& parameters) {
    if (parameters.octaves < 1 || parameters.octaves > 4)
        return make_error(ErrorCode::invalid_argument, "arpeggio octave range must lie in 1..4");
    if (!valid_gate(parameters.gate))
        return make_error(ErrorCode::invalid_argument, "arpeggio gate must lie in (0, 1]");
    if (parameters.chord_tolerance && *parameters.chord_tolerance < 0)
        return make_error(ErrorCode::invalid_argument, "chord tolerance must not be negative");
    auto prepared = prepare_transform(document, selection, "arpeggiate", true);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    DeterministicRandom random(parameters.seed);
    const auto groups =
        onset_groups(editor, selected, parameters.chord_tolerance.value_or(default_onset_tolerance(editor.ppq())));
    std::vector<EventId> created;
    for (const auto& group : groups) {
        if (std::ranges::any_of(group, [&](std::size_t note) { return editor.is_unterminated(note); })) {
            report.skipped += group.size();
            continue;
        }
        std::vector<std::size_t> played = group;
        std::ranges::stable_sort(played, [&](std::size_t a, std::size_t b) {
            return std::make_pair(editor.values(a).start, a) < std::make_pair(editor.values(b).start, b);
        });
        Tick chord_start = editor.values(played.front()).start;
        Tick chord_end = 0;
        std::map<int, int> velocity_by_pitch;
        std::vector<int> played_pitches;
        for (const auto note : played) {
            const auto values = editor.values(note);
            chord_start = std::min(chord_start, values.start);
            chord_end = std::max(chord_end, values.end);
            if (!velocity_by_pitch.contains(values.pitch)) {
                velocity_by_pitch[values.pitch] = values.velocity;
                played_pitches.push_back(values.pitch);
            }
        }
        std::vector<int> base = played_pitches;
        if (parameters.pattern != ArpeggioPattern::as_played)
            std::ranges::sort(base);
        std::vector<int> pitches;
        std::vector<int> velocities;
        for (int octave = 0; octave < parameters.octaves; ++octave) {
            for (const int pitch : base) {
                const int shifted = pitch + 12 * octave;
                if (shifted > 127) {
                    ++report.skipped;
                    continue;
                }
                pitches.push_back(shifted);
                velocities.push_back(velocity_by_pitch[pitch]);
            }
        }
        std::vector<std::size_t> cycle;
        switch (parameters.pattern) {
        case ArpeggioPattern::up:
        case ArpeggioPattern::as_played:
        case ArpeggioPattern::random:
            for (std::size_t index = 0; index < pitches.size(); ++index)
                cycle.push_back(index);
            break;
        case ArpeggioPattern::down:
            for (std::size_t index = pitches.size(); index-- > 0;)
                cycle.push_back(index);
            break;
        case ArpeggioPattern::up_down:
            for (std::size_t index = 0; index < pitches.size(); ++index)
                cycle.push_back(index);
            for (std::size_t index = pitches.size() - 1; index-- > 1;)
                cycle.push_back(index);
            break;
        }
        auto grid = Grid::make(editor.ppq(), GridSpec{parameters.rate, parameters.swing, chord_start});
        if (!grid)
            return std::unexpected(grid.error());
        const NoteValues first = editor.values(played.front());
        const std::size_t track = editor.notes()[played.front()].on_track;
        for (std::int64_t step = 0;; ++step) {
            if (step >= max_generated_steps)
                return make_error(ErrorCode::limit_exceeded, "arpeggio would create too many notes");
            const Tick start = grid->point_tick(step);
            if (start >= chord_end)
                break;
            const std::size_t choice =
                parameters.pattern == ArpeggioPattern::random
                    ? random.bounded(static_cast<std::uint32_t>(pitches.size()))
                    : cycle[static_cast<std::size_t>(step % static_cast<std::int64_t>(cycle.size()))];
            const Tick length = gated_length(subtract(grid->point(step + 1), grid->point(step)), parameters.gate);
            NoteValues values;
            values.start = start;
            values.end = std::max(start + 1, std::min(start + length, chord_end));
            values.pitch = pitches[choice];
            values.velocity = velocities[choice];
            values.channel = first.channel;
            created.push_back(editor.add(track, values));
            ++report.created;
        }
        for (const auto note : group) {
            editor.remove(note);
            ++report.removed;
        }
    }
    auto preview = finish_preview(editor, std::move(report), "Arpeggiate");
    preview.created_notes = std::move(created);
    return preview;
}

// --- Euclidean rhythm ----------------------------------------------------------------------------------

Result<std::vector<bool>> euclidean_pattern(int steps, int pulses, int rotation) {
    if (steps < 1 || steps > 256)
        return make_error(ErrorCode::invalid_argument, "Euclidean steps must lie in 1..256");
    if (pulses < 0 || pulses > steps)
        return make_error(ErrorCode::invalid_argument, "Euclidean pulses must lie in 0..steps");
    std::vector<bool> pattern(static_cast<std::size_t>(steps));
    for (int step = 0; step < steps; ++step) {
        const int source = static_cast<int>(positive_modulo(static_cast<std::int64_t>(step) - rotation, steps));
        pattern[static_cast<std::size_t>(step)] = (static_cast<std::int64_t>(source) * pulses) % steps < pulses;
    }
    return pattern;
}

Result<TransformPreview> euclidean_rhythm(const DocumentData& document, const TargetSelection& selection,
                                          const EuclideanParameters& parameters) {
    auto pattern = euclidean_pattern(parameters.steps, parameters.pulses, parameters.rotation);
    if (!pattern)
        return std::unexpected(pattern.error());
    if (parameters.pitch < 0 || parameters.pitch > 127 || parameters.channel > 15)
        return make_error(ErrorCode::invalid_argument, "Euclidean pitch or channel out of range");
    if (parameters.velocity < 1 || parameters.velocity > 127 || parameters.accent_velocity < 1 ||
        parameters.accent_velocity > 127)
        return make_error(ErrorCode::invalid_argument, "Euclidean velocities must lie in 1..127");
    if (parameters.cycles < 1 || parameters.cycles > 1024 || parameters.start < 0)
        return make_error(ErrorCode::invalid_argument, "Euclidean cycles must lie in 1..1024 and start at tick 0 or later");
    if (!valid_gate(parameters.gate))
        return make_error(ErrorCode::invalid_argument, "Euclidean gate must lie in (0, 1]");
    if (selection.tracks.empty())
        return make_error(ErrorCode::invalid_argument, "Euclidean rhythm needs a destination track");
    TargetSelection destination = selection;
    destination.notes.clear();
    auto prepared = prepare_transform(document, destination, "euclidean", true);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    report.targeted = 0;
    const auto track = editor.sequence().track_index(selection.tracks.front());
    auto grid = Grid::make(editor.ppq(), GridSpec{parameters.step_length, Rational{1, 2}, parameters.start});
    if (!grid)
        return std::unexpected(grid.error());
    const Tick length = gated_length(grid->step_ticks(), parameters.gate);
    std::vector<EventId> created;
    for (int cycle = 0; cycle < parameters.cycles; ++cycle) {
        int pulse = 0;
        for (int step = 0; step < parameters.steps; ++step) {
            if (!(*pattern)[static_cast<std::size_t>(step)])
                continue;
            const std::int64_t index = static_cast<std::int64_t>(cycle) * parameters.steps + step;
            NoteValues values;
            values.start = grid->point_tick(index);
            values.end = values.start + length;
            values.pitch = parameters.pitch;
            values.channel = parameters.channel;
            values.velocity = std::ranges::find(parameters.accent_pulses, pulse) != parameters.accent_pulses.end()
                                  ? parameters.accent_velocity
                                  : parameters.velocity;
            created.push_back(editor.add(*track, values));
            ++report.created;
            ++pulse;
        }
    }
    auto preview = finish_preview(editor, std::move(report), "Euclidean rhythm");
    preview.created_notes = std::move(created);
    return preview;
}

// --- Groove ---------------------------------------------------------------------------------------------

Result<Groove> extract_groove(const DocumentData& document, const TargetSelection& selection,
                              const GrooveExtractParameters& parameters) {
    if (parameters.steps < 1 || parameters.steps > 1024)
        return make_error(ErrorCode::invalid_argument, "groove steps must lie in 1..1024");
    auto prepared = prepare_transform(document, selection, "groove_extract", true);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    auto grid = Grid::make(editor.ppq(), parameters.grid);
    if (!grid)
        return std::unexpected(grid.error());
    const auto steps = static_cast<std::size_t>(parameters.steps);
    std::vector<std::int64_t> offset_sums(steps, 0);
    std::vector<std::int64_t> velocity_sums(steps, 0);
    std::vector<std::int64_t> counts(steps, 0);
    std::int64_t total_velocity = 0;
    std::int64_t total_count = 0;
    for (const auto note : selected) {
        const auto values = editor.values(note);
        const auto index = grid->nearest_index(values.start);
        const auto slot = static_cast<std::size_t>(positive_modulo(index, parameters.steps));
        offset_sums[slot] += values.start - grid->point_tick(index);
        velocity_sums[slot] += values.velocity;
        ++counts[slot];
        total_velocity += values.velocity;
        ++total_count;
    }
    Groove groove;
    groove.ppq = editor.ppq();
    groove.grid = parameters.grid;
    groove.steps = parameters.steps;
    for (std::size_t slot = 0; slot < steps; ++slot) {
        groove.sample_counts.push_back(static_cast<int>(counts[slot]));
        if (counts[slot] == 0) {
            groove.timing_offsets.push_back(0);
            groove.velocity_offsets.push_back(0);
            continue;
        }
        groove.timing_offsets.push_back(mul_div_round(offset_sums[slot], 1, counts[slot]).value_or(0));
        const std::int64_t numerator = velocity_sums[slot] * total_count - total_velocity * counts[slot];
        groove.velocity_offsets.push_back(
            static_cast<int>(mul_div_round(numerator, 1, counts[slot] * total_count).value_or(0)));
    }
    return groove;
}

Result<TransformPreview> apply_groove(const DocumentData& document, const TargetSelection& selection,
                                      const GrooveApplyParameters& parameters) {
    const auto& groove = parameters.groove;
    if (groove.ppq <= 0 || groove.steps < 1 || groove.timing_offsets.size() != static_cast<std::size_t>(groove.steps) ||
        groove.velocity_offsets.size() != static_cast<std::size_t>(groove.steps))
        return make_error(ErrorCode::invalid_argument, "groove template is inconsistent");
    if (!unit_fraction(parameters.strength) || !unit_fraction(parameters.velocity_strength))
        return make_error(ErrorCode::invalid_argument, "groove strengths must lie between 0 and 1");
    auto prepared = prepare_transform(document, selection, "groove_apply", true);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    auto grid = Grid::make(editor.ppq(), parameters.target_grid);
    if (!grid)
        return std::unexpected(grid.error());
    for (const auto note : selected) {
        const NoteValues before = editor.values(note);
        const auto index = grid->nearest_index(before.start);
        const auto slot = static_cast<std::size_t>(positive_modulo(index, groove.steps));
        const auto offset = mul_div_round(groove.timing_offsets[slot], editor.ppq(), groove.ppq);
        if (!offset)
            return make_error(ErrorCode::limit_exceeded, "groove offset overflows");
        const Rational point = grid->point(index);
        auto target = make_rational(static_cast<__int128>(point.numerator) +
                                        static_cast<__int128>(*offset) * point.denominator,
                                    point.denominator);
        if (!target)
            return std::unexpected(target.error());
        auto moved = move_toward(before.start, *target, parameters.strength);
        if (!moved)
            return std::unexpected(moved.error());
        NoteValues after = before;
        after.start = moved->first;
        if (compare_rational(moved->second, report.max_rounding_error) > 0)
            report.max_rounding_error = moved->second;
        if (after.start < 0) {
            after.start = 0;
            ++report.clipped;
        }
        if (!editor.is_unterminated(note)) {
            if (parameters.preserve_duration)
                after.end = before.end + (after.start - before.start);
            if (after.end <= after.start) {
                after.end = after.start + 1;
                ++report.clipped;
            }
        }
        const std::int64_t velocity =
            before.velocity + mul_div_round(groove.velocity_offsets[slot], parameters.velocity_strength.numerator,
                                            parameters.velocity_strength.denominator)
                                  .value_or(0);
        after.velocity = static_cast<int>(std::clamp<std::int64_t>(velocity, 1, 127));
        if (after.velocity != velocity)
            ++report.clipped;
        report.max_movement = std::max(report.max_movement, std::abs(after.start - before.start));
        if (after != before) {
            editor.update(note, after);
            ++report.changed;
        }
    }
    return finish_preview(editor, std::move(report), "Apply groove");
}

std::string groove_to_json(const Groove& groove) {
    nlohmann::json root;
    root["format"] = "mwb.groove";
    root["version"] = 1;
    root["ppq"] = groove.ppq;
    root["grid"] = nlohmann::json{{"step", groove.grid.step.to_string()},
                                  {"swing", groove.grid.swing.to_string()},
                                  {"origin", groove.grid.origin}};
    root["steps"] = groove.steps;
    root["timing"] = groove.timing_offsets;
    root["velocity"] = groove.velocity_offsets;
    root["samples"] = groove.sample_counts;
    return root.dump();
}

Result<Groove> groove_from_json(std::string_view json) {
    auto parsed = parse_json_object(json);
    if (!parsed)
        return std::unexpected(parsed.error());
    FieldReader reader(*parsed, "groove");
    std::string format;
    int version = 0;
    reader.text("format", format);
    reader.integer("version", version);
    MWB_TRY(reader.status());
    if (format != "mwb.groove" || version != 1)
        return make_error(ErrorCode::unsupported, "unsupported groove format or version");
    Groove groove;
    reader.integer("ppq", groove.ppq);
    reader.integer("steps", groove.steps);
    if (const auto* grid = reader.object("grid")) {
        FieldReader grid_reader(*grid, "groove.grid");
        grid_reader.rational("step", groove.grid.step);
        grid_reader.rational("swing", groove.grid.swing);
        grid_reader.integer("origin", groove.grid.origin);
        MWB_TRY(grid_reader.status());
    }
    std::vector<std::int64_t> timing;
    reader.integer_list("timing", timing);
    groove.timing_offsets = timing;
    reader.integer_list("velocity", groove.velocity_offsets);
    reader.integer_list("samples", groove.sample_counts);
    MWB_TRY(reader.status());
    if (groove.steps < 1 || groove.timing_offsets.size() != static_cast<std::size_t>(groove.steps) ||
        groove.velocity_offsets.size() != static_cast<std::size_t>(groove.steps))
        return make_error(ErrorCode::invalid_data, "groove slot arrays do not match the step count");
    return groove;
}

// --- Generators -------------------------------------------------------------------------------------------

Result<TransformPreview> insert_notes(const DocumentData& document, TrackId track, std::span<const GeneratedNote> notes,
                                      std::uint8_t channel, std::string tool) {
    if (channel > 15)
        return make_error(ErrorCode::invalid_argument, "channel must lie in 0..15");
    const auto location = find_track(document, track);
    if (!location)
        return make_error(ErrorCode::not_found, "the destination track does not exist");
    for (const auto& note : notes)
        MWB_TRY(validate_note(note));
    auto editor = NoteEditor::open(document, document.sequences[location->sequence].id);
    if (!editor)
        return std::unexpected(editor.error());
    TransformReport report;
    report.tool = tool;
    std::vector<EventId> created;
    for (const auto& note : notes) {
        NoteValues values;
        values.start = note.start;
        values.end = note.end;
        values.pitch = note.pitch;
        values.velocity = note.velocity;
        values.channel = channel;
        created.push_back(editor->add(location->track, values));
        ++report.created;
    }
    auto preview = finish_preview(*editor, std::move(report), "Insert " + tool);
    preview.created_notes = std::move(created);
    return preview;
}

Result<TransformPreview> insert_chord(const DocumentData& document, TrackId track, const ChordSpec& chord,
                                      const ChordRenderSpec& render, OutOfRangeAction out_of_range) {
    auto voicing = voice_chord(chord);
    if (!voicing)
        return std::unexpected(voicing.error());
    if (!voicing->out_of_range.empty() && out_of_range == OutOfRangeAction::reject)
        return make_error(ErrorCode::invalid_argument,
                          std::format("{} chord tones lie outside the register", voicing->out_of_range.size()),
                          voicing->symbol);
    std::vector<int> pitches;
    for (const int pitch : voicing->pitches)
        if (std::ranges::find(voicing->out_of_range, pitch) == voicing->out_of_range.end())
            pitches.push_back(pitch);
    auto notes = render_chord(pitches, render);
    if (!notes)
        return std::unexpected(notes.error());
    auto preview = insert_notes(document, track, *notes, render.channel, "chord");
    if (preview) {
        preview->report.skipped = voicing->out_of_range.size();
        preview->report.messages.push_back("Chord " + voicing->symbol);
    }
    return preview;
}

Result<TransformPreview> insert_progression(const DocumentData& document, TrackId track,
                                            std::span<const ProgressionStep> steps, const VoiceLeadingOptions& options,
                                            Tick start, std::uint8_t channel) {
    auto voiced = voice_lead(steps, options);
    if (!voiced)
        return std::unexpected(voiced.error());
    auto notes = render_progression(steps, *voiced, start);
    if (!notes)
        return std::unexpected(notes.error());
    auto preview = insert_notes(document, track, *notes, channel, "progression");
    if (!preview)
        return preview;
    const auto location = find_track(document, track);
    const Sequence& sequence = document.sequences[location->sequence];
    Sequence before = sequence;
    before.tracks.clear();
    Sequence after = before;
    Recipe recipe;
    recipe.id = RecipeId::generate();
    recipe.tool_id = "progression";
    recipe.algorithm_version = voice_leading_algorithm_version;
    recipe.source_semantic_hash = semantic_hash(sequence);
    nlohmann::json parameters;
    parameters["start"] = start;
    parameters["channel"] = channel;
    parameters["progression"] = nlohmann::json::parse(progression_to_json(steps, options), nullptr, false);
    recipe.parameters_json = parameters.dump();
    recipe.targets_json = nlohmann::json{{"version", 1}, {"track", track.to_string()}}.dump();
    after.recipes.push_back(std::move(recipe));
    preview->changes.operations.push_back(SequencePropertiesChange{sequence.id, std::move(before), std::move(after)});
    preview->report.messages.push_back(std::format("Voice-leading cost {} (weights movement {}, crossing {}, register {}, "
                                                   "common tone {}; candidate limit {}{})",
                                                   voiced->total_cost, voiced->weights.movement,
                                                   voiced->weights.crossing, voiced->weights.register_violation,
                                                   voiced->weights.common_tone, voiced->candidate_limit,
                                                   voiced->truncated ? ", truncated" : ""));
    return preview;
}

} // namespace mwb::music
