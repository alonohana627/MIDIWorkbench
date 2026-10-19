#include "mwb/document/file_system.hpp"

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

} // namespace

Status flush_file(const std::filesystem::path& path) {
    FileHandle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid())
        return make_error(ErrorCode::io, last_error_text("opening the file for flushing"));
    if (!FlushFileBuffers(file.get()))
        return make_error(ErrorCode::io, last_error_text("flushing the file"));
    return ok();
}

Status replace_file(const std::filesystem::path& replacement, const std::filesystem::path& target) {
    if (!MoveFileExW(replacement.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return make_error(ErrorCode::io, last_error_text("replacing the destination file"));
    return ok();
}

#else

namespace {

/// Text of errno for an operation.
std::string errno_text(const char* operation) {
    return std::string(operation) + " failed: " + std::strerror(errno);
}

/// fsync of a file or directory opened read-only.
Status sync_path(const std::filesystem::path& path, bool directory) {
    const int descriptor = ::open(path.c_str(), directory ? (O_RDONLY | O_DIRECTORY) : O_RDWR);
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
    if (handle == INVALID_HANDLE_VALUE)
        return make_error(ErrorCode::conflict, "the file is locked by another process");
    lock.handle_ = handle;
#else
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (descriptor < 0)
        return make_error(ErrorCode::io, "cannot create the lock file");
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
    if (handle == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_SHARING_VIOLATION || GetLastError() == ERROR_ACCESS_DENIED;
    CloseHandle(handle);
    return false; // a leftover file nobody holds
#else
    const int descriptor = ::open(path.c_str(), O_RDWR);
    if (descriptor < 0)
        return false;
    const bool held = ::flock(descriptor, LOCK_EX | LOCK_NB) != 0;
    ::close(descriptor);
    return held;
#endif
}

} // namespace mwb::document::file_system
