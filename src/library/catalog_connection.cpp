// Catalog connection policy, schema creation and SQLite C API adapters (sections 22.5, 23.2, 30.1).

#include "mwb/library/catalog_connection.hpp"

#include "mwb/document/sqlite_support.hpp"
#include "mwb/library/catalog_schema.hpp"

#include <SQLiteCpp/Transaction.h>
#include <sqlite3.h>

#include <format>

namespace mwb::library {

namespace sqlite_support = document::sqlite_support;

namespace {

Status configure(SQLite::Database& database, ConnectionRole role, int busy_timeout_ms) {
    database.setBusyTimeout(busy_timeout_ms);
    int enabled = 0;
    if (sqlite3_db_config(database.getHandle(), SQLITE_DBCONFIG_DEFENSIVE, 1, &enabled) != SQLITE_OK || enabled != 1)
        return make_error(ErrorCode::database, "cannot enable SQLite defensive mode");
    if (sqlite3_db_config(database.getHandle(), SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, &enabled) != SQLITE_OK ||
        enabled != 0)
        return make_error(ErrorCode::database, "cannot disable trusted_schema");
    database.exec("PRAGMA foreign_keys = ON");
    database.exec("PRAGMA synchronous = FULL");
    database.exec("PRAGMA trusted_schema = OFF");
    if (role == ConnectionRole::writer) {
        SQLite::Statement mode(database, "PRAGMA journal_mode = WAL");
        if (!mode.executeStep() || mode.getColumn(0).getString() != "wal")
            return make_error(ErrorCode::database, "the catalog could not switch to WAL mode (local filesystem required)");
    } else {
        database.exec("PRAGMA query_only = ON");
    }
    return ok();
}

int interrupt_when_stopped(void* context) {
    const auto* stop = static_cast<const std::stop_token*>(context);
    return stop->stop_requested() ? 1 : 0;
}

} // namespace

Result<std::unique_ptr<SQLite::Database>> open_catalog_connection(const std::filesystem::path& database_path,
                                                                  ConnectionRole role, int busy_timeout_ms,
                                                                  bool create) {
    return sqlite_support::guarded("open catalog", [&]() -> Result<std::unique_ptr<SQLite::Database>> {
        int flags = role == ConnectionRole::writer ? SQLite::OPEN_READWRITE : SQLite::OPEN_READONLY;
        if (create && role == ConnectionRole::writer)
            flags |= SQLite::OPEN_CREATE;
        flags |= SQLite::OPEN_NOMUTEX;
        auto database = std::make_unique<SQLite::Database>(sqlite_support::path_to_utf8(database_path), flags,
                                                           busy_timeout_ms);
        MWB_TRY(configure(*database, role, busy_timeout_ms));
        return database;
    });
}

Status create_catalog_schema(SQLite::Database& database, const Uuid& library_uuid, std::int64_t created_ms) {
    return sqlite_support::guarded("create catalog schema", [&]() -> Status {
        SQLite::Transaction transaction(database, SQLite::TransactionBehavior::IMMEDIATE);
        database.exec(std::string(catalog_ddl_v1));
        database.exec(std::string(catalog_tuning_indexes_v1));
        SQLite::Statement insert(database, "INSERT INTO catalog(singleton, uuid, created_ms, generation) VALUES (1, ?1, ?2, 0)");
        sqlite_support::bind_uuid(insert, 1, library_uuid);
        insert.bind(2, created_ms);
        insert.exec();
        SQLite::Statement setting(database, "INSERT INTO catalog_setting(key, schema_version, value_json) VALUES "
                                            "('migration.history', 1, ?1)");
        setting.bind(1, std::format("[{{\"version\":1,\"applied_ms\":{}}}]", created_ms));
        setting.exec();
        transaction.commit();
        return ok();
    });
}

Status check_catalog_header(SQLite::Database& database) {
    return sqlite_support::guarded("check catalog header", [&]() -> Status {
        const auto application = sqlite_support::pragma_integer(database, "application_id");
        if (application != catalog_application_id)
            return make_error(ErrorCode::invalid_data, "the file is not a MIDI Workbench library catalog");
        const auto version = sqlite_support::pragma_integer(database, "user_version");
        if (version > catalog_schema_version)
            return make_error(ErrorCode::unsupported,
                              std::format("the catalog uses schema version {}, newer than this application ({}); "
                                          "write access is refused",
                                          version, catalog_schema_version));
        if (version < 1)
            return make_error(ErrorCode::invalid_data, "the catalog has no schema version");
        return ok();
    });
}

Result<Uuid> read_catalog_uuid(SQLite::Database& database) {
    return sqlite_support::guarded("read catalog identity", [&]() -> Result<Uuid> {
        SQLite::Statement query(database, "SELECT uuid FROM catalog WHERE singleton = 1");
        if (!query.executeStep())
            return make_error(ErrorCode::invalid_data, "the catalog identity row is missing");
        return sqlite_support::column_uuid(query, 0, "catalog uuid");
    });
}

void bind_parameters(SQLite::Statement& statement, const std::vector<search::SqlValue>& parameters) {
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        const auto position = static_cast<int>(index + 1);
        std::visit([&](const auto& value) { statement.bind(position, value); }, parameters[index]);
    }
}

ProgressInterrupt::ProgressInterrupt(SQLite::Database& database, std::stop_token stop)
    : database_(database), stop_(std::move(stop)) {
    sqlite3_progress_handler(database_.getHandle(), 1000, &interrupt_when_stopped, &stop_);
}

ProgressInterrupt::~ProgressInterrupt() { sqlite3_progress_handler(database_.getHandle(), 0, nullptr, nullptr); }

Result<std::vector<std::string>> query_plan(SQLite::Database& database, const search::SqlFragment& statement) {
    return sqlite_support::guarded("explain query plan", [&]() -> Result<std::vector<std::string>> {
        SQLite::Statement explain(database, "EXPLAIN QUERY PLAN " + statement.sql);
        bind_parameters(explain, statement.parameters);
        std::vector<std::string> lines;
        while (explain.executeStep())
            lines.push_back(explain.getColumn(3).getString());
        return lines;
    });
}

Status backup_database(SQLite::Database& source, const std::filesystem::path& destination) {
    return sqlite_support::guarded("backup catalog", [&]() -> Status {
        SQLite::Database target(sqlite_support::path_to_utf8(destination),
                                SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        sqlite3_backup* backup = sqlite3_backup_init(target.getHandle(), "main", source.getHandle(), "main");
        if (backup == nullptr)
            return make_error(ErrorCode::database, std::string("backup init failed: ") + sqlite3_errmsg(target.getHandle()));
        // One step copies every page inside a single read transaction, so the copy is a consistent
        // snapshot. BUSY/LOCKED are retried a bounded number of times with a short backoff.
        int step = sqlite3_backup_step(backup, -1);
        for (int attempt = 0; attempt < 200 && (step == SQLITE_BUSY || step == SQLITE_LOCKED); ++attempt) {
            sqlite3_sleep(25);
            step = sqlite3_backup_step(backup, -1);
        }
        const int finished = sqlite3_backup_finish(backup);
        if (step != SQLITE_DONE || finished != SQLITE_OK)
            return make_error(ErrorCode::database, std::string("backup failed: ") + sqlite3_errmsg(target.getHandle()));
        target.exec("PRAGMA journal_mode = DELETE");
        return ok();
    });
}

} // namespace mwb::library
