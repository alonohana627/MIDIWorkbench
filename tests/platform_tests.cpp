// Platform adapter tests (specification sections 13.2, 14.1, 22.4, 34.5). Audio and MIDI use the
// null backends; only the explicit enumeration test touches real WinMM / WASAPI, and it plays
// nothing. Waiting uses wait_until with deadlines, never fixed sleeps.

#include "test_support.hpp"

#include "mwb/audio/builtin_synth.hpp"
#include "mwb/audio/metronome.hpp"
#include "mwb/core/change_set.hpp"
#include "mwb/core/model.hpp"
#include "mwb/playback/snapshot.hpp"
#include "mwb/playback/transport.hpp"
#include "mwb/platform/audio_engine.hpp"
#include "mwb/platform/audio_output.hpp"
#include "mwb/platform/file_watcher.hpp"
#include "mwb/platform/midi_backend.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <mutex>
#include <vector>

using namespace mwb;
using namespace mwb::platform;
using namespace std::chrono_literals;

namespace {

DocumentData melody_document() {
    DocumentData document = make_blank_document();
    const TrackId track = document.sequences.front().tracks.front().id;
    ChangeBuilder builder(document, "melody");
    for (int index = 0; index < 8; ++index)
        builder.add_note(track, index * 480, index * 480 + 400, 0, static_cast<std::uint8_t>(60 + index), 100);
    REQUIRE(apply_change_set(document, builder.finish()));
    return document;
}

std::shared_ptr<const playback::PlaybackSnapshot> compile(const DocumentData& document, std::uint32_t rate,
                                                          std::size_t budget = 8192) {
    playback::PlaybackOptions options;
    options.sample_rate = rate;
    options.max_events_per_block = budget;
    auto snapshot = playback::compile_playback(document, options);
    REQUIRE(snapshot);
    return std::shared_ptr<const playback::PlaybackSnapshot>(std::move(*snapshot));
}

/// Counts frames delivered by an output.
class CountingCallback final : public IAudioRenderCallback {
public:
    void render(float* interleaved_stereo, std::uint32_t frame_count) noexcept override {
        std::fill(interleaved_stereo, interleaved_stereo + static_cast<std::size_t>(frame_count) * 2, 0.0f);
        frames += frame_count;
        ++calls;
        largest = std::max(largest, frame_count);
    }
    std::uint64_t frames = 0;
    std::uint64_t calls = 0;
    std::uint32_t largest = 0;
};

} // namespace

