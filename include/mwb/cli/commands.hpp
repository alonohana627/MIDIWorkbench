#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/validation.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace mwb::cli {

/// Storage for every CLI option; owned by run() for the whole parse and execution.
struct CommandOptions {
    std::string input;
    std::string output;
    std::string profile;
    std::string recipe;
    std::string synth = "builtin";
    std::string wav_format = "pcm24";
    std::string meter = "4/4";
    std::string library;
    std::string from;
    std::string query;
    std::string melody;
    std::string melody_mode = "transposed";
    double bpm = 120.0;
    double tail_seconds = 2.0;
    int ppq = 960;
    int sample_rate = 48000;
    int limit = 100;
    std::int64_t rows = 100000;
    bool json = true;
    bool strict = false;
    bool overwrite = false;
    bool normalize = false;
    bool verify_objects = false;
    bool rebuild_fts = false;
    bool warnings_as_errors = false;
};

/// Result of one command: versioned JSON for stdout, an optional human message for stderr, and
/// the process exit code.
struct CommandOutput {
    nlohmann::json json;
    std::string message;
    int exit_code = 0;
};

[[nodiscard]] int exit_code_for(ErrorCode code);
[[nodiscard]] nlohmann::json diagnostics_json(const std::vector<Diagnostic>& diagnostics);
[[nodiscard]] nlohmann::json error_json(const Error& error);
/// Standard failure output for `command`.
[[nodiscard]] CommandOutput failure(const std::string& command, const Error& error);

[[nodiscard]] CommandOutput command_inspect(const CommandOptions& options);
[[nodiscard]] CommandOutput command_validate(const CommandOptions& options);
[[nodiscard]] CommandOutput command_create(const CommandOptions& options);
[[nodiscard]] CommandOutput command_convert(const CommandOptions& options);
[[nodiscard]] CommandOutput command_transform(const CommandOptions& options);
[[nodiscard]] CommandOutput command_render(const CommandOptions& options);
[[nodiscard]] CommandOutput command_library_import(const CommandOptions& options);
[[nodiscard]] CommandOutput command_library_search(const CommandOptions& options);
[[nodiscard]] CommandOutput command_library_analyze(const CommandOptions& options);
[[nodiscard]] CommandOutput command_library_melody(const CommandOptions& options);
[[nodiscard]] CommandOutput command_library_backup(const CommandOptions& options);
[[nodiscard]] CommandOutput command_library_verify(const CommandOptions& options);
[[nodiscard]] CommandOutput command_library_repair(const CommandOptions& options);
[[nodiscard]] CommandOutput command_devices(const CommandOptions& options);
[[nodiscard]] CommandOutput command_capabilities(const CommandOptions& options);
[[nodiscard]] CommandOutput command_bench(const CommandOptions& options);

} // namespace mwb::cli
