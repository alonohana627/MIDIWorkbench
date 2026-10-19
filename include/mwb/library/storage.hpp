#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/hashing.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

/// Managed object storage and the filesystem adapter of the library (specification sections 22.1,
/// 22.2 and 29.1). Platform code lives only in storage.cpp behind _WIN32 with a POSIX branch.
namespace mwb::library {

/// Identity, size and modification time of an open file (volume serial + file index on Windows,
/// device + inode on POSIX). Used to detect sources that change while they are captured.
struct FileIdentity {
    std::vector<std::uint8_t> identity;
    std::uint64_t byte_count = 0;
    std::int64_t modified_ms = 0;
    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

/// Identity of the file currently at `path`.
[[nodiscard]] Result<FileIdentity> query_file_identity(const std::filesystem::path& path);

/// Bytes captured from a source through one stable open handle, hashed while copying.
struct CapturedFile {
    std::filesystem::path staging_path;   ///< Unique staging copy (deleted by the caller).
    std::vector<std::uint8_t> bytes;      ///< The captured bytes (authoritative, section 22.2 step 4).
    Sha256Digest sha256{};
    FileIdentity identity_at_open;
    FileIdentity identity_after_copy;     ///< Through the same handle after copying.
};

/// Copies `source` into a new file in `staging_directory` while hashing, honoring cancellation and
/// the byte limit (checked before allocation). The handle allows concurrent writers so a changing
/// source is detected by identity comparison instead of blocking the user's other applications.
[[nodiscard]] Result<CapturedFile> capture_file(const std::filesystem::path& source,
                                                const std::filesystem::path& staging_directory,
                                                std::uint64_t max_bytes, std::stop_token stop);

/// Writes bytes to `path` durably: temporary sibling, flush, atomic replace.
[[nodiscard]] Status write_file_durably(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);

/// Reads a whole file (bounded).
[[nodiscard]] Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path, std::uint64_t max_bytes);

/// SHA-256 of a file computed in bounded chunks; also returns its size.
struct FileDigest {
    Sha256Digest sha256{};
    std::uint64_t byte_count = 0;
};
[[nodiscard]] Result<FileDigest> hash_file(const std::filesystem::path& path, std::stop_token stop = {});

/// Case rule of the filesystem for source comparison keys: the Windows upper-case mapping on
/// Windows (case-insensitive NTFS semantics), exact bytes elsewhere.
[[nodiscard]] std::string filesystem_comparison_key(std::string_view utf8_relative_path);

/// UTF-8 text of a path (generic separators).
[[nodiscard]] std::string path_utf8(const std::filesystem::path& path);
/// Path from UTF-8 text (the inverse of path_utf8).
[[nodiscard]] std::filesystem::path path_from_utf8_text(std::string_view text);

/// Content-addressed immutable objects under `objects/ab/<sha256>.<ext>` (section 22.1).
class ObjectStore {
public:
    explicit ObjectStore(std::filesystem::path library_root);

    /// Path of an object; generated from the hash and a known extension only (an unknown
    /// extension yields a path inside the object directory that never exists).
    [[nodiscard]] std::filesystem::path object_path(const Sha256Digest& sha256, std::string_view extension) const;

    /// Publishes a flushed staging file under its hash. If the object already exists its size is
    /// verified and it is reused (the staging file is removed). Returns true when newly published.
    [[nodiscard]] Result<bool> publish(const std::filesystem::path& staging_file, const Sha256Digest& sha256,
                                       std::string_view extension, std::uint64_t byte_count);

    /// Publishes in-memory bytes (native revisions): staging write + flush + publish.
    [[nodiscard]] Result<bool> publish_bytes(std::span<const std::uint8_t> bytes, const Sha256Digest& sha256,
                                             std::string_view extension);

    /// Reads and verifies an object's bytes against its hash.
    [[nodiscard]] Result<std::vector<std::uint8_t>> read(const Sha256Digest& sha256, std::string_view extension,
                                                         std::uint64_t max_bytes = 1ull << 31) const;

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }
    [[nodiscard]] std::filesystem::path staging_directory() const { return root_ / "staging"; }

private:
    std::filesystem::path root_;
};

/// Exclusive per-library writer lock (`library.lock`). A second writer, in this or another
/// process, fails with ErrorCode::busy and a clear diagnostic.
class WriterLock {
public:
    WriterLock(const WriterLock&) = delete;
    WriterLock& operator=(const WriterLock&) = delete;
    WriterLock(WriterLock&&) noexcept;
    WriterLock& operator=(WriterLock&&) noexcept;
    ~WriterLock();

    /// Acquires the lock file in `library_root` without waiting.
    [[nodiscard]] static Result<WriterLock> acquire(const std::filesystem::path& library_root);

private:
    struct Handle;
    explicit WriterLock(std::unique_ptr<Handle> handle);
    std::unique_ptr<Handle> handle_;
};

} // namespace mwb::library
