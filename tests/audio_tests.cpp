// Built-in synth, SoundFont adapter and offline WAV render tests (specification sections 13, 15,
// 34.5). Renders are offline; nothing is played through a device.

#include "test_support.hpp"

#include "mwb/audio/block_renderer.hpp"
#include "mwb/audio/builtin_synth.hpp"
#include "mwb/audio/meters.hpp"
#include "mwb/audio/offline_renderer.hpp"
#include "mwb/audio/soundfont_synth.hpp"
#include "mwb/core/change_set.hpp"
#include "mwb/core/model.hpp"
#include "mwb/playback/snapshot.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <numbers>
#include <stop_token>
#include <string>
#include <tuple>
#include <vector>

using namespace mwb;
using namespace mwb::audio;

namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/// Format information read back with miniaudio's decoder.
struct DecodedInfo {
    ma_format format = ma_format_unknown;
    ma_uint32 channels = 0;
    ma_uint32 sample_rate = 0;
    ma_uint64 frames = 0;
};

DecodedInfo decode_info(const std::vector<std::uint8_t>& bytes) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_unknown, 0, 0);
    ma_decoder decoder;
    REQUIRE(ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &decoder) == MA_SUCCESS);
    DecodedInfo info;
    info.format = decoder.outputFormat;
    info.channels = decoder.outputChannels;
    info.sample_rate = decoder.outputSampleRate;
    REQUIRE(ma_decoder_get_length_in_pcm_frames(&decoder, &info.frames) == MA_SUCCESS);
    ma_decoder_uninit(&decoder);
    return info;
}

/// A short phrase: four notes, a sustained chord and a drum hit.
DocumentData phrase_document() {
    DocumentData document = make_blank_document();
    const TrackId track = document.sequences.front().tracks.front().id;
    ChangeBuilder builder(document, "phrase");
    builder.add_note(track, 0, 960, 0, 60, 100);
    builder.add_note(track, 960, 1920, 0, 64, 90);
    builder.add_note(track, 1920, 2880, 0, 67, 80);
    builder.add_note(track, 2880, 3840, 0, 72, 110);
    builder.add_note(track, 3840, 7680, 0, 48, 100);
    builder.add_note(track, 0, 120, 9, 36, 120);
    builder.insert(track, events::control_change(3840, 0, 64, 127));
    REQUIRE(apply_change_set(document, builder.finish()));
    document.sequences.front().end_tick = 7680;
    document.sequences.front().tracks.front().end_tick = 7680;
    document.sequences.front().regions.clear();
    return document;
}

std::unique_ptr<const playback::PlaybackSnapshot> compile(const DocumentData& document, std::uint32_t rate) {
    playback::PlaybackOptions options;
    options.sample_rate = rate;
    auto snapshot = playback::compile_playback(document, options);
    REQUIRE(snapshot);
    return std::move(*snapshot);
}

std::unique_ptr<BuiltinSynth> make_synth(std::size_t voices = 256) {
    BuiltinSynthSettings settings;
    settings.voice_capacity = voices;
    auto synth = BuiltinSynth::create(settings);
    REQUIRE(synth);
    return std::move(*synth);
}

double energy(const std::vector<float>& buffer, std::size_t channel) {
    double sum = 0.0;
    for (std::size_t index = channel; index < buffer.size(); index += 2)
        sum += static_cast<double>(buffer[index]) * buffer[index];
    return sum;
}

playback::RealtimeMidiEvent message(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
    return playback::RealtimeMidiEvent::short_message(status, data1, data2);
}

} // namespace

