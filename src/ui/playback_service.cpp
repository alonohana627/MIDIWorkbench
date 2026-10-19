#include "mwb/ui/playback_service.hpp"

#include "mwb/audio/builtin_synth.hpp"
#include "mwb/audio/metronome.hpp"
#include "mwb/playback/snapshot.hpp"
#include "mwb/playback/transport.hpp"
#include "mwb/platform/audio_engine.hpp"
#include "mwb/platform/audio_output.hpp"
#include "mwb/platform/midi_backend.hpp"
#include "mwb/ui/task_thread.hpp"

#include <map>

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace mwb::ui {

namespace {

/// Channel reserved for auditioning (keyboard, editor scrub). Audition voices are sent as live
/// input to the built-in synth on this channel, so the document's channel state (program, bend,
/// sustain) is never changed by auditioning.
constexpr std::uint8_t audition_channel = 15;

/// Compiles playback snapshots on a dedicated task thread; only the newest request is kept
/// (older generations are obsolete and dropped, section 20.4).
class SnapshotCompiler {
public:
    using Delivery = std::function<void(std::shared_ptr<const playback::PlaybackSnapshot>, std::uint64_t, QString)>;

    explicit SnapshotCompiler(Delivery delivery) : delivery_(std::move(delivery)) {
        thread_ = std::jthread([this](std::stop_token stop) { loop(stop); });
    }
    ~SnapshotCompiler() {
        {
            std::lock_guard lock(mutex_);
            cancel_.request_stop(); // a compile in flight ends at its next check; closing never waits for it
        }
        thread_.request_stop();
        wake_.notify_all();
    }

    void request(std::shared_ptr<const DocumentData> document, playback::PlaybackOptions options, std::uint64_t generation) {
        {
            std::lock_guard lock(mutex_);
            pending_ = Request{std::move(document), options, generation};
            cancel_.request_stop(); // an in-flight older compile is cancelled
        }
        wake_.notify_all();
    }

private:
    struct Request {
        std::shared_ptr<const DocumentData> document;
        playback::PlaybackOptions options;
        std::uint64_t generation = 0;
    };

    void loop(std::stop_token stop) {
        while (!stop.stop_requested()) {
            Request request;
            std::stop_token cancel;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, stop, [this] { return pending_.has_value(); });
                if (stop.stop_requested())
                    return;
                request = std::move(*pending_);
                pending_.reset();
                cancel_ = std::stop_source{};
                cancel = cancel_.get_token();
            }
            // An exception (std::bad_alloc on a pathological document, or any other) must never
            // escape this std::jthread, which would call std::terminate and lose unsaved work: it
            // is delivered as a refused compile instead (round 5 finding 1).
            try {
                auto compiled = playback::compile_playback(*request.document, request.options, cancel);
                if (cancel.stop_requested())
                    continue;
                if (compiled)
                    delivery_(std::shared_ptr<const playback::PlaybackSnapshot>(std::move(*compiled)), request.generation, {});
                else
                    delivery_(nullptr, request.generation, QString::fromStdString(compiled.error().describe()));
            } catch (const std::exception& error) {
                deliver_failure(request.generation, QString::fromLocal8Bit(error.what()));
            } catch (...) {
                deliver_failure(request.generation, QObject::tr("unexpected error"));
            }
        }
    }

    void deliver_failure(std::uint64_t generation, const QString& reason) noexcept {
        try {
            delivery_(nullptr, generation, QObject::tr("could not prepare playback (%1)").arg(reason));
        } catch (...) {
            // Out of memory even for the message: the request is dropped, playback stays stopped.
        }
    }

    Delivery delivery_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<Request> pending_;
    std::stop_source cancel_;
    std::jthread thread_;
};

