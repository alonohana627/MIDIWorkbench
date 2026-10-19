#include "mwb/platform/audio_engine.hpp"

#include <readerwriterqueue.h>

#include <algorithm>
#include <cmath>
#include <thread>

namespace mwb::platform {

namespace {

constexpr std::size_t max_commands_per_block = 64;
constexpr std::size_t max_live_events_per_block = 256;

std::int64_t steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

/// Preallocated queues between the owner, input and MIDI output threads and the render thread.
struct AudioEngine::Queues {
    explicit Queues(const AudioEngineSettings& settings)
        : control(settings.control_queue_capacity),
          live(settings.live_queue_capacity),
          monitor(settings.live_queue_capacity),
          external(settings.external_queue_capacity) {}
    moodycamel::ReaderWriterQueue<EngineCommand> control;               ///< Owner -> render.
    moodycamel::ReaderWriterQueue<playback::RealtimeMidiEvent> live;    ///< Owner (auditions) -> render.
    moodycamel::ReaderWriterQueue<playback::RealtimeMidiEvent> monitor; ///< Input monitor -> render.
    moodycamel::BlockingReaderWriterQueue<ExternalMidiEvent> external;  ///< Render -> MIDI output.
};

/// Forwards events of external routes to the external queue with their due time.
class AudioEngine::ExternalForwarder final : public playback::IEventSink {
public:
    explicit ExternalForwarder(AudioEngine& engine) : engine_(engine) {}

    void begin_slice(std::int64_t slice_start_ns, std::uint32_t sample_rate) noexcept {
        slice_start_ns_ = slice_start_ns;
        sample_rate_ = sample_rate == 0 ? 48000 : sample_rate;
    }