TEST_SUITE("audio") {
    TEST_CASE("built-in synth validates its voice pool bounds") {
        BuiltinSynthSettings settings;
        settings.voice_capacity = 0;
        CHECK_FALSE(BuiltinSynth::create(settings));
        settings.voice_capacity = 513;
        CHECK_FALSE(BuiltinSynth::create(settings));
        settings.voice_capacity = 512;
        auto synth = BuiltinSynth::create(settings);
        REQUIRE(synth);
        CHECK((*synth)->voice_capacity() == 512);
        CHECK((*synth)->prepare(48000, 512));
        CHECK_FALSE((*synth)->prepare(100, 512));
    }

    TEST_CASE("every timbre and the percussion kit produce finite, non-silent audio") {
        auto synth = make_synth();
        REQUIRE(synth->prepare(48000, 1024));
        for (const std::uint8_t program : std::vector<std::uint8_t>{0, 4, 33, 48, 80}) {
            synth->dispatch(message(0xC0, program, 0));
            synth->dispatch(message(0x90, 60, 100));
            std::vector<float> buffer(2048, 0.0f);
            synth->render(buffer.data(), 1024);
            synth->dispatch(message(0x80, 60, 0));
            CHECK(energy(buffer, 0) > 0.0);
            CHECK(std::ranges::all_of(buffer, [](float value) { return std::isfinite(value); }));
        }
        for (const std::uint8_t note : std::vector<std::uint8_t>{36, 38, 39, 42, 46, 45, 49, 51, 37}) {
            synth->dispatch(message(0x99, note, 110));
            std::vector<float> buffer(2048, 0.0f);
            synth->render(buffer.data(), 1024);
            CHECK(energy(buffer, 0) > 0.0);
        }
        CHECK(parse_timbre("saw-pad") == Timbre::saw_pad);
        CHECK_FALSE(parse_timbre("auto"));
        CHECK(timbre_for_program(33) == Timbre::bass);
    }

    TEST_CASE("voice stealing beyond the pool prefers released voices") {
        auto synth = make_synth(2);
        REQUIRE(synth->prepare(48000, 4800));
        synth->dispatch(message(0xB0, 10, 0));    // Channel 1 hard left.
        synth->dispatch(message(0xB1, 10, 127));  // Channel 2 hard right.
        synth->dispatch(message(0x90, 60, 100));  // A (left)
        synth->dispatch(message(0x91, 64, 100));  // B (right), held
        std::vector<float> buffer(9600, 0.0f);
        synth->render(buffer.data(), 4800);
        synth->dispatch(message(0x80, 60, 0));    // A released
        synth->dispatch(message(0x90, 67, 100));  // C needs a voice: must steal A, not B
        CHECK(synth->steal_count() == 1);
        std::ranges::fill(buffer, 0.0f);
        synth->render(buffer.data(), 4800);
        std::ranges::fill(buffer, 0.0f);
        synth->render(buffer.data(), 4800);
        CHECK(energy(buffer, 1) > 1.0e-3); // B still sounds on the right.
        CHECK(synth->active_voices() <= 2);

        // Far beyond the pool: every extra note steals, the pool never grows.
        for (std::uint8_t note = 30; note < 90; ++note)
            synth->dispatch(message(0x92, note, 90));
        CHECK(synth->steal_count() == 61);
        synth->render(buffer.data(), 4800);
        CHECK(synth->active_voices() <= 2);
        CHECK(synth->dropped_notes() == 0);
        CHECK(std::ranges::all_of(buffer, [](float value) { return std::isfinite(value) && std::fabs(value) < 4.0f; }));
        synth->panic();
        for (int block = 0; block < 4; ++block)
            synth->render(buffer.data(), 4800);
        CHECK(synth->active_voices() == 0);
    }

    TEST_CASE("sustain pedal holds released notes until it is lifted") {
        auto synth = make_synth(8);
        REQUIRE(synth->prepare(48000, 4800));
        synth->dispatch(message(0xB0, 64, 127));
        synth->dispatch(message(0x90, 60, 100));
        synth->dispatch(message(0x80, 60, 0));
        std::vector<float> buffer(9600, 0.0f);
        for (int block = 0; block < 10; ++block)
            synth->render(buffer.data(), 4800);
        CHECK(synth->active_voices() == 1);
        synth->dispatch(message(0xB0, 64, 0));
        for (int block = 0; block < 20; ++block)
            synth->render(buffer.data(), 4800);
        CHECK(synth->active_voices() == 0);
    }

    TEST_CASE("peak meter publishes peaks and clipping through atomics") {
        PeakMeter meter;
        const std::vector<float> block = {0.25f, -0.5f, 1.5f, 0.1f};
        meter.update(block.data(), 2);
        const PeakReading reading = meter.take();
        CHECK(reading.left == doctest::Approx(1.5f));
        CHECK(reading.right == doctest::Approx(0.5f));
        CHECK(reading.clipped_samples == 1);
        CHECK(meter.take().left == 0.0f);
    }

    TEST_CASE("SoundFont load failures are reported, not crashed on") {
        test::TemporaryDirectory directory;
        const auto garbage = directory.path() / "garbage.sf2";
        {
            std::ofstream stream(garbage, std::ios::binary);
            for (int index = 0; index < 4096; ++index)
                stream.put(static_cast<char>((index * 37) & 0xFF));
        }
        auto result = SoundFontSynth::load_file(garbage);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::invalid_data);

        auto missing = SoundFontSynth::load_file(directory.path() / "missing.sf2");
        REQUIRE_FALSE(missing);
        CHECK(missing.error().code == ErrorCode::io);

        // RIFF sfbk header with truncated contents.
        std::vector<std::uint8_t> truncated = test::make_test_soundfont();
        truncated.resize(200);
        auto broken = SoundFontSynth::load_memory(truncated);
        CHECK_FALSE(broken);

        SoundFontOptions tiny_limit;
        tiny_limit.max_file_bytes = 1024;
        const std::vector<std::uint8_t> bank = test::make_test_soundfont();
        auto limited = SoundFontSynth::load_memory(bank, tiny_limit);
        REQUIRE_FALSE(limited);
        CHECK(limited.error().code == ErrorCode::limit_exceeded);
    }

    TEST_CASE("a generated SoundFont loads, enumerates presets and renders without growing") {
        test::TemporaryDirectory directory;
        const auto path = directory.path() / "test.sf2";
        {
            const std::vector<std::uint8_t> bank = test::make_test_soundfont();
            std::ofstream stream(path, std::ios::binary);
            stream.write(reinterpret_cast<const char*>(bank.data()), static_cast<std::streamsize>(bank.size()));
        }
        SoundFontOptions options;
        options.max_voices = 4;
        auto loaded = SoundFontSynth::load_file(path, options);
        REQUIRE(loaded);
        SoundFontSynth& synth = **loaded;
        REQUIRE(synth.presets().size() == 1);
        CHECK(synth.presets()[0].name == "Test Sine");
        CHECK(synth.presets()[0].bank == 0);
        CHECK(synth.presets()[0].program == 0);
        CHECK(synth.bank_sha256().size() == 64);
        REQUIRE(synth.prepare(48000, 1024));
        for (std::uint8_t note = 50; note < 70; ++note) // More notes than voices.
            synth.dispatch(message(0x90, note, 100));
        std::vector<float> buffer(2048, 0.0f);
        synth.render(buffer.data(), 1024);
        CHECK(energy(buffer, 0) > 0.0);
        CHECK(synth.active_voices() <= 4);
        synth.panic();
        CHECK(synth.identity().engine == "sf2");
    }
}

