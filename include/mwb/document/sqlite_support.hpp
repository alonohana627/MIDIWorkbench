#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/ids.hpp"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Exception.h>
#include <SQLiteCpp/Statement.h>

#include <array>
#include <compare>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Shared SQLite helpers of the document module (native files and the recovery database).
/// SQLiteCpp throws; every public entry point of this module runs its database work through
/// guarded() so that no exception crosses the module boundary (section 19.3).
namespace mwb::document::sqlite_support {

/// Runs `body` and converts SQLiteCpp, allocation and other standard exceptions into Result
/// errors. `body` must return mwb::Result<T> or mwb::Status.
template <class Function>
[[nodiscard]] auto guarded(std::string_view operation, Function&& body) -> decltype(body()) {
    try {
        return body();
    } catch (const SQLite::Exception& exception) {
        return make_error(ErrorCode::database, std::string(operation) + ": " + exception.what());
    } catch (const std::bad_alloc&) {
        return make_error(ErrorCode::limit_exceeded, std::string(operation) + ": out of memory");
    } catch (const std::exception& exception) {
        return make_error(ErrorCode::internal, std::string(operation) + ": " + exception.what());
    }
}

/// UTF-8 form of a path for SQLite's filename arguments. SQLite's Windows VFS converts UTF-8
/// file names to UTF-16, so Hebrew, emoji and other non-ASCII names round-trip exactly.
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path);

/// Binds a 16-byte identity as a BLOB.
void bind_uuid(SQLite::Statement& statement, int index, const Uuid& identity);

/// Binds bytes as a BLOB. An empty range binds a zero-length BLOB, never NULL.
void bind_bytes(SQLite::Statement& statement, int index, std::span<const std::uint8_t> bytes);

/// Reads a BLOB (or TEXT) column as bytes; NULL and zero-length values give an empty vector.
[[nodiscard]] std::vector<std::uint8_t> column_bytes(const SQLite::Statement& statement, int index);

/// Reads a 16-byte identity column, rejecting other lengths and NULL.
[[nodiscard]] Result<Uuid> column_uuid(const SQLite::Statement& statement, int index, std::string_view what);

/// Reads a 32-byte digest column, rejecting other lengths.
[[nodiscard]] Result<std::array<std::uint8_t, 32>> column_digest(const SQLite::Statement& statement, int index,
                                                                  std::string_view what);

/// Value of an integer PRAGMA (application_id, user_version, ...).
[[nodiscard]] std::int64_t pragma_integer(SQLite::Database& database, const std::string& pragma);

/// Runs PRAGMA integrity_check (or quick_check when `quick`) and PRAGMA foreign_key_check;
/// returns invalid_data describing the first problem.
[[nodiscard]] Status check_database_integrity(SQLite::Database& database, bool quick);

/// Hardens a connection that reads externally supplied bytes (section 22.5): bounded
/// sqlite3_limit values, defensive mode, trusted_schema off, triggers and views disabled and the
/// connection made query-only.
[[nodiscard]] Status harden_untrusted_connection(SQLite::Database& database, std::int64_t max_blob_bytes);

/// One row of sqlite_schema used for whitelist comparisons.
struct SchemaObject {
    std::string type;
    std::string name;
    std::string table_name;
    std::string sql; ///< Empty for automatic indexes.
    friend auto operator<=>(const SchemaObject&, const SchemaObject&) = default;
};

/// All schema objects of the main database, sorted.
[[nodiscard]] std::vector<SchemaObject> read_schema_objects(SQLite::Database& database);

/// Schema objects produced by executing `ddl` in a fresh in-memory database; the reference
/// against which externally supplied files are whitelisted.
[[nodiscard]] std::vector<SchemaObject> reference_schema_objects(std::string_view ddl);

/// Compares the file's schema with the reference schema. Any trigger, view, virtual table or
/// other object not in the reference, a missing object or an altered definition is rejected.
[[nodiscard]] Status verify_schema_whitelist(SQLite::Database& database, std::string_view ddl,
                                             std::string_view file_kind);

/// Copies the main database of an open connection into a byte vector (sqlite3_serialize).
[[nodiscard]] Result<std::vector<std::uint8_t>> serialize_database(SQLite::Database& database);

/// Replaces the main database of an open connection with a read-only copy of `bytes`
/// (sqlite3_deserialize).
[[nodiscard]] Status deserialize_read_only(SQLite::Database& database, std::span<const std::uint8_t> bytes);

} // namespace mwb::document::sqlite_support
