#pragma once

#include "mwb/core/error.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace mwb::platform {

/// Kind of a coalesced file-system hint.
enum class FileChangeKind : std::uint8_t {
    added,
    modified,
    removed,
    renamed,          ///< `path` is the new name, `previous_path` the old one.
    rescan_required,  ///< Overflow or watch failure: the consumer must reconcile by scanning.
};

/// One debounced change hint (section 22.4). Notifications are hints only; the library still runs
/// its stable-capture procedure and reconciliation scans.
struct FileChangeHint {
    FileChangeKind kind = FileChangeKind::modified;
    std::filesystem::path path;
    std::filesystem::path previous_path;
};

/// Watcher configuration.
struct FileWatcherSettings {
    std::chrono::milliseconds debounce{300};  ///< Quiet period before a path's hint is delivered.
    std::size_t max_pending_paths = 65536;    ///< Beyond this the batch collapses to rescan_required.
    bool recursive = true;
};

/// Delivers batches of debounced hints on the watcher's own thread.
using FileChangeCallback = std::function<void(std::vector<FileChangeHint>)>;

/// Directory watcher adapter over efsw (ReadDirectoryChangesW on Windows, inotify on Linux).
/// Raw notifications are coalesced per path; a path is delivered once it has been quiet for the
/// debounce interval, so a burst of writes yields one hint. Excluded directories (the library's
/// own object, cache and staging folders) are filtered out.
class DirectoryWatcher {
public:
    [[nodiscard]] static Result<std::unique_ptr<DirectoryWatcher>> create(FileWatcherSettings settings,
                                                                          FileChangeCallback callback);
    ~DirectoryWatcher();
    DirectoryWatcher(const DirectoryWatcher&) = delete;
    DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;

    /// Starts watching a directory (must exist).
    [[nodiscard]] Status add_directory(const std::filesystem::path& directory);
    /// Stops watching a directory.
    void remove_directory(const std::filesystem::path& directory);
    /// Ignores changes at or below `directory`.
    void exclude(const std::filesystem::path& directory);
    /// Number of raw notifications received (diagnostics).
    [[nodiscard]] std::uint64_t raw_notifications() const noexcept;

    struct State;

private:
    explicit DirectoryWatcher(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace mwb::platform
