#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/model.hpp"
#include "mwb/midi/codec.hpp"

#include <filesystem>
#include <memory>
#include <string_view>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

/// File-level document operations shared by the GUI, CLI and worker (one implementation for
/// every entry point, section 35.3 rule 5).
namespace mwb::app {

/// Kind of file identified by content (section 16.1: never by extension alone).
enum class FileKind : std::uint8_t { midi, native, unknown };

/// Identifies native SQLite documents ("SQLite format 3" header) and SMF/RMID content.
[[nodiscard]] FileKind identify_file(std::span<const std::uint8_t> head);

/// Reads a whole file with a size limit, distinguishing missing files from sharing violations.
[[nodiscard]] Result<std::vector<std::uint8_t>> read_file_bytes(const std::filesystem::path& path,
                                                                std::uintmax_t max_bytes);

/// Writes bytes to a sibling temporary file, flushes, then atomically replaces the target.
/// With `allow_overwrite` false an existing target is refused (CLI default, section 31).
[[nodiscard]] Status write_file_atomically(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                                           bool allow_overwrite);

/// An opened document with how it was read.
struct OpenedDocument {
    DocumentData document;
    FileKind kind = FileKind::unknown;
    std::optional<midi::ImportReport> import_report; ///< Present for MIDI files.
    std::vector<Diagnostic> diagnostics;
};

/// Opens a .mid/.midi/.rmi or native .mwb file by content.
/// MIDI bytes go through `parser` when given (the isolated worker), else import_midi.
[[nodiscard]] Result<OpenedDocument> open_document(const std::filesystem::path& path,
                                                   const midi::ParseLimits& limits = {},
                                                   std::stop_token stop = {}, const midi::MidiParser& parser = {});

/// A parser that runs every file in the mwb-worker process (section 28.2): bounded memory and
/// time, and a crash fails only that file. The worker starts on first use and restarts after a
/// failure; calls are serialized. When `worker_executable` does not exist (developer builds that
/// did not build the worker), files are parsed in this process and `isolated()` reports false.
class WorkerParser {
public:
    explicit WorkerParser(std::filesystem::path worker_executable);
    ~WorkerParser();
    WorkerParser(const WorkerParser&) = delete;
    WorkerParser& operator=(const WorkerParser&) = delete;

    [[nodiscard]] Result<midi::ImportResult> parse(std::span<const std::uint8_t> bytes, std::string_view name,
                                                   const midi::ParseLimits& limits, std::stop_token stop);
    /// A MidiParser bound to this object (which must outlive it).
    [[nodiscard]] midi::MidiParser parser();
    [[nodiscard]] bool isolated() const;
    /// Loads an SF2 bank in the worker (in this process when the worker is not built) and returns
    /// its preset count; the caller loads the bank only after this succeeded.
    [[nodiscard]] Result<std::size_t> validate_soundfont(std::span<const std::uint8_t> bytes, std::stop_token stop = {});
    /// What isolation applies, for reports (`capabilities`).
    [[nodiscard]] std::string isolation_description() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

/// Full path of the running executable (empty when unknown).
[[nodiscard]] std::filesystem::path current_executable_path();

/// Display name of a path as UTF-8.
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path);
/// Path from UTF-8 text (wide on Windows).
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view text);

} // namespace mwb::app
