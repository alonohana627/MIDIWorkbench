#include "mwb/app/document_io.hpp"
#include "mwb/app/environment.hpp"
#include "mwb/cli/cli.hpp"
#include "mwb/cli/commands.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/core/tempo_map.hpp"
#include "mwb/core/text.hpp"
#include "mwb/document/native_format.hpp"
#include "mwb/midi/codec.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace mwb::cli {

namespace {

nlohmann::json division_json(const Division& division) {
    if (division.is_ppq())
        return {{"kind", "ppq"}, {"ppq", division.ppq}};
    return {{"kind", "smpte"},
            {"fps_code", static_cast<int>(division.smpte_format)},
            {"ticks_per_frame", division.ticks_per_frame}};
}

/// Per-sequence facts shared by inspect and validate.
nlohmann::json sequence_json(const Sequence& sequence) {
    const auto pairing = pair_notes(sequence);
    const auto tempo = TempoMap::build(sequence);
    const Tick end = sequence.effective_end();
    nlohmann::json value = {{"name", sequence.name},
                            {"division", division_json(sequence.division)},
                            {"end_tick", end},
                            {"duration_seconds", tempo.tick_to_seconds(end)},
                            {"track_count", sequence.tracks.size()},
                            {"note_count", pairing.notes.size()}};
    if (sequence.division.is_ppq()) {
        std::int64_t minimum = INT64_MAX;
        std::int64_t maximum = 0;
        for (const auto& segment : tempo.segments()) {
            minimum = std::min(minimum, segment.microseconds_per_quarter);
            maximum = std::max(maximum, segment.microseconds_per_quarter);
        }
        value["tempo"] = {{"origin", tempo.has_explicit_initial_tempo() ? "explicit" : "default"},
                          {"initial_bpm", 60'000'000.0 / static_cast<double>(tempo.tempo_at(0))},
                          {"min_bpm", 60'000'000.0 / static_cast<double>(maximum)},
                          {"max_bpm", 60'000'000.0 / static_cast<double>(minimum)},
                          {"changes", tempo.change_count()}};
        const auto meter = MeterMap::build(sequence);
        nlohmann::json meters = nlohmann::json::array();
        for (const auto& segment : meter.segments())
            meters.push_back({{"tick", segment.tick},
                              {"meter", std::to_string(segment.numerator) + "/" + std::to_string(segment.denominator)}});
        value["meters"] = std::move(meters);
    }
    nlohmann::json tracks = nlohmann::json::array();
    for (std::size_t index = 0; index < sequence.tracks.size(); ++index) {
        const auto& track = sequence.tracks[index];
        std::set<int> channels;
        std::set<int> programs;
        std::size_t notes = 0;
        for (const auto& event : track.events) {
            if (!event.is_channel())
                continue;
            channels.insert(event.channel() + 1);
            if (event.message() == ChannelMessage::program_change)
                programs.insert(event.data1() + 1);
            if (event.is_note_start())
                ++notes;
        }
        tracks.push_back({{"index", index},
                          {"name", track.name},
                          {"event_count", track.events.size()},
                          {"note_count", notes},
                          {"channels", channels},
                          {"programs", programs},
                          {"end_tick", track.end_tick},
                          {"drum_hint", track.is_drum}});
    }
    value["tracks"] = std::move(tracks);
    return value;
}

void split_diagnostics(const std::vector<Diagnostic>& diagnostics, bool strict, nlohmann::json& warnings,
                       nlohmann::json& errors) {
    for (const auto& diagnostic : diagnostics) {
        nlohmann::json item = diagnostics_json({diagnostic})[0];
        if (diagnostic.severity == Severity::error || (strict && diagnostic.severity == Severity::warning))
            errors.push_back(std::move(item));
        else if (diagnostic.severity == Severity::warning)
            warnings.push_back(std::move(item));
    }
}

nlohmann::json summary_json(const app::OpenedDocument& opened) {
    const auto& document = opened.document;
    std::size_t tracks = 0;
    std::size_t notes = 0;
    for (const auto& sequence : document.sequences) {
        tracks += sequence.tracks.size();
        notes += pair_notes(sequence).notes.size();
    }
    nlohmann::json summary = {{"container", opened.kind == app::FileKind::native
                                                ? "native"
                                                : midi::container_name(opened.import_report->container)},
                              {"title", document.title},
                              {"sequence_count", document.sequences.size()},
                              {"track_count", tracks},
                              {"note_count", notes}};
    if (opened.import_report) {
        summary["format"] = opened.import_report->format;
        summary["reader_result"] = opened.import_report->reader_result;
        summary["semantic_model_complete"] = opened.import_report->semantic_model_complete;
    }
    if (!document.sequences.empty())
        summary["division"] = division_json(document.sequences.front().division);
    return summary;
}

Result<std::pair<std::int32_t, std::int32_t>> parse_meter(const std::string& text) {
    const auto slash = text.find('/');
    if (slash == std::string::npos)
        return make_error(ErrorCode::invalid_argument, "meter must look like 4/4");
    try {
        const int numerator = std::stoi(text.substr(0, slash));
        const int denominator = std::stoi(text.substr(slash + 1));
        if (numerator < 1 || numerator > 255 || denominator < 1 || denominator > 64 ||
            (denominator & (denominator - 1)) != 0)
            return make_error(ErrorCode::invalid_argument, "meter denominator must be a power of two up to 64");
        return std::pair{numerator, denominator};
    } catch (const std::exception&) {
        return make_error(ErrorCode::invalid_argument, "meter must look like 4/4");
    }
}

bool has_extension(const std::string& path, const char* extension) {
    const std::string lowered = ascii_lower(path);
    const std::string suffix = std::string(".") + extension;
    return lowered.size() >= suffix.size() && lowered.compare(lowered.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

CommandOutput command_inspect(const CommandOptions& options) {
    auto opened = app::open_document(app::path_from_utf8(options.input), {}, {}, worker_parser().parser());
    if (!opened)
        return failure("inspect", opened.error());
    nlohmann::json warnings = nlohmann::json::array();
    nlohmann::json errors = nlohmann::json::array();
    split_diagnostics(opened->diagnostics, false, warnings, errors);
    nlohmann::json sequences = nlohmann::json::array();
    for (const auto& sequence : opened->document.sequences)
        sequences.push_back(sequence_json(sequence));
    CommandOutput output;
    output.json = {{"schema_version", 1},
                   // Errors are "error", warnings alone "warning", neither "ok" (code-runtime-33).
                   {"status", !errors.empty() ? "error" : !warnings.empty() ? "warning" : "ok"},
                   {"command", "inspect"},
                   {"summary", summary_json(*opened)},
                   {"sequences", std::move(sequences)},
                   {"warnings", std::move(warnings)},
                   {"errors", std::move(errors)}};
    return output;
}

CommandOutput command_validate(const CommandOptions& options) {
    auto opened = app::open_document(app::path_from_utf8(options.input), {}, {}, worker_parser().parser());
    if (!opened)
        return failure("validate", opened.error());
    auto diagnostics = opened->diagnostics;
    const auto domain = validate_document(opened->document);
    diagnostics.insert(diagnostics.end(), domain.begin(), domain.end());
    nlohmann::json warnings = nlohmann::json::array();
    nlohmann::json errors = nlohmann::json::array();
    split_diagnostics(diagnostics, options.strict, warnings, errors);
    CommandOutput output;
    output.exit_code = errors.empty() ? exit_success : exit_invalid_data;
    output.json = {{"schema_version", 1},
                   {"status", errors.empty() ? "ok" : "invalid"},
                   {"command", "validate"},
                   {"strict", options.strict},
                   {"summary", summary_json(*opened)},
                   {"warnings", std::move(warnings)},
                   {"errors", std::move(errors)}};
    return output;
}

CommandOutput command_create(const CommandOptions& options) {
    auto meter = parse_meter(options.meter);
    if (!meter)
        return failure("create", meter.error());
    DocumentData document = make_blank_document("Untitled");
    auto& sequence = document.sequences.front();
    sequence.division = Division::make_ppq(options.ppq);
    auto& track = sequence.tracks.front();
    const auto microseconds = static_cast<std::uint32_t>(std::llround(60'000'000.0 / options.bpm));
    int power = 0;
    while ((1 << power) < meter->second)
        ++power;
    track.events.clear();
    Event tempo = events::tempo(0, microseconds);
    tempo.order_key = order_key_spacing;
    Event signature = events::time_signature(0, static_cast<std::uint8_t>(meter->first), static_cast<std::uint8_t>(power));
    signature.order_key = 2 * order_key_spacing;
    track.events.push_back(std::move(tempo));
    track.events.push_back(std::move(signature));
    const Tick bar = static_cast<Tick>(meter->first) * 4 * options.ppq / meter->second;
    sequence.end_tick = 4 * bar;
    track.end_tick = sequence.end_tick;
    sequence.regions.front().end_tick = 4 * bar;

    const auto path = app::path_from_utf8(options.output);
    Status written = ok();
    if (has_extension(options.output, "mwb")) {
        std::error_code error;
        if (!options.overwrite && std::filesystem::exists(path, error))
            return failure("create", Error{ErrorCode::io, "destination exists; pass --overwrite to replace it", {}, {}});
        written = document::save_native(document, path);
    } else {
        auto bytes = midi::export_midi(document, {});
        if (!bytes)
            return failure("create", bytes.error());
        written = app::write_file_atomically(path, *bytes, options.overwrite);
    }
    if (!written)
        return failure("create", written.error());
    CommandOutput output;
    output.json = {{"schema_version", 1},
                   {"status", "ok"},
                   {"command", "create"},
                   {"summary",
                    {{"output", options.output},
                     {"ppq", options.ppq},
                     {"bpm", 60'000'000.0 / microseconds},
                     {"meter", options.meter}}},
                   {"warnings", nlohmann::json::array()},
                   {"errors", nlohmann::json::array()}};
    return output;
}

CommandOutput command_convert(const CommandOptions& options) {
    auto opened = app::open_document(app::path_from_utf8(options.input), {}, {}, worker_parser().parser());
    if (!opened)
        return failure("convert", opened.error());
    const std::string profile = options.profile.empty() ? (has_extension(options.output, "mwb") ? "native" : "preserving")
                                                        : options.profile;
    const auto path = app::path_from_utf8(options.output);
    nlohmann::json report = nlohmann::json::object();
    nlohmann::json warnings = nlohmann::json::array();
    if (profile == "native") {
        std::error_code error;
        if (!options.overwrite && std::filesystem::exists(path, error))
            return failure("convert", Error{ErrorCode::io, "destination exists; pass --overwrite to replace it", {}, {}});
        if (auto saved = document::save_native(opened->document, path); !saved)
            return failure("convert", saved.error());
        report = {{"profile", "native"}, {"lossless", true}};
    } else {
        midi::ExportOptions export_options;
        if (profile == "preserving") {
            export_options.mode = midi::ExportMode::preserving;
        } else if (profile == "canonical") {
            export_options.mode = midi::ExportMode::compatibility;
            export_options.profile = midi::CompatibilityProfile::canonical_type1;
        } else if (profile == "type0") {
            export_options.mode = midi::ExportMode::compatibility;
            export_options.profile = midi::CompatibilityProfile::type0;
        } else if (profile == "original") {
            export_options.mode = midi::ExportMode::original;
        } else {
            return failure("convert", Error{ErrorCode::invalid_argument,
                                            "unknown profile; use preserving, canonical, type0, original or native",
                                            {}, {}});
        }
        midi::ExportReport export_report;
        auto bytes = midi::export_midi(opened->document, export_options, &export_report);
        if (!bytes)
            return failure("convert", bytes.error());
        if (auto written = app::write_file_atomically(path, *bytes, options.overwrite); !written)
            return failure("convert", written.error());
        report = nlohmann::json::parse(export_report.to_json());
        report["profile"] = profile;
        report["byte_identical_to_source"] = profile == "original";
        report["notes"] = "Native-only state (mute/solo, scale regions, recipes, instruments, layout) is not "
                          "representable in SMF and is not exported.";
        for (const auto& diagnostic : export_report.diagnostics)
            if (diagnostic.severity != Severity::info)
                warnings.push_back(diagnostics_json({diagnostic})[0]);
    }
    CommandOutput output;
    output.json = {{"schema_version", 1},
                   {"status", "ok"},
                   {"command", "convert"},
                   {"summary", summary_json(*opened)},
                   {"report", std::move(report)},
                   {"warnings", std::move(warnings)},
                   {"errors", nlohmann::json::array()}};
    return output;
}

CommandOutput command_capabilities(const CommandOptions&) {
    CommandOutput output;
    output.json = nlohmann::json::parse(app::capabilities_json());
    output.json["status"] = "ok";
    output.json["command"] = "capabilities";
    output.json["parser_isolation"] = worker_parser().isolation_description();
    output.json["warnings"] = nlohmann::json::array();
    output.json["errors"] = nlohmann::json::array();
    return output;
}

} // namespace mwb::cli
