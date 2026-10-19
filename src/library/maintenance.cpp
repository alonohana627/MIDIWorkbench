// Backup, restore, verify and repair (specification section 30).

#include "mwb/library/maintenance.hpp"

#include "mwb/document/file_system.hpp"
#include "mwb/document/sqlite_support.hpp"
#include "mwb/library/catalog_connection.hpp"
#include "mwb/library/storage.hpp"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <format>
#include <algorithm>
#include <fstream>
#include <set>

namespace mwb::library {

namespace sqlite_support = document::sqlite_support;
using Json = nlohmann::json;

namespace {

constexpr std::uint64_t manifest_limit = 512ull * 1024ull * 1024ull;

/// Copies a file while hashing it, then flushes the copy.
Result<FileDigest> copy_with_hash(const std::filesystem::path& source, const std::filesystem::path& destination,
                                  std::stop_token stop) {
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        std::error_code error;
        if (!std::filesystem::exists(source, error))
            return make_error(ErrorCode::not_found, "a file is missing", path_utf8(source.filename()));
        return make_error(ErrorCode::io, "cannot open a file", path_utf8(source.filename()));
    }
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output)
        return make_error(ErrorCode::io, "cannot create a file", path_utf8(destination.filename()));
    Sha256Builder hasher;
    FileDigest digest;
    std::vector<char> buffer(1u << 20);
    while (input) {
        if (stop.stop_requested())
            return make_error(ErrorCode::cancelled, "copy cancelled");
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count == 0)
            break;
        hasher.update(std::span(reinterpret_cast<const std::uint8_t*>(buffer.data()), count));
        output.write(buffer.data(), static_cast<std::streamsize>(count));
        digest.byte_count += count;
    }
    if (input.bad())
        return make_error(ErrorCode::io, "read failed", path_utf8(source.filename()));
    output.close();
    if (!output)
        return make_error(ErrorCode::io, "write failed", path_utf8(destination.filename()));
    MWB_TRY(document::file_system::flush_file(destination));
    digest.sha256 = hasher.finish();
    return digest;
}

