#include "mwb/app/document_io.hpp"
#include "mwb/cli/cli.hpp"
#include "mwb/cli/commands.hpp"
#include "mwb/library/bench.hpp"
#include "mwb/library/library.hpp"
#include "mwb/core/pitch.hpp"
#include "mwb/library/maintenance.hpp"

#include <sstream>

namespace mwb::cli {

namespace {

/// Opens the library at `root`; with `create_missing` a new library is created when the folder
/// has no catalog yet (import is the only command that creates one).
Result<std::unique_ptr<library::Library>> open_library(const std::string& root, bool create_missing) {
    const auto path = app::path_from_utf8(root);
    std::error_code error;
    if (create_missing && !std::filesystem::exists(path / "library.sqlite", error))
        return library::Library::create(path);
    return library::Library::open(path);
}

CommandOutput success(const std::string& command, nlohmann::json body) {
    CommandOutput output;
    output.json = {{"schema_version", 1}, {"status", "ok"}, {"command", command}};
    for (auto& [key, value] : body.items())
        output.json[key] = std::move(value);
    if (!output.json.contains("warnings"))
        output.json["warnings"] = nlohmann::json::array();
    output.json["errors"] = nlohmann::json::array();
    return output;
}

} // namespace

CommandOutput command_library_import(const CommandOptions& options) {
    auto library = open_library(options.library, true);
    if (!library)
        return failure("library import", library.error());
    auto report = (*library)->import_paths({app::path_from_utf8(options.from)});
    if (!report)
        return failure("library import", report.error());
    auto output = success("library import", {{"report", nlohmann::json::parse(report->to_json())}});
    const auto& counts = report->counts;
    // Folder import continues past individual failures; a partial batch exits 7 (section 31).
    if (counts.failed > 0 || counts.changed_during_read > 0) {
        output.exit_code = exit_partial;
        output.json["status"] = "partial";
    }
    if (counts.cancelled > 0)
        output.exit_code = exit_cancelled;
    return output;
}

CommandOutput command_library_search(const CommandOptions& options) {
    auto library = open_library(options.library, false);
    if (!library)
        return failure("library search", library.error());
    library::SearchRequest request;
    request.query_text = options.query;
    request.page_size = static_cast<std::size_t>(options.limit);
    auto page = (*library)->search(request);
    if (!page)
        return failure("library search", page.error());
    std::vector<std::int64_t> ids;
    for (const auto& row : page->rows)
        ids.push_back(row.id);
    nlohmann::json results = nlohmann::json::array();
    auto rows = (*library)->browse_rows_for(ids);
    if (!rows)
        return failure("library search", rows.error());
    for (const auto& row : *rows) {
        nlohmann::json item = {{"asset_id", row.id},
                               {"title", row.text[0].empty() ? row.sort_title : row.text[0]},
                               {"tags", row.tags},
                               {"rating", row.rating},
                               {"favorite", row.favorite}};
        if (!row.sequences.empty()) {
            const auto& facts = row.sequences.front();
            if (facts.tempo_representative)
                item["bpm"] = *facts.tempo_representative;
            item["tempo_origin"] = facts.tempo_origin;
            if (facts.key_root && facts.key_mode)
                item["key"] = {{"root", *facts.key_root}, {"mode", *facts.key_mode}, {"origin", facts.key_origin}};
            item["notes"] = facts.note_count;
        }
        results.push_back(std::move(item));
    }
    return success("library search", {{"query", options.query},
                                      {"results", std::move(results)},
                                      {"has_more", page->next.has_value()},
                                      {"truncated", page->truncated}});
}

/// library melody: musical content search (section 26). Notes are pitch names or numbers, a
/// quarter note apart unless ":ticks" gives a duration (at PPQ 960).
CommandOutput command_library_melody(const CommandOptions& options) {
    std::vector<search::QueryNote> notes;
    std::istringstream words(options.melody);
    std::string word;
    Tick onset = 0;
    while (words >> word) {
        Tick duration = 960;
        const auto colon = word.find(':');
        if (colon != std::string::npos) {
            try {
                duration = std::stoll(word.substr(colon + 1));
            } catch (const std::exception&) {
                duration = 0;
            }
            word = word.substr(0, colon);
        }
        const auto pitch = parse_pitch(word);
        if (!pitch || duration <= 0)
            return failure("library melody", Error{ErrorCode::invalid_argument, "cannot read the note " + word, {}, {}});
        notes.push_back(search::QueryNote{onset, duration, *pitch, 100});
        onset += duration;
    }
    if (notes.empty() || notes.size() > 256)
        return failure("library melody", Error{ErrorCode::invalid_argument, "give 1 to 256 notes", {}, {}});
    auto library = open_library(options.library, false);
    if (!library)
        return failure("library melody", library.error());
    nlohmann::json matches = nlohmann::json::array();
    nlohmann::json details;
    if (options.melody_mode == "similar") {
        search::ApproximateQuery query;
        query.notes = notes;
        query.result_limit = static_cast<std::size_t>(options.limit);
        auto result = (*library)->similar_search(query);
        if (!result)
            return failure("library melody", result.error());
        for (const auto& match : result->matches)
            matches.push_back({{"asset_id", match.stream.asset_id},
                               {"revision_id", match.stream.revision_id},
                               {"sequence", match.stream.sequence_no},
                               {"track", match.stream.track_no},
                               {"start_tick", match.start_tick},
                               {"end_tick", match.end_tick},
                               {"score", match.components.score},
                               {"components",
                                {{"pitch", match.components.pitch},
                                 {"rhythm", match.components.rhythm},
                                 {"duration", match.components.duration},
                                 {"velocity", match.components.velocity},
                                 {"edit", match.components.edit}}},
                               {"transpose", match.components.transpose},
                               {"time_scale", match.components.time_scale},
                               {"explanation", match.explanation}});
        details = {{"label", result->label},
                   {"partial", result->partial},
                   {"candidate_budget_reached", result->candidate_budget_reached}};
    } else {
        search::ExactQuery query;
        query.notes = notes;
        query.result_limit = static_cast<std::size_t>(options.limit);
        if (options.melody_mode == "absolute")
            query.mode = search::MatchMode::absolute;
        else if (options.melody_mode == "contour")
            query.mode = search::MatchMode::contour;
        else if (options.melody_mode == "rhythm")
            query.mode = search::MatchMode::rhythm;
        else if (options.melody_mode != "transposed")
            return failure("library melody", Error{ErrorCode::invalid_argument,
                                                   "mode must be absolute, transposed, contour, rhythm or similar", {}, {}});
        auto result = (*library)->motif_search(query);
        if (!result)
            return failure("library melody", result.error());
        for (const auto& occurrence : result->occurrences)
            matches.push_back({{"asset_id", occurrence.stream.asset_id},
                               {"revision_id", occurrence.stream.revision_id},
                               {"sequence", occurrence.stream.sequence_no},
                               {"track", occurrence.stream.track_no},
                               {"start_tick", occurrence.start_tick},
                               {"end_tick", occurrence.end_tick},
                               {"transpose", occurrence.transpose},
                               {"note_ordinals", occurrence.ordinals}});
        details = {{"label", "exact search"},
                   {"partial", result->partial},
                   {"partial_reason", result->partial_reason},
                   {"indexed", result->indexed}};
    }
    return success("library melody", {{"mode", options.melody_mode}, {"matches", std::move(matches)}, {"search", details}});
}

CommandOutput command_library_analyze(const CommandOptions& options) {
    auto library = open_library(options.library, false);
    if (!library)
        return failure("library analyze", library.error());
    library::AnalyzeOptions analyze;
    if (options.profile == "fingerprints") {
        analyze.motif = false;
        analyze.features = false;
    } else if (options.profile == "motif") {
        analyze.fingerprints = false;
        analyze.features = false;
    } else if (!options.profile.empty() && options.profile != "all") {
        return failure("library analyze",
                       Error{ErrorCode::invalid_argument, "profile must be all, fingerprints or motif", {}, {}});
    }
    auto report = (*library)->analyze(analyze);
    if (!report)
        return failure("library analyze", report.error());
    auto output = success("library analyze", {{"report",
                                               {{"analyzed", report->revisions_analyzed},
                                                {"skipped", report->revisions_skipped},
                                                {"failed", report->revisions_failed},
                                                {"streams", report->streams},
                                                {"grams", report->grams},
                                                {"cancelled", report->cancelled},
                                                {"budget_reached", report->budget_reached}}}});
    // A full disk budget leaves heads unindexed, which is a partial result like a failed file.
    if (report->revisions_failed > 0 || report->budget_reached) {
        output.exit_code = exit_partial;
        output.json["status"] = "partial";
    }
    return output;
}

CommandOutput command_library_backup(const CommandOptions& options) {
    auto library = open_library(options.library, false);
    if (!library)
        return failure("library backup", library.error());
    auto report = library::create_backup(**library, app::path_from_utf8(options.output));
    if (!report)
        return failure("library backup", report.error());
    return success("library backup", {{"report",
                                       {{"directory", app::path_to_utf8(report->directory)},
                                        {"objects", report->object_count},
                                        {"bytes", report->byte_count},
                                        {"complete", report->complete}}}});
}

CommandOutput command_library_verify(const CommandOptions& options) {
    library::VerifyOptions verify;
    verify.objects = options.verify_objects;
    auto report = library::verify_library(app::path_from_utf8(options.library), verify);
    if (!report)
        return failure("library verify", report.error());
    auto output = success("library verify", {{"report", nlohmann::json::parse(report->to_json())}});
    if (!report->ok()) {
        output.exit_code = exit_invalid_data;
        output.json["status"] = "invalid";
    }
    return output;
}

CommandOutput command_library_repair(const CommandOptions& options) {
    if (!options.rebuild_fts && !options.rebuild_derived)
        return failure("library repair",
                       Error{ErrorCode::invalid_argument, "choose a repair: --rebuild-fts and/or --rebuild-derived", {}, {}});
    auto library = open_library(options.library, false);
    if (!library)
        return failure("library repair", library.error());
    library::RepairOptions repair;
    repair.rebuild_fts = options.rebuild_fts;
    repair.rebuild_derived = options.rebuild_derived;
    auto report = library::repair_library(**library, repair);
    if (!report)
        return failure("library repair", report.error());
    return success("library repair", {{"report",
                                       {{"safety_backup", app::path_to_utf8(report->safety_backup)},
                                        {"full_backup", report->full_backup},
                                        {"fts_rows", report->fts_rows},
                                        {"derived_rows_removed", report->derived_rows_removed}}}});
}

CommandOutput command_library_gc(const CommandOptions& options) {
    auto library = open_library(options.library, false);
    if (!library)
        return failure("library gc", library.error());
    library::GarbageCollectionOptions gc;
    gc.grace_period_ms = static_cast<std::int64_t>(options.grace_days * 24.0 * 60.0 * 60.0 * 1000.0);
    auto report = (*library)->collect_garbage(gc);
    if (!report)
        return failure("library gc", report.error());
    auto output = success("library gc", {{"report",
                                          {{"unreferenced", report->unreferenced},
                                           {"deleted", report->deleted},
                                           {"orphan_files", report->orphan_files},
                                           {"waiting", report->waiting},
                                           {"staging_removed", report->staging_removed},
                                           {"suspended", report->suspended}}}});
    if (report->suspended) { // a running backup holds the objects; nothing was removed
        output.exit_code = exit_partial;
        output.json["status"] = "suspended";
    }
    return output;
}

CommandOutput command_bench(const CommandOptions& options) {
    library::BenchOptions bench;
    bench.rows = static_cast<std::size_t>(options.rows);
    bench.motif_rows = static_cast<std::size_t>(options.motif_rows);
    bench.repetitions = static_cast<std::size_t>(options.repetitions);
    auto report = library::run_bench(app::path_from_utf8(options.library), bench);
    if (!report)
        return failure("bench", report.error());
    auto output = success("bench", {{"report", nlohmann::json::parse(report->to_json())}});
    if (!report->plan_problems.empty()) {
        output.exit_code = exit_invalid_data;
        output.json["status"] = "plan_problems";
    }
    return output;
}

} // namespace mwb::cli
