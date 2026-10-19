#include "mwb/app/document_io.hpp"

#include "mwb/core/text.hpp"

#include "mwb/audio/soundfont_synth.hpp"
#include "mwb/document/file_system.hpp"
#include "mwb/document/native_format.hpp"
#include "mwb/worker/worker.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <array>
#include <cstring>
#include <mutex>
#include <fstream>
#include <random>

namespace mwb::app {

namespace {

constexpr std::uintmax_t default_native_limit = 1024ull * 1024ull * 1024ull;

} // namespace

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path path_from_utf8(std::string_view text) {
    // Invalid UTF-8 would make the path conversion throw: it is replaced, never thrown.
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    if (is_valid_utf8(bytes))
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    std::u8string cleaned;
    for (const char character : text)
        cleaned.push_back(static_cast<unsigned char>(character) < 0x80 ? static_cast<char8_t>(character) : u8'_');
    return std::filesystem::path(cleaned);
}

FileKind identify_file(std::span<const std::uint8_t> head) {
    static constexpr char sqlite_magic[] = "SQLite format 3";
    if (head.size() >= 16 && std::memcmp(head.data(), sqlite_magic, 16) == 0)
        return FileKind::native;
    if (midi::detect_container(head) != midi::Container::unknown)
        return FileKind::midi;
    return FileKind::unknown;
}

Result<std::vector<std::uint8_t>> read_file_bytes(const std::filesystem::path& path, std::uintmax_t max_bytes) {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error || !std::filesystem::exists(status))
        return make_error(ErrorCode::not_found, "file does not exist", path_to_utf8(path.filename()));
    if (!std::filesystem::is_regular_file(status))
        return make_error(ErrorCode::invalid_argument, "path is not a regular file", path_to_utf8(path.filename()));
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return make_error(ErrorCode::io, "could not read the file size", path_to_utf8(path.filename()));
    if (size > max_bytes)
        return make_error(ErrorCode::limit_exceeded, "file is larger than the configured limit",
                          path_to_utf8(path.filename()));
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return make_error(ErrorCode::io, "file could not be opened (it may be locked by another program)",
                          path_to_utf8(path.filename()));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0 && !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        return make_error(ErrorCode::io, "file could not be read completely", path_to_utf8(path.filename()));
    return bytes;
}

Status write_file_atomically(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                             bool allow_overwrite) {
    std::error_code error;
    if (!allow_overwrite && std::filesystem::exists(path, error))
        return make_error(ErrorCode::io, "destination exists; pass --overwrite to replace it",
                          path_to_utf8(path.filename()));
    std::random_device device;
    const auto temporary = path.parent_path() / (path.filename().wstring() + L".tmp-" + std::to_wstring(device()));
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
            return make_error(ErrorCode::io, "could not create a temporary file next to the destination",
                              path_to_utf8(path.filename()));
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        if (!stream) {
            std::filesystem::remove(temporary, error);
            return make_error(ErrorCode::io, "could not write the temporary file (disk full?)",
                              path_to_utf8(path.filename()));
        }
    }
    if (auto flushed = document::file_system::flush_file(temporary); !flushed) {
        std::filesystem::remove(temporary, error);
        return flushed;
    }
    if (!allow_overwrite) {
        // Without --overwrite the final step itself refuses an existing file, so a file created
        // after the check above is never replaced (code-runtime-25).
#ifdef _WIN32
        const bool moved = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#else
        const bool moved = ::link(temporary.c_str(), path.c_str()) == 0 && ::unlink(temporary.c_str()) == 0;
#endif
        if (!moved) {
            std::filesystem::remove(temporary, error);
            return make_error(ErrorCode::io, "destination exists; pass --overwrite to replace it",
                              path_to_utf8(path.filename()));
        }
        return ok();
    }
    if (auto replaced = document::file_system::replace_file(temporary, path); !replaced) {
        std::filesystem::remove(temporary, error);
        return replaced;
    }
    return ok();
}

