#include "mwb/app/document_io.hpp"
#include "mwb/cli/cli.hpp"
#include "mwb/cli/commands.hpp"
#include "mwb/core/text.hpp"
#include "mwb/document/native_format.hpp"
#include "mwb/midi/codec.hpp"
#include "mwb/music/transform.hpp"

namespace mwb::cli {

namespace {

bool ends_with_extension(const std::string& path, const char* extension) {
    const std::string lowered = ascii_lower(path);
    const std::string suffix = std::string(".") + extension;
    return lowered.size() >= suffix.size() && lowered.compare(lowered.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

/// transform INPUT --recipe RECIPE.json --output FILE
/// A recipe is one transform object as written by parameters_to_json, or {"steps": [...]} with
/// several. Each step may carry "targets" in the selection_to_json form; by default a step targets
/// every note of the first sequence. The same recipe and input always produce the same events.
CommandOutput command_transform(const CommandOptions& options) {
    auto opened = app::open_document(app::path_from_utf8(options.input));
    if (!opened)
        return failure("transform", opened.error());
    auto recipe_bytes = app::read_file_bytes(app::path_from_utf8(options.recipe), 4u * 1024u * 1024u);
    if (!recipe_bytes)
        return failure("transform", recipe_bytes.error());
    const std::string recipe_text(recipe_bytes->begin(), recipe_bytes->end());
    const auto recipe = nlohmann::json::parse(recipe_text, nullptr, false);
    if (recipe.is_discarded() || !recipe.is_object())
        return failure("transform", Error{ErrorCode::invalid_argument, "recipe is not a JSON object", {}, {}});
    std::vector<nlohmann::json> steps;
    if (recipe.contains("steps")) {
        if (!recipe["steps"].is_array() || recipe["steps"].size() > 256)
            return failure("transform", Error{ErrorCode::invalid_argument, "recipe steps must be an array of up to 256", {}, {}});
        for (const auto& step : recipe["steps"])
            steps.push_back(step);
    } else {
        steps.push_back(recipe);
    }
    DocumentData document = std::move(opened->document);
    if (document.sequences.empty())
        return failure("transform", Error{ErrorCode::invalid_data, "document has no sequence", {}, {}});
    nlohmann::json reports = nlohmann::json::array();
    for (std::size_t index = 0; index < steps.size(); ++index) {
        const auto& step = steps[index];
        auto parameters = music::parameters_from_json(step.dump());
        if (!parameters)
            return failure("transform", parameters.error().with_context("step " + std::to_string(index + 1)));
        music::TargetSelection selection;
        selection.sequence = document.sequences.front().id;
        if (step.contains("targets")) {
            auto parsed = music::selection_from_json(step["targets"].is_string() ? step["targets"].get<std::string>()
                                                                                 : step["targets"].dump());
            if (!parsed)
                return failure("transform", parsed.error().with_context("step " + std::to_string(index + 1)));
            selection = *parsed;
        }
        auto preview = music::preview_transform(document, selection, *parameters);
        if (!preview)
            return failure("transform", preview.error().with_context("step " + std::to_string(index + 1)));
        if (auto applied = apply_change_set(document, preview->changes); !applied)
            return failure("transform", applied.error());
        if (auto stored = music::make_recipe(document, selection, *parameters)) {
            // Provenance travels with native output; SMF output carries only concrete events.
            const auto sequence_index = document.sequence_index(selection.sequence);
            if (sequence_index)
                document.sequences[*sequence_index].recipes.push_back(std::move(*stored));
        }
        const auto& report = preview->report;
        reports.push_back({{"tool", report.tool},
                           {"targeted", report.targeted},
                           {"changed", report.changed},
                           {"created", report.created},
                           {"removed", report.removed},
                           {"clipped", report.clipped},
                           {"skipped", report.skipped},
                           {"collisions", report.collisions},
                           {"max_movement", report.max_movement},
                           {"messages", report.messages}});
    }
    const auto path = app::path_from_utf8(options.output);
    if (ends_with_extension(options.output, "mwb")) {
        std::error_code error;
        if (!options.overwrite && std::filesystem::exists(path, error))
            return failure("transform", Error{ErrorCode::io, "destination exists; pass --overwrite to replace it", {}, {}});
        if (auto saved = document::save_native(document, path); !saved)
            return failure("transform", saved.error());
    } else {
        auto bytes = midi::export_midi(document, {});
        if (!bytes)
            return failure("transform", bytes.error());
        if (auto written = app::write_file_atomically(path, *bytes, options.overwrite); !written)
            return failure("transform", written.error());
    }
    CommandOutput output;
    output.json = {{"schema_version", 1},
                   {"status", "ok"},
                   {"command", "transform"},
                   {"summary", {{"steps", steps.size()}, {"output", options.output}}},
                   {"reports", std::move(reports)},
                   {"warnings", nlohmann::json::array()},
                   {"errors", nlohmann::json::array()}};
    return output;
}

} // namespace mwb::cli
