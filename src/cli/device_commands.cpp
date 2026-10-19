#include "mwb/app/document_io.hpp"
#include "mwb/audio/builtin_synth.hpp"
#include "mwb/audio/offline_renderer.hpp"
#include "mwb/audio/soundfont_synth.hpp"
#include "mwb/cli/cli.hpp"
#include "mwb/cli/commands.hpp"
#include "mwb/core/text.hpp"
#include "mwb/playback/snapshot.hpp"
#include "mwb/platform/audio_output.hpp"
#include "mwb/platform/midi_backend.hpp"

#include <cmath>

namespace mwb::cli {

/// render INPUT --output FILE.wav [--synth builtin|bank.sf2]: offline rendering through the same
/// snapshot, scheduler and synths as live playback. No external MIDI is ever sent.
CommandOutput command_render(const CommandOptions& options) {
    auto opened = app::open_document(app::path_from_utf8(options.input), {}, {}, worker_parser().parser());
    if (!opened)
        return failure("render", opened.error());
    DocumentData document = std::move(opened->document);
    if (document.sequences.empty())
        return failure("render", Error{ErrorCode::invalid_data, "document has no sequence", {}, {}});
    const auto output = app::path_from_utf8(options.output);
    std::error_code exists_error;
    if (!options.overwrite && std::filesystem::exists(output, exists_error))
        return failure("render", Error{ErrorCode::io, "destination exists; pass --overwrite to replace it", {}, {}});

    audio::RenderSettings settings;
    settings.output = output;
    settings.sample_rate = static_cast<std::uint32_t>(options.sample_rate);
    settings.tail_seconds = options.tail_seconds;
    settings.normalize = options.normalize;
    if (options.wav_format == "pcm16")
        settings.format = audio::WavSampleFormat::pcm16;
    else if (options.wav_format == "float32")
        settings.format = audio::WavSampleFormat::float32;
    else if (options.wav_format == "pcm24")
        settings.format = audio::WavSampleFormat::pcm24;
    else
        return failure("render", Error{ErrorCode::invalid_argument, "format must be pcm16, pcm24 or float32", {}, {}});

    std::unique_ptr<audio::SoundFontSynth> soundfont;
    if (ascii_lower(options.synth) != "builtin") {
        // The bank is parsed in the worker first; only a bank that loaded there is loaded here.
        auto bytes = audio::SoundFontSynth::read_file(app::path_from_utf8(options.synth));
        if (!bytes)
            return failure("render", bytes.error());
        auto checked = worker_parser().validate_soundfont(*bytes);
        if (!checked)
            return failure("render", checked.error().with_context("checking the SoundFont"));
        auto loaded = audio::SoundFontSynth::load_memory(*bytes);
        if (!loaded)
            return failure("render", loaded.error());
        soundfont = std::move(*loaded);
        for (auto& sequence : document.sequences)
            for (auto& track : sequence.tracks) {
                track.routing.output = OutputKind::soundfont;
                track.instrument.backend = "sf2";
                track.instrument.bank_sha256 = soundfont->bank_sha256();
            }
    }
    auto builtin = audio::BuiltinSynth::create();
    if (!builtin)
        return failure("render", builtin.error());
    playback::PlaybackOptions playback_options;
    playback_options.sequence = document.sequences.front().id;
    playback_options.sample_rate = settings.sample_rate;
    auto snapshot = playback::compile_playback(document, playback_options, {});
    if (!snapshot)
        return failure("render", snapshot.error());
    auto report = audio::render_to_wav(**snapshot, **builtin, settings, {}, soundfont.get());
    if (!report)
        return failure("render", report.error());
    CommandOutput result;
    result.json = {{"schema_version", 1},
                   {"status", "ok"},
                   {"command", "render"},
                   {"summary",
                    {{"output", options.output},
                     {"sample_rate", settings.sample_rate},
                     {"format", audio::wav_sample_format_name(settings.format)},
                     {"total_frames", report->total_frames},
                     {"musical_frames", report->musical_frames},
                     {"tail_frames", report->tail_frames},
                     {"peak_before", report->peak_before},
                     {"peak_after", report->peak_after},
                     {"gain", report->gain},
                     {"clipped_samples", report->clipped_samples}}},
                   {"provenance", report->provenance.to_text()},
                   {"warnings", report->clipped_samples > 0
                                    ? nlohmann::json::array({{{"code", "render.clipped"},
                                                              {"message", "output clipped; consider --normalize"}}})
                                    : nlohmann::json::array()},
                   {"errors", nlohmann::json::array()}};
    return result;
}

/// devices --json: audio endpoints and MIDI ports with their backend identity.
CommandOutput command_devices(const CommandOptions&) {
    nlohmann::json audio = nlohmann::json::array();
    nlohmann::json warnings = nlohmann::json::array();
    std::string audio_backend = "unavailable";
    if (auto output = platform::create_audio_output()) {
        audio_backend = std::string((*output)->backend_name());
        if (auto devices = (*output)->enumerate_devices()) {
            for (const auto& device : *devices)
                audio.push_back({{"id", device.id}, {"name", device.name}, {"default", device.is_default}});
        } else {
            warnings.push_back({{"code", "devices.audio"}, {"message", devices.error().describe()}});
        }
    } else {
        warnings.push_back({{"code", "devices.audio"}, {"message", output.error().describe()}});
    }
    nlohmann::json inputs = nlohmann::json::array();
    nlohmann::json outputs = nlohmann::json::array();
    auto backend = platform::create_midi_backend();
    if (!backend)
        return failure("devices", backend.error().wrapped(ErrorCode::device, "MIDI backend unavailable"));
    const auto describe = [](const platform::MidiPortInfo& port) {
        return nlohmann::json{{"backend", port.backend},
                              {"stable_id", port.stable_id},
                              {"name", port.display_name},
                              {"session_index", port.session_index}};
    };
    if (auto ports = (*backend)->enumerate_inputs())
        for (const auto& port : *ports)
            inputs.push_back(describe(port));
    else
        warnings.push_back({{"code", "devices.midi_in"}, {"message", ports.error().describe()}});
    if (auto ports = (*backend)->enumerate_outputs())
        for (const auto& port : *ports)
            outputs.push_back(describe(port));
    else
        warnings.push_back({{"code", "devices.midi_out"}, {"message", ports.error().describe()}});
    CommandOutput result;
    result.json = {{"schema_version", 1},
                   {"status", "ok"},
                   {"command", "devices"},
                   {"audio", {{"backend", audio_backend}, {"devices", std::move(audio)}}},
                   {"midi", {{"inputs", std::move(inputs)}, {"outputs", std::move(outputs)}}},
                   {"warnings", std::move(warnings)},
                   {"errors", nlohmann::json::array()}};
    return result;
}

} // namespace mwb::cli
