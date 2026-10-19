#include "test_support.hpp"

#include "mwb/app/document_io.hpp"
#include "mwb/cli/cli.hpp"
#include "mwb/core/change_set.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/midi/codec.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

using namespace mwb;

namespace {

#ifdef MWB_WORKER_PATH
/// The CLI looks for mwb-worker next to its executable; the test executable lives elsewhere, so
/// the variable points it at the built worker before any command runs (static initialization).
[[maybe_unused]] const bool worker_configured = [] {
#ifdef _WIN32
    return _putenv_s("MWB_WORKER", MWB_WORKER_PATH) == 0;
#else
    return setenv("MWB_WORKER", MWB_WORKER_PATH, 1) == 0;
#endif
}();
#endif

struct Run {
    int code = 0;
    nlohmann::json json;
    std::string error;
};

Run run(std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), "mwb-cli");
    std::string output;
    std::string error;
    Run result;
    result.code = cli::run(arguments, output, error);
    result.json = nlohmann::json::parse(output, nullptr, false);
    result.error = error;
    return result;
}

std::string path_text(const std::filesystem::path& path) { return app::path_to_utf8(path); }

std::filesystem::path write_phrase(const std::filesystem::path& directory, const std::string& name) {
    auto document = make_blank_document();
    ChangeBuilder builder(document, "fixture");
    const TrackId track = document.sequences[0].tracks[0].id;
    const int pitches[] = {60, 62, 64, 65, 67, 69, 71, 72};
    for (int index = 0; index < 8; ++index)
        builder.add_note(track, index * 480 + 7, index * 480 + 400, 0, static_cast<std::uint8_t>(pitches[index]), 100);
    REQUIRE(apply_change_set(document, builder.finish()));
    auto bytes = midi::export_midi(document, {});
    REQUIRE(bytes);
    const auto path = directory / app::path_from_utf8(name);
    REQUIRE(app::write_file_atomically(path, *bytes, true));
    return path;
}

} // namespace

