#pragma once

#include "mwb/audio/block_renderer.hpp"
#include "mwb/audio/meters.hpp"
#include "mwb/audio/metronome.hpp"
#include "mwb/audio/synth.hpp"
#include "mwb/core/error.hpp"
#include "mwb/playback/handoff.hpp"
#include "mwb/playback/realtime_event.hpp"
#include "mwb/playback/scheduler.hpp"
#include "mwb/playback/snapshot.hpp"
#include "mwb/platform/audio_output.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace mwb::platform {

/// Transport commands sent from the owner thread to the render thread.
enum class EngineCommandKind : std::uint8_t { play, stop, seek, panic, metronome_on, metronome_off };

/// One control message (fixed size, travels through a preallocated SPSC queue).
struct EngineCommand {
    EngineCommandKind kind = EngineCommandKind::stop;
    Tick tick = 0;
};

/// An event for an external MIDI endpoint, with the steady-clock time it should be sent.
struct ExternalMidiEvent {
    std::int64_t due_steady_ns = 0;   ///< steady_clock nanoseconds (output latency included).
    std::uint64_t route_key = 0;      ///< playback::Route::key of the destination route.
    std::uint8_t size = 0;
    std::array<std::uint8_t, 3> bytes{};
};

/// Engine configuration, fixed at creation (all queues are preallocated here).
struct AudioEngineSettings {
    std::uint32_t max_block_frames = 4096;     ///< Larger callbacks are processed in slices.
    std::size_t control_queue_capacity = 256;
    std::size_t live_queue_capacity = 1024;
    std::size_t external_queue_capacity = 4096;
    playback::SchedulerSettings scheduler;
    float initial_volume = 0.5f;               ///< Conservative start level (section 13.2).
    float volume_ramp_seconds = 0.02f;         ///< Smoothing time constant for volume changes.
};

/// Telemetry published by the render thread (atomics, coalescable).
struct EngineTelemetry {
    std::uint64_t frames_rendered = 0;
    std::uint64_t callbacks = 0;
    std::uint64_t deadline_misses = 0;     ///< Callbacks whose processing took longer than their audio.
    std::uint64_t control_dropped = 0;     ///< Commands rejected because the control queue was full.
    std::uint64_t live_dropped = 0;        ///< Live input events rejected because the queue was full.
    std::uint64_t external_dropped = 0;    ///< External events the MIDI output side could not accept.
    std::uint64_t snapshot_generation = 0; ///< Generation currently used for playback.
    std::int64_t song_frame = 0;
    Tick position = 0;
    std::uint64_t epoch = 0;
    bool playing = false;
    bool reached_end = false;
};

/// Owns the real-time render callback (section 13.2/13.4/20): pulls control commands and live
/// input from preallocated SPSC queues (moodycamel ReaderWriterQueue), swaps snapshots at block
/// boundaries through the retiring handoff, runs the playback scheduler into the shared
/// BlockRenderer (the same path as offline rendering), applies the smoothed master volume and
/// publishes peak meters and counters. The callback never allocates, locks, logs or destroys a
/// snapshot.
///
/// Wiring for live playback: create the engine, set instruments, open() it on an IAudioOutput
/// (which prepares the synths at the negotiated rate), compile a snapshot at sample_rate() and
/// publish() it, then post(play). After device loss call handle_device_lost().
class AudioEngine final : public IAudioRenderCallback {
public:
    [[nodiscard]] static Result<std::unique_ptr<AudioEngine>> create(const AudioEngineSettings& settings = {});
    ~AudioEngine() override;
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    /// Sets the instruments (built-in synth required; SoundFont and metronome optional). Only
    /// while no output is running.
    [[nodiscard]] Status set_instruments(audio::ISynth* builtin, audio::ISynth* soundfont,
                                         audio::MetronomeSynth* metronome, audio::BuiltinSynth* timbre_target = nullptr);
    /// Opens `output`, prepares instruments at the negotiated rate and starts the device.
    [[nodiscard]] Result<NegotiatedFormat> open(IAudioOutput& output, const AudioOutputConfig& config);
    /// Stops and closes the output (owned notes are released by a final panic on the synths).
    void close();
    /// Sample rate snapshots must be compiled for (0 before open()).
    [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }

