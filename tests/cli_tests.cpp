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