TEST_SUITE("cli") {
    TEST_CASE("invalid arguments exit 1 and missing files exit 3") {
        CHECK(run({"inspect"}).code == cli::exit_invalid_arguments);
        CHECK(run({"bogus"}).code == cli::exit_invalid_arguments);
        const auto missing = run({"inspect", "no-such-file.mid"});
        CHECK(missing.code == cli::exit_filesystem);
        CHECK(missing.json["status"] == "error");
        CHECK(missing.json["errors"][0]["code"] == "not_found");
    }

    TEST_CASE("inspect reports the versioned summary shape") {
        test::TemporaryDirectory directory;
        const auto file = write_phrase(directory.path(), "phrase.mid");
        const auto result = run({"inspect", path_text(file), "--json"});
        REQUIRE(result.code == cli::exit_success);
        CHECK(result.json["schema_version"] == 1);
        CHECK(result.json["command"] == "inspect");
        CHECK(result.json["summary"]["container"] == "smf");
        CHECK(result.json["summary"]["note_count"] == 8);
        CHECK(result.json["summary"]["division"]["ppq"] == 960);
        CHECK(result.json["sequences"][0]["tempo"]["initial_bpm"] == 120.0);
    }

    TEST_CASE("validate distinguishes invalid data with exit 2") {
        test::TemporaryDirectory directory;
        const auto garbage = directory.path() / "garbage.mid";
        std::ofstream(garbage, std::ios::binary) << "MThd garbage that is not a MIDI file";
        CHECK(run({"validate", path_text(garbage)}).code == cli::exit_invalid_data);
        const auto good = write_phrase(directory.path(), "good.mid");
        CHECK(run({"validate", path_text(good), "--strict"}).code == cli::exit_success);
    }

    TEST_CASE("create and convert refuse to overwrite unless asked") {
        test::TemporaryDirectory directory;
        const auto native = directory.path() / "new.mwb";
        REQUIRE(run({"create", "--output", path_text(native), "--ppq", "480", "--bpm", "90", "--meter", "6/8"}).code ==
                cli::exit_success);
        CHECK(run({"create", "--output", path_text(native)}).code == cli::exit_filesystem);
        CHECK(run({"create", "--output", path_text(native), "--ppq", "480", "--bpm", "90", "--meter", "6/8", "--overwrite"}).code ==
              cli::exit_success);
        const auto midi_file = directory.path() / "converted.mid";
        const auto converted = run({"convert", path_text(native), "--output", path_text(midi_file), "--profile", "canonical"});
        REQUIRE(converted.code == cli::exit_success);
        CHECK(converted.json["report"]["profile"] == "canonical");
        const auto inspected = run({"inspect", path_text(midi_file)});
        CHECK(inspected.json["summary"]["division"]["ppq"] == 480);
        CHECK(inspected.json["sequences"][0]["meters"][0]["meter"] == "6/8");
    }

    TEST_CASE("original-bytes conversion is byte identical") {
        test::TemporaryDirectory directory;
        const auto source = write_phrase(directory.path(), "source.mid");
        const auto copy = directory.path() / "copy.mid";
        REQUIRE(run({"convert", path_text(source), "--output", path_text(copy), "--profile", "original"}).code ==
                cli::exit_success);
        CHECK(*app::read_file_bytes(source, 1 << 20) == *app::read_file_bytes(copy, 1 << 20));
    }

    TEST_CASE("transform applies a deterministic recipe") {
        test::TemporaryDirectory directory;
        const auto source = write_phrase(directory.path(), "phrase.mid");
        const auto recipe = directory.path() / "recipe.json";
        std::ofstream(recipe) << R"({"steps": [
            {"format": "mwb.transform", "tool": "transpose", "version": 1, "parameters": {"mode": "semitones", "amount": 12}},
            {"format": "mwb.transform", "tool": "humanize", "version": 1, "parameters": {"seed": "7", "timing_range": 20},
             "prng": {"name": "pcg32", "version": 1, "seed": "7"}}]})";
        const auto first = directory.path() / "first.mid";
        const auto second = directory.path() / "second.mid";
        const auto result = run({"transform", path_text(source), "--recipe", path_text(recipe), "--output", path_text(first)});
        INFO(result.json.dump());
        REQUIRE(result.code == cli::exit_success);
        CHECK(result.json["reports"].size() == 2);
        REQUIRE(run({"transform", path_text(source), "--recipe", path_text(recipe), "--output", path_text(second)}).code ==
                cli::exit_success);
        auto a = app::open_document(first);
        auto b = app::open_document(second);
        REQUIRE(a);
        REQUIRE(b);
        const auto notes_a = pair_notes(a->document.sequences[0]).notes;
        const auto notes_b = pair_notes(b->document.sequences[0]).notes;
        REQUIRE(notes_a.size() == 8);
        for (std::size_t index = 0; index < notes_a.size(); ++index) {
            CHECK(notes_a[index].start == notes_b[index].start);
            CHECK(notes_a[index].pitch == notes_b[index].pitch);
        }
        CHECK(notes_a[0].pitch == 72);
    }

    TEST_CASE("render writes a WAV with the documented frame count") {
        test::TemporaryDirectory directory;
        const auto source = write_phrase(directory.path(), "phrase.mid");
        const auto wav = directory.path() / "phrase.wav";
        const auto result = run({"render", path_text(source), "--output", path_text(wav), "--sample-rate", "48000",
                                 "--format", "pcm16", "--tail", "1"});
        INFO(result.json.dump());
        REQUIRE(result.code == cli::exit_success);
        CHECK(result.json["summary"]["tail_frames"] == 48000);
        CHECK(std::filesystem::file_size(wav) > 44u);
        CHECK(run({"render", path_text(source), "--output", path_text(wav)}).code == cli::exit_filesystem);
    }

    TEST_CASE("render writes to names outside the ANSI code page (code-runtime-13)") {
        // This test process has no UTF-8 manifest, like any tool embedding the renderer: names
        // with Japanese and an emoji must not go through the ANSI code page.
        test::TemporaryDirectory directory;
        const auto source = write_phrase(directory.path(), "phrase.mid");
        const auto folder = directory.path() / app::path_from_utf8("\xF0\x9F\x8E\xB5 out");
        std::filesystem::create_directories(folder);
        const auto wav = folder / app::path_from_utf8("\xE6\x97\xA5\xE6\x9C\xAC\xF0\x9F\x8E\xB5.wav");
        const auto result = run({"render", path_text(source), "--output", path_text(wav), "--tail", "0"});
        INFO(result.json.dump());
        REQUIRE(result.code == cli::exit_success);
        CHECK(std::filesystem::file_size(wav) > 44u);
        std::size_t leftovers = 0;
        for (const auto& entry : std::filesystem::directory_iterator(folder))
            leftovers += entry.path() != wav ? 1u : 0u;
        CHECK(leftovers == 0); // the temporary file was renamed, not left behind
    }

    TEST_CASE("library import, search, analyze, backup, verify and repair") {
        test::TemporaryDirectory directory;
        const auto sources = directory.path() / "sources";
        std::filesystem::create_directories(sources);
        write_phrase(sources, "scale.mid");
        write_phrase(sources, "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D.mid"); // Hebrew file name, same bytes
        const auto library = directory.path() / "library";
        const auto imported = run({"library", "import", "--library", path_text(library), "--from", path_text(sources)});
        INFO(imported.json.dump());
        REQUIRE(imported.code == cli::exit_success);
        CHECK(imported.json["report"]["counts"]["discovered"] == 2);
        const auto analyzed = run({"library", "analyze", "--library", path_text(library)});
        CHECK(analyzed.code == cli::exit_success);
        const auto searched = run({"library", "search", "--library", path_text(library), "--query", "notes:>=8"});
        REQUIRE(searched.code == cli::exit_success);
        CHECK(searched.json["results"].size() >= 1);
        CHECK(run({"library", "search", "--library", path_text(library), "--query", "bpm:(("}).code ==
              cli::exit_invalid_arguments); // a malformed query is an invalid argument
        const auto melody = run({"library", "melody", "--library", path_text(library), "--notes", "D4 E4 F4 G4", "--mode",
                                 "transposed"});
        INFO(melody.json.dump());
        REQUIRE(melody.code == cli::exit_success);
        CHECK(melody.json["matches"].size() >= 1);
        const auto similar = run({"library", "melody", "--library", path_text(library), "--notes", "C4 D4 E4 F4 G4",
                                  "--mode", "similar"});
        REQUIRE(similar.code == cli::exit_success);
        CHECK(similar.json["matches"].size() >= 1);
        const auto backup = directory.path() / "backup";
        CHECK(run({"library", "backup", "--library", path_text(library), "--output", path_text(backup)}).code ==
              cli::exit_success);
        const auto verified = run({"library", "verify", "--library", path_text(library), "--objects"});
        CHECK(verified.code == cli::exit_success);
        CHECK(verified.json["report"]["ok"] == true);
        CHECK(run({"library", "repair", "--library", path_text(library), "--rebuild-fts"}).code == cli::exit_success);
        // A derived-index rebuild drops the motif index; search reports partial until analyze runs.
        const auto rebuilt = run({"library", "repair", "--library", path_text(library), "--rebuild-derived"});
        INFO(rebuilt.json.dump());
        REQUIRE(rebuilt.code == cli::exit_success);
        CHECK(rebuilt.json["report"]["derived_rows_removed"].get<int>() > 0);
        CHECK(run({"library", "repair", "--library", path_text(library)}).code == cli::exit_invalid_arguments);
        const auto reanalyzed = run({"library", "analyze", "--library", path_text(library)});
        INFO(reanalyzed.json.dump());
        CHECK(reanalyzed.code == cli::exit_success);
        CHECK(reanalyzed.json["report"]["analyzed"].get<int>() > 0);
        CHECK(reanalyzed.json["report"]["budget_reached"] == false);
        const auto after = run({"library", "melody", "--library", path_text(library), "--notes", "D4 E4 F4 G4", "--mode",
                                "transposed"});
        CHECK(after.code == cli::exit_success);
        CHECK(after.json["matches"].size() >= 1);
        // gc: nothing is unreferenced in a fresh library; the report is complete and nothing is lost.
        const auto gc = run({"library", "gc", "--library", path_text(library), "--grace-days", "0"});
        INFO(gc.json.dump());
        REQUIRE(gc.code == cli::exit_success);
        CHECK(gc.json["report"]["suspended"] == false);
        CHECK(gc.json["report"]["deleted"] == 0);
        CHECK(run({"library", "gc", "--library", path_text(library), "--grace-days", "-1"}).code ==
              cli::exit_invalid_arguments);
        CHECK(run({"library", "verify", "--library", path_text(library), "--objects"}).code == cli::exit_success);
    }

    TEST_CASE("untrusted MIDI files and SF2 banks are parsed in mwb-worker (code-runtime-14)") {
        const auto capabilities = run({"capabilities", "--json"});
        REQUIRE(capabilities.code == cli::exit_success);
        INFO(capabilities.json["parser_isolation"].dump());
        CHECK(capabilities.json["parser_isolation"].get<std::string>().starts_with("mwb-worker:"));
        test::TemporaryDirectory directory;
        const auto source = write_phrase(directory.path(), "phrase.mid");
        // A broken bank is refused by the worker; the CLI reports it and keeps running.
        const auto broken = directory.path() / "broken.sf2";
        {
            auto bytes = test::make_test_soundfont();
            bytes.resize(bytes.size() / 2);
            std::ofstream file(broken, std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        const auto refused = run({"render", path_text(source), "--output", path_text(directory.path() / "a.wav"), "--synth",
                                  path_text(broken)});
        INFO(refused.json.dump());
        CHECK(refused.code != cli::exit_success);
        CHECK(refused.json.dump().find("SoundFont") != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(directory.path() / "a.wav"));
        const auto good = directory.path() / "good.sf2";
        {
            const auto bytes = test::make_test_soundfont();
            std::ofstream file(good, std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        const auto rendered = run({"render", path_text(source), "--output", path_text(directory.path() / "b.wav"), "--synth",
                                   path_text(good), "--tail", "0"});
        INFO(rendered.json.dump());
        CHECK(rendered.code == cli::exit_success);
        // Import and analysis go through the same worker.
        const auto sources = directory.path() / "sources";
        std::filesystem::create_directories(sources);
        write_phrase(sources, "good.mid");
        const auto imported = run({"library", "import", "--library", path_text(directory.path() / "lib"), "--from",
                                   path_text(sources)});
        INFO(imported.json["report"]["counts"].dump());
        CHECK(imported.code == cli::exit_success);
        CHECK(imported.json["report"]["counts"]["imported"].get<int>() == 1);
        const auto analyzed = run({"library", "analyze", "--library", path_text(directory.path() / "lib")});
        CHECK(analyzed.code == cli::exit_success);
        CHECK(analyzed.json["report"]["analyzed"].get<int>() == 1);
    }

    TEST_CASE("flags after the subcommand, JSON for argument errors, consistent statuses (code-runtime-20/21/23/33)") {
        test::TemporaryDirectory directory;
        const auto source = write_phrase(directory.path(), "phrase.mid");
        // A global flag after the subcommand is accepted.
        const auto inspected = run({"inspect", path_text(source), "--warnings-as-errors"});
        INFO(inspected.json.dump());
        CHECK(inspected.code == cli::exit_success);
        CHECK(inspected.json["status"] == (inspected.json["warnings"].empty() ? "ok" : "warning"));
        // Argument errors still print versioned JSON on stdout.
        const auto missing = run({"inspect"});
        CHECK(missing.code == cli::exit_invalid_arguments);
        CHECK(missing.json["status"] == "error");
        CHECK(missing.json["schema_version"] == 1);
        CHECK_FALSE(missing.json["errors"].empty());
        // Absurd melody durations are refused instead of overflowing the onsets.
        const auto library = directory.path() / "library";
        REQUIRE(run({"library", "import", "--library", path_text(library), "--from", path_text(source)}).code ==
                cli::exit_success);
        CHECK(run({"library", "melody", "--library", path_text(library), "--notes", "C4:9000000000000000000 D4"}).code ==
              cli::exit_invalid_arguments);
    }

    TEST_CASE("capabilities lists dependencies and SQLite options") {
        const auto result = run({"capabilities", "--json"});
        REQUIRE(result.code == cli::exit_success);
        CHECK(result.json["dependencies"].size() >= 20);
        bool fts5 = false;
        for (const auto& option : result.json["sqlite"]["compile_options"])
            fts5 = fts5 || option.get<std::string>() == "ENABLE_FTS5";
        CHECK(fts5);
    }
}
