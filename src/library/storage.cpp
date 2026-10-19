// Library filesystem adapter: stable capture, file identity, object publication, writer lock
// (specification sections 22.1, 22.2, 29.1). Platform code is confined to this file (section 4.6).

#include "mwb/library/storage.hpp"

#include "mwb/core/ids.hpp"
#include "mwb/document/file_system.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <system_error>

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
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mwb::library {

namespace {

constexpr std::size_t copy_chunk_bytes = 1u << 20;

std::string describe_path(const std::filesystem::path& path) { return path_utf8(path.filename()); }

#ifdef _WIN32

/// RAII owner of a Win32 file handle.
struct HandleCloser {
    void operator()(HANDLE handle) const { CloseHandle(handle); }
};
using FileHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

Error windows_error(std::string_view operation, const std::filesystem::path& path, DWORD code) {
    ErrorCode mapped = ErrorCode::io;
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
        mapped = ErrorCode::not_found;
    else if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION)
        mapped = ErrorCode::busy;
    return Error{mapped, std::format("{} failed (Windows error {})", operation, code), describe_path(path), nullptr};
}

std::int64_t filetime_to_unix_ms(const FILETIME& time) {
    const auto ticks = (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    constexpr std::uint64_t epoch_difference = 116444736000000000ull; // 1601 -> 1970 in 100 ns.
    return static_cast<std::int64_t>((ticks - epoch_difference) / 10000);
}

Result<FileIdentity> identity_of(HANDLE handle, const std::filesystem::path& path) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information))
        return std::unexpected(windows_error("GetFileInformationByHandle", path, GetLastError()));
    FileIdentity identity;
    const auto append32 = [&](DWORD value) {
        for (int shift = 0; shift < 32; shift += 8)
            identity.identity.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    };
    append32(information.dwVolumeSerialNumber);
    append32(information.nFileIndexHigh);
    append32(information.nFileIndexLow);
    identity.byte_count = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
    identity.modified_ms = filetime_to_unix_ms(information.ftLastWriteTime);
    return identity;
}

Result<FileHandle> open_for_read(const std::filesystem::path& path, DWORD access) {
    HANDLE handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::unexpected(windows_error("open", path, GetLastError()));
    return FileHandle(handle);
}

#else

/// RAII owner of a POSIX file descriptor.
class FileHandle {
public:
    explicit FileHandle(int descriptor) : descriptor_(descriptor) {}
    FileHandle(FileHandle&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    FileHandle& operator=(FileHandle&&) = delete;
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() {
        if (descriptor_ >= 0)
            ::close(descriptor_);
    }
    [[nodiscard]] int get() const { return descriptor_; }

private:
    int descriptor_;
};

Error posix_error(std::string_view operation, const std::filesystem::path& path, int code) {
    ErrorCode mapped = ErrorCode::io;
    if (code == ENOENT || code == ENOTDIR)
        mapped = ErrorCode::not_found;
    else if (code == EWOULDBLOCK || code == EAGAIN || code == EBUSY)
        mapped = ErrorCode::busy;
    return Error{mapped, std::format("{} failed (errno {})", operation, code), describe_path(path), nullptr};
}

Result<FileIdentity> identity_of(int descriptor, const std::filesystem::path& path) {
    struct stat status {};
    if (::fstat(descriptor, &status) != 0)
        return std::unexpected(posix_error("fstat", path, errno));
    FileIdentity identity;
    const auto append64 = [&](std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8)
            identity.identity.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    };
    append64(static_cast<std::uint64_t>(status.st_dev));
    append64(static_cast<std::uint64_t>(status.st_ino));
    identity.byte_count = static_cast<std::uint64_t>(status.st_size);
    identity.modified_ms = static_cast<std::int64_t>(status.st_mtim.tv_sec) * 1000 + status.st_mtim.tv_nsec / 1000000;
    return identity;
}