/// Forwards the engine's external-route events to MIDI output endpoints on its own thread
/// (section 14.1): the audio callback only enqueues; this router waits on the queue and hands
/// each message to the endpoint's output thread with its due time. Endpoints are opened by
/// stable identity; a missing endpoint is reported, never replaced by another device.
class ExternalMidiRouter {
public:
    explicit ExternalMidiRouter(platform::AudioEngine& engine) : engine_(engine) {
        thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    ~ExternalMidiRouter() { shutdown(); }

    /// Stops forwarding and sends what the (closed) engine still queued. Called while the engine
    /// is alive; afterwards the router never touches the engine again, so another thread may
    /// still hold (and finally destroy) it. Idempotent.
    void shutdown() {
        if (std::exchange(shut_down_, true))
            return;
        thread_.request_stop();
        if (thread_.joinable())
            thread_.join();
        // The engine was closed first and queued its owned note-offs and the panic messages;
        // send what is still queued now, immediately, and wait (bounded) until the outputs have
        // handed it to the drivers, so quitting never leaves a hardware synth with stuck notes.
        platform::ExternalMidiEvent event;
        std::lock_guard lock(mutex_);
        while (engine_.wait_external(event, std::chrono::microseconds(0)))
            if (const auto found = routes_.find(event.route_key); found != routes_.end())
                std::ignore = found->second->send(std::span<const std::uint8_t>(event.bytes.data(), event.size), 0);
        for (auto& [endpoint, output] : outputs_)
            std::ignore = output->flush(std::chrono::milliseconds(200)); // a stalled driver is not waited for longer
    }

    /// Maps route keys of a newly published snapshot to endpoints, opening outputs as needed.
    /// Returns a description of endpoints that could not be opened.
    QString update_routes(const playback::PlaybackSnapshot& snapshot) {
        std::map<std::uint64_t, std::string> wanted;
        for (const auto& route : snapshot.routes)
            if (route.output == OutputKind::external_midi && !route.endpoint.empty())
                wanted[route.key] = route.endpoint;
        QStringList problems;
        // Devices open on the snapshot compiler's thread and outside the send lock, so neither the
        // GUI nor the output thread waits for a slow driver (section 19.4).
        std::lock_guard update_lock(update_mutex_);
        if (!wanted.empty() && !backend_) {
            auto backend = platform::create_midi_backend();
            if (!backend)
                return QObject::tr("MIDI output unavailable: %1").arg(QString::fromStdString(backend.error().describe()));
            backend_ = std::move(*backend);
        }
        std::map<std::string, std::unique_ptr<platform::IMidiOutput>> opened_now;
        for (const auto& [key, endpoint] : wanted) {
            bool open = false;
            {
                std::lock_guard lock(mutex_);
                open = outputs_.contains(endpoint);
            }
            if (open || opened_now.contains(endpoint))
                continue;
            auto opened = backend_->open_output(endpoint);
            if (!opened) {
                problems << QString::fromStdString(endpoint + ": " + opened.error().describe());
                continue;
            }
            opened_now[endpoint] = std::move(*opened);
        }
        std::lock_guard lock(mutex_);
        for (auto& [endpoint, output] : opened_now)
            outputs_[endpoint] = std::move(output);
        // Routes are only added, never cleared: when a route disappears (mute, solo, channel or
        // endpoint change) the scheduler still releases its sounding notes through the old route
        // key, and those note-offs must reach the device. A key is a hash of (output, endpoint,
        // channel), so an old key can never point at the wrong endpoint, and the map is bounded by
        // the endpoints times 16 channels.
        for (const auto& [key, endpoint] : wanted)
            if (const auto found = outputs_.find(endpoint); found != outputs_.end())
                routes_[key] = found->second.get();
        return problems.join("; ");
    }

private:
    void run(std::stop_token stop) {
        platform::ExternalMidiEvent event;
        while (!stop.stop_requested()) {
            if (!engine_.wait_external(event, std::chrono::milliseconds(20)))
                continue;
            std::lock_guard lock(mutex_);
            const auto found = routes_.find(event.route_key);
            if (found == routes_.end())
                continue; // no open endpoint for this route: reported when routes were updated
            std::ignore = found->second->send(std::span<const std::uint8_t>(event.bytes.data(), event.size),
                                              event.due_steady_ns); // a full output queue is counted by the output
        }
    }

    platform::AudioEngine& engine_;
    std::mutex update_mutex_; ///< Serializes route updates (device opens).
    std::mutex mutex_;        ///< Guards routes for the output thread; never held while opening.
    std::unique_ptr<platform::IMidiBackend> backend_;
    std::map<std::string, std::unique_ptr<platform::IMidiOutput>> outputs_;
    std::map<std::uint64_t, platform::IMidiOutput*> routes_;
    bool shut_down_ = false; ///< Owner (GUI) thread only.
    std::jthread thread_;
};

/// Everything one open audio output needs, built off the GUI thread and handed over whole.
/// Members are destroyed bottom-up: the engine (which closes the running output) before the
/// output, the synths last.
struct OpenedDevice {
    std::unique_ptr<audio::BuiltinSynth> builtin;
    std::unique_ptr<audio::MetronomeSynth> metronome;
    std::unique_ptr<platform::IAudioOutput> output;
    std::unique_ptr<platform::AudioEngine> engine;
    std::vector<AudioDeviceChoice> devices;
    QString description; ///< Negotiated format, or why the device could not be opened.
};

/// Creates the synths and engine and opens the output (any thread; may take hundreds of ms).
OpenedDevice open_audio(const std::optional<std::string>& device_id) {
    OpenedDevice opened;
    auto builtin = audio::BuiltinSynth::create();
    auto engine = platform::AudioEngine::create();
    auto output = platform::create_audio_output();
    if (!builtin || !engine || !output) {
        opened.description = QObject::tr("No audio output: %1")
                                 .arg(QString::fromStdString(!builtin  ? builtin.error().describe()
                                                             : !engine ? engine.error().describe()
                                                                       : output.error().describe()));
        return opened;
    }
    if (auto devices = (*output)->enumerate_devices())
        for (const auto& device : *devices)
            opened.devices.push_back({device.id, QString::fromStdString(device.name), device.is_default});
    auto metronome = std::make_unique<audio::MetronomeSynth>();
    if (auto set = (*engine)->set_instruments(builtin->get(), nullptr, metronome.get(), builtin->get()); !set) {
        opened.description = QString::fromStdString(set.error().describe());
        return opened;
    }
    platform::AudioOutputConfig config;
    config.device_id = device_id;
    auto negotiated = (*engine)->open(**output, config);
    if (!negotiated) {
        opened.description =
            QObject::tr("Audio device unavailable: %1").arg(QString::fromStdString(negotiated.error().describe()));
        return opened;
    }
    // Report what the device actually gave us, never a requested value (section 13.2).
    opened.description = QObject::tr("%1 %2 · %3 Hz · %4 frames (%5 ms)")
                             .arg(QString::fromStdString(negotiated->backend))
                             .arg(QString::fromStdString(negotiated->device_name))
                             .arg(negotiated->sample_rate)
                             .arg(negotiated->period_frames)
                             .arg(negotiated->buffer_latency_ms(), 0, 'f', 1);
    opened.builtin = std::move(*builtin);
    opened.metronome = std::move(metronome);
    opened.output = std::move(*output);
    opened.engine = std::move(*engine);
    return opened;
}

/// PlaybackService over AudioEngine (miniaudio WASAPI or the null backend), the built-in synth
/// and the metronome.
class EnginePlaybackService final : public PlaybackService {
public:
    EnginePlaybackService(QObject* parent, std::optional<std::string> device_id)
        : PlaybackService(parent), device_id_(std::move(device_id)) {
        compiler_ = std::make_unique<SnapshotCompiler>(
            [guard = QPointer<EnginePlaybackService>(this), self = this](std::shared_ptr<const playback::PlaybackSnapshot> snapshot,
                                                           std::uint64_t generation, QString error) {
                // External MIDI routes (device opens) are updated here, on the compiler thread;
                // the snapshot is then delivered on the GUI thread, which owns the engine's publish side.
                QString problems;
                // `self` is valid here: the service destroys (and joins) the compiler first. The
                // router is copied under the lock and used without it, so a slow MIDI driver open
                // never blocks the GUI; a router replaced meanwhile was shut down first and no
                // longer uses the engine.
                if (snapshot) {
                    std::shared_ptr<ExternalMidiRouter> router;
                    {
                        std::lock_guard lock(self->router_mutex_);
                        router = self->router_;
                    }
                    if (router)
                        problems = router->update_routes(*snapshot);
                }
                QMetaObject::invokeMethod(
                    qApp, // context outlives the task; the lambda checks the guard
                    [guard, snapshot = std::move(snapshot), generation, error, problems]() mutable {
                        if (guard)
                            guard->deliver(std::move(snapshot), generation, error, problems);
                    },
                    Qt::QueuedConnection);
            });
        start_open();
        // 50 ms: also the latency of the CountingIn -> Recording transition and of the end of the song.
        retire_timer_.setInterval(50);
        connect(&retire_timer_, &QTimer::timeout, this, [this] {
            if (engine_)
                engine_->collect_retired(); // destroy retired snapshots off the render thread
            check_device();
        });
        retire_timer_.start();
    }

