#include "mwb/ui/playback_service.hpp"

#include "mwb/audio/builtin_synth.hpp"
#include "mwb/audio/metronome.hpp"
#include "mwb/playback/snapshot.hpp"
#include "mwb/playback/transport.hpp"
#include "mwb/platform/audio_engine.hpp"
#include "mwb/platform/audio_output.hpp"
#include "mwb/platform/midi_backend.hpp"

#include <map>

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <condition_variable>
#include <mutex>
#include <thread>

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
            auto compiled = playback::compile_playback(*request.document, request.options, cancel);
            if (cancel.stop_requested())
                continue;
            if (compiled)
                delivery_(std::shared_ptr<const playback::PlaybackSnapshot>(std::move(*compiled)), request.generation, {});
            else
                delivery_(nullptr, request.generation, QString::fromStdString(compiled.error().describe()));
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
    ~ExternalMidiRouter() {
        thread_.request_stop();
        if (thread_.joinable())
            thread_.join();
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
        routes_.clear();
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
    std::jthread thread_;
};

/// PlaybackService over AudioEngine (miniaudio WASAPI or the null backend), the built-in synth
/// and the metronome.
class EnginePlaybackService final : public PlaybackService {
public:
    explicit EnginePlaybackService(QObject* parent) : PlaybackService(parent) {
        compiler_ = std::make_unique<SnapshotCompiler>(
            [guard = QPointer<EnginePlaybackService>(this), self = this](std::shared_ptr<const playback::PlaybackSnapshot> snapshot,
                                                           std::uint64_t generation, QString error) {
                // External MIDI routes (device opens) are updated here, on the compiler thread;
                // the snapshot is then delivered on the GUI thread, which owns the engine's publish side.
                QString problems;
                // `self` is valid here: the service destroys (and joins) the compiler first.
                if (snapshot)
                    if (auto router = self->router())
                        problems = router->update_routes(*snapshot);
                QMetaObject::invokeMethod(
                    qApp, // context outlives the task; the lambda checks the guard
                    [guard, snapshot = std::move(snapshot), generation, error, problems]() mutable {
                        if (guard)
                            guard->deliver(std::move(snapshot), generation, error, problems);
                    },
                    Qt::QueuedConnection);
            });
        open_device();
        retire_timer_.setInterval(250);
        connect(&retire_timer_, &QTimer::timeout, this, [this] {
            if (engine_)
                engine_->collect_retired(); // destroy retired snapshots off the render thread
            check_device();
        });
        retire_timer_.start();
    }

    ~EnginePlaybackService() override {
        retire_timer_.stop();
        compiler_.reset();
        // Panic and close the engine first, so the note-offs it queues still reach external
        // MIDI through the router; only then stop the router thread.
        if (engine_) {
            std::ignore = engine_->post({platform::EngineCommandKind::panic, 0}); // no stuck notes on shutdown
            engine_->close();
        }
        {
            std::lock_guard lock(router_mutex_);
            router_.reset();
        }
    }

    void publish_document(const DocumentData& document, std::size_t sequence_index, std::uint64_t generation) override {
        if (!engine_ || document.sequences.empty())
            return;
        // Serial numbers are unique per service, so two documents with equal generations never
        // satisfy each other's request.
        std::ignore = generation;
        requested_generation_ = ++publish_serial_;
        playback::PlaybackOptions options;
        options.sequence = document.sequences[std::min(sequence_index, document.sequences.size() - 1)].id;
        options.sample_rate = engine_->sample_rate();
        options.session.loop = loop_;
        options.session.tempo_multiplier = tempo_multiplier_;
        compiler_->request(std::make_shared<const DocumentData>(document), options, requested_generation_);
    }

    void play(Tick from) override {
        if (!engine_)
            return;
        const auto command = transport_.state() == playback::TransportState::paused ? playback::TransportCommand::resume
                                                                                     : playback::TransportCommand::play;
        if (transport_.state() == playback::TransportState::playing)
            return;
        if (!transport_.apply(command))
            return;
        transport_.set_position(from);
        pending_play_from_ = from;
        if (have_snapshot_ && delivered_generation_ == requested_generation_)
            start_engine(); // otherwise deliver() starts it once the current document is compiled
        emit transport_state_changed();
    }

    void pause() override {
        if (!engine_ || !transport_.apply(playback::TransportCommand::pause))
            return;
        paused_at_ = engine_->telemetry().position;
        std::ignore = engine_->post({platform::EngineCommandKind::stop, paused_at_}); // releases owned notes
        emit transport_state_changed();
    }

    void stop(Tick return_to) override {
        if (!engine_)
            return;
        std::ignore = transport_.apply(playback::TransportCommand::stop); // already stopped is fine
        pending_play_from_.reset();
        std::ignore = engine_->post({platform::EngineCommandKind::stop, return_to});
        paused_at_ = return_to;
        transport_.set_position(return_to);
        emit transport_state_changed();
    }

    void seek(Tick tick) override {
        if (!engine_ || !transport_.seek(tick))
            return;
        std::ignore = engine_->post({platform::EngineCommandKind::seek, tick});
    }

    void set_loop(std::optional<TimeRange> loop) override { loop_ = loop; }

    void set_metronome(bool enabled) override {
        if (engine_)
            std::ignore = engine_->post({enabled ? platform::EngineCommandKind::metronome_on
                                                 : platform::EngineCommandKind::metronome_off,
                                         0});
    }

    void set_tempo_multiplier(double multiplier) override {
        tempo_multiplier_ = Rational::make(static_cast<std::int64_t>(std::llround(multiplier * 1000)), 1000)
                                .value_or(Rational{1, 1});
    }

