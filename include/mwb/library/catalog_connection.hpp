#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/ids.hpp"
#include "mwb/search/sql.hpp"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

/// Catalog connections with the policy of specification section 22.5: WAL, foreign_keys=ON,
/// synchronous=FULL, a finite busy timeout, defensive mode and trusted_schema=OFF on every
/// connection; read connections are additionally query-only.
namespace mwb::library {

/// Role of a connection.
enum class ConnectionRole : std::uint8_t { writer, reader };

/// Opens and configures a catalog connection. A writer may create the file.
[[nodiscard]] Result<std::unique_ptr<SQLite::Database>> open_catalog_connection(const std::filesystem::path& database,
                                                                                ConnectionRole role,
                                                                                int busy_timeout_ms, bool create = false);

/// Creates the section 23.2 schema plus the tuning indexes and the catalog identity row.
[[nodiscard]] Status create_catalog_schema(SQLite::Database& database, const Uuid& library_uuid, std::int64_t created_ms);

/// Checks application_id and user_version; a newer schema is refused (section 23.4).
[[nodiscard]] Status check_catalog_header(SQLite::Database& database);

/// Library identity stored in the catalog row.
[[nodiscard]] Result<Uuid> read_catalog_uuid(SQLite::Database& database);

/// Binds search parameters (?1..?N) to a statement.
void bind_parameters(SQLite::Statement& statement, const std::vector<search::SqlValue>& parameters);

/// Installs a progress handler that interrupts statements once `stop` is requested; removed by
/// the returned guard's destructor (section 20.4).
class ProgressInterrupt {
public:
    ProgressInterrupt(SQLite::Database& database, std::stop_token stop);
    ~ProgressInterrupt();
    ProgressInterrupt(const ProgressInterrupt&) = delete;
    ProgressInterrupt& operator=(const ProgressInterrupt&) = delete;

private:
    SQLite::Database& database_;
    std::stop_token stop_;
};

/// EXPLAIN QUERY PLAN detail lines of `statement`.
[[nodiscard]] Result<std::vector<std::string>> query_plan(SQLite::Database& database, const search::SqlFragment& statement);

/// Online consistent copy of `source` into a new database file through the SQLite backup API; the
/// copy is switched to rollback-journal mode so it does not depend on WAL sidecars.
[[nodiscard]] Status backup_database(SQLite::Database& source, const std::filesystem::path& destination);

} // namespace mwb::library
