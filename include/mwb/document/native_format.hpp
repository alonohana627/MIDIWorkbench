#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/model.hpp"
#include "mwb/core/validation.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

/// Native .mwb project files: closed, self-contained SQLite databases with the schema of
/// specification section 23.1 (application_id 1297564209, user_version 1).
namespace mwb::document {

/// SQLite application_id of native documents ("MWB1").
inline constexpr std::int32_t native_application_id = 1297564209;
/// Current native schema version.
inline constexpr std::int32_t native_schema_version = 1;

/// Pairing policy recorded in note_link.pairing_policy: core pair_notes FIFO pairing.
inline constexpr std::string_view note_pairing_policy = "fifo-v1";

/// Schema version written to document_setting.schema_version.
inline constexpr std::int32_t document_setting_schema_version = 1;

/// Limits for opening externally supplied native files (section 28.3).
struct NativeLoadLimits {
    std::uint64_t max_file_bytes = 1024ull * 1024ull * 1024ull;
    DocumentLimits document;
    /// Hard cap on stored events read into memory (parsed-events limit of section 28.3). Above
    /// document.max_events_per_document but below this cap a file opens read-only.
    std::uint64_t max_stored_events = 2'000'000;
    /// Largest single BLOB the bounded connection may return (source bytes are at most 256 MiB).
    std::int64_t max_blob_bytes = 256ll * 1024 * 1024 + 64 * 1024;
};

/// A validated native document plus how it may be used.
struct NativeOpenResult {
    DocumentData document;
    /// True when saving would lose information (for example opaque records this version cannot
    /// represent) or when the document exceeds the editable limits; the caller opens it for
    /// inspection/export only.
    bool read_only = false;
    /// Warnings and informational findings (validation and preservation notes).
    std::vector<Diagnostic> diagnostics;
};

/// Keys allowed in document_setting (section 23.1): presentation preferences, never actions.
[[nodiscard]] std::span<const std::string_view> document_setting_keys();
/// True when `key` is a whitelisted document_setting key.
[[nodiscard]] bool is_allowed_document_setting(std::string_view key);

/// Saves atomically: complete temporary database in the destination directory (rollback journal
/// DELETE mode, synchronous FULL), integrity check, close, flush, then replace the target. The old
/// file is kept if any step fails (section 21.3).
[[nodiscard]] Status save_native(const DocumentData& document, const std::filesystem::path& path);

/// Serializes to the bytes of a native file (used for immutable library objects and recovery
/// checkpoints).
[[nodiscard]] Result<std::vector<std::uint8_t>> native_to_bytes(const DocumentData& document);

/// Opens and validates a native file: application id, schema version, whitelist of schema
/// objects (no triggers, views or extra virtual tables), JSON contracts and domain invariants,
/// using a bounded read-only connection (sections 22.5 and 23.1). A file that can only be opened
/// read-only is refused with ErrorCode::unsupported; use open_native() to inspect it.
[[nodiscard]] Result<DocumentData> load_native(const std::filesystem::path& path, const NativeLoadLimits& limits = {});

/// Loads a native document from in-memory bytes.
[[nodiscard]] Result<DocumentData> native_from_bytes(std::span<const std::uint8_t> bytes,
                                                     const NativeLoadLimits& limits = {});

/// Like load_native() but also accepts files that may only be inspected, reporting why.
[[nodiscard]] Result<NativeOpenResult> open_native(const std::filesystem::path& path,
                                                   const NativeLoadLimits& limits = {});

/// Like native_from_bytes() but also accepts documents that may only be inspected.
[[nodiscard]] Result<NativeOpenResult> open_native_bytes(std::span<const std::uint8_t> bytes,
                                                         const NativeLoadLimits& limits = {});

} // namespace mwb::document