    void audition_note_on(int pitch, int velocity, int, const std::string& synth_preset) override {
        if (!engine_ || !builtin_)
            return;
        builtin_->set_channel_timbre(audition_channel, audio::parse_timbre(synth_preset));
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
        display.device_description = device_description_;
        if (!engine_)
            return display;
        const auto telemetry = engine_->telemetry();
        display.position = transport_.state() == playback::TransportState::playing ? telemetry.position : paused_at_;
        display.snapshot_generation = telemetry.snapshot_generation;
        display.playing_older_generation = requested_generation_ != delivered_generation_;
        const auto peaks = engine_->meter().take();
        display.peak_left = peaks.left;
        display.peak_right = peaks.right;
        display.clipped = peaks.clipped_samples > 0;
        display.underflows = telemetry.deadline_misses;
        return display;
    }

    [[nodiscard]] bool has_device() const override { return engine_ && !engine_->device_lost(); }

private:
    void open_device() {
        auto builtin = audio::BuiltinSynth::create();
        auto engine = platform::AudioEngine::create();
        auto output = platform::create_audio_output();
        if (!builtin || !engine || !output) {
            device_description_ = tr("No audio output: %1")
                                      .arg(QString::fromStdString(!builtin  ? builtin.error().describe()
                                                                  : !engine ? engine.error().describe()
                                                                            : output.error().describe()));
            return;
        }
        builtin_ = std::move(*builtin);
        metronome_ = std::make_unique<audio::MetronomeSynth>();
        engine_ = std::move(*engine);
        output_ = std::move(*output);
        if (auto set = engine_->set_instruments(builtin_.get(), nullptr, metronome_.get(), builtin_.get()); !set) {
            device_description_ = QString::fromStdString(set.error().describe());
            engine_.reset();
            return;
        }
        auto negotiated = engine_->open(*output_, {});
        if (!negotiated) {
            device_description_ = tr("Audio device unavailable: %1").arg(QString::fromStdString(negotiated.error().describe()));
            engine_.reset();
            return;
        }
        {
            std::lock_guard lock(router_mutex_);
            router_ = std::make_shared<ExternalMidiRouter>(*engine_);
        }
        // Report what the device actually gave us, never a requested value (section 13.2).
        device_description_ = tr("%1 %2 · %3 Hz · %4 frames (%5 ms)")
                                  .arg(QString::fromStdString(negotiated->backend))
                                  .arg(QString::fromStdString(negotiated->device_name))
                                  .arg(negotiated->sample_rate)
                                  .arg(negotiated->period_frames)
                                  .arg(negotiated->buffer_latency_ms(), 0, 'f', 1);
    }

    void check_device() {
        if (engine_ && transport_.state() == playback::TransportState::playing) {
            const auto telemetry = engine_->telemetry();
            if (telemetry.reached_end && !telemetry.playing && !pending_play_from_) {
                std::ignore = transport_.apply(playback::TransportCommand::stop); // end of song
                paused_at_ = 0;
                emit transport_state_changed();
            }
        }
        if (!engine_ || !engine_->device_lost() || transport_.state() == playback::TransportState::device_lost)
            return;
        engine_->handle_device_lost(); // voices released, position kept
        std::ignore = transport_.apply(playback::TransportCommand::device_failure);
        device_description_ = tr("Audio device lost; choose another output and restart playback");
        emit device_lost(device_description_);
        emit transport_state_changed();
    }

    [[nodiscard]] std::shared_ptr<ExternalMidiRouter> router() {
        std::lock_guard lock(router_mutex_);
        return router_;
    }

    void deliver(std::shared_ptr<const playback::PlaybackSnapshot> snapshot, std::uint64_t generation, const QString& error,
                 const QString& problems) {
        if (!engine_)
            return;
        if (!snapshot) {
            device_description_ = tr("Playback refused: %1").arg(error);
            return;
        }
        if (!problems.isEmpty())
            device_description_ = tr("External MIDI endpoint unavailable: %1").arg(problems);
        if (auto published = engine_->publish(std::move(snapshot)); !published) {
            device_description_ = tr("Playback refused: %1").arg(QString::fromStdString(published.error().describe()));
            return;
        }
        delivered_generation_ = generation;
        have_snapshot_ = true;
        if (pending_play_from_ && transport_.state() == playback::TransportState::playing &&
            delivered_generation_ == requested_generation_)
            start_engine();
    }

    void start_engine() {
        if (!pending_play_from_)
            return;
        std::ignore = engine_->post({platform::EngineCommandKind::play, *pending_play_from_}); // queue full only under flood
        pending_play_from_.reset();
    }

    std::unique_ptr<SnapshotCompiler> compiler_;
    std::unique_ptr<audio::BuiltinSynth> builtin_;
    std::unique_ptr<audio::MetronomeSynth> metronome_;
    std::unique_ptr<platform::AudioEngine> engine_;
    std::unique_ptr<platform::IAudioOutput> output_;
    std::mutex router_mutex_;
    std::shared_ptr<ExternalMidiRouter> router_;
    playback::Transport transport_;
    QTimer retire_timer_;
    QString device_description_;
    std::optional<TimeRange> loop_;
    Rational tempo_multiplier_{1, 1};
    std::optional<Tick> pending_play_from_;
    Tick paused_at_ = 0;
    std::uint64_t publish_serial_ = 0;
    std::uint64_t requested_generation_ = 0;
    std::uint64_t delivered_generation_ = 0;
    bool have_snapshot_ = false;
};

} // namespace

std::unique_ptr<PlaybackService> create_playback_service(QObject* parent) {
    return std::make_unique<EnginePlaybackService>(parent);
}

} // namespace mwb::ui