TEST_SUITE("platform") {
    TEST_CASE("null audio output drives the render callback from a synthetic clock") {
        NullAudioSettings settings;
        settings.sample_rate = 48000;
        settings.period_frames = 256;
        NullAudioOutput output(settings);
        CountingCallback callback;
        const auto format = output.open(AudioOutputConfig{}, callback);
        REQUIRE(format);
        CHECK(format->sample_rate == 48000);
        CHECK(format->period_frames == 256);
        REQUIRE(output.pump(10000));
        CHECK(callback.frames == 10000);
        CHECK(output.rendered_frames() == 10000);
        CHECK(callback.largest == 256);
        CHECK(callback.calls == 40); // 39 full periods and one partial.
        output.close();
        CHECK(output.enumerate_devices()->size() == 1);
        if (null_audio_backend_requested()) {
            auto chosen = create_audio_output();
            REQUIRE(chosen);
            CHECK((*chosen)->backend_name() == "null");
        }
    }

    TEST_CASE("audio engine plays a snapshot through the null backend and counts frames") {
        auto synth = audio::BuiltinSynth::create();
        REQUIRE(synth);
        audio::MetronomeSynth metronome;
        auto engine_result = AudioEngine::create();
        REQUIRE(engine_result);
        AudioEngine& engine = **engine_result;
        REQUIRE(engine.set_instruments(synth->get(), nullptr, &metronome, synth->get()));

        NullAudioSettings settings;
        settings.paced = false; // Synthetic clock as fast as possible.
        settings.period_frames = 512;
        NullAudioOutput output(settings);
        const auto format = engine.open(output, AudioOutputConfig{});
        REQUIRE(format);
        CHECK(engine.sample_rate() == 48000);

        const DocumentData document = melody_document();
        REQUIRE(engine.publish(compile(document, engine.sample_rate())));
        REQUIRE(engine.post(EngineCommand{EngineCommandKind::play, 0}));
        REQUIRE(test::wait_until([&] { return engine.telemetry().snapshot_generation != 0; }));
        REQUIRE(test::wait_until([&] { return engine.telemetry().reached_end; }, 20s));
        const EngineTelemetry telemetry = engine.telemetry();
        CHECK(telemetry.frames_rendered >= 384000); // At least the four bars (8 s at 48 kHz).
        CHECK(output.rendered_frames() >= telemetry.frames_rendered); // The output counts after the callback returns.
        CHECK(telemetry.callbacks > 0);
        CHECK(telemetry.control_dropped == 0);
        CHECK_FALSE(telemetry.playing);
        const audio::PeakReading peaks = engine.meter().take();
        CHECK(peaks.left > 0.0f);

        // A newer snapshot is swapped in and the old one is retired off the render thread.
        auto first_generation = telemetry.snapshot_generation;
        REQUIRE(engine.publish(compile(document, engine.sample_rate())));
        REQUIRE(test::wait_until([&] { return engine.telemetry().snapshot_generation != first_generation; }));
        REQUIRE(test::wait_until([&] { return engine.collect_retired() >= 1; }));

        // Live input reaches the synth.
        REQUIRE(engine.post_live(playback::RealtimeMidiEvent::short_message(0x90, 72, 100)));
        REQUIRE(test::wait_until([&] { return synth.value()->active_voices() > 0; }));
        REQUIRE(engine.post_live(playback::RealtimeMidiEvent::short_message(0x80, 72, 0)));
        engine.set_master_volume(0.25f);

        // Snapshots refused by the real-time preflight never reach the engine.
        DocumentData dense = make_blank_document();
        {
            ChangeBuilder builder(dense, "dense");
            for (int index = 0; index < 64; ++index)
                builder.insert(dense.sequences.front().tracks.front().id, events::control_change(0, 0, 1, 5));
            REQUIRE(apply_change_set(dense, builder.finish()));
        }
        const auto refused = engine.publish(compile(dense, engine.sample_rate(), 16));
        REQUIRE_FALSE(refused);
        CHECK(refused.error().message.find("refused") != std::string::npos);
        engine.close();
    }

    TEST_CASE("device loss releases notes and moves the transport to DeviceLost") {
        auto synth = audio::BuiltinSynth::create();
        REQUIRE(synth);
        auto engine_result = AudioEngine::create();
        REQUIRE(engine_result);
        AudioEngine& engine = **engine_result;
        REQUIRE(engine.set_instruments(synth->get(), nullptr, nullptr));
        NullAudioSettings settings;
        settings.paced = true;
        NullAudioOutput output(settings);
        REQUIRE(engine.open(output, AudioOutputConfig{}));
        playback::Transport transport;
        REQUIRE(engine.publish(compile(melody_document(), engine.sample_rate())));
        REQUIRE(transport.apply(playback::TransportCommand::play));
        REQUIRE(engine.post(EngineCommand{EngineCommandKind::play, 0}));
        REQUIRE(test::wait_until([&] { return engine.telemetry().playing && synth.value()->active_voices() > 0; }));
        output.simulate_device_loss();
        REQUIRE(test::wait_until([&] { return engine.device_lost(); }));
        REQUIRE(transport.apply(playback::TransportCommand::device_failure));
        engine.handle_device_lost();
        CHECK(transport.state() == playback::TransportState::device_lost);
        CHECK_FALSE(engine.telemetry().playing);
        CHECK(engine.telemetry().position > 0); // Position retained.
        REQUIRE(transport.apply(playback::TransportCommand::reconfigure));
    }

    TEST_CASE("external routes reach the MIDI output side with due times") {
        auto synth = audio::BuiltinSynth::create();
        REQUIRE(synth);
        auto engine_result = AudioEngine::create();
        REQUIRE(engine_result);
        AudioEngine& engine = **engine_result;
        REQUIRE(engine.set_instruments(synth->get(), nullptr, nullptr));
        NullAudioSettings settings;
        settings.paced = false;
        NullAudioOutput output(settings);
        REQUIRE(engine.open(output, AudioOutputConfig{}));
        DocumentData document = melody_document();
        document.sequences.front().tracks.front().routing.output = OutputKind::external_midi;
        document.sequences.front().tracks.front().routing.endpoint_id = "null:out:loopback";
        const auto snapshot = compile(document, engine.sample_rate());
        REQUIRE(engine.publish(snapshot));
        REQUIRE(engine.post(EngineCommand{EngineCommandKind::play, 0}));
        ExternalMidiEvent event;
        bool note_on = false;
        REQUIRE(test::wait_until([&] {
            while (engine.wait_external(event, 1ms))
                if ((event.bytes[0] & 0xF0) == 0x90)
                    note_on = true;
            return note_on;
        }));
        CHECK(event.route_key == snapshot->routes.front().key);
        CHECK(event.due_steady_ns > 0);
        CHECK(synth.value()->active_voices() == 0); // Nothing went to the internal synth.
        engine.close();
    }

    TEST_CASE("null MIDI backend round trip through the output thread") {
        auto backend = create_null_midi_backend();
        const auto inputs = backend->enumerate_inputs();
        const auto outputs = backend->enumerate_outputs();
        REQUIRE(inputs);
        REQUIRE(outputs);
        REQUIRE(inputs->size() == 1);
        auto input = backend->open_input(inputs->front().stable_id);
        REQUIRE(input);
        auto output = backend->open_output(outputs->front().stable_id);
        REQUIRE(output);

        const std::array<std::uint8_t, 3> note_on{0x90, 60, 100};
        const std::array<std::uint8_t, 3> note_off{0x80, 60, 0};
        REQUIRE((*output)->send(note_on));
        const auto due = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             (std::chrono::steady_clock::now() + 5ms).time_since_epoch())
                             .count();
        REQUIRE((*output)->send(note_off, due));
        CHECK_FALSE((*output)->send(std::array<std::uint8_t, 2>{0x90, 60})); // Malformed length.

        std::vector<MidiInputMessage> received;
        REQUIRE(test::wait_until([&] {
            std::array<MidiInputMessage, 16> buffer{};
            const std::size_t count = (*input)->drain(buffer);
            received.insert(received.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
            return received.size() >= 2;
        }));
        CHECK(received[0].bytes == note_on);
        CHECK(received[1].bytes == note_off);
        CHECK(received[1].arrival_ns >= due);
        CHECK(received[0].quality == TimestampQuality::arrival_estimated);
        REQUIRE((*output)->flush(2000ms));
        CHECK((*output)->statistics().sent == 2);

        // SysEx transmission is disabled by default.
        const std::array<std::uint8_t, 4> sysex{0xF0, 0x7E, 0x01, 0xF7};
        const auto refused = (*output)->send_sysex(sysex);
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == ErrorCode::unsupported);
        MidiOutputOptions allow;
        allow.allow_sysex = true;
        auto sysex_output = backend->open_output(outputs->front().stable_id, allow);
        REQUIRE(sysex_output);
        REQUIRE((*sysex_output)->send_sysex(sysex));
        REQUIRE(test::wait_until([&] { return (*input)->statistics().long_messages_dropped == 1; }));

        CHECK_FALSE(backend->open_input("winmm:in:0000:0000:Missing#0"));
        if (null_midi_backend_requested()) {
            auto chosen = create_midi_backend();
            REQUIRE(chosen);
            CHECK((*chosen)->name() == "null");
        }
    }

    TEST_CASE("64-bit timestamp extension survives WinMM millisecond wrap") {
        TimestampExtender extender(winmm_timestamp_wrap_ns);
        const std::int64_t near_end = winmm_timestamp_wrap_ns - 1'000'000;
        CHECK(extender.extend(near_end) == near_end);
        CHECK(extender.extend(near_end - 500) == near_end - 500); // Jitter, not a wrap.
        CHECK(extender.extend(2'000'000) == winmm_timestamp_wrap_ns + 2'000'000);
        CHECK(extender.wraps() == 1);
        CHECK(timestamp_quality_name(TimestampQuality::calibrated) == "calibrated");
    }

    TEST_CASE("real WinMM MIDI and WASAPI enumeration runs without failing") {
        auto backend = create_libremidi_backend();
        REQUIRE(backend);
        const auto inputs = (*backend)->enumerate_inputs();
        const auto outputs = (*backend)->enumerate_outputs();
        REQUIRE(inputs);
        REQUIRE(outputs);
        MESSAGE("MIDI inputs: " << inputs->size() << ", outputs: " << outputs->size());
        if (inputs->empty() && outputs->empty())
            MESSAGE("SKIPPED: no MIDI endpoints on this machine; identity checks not exercised");
        for (const MidiPortInfo& port : *outputs) {
            CHECK_FALSE(port.stable_id.empty());
            CHECK_FALSE(port.display_name.empty());
            CHECK_FALSE(port.is_input);
        }
        for (const MidiPortInfo& port : *inputs)
            CHECK(port.is_input);

        auto audio = create_miniaudio_output();
        if (!audio) {
            MESSAGE("SKIPPED: audio backend unavailable: " << audio.error().describe());
            return;
        }
        const auto devices = (*audio)->enumerate_devices();
        REQUIRE(devices);
        MESSAGE("audio backend " << (*audio)->backend_name() << ", playback devices: " << devices->size());
        for (const AudioDeviceInfo& device : *devices)
            CHECK_FALSE(device.id.empty());
    }

    TEST_CASE("file watcher debounces bursts into one hint and honours exclusions") {
        test::TemporaryDirectory directory;
        std::mutex mutex;
        std::vector<FileChangeHint> hints;
        std::size_t batches = 0;
        FileWatcherSettings settings;
        settings.debounce = 250ms;
        auto watcher = DirectoryWatcher::create(settings, [&](std::vector<FileChangeHint> batch) {
            std::lock_guard lock(mutex);
            ++batches;
            hints.insert(hints.end(), batch.begin(), batch.end());
        });
        REQUIRE(watcher);
        const auto excluded = directory.path() / "cache";
        std::filesystem::create_directory(excluded);
        (*watcher)->exclude(excluded);
        REQUIRE((*watcher)->add_directory(directory.path()));
        CHECK_FALSE((*watcher)->add_directory(directory.path() / "missing"));

        const auto file = directory.path() / "song.mid";
        for (int write = 0; write < 5; ++write) {
            std::ofstream stream(file, std::ios::binary | std::ios::app);
            stream << "MThd" << write;
        }
        {
            std::ofstream ignored(excluded / "preview.wav", std::ios::binary);
            ignored << "data";
        }
        auto count_for = [&](const std::filesystem::path& path) {
            std::lock_guard lock(mutex);
            return std::ranges::count_if(hints, [&](const FileChangeHint& hint) {
                return hint.path.lexically_normal() == path.lexically_normal();
            });
        };
        REQUIRE(test::wait_until([&] { return count_for(file) >= 1; }, 10s));
        CHECK((*watcher)->raw_notifications() >= 2); // Several raw events...
        CHECK(count_for(file) == 1);                 // ...coalesced into one hint.
        {
            std::lock_guard lock(mutex);
            const auto hint = std::ranges::find_if(hints, [&](const FileChangeHint& candidate) {
                return candidate.path.lexically_normal() == file.lexically_normal();
            });
            REQUIRE(hint != hints.end());
            CHECK(hint->kind == FileChangeKind::added);
        }
        // A later change arrives as a separate hint; the excluded write never does.
        const auto second = directory.path() / "other.mid";
        {
            std::ofstream stream(second, std::ios::binary);
            stream << "x";
        }
        REQUIRE(test::wait_until([&] { return count_for(second) >= 1; }, 10s));
        CHECK(count_for(excluded / "preview.wav") == 0);
        std::filesystem::remove(file);
        REQUIRE(test::wait_until(
            [&] {
                std::lock_guard lock(mutex);
                return std::ranges::any_of(hints, [&](const FileChangeHint& hint) {
                    return hint.kind == FileChangeKind::removed && hint.path.lexically_normal() == file.lexically_normal();
                });
            },
            10s));
        (*watcher)->remove_directory(directory.path());
    }
}