    void on_event(const playback::RealtimeMidiEvent& event) noexcept override {
        const playback::PlaybackSnapshot* snapshot = engine_.scheduler_.snapshot();
        ExternalMidiEvent external;
        external.route_key = snapshot != nullptr && event.route < snapshot->routes.size()
                                 ? snapshot->routes[event.route].key
                                 : 0;
        external.size = event.size;
        external.bytes = event.bytes;
        external.due_steady_ns = slice_start_ns_ +
                                 static_cast<std::int64_t>(event.frame_offset) * 1'000'000'000 / sample_rate_ +
                                 engine_.external_latency_ns_.load(std::memory_order_relaxed);
        if (!engine_.queues_->external.try_enqueue(external))
            engine_.external_dropped_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    AudioEngine& engine_;
    std::int64_t slice_start_ns_ = 0;
    std::uint32_t sample_rate_ = 48000;
};

AudioEngine::AudioEngine(const AudioEngineSettings& settings)
    : settings_(settings),
      queues_(std::make_unique<Queues>(settings)),
      forwarder_(std::make_unique<ExternalForwarder>(*this)),
      scheduler_(settings.scheduler),
      renderer_(settings.scheduler.route_capacity),
      current_volume_(settings.initial_volume),
      target_volume_(settings.initial_volume) {
    renderer_.set_external_sink(forwarder_.get());
}

AudioEngine::~AudioEngine() {
    close();
    handoff_.reset();
}

Result<std::unique_ptr<AudioEngine>> AudioEngine::create(const AudioEngineSettings& settings) {
    if (settings.max_block_frames == 0 || settings.max_block_frames > 65536)
        return make_error(ErrorCode::invalid_argument, "engine block size must be between 1 and 65536 frames");
    if (settings.control_queue_capacity == 0 || settings.live_queue_capacity == 0 || settings.external_queue_capacity == 0)
        return make_error(ErrorCode::invalid_argument, "engine queue capacities must be positive");
    if (!(settings.initial_volume >= 0.0f) || settings.initial_volume > 2.0f)
        return make_error(ErrorCode::invalid_argument, "initial volume must be between 0 and 2");
    return std::unique_ptr<AudioEngine>(new AudioEngine(settings));
}

Status AudioEngine::set_instruments(audio::ISynth* builtin, audio::ISynth* soundfont, audio::MetronomeSynth* metronome,
                                    audio::BuiltinSynth* timbre_target) {
    if (output_ != nullptr)
        return make_error(ErrorCode::busy, "instruments can only change while the output is closed");
    if (builtin == nullptr)
        return make_error(ErrorCode::invalid_argument, "the built-in synth is required");
    builtin_ = builtin;
    soundfont_ = soundfont;
    metronome_ = metronome;
    renderer_.set_instruments(builtin, soundfont, metronome);
    renderer_.set_builtin_timbre_target(timbre_target);
    return ok();
}

Result<NegotiatedFormat> AudioEngine::open(IAudioOutput& output, const AudioOutputConfig& config) {
    if (output_ != nullptr)
        return make_error(ErrorCode::busy, "the engine already drives an output");
    if (builtin_ == nullptr)
        return make_error(ErrorCode::invalid_argument, "set the instruments before opening the output");
    auto format = output.open(config, *this);
    if (!format)
        return std::unexpected(std::move(format).error());
    auto fail = [&](Error error) -> Result<NegotiatedFormat> {
        output.close();
        return std::unexpected(std::move(error));
    };
    const std::uint32_t rate = format->sample_rate;
    if (auto prepared = builtin_->prepare(rate, settings_.max_block_frames); !prepared)
        return fail(prepared.error());
    if (soundfont_ != nullptr)
        if (auto prepared = soundfont_->prepare(rate, settings_.max_block_frames); !prepared)
            return fail(prepared.error());
    if (metronome_ != nullptr)
        if (auto prepared = metronome_->prepare(rate); !prepared)
            return fail(prepared.error());
    sample_rate_ = rate;
    volume_coefficient_ =
        1.0f - std::exp(-1.0f / std::max(settings_.volume_ramp_seconds * static_cast<float>(rate), 1.0f));
    output_ = &output;
    output_latency_ns_ = static_cast<std::int64_t>(format->buffer_latency_ms() * 1'000'000.0);
    // The render thread is not running yet: live input queued while no device was open is stale
    // (it would sound all at once), so it is discarded here.
    playback::RealtimeMidiEvent stale;
    while (queues_->live.try_dequeue(stale) || queues_->monitor.try_dequeue(stale)) {
    }
    if (auto started = output.start(); !started) {
        output_ = nullptr;
        return fail(started.error());
    }
    return format;
}

void AudioEngine::close() {
    if (output_ == nullptr)
        return;
    std::ignore = output_->stop(); // Closing below releases the device whatever stop reported.
    output_->close();
    output_ = nullptr;
    // The render thread has stopped: release owned notes and panic every used route, so external
    // MIDI gets its note-offs, sustain-off, all-notes-off and bend reset before the router drains.
    renderer_.begin(nullptr, 0);
    scheduler_.panic(renderer_);
    if (builtin_ != nullptr)
        builtin_->panic();
    if (soundfont_ != nullptr)
        soundfont_->panic();
    std::ignore = scheduler_.set_snapshot(nullptr, renderer_); // Clearing never fails.
    std::ignore = renderer_.bind(nullptr);                     // Same.
    playing_.store(false, std::memory_order_release);
    counting_in_.store(false, std::memory_order_relaxed);
    clock_playing_.store(false, std::memory_order_relaxed);
}

Status AudioEngine::publish(std::shared_ptr<const playback::PlaybackSnapshot> snapshot) {
    if (!snapshot)
        return make_error(ErrorCode::invalid_argument, "no snapshot to publish");
    if (!snapshot->live_playback_allowed)
        return make_error(ErrorCode::limit_exceeded, snapshot->live_refusal);
    if (sample_rate_ != 0 && snapshot->mapping.sample_rate() != sample_rate_)
        return make_error(ErrorCode::invalid_argument, "the snapshot was compiled for a different sample rate");
    if (snapshot->routes.size() > settings_.scheduler.route_capacity)
        return make_error(ErrorCode::limit_exceeded, "the snapshot has more routes than the engine supports");
    recent_.push_back(snapshot);
    while (recent_.size() > 8)
        recent_.pop_front();
    return handoff_.publish(std::move(snapshot));
}

std::size_t AudioEngine::collect_retired() { return handoff_.collect_retired(); }

Status AudioEngine::post(EngineCommand command) {
    if (!queues_->control.try_enqueue(command)) {
        control_dropped_.fetch_add(1, std::memory_order_relaxed);
        return make_error(ErrorCode::busy, "the engine control queue is full");
    }
    return ok();
}

bool AudioEngine::post_live(const playback::RealtimeMidiEvent& event) noexcept {
    if (queues_->live.try_enqueue(event))
        return true;
    live_dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool AudioEngine::post_monitor(const playback::RealtimeMidiEvent& event) noexcept {
    if (queues_->monitor.try_enqueue(event))
        return true;
    live_dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void AudioEngine::set_master_volume(float volume) noexcept {
    target_volume_.store(std::clamp(volume, 0.0f, 2.0f), std::memory_order_relaxed);
}

bool AudioEngine::wait_external(ExternalMidiEvent& event, std::chrono::microseconds timeout) {
    return queues_->external.wait_dequeue_timed(event, timeout);
}

void AudioEngine::set_external_latency(std::chrono::nanoseconds latency) noexcept {
    external_latency_ns_.store(latency.count(), std::memory_order_relaxed);
}

EngineTelemetry AudioEngine::telemetry() const noexcept {
    EngineTelemetry telemetry;
    telemetry.frames_rendered = frames_rendered_.load(std::memory_order_acquire);
    telemetry.callbacks = callbacks_.load(std::memory_order_relaxed);
    telemetry.deadline_misses = deadline_misses_.load(std::memory_order_relaxed);
    telemetry.control_dropped = control_dropped_.load(std::memory_order_relaxed);
    telemetry.live_dropped = live_dropped_.load(std::memory_order_relaxed);
    telemetry.external_dropped = external_dropped_.load(std::memory_order_relaxed);
    telemetry.snapshot_generation = snapshot_generation_.load(std::memory_order_relaxed);
    telemetry.song_frame = song_frame_.load(std::memory_order_relaxed);
    telemetry.position = position_.load(std::memory_order_relaxed);
    telemetry.epoch = epoch_.load(std::memory_order_relaxed);
    telemetry.run = clock_run_.load(std::memory_order_relaxed);
    telemetry.playing = playing_.load(std::memory_order_relaxed);
    telemetry.counting_in = counting_in_.load(std::memory_order_relaxed);
    telemetry.reached_end = reached_end_.load(std::memory_order_relaxed);
    return telemetry;
}

EngineClock AudioEngine::clock() const noexcept {
    EngineClock clock;
    for (;;) {
        const std::uint64_t before = clock_sequence_.load(std::memory_order_acquire);
        if ((before & 1u) != 0) {
            std::this_thread::yield();
            continue;
        }
        clock.steady_ns = clock_steady_ns_.load(std::memory_order_relaxed);
        clock.absolute_frame = clock_absolute_.load(std::memory_order_relaxed);
        clock.scheduler.anchor = clock_anchor_.load(std::memory_order_relaxed);
        clock.scheduler.iteration = clock_iteration_.load(std::memory_order_relaxed);
        clock.scheduler.passes = clock_passes_.load(std::memory_order_relaxed);
        clock.scheduler.looping = clock_looping_.load(std::memory_order_relaxed);
        clock.scheduler.playing = clock_playing_.load(std::memory_order_relaxed);
        clock.snapshot_generation = clock_generation_.load(std::memory_order_relaxed);
        clock.run = clock_run_.load(std::memory_order_relaxed);
        clock.counting_in = clock_counting_in_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (clock_sequence_.load(std::memory_order_relaxed) == before)
            return clock;
    }
}

std::optional<Tick> AudioEngine::transport_tick_at(std::int64_t steady_ns, std::uint64_t run) const {
    const EngineClock clock = this->clock();
    if (!clock.scheduler.playing || clock.run != run || sample_rate_ == 0)
        return std::nullopt;
    const playback::PlaybackSnapshot* snapshot = nullptr;
    for (const auto& candidate : recent_)
        if (candidate->snapshot_generation == clock.snapshot_generation)
            snapshot = candidate.get();
    if (snapshot == nullptr)
        return std::nullopt;
    // The frame being heard at `steady_ns` was rendered one output latency earlier.
    const __int128 elapsed_ns = static_cast<__int128>(steady_ns) - clock.steady_ns - output_latency_ns_;
    const auto absolute = clock.absolute_frame + static_cast<std::int64_t>(elapsed_ns * sample_rate_ / 1'000'000'000);
    return playback::linear_tick_at(*snapshot, clock.scheduler, absolute);
}

bool AudioEngine::device_lost() const noexcept { return output_ != nullptr && output_->device_lost(); }

void AudioEngine::handle_device_lost() noexcept { close(); }

void AudioEngine::apply_command(const EngineCommand& command) noexcept {
    switch (command.kind) {
    case EngineCommandKind::play:
        reached_end_.store(false, std::memory_order_relaxed);
        stop_pending_ = false;
        ++runs_;
        scheduler_.start(command.tick, renderer_, 0, command.count_in_bars);
        return;
    case EngineCommandKind::stop:
        scheduler_.stop(renderer_);
        return;
    case EngineCommandKind::seek:
        reached_end_.store(false, std::memory_order_relaxed);
        scheduler_.seek(command.tick, renderer_);
        return;
    case EngineCommandKind::panic:
        scheduler_.panic(renderer_);
        if (builtin_ != nullptr)
            builtin_->panic();
        if (soundfont_ != nullptr)
            soundfont_->panic();
        return;
    case EngineCommandKind::metronome_on:
        scheduler_.set_metronome(true);
        return;
    case EngineCommandKind::metronome_off:
        scheduler_.set_metronome(false);
        return;
    }
}

void AudioEngine::render_slice(float* output, std::uint32_t frames, std::int64_t slice_start_ns) noexcept {
    renderer_.begin(output, frames);
    forwarder_->begin_slice(slice_start_ns, sample_rate_);
    if (stop_pending_) {
        scheduler_.stop(renderer_);
        stop_pending_ = false;
        end_reached_in_callback_ = true; // Published after `playing` in render().
    }
    if (const playback::PlaybackSnapshot* next = handoff_.acquire_pending()) {
        if (scheduler_.set_snapshot(next, renderer_) && renderer_.bind(next)) {
            snapshot_generation_.store(next->snapshot_generation, std::memory_order_relaxed);
        } else {
            std::ignore = scheduler_.set_snapshot(nullptr, renderer_); // Stops and releases; never fails.
            std::ignore = renderer_.bind(nullptr);                     // Same.
        }
        handoff_.retire_previous();
    }
    // Commands run after the swap so a play posted right after publish() sees its snapshot.
    EngineCommand command;
    for (std::size_t count = 0; count < max_commands_per_block && queues_->control.try_dequeue(command); ++count)
        apply_command(command);
    playback::RealtimeMidiEvent live;
    for (std::size_t count = 0; count < max_live_events_per_block && queues_->live.try_dequeue(live); ++count) {
        live.frame_offset = 0;
        renderer_.dispatch_live(live);
    }
    for (std::size_t count = 0; count < max_live_events_per_block && queues_->monitor.try_dequeue(live); ++count) {
        live.frame_offset = 0;
        renderer_.dispatch_live(live);
    }
    const playback::BlockResult result = scheduler_.process_block(frames, renderer_);
    if (result.reached_end && scheduler_.playing())
        stop_pending_ = true;
    renderer_.finish();

    const float target = target_volume_.load(std::memory_order_relaxed);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        current_volume_ += (target - current_volume_) * volume_coefficient_;
        output[2 * frame] *= current_volume_;
        output[2 * frame + 1] *= current_volume_;
    }
    meter_.update(output, frames);
}

void AudioEngine::render(float* interleaved_stereo, std::uint32_t frame_count) noexcept {
    const std::int64_t start_ns = steady_now_ns();
    const std::int64_t start_frame = scheduler_.absolute_frame();
    std::uint32_t done = 0;
    while (done < frame_count) {
        const std::uint32_t slice = std::min(frame_count - done, settings_.max_block_frames);
        const std::int64_t slice_ns =
            start_ns + static_cast<std::int64_t>(done) * 1'000'000'000 / std::max<std::uint32_t>(sample_rate_, 1);
        render_slice(interleaved_stereo + static_cast<std::size_t>(done) * 2, slice, slice_ns);
        done += slice;
    }
    const std::int64_t elapsed = steady_now_ns() - start_ns;
    const std::int64_t budget =
        static_cast<std::int64_t>(frame_count) * 1'000'000'000 / std::max<std::uint32_t>(sample_rate_, 1);
    if (elapsed > budget)
        deadline_misses_.fetch_add(1, std::memory_order_relaxed);
    callbacks_.fetch_add(1, std::memory_order_relaxed);
    song_frame_.store(scheduler_.song_frame(), std::memory_order_relaxed);
    position_.store(scheduler_.position_tick(), std::memory_order_relaxed);
    epoch_.store(scheduler_.epoch(), std::memory_order_relaxed);
    counting_in_.store(scheduler_.in_count_in(), std::memory_order_relaxed);
    // Clock seqlock write (wait-free for the render thread).
    const playback::SchedulerClock scheduler_clock = scheduler_.clock();
    clock_sequence_.fetch_add(1, std::memory_order_acq_rel);
    clock_steady_ns_.store(start_ns, std::memory_order_relaxed);
    clock_absolute_.store(start_frame, std::memory_order_relaxed);
    clock_anchor_.store(scheduler_clock.anchor, std::memory_order_relaxed);
    clock_iteration_.store(scheduler_clock.iteration, std::memory_order_relaxed);
    clock_passes_.store(scheduler_clock.passes, std::memory_order_relaxed);
    clock_looping_.store(scheduler_clock.looping, std::memory_order_relaxed);
    clock_playing_.store(scheduler_clock.playing, std::memory_order_relaxed);
    clock_generation_.store(snapshot_generation_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    clock_run_.store(runs_, std::memory_order_relaxed);
    clock_counting_in_.store(scheduler_.in_count_in(), std::memory_order_relaxed);
    clock_sequence_.fetch_add(1, std::memory_order_release);
    playing_.store(scheduler_.playing(), std::memory_order_release);
    if (end_reached_in_callback_) {
        reached_end_.store(true, std::memory_order_release);
        end_reached_in_callback_ = false;
    }
    frames_rendered_.fetch_add(frame_count, std::memory_order_release);
}

} // namespace mwb::platform