Result<FileHandle> open_for_read(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return std::unexpected(posix_error("open", path, errno));
    return FileHandle(descriptor);
}

#endif

/// Reads up to `size` bytes; returns the count (0 at end of file).
Result<std::size_t> read_some(
#ifdef _WIN32
    HANDLE handle,
#else
    int handle,
#endif
    std::uint8_t* buffer, std::size_t size, const std::filesystem::path& path) {
#ifdef _WIN32
    DWORD read = 0;
    if (!ReadFile(handle, buffer, static_cast<DWORD>(size), &read, nullptr))
        return std::unexpected(windows_error("read", path, GetLastError()));
    return static_cast<std::size_t>(read);
#else
    const auto read = ::read(handle, buffer, size);
    if (read < 0)
        return std::unexpected(posix_error("read", path, errno));
    return static_cast<std::size_t>(read);
#endif
}

Status write_all(std::ofstream& stream, std::span<const std::uint8_t> bytes, const std::filesystem::path& path) {
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream)
        return make_error(ErrorCode::io, "write failed", describe_path(path));
    return ok();
}

} // namespace

std::string path_utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

Result<FileIdentity> query_file_identity(const std::filesystem::path& path) {
#ifdef _WIN32
    auto handle = open_for_read(path, FILE_READ_ATTRIBUTES);
    if (!handle)
        return std::unexpected(std::move(handle).error());
    return identity_of(handle->get(), path);
#else
    auto handle = open_for_read(path);
    if (!handle)
        return std::unexpected(std::move(handle).error());
    return identity_of(handle->get(), path);
#endif
}

Result<CapturedFile> capture_file(const std::filesystem::path& source, const std::filesystem::path& staging_directory,
                                  std::uint64_t max_bytes, std::stop_token stop) {
#ifdef _WIN32
    auto handle = open_for_read(source, GENERIC_READ);
#else
    auto handle = open_for_read(source);
#endif
    if (!handle)
        return std::unexpected(std::move(handle).error());
    CapturedFile captured;
    auto identity = identity_of(handle->get(), source);
    if (!identity)
        return std::unexpected(std::move(identity).error());
    captured.identity_at_open = std::move(*identity);
    if (captured.identity_at_open.byte_count > max_bytes)
        return make_error(ErrorCode::limit_exceeded,
                          std::format("the file is larger than the {} byte capture limit", max_bytes),
                          describe_path(source));
    std::error_code error;
    std::filesystem::create_directories(staging_directory, error);
    captured.staging_path = staging_directory / (Uuid::generate().to_string() + ".part");
    std::ofstream staging(captured.staging_path, std::ios::binary | std::ios::trunc);
    if (!staging)
        return make_error(ErrorCode::io, "cannot create a staging file");
    const auto discard_staging = [&] {
        staging.close();
        std::error_code ignored;
        std::filesystem::remove(captured.staging_path, ignored);
    };
    captured.bytes.reserve(static_cast<std::size_t>(captured.identity_at_open.byte_count));
    Sha256Builder hasher;
    std::vector<std::uint8_t> buffer(copy_chunk_bytes);
    while (true) {
        if (stop.stop_requested()) {
            discard_staging();
            return make_error(ErrorCode::cancelled, "capture cancelled", describe_path(source));
        }
        auto read = read_some(handle->get(), buffer.data(), buffer.size(), source);
        if (!read) {
            discard_staging();
            return std::unexpected(std::move(read).error());
        }
        if (*read == 0)
            break;
        if (captured.bytes.size() + *read > max_bytes) {
            discard_staging();
            return make_error(ErrorCode::limit_exceeded, "the file grew beyond the capture limit", describe_path(source));
        }
        const std::span<const std::uint8_t> chunk(buffer.data(), *read);
        hasher.update(chunk);
        captured.bytes.insert(captured.bytes.end(), chunk.begin(), chunk.end());
        if (auto written = write_all(staging, chunk, captured.staging_path); !written) {
            discard_staging();
            return std::unexpected(std::move(written).error());
        }
    }
    staging.close();
    if (!staging) {
        discard_staging();
        return make_error(ErrorCode::io, "cannot finish the staging file");
    }
    captured.sha256 = hasher.finish();
    auto after = identity_of(handle->get(), source);
    if (!after) {
        discard_staging();
        return std::unexpected(std::move(after).error());
    }
    captured.identity_after_copy = std::move(*after);
    return captured;
}