    ~EnginePlaybackService() override {
        retire_timer_.stop();
        device_thread_.stop(); // an open in progress finishes; its result is dropped by the guard
        compiler_.reset();
        close_device();
    }

    void publish_snapshot(std::shared_ptr<const DocumentData> document, std::size_t sequence_index,
                          std::uint64_t generation) override {
        if (!document || document->sequences.empty())
            return;
        // Serial numbers are unique per service, so two documents with equal generations never
        // satisfy each other's request.
        std::ignore = generation;
        last_sequence_index_ = std::min(sequence_index, document->sequences.size() - 1);
        last_document_ = std::move(document); // shared, never copied here
        republish(); // without an engine yet, the document is compiled once the device is open
    }

    void play(Tick from) override {
        if (!engine_ && !opening_)
            return;
        const auto command = transport_.state() == playback::TransportState::paused ? playback::TransportCommand::resume
                                                                                     : playback::TransportCommand::play;
        if (rolling())
            return;
        if (!transport_.apply(command))
            return;
        transport_.set_position(from);
        pending_play_from_ = from;
        pending_count_in_ = 0;
        if (engine_ && have_snapshot_ && delivered_generation_ == requested_generation_)
            start_engine(); // otherwise deliver() starts it once the current document is compiled
        emit transport_state_changed();
    }

