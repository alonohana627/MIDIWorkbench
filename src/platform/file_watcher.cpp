#include "mwb/platform/file_watcher.hpp"

#include <efsw/efsw.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace mwb::platform {

namespace {

std::string to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path from_utf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

/// True when `path` equals `root` or lies below it (lexical comparison).
bool is_within(const std::filesystem::path& path, const std::filesystem::path& root) {
    auto path_part = path.begin();
    for (auto root_part = root.begin(); root_part != root.end(); ++root_part, ++path_part) {
        if (path_part == path.end() || *path_part != *root_part)
            return false;
    }
    return true;
}

/// A change waiting for its quiet period.
struct PendingChange {
    FileChangeKind kind = FileChangeKind::modified;
    std::filesystem::path previous_path;
    std::chrono::steady_clock::time_point deadline;
};

} // namespace

/// Listener state shared with efsw's thread and the debounce thread.
struct DirectoryWatcher::State final : public efsw::FileWatchListener {
    State(FileWatcherSettings watcher_settings, FileChangeCallback change_callback)
        : settings(watcher_settings), callback(std::move(change_callback)) {}

    void handleFileAction(efsw::WatchID, const std::string& directory, const std::string& filename,
                          efsw::Action action, const std::string& old_filename) override {
        raw.fetch_add(1, std::memory_order_relaxed);
        const std::filesystem::path path = (from_utf8(directory) / from_utf8(filename)).lexically_normal();
        std::lock_guard lock(mutex);
        for (const auto& excluded_root : excluded)
            if (is_within(path, excluded_root))
                return;
        if (pending.size() >= settings.max_pending_paths && !pending.contains(path)) {
            rescan_deadline = std::chrono::steady_clock::now() + settings.debounce;
            rescan = true;
            changed.notify_all();
            return;
        }
        PendingChange& change = pending[path];
        const bool fresh = change.deadline == std::chrono::steady_clock::time_point{};
        FileChangeKind kind = FileChangeKind::modified;
        switch (action) {
        case efsw::Actions::Add:
            kind = FileChangeKind::added;
            break;
        case efsw::Actions::Delete:
            kind = FileChangeKind::removed;
            break;
        case efsw::Actions::Modified:
            kind = FileChangeKind::modified;
            break;
        case efsw::Actions::Moved:
            kind = FileChangeKind::renamed;
            change.previous_path = (from_utf8(directory) / from_utf8(old_filename)).lexically_normal();
            break;
        }
        // A file added and then written inside one quiet period is still "added".
        if (fresh || !(change.kind == FileChangeKind::added && kind == FileChangeKind::modified))
            change.kind = kind;
        change.deadline = std::chrono::steady_clock::now() + settings.debounce;
        changed.notify_all();
    }

    void handleMissedFileActions(efsw::WatchID, const std::string&) override {
        std::lock_guard lock(mutex);
        rescan = true;
        rescan_deadline = std::chrono::steady_clock::now() + settings.debounce;
        changed.notify_all();
    }

    /// Debounce thread: delivers paths that have been quiet for the debounce interval.
    void dispatch(std::stop_token stop) {
        std::unique_lock lock(mutex);
        while (!stop.stop_requested()) {
            if (pending.empty() && !rescan) {
                changed.wait(lock, stop, [this] { return !pending.empty() || rescan; });
                continue;
            }
            auto earliest = rescan ? rescan_deadline : std::chrono::steady_clock::time_point::max();
            for (const auto& [path, change] : pending)
                earliest = std::min(earliest, change.deadline);
            if (std::chrono::steady_clock::now() < earliest) {
                changed.wait_until(lock, stop, earliest, [] { return false; });
                continue;
            }
            std::vector<FileChangeHint> batch;
            const auto now = std::chrono::steady_clock::now();
            if (rescan && rescan_deadline <= now) {
                batch.push_back(FileChangeHint{FileChangeKind::rescan_required, {}, {}});
                rescan = false;
                pending.clear();
            }
            for (auto iterator = pending.begin(); iterator != pending.end();) {
                if (iterator->second.deadline <= now) {
                    batch.push_back(FileChangeHint{iterator->second.kind, iterator->first, iterator->second.previous_path});
                    iterator = pending.erase(iterator);
                } else {
                    ++iterator;
                }
            }
            if (batch.empty())
                continue;
            lock.unlock();
            callback(std::move(batch));
            lock.lock();
        }
    }

    FileWatcherSettings settings;
    FileChangeCallback callback;
    std::mutex mutex;
    std::condition_variable_any changed;
    std::map<std::filesystem::path, PendingChange> pending;
    std::vector<std::filesystem::path> excluded;
    bool rescan = false;
    std::chrono::steady_clock::time_point rescan_deadline;
    std::atomic<std::uint64_t> raw{0};
    std::map<std::filesystem::path, efsw::WatchID> watches;
    bool started = false;
    std::unique_ptr<efsw::FileWatcher> watcher;
    std::jthread dispatcher;
};

Result<std::unique_ptr<DirectoryWatcher>> DirectoryWatcher::create(FileWatcherSettings settings,
                                                                   FileChangeCallback callback) {
    if (!callback)
        return make_error(ErrorCode::invalid_argument, "a change callback is required");
    if (settings.debounce.count() < 0 || settings.max_pending_paths == 0)
        return make_error(ErrorCode::invalid_argument, "invalid watcher settings");
    auto state = std::make_unique<State>(settings, std::move(callback));
    state->watcher = std::make_unique<efsw::FileWatcher>();
    State* raw_state = state.get();
    state->dispatcher = std::jthread([raw_state](std::stop_token stop) { raw_state->dispatch(stop); });
    return std::unique_ptr<DirectoryWatcher>(new DirectoryWatcher(std::move(state)));
}

DirectoryWatcher::DirectoryWatcher(std::unique_ptr<State> state) : state_(std::move(state)) {}

DirectoryWatcher::~DirectoryWatcher() {
    state_->dispatcher.request_stop();
    if (state_->dispatcher.joinable())
        state_->dispatcher.join();
    state_->watcher.reset(); // Joins efsw's thread before the listener state goes away.
}

Status DirectoryWatcher::add_directory(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
        return make_error(ErrorCode::not_found, "the directory to watch does not exist");
    const std::filesystem::path key = directory.lexically_normal();
    {
        std::lock_guard lock(state_->mutex);
        if (state_->watches.contains(key))
            return ok();
    }
    const efsw::WatchID id = state_->watcher->addWatch(to_utf8(key), state_.get(), state_->settings.recursive);
    if (id < 0)
        return make_error(ErrorCode::io, "cannot watch the directory", efsw::Errors::Log::getLastErrorLog());
    {
        std::lock_guard lock(state_->mutex);
        state_->watches.emplace(key, id);
    }
    if (!state_->started) {
        state_->watcher->watch();
        state_->started = true;
    }
    return ok();
}

void DirectoryWatcher::remove_directory(const std::filesystem::path& directory) {
    const std::filesystem::path key = directory.lexically_normal();
    efsw::WatchID id = 0;
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->watches.find(key);
        if (found == state_->watches.end())
            return;
        id = found->second;
        state_->watches.erase(found);
    }
    state_->watcher->removeWatch(id);
}

void DirectoryWatcher::exclude(const std::filesystem::path& directory) {
    std::lock_guard lock(state_->mutex);
    state_->excluded.push_back(directory.lexically_normal());
}

std::uint64_t DirectoryWatcher::raw_notifications() const noexcept { return state_->raw.load(std::memory_order_relaxed); }

} // namespace mwb::platform
