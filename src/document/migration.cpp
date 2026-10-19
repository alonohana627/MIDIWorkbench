#include "mwb/document/migration.hpp"

#include "mwb/core/hashing.hpp"
#include "mwb/document/schemas.hpp"

#include <array>
#include <string>

namespace mwb::document::migration {

namespace {

constexpr std::array<SchemaChecksum, 1> native_checksums{
    SchemaChecksum{1, "ae2f58dc4172f4ca77627d61cab6bab4ab09648824ae49c0355c5f49763240a4"},
};

constexpr std::array<SchemaChecksum, 1> recovery_checksums{
    SchemaChecksum{1, "44f2c189621c86a54e3e2b69376653e171f9b8870245bce07fbfadbb4df4aa5d"},
};

/// SHA-256 of SQL text with carriage returns removed, so a checkout with CRLF line endings
/// yields the same checksum as the canonical LF text.
std::string normalized_sha256_hex(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (const char character : text)
        if (character != '\r')
            normalized.push_back(character);
    return to_hex(sha256(normalized));
}

const char* family_name(SchemaFamily family) {
    switch (family) {
    case SchemaFamily::native: return "native document";
    case SchemaFamily::recovery: return "recovery database";
    }
    return "database";
}

} // namespace

std::int32_t current_version(SchemaFamily family) {
    switch (family) {
    case SchemaFamily::native: return 1;
    case SchemaFamily::recovery: return 1;
    }
    return 1;
}

std::string_view current_ddl(SchemaFamily family) {
    switch (family) {
    case SchemaFamily::native: return schemas::native_v1;
    case SchemaFamily::recovery: return schemas::recovery_v1;
    }
    return schemas::native_v1;
}

std::span<const SchemaChecksum> recorded_checksums(SchemaFamily family) {
    switch (family) {
    case SchemaFamily::native: return native_checksums;
    case SchemaFamily::recovery: return recovery_checksums;
    }
    return {};
}

std::span<const MigrationStep> migration_steps(SchemaFamily family) {
    switch (family) {
    case SchemaFamily::native:
    case SchemaFamily::recovery: return {};
    }
    return {};
}

Status verify_checksums(SchemaFamily family) {
    const auto checksums = recorded_checksums(family);
    if (checksums.empty() || checksums.back().version != current_version(family))
        return make_error(ErrorCode::internal, std::string(family_name(family)) + " schema has no recorded checksum");
    if (normalized_sha256_hex(current_ddl(family)) != checksums.back().ddl_sha256_hex)
        return make_error(ErrorCode::internal,
                          std::string(family_name(family)) + " schema DDL differs from its recorded checksum");
    for (const auto& step : migration_steps(family)) {
        if (normalized_sha256_hex(step.sql) != step.sql_sha256_hex)
            return make_error(ErrorCode::internal, std::string(family_name(family)) + " migration from version " +
                                                       std::to_string(step.from_version) + " fails its checksum");
    }
    return ok();
}

Result<SchemaAccess> classify_version(SchemaFamily family, std::int64_t file_version) {
    const std::int32_t current = current_version(family);
    if (file_version <= 0)
        return make_error(ErrorCode::invalid_data,
                          std::string("not a ") + family_name(family) + " (schema version " +
                              std::to_string(file_version) + ")");
    if (file_version > current)
        return make_error(ErrorCode::unsupported,
                          std::string(family_name(family)) + " uses schema version " + std::to_string(file_version) +
                              ", newer than the supported version " + std::to_string(current) +
                              "; it cannot be opened for writing");
    if (file_version == current)
        return SchemaAccess::read_write;
    // Every version between file_version and current needs a step.
    const auto steps = migration_steps(family);
    for (std::int64_t version = file_version; version < current; ++version) {
        bool found = false;
        for (const auto& step : steps)
            found = found || step.from_version == version;
        if (!found)
            return make_error(ErrorCode::unsupported, std::string(family_name(family)) + " schema version " +
                                                          std::to_string(file_version) + " has no migration path");
    }
    return SchemaAccess::needs_migration;
}

} // namespace mwb::document::migration