    std::optional<Tick> record(Tick from, int count_in_bars) override {
        if (!engine_ && !opening_)
            return std::nullopt;
        const auto state = transport_.state();
        if (state == playback::TransportState::playing) {
            // Punch-in (section 13.3): capture starts where the transport is now.
            if (!transport_.apply(playback::TransportCommand::punch_in))
                return std::nullopt;
            const Tick at = pending_play_from_ ? *pending_play_from_
                                               : engine_ ? engine_->telemetry().position : transport_.position();
            transport_.set_position(at);
            recording_ = true;
            republish(); // playback now runs on past the song end
            emit transport_state_changed();
            return at;
        }
        if (state == playback::TransportState::paused) {
            std::ignore = transport_.apply(playback::TransportCommand::stop); // Paused -> Stopped is an edge
            if (engine_)
                std::ignore = engine_->post({platform::EngineCommandKind::stop, from});
        }
        const bool count_in = count_in_bars > 0;
        if (!transport_.apply(count_in ? playback::TransportCommand::record_with_count_in : playback::TransportCommand::record))
            return std::nullopt;
        transport_.set_position(from);
        recording_ = true;
        pending_play_from_ = from;
        pending_count_in_ = static_cast<std::uint32_t>(std::clamp(count_in_bars, 0, 16));
        republish(); // deliver() starts the engine once this compile (with the extended end) lands
        emit transport_state_changed();
        return from;
    }

    void punch_out() override {
        if (transport_.state() != playback::TransportState::recording)
            return;
        std::ignore = transport_.apply(playback::TransportCommand::punch_out); // Recording -> Playing is an edge
        recording_ = false;
        republish(); // back to the song's own end (the take's extended end is no longer needed)
        emit transport_state_changed();
    }

    [[nodiscard]] std::optional<Tick> transport_tick_at(std::int64_t steady_ns) const override {
        if (!engine_ || pending_play_from_ || !rolling() || current_run_ == 0)
            return std::nullopt;
        return engine_->transport_tick_at(steady_ns, current_run_);
    }

    [[nodiscard]] std::optional<TimeRange> loop() const override { return loop_; }
    [[nodiscard]] Rational tempo_multiplier() const override { return tempo_multiplier_; }

    void pause() override {
        if (!transport_.apply(playback::TransportCommand::pause))
            return;
        paused_at_ = engine_ && !pending_play_from_ ? engine_->telemetry().position : transport_.position();
        pending_play_from_.reset();
        if (engine_)
            std::ignore = engine_->post({platform::EngineCommandKind::stop, paused_at_}); // releases owned notes
        emit transport_state_changed();
    }

