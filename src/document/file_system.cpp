#include "mwb/document/file_system.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace mwb::document::file_system {

#ifdef _WIN32

namespace {

/// Owns a Win32 file handle for the duration of one call.
class FileHandle {
public:
    explicit FileHandle(HANDLE handle) : handle_(handle) {}
    ~FileHandle() {
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    [[nodiscard]] HANDLE get() const { return handle_; }
    [[nodiscard]] bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_;
};

/// Text of the calling thread's last Win32 error.
std::string last_error_text(const char* operation) {
    const DWORD code = GetLastError();
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string message = std::string(operation) + " failed (Win32 error " + std::to_string(code) + ")";
    if (length > 0 && buffer != nullptr) {
        const int needed = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        if (needed > 0) {
            std::string text(static_cast<std::size_t>(needed), '\0');
            WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), text.data(), needed, nullptr, nullptr);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
                text.pop_back();
            message += ": " + text;
        }
    }
    if (buffer != nullptr)
        LocalFree(buffer);
    return message;
}

/// Transient locks held briefly by antivirus scanners, the search indexer, cloud sync or backup
/// tools on a freshly written file (section 29.1: bounded backoff retry of transient locks).
bool is_transient_lock(DWORD code) {
    return code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION || code == ERROR_ACCESS_DENIED;
}

/// Backoff before each retry: nine retries wait about 1.9 s in total. Saves and checkpoints run
/// on background threads, so the wait never blocks the GUI.
constexpr std::array<DWORD, 9> retry_delays_ms{25, 50, 100, 200, 300, 300, 300, 300, 300};
constexpr int max_transient_retries = static_cast<int>(retry_delays_ms.size());
DWORD retry_delay_ms(int attempt) {
    return retry_delays_ms[static_cast<std::size_t>(std::clamp(attempt, 1, max_transient_retries) - 1)];
}

/// True when `path` exists and is marked read-only: access denied is then permanent.
bool is_read_only_file(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0;
}

} // namespace

Status flush_file(const std::filesystem::path& path) {
    for (int attempt = 0;; ++attempt) {
        FileHandle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.valid()) {
            if (!FlushFileBuffers(file.get()))
                return make_error(ErrorCode::io, last_error_text("flushing the file"));
            return ok();
        }
        const DWORD code = GetLastError();
        if (attempt >= max_transient_retries || !is_transient_lock(code) ||
            (code == ERROR_ACCESS_DENIED && is_read_only_file(path))) {
            SetLastError(code);
            return make_error(ErrorCode::io, last_error_text("opening the file for flushing"));
        }
        Sleep(retry_delay_ms(attempt + 1));
    }
}

Status replace_file(const std::filesystem::path& replacement, const std::filesystem::path& target) {
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(replacement.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return ok();
        const DWORD code = GetLastError();
        // A read-only destination is a permanent refusal: fail at once instead of retrying.
        if (attempt >= max_transient_retries || !is_transient_lock(code) ||
            (code == ERROR_ACCESS_DENIED && is_read_only_file(target))) {
            SetLastError(code);
            return make_error(ErrorCode::io, last_error_text("replacing the destination file"));
        }
        Sleep(retry_delay_ms(attempt + 1));
    }
}

std::optional<std::string> destination_limitation(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error)
        return std::nullopt;
    std::wstring root = absolute.root_path().wstring();
    if (root.empty())
        return std::nullopt;
    if (root.back() != L'\\' && root.back() != L'/')
        root.push_back(L'\\');
    switch (GetDriveTypeW(root.c_str())) {
    case DRIVE_REMOTE:
        return std::string("the destination is a network location: a save completes only when the server has the "
                           "data, and another computer may change the file meanwhile");
    case DRIVE_REMOVABLE:
        return std::string("the destination is a removable drive: removing it during a save loses the new version");
    default: return std::nullopt;
    }
}

#else

