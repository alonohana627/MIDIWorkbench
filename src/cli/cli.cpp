#include "mwb/cli/cli.hpp"

#include "mwb/app/document_io.hpp"
#include "mwb/app/environment.hpp"
#include "mwb/cli/commands.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <sstream>

namespace mwb::cli {

int exit_code_for(ErrorCode code) {
    switch (code) {
    case ErrorCode::invalid_argument: return exit_invalid_arguments;
    case ErrorCode::invalid_data:
    case ErrorCode::unsupported:
    case ErrorCode::limit_exceeded: return exit_invalid_data;
    case ErrorCode::io:
    case ErrorCode::database:
    case ErrorCode::not_found:
    case ErrorCode::busy:
    case ErrorCode::conflict: return exit_filesystem;
    case ErrorCode::device: return exit_device;
    case ErrorCode::cancelled: return exit_cancelled;
    case ErrorCode::internal: return exit_internal;
    }
    return exit_internal;
}

nlohmann::json diagnostics_json(const std::vector<Diagnostic>& diagnostics) {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& diagnostic : diagnostics) {
        nlohmann::json item = {{"severity", severity_name(diagnostic.severity)},
                               {"code", diagnostic.code},
                               {"message", diagnostic.message}};
        if (diagnostic.sequence >= 0)
            item["sequence"] = diagnostic.sequence;
        if (diagnostic.track >= 0)
            item["track"] = diagnostic.track;
        if (diagnostic.event_ordinal >= 0)
            item["event_ordinal"] = diagnostic.event_ordinal;
        list.push_back(std::move(item));
    }
    return list;
}

nlohmann::json error_json(const Error& error) {
    return {{"code", std::string(error_code_name(error.code))}, {"message", error.describe()}};
}

CommandOutput failure(const std::string& command, const Error& error) {
    CommandOutput output;
    output.exit_code = exit_code_for(error.code);
    output.json = {{"schema_version", 1},
                   {"status", "error"},
                   {"command", command},
                   {"warnings", nlohmann::json::array()},
                   {"errors", nlohmann::json::array({error_json(error)})}};
    output.message = error.describe();
    return output;
}

app::WorkerParser& worker_parser() {
    static app::WorkerParser parser([] {
        if (const char* configured = std::getenv("MWB_WORKER"); configured != nullptr && *configured != '\0')
            return app::path_from_utf8(configured);
#ifdef _WIN32
        constexpr const char* name = "mwb-worker.exe";
#else
        constexpr const char* name = "mwb-worker";
#endif
        return app::current_executable_path().parent_path() / name;
    }());
    return parser;
}

