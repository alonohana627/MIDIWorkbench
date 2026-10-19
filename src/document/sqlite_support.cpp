#include "mwb/document/sqlite_support.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <stdexcept>
#include <tuple>

namespace mwb::document::sqlite_support {

namespace {

/// Address used when binding an empty BLOB: sqlite3_bind_blob binds NULL for a null pointer,
/// which would violate NOT NULL payload columns.
constexpr std::uint8_t empty_blob_marker = 0;

} // namespace

std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

void bind_uuid(SQLite::Statement& statement, int index, const Uuid& identity) {
    statement.bind(index, identity.bytes().data(), static_cast<int>(identity.bytes().size()));
}

void bind_bytes(SQLite::Statement& statement, int index, std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) {
        statement.bind(index, &empty_blob_marker, 0);
        return;
    }
    if (bytes.size() > static_cast<std::size_t>(INT_MAX))
        throw std::length_error("BLOB larger than SQLite can bind");
    statement.bind(index, bytes.data(), static_cast<int>(bytes.size()));
}

void bind_bytes_no_copy(SQLite::Statement& statement, int index, std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) {
        statement.bindNoCopy(index, &empty_blob_marker, 0);
        return;
    }
    if (bytes.size() > static_cast<std::size_t>(INT_MAX))
        throw std::length_error("BLOB larger than SQLite can bind");
    statement.bindNoCopy(index, bytes.data(), static_cast<int>(bytes.size()));
}

std::vector<std::uint8_t> column_bytes(const SQLite::Statement& statement, int index) {
    const SQLite::Column column = statement.getColumn(index);
    if (column.isNull())
        return {};
    // getBlob() converts TEXT to BLOB without copying; getBytes() must follow it (SQLite rule).
    const auto* data = static_cast<const std::uint8_t*>(column.getBlob());
    const int size = column.getBytes();
    if (data == nullptr || size <= 0)
        return {};
    return std::vector<std::uint8_t>(data, data + size);
}

Result<Uuid> column_uuid(const SQLite::Statement& statement, int index, std::string_view what) {
    const SQLite::Column column = statement.getColumn(index);
    if (!column.isBlob())
        return make_error(ErrorCode::invalid_data, std::string(what) + " is not a 16-byte identity");
    const auto bytes = column_bytes(statement, index);
    auto identity = Uuid::from_bytes(bytes);
    if (!identity)
        return make_error(ErrorCode::invalid_data, std::string(what) + " is not a 16-byte identity");
    return *identity;
}

Result<std::array<std::uint8_t, 32>> column_digest(const SQLite::Statement& statement, int index,
                                                   std::string_view what) {
    const auto bytes = column_bytes(statement, index);
    if (bytes.size() != 32)
        return make_error(ErrorCode::invalid_data, std::string(what) + " is not a 32-byte digest");
    std::array<std::uint8_t, 32> digest{};
    std::copy(bytes.begin(), bytes.end(), digest.begin());
    return digest;
}

std::int64_t pragma_integer(SQLite::Database& database, const std::string& pragma) {
    SQLite::Statement statement(database, "PRAGMA " + pragma);
    if (!statement.executeStep())
        throw SQLite::Exception("PRAGMA " + pragma + " returned no row");
    return statement.getColumn(0).getInt64();
}

Status check_database_integrity(SQLite::Database& database, bool quick) {
    {
        SQLite::Statement statement(database, quick ? "PRAGMA quick_check" : "PRAGMA integrity_check");
        if (!statement.executeStep())
            return make_error(ErrorCode::invalid_data, "integrity check returned no result");
        const std::string first = statement.getColumn(0).getString();
        if (first != "ok")
            return make_error(ErrorCode::invalid_data, "database integrity check failed: " + first);
    }
    {
        SQLite::Statement statement(database, "PRAGMA foreign_key_check");
        if (statement.executeStep())
            return make_error(ErrorCode::invalid_data,
                              "foreign key check failed in table " + statement.getColumn(0).getString());
    }
    return ok();
}