    void stop(Tick return_to) override {
        // CountingIn leaves through Cancel, every other rolling state through Stop; an already
        // stopped (or device-lost) transport refuses both, which is fine.
        std::ignore = transport_.apply(transport_.state() == playback::TransportState::counting_in
                                           ? playback::TransportCommand::cancel
                                           : playback::TransportCommand::stop);
        pending_play_from_.reset();
        if (std::exchange(recording_, false))
            republish(); // the next Play stops at the song end again, also when the take was refused
        if (engine_)
            std::ignore = engine_->post({platform::EngineCommandKind::stop, return_to});
        paused_at_ = return_to;
        transport_.set_position(return_to);
        emit transport_state_changed();
    }

    void seek(Tick tick) override {
        if (!transport_.seek(tick))
            return;
        if (pending_play_from_)
            pending_play_from_ = tick; // not started yet: start there
        else if (engine_)
            std::ignore = engine_->post({platform::EngineCommandKind::seek, tick});
    }

    void set_loop(std::optional<TimeRange> loop) override { loop_ = loop; }

    void set_metronome(bool enabled) override {
        metronome_on_ = enabled;
        if (engine_)
            std::ignore = engine_->post({enabled ? platform::EngineCommandKind::metronome_on
                                                 : platform::EngineCommandKind::metronome_off,
                                         0});
    }

    void set_tempo_multiplier(double multiplier) override {
        const Rational speed = Rational::make(static_cast<std::int64_t>(std::llround(multiplier * 1000)), 1000)
                                   .value_or(Rational{1, 1});
        if (speed == tempo_multiplier_)
            return;
        tempo_multiplier_ = speed;
        // Heard at once: the scheduler adopts the recompiled snapshot at the same musical
        // position (code-uiA-2); a stopped transport compiles it at the next play.
        if (rolling())
            republish();
    }

    void audition_note_on(int pitch, int velocity, int, const std::string& synth_preset) override {
        if (!engine_ || !builtin_)
            return;
        builtin_->set_audition_timbre(audio::parse_timbre(synth_preset)); // private channel: never a document channel
        std::ignore = engine_->post_live(playback::RealtimeMidiEvent::short_message(
            static_cast<std::uint8_t>(0x90 | audition_channel), static_cast<std::uint8_t>(pitch),
            static_cast<std::uint8_t>(std::clamp(velocity, 1, 127)))); // a full queue only drops this audition note
    }

    void audition_note_off(int pitch, int) override {
        if (!engine_)
            return;
        std::ignore = engine_->post_live(playback::RealtimeMidiEvent::short_message(
            static_cast<std::uint8_t>(0x80 | audition_channel), static_cast<std::uint8_t>(pitch), 0));
    }

    void set_live_timbre(const std::string& synth_preset) override {
        live_timbre_ = synth_preset;
        if (builtin_)
            builtin_->set_audition_timbre(audio::parse_timbre(synth_preset));
    }

    void monitor_message(std::span<const std::uint8_t> message) override {
        if (message.size() < 2 || message.size() > 3)
            return;
        const std::uint8_t kind = message[0] & 0xF0;
        // Notes, poly pressure, controllers (sustain, modulation, ...), channel pressure and bend;
        // program changes would only fight the track's timbre and are not monitored.
        if (kind < 0x80 || kind == 0xC0 || kind > 0xE0)
            return;
        const auto status = static_cast<std::uint8_t>(kind | audition_channel);
        const auto event = message.size() == 3
                               ? playback::RealtimeMidiEvent::short_message(status, message[1], message[2])
                               : playback::RealtimeMidiEvent::short_message(status, message[1], 0);
        // The lock only guards against a device reopen swapping the engine on the GUI thread; the
        // monitor queue itself has this thread as its single producer.
        std::lock_guard lock(live_mutex_);
        if (engine_)
            std::ignore = engine_->post_monitor(event); // a full queue drops this one message
    }

    void panic(bool broad) override {
        if (!engine_)
            return;
        std::ignore = engine_->post({platform::EngineCommandKind::panic, 0});
        // Releases audition notes on the private channel too.
        std::ignore = engine_->post_live(playback::RealtimeMidiEvent::short_message(
            static_cast<std::uint8_t>(0xB0 | audition_channel), broad ? 120 : 123, 0));
        std::ignore = engine_->post_live(playback::RealtimeMidiEvent::short_message(
            static_cast<std::uint8_t>(0xB0 | audition_channel), 64, 0));
    }