int run(const std::vector<std::string>& arguments, std::string& standard_output, std::string& standard_error) {
    // Every option is bound to storage owned by this function and used before it returns
    // (section 31: bindings must outlive the parse).
    CommandOptions options;
    CLI::App application{"MIDI Workbench command-line interface", "mwb-cli"};
    application.require_subcommand(1);
    application.add_flag("--warnings-as-errors", options.warnings_as_errors, "Treat warnings as failures");

    auto* inspect = application.add_subcommand("inspect", "Report container, sequences, tracks, timing and warnings");
    inspect->add_option("input", options.input, "MIDI or native file")->required();
    inspect->add_flag("--json", options.json, "Machine-readable JSON (always on)");

    auto* validate = application.add_subcommand("validate", "Validate format and domain invariants");
    validate->add_option("input", options.input)->required();
    validate->add_flag("--strict", options.strict, "Warnings also make the input invalid");
    validate->add_flag("--json", options.json);

    auto* create = application.add_subcommand("create", "Create an empty valid document (.mwb or .mid)");
    create->add_option("--output", options.output)->required();
    create->add_option("--ppq", options.ppq, "Pulses per quarter note")->check(CLI::Range(1, 32767));
    create->add_option("--bpm", options.bpm, "Quarter-note BPM")->check(CLI::Range(5.0, 999.999));
    create->add_option("--meter", options.meter, "Time signature such as 4/4 or 6/8");
    create->add_flag("--overwrite", options.overwrite);

    auto* convert = application.add_subcommand("convert", "Convert with a structured loss report");
    convert->add_option("input", options.input)->required();
    convert->add_option("--output", options.output)->required();
    convert->add_option("--profile", options.profile,
                        "preserving (default), canonical, type0, original, or native");
    convert->add_flag("--overwrite", options.overwrite);

    auto* transform = application.add_subcommand("transform", "Apply a validated deterministic recipe");
    transform->add_option("input", options.input)->required();
    transform->add_option("--recipe", options.recipe)->required();
    transform->add_option("--output", options.output)->required();
    transform->add_flag("--overwrite", options.overwrite);

    auto* render = application.add_subcommand("render", "Offline WAV rendering with the built-in synth");
    render->add_option("input", options.input)->required();
    render->add_option("--output", options.output)->required();
    render->add_option("--synth", options.synth, "builtin (default) or a path to an .sf2 bank");
    render->add_option("--sample-rate", options.sample_rate)->check(CLI::IsMember({44100, 48000, 96000}));
    render->add_option("--format", options.wav_format, "pcm16, pcm24 (default) or float32");
    render->add_option("--tail", options.tail_seconds, "Tail in seconds (0-30)")->check(CLI::Range(0.0, 30.0));
    render->add_flag("--normalize", options.normalize);
    render->add_flag("--overwrite", options.overwrite);

    auto* library = application.add_subcommand("library", "Managed library operations");
    library->require_subcommand(1);
    auto* library_import = library->add_subcommand("import", "Resumable managed import");
    library_import->add_option("--library", options.library)->required();
    library_import->add_option("--from", options.from)->required();
    library_import->add_flag("--json", options.json);
    auto* library_search = library->add_subcommand("search", "Text and structured search");
    library_search->add_option("--library", options.library)->required();
    library_search->add_option("--query", options.query)->required();
    library_search->add_option("--limit", options.limit)->check(CLI::Range(1, 500));
    library_search->add_flag("--json", options.json);
    auto* library_melody = library->add_subcommand("melody", "Find a melody by notes (exact, transposed, contour, rhythm or similar)");
    library_melody->add_option("--library", options.library)->required();
    library_melody->add_option("--notes", options.melody, "Pitches such as C4 E4 G4; append :ticks for a duration (C4:480)")
        ->required();
    library_melody->add_option("--mode", options.melody_mode, "absolute, transposed (default), contour, rhythm or similar");
    library_melody->add_option("--limit", options.limit)->check(CLI::Range(1, 500));
    library_melody->add_flag("--json", options.json);
    auto* library_analyze = library->add_subcommand("analyze", "Build explicit derived indexes");
    library_analyze->add_option("--library", options.library)->required();
    library_analyze->add_option("--profile", options.profile);
    auto* library_backup = library->add_subcommand("backup", "Create a verified consistent backup");
    library_backup->add_option("--library", options.library)->required();
    library_backup->add_option("--output", options.output)->required();
    auto* library_verify = library->add_subcommand("verify", "Read-only integrity and hash checks");
    library_verify->add_option("--library", options.library)->required();
    library_verify->add_flag("--objects", options.verify_objects);
    library_verify->add_flag("--json", options.json);
    auto* library_repair = library->add_subcommand("repair", "Explicit bounded repair with backup");
    library_repair->add_option("--library", options.library)->required();
    library_repair->add_flag("--rebuild-fts", options.rebuild_fts);
    library_repair->add_flag("--rebuild-derived", options.rebuild_derived,
                             "Drop fingerprints, motif and feature indexes so the next analyze rebuilds them");
    auto* library_gc = library->add_subcommand("gc", "Remove unreferenced objects after their grace period");
    library_gc->add_option("--library", options.library)->required();
    library_gc->add_option("--grace-days", options.grace_days, "Keep unreferenced objects younger than this (default 30)")
        ->check(CLI::Range(0.0, 3650.0));
    library_gc->add_flag("--json", options.json);

    auto* devices = application.add_subcommand("devices", "Enumerate audio and MIDI endpoints");
    devices->add_flag("--json", options.json);
    auto* capabilities = application.add_subcommand("capabilities", "Build ID, dependency versions and backends");
    capabilities->add_flag("--json", options.json);
    auto* bench = application.add_subcommand("bench", "Generate a synthetic catalog and time hot paths");
    bench->add_option("--library", options.library)->required();
    bench->add_option("--rows", options.rows)->check(CLI::Range(1, 10'000'000));
    bench->add_option("--motif-rows", options.motif_rows, "Also index this many melody streams (M10K: 10000)")
        ->check(CLI::Range(0, 1'000'000));
    bench->add_option("--repetitions", options.repetitions, "Samples per timing (default 25)")->check(CLI::Range(1, 1000));
    bench->add_flag("--json", options.json);

    std::vector<std::string> reversed(arguments.rbegin(), arguments.rend());
    if (!reversed.empty())
        reversed.pop_back(); // program name
    try {
        application.parse(reversed);
    } catch (const CLI::CallForHelp&) {
        standard_output = application.help();
        return exit_success;
    } catch (const CLI::CallForAllHelp&) {
        standard_output = application.help("", CLI::AppFormatMode::All);
        return exit_success;
    } catch (const CLI::ParseError& error) {
        standard_error = std::string("error: ") + error.what() + "\n";
        return exit_invalid_arguments;
    }

    CommandOutput output;
    try {
    if (inspect->parsed())
        output = command_inspect(options);
    else if (validate->parsed())
        output = command_validate(options);
    else if (create->parsed())
        output = command_create(options);
    else if (convert->parsed())
        output = command_convert(options);
    else if (transform->parsed())
        output = command_transform(options);
    else if (render->parsed())
        output = command_render(options);
    else if (library_import->parsed())
        output = command_library_import(options);
    else if (library_search->parsed())
        output = command_library_search(options);
    else if (library_melody->parsed())
        output = command_library_melody(options);
    else if (library_analyze->parsed())
        output = command_library_analyze(options);
    else if (library_backup->parsed())
        output = command_library_backup(options);
    else if (library_verify->parsed())
        output = command_library_verify(options);
    else if (library_repair->parsed())
        output = command_library_repair(options);
    else if (library_gc->parsed())
        output = command_library_gc(options);
    else if (devices->parsed())
        output = command_devices(options);
    else if (capabilities->parsed())
        output = command_capabilities(options);
    else if (bench->parsed())
        output = command_bench(options);

    } catch (const std::exception& error) {
        // Exception barrier: library or JSON exceptions become a stable internal-failure result.
        output = failure("cli", Error{ErrorCode::internal, std::string("unexpected failure: ") + error.what(), {}, {}});
    }
    if (options.warnings_as_errors && output.exit_code == exit_success && output.json.contains("warnings") &&
        !output.json["warnings"].empty()) {
        output.exit_code = exit_invalid_data;
        output.json["status"] = "error";
    }
    // Text from files (titles, track names) may not be valid UTF-8: replace, never throw.
    standard_output = output.json.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    if (!output.message.empty())
        standard_error += output.message + "\n";
    return output.exit_code;
}

} // namespace mwb::cli
