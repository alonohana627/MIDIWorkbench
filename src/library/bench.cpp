// Hot-query plan policy and the synthetic catalog benchmark (sections 22.5 and 33).

#include "mwb/library/bench.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>

namespace mwb::library {

namespace {

Result<HotQuery> compiled(Library& library, std::string name, const SearchRequest& request) {
    auto search = library.compile_search(request);
    if (!search)
        return std::unexpected(std::move(search).error());
    return HotQuery{std::move(name), std::move(search->statement)};
}

double percentile(std::vector<double> samples, double fraction) {
    if (samples.empty())
        return 0.0;
    std::ranges::sort(samples);
    const auto rank = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(samples.size())));
    return samples[std::clamp<std::size_t>(rank, 1, samples.size()) - 1];
}

} // namespace

Result<std::vector<HotQuery>> hot_queries(Library& library) {
    std::vector<HotQuery> queries;
    SearchRequest paging;
    paging.after = search::PageCursor{std::string("m"), 0};
    SearchRequest tag;
    tag.query_text = "tag:bass";
    SearchRequest fts_bpm;
    fts_bpm.query_text = "lead bpm:120..140";
    SearchRequest fts_bpm_relevance = fts_bpm;
    fts_bpm_relevance.order.field = search::SortField::relevance;
    for (auto [name, request] : {std::pair{"paging_title", paging}, std::pair{"tag_filter", tag},
                                 std::pair{"fts_bpm", fts_bpm}, std::pair{"fts_bpm_relevance", fts_bpm_relevance}}) {
        auto query = compiled(library, name, request);
        if (!query)
            return std::unexpected(std::move(query).error());
        queries.push_back(std::move(*query));
    }
    queries.push_back(HotQuery{"job_leasing", search::SqlFragment{std::string(lease_job_sql), {now_utc_ms()}}});
    return queries;
}

std::vector<std::string> plan_problems(const std::vector<std::string>& plan_lines) {
    std::vector<std::string> problems;
    for (const auto& line : plan_lines) {
        if (line.find("USE TEMP B-TREE") != std::string::npos) {
            problems.push_back(line);
            continue;
        }
        if (line.starts_with("SCAN ") && line.find("VIRTUAL TABLE") == std::string::npos &&
            !line.starts_with("SCAN CONSTANT ROW"))
            problems.push_back(line);
    }
    return problems;
}

std::string BenchReport::to_json() const {
    nlohmann::json object{{"version", 1}, {"rows", rows}, {"populate_seconds", populate_seconds}};
    nlohmann::json list = nlohmann::json::array();
    for (const auto& timing : timings)
        list.push_back({{"name", timing.name},
                        {"samples", timing.samples},
                        {"p50_ms", timing.p50_ms},
                        {"p95_ms", timing.p95_ms},
                        {"p99_ms", timing.p99_ms},
                        {"max_ms", timing.max_ms}});
    object["timings"] = std::move(list);
    object["plan_problems"] = plan_problems;
    return object.dump();
}

Result<BenchReport> run_bench(const std::filesystem::path& directory, const BenchOptions& options, std::stop_token stop) {
    auto created = Library::create(directory);
    if (!created)
        return std::unexpected(std::move(created).error());
    auto& library = **created;
    BenchReport report;
    const auto populate_start = std::chrono::steady_clock::now();
    auto rows = library.populate_synthetic(options.rows, options.seed, stop);
    if (!rows)
        return std::unexpected(std::move(rows).error());
    report.rows = *rows;
    report.populate_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - populate_start).count();
    for (std::size_t index = 0; index < options.repetitions * 2; ++index)
        MWB_TRY(library.enqueue_job("bench", std::format("bench-{}", index), "{}"));

    auto queries = hot_queries(library);
    if (!queries)
        return std::unexpected(std::move(queries).error());
    for (const auto& query : *queries) {
        auto plan = library.explain_query_plan(query.statement);
        if (!plan)
            return std::unexpected(std::move(plan).error());
        for (const auto& problem : plan_problems(*plan))
            report.plan_problems.push_back(query.name + ": " + problem);
    }

    SearchRequest first_page;
    // A deep keyset cursor: synthetic titles start with lowercase words, so "m" lands mid-catalog.
    SearchRequest deep;
    deep.after = search::PageCursor{std::string("m"), 0};
    SearchRequest tag;
    tag.query_text = "tag:bass";
    SearchRequest fts_bpm;
    fts_bpm.query_text = "lead bpm:120..140";
    SearchRequest relevance = fts_bpm;
    relevance.order.field = search::SortField::relevance;

    const auto measure = [&](std::string name, const std::function<Status()>& operation) -> Status {
        std::vector<double> samples;
        for (std::size_t repetition = 0; repetition < options.repetitions; ++repetition) {
            if (stop.stop_requested())
                return make_error(ErrorCode::cancelled, "benchmark cancelled");
            const auto start = std::chrono::steady_clock::now();
            MWB_TRY(operation());
            samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        BenchTiming timing;
        timing.name = std::move(name);
        timing.samples = samples.size();
        timing.p50_ms = percentile(samples, 0.50);
        timing.p95_ms = percentile(samples, 0.95);
        timing.p99_ms = percentile(samples, 0.99);
        timing.max_ms = percentile(samples, 1.0);
        report.timings.push_back(std::move(timing));
        return ok();
    };
    const auto search_operation = [&](const SearchRequest& request) {
        return [&library, &stop, request]() -> Status {
            auto page = library.search(request, stop);
            if (!page)
                return std::unexpected(std::move(page).error());
            return ok();
        };
    };
    MWB_TRY(measure("page_first_title", search_operation(first_page)));
    MWB_TRY(measure("page_deep_title", search_operation(deep)));
    MWB_TRY(measure("tag_filter", search_operation(tag)));
    MWB_TRY(measure("fts_bpm_title", search_operation(fts_bpm)));
    MWB_TRY(measure("fts_bpm_relevance", search_operation(relevance)));
    MWB_TRY(measure("changes_since", [&]() -> Status {
        auto changes = library.changes_since(0);
        if (!changes)
            return std::unexpected(std::move(changes).error());
        return ok();
    }));
    MWB_TRY(measure("job_lease", [&]() -> Status {
        auto job = library.lease_job();
        if (!job)
            return std::unexpected(std::move(job).error());
        if (*job)
            MWB_TRY(library.finish_job((*job)->id, true));
        return ok();
    }));
    return report;
}

} // namespace mwb::library