    [[nodiscard]] TransportDisplay display() const override {
        TransportDisplay display;
        switch (transport_.state()) {
        case playback::TransportState::stopped: display.state = TransportDisplay::State::stopped; break;
        case playback::TransportState::playing: display.state = TransportDisplay::State::playing; break;
        case playback::TransportState::paused: display.state = TransportDisplay::State::paused; break;
        case playback::TransportState::counting_in: display.state = TransportDisplay::State::counting_in; break;
        case playback::TransportState::recording: display.state = TransportDisplay::State::recording; break;
        case playback::TransportState::device_lost: display.state = TransportDisplay::State::device_lost; break;
        }
        if (lost_ && !rolling())
            display.state = TransportDisplay::State::device_lost;
        display.device_description = device_description_;
        display.opening = opening_;
        display.playing_older_generation = requested_generation_ != delivered_generation_;
        if (!engine_ || pending_play_from_) {
            display.position = rolling() ? transport_.position() : paused_at_;
            return display;
        }
        const auto telemetry = engine_->telemetry();
        display.position = rolling() ? telemetry.position : paused_at_;
        display.snapshot_generation = telemetry.snapshot_generation;
        display.underflows = telemetry.deadline_misses;
        return display;
    }

    [[nodiscard]] TransportMeters take_meters() override {
        TransportMeters meters;
        if (!engine_)
            return meters;
        const auto peaks = engine_->meter().take();
        meters.peak_left = peaks.left;
        meters.peak_right = peaks.right;
        meters.clipped = peaks.clipped_samples > 0;
        return meters;
    }

    [[nodiscard]] bool has_device() const override { return engine_ && !engine_->device_lost(); }

    void reopen_device(std::optional<std::string> device_id) override {
        device_id_ = std::move(device_id);
        if (opening_) {
            reopen_again_ = true; // the newest choice wins once the running open finishes
            return;
        }
        // A rolling transport stops: the new device starts from a clean state (DeviceLost stays
        // until the open succeeds, then Reconfigure leads back to Stopped).
        if (rolling() || transport_.state() == playback::TransportState::paused)
            stop(transport_.position());
        close_device();
        start_open();
    }

    [[nodiscard]] std::vector<AudioDeviceChoice> audio_devices() const override { return devices_; }

    void simulate_device_loss() override {
        if (auto* null_output = dynamic_cast<platform::NullAudioOutput*>(output_.get()))
            null_output->simulate_device_loss();
    }

private:
    void start_open() {
        opening_ = true;
        last_open_attempt_ = std::chrono::steady_clock::now();
        device_description_ = tr("Opening the audio output…");
        emit device_changed();
        std::ignore = device_thread_.post([guard = QPointer<EnginePlaybackService>(this), id = device_id_](std::stop_token) {
            auto opened = std::make_shared<OpenedDevice>(open_audio(id));
            QMetaObject::invokeMethod(
                qApp,
                [guard, opened] {
                    if (guard)
                        guard->install(std::move(*opened));
                    // Otherwise `opened` closes and destroys the device here, on the GUI thread.
                },
                Qt::QueuedConnection);
        });
    }

    /// GUI thread: takes over a device opened in the background.
    void install(OpenedDevice opened) {
        opening_ = false;
        device_description_ = opened.description;
        if (!opened.devices.empty())
            devices_ = std::move(opened.devices);
        if (opened.engine) {
            {
                std::lock_guard lock(live_mutex_);
                builtin_ = std::move(opened.builtin);
                metronome_ = std::move(opened.metronome);
                output_ = std::move(opened.output);
                engine_ = std::move(opened.engine);
            }
            {
                std::lock_guard lock(router_mutex_);
                router_ = std::make_shared<ExternalMidiRouter>(*engine_);
            }
            if (!live_timbre_.empty())
                builtin_->set_audition_timbre(audio::parse_timbre(live_timbre_));
            if (metronome_on_)
                std::ignore = engine_->post({platform::EngineCommandKind::metronome_on, 0});
            // A new engine counts its runs from zero and has no snapshot yet; compiles requested
            // for the previous engine (another sample rate) are ignored by deliver().
            current_run_ = 0;
            have_snapshot_ = false;
            first_generation_ = publish_serial_ + 1;
            if (transport_.state() == playback::TransportState::device_lost)
                std::ignore = transport_.apply(playback::TransportCommand::reconfigure); // DeviceLost -> Stopped
            lost_ = false;
            republish(); // also starts a play or record requested while the device was opening
        } else if (pending_play_from_) {
            // Nothing to play on: requests made while opening are dropped.
            stop(*pending_play_from_);
        }
        emit device_changed();
        emit transport_state_changed();
        if (std::exchange(reopen_again_, false))
            reopen_device(device_id_);
    }