Status write_file_durably(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    auto temporary = path;
    temporary += ".tmp-" + Uuid::generate().to_string();
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
            return make_error(ErrorCode::io, "cannot create a temporary file", describe_path(path));
        MWB_TRY(write_all(stream, bytes, temporary));
        stream.close();
        if (!stream)
            return make_error(ErrorCode::io, "cannot finish a temporary file", describe_path(path));
    }
    if (auto flushed = document::file_system::flush_file(temporary); !flushed) {
        std::filesystem::remove(temporary, error);
        return flushed;
    }
    if (auto replaced = document::file_system::replace_file(temporary, path); !replaced) {
        std::filesystem::remove(temporary, error);
        return replaced;
    }
    return ok();
}

Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path, std::uint64_t max_bytes) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return make_error(error == std::errc::no_such_file_or_directory ? ErrorCode::not_found : ErrorCode::io,
                          "cannot read the file size", describe_path(path));
    if (size > max_bytes)
        return make_error(ErrorCode::limit_exceeded, "the file exceeds the read limit", describe_path(path));
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return make_error(ErrorCode::io, "cannot open the file", describe_path(path));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size()))
        return make_error(ErrorCode::io, "short read", describe_path(path));
    return bytes;
}

Result<FileDigest> hash_file(const std::filesystem::path& path, std::stop_token stop) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        std::error_code error;
        if (!std::filesystem::exists(path, error))
            return make_error(ErrorCode::not_found, "the file is missing", describe_path(path));
        return make_error(ErrorCode::io, "cannot open the file", describe_path(path));
    }
    Sha256Builder hasher;
    FileDigest digest;
    std::vector<char> buffer(copy_chunk_bytes);
    while (stream) {
        if (stop.stop_requested())
            return make_error(ErrorCode::cancelled, "hashing cancelled");
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(stream.gcount());
        hasher.update(std::span(reinterpret_cast<const std::uint8_t*>(buffer.data()), count));
        digest.byte_count += count;
    }
    if (stream.bad())
        return make_error(ErrorCode::io, "read failed", describe_path(path));
    digest.sha256 = hasher.finish();
    return digest;
}

std::string filesystem_comparison_key(std::string_view utf8_relative_path) {
#ifdef _WIN32
    if (utf8_relative_path.empty())
        return {};
    const auto source_length = static_cast<int>(utf8_relative_path.size());
    const int wide_length = MultiByteToWideChar(CP_UTF8, 0, utf8_relative_path.data(), source_length, nullptr, 0);
    if (wide_length <= 0)
        return std::string(utf8_relative_path);
    std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_relative_path.data(), source_length, wide.data(), wide_length);
    // CharUpperBuffW applies the system upper-case table, the same mapping NTFS uses for its
    // case-insensitive comparisons of ordinary names.
    CharUpperBuffW(wide.data(), static_cast<DWORD>(wide.size()));
    const int narrow_length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length, nullptr, 0, nullptr, nullptr);
    std::string key(static_cast<std::size_t>(std::max(0, narrow_length)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length, key.data(), narrow_length, nullptr, nullptr);
    return key;
#else
    return std::string(utf8_relative_path);
#endif
}

ObjectStore::ObjectStore(std::filesystem::path library_root) : root_(std::move(library_root)) {}