    /// Publishes a snapshot (owner thread). Refused when live playback is not allowed by the
    /// real-time preflight, when it was compiled for another rate, or has too many routes.
    [[nodiscard]] Status publish(std::shared_ptr<const playback::PlaybackSnapshot> snapshot);
    /// Destroys snapshots the render thread retired (owner thread, call periodically).
    std::size_t collect_retired();
    /// Queues a transport command; ErrorCode::busy when the control queue is full.
    [[nodiscard]] Status post(EngineCommand command);
    /// Queues a live-input event for the built-in synth (single producer); false when full.
    [[nodiscard]] bool post_live(const playback::RealtimeMidiEvent& event) noexcept;
    /// Target master volume (linear 0..2); smoothed on the render thread.
    void set_master_volume(float volume) noexcept;
    /// Takes the next external MIDI event, waiting up to `timeout` (MIDI output side).
    [[nodiscard]] bool wait_external(ExternalMidiEvent& event, std::chrono::microseconds timeout);
    /// Latency added to external event due times (calibration, owner thread).
    void set_external_latency(std::chrono::nanoseconds latency) noexcept;

    [[nodiscard]] EngineTelemetry telemetry() const noexcept;
    /// Peak meter (read with take() at display rate).
    [[nodiscard]] audio::PeakMeter& meter() noexcept { return meter_; }
    /// True when the output reported device loss; the owner then calls handle_device_lost().
    [[nodiscard]] bool device_lost() const noexcept;
    /// Releases voices and owned notes after device loss (render thread no longer running) and
    /// closes the output; the document position is retained in telemetry().position.
    void handle_device_lost() noexcept;

    /// Render callback (audio thread).
    void render(float* interleaved_stereo, std::uint32_t frame_count) noexcept override;

private:
    struct Queues;
    class ExternalForwarder;
    explicit AudioEngine(const AudioEngineSettings& settings);
    void render_slice(float* output, std::uint32_t frames, std::int64_t slice_start_ns) noexcept;
    void apply_command(const EngineCommand& command) noexcept;

    AudioEngineSettings settings_;
    std::unique_ptr<Queues> queues_;
    std::unique_ptr<ExternalForwarder> forwarder_;
    playback::Scheduler scheduler_;
    audio::BlockRenderer renderer_;
    playback::RetiringHandoff<playback::PlaybackSnapshot> handoff_;
    audio::PeakMeter meter_;
    audio::ISynth* builtin_ = nullptr;
    audio::ISynth* soundfont_ = nullptr;
    audio::MetronomeSynth* metronome_ = nullptr;
    IAudioOutput* output_ = nullptr;
    std::uint32_t sample_rate_ = 0;
    float current_volume_ = 0.5f;
    float volume_coefficient_ = 0.0f;
    bool stop_pending_ = false;
    bool end_reached_in_callback_ = false;

    std::atomic<float> target_volume_{0.5f};
    std::atomic<std::int64_t> external_latency_ns_{0};
    std::atomic<std::uint64_t> frames_rendered_{0};
    std::atomic<std::uint64_t> callbacks_{0};
    std::atomic<std::uint64_t> deadline_misses_{0};
    std::atomic<std::uint64_t> control_dropped_{0};
    std::atomic<std::uint64_t> live_dropped_{0};
    std::atomic<std::uint64_t> external_dropped_{0};
    std::atomic<std::uint64_t> snapshot_generation_{0};
    std::atomic<std::int64_t> song_frame_{0};
    std::atomic<Tick> position_{0};
    std::atomic<std::uint64_t> epoch_{0};
    std::atomic<bool> playing_{false};
    std::atomic<bool> reached_end_{false};
};

} // namespace mwb::platform
