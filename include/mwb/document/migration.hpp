#pragma once

#include "mwb/core/error.hpp"

#include <cstdint>
#include <span>
#include <string_view>

/// Schema migration rules of specification section 23.4 for the native and recovery databases.
/// The native format has only version 1; the recovery database has version 2 (in-progress capture
/// chunks) with one in-place step from version 1. A newer schema than the application knows is
/// always refused.
namespace mwb::document::migration {

/// Which database family a schema version belongs to.
enum class SchemaFamily : std::uint8_t { native, recovery };

/// Recorded identity of one schema version: the SHA-256 (lowercase hex) of its exact DDL text.
/// A mismatch means the shipped DDL was edited without a version bump.
struct SchemaChecksum {
    std::int32_t version = 0;
    std::string_view ddl_sha256_hex;
};

/// One migration step from `from_version` to `from_version + 1`. Steps run in one transaction
/// on a copy (native documents are migrated read-only into a new file, never in place).
struct MigrationStep {
    std::int32_t from_version = 0;
    std::string_view description;
    std::string_view sql;
    std::string_view sql_sha256_hex; ///< Checksum of `sql`, verified before the step runs.
};

/// Current schema version of a family.
[[nodiscard]] std::int32_t current_version(SchemaFamily family);

/// DDL text of the current version of a family.
[[nodiscard]] std::string_view current_ddl(SchemaFamily family);

/// Recorded DDL checksums of every released version of a family, oldest first.
[[nodiscard]] std::span<const SchemaChecksum> recorded_checksums(SchemaFamily family);

/// Migration steps of a family, ordered by from_version (empty while only version 1 exists).
/// Native documents are migrated read-only into a new file; the recovery database in place.
[[nodiscard]] std::span<const MigrationStep> migration_steps(SchemaFamily family);

/// Verifies the shipped DDL of the current version against its recorded checksum and every
/// migration step against its own checksum.
[[nodiscard]] Status verify_checksums(SchemaFamily family);

/// Access a file of `file_version` may be opened with.
enum class SchemaAccess : std::uint8_t {
    read_write,        ///< Current version.
    needs_migration,   ///< Older version with a complete migration path.
};

/// Decides how a file of the given user_version may be used. A newer version than the
/// application knows is refused (write access is never granted to an unknown schema); an older
/// version without a migration path is unsupported; zero or negative is not a database of this
/// family.
[[nodiscard]] Result<SchemaAccess> classify_version(SchemaFamily family, std::int64_t file_version);

} // namespace mwb::document::migration
