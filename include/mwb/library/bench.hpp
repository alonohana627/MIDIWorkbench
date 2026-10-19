#pragma once

#include "mwb/core/error.hpp"
#include "mwb/library/library.hpp"
#include "mwb/search/sql.hpp"

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

/// Hot catalog queries, their EXPLAIN QUERY PLAN policy (section 22.5) and the synthetic-catalog
/// benchmark behind `mwb-cli bench` (section 33).
namespace mwb::library {

/// A named hot statement.
struct HotQuery {
    std::string name;
    search::SqlFragment statement;
};

/// The hot queries the plan tests and the benchmark measure: keyset paging by (sort_title, id),
/// tag filter, FTS plus bpm, and job leasing.
[[nodiscard]] Result<std::vector<HotQuery>> hot_queries(Library& library);

/// Plan lines that violate the policy: full table or index scans (other than FTS/json_each
/// virtual tables and constant one-row subqueries) and temporary B-tree sorts.
[[nodiscard]] std::vector<std::string> plan_problems(const std::vector<std::string>& plan_lines);

/// Benchmark settings.
struct BenchOptions {
    std::size_t rows = 10'000;
    std::size_t repetitions = 25;
    std::uint64_t seed = 1;
    /// Assets that also get an indexed melody stream (M10K at 10,000): adds exact and approximate
    /// motif search timings; 0 skips them.
    std::size_t motif_rows = 0;
};

/// Latency percentiles of one measured operation.
struct BenchTiming {
    std::string name;
    std::size_t samples = 0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
};

/// Benchmark result (`bench --json`).
struct BenchReport {
    std::size_t rows = 0;
    double populate_seconds = 0.0;
    std::vector<BenchTiming> timings;
    std::vector<std::string> plan_problems; ///< "query: plan line" for every violation.
    /// In-memory browse index after its build: rows loaded, measured bytes, and whether it passed
    /// its budget (then the rows are those loaded before the build stopped).
    std::size_t browse_index_rows = 0;
    std::size_t browse_index_bytes = 0;
    bool browse_index_over_budget = false;
    [[nodiscard]] std::string to_json() const;
};

/// Creates a library in `directory` (must be empty or missing), generates `rows` synthetic assets
/// and times first/deep keyset pages, tag filter, FTS + bpm (title and relevance order), change
/// polling and job leasing. Timings are measured, never estimated.
[[nodiscard]] Result<BenchReport> run_bench(const std::filesystem::path& directory, const BenchOptions& options = {},
                                            std::stop_token stop = {});

} // namespace mwb::library
