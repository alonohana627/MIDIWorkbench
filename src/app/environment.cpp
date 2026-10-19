#include "mwb/app/environment.hpp"

#include "mwb/app/third_party.hpp"
#include "mwb/build_info.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <cstdlib>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace mwb::app {

namespace {

std::filesystem::path local_application_data() {
#ifdef _WIN32
    PWSTR folder = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &folder)) && folder)
        result = folder;
    CoTaskMemFree(folder);
    if (!result.empty())
        return result;
    return std::filesystem::temp_directory_path();
#else
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data)
        return data;
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".local" / "share";
    return std::filesystem::temp_directory_path();
#endif
}

std::shared_ptr<spdlog::logger>& application_logger() {
    static std::shared_ptr<spdlog::logger> logger;
    return logger;
}

} // namespace

DataDirectories resolve_data_directories(const std::filesystem::path& workspace_override) {
    DataDirectories directories;
    directories.root = workspace_override.empty() ? local_application_data() / "MIDIWorkbench" : workspace_override;
    directories.recovery = directories.root / "recovery";
    directories.logs = directories.root / "logs";
    directories.drag_cache = directories.root / "drag-cache";
    directories.preferences_file = directories.root / "preferences.json";
    return directories;
}

Status ensure_data_directories(const DataDirectories& directories) {
    for (const auto* path : {&directories.root, &directories.recovery, &directories.logs, &directories.drag_cache}) {
        std::error_code error;
        std::filesystem::create_directories(*path, error);
        if (error)
            return make_error(ErrorCode::io, "could not create an application data directory: " + error.message());
    }
    return ok();
}

Status start_logging(const std::filesystem::path& logs, std::size_t file_bytes, std::size_t file_count) {
    static std::once_flag once;
    Status status = ok();
    std::call_once(once, [&] {
        try {
            auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>((logs / "midiworkbench.log").string(),
                                                                               file_bytes, file_count);
            auto logger = std::make_shared<spdlog::logger>("mwb", std::move(sink));
            logger->set_pattern("%Y-%m-%dT%H:%M:%S.%e %l %v");
            logger->flush_on(spdlog::level::warn);
            application_logger() = std::move(logger);
        } catch (const std::exception& error) {
            status = make_error(ErrorCode::io, std::string("could not start logging: ") + error.what());
        }
    });
    return status;
}

void log_event(const char* subsystem, const char* event_code, const std::string& message) {
    if (auto& logger = application_logger())
        logger->info("[{}] {} {}", subsystem, event_code, message);
}

std::string capabilities_json() {
    nlohmann::json report;
    report["schema_version"] = 1;
    report["application"] = {{"name", "MIDI Workbench"},
                             {"version", build_info::version},
                             {"build_id", std::string(build_info::version) + "+" + build_info::configured_at},
                             {"compiler", build_info::compiler},
                             {"build_type", build_info::build_type},
                             {"system", build_info::system},
                             {"qt", build_info::qt_version}};
#ifdef _LIBCPP_VERSION
    report["application"]["standard_library"] = "libc++ " + std::to_string(_LIBCPP_VERSION);
#endif
    nlohmann::json sqlite = {{"version", sqlite3_libversion()}, {"source_id", sqlite3_sourceid()}};
    nlohmann::json options = nlohmann::json::array();
    for (int index = 0;; ++index) {
        const char* option = sqlite3_compileoption_get(index);
        if (!option)
            break;
        options.push_back(option);
    }
    sqlite["compile_options"] = std::move(options);
    report["sqlite"] = std::move(sqlite);
    nlohmann::json dependencies = nlohmann::json::array();
    for (const auto& notice : third_party_notices())
        dependencies.push_back({{"name", notice.name},
                                {"version", notice.version},
                                {"license", notice.license},
                                {"purpose", notice.purpose}});
    report["dependencies"] = std::move(dependencies);
    const char* audio = std::getenv("MWB_AUDIO_BACKEND");
    const char* midi = std::getenv("MWB_MIDI_BACKEND");
    report["backends"] = {{"audio", audio && std::string(audio) == "null" ? "null" : "wasapi (miniaudio)"},
                          {"midi", midi && std::string(midi) == "null" ? "null" : "winmm (libremidi)"}};
    return report.dump(2);
}

} // namespace mwb::app
