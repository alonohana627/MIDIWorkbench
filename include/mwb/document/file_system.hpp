#pragma once

#include "mwb/core/error.hpp"

#include <filesystem>

/// Small filesystem adapter used by atomic saves (section 21.3). The implementation is the only
/// platform-specific code of the document module (section 4.6): Win32 wide-character APIs on
/// Windows, POSIX calls elsewhere.
namespace mwb::document::file_system {

/// An exclusive, process-lifetime lock on a file (Windows: opened without sharing and deleted on
/// close; the OS releases it if the process dies). Used to mark live recovery sessions.
class ExclusiveFileLock {
public:
    ExclusiveFileLock() = default;
    ~ExclusiveFileLock();
    ExclusiveFileLock(ExclusiveFileLock&& other) noexcept;
    ExclusiveFileLock& operator=(ExclusiveFileLock&& other) noexcept;
    ExclusiveFileLock(const ExclusiveFileLock&) = delete;
    ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;

    /// Acquires the lock, creating the file; fails when another holder has it.
    [[nodiscard]] static Result<ExclusiveFileLock> acquire(const std::filesystem::path& path);
    /// True when some live holder (this or another process) has the lock on `path`.
    [[nodiscard]] static bool is_held(const std::filesystem::path& path);

private:
    void release();
    void* handle_ = nullptr;
    int descriptor_ = -1;
    std::filesystem::path path_;
};

/// Forces the file's data and metadata to stable storage (FlushFileBuffers / fsync).
[[nodiscard]] Status flush_file(const std::filesystem::path& path);

/// Atomically replaces `target` with `replacement` (both in the same directory): MoveFileExW with
/// MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH on Windows, rename(2) followed by a
/// directory sync on POSIX. When this fails the previous `target` is left untouched and
/// `replacement` still exists.
[[nodiscard]] Status replace_file(const std::filesystem::path& replacement, const std::filesystem::path& target);

} // namespace mwb::document::file_system
