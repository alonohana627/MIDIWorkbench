#pragma once

#include "mwb/core/error.hpp"

#include <filesystem>
#include <string>

namespace mwb::app {

/// Per-user data locations (section 29.2): installed mode uses LocalAppData; portable mode uses
/// an explicitly selected directory. Nothing here requires administrator rights.
struct DataDirectories {
    std::filesystem::path root;       ///< %LOCALAPPDATA%/MIDIWorkbench or the portable directory.
    std::filesystem::path recovery;   ///< Recovery databases (section 21.2).
    std::filesystem::path logs;       ///< Rotating logs (section 30.4).
    std::filesystem::path drag_cache; ///< Temporary .mid files for drag-out (section 14.3).
    std::filesystem::path preferences_file;
};

/// Resolves the directories, honoring an explicit workspace override (`--workspace`).
[[nodiscard]] DataDirectories resolve_data_directories(const std::filesystem::path& workspace_override = {});

/// Creates the directories that do not exist yet.
[[nodiscard]] Status ensure_data_directories(const DataDirectories& directories);

/// Starts the rotating file logger (5 files x 10 MiB by default) under `logs`. Safe to call once.
[[nodiscard]] Status start_logging(const std::filesystem::path& logs, std::size_t file_bytes = 10u * 1024u * 1024u,
                                   std::size_t file_count = 5);

/// Writes one structured log line (never call from audio or MIDI callbacks).
void log_event(const char* subsystem, const char* event_code, const std::string& message);

/// Build and dependency report used by `mwb-cli capabilities` and the About box (JSON text).
[[nodiscard]] std::string capabilities_json();

} // namespace mwb::app