Status write_text_durably(const std::filesystem::path& path, const std::string& text) {
    return write_file_durably(path, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

bool is_hex(std::string_view text) {
    return std::ranges::all_of(text, [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

/// Only the three kinds of library files may appear in a manifest; anything else (absolute paths,
/// "..", other directories) is rejected before any file is touched.
bool is_allowed_manifest_path(std::string_view path) {
    if (path == "library.sqlite" || path == "library.json")
        return true;
    // objects/ab/<64 hex>.<ext>
    if (path.size() != 8 + 3 + 64 + 4 || !path.starts_with("objects/") || path[10] != '/')
        return false;
    const auto prefix = path.substr(8, 2);
    const auto hash = path.substr(11, 64);
    const auto extension = path.substr(75);
    return is_hex(prefix) && is_hex(hash) && hash.substr(0, 2) == prefix &&
           (extension == ".mid" || extension == ".rmi" || extension == ".mwb");
}

/// Opens a catalog for verification: reads only (query_only), but a read-write handle so WAL
/// shared memory works whether or not a writer is running.
Result<std::unique_ptr<SQLite::Database>> open_for_verification(const std::filesystem::path& database_path) {
    return sqlite_support::guarded("open catalog for verification", [&]() -> Result<std::unique_ptr<SQLite::Database>> {
        std::error_code error;
        if (!std::filesystem::is_regular_file(database_path, error))
            return make_error(ErrorCode::not_found, "the library catalog is missing");
        auto database = std::make_unique<SQLite::Database>(sqlite_support::path_to_utf8(database_path),
                                                           SQLite::OPEN_READWRITE, 5000);
        int enabled = 0;
        sqlite3_db_config(database->getHandle(), SQLITE_DBCONFIG_DEFENSIVE, 1, &enabled);
        sqlite3_db_config(database->getHandle(), SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, &enabled);
        database->exec("PRAGMA query_only = ON");
        return database;
    });
}

} // namespace

std::string VerifyReport::to_json() const {
    Json object{{"version", 1},
                {"ok", ok()},
                {"integrity", integrity_messages},
                {"foreign_key_violations", foreign_key_violations},
                {"objects_checked", objects_checked},
                {"missing_objects", missing_objects},
                {"corrupt_objects", corrupt_objects}};
    return object.dump();
}

Result<VerifyReport> verify_library(const std::filesystem::path& library_root, const VerifyOptions& options,
                                    std::stop_token stop) {
    auto database = open_for_verification(library_root / "library.sqlite");
    if (!database)
        return std::unexpected(std::move(database).error());
    MWB_TRY(check_catalog_header(**database));
    VerifyReport report;
    struct ObjectRow {
        Sha256Digest sha;
        std::string extension;
        std::uint64_t byte_count;
    };
    auto objects = sqlite_support::guarded("verify catalog", [&]() -> Result<std::vector<ObjectRow>> {
        SQLite::Statement integrity(**database, "PRAGMA integrity_check");
        while (integrity.executeStep()) {
            const auto message = integrity.getColumn(0).getString();
            if (message != "ok")
                report.integrity_messages.push_back(message);
        }
        SQLite::Statement foreign_keys(**database, "PRAGMA foreign_key_check");
        while (foreign_keys.executeStep())
            ++report.foreign_key_violations;
        std::vector<ObjectRow> rows;
        if (!options.objects)
            return rows;
        SQLite::Statement query(**database, "SELECT sha256, extension, byte_count FROM object_file ORDER BY sha256");
        while (query.executeStep()) {
            auto digest = sqlite_support::column_digest(query, 0, "object hash");
            if (!digest) {
                report.integrity_messages.push_back("object_file row with a malformed hash");
                continue;
            }
            rows.push_back(ObjectRow{*digest, query.getColumn(1).getString(),
                                     static_cast<std::uint64_t>(query.getColumn(2).getInt64())});
        }
        return rows;
    });
    if (!objects)
        return std::unexpected(std::move(objects).error());
    database->reset(); // No connection is held while hashing files.
    ObjectStore store(library_root);
    for (const auto& object : *objects) {
        if (stop.stop_requested())
            return make_error(ErrorCode::cancelled, "verification cancelled");
        ++report.objects_checked;
        const auto digest = hash_file(store.object_path(object.sha, object.extension), stop);
        if (!digest) {
            if (digest.error().code == ErrorCode::cancelled)
                return std::unexpected(digest.error());
            report.missing_objects.push_back(to_hex(object.sha));
            continue;
        }
        if (digest->byte_count != object.byte_count || digest->sha256 != object.sha)
            report.corrupt_objects.push_back(to_hex(object.sha));
    }
    return report;
}

Result<BackupReport> create_backup(Library& library, const std::filesystem::path& destination, std::stop_token stop) {
    std::error_code error;
    if (std::filesystem::exists(destination, error) && !std::filesystem::is_empty(destination, error))
        return make_error(ErrorCode::invalid_argument, "the backup destination must be empty or missing");
    std::filesystem::create_directories(destination, error);
    if (error)
        return make_error(ErrorCode::io, "cannot create the backup directory");
    MWB_TRY(write_text_durably(destination / incomplete_marker_name, "backup in progress\n"));
    Library::GarbageCollectionPause pause(library); // Step 1.
    BackupReport report;
    report.directory = destination;
    // Step 2: consistent catalog copy.
    const auto catalog_copy = destination / "library.sqlite";
    MWB_TRY(library.backup_catalog(catalog_copy));
    MWB_TRY(document::file_system::flush_file(catalog_copy));
    // Step 3: the object set comes from the completed copy, not the live catalog.
    struct ObjectRow {
        Sha256Digest sha;
        std::string extension;
        std::uint64_t byte_count;
    };
    auto copy_database = open_for_verification(catalog_copy);
    if (!copy_database)
        return std::unexpected(std::move(copy_database).error());
    std::int64_t user_version = 0;
    auto objects = sqlite_support::guarded("backup object set", [&]() -> Result<std::vector<ObjectRow>> {
        user_version = sqlite_support::pragma_integer(**copy_database, "user_version");
        SQLite::Statement query(**copy_database,
                                "SELECT o.sha256, o.extension, o.byte_count FROM object_file AS o WHERE EXISTS (SELECT 1 "
                                "FROM revision AS r WHERE r.object_sha256 = o.sha256) OR EXISTS (SELECT 1 FROM "
                                "source_location AS s WHERE s.last_object_sha256 = o.sha256) ORDER BY o.sha256");
        std::vector<ObjectRow> rows;
        while (query.executeStep()) {
            auto digest = sqlite_support::column_digest(query, 0, "object hash");
            if (!digest)
                return std::unexpected(std::move(digest).error());
            rows.push_back(ObjectRow{*digest, query.getColumn(1).getString(),
                                     static_cast<std::uint64_t>(query.getColumn(2).getInt64())});
        }
        return rows;
    });
    copy_database->reset();
    if (!objects)
        return std::unexpected(std::move(objects).error());

    Json files = Json::array();
    const auto add_file = [&](const std::string& relative, const FileDigest& digest) {
        files.push_back(Json{{"path", relative}, {"bytes", digest.byte_count}, {"sha256", to_hex(digest.sha256)}});
        report.byte_count += digest.byte_count;
    };
    auto catalog_digest = hash_file(catalog_copy, stop);
    if (!catalog_digest)
        return std::unexpected(std::move(catalog_digest).error());
    add_file("library.sqlite", *catalog_digest);
    auto descriptor = copy_with_hash(library.root() / "library.json", destination / "library.json", stop);
    if (!descriptor)
        return std::unexpected(std::move(descriptor).error());
    add_file("library.json", *descriptor);
    // Step 4: copy immutable objects, verifying each hash.
    ObjectStore live(library.root());
    ObjectStore copy(destination);
    for (const auto& object : *objects) {
        const auto source = live.object_path(object.sha, object.extension);
        const auto target = copy.object_path(object.sha, object.extension);
        auto digest = copy_with_hash(source, target, stop);
        if (!digest)
            return std::unexpected(digest.error().wrapped(ErrorCode::invalid_data,
                                                          "backup incomplete: an object could not be copied"));
        if (digest->sha256 != object.sha || digest->byte_count != object.byte_count)
            return make_error(ErrorCode::invalid_data, "backup incomplete: a stored object is corrupt", to_hex(object.sha));
        add_file(path_utf8(target.lexically_relative(destination)), *digest);
        ++report.object_count;
    }
    // Step 5: write and verify the manifest, then publish by removing the marker.
    Json manifest{{"format", "midi-workbench-backup"},
                  {"version", 1},
                  {"library_uuid", library.uuid().to_string()},
                  {"catalog", {{"application_id", 1297564204}, {"user_version", user_version}}},
                  {"created_ms", now_utc_ms()},
                  {"derived_indexes", "included"},
                  {"files", files}};
    const auto manifest_text = manifest.dump(1);
    MWB_TRY(write_text_durably(destination / manifest_name, manifest_text));
    auto reread = read_file(destination / manifest_name, manifest_limit);
    if (!reread || std::string(reread->begin(), reread->end()) != manifest_text)
        return make_error(ErrorCode::io, "the backup manifest could not be verified");
    for (const auto& entry : files) {
        const auto size = std::filesystem::file_size(destination / entry["path"].get<std::string>(), error);
        if (error || size != entry["bytes"].get<std::uint64_t>())
            return make_error(ErrorCode::io, "a backup file does not match the manifest", entry["path"].get<std::string>());
    }
    std::filesystem::remove(destination / incomplete_marker_name, error);
    if (error)
        return make_error(ErrorCode::io, "cannot remove the incomplete marker");
    report.complete = true;
    return report;
}

Result<VerifyReport> restore_backup(const std::filesystem::path& backup, const std::filesystem::path& destination) {
    std::error_code error;
    if (std::filesystem::exists(backup / incomplete_marker_name, error))
        return make_error(ErrorCode::invalid_data, "the backup is incomplete and cannot be restored");
    if (std::filesystem::exists(destination, error) && !std::filesystem::is_empty(destination, error))
        return make_error(ErrorCode::invalid_argument, "restore needs a new empty directory");
    auto manifest_bytes = read_file(backup / manifest_name, manifest_limit);
    if (!manifest_bytes)
        return std::unexpected(manifest_bytes.error().wrapped(ErrorCode::invalid_data, "the backup has no manifest"));
    const auto manifest = Json::parse(manifest_bytes->begin(), manifest_bytes->end(), nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object() || !manifest.contains("version") ||
        !manifest["version"].is_number_integer() || manifest["version"].get<int>() != 1 || !manifest.contains("files") ||
        !manifest["files"].is_array())
        return make_error(ErrorCode::invalid_data, "the backup manifest is malformed");
    struct Entry {
        std::string path;
        std::uint64_t bytes;
        Sha256Digest sha;
    };
    std::vector<Entry> entries;
    std::set<std::string> seen;
    for (const auto& file : manifest["files"]) {
        if (!file.is_object() || !file.contains("path") || !file["path"].is_string() || !file.contains("bytes") ||
            !file["bytes"].is_number_unsigned() || !file.contains("sha256") || !file["sha256"].is_string())
            return make_error(ErrorCode::invalid_data, "a manifest entry is malformed");
        Entry entry{file["path"].get<std::string>(), file["bytes"].get<std::uint64_t>(), {}};
        if (!is_allowed_manifest_path(entry.path) || !seen.insert(entry.path).second)
            return make_error(ErrorCode::invalid_data, "the manifest names a path outside the library layout", entry.path);
        if (!parse_sha256_hex(file["sha256"].get<std::string>(), entry.sha))
            return make_error(ErrorCode::invalid_data, "a manifest hash is malformed", entry.path);
        entries.push_back(std::move(entry));
    }
    if (!seen.contains("library.sqlite") || !seen.contains("library.json"))
        return make_error(ErrorCode::invalid_data, "the manifest lacks the catalog or the descriptor");
    // Validate every source file before anything is written.
    for (const auto& entry : entries) {
        const auto digest = hash_file(backup / entry.path);
        if (!digest)
            return make_error(ErrorCode::invalid_data, "a backup file is missing", entry.path);
        if (digest->byte_count != entry.bytes || digest->sha256 != entry.sha)
            return make_error(ErrorCode::invalid_data, "a backup file does not match its manifest hash", entry.path);
    }
    std::filesystem::create_directories(destination, error);
    if (error)
        return make_error(ErrorCode::io, "cannot create the restore directory");
    MWB_TRY(write_text_durably(destination / incomplete_marker_name, "restore in progress\n"));
    for (const auto& entry : entries) {
        auto digest = copy_with_hash(backup / entry.path, destination / entry.path, {});
        if (!digest)
            return std::unexpected(std::move(digest).error());
        if (digest->sha256 != entry.sha)
            return make_error(ErrorCode::io, "a restored file changed while it was copied", entry.path);
    }
    for (const auto* directory : {"objects", "staging", "cache", "backups"})
        std::filesystem::create_directories(destination / directory, error);
    auto report = verify_library(destination);
    if (!report)
        return report;
    if (!report->ok())
        return make_error(ErrorCode::invalid_data, "the restored library failed verification", report->to_json());
    {
        auto opened = Library::open(destination);
        if (!opened)
            return std::unexpected(opened.error().wrapped(ErrorCode::invalid_data, "the restored library cannot be opened"));
    }
    std::filesystem::remove(destination / incomplete_marker_name, error);
    return report;
}

Result<RepairReport> repair_rebuild_fts(Library& library) {
    RepairReport report;
    report.safety_backup = library.root() / "backups" / std::format("{}-pre-repair", now_utc_ms());
    auto backup = create_backup(library, report.safety_backup);
    if (backup) {
        report.full_backup = true;
    } else {
        // Damaged objects prevent a verified full backup; keep at least the catalog evidence.
        const auto catalog_only = report.safety_backup.string() + "-catalog";
        std::error_code error;
        std::filesystem::create_directories(catalog_only, error);
        MWB_TRY(library.backup_catalog(std::filesystem::path(catalog_only) / "library.sqlite"));
        report.safety_backup = catalog_only;
    }
    auto rows = library.rebuild_fts();
    if (!rows)
        return std::unexpected(std::move(rows).error());
    report.fts_rows = *rows;
    return report;
}

} // namespace mwb::library