namespace {

/// Text of errno for an operation.
std::string errno_text(const char* operation) {
    return std::string(operation) + " failed: " + std::strerror(errno);
}

/// fsync of a file or directory opened read-only.
Status sync_path(const std::filesystem::path& path, bool directory) {
    const int descriptor = ::open(path.c_str(), (directory ? (O_RDONLY | O_DIRECTORY) : O_RDWR) | O_CLOEXEC);
    if (descriptor < 0)
        return make_error(ErrorCode::io, errno_text("opening for fsync"));
    const int result = ::fsync(descriptor);
    ::close(descriptor);
    if (result != 0)
        return make_error(ErrorCode::io, errno_text("fsync"));
    return ok();
}

} // namespace

Status flush_file(const std::filesystem::path& path) { return sync_path(path, false); }

Status replace_file(const std::filesystem::path& replacement, const std::filesystem::path& target) {
    if (std::rename(replacement.c_str(), target.c_str()) != 0)
        return make_error(ErrorCode::io, errno_text("replacing the destination file"));
    auto directory = target.parent_path();
    if (directory.empty())
        directory = ".";
    return sync_path(directory, true);
}

std::optional<std::string> destination_limitation(const std::filesystem::path&) {
    return std::nullopt; // mount types are not classified on POSIX
}

#endif


// --- ExclusiveFileLock ------------------------------------------------------------------------------

ExclusiveFileLock::~ExclusiveFileLock() { release(); }

ExclusiveFileLock::ExclusiveFileLock(ExclusiveFileLock&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), descriptor_(std::exchange(other.descriptor_, -1)),
      path_(std::move(other.path_)) {}

ExclusiveFileLock& ExclusiveFileLock::operator=(ExclusiveFileLock&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = std::exchange(other.handle_, nullptr);
        descriptor_ = std::exchange(other.descriptor_, -1);
        path_ = std::move(other.path_);
    }
    return *this;
}

void ExclusiveFileLock::release() {
#ifdef _WIN32
    if (handle_)
        CloseHandle(static_cast<HANDLE>(handle_)); // FILE_FLAG_DELETE_ON_CLOSE removes the file
    handle_ = nullptr;
#else
    if (descriptor_ >= 0) {
        std::error_code error;
        std::filesystem::remove(path_, error);
        ::close(descriptor_);
    }
    descriptor_ = -1;
#endif
}

Result<ExclusiveFileLock> ExclusiveFileLock::acquire(const std::filesystem::path& path) {
    ExclusiveFileLock lock;
    lock.path_ = path;
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0 /* no sharing */, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        // Only a sharing violation means another holder; anything else (missing directory,
        // permissions) is an I/O problem and must not be reported as a live session.
        if (GetLastError() == ERROR_SHARING_VIOLATION)
            return make_error(ErrorCode::conflict, "the file is locked by another process");
        return make_error(ErrorCode::io, last_error_text("creating the lock file"));
    }
    lock.handle_ = handle;
#else
    // O_CLOEXEC: a spawned worker process must not inherit the lock and keep a crashed session
    // looking live.
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor < 0)
        return make_error(ErrorCode::io, errno_text("creating the lock file"));
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        ::close(descriptor);
        return make_error(ErrorCode::conflict, "the file is locked by another process");
    }
    lock.descriptor_ = descriptor;
#endif
    return lock;
}

bool ExclusiveFileLock::is_held(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error))
        return false;
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_SHARING_VIOLATION)
            return true;
        // Access denied also comes from a lock file whose holder is closing it (delete pending,
        // the file still exists); a plain permission problem on a missing file is not a holder.
        return code == ERROR_ACCESS_DENIED && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    CloseHandle(handle);
    return false; // a leftover file nobody holds
#else
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (descriptor < 0)
        return false;
    const bool held = ::flock(descriptor, LOCK_EX | LOCK_NB) != 0;
    ::close(descriptor);
    return held;
#endif
}

} // namespace mwb::document::file_system