std::filesystem::path ObjectStore::object_path(const Sha256Digest& sha256, std::string_view extension) const {
    const auto hex = to_hex(sha256);
    return root_ / "objects" / hex.substr(0, 2) / (hex + "." + std::string(extension));
}

Result<bool> ObjectStore::publish(const std::filesystem::path& staging_file, const Sha256Digest& sha256,
                                  std::string_view extension, std::uint64_t byte_count) {
    const auto target = object_path(sha256, extension);
    std::error_code error;
    if (std::filesystem::exists(target, error)) {
        const auto size = std::filesystem::file_size(target, error);
        if (!error && size == byte_count) {
            std::filesystem::remove(staging_file, error);
            return false;
        }
        // A present but damaged object is replaced by the verified captured bytes below.
    }
    std::filesystem::create_directories(target.parent_path(), error);
    if (error)
        return make_error(ErrorCode::io, "cannot create the object directory");
    MWB_TRY(document::file_system::flush_file(staging_file));
    MWB_TRY(document::file_system::replace_file(staging_file, target));
    return true;
}

Result<bool> ObjectStore::publish_bytes(std::span<const std::uint8_t> bytes, const Sha256Digest& sha256,
                                        std::string_view extension) {
    std::error_code error;
    std::filesystem::create_directories(staging_directory(), error);
    const auto staging_file = staging_directory() / (Uuid::generate().to_string() + ".part");
    {
        std::ofstream stream(staging_file, std::ios::binary | std::ios::trunc);
        if (!stream)
            return make_error(ErrorCode::io, "cannot create a staging file");
        MWB_TRY(write_all(stream, bytes, staging_file));
        stream.close();
        if (!stream)
            return make_error(ErrorCode::io, "cannot finish a staging file");
    }
    auto published = publish(staging_file, sha256, extension, bytes.size());
    if (!published)
        std::filesystem::remove(staging_file, error);
    return published;
}

Result<std::vector<std::uint8_t>> ObjectStore::read(const Sha256Digest& sha256, std::string_view extension,
                                                    std::uint64_t max_bytes) const {
    auto bytes = read_file(object_path(sha256, extension), max_bytes);
    if (!bytes)
        return bytes;
    if (mwb::sha256(*bytes) != sha256)
        return make_error(ErrorCode::invalid_data, "the stored object does not match its hash", to_hex(sha256));
    return bytes;
}

// --- Writer lock ----------------------------------------------------------------------------------

struct WriterLock::Handle {
#ifdef _WIN32
    FileHandle file;
#else
    FileHandle file{-1};
#endif
};

WriterLock::WriterLock(std::unique_ptr<Handle> handle) : handle_(std::move(handle)) {}
WriterLock::WriterLock(WriterLock&&) noexcept = default;
WriterLock& WriterLock::operator=(WriterLock&&) noexcept = default;
WriterLock::~WriterLock() = default;

Result<WriterLock> WriterLock::acquire(const std::filesystem::path& library_root) {
    const auto path = library_root / "library.lock";
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION)
            return make_error(ErrorCode::busy,
                              "the library is already open for writing by another MIDI Workbench process or window; "
                              "close it there or open this library read-only",
                              describe_path(library_root));
        return std::unexpected(windows_error("lock the library", path, code));
    }
    auto owned = std::make_unique<Handle>();
    owned->file = FileHandle(handle);
    return WriterLock(std::move(owned));
#else
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (descriptor < 0)
        return std::unexpected(posix_error("lock the library", path, errno));
    FileHandle file(descriptor);
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK)
            return make_error(ErrorCode::busy,
                              "the library is already open for writing by another MIDI Workbench process or window; "
                              "close it there or open this library read-only",
                              describe_path(library_root));
        return std::unexpected(posix_error("lock the library", path, errno));
    }
    auto owned = std::make_unique<Handle>(Handle{std::move(file)});
    return WriterLock(std::move(owned));
#endif
}

} // namespace mwb::library