    /// GUI thread: closes and destroys the current device stack. The engine closes first (it
    /// releases owned notes and panics every used route into the external queue), then the
    /// router sends what is queued, then the engine and the output go.
    void close_device() {
        if (engine_)
            engine_->close();
        std::shared_ptr<ExternalMidiRouter> router;
        {
            std::lock_guard lock(router_mutex_);
            router = std::move(router_);
        }
        if (router)
            router->shutdown(); // drains the engine's queue now, while the engine still exists
        router.reset();
        std::lock_guard lock(live_mutex_);
        engine_.reset();
        output_.reset();
        metronome_.reset();
        builtin_.reset();
        have_snapshot_ = false;
    }

    void check_device() {
        if (engine_ && rolling() && !pending_play_from_) {
            const auto telemetry = engine_->telemetry();
            // The engine reports which play run it is in, so state from an earlier run is ignored.
            const bool this_run = telemetry.run == current_run_;
            if (transport_.state() == playback::TransportState::counting_in && this_run && telemetry.playing &&
                !telemetry.counting_in) {
                std::ignore = transport_.apply(playback::TransportCommand::count_in_complete); // CountingIn -> Recording
                emit transport_state_changed();
            } else if (this_run && telemetry.reached_end && !telemetry.playing) {
                std::ignore = transport_.apply(transport_.state() == playback::TransportState::counting_in
                                                   ? playback::TransportCommand::cancel
                                                   : playback::TransportCommand::stop); // end of song
                recording_ = false;
                paused_at_ = 0;
                emit transport_state_changed();
            }
        }
        // While the device is lost, try the chosen output again every 2 s (it may be plugged back).
        if (lost_ && !opening_ &&
            std::chrono::steady_clock::now() - last_open_attempt_ >= std::chrono::seconds(2)) {
            close_device();
            start_open();
            return;
        }
        if (!engine_ || !engine_->device_lost())
            return;
        const Tick position = rolling() && !pending_play_from_ ? engine_->telemetry().position : transport_.position();
        engine_->handle_device_lost(); // voices released
        close_device();                // nothing can play until an output is open again
        lost_ = true;
        // Playing/Recording -> DeviceLost; CountingIn is cancelled; a stopped or paused transport
        // stays where it is (the state machine has no DeviceFailure edge there) and shows the loss.
        if (transport_.state() == playback::TransportState::counting_in)
            std::ignore = transport_.apply(playback::TransportCommand::cancel);
        else if (rolling())
            std::ignore = transport_.apply(playback::TransportCommand::device_failure);
        recording_ = false;
        pending_play_from_.reset();
        paused_at_ = position;
        transport_.set_position(position);
        last_open_attempt_ = std::chrono::steady_clock::now();
        device_description_ = tr("Audio output lost; reconnecting automatically, or choose an output in Transport > Audio Output");
        emit device_lost(device_description_);
        emit transport_state_changed();
    }

    void deliver(std::shared_ptr<const playback::PlaybackSnapshot> snapshot, std::uint64_t generation, const QString& error,
                 const QString& problems) {
        if (!engine_ || generation < first_generation_)
            return; // compiled for a previous (or no) device
        if (!snapshot) {
            refuse(error, generation);
            return;
        }
        if (!problems.isEmpty())
            device_description_ = tr("External MIDI endpoint unavailable: %1").arg(problems);
        if (auto published = engine_->publish(std::move(snapshot)); !published) {
            refuse(QString::fromStdString(published.error().describe()), generation);
            return;
        }
        delivered_generation_ = generation;
        have_snapshot_ = true;
        if (pending_play_from_ && rolling() && delivered_generation_ == requested_generation_)
            start_engine();
    }

