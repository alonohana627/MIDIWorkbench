#pragma once

#include "mwb/core/error.hpp"
#include "mwb/library/library.hpp"

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

/// Backup, restore, verification and repair of a managed library (specification section 30).
namespace mwb::library {

/// File name of the marker that labels a backup (or restore) that never completed.
inline constexpr std::string_view incomplete_marker_name = "INCOMPLETE";
/// File name of the backup manifest.
inline constexpr std::string_view manifest_name = "manifest.json";

/// Result of a backup.
struct BackupReport {
    std::filesystem::path directory;
    std::size_t object_count = 0;
    std::uint64_t byte_count = 0;
    bool complete = false;
};

/// Section 30.1: suspends GC, copies the catalog with the online backup API, reads the referenced
/// object set from the completed copy, copies objects while verifying hashes, writes and verifies
/// the manifest (paths, sizes, SHA-256) and only then removes the incomplete marker. Any missing or
/// corrupt object fails the backup, which keeps its INCOMPLETE marker.
[[nodiscard]] Result<BackupReport> create_backup(Library& library, const std::filesystem::path& destination,
                                                 std::stop_token stop = {});

/// Verification options.
struct VerifyOptions {
    bool objects = true; ///< Hash every object file (section 30.3).
};

/// Read-only verification findings.
struct VerifyReport {
    std::vector<std::string> integrity_messages;   ///< Anything other than "ok" from integrity_check.
    std::size_t foreign_key_violations = 0;
    std::size_t objects_checked = 0;
    std::vector<std::string> missing_objects;      ///< SHA-256 hex.
    std::vector<std::string> corrupt_objects;      ///< SHA-256 hex (size or hash mismatch).
    [[nodiscard]] bool ok() const {
        return integrity_messages.empty() && foreign_key_violations == 0 && missing_objects.empty() &&
               corrupt_objects.empty();
    }
    /// Versioned JSON for `library verify --json`.
    [[nodiscard]] std::string to_json() const;
};

/// Integrity check, foreign-key check and object hash verification without modifying anything.
/// Works on a library whose writer is open elsewhere (WAL readers do not block it).
[[nodiscard]] Result<VerifyReport> verify_library(const std::filesystem::path& library_root,
                                                  const VerifyOptions& options = {}, std::stop_token stop = {});

/// Section 30.2: validates the manifest and every file hash, restores into a new empty directory,
/// verifies the restored library and opens it once. The backup is never modified.
[[nodiscard]] Result<VerifyReport> restore_backup(const std::filesystem::path& backup,
                                                  const std::filesystem::path& destination);

/// Repair result.
struct RepairReport {
    std::filesystem::path safety_backup;
    bool full_backup = false;     ///< False when only the catalog could be saved (damaged objects).
    std::size_t fts_rows = 0;
    std::size_t derived_rows_removed = 0;
};

/// Which repairs `library repair` runs after its one safety backup.
struct RepairOptions {
    bool rebuild_fts = false;
    bool rebuild_derived = false; ///< Drops derived indexes; the next analyze rebuilds them.
};

/// `library repair`: safety backup first, then the chosen repairs.
[[nodiscard]] Result<RepairReport> repair_library(Library& library, const RepairOptions& options);
/// `library repair --rebuild-fts`: safety backup first, then one-transaction FTS rebuild.
[[nodiscard]] Result<RepairReport> repair_rebuild_fts(Library& library);

} // namespace mwb::library