Result<OpenedDocument> open_document(const std::filesystem::path& path, const midi::ParseLimits& limits,
                                     std::stop_token stop, const midi::MidiParser& parser) {
    // The kind decides the size limit, so a huge MIDI file is refused before it is read, not
    // after reading up to the native limit (code-runtime-24).
    std::uintmax_t limit = std::max<std::uintmax_t>(limits.max_bytes, default_native_limit);
    {
        std::array<std::uint8_t, 16> head{};
        std::ifstream probe(path, std::ios::binary);
        if (probe) {
            probe.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
            const auto got = static_cast<std::size_t>(std::max<std::streamsize>(0, probe.gcount()));
            if (identify_file(std::span(head.data(), got)) == FileKind::midi)
                limit = limits.max_bytes;
        }
    }
    auto bytes = read_file_bytes(path, limit);
    if (!bytes)
        return std::unexpected(bytes.error());
    OpenedDocument opened;
    opened.kind = identify_file(*bytes);
    switch (opened.kind) {
    case FileKind::native: {
        auto document = document::native_from_bytes(*bytes);
        if (!document)
            return std::unexpected(document.error().with_context(path_to_utf8(path.filename())));
        opened.document = std::move(*document);
        return opened;
    }
    case FileKind::midi: {
        if (bytes->size() > limits.max_bytes)
            return make_error(ErrorCode::limit_exceeded, "MIDI file is larger than the import limit");
        const std::string name = path_to_utf8(path.filename());
        auto imported = parser ? parser(*bytes, name, limits, stop) : midi::import_midi(*bytes, name, limits, stop);
        if (!imported)
            return std::unexpected(imported.error().with_context(path_to_utf8(path.filename())));
        opened.document = std::move(imported->document);
        opened.diagnostics = imported->report.diagnostics;
        opened.import_report = std::move(imported->report);
        return opened;
    }
    case FileKind::unknown: break;
    }
    return make_error(ErrorCode::unsupported, "not a MIDI file or MIDI Workbench document (checked by content)",
                      path_to_utf8(path.filename()));
}

// --- WorkerParser -----------------------------------------------------------------------------------

struct WorkerParser::State {
    std::filesystem::path executable;
    bool available = false;
    std::mutex mutex;
    std::unique_ptr<worker::WorkerClient> client;
};

WorkerParser::WorkerParser(std::filesystem::path worker_executable) : state_(std::make_unique<State>()) {
    state_->executable = std::move(worker_executable);
    std::error_code error;
    state_->available = std::filesystem::exists(state_->executable, error);
}

WorkerParser::~WorkerParser() {
    if (state_ && state_->client)
        state_->client->stop();
}

bool WorkerParser::isolated() const { return state_->available; }

Result<std::size_t> WorkerParser::validate_soundfont(std::span<const std::uint8_t> bytes, std::stop_token stop) {
    if (!state_->available) {
        auto bank = audio::SoundFontSynth::load_memory(bytes); // developer build without the worker
        if (!bank)
            return std::unexpected(std::move(bank).error());
        return (*bank)->presets().size();
    }
    std::lock_guard lock(state_->mutex);
    if (!state_->client || !state_->client->is_alive()) {
        auto started = worker::WorkerClient::start(state_->executable);
        if (!started)
            return std::unexpected(started.error().with_context("starting the parser worker"));
        state_->client = std::move(*started);
    }
    return state_->client->validate_soundfont(bytes, std::move(stop)); // a dead worker restarts next time
}

std::string WorkerParser::isolation_description() const {
    if (!state_->available)
        return "in process (mwb-worker was not found next to the executable)";
    return "mwb-worker: separate process with a memory and time limit (" + path_to_utf8(state_->executable) + ")";
}

std::filesystem::path current_executable_path() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2); // truncated: a long path
    }
#else
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error);
#endif
}

midi::MidiParser WorkerParser::parser() {
    return [this](std::span<const std::uint8_t> bytes, std::string_view name, const midi::ParseLimits& limits,
                  std::stop_token stop) { return parse(bytes, name, limits, std::move(stop)); };
}

Result<midi::ImportResult> WorkerParser::parse(std::span<const std::uint8_t> bytes, std::string_view name,
                                               const midi::ParseLimits& limits, std::stop_token stop) {
    if (!state_->available)
        return midi::import_midi(bytes, name, limits, std::move(stop));
    std::lock_guard lock(state_->mutex);
    if (!state_->client || !state_->client->is_alive()) {
        auto started = worker::WorkerClient::start(state_->executable);
        if (!started)
            return std::unexpected(started.error().with_context("starting the parser worker"));
        state_->client = std::move(*started);
    }
    auto parsed = state_->client->parse_midi(bytes, std::string(name), limits, std::move(stop));
    if (!parsed)
        return std::unexpected(parsed.error()); // the next call restarts a dead worker
    auto report = midi::ImportReport::from_json(parsed->report_json);
    if (!report)
        return std::unexpected(report.error().with_context("reading the parser worker's report"));
    return midi::ImportResult{std::move(parsed->document), std::move(*report)};
}

} // namespace mwb::app