    /// The current document cannot be played: a waiting Play or Record ends (the transport does
    /// not claim Playing with a frozen playhead) and the reason is reported.
    void refuse(const QString& reason, std::uint64_t generation) {
        device_description_ = tr("Playback refused: %1").arg(reason);
        if (generation == requested_generation_)
            delivered_generation_ = generation; // no newer edit is "about to play"
        if (pending_play_from_ && rolling())
            stop(*pending_play_from_);
        emit playback_refused(device_description_);
    }

    void start_engine() {
        if (!pending_play_from_)
            return;
        // Each applied play command starts a new engine run; counting the posted ones tells
        // transport_tick_at() which run is ours (a full queue only happens under a flood).
        if (engine_->post({platform::EngineCommandKind::play, *pending_play_from_, pending_count_in_}))
            ++current_run_;
        pending_play_from_.reset();
        pending_count_in_ = 0;
    }

    /// Playing, CountingIn or Recording.
    [[nodiscard]] bool rolling() const {
        const auto state = transport_.state();
        return state == playback::TransportState::playing || state == playback::TransportState::counting_in ||
               state == playback::TransportState::recording;
    }

    /// Compiles the last published document again with the current session settings.
    void republish() {
        if (!engine_ || !last_document_)
            return;
        requested_generation_ = ++publish_serial_;
        const Sequence& sequence = last_document_->sequences[last_sequence_index_];
        playback::PlaybackOptions options;
        options.sequence = sequence.id;
        options.sample_rate = engine_->sample_rate();
        options.session.loop = loop_;
        options.session.tempo_multiplier = tempo_multiplier_;
        if (recording_) {
            // A take may run past the end of the song: keep the transport (and the click) going
            // for 2000 bars of 4/4 after the current position instead of stopping at the end.
            const Tick ppq = sequence.division.is_ppq() ? sequence.division.ppq : default_ppq;
            options.minimum_end_tick = std::max<Tick>(transport_.position(), 0) + 2000 * 4 * ppq;
        }
        compiler_->request(last_document_, options, requested_generation_);
    }

    std::unique_ptr<SnapshotCompiler> compiler_;
    std::mutex live_mutex_; ///< Held by the GUI while it swaps the engine, and by monitor_message().
    std::unique_ptr<audio::BuiltinSynth> builtin_;
    std::unique_ptr<audio::MetronomeSynth> metronome_;
    std::unique_ptr<platform::IAudioOutput> output_;
    std::unique_ptr<platform::AudioEngine> engine_; ///< Destroyed before output_ (it closes the stream).
    std::mutex router_mutex_; ///< Held while the compiler thread updates routes and while the router is replaced.
    std::shared_ptr<ExternalMidiRouter> router_;
    playback::Transport transport_;
    QTimer retire_timer_;
    QString device_description_;
    std::optional<std::string> device_id_;  ///< Output to open (nullopt: the default).
    std::vector<AudioDeviceChoice> devices_;
    bool opening_ = false;
    bool lost_ = false;          ///< The output was lost and no new one is open yet.
    bool reopen_again_ = false;
    std::chrono::steady_clock::time_point last_open_attempt_{};
    std::string live_timbre_;
    bool metronome_on_ = false;
    std::optional<TimeRange> loop_;
    Rational tempo_multiplier_{1, 1};
    std::shared_ptr<const DocumentData> last_document_; ///< What publish_document() last asked for.
    std::size_t last_sequence_index_ = 0;
    std::optional<Tick> pending_play_from_;
    std::uint32_t pending_count_in_ = 0;  ///< Count-in bars of the pending play command.
    std::uint64_t current_run_ = 0;       ///< Play commands posted (= the engine run they start).
    bool recording_ = false;              ///< A take is running (snapshots extend past the song end).
    Tick paused_at_ = 0;
    std::uint64_t publish_serial_ = 0;
    std::uint64_t requested_generation_ = 0;
    std::uint64_t delivered_generation_ = 0;
    std::uint64_t first_generation_ = 1;  ///< First compile request made for the current engine.
    bool have_snapshot_ = false;
    TaskThread device_thread_;            ///< Opens devices (last member: stopped first in the destructor body).
};

} // namespace

std::unique_ptr<PlaybackService> create_playback_service(QObject* parent, std::optional<std::string> device_id) {
    return std::make_unique<EnginePlaybackService>(parent, std::move(device_id));
}

} // namespace mwb::ui