Status harden_untrusted_connection(SQLite::Database& database, std::int64_t max_blob_bytes) {
    sqlite3* handle = database.getHandle();
    const auto limit_value = static_cast<int>(std::clamp<std::int64_t>(max_blob_bytes, 1, 1'000'000'000));
    // sqlite3_limit returns the previous value; the new values are what matter here.
    std::ignore = sqlite3_limit(handle, SQLITE_LIMIT_LENGTH, limit_value);
    std::ignore = sqlite3_limit(handle, SQLITE_LIMIT_SQL_LENGTH, 100'000);
    std::ignore = sqlite3_limit(handle, SQLITE_LIMIT_COLUMN, 64);
    std::ignore = sqlite3_limit(handle, SQLITE_LIMIT_EXPR_DEPTH, 64);
    std::ignore = sqlite3_limit(handle, SQLITE_LIMIT_COMPOUND_SELECT, 4);
    std::ignore = sqlite3_limit(handle, SQLITE_LIMIT_VDBE_OP, 250'000);
    std::ignore = sqlite3_limit(handle, SQLITE_LIMIT_FUNCTION_ARG, 8);
    std::ignore = sqlite3_limit(handle, SQLITE_LIMIT_ATTACHED, 0);
    std::ignore = sqlite3_limit(handle, SQLITE_LIMIT_LIKE_PATTERN_LENGTH, 64);
    std::ignore = sqlite3_limit(handle, SQLITE_LIMIT_VARIABLE_NUMBER, 32);
    std::ignore = sqlite3_limit(handle, SQLITE_LIMIT_TRIGGER_DEPTH, 1);
    std::ignore = sqlite3_limit(handle, SQLITE_LIMIT_WORKER_THREADS, 0);

    const auto configure = [&](int option, int value, const char* name) -> Status {
        int applied = -1;
        if (sqlite3_db_config(handle, option, value, &applied) != SQLITE_OK || applied != value)
            return make_error(ErrorCode::database, std::string("cannot configure SQLite option ") + name);
        return ok();
    };
    MWB_TRY(configure(SQLITE_DBCONFIG_DEFENSIVE, 1, "defensive"));
    MWB_TRY(configure(SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, "trusted_schema"));
    MWB_TRY(configure(SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, "enable_trigger"));
    MWB_TRY(configure(SQLITE_DBCONFIG_ENABLE_VIEW, 0, "enable_view"));
    database.exec("PRAGMA trusted_schema = OFF");
    // SQLite's guidance for untrusted database files: verify cell sizes while reading b-tree
    // pages (catches crafted pages earlier) and never memory-map the file.
    database.exec("PRAGMA cell_size_check = ON");
    database.exec("PRAGMA mmap_size = 0");
    database.exec("PRAGMA query_only = ON");
    return ok();
}

std::vector<SchemaObject> read_schema_objects(SQLite::Database& database) {
    std::vector<SchemaObject> objects;
    SQLite::Statement statement(database,
                                "SELECT type, name, tbl_name, coalesce(sql, '') FROM sqlite_schema ORDER BY type, name");
    while (statement.executeStep()) {
        objects.push_back(SchemaObject{statement.getColumn(0).getString(), statement.getColumn(1).getString(),
                                       statement.getColumn(2).getString(), statement.getColumn(3).getString()});
    }
    std::sort(objects.begin(), objects.end());
    return objects;
}

std::vector<SchemaObject> reference_schema_objects(std::string_view ddl) {
    SQLite::Database reference(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    reference.exec(std::string(ddl));
    return read_schema_objects(reference);
}

Status verify_schema_whitelist(SQLite::Database& database, std::string_view ddl, std::string_view file_kind) {
    const auto expected = reference_schema_objects(ddl);
    const auto actual = read_schema_objects(database);
    for (const auto& object : actual) {
        if (!std::binary_search(expected.begin(), expected.end(), object)) {
            const bool same_name = std::any_of(expected.begin(), expected.end(), [&](const SchemaObject& candidate) {
                return candidate.type == object.type && candidate.name == object.name;
            });
            return make_error(ErrorCode::invalid_data,
                              std::string(file_kind) + (same_name ? " has an altered schema object: "
                                                                  : " contains an unexpected schema object: ") +
                                  object.type + " " + object.name);
        }
    }
    for (const auto& object : expected) {
        if (!std::binary_search(actual.begin(), actual.end(), object))
            return make_error(ErrorCode::invalid_data,
                              std::string(file_kind) + " is missing schema object " + object.type + " " + object.name);
    }
    return ok();
}

Result<std::vector<std::uint8_t>> serialize_database(SQLite::Database& database) {
    sqlite3_int64 size = 0;
    unsigned char* data = sqlite3_serialize(database.getHandle(), "main", &size, 0);
    if (data == nullptr)
        return make_error(ErrorCode::database, "cannot serialize the database");
    std::vector<std::uint8_t> bytes;
    try {
        bytes.assign(data, data + size);
    } catch (...) {
        sqlite3_free(data);
        throw;
    }
    sqlite3_free(data);
    return bytes;
}

Status deserialize_read_only(SQLite::Database& database, std::span<const std::uint8_t> bytes) {
    if (bytes.empty())
        return make_error(ErrorCode::invalid_data, "empty database image");
    auto* buffer = static_cast<unsigned char*>(sqlite3_malloc64(bytes.size()));
    if (buffer == nullptr)
        return make_error(ErrorCode::limit_exceeded, "cannot allocate the database image");
    std::memcpy(buffer, bytes.data(), bytes.size());
    const auto size = static_cast<sqlite3_int64>(bytes.size());
    // With FREEONCLOSE SQLite owns the buffer from here on, also when the call fails.
    const int result = sqlite3_deserialize(database.getHandle(), "main", buffer, size, size,
                                           SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_READONLY);
    if (result != SQLITE_OK)
        return make_error(ErrorCode::database, std::string("cannot open the database image: ") + sqlite3_errstr(result));
    return ok();
}

} // namespace mwb::document::sqlite_support