TEST_SUITE("render") {
    TEST_CASE("offline render has the exact frame count including the tail") {
        const DocumentData document = phrase_document();
        const auto snapshot = compile(document, 48000);
        CHECK(snapshot->end_frame == 192000); // 7680 ticks at 120 BPM = 4 s.
        test::TemporaryDirectory directory;
        auto synth = make_synth();
        RenderSettings settings;
        settings.output = directory.path() / "phrase.wav";
        settings.sample_rate = 48000;
        settings.tail_seconds = 2.0;
        const auto report = render_to_wav(*snapshot, *synth, settings);
        REQUIRE(report);
        CHECK(report->musical_frames == 192000);
        CHECK(report->tail_frames == 96000);
        CHECK(report->total_frames == 288000);
        const DecodedInfo info = decode_info(read_file(settings.output));
        CHECK(info.frames == 288000);
        CHECK(info.channels == 2);
        CHECK(info.sample_rate == 48000);
        CHECK(info.format == ma_format_s24);
        CHECK(report->peak_after > 0.0);
        CHECK(report->provenance.synth_engine == "builtin");
        CHECK(report->provenance.source_semantic_hash.size() == 64);
        CHECK(report->provenance.to_text().find("sample_rate=48000") != std::string::npos);

        RenderSettings ranged = settings;
        ranged.output = directory.path() / "range.wav";
        ranged.range = TimeRange{960, 1920};
        ranged.tail_seconds = 0.5;
        const auto ranged_report = render_to_wav(*snapshot, *synth, ranged);
        REQUIRE(ranged_report);
        CHECK(decode_info(read_file(ranged.output)).frames == 24000 + 24000);

        RenderSettings no_tail = settings;
        no_tail.output = directory.path() / "no_tail.wav";
        no_tail.tail_seconds = 0.0;
        REQUIRE(render_to_wav(*snapshot, *synth, no_tail));
        CHECK(decode_info(read_file(no_tail.output)).frames == 192000);

        RenderSettings bad = settings;
        bad.tail_seconds = 31.0;
        CHECK_FALSE(render_to_wav(*snapshot, *synth, bad));
        bad = settings;
        bad.sample_rate = 44100; // Snapshot compiled at 48 kHz.
        CHECK_FALSE(render_to_wav(*snapshot, *synth, bad));
    }

    TEST_CASE("rendering twice produces identical bytes, independent of the block size") {
        const DocumentData document = phrase_document();
        const auto snapshot = compile(document, 44100);
        test::TemporaryDirectory directory;
        auto synth = make_synth();
        RenderSettings settings;
        settings.sample_rate = 44100;
        settings.format = WavSampleFormat::pcm16;
        settings.dither = true;
        settings.output = directory.path() / "first.wav";
        REQUIRE(render_to_wav(*snapshot, *synth, settings));
        settings.output = directory.path() / "second.wav";
        REQUIRE(render_to_wav(*snapshot, *synth, settings));
        const auto first = read_file(directory.path() / "first.wav");
        CHECK(first == read_file(directory.path() / "second.wav"));
        settings.output = directory.path() / "third.wav";
        settings.block_frames = 333;
        REQUIRE(render_to_wav(*snapshot, *synth, settings));
        CHECK(first == read_file(directory.path() / "third.wav"));
        settings.output = directory.path() / "seeded.wav";
        settings.dither_seed = 99;
        settings.block_frames = 512;
        REQUIRE(render_to_wav(*snapshot, *synth, settings));
        CHECK(first != read_file(directory.path() / "seeded.wav"));
    }

    TEST_CASE("16-bit, 24-bit and float WAV headers read back through miniaudio at 44.1, 48 and 96 kHz") {
        const DocumentData document = phrase_document();
        test::TemporaryDirectory directory;
        auto synth = make_synth();
        const std::vector<std::tuple<WavSampleFormat, ma_format, std::uint32_t>> cases = {
            {WavSampleFormat::pcm16, ma_format_s16, 44100},
            {WavSampleFormat::pcm24, ma_format_s24, 48000},
            {WavSampleFormat::float32, ma_format_f32, 96000},
        };
        for (const auto& [format, expected, rate] : cases) {
            const auto snapshot = compile(document, rate);
            RenderSettings settings;
            settings.sample_rate = rate;
            settings.format = format;
            settings.tail_seconds = 0.25;
            settings.range = TimeRange{0, 960};
            settings.output = directory.path() / (std::string(wav_sample_format_name(format)) + ".wav");
            const auto report = render_to_wav(*snapshot, *synth, settings);
            REQUIRE(report);
            const DecodedInfo info = decode_info(read_file(settings.output));
            CHECK(info.format == expected);
            CHECK(info.sample_rate == rate);
            CHECK(info.channels == 2);
            CHECK(info.frames == report->total_frames);
            CHECK(report->total_frames == rate / 2 + rate / 4);
        }
    }

    TEST_CASE("peak normalization reports pre and post peak, gain and clipping") {
        const DocumentData document = phrase_document();
        const auto snapshot = compile(document, 48000);
        test::TemporaryDirectory directory;
        auto synth = make_synth();
        RenderSettings settings;
        settings.sample_rate = 48000;
        settings.format = WavSampleFormat::float32;
        settings.normalize = true;
        settings.normalize_peak_dbfs = -1.0;
        settings.output = directory.path() / "normalized.wav";
        const auto report = render_to_wav(*snapshot, *synth, settings);
        REQUIRE(report);
        CHECK(report->peak_before > 0.0);
        CHECK(report->peak_after == doctest::Approx(std::pow(10.0, -1.0 / 20.0)).epsilon(1e-6));
        CHECK(report->gain == doctest::Approx(report->peak_after / report->peak_before));
        CHECK(report->clipped_samples == 0);
        CHECK(report->provenance.normalize);

        settings.normalize_peak_dbfs = 6.0; // Deliberately above full scale: clipping is reported.
        settings.output = directory.path() / "hot.wav";
        const auto hot = render_to_wav(*snapshot, *synth, settings);
        REQUIRE(hot);
        CHECK(hot->clipped_samples > 0);
    }

    TEST_CASE("cancel removes incomplete output and keeps an existing target; success replaces it") {
        const DocumentData document = phrase_document();
        const auto snapshot = compile(document, 48000);
        test::TemporaryDirectory directory;
        auto synth = make_synth();
        RenderSettings settings;
        settings.sample_rate = 48000;
        settings.output = directory.path() / "target.wav";
        {
            std::ofstream existing(settings.output, std::ios::binary);
            existing << "previous";
        }
        std::stop_source stop;
        stop.request_stop();
        const auto cancelled = render_to_wav(*snapshot, *synth, settings, stop.get_token());
        REQUIRE_FALSE(cancelled);
        CHECK(cancelled.error().code == ErrorCode::cancelled);
        CHECK(read_file(settings.output) == std::vector<std::uint8_t>{'p', 'r', 'e', 'v', 'i', 'o', 'u', 's'});

        // Cancel in the middle through the progress callback.
        std::stop_source midway;
        settings.progress = [&](double fraction) {
            if (fraction > 0.3)
                midway.request_stop();
        };
        const auto interrupted = render_to_wav(*snapshot, *synth, settings, midway.get_token());
        REQUIRE_FALSE(interrupted);
        std::size_t entries = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory.path())) {
            ++entries;
            CHECK(entry.path().filename() == "target.wav");
        }
        CHECK(entries == 1);

        settings.progress = nullptr;
        REQUIRE(render_to_wav(*snapshot, *synth, settings));
        CHECK(decode_info(read_file(settings.output)).frames == 192000 + 96000);
    }

    TEST_CASE("the metronome is excluded from renders unless requested") {
        DocumentData document = make_blank_document(); // No notes: silence apart from clicks.
        const auto snapshot = compile(document, 48000);
        test::TemporaryDirectory directory;
        auto synth = make_synth();
        RenderSettings settings;
        settings.sample_rate = 48000;
        settings.format = WavSampleFormat::float32;
        settings.tail_seconds = 0.0;
        settings.output = directory.path() / "silent.wav";
        const auto silent = render_to_wav(*snapshot, *synth, settings);
        REQUIRE(silent);
        CHECK(silent->peak_after == 0.0);
        settings.include_metronome = true;
        settings.output = directory.path() / "clicks.wav";
        const auto clicks = render_to_wav(*snapshot, *synth, settings);
        REQUIRE(clicks);
        CHECK(clicks->peak_after > 0.1);
        CHECK(clicks->provenance.metronome);
    }

    TEST_CASE("the block renderer applies events at exact sample offsets") {
        auto synth = make_synth();
        REQUIRE(synth->prepare(48000, 256));
        BlockRenderer renderer;
        renderer.set_instruments(synth.get(), nullptr, nullptr);
        std::vector<float> buffer(512, 1.0f);
        renderer.begin(buffer.data(), 256);
        renderer.on_event(playback::RealtimeMidiEvent::short_message(0x90, 69, 127, 100, playback::no_route));
        renderer.finish();
        for (std::size_t frame = 0; frame < 100; ++frame)
            CHECK(buffer[2 * frame] == 0.0f);
        CHECK(std::ranges::any_of(buffer.begin() + 200, buffer.end(), [](float value) { return value != 0.0f; }));
        CHECK(renderer.block_events() == 1);
    }
}
