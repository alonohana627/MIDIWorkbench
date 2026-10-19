#pragma once

#include <string>
#include <vector>

/// mwb-cli: headless command-line interface (specification section 31). Commands reuse the same
/// application and core services as the GUI; machine output is versioned JSON on stdout,
/// diagnostics go to stderr.
namespace mwb::cli {

/// Stable exit codes of section 31.
enum ExitCode : int {
    exit_success = 0,
    exit_invalid_arguments = 1,
    exit_invalid_data = 2,
    exit_filesystem = 3,
    exit_device = 4,
    exit_internal = 5,
    exit_cancelled = 6,
    exit_partial = 7,
};

/// Runs the CLI with UTF-8 arguments (argv[0] is the program name). Output goes to the
/// provided streams so tests can capture it.
int run(const std::vector<std::string>& arguments, std::string& standard_output, std::string& standard_error);

} // namespace mwb::cli
