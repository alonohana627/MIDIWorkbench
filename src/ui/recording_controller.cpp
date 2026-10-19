#include "mwb/ui/recording_controller.hpp"

#include "mwb/document/recovery.hpp"
#include "mwb/platform/midi_backend.hpp"
#include "mwb/ui/document_controller.hpp"
#include "mwb/ui/keyboard_widget.hpp"
#include "mwb/ui/playback_service.hpp"

#include <QCoreApplication>
#include <QMetaObject>

#include <array>
#include <atomic>
#include <limits>
#include <mutex>

namespace mwb::ui {

namespace {

std::int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

/// What the monitor thread hands to the GUI thread.
struct RecordingController::Inbox {
    std::mutex mutex;
    std::vector<app::CapturedMessage> messages;
    std::atomic<bool> monitoring{true};
};

/// The MIDI backend lives on the device thread only (enumeration and opens can block).
struct RecordingController::Devices {
    std::unique_ptr<platform::IMidiBackend> backend;
};

RecordingController::RecordingController(PlaybackService& playback, QObject* parent)
    : QObject(parent), playback_(playback), inbox_(std::make_shared<Inbox>()), devices_(std::make_shared<Devices>()) {
    poll_timer_.setInterval(4); // only recording, step entry and the key display: monitoring does not wait for it
    connect(&poll_timer_, &QTimer::timeout, this, &RecordingController::poll_input);
    connect(&playback_, &PlaybackService::transport_state_changed, this, &RecordingController::transport_changed);
}

RecordingController::~RecordingController() {
    device_thread_.stop(); // an open in progress finishes; its result is dropped by the guard
    stop_monitor();
}

void RecordingController::list_input_ports(std::function<void(PortList, QString)> done) {
    std::ignore = device_thread_.post([guard = QPointer<RecordingController>(this), devices = devices_,
                                       done = std::move(done)](std::stop_token) {
        PortList ports;
        QString error;
        if (!devices->backend) {
            if (auto backend = platform::create_midi_backend())
                devices->backend = std::move(*backend);
            else
                error = tr("MIDI input unavailable: %1").arg(QString::fromStdString(backend.error().describe()));
        }
        if (devices->backend) {
            if (auto inputs = devices->backend->enumerate_inputs())
                for (const auto& port : *inputs)
                    ports.emplace_back(QString::fromStdString(port.stable_id), QString::fromStdString(port.display_name));
            else
                error = QString::fromStdString(inputs.error().describe());
        }
        QMetaObject::invokeMethod(
            qApp,
            [guard, done, ports = std::move(ports), error] {
                if (guard)
                    done(ports, error);
            },
            Qt::QueuedConnection);
    });
}

void RecordingController::open_input(const QString& stable_id) {
    poll_timer_.stop();
    stop_monitor();
    input_.reset();
    input_name_.clear();
    if (stable_id.isEmpty()) {
        emit input_changed(QString(), QString());
        return;
    }
    // Opened by stable identity on the device thread; a missing or busy endpoint is reported,
    // never substituted.
    std::ignore = device_thread_.post([guard = QPointer<RecordingController>(this), devices = devices_,
                                       stable_id](std::stop_token) {
        std::shared_ptr<platform::IMidiInput> input;
        QString error;
        if (!devices->backend) {
            if (auto backend = platform::create_midi_backend())
                devices->backend = std::move(*backend);
            else
                error = QString::fromStdString(backend.error().describe());
        }
        if (devices->backend) {
            if (auto opened = devices->backend->open_input(stable_id.toStdString()))
                input = std::move(*opened);
            else
                error = QString::fromStdString(opened.error().describe());
        }
        QMetaObject::invokeMethod(
            qApp,
            [guard, input, stable_id, error] {
                if (guard)
                    guard->install_input(input, stable_id, error);
            },
            Qt::QueuedConnection);
    });
}

void RecordingController::install_input(std::shared_ptr<platform::IMidiInput> input, const QString& stable_id,
                                        const QString& error) {
    stop_monitor();
    input_.reset();
    input_name_.clear();
    if (!input) {
        emit input_changed(QString(), error.isEmpty() ? tr("could not open %1").arg(stable_id) : error);
        return;
    }
    input_ = std::move(input);
    input_name_ = QString::fromStdString(input_->info().display_name);
    last_overflow_ = 0;
    update_live_timbre();
    // The monitor thread owns the input path: it blocks on the queue, sends each channel message
    // to the engine at once and leaves a copy for the GUI.
    monitor_ = std::jthread([input = input_, inbox = inbox_, playback = &playback_](std::stop_token stop) {
        std::array<platform::MidiInputMessage, 64> buffer{};
        while (!stop.stop_requested()) {
            const std::size_t count = input->wait_drain(buffer, std::chrono::milliseconds(20));
            if (count == 0)
                continue;
            if (inbox->monitoring.load(std::memory_order_relaxed))
                for (std::size_t index = 0; index < count; ++index)
                    playback->monitor_message(std::span<const std::uint8_t>(buffer[index].bytes.data(), buffer[index].size));
            std::lock_guard lock(inbox->mutex);
            for (std::size_t index = 0; index < count; ++index) {
                app::CapturedMessage message;
                message.time_ns = buffer[index].arrival_ns;
                message.size = buffer[index].size;
                message.bytes = buffer[index].bytes;
                // The stored time is the callback's arrival clock, not the driver timestamp, so the
                // evidence says so (spec 8.4 rule 1, code-runtime-17).
                message.quality = std::string(platform::timestamp_quality_name(platform::TimestampQuality::arrival_estimated));
                inbox->messages.push_back(message);
            }
        }
    });
    poll_timer_.start();
    emit input_changed(input_name_, QString());
}

void RecordingController::stop_monitor() {
    if (monitor_.joinable()) {
        monitor_.request_stop();
        monitor_.join(); // at most one wait_drain timeout (20 ms)
    }
    std::lock_guard lock(inbox_->mutex);
    inbox_->messages.clear();
}

void RecordingController::set_monitoring(bool monitoring) { inbox_->monitoring.store(monitoring, std::memory_order_relaxed); }

QString RecordingController::input_name() const { return input_ ? input_name_ : tr("none"); }

void RecordingController::update_live_timbre() {
    if (!controller_)
        return;
    const auto active = controller_->active_track_index();
    playback_.set_live_timbre(active ? controller_->active_sequence().tracks[*active].instrument.synth_preset
                                     : std::string("classic"));
}

void RecordingController::follow_edit_cursor() {
    if (step_ && controller_ && step_->set_cursor(controller_->edit_cursor()))
        emit status_message(tr("Step recording continues at the edit cursor"));
}

void RecordingController::set_target(DocumentController* controller, KeyboardWidget* keyboard) {
    if (recorder_ && controller != controller_)
        finish_take(true); // switching documents ends the take at the current position
    if (step_ && controller != controller_)
        set_step_recording(false);
    for (const auto& connection : target_connections_)
        disconnect(connection);
    target_connections_.clear();
    controller_ = controller;
    keyboard_ = keyboard;
    if (controller_) {
        target_connections_.push_back(connect(controller_, &DocumentController::edit_cursor_changed, this,
                                              &RecordingController::follow_edit_cursor));
        target_connections_.push_back(connect(controller_, &DocumentController::view_state_changed, this,
                                              &RecordingController::update_live_timbre));
        target_connections_.push_back(connect(controller_, &DocumentController::document_changed, this,
                                              &RecordingController::update_live_timbre));
    }
    update_live_timbre();
}

Tick RecordingController::tick_for(std::int64_t steady_ns) {
    if (!controller_)
        return 0;
    // The engine's audio clock: output latency, tempo multiplier and loop passes included.
    if (const auto tick = playback_.transport_tick_at(steady_ns)) {
        anchor_ns_ = steady_ns;
        anchor_tick_ = *tick;
        anchored_ = true;
        return *tick;
    }
    // Before the run started (the snapshot is still compiling, or input during the start-up)
    // the message precedes the take: the punch-in rules keep a held note from the boundary.
    if (!anchored_)
        return before_start_;
    // The engine stopped mid-take (or there is no audio device): continue from the last placed
    // message through the tempo map at the playback speed.
    const auto& tempo = controller_->tempo_map();
    const Rational speed = playback_.tempo_multiplier();
    const auto elapsed_us = static_cast<std::int64_t>(static_cast<__int128>(steady_ns - anchor_ns_) / 1000 *
                                                      speed.numerator / std::max<std::int64_t>(speed.denominator, 1));
    return tempo.microseconds_to_tick(tempo.tick_to_microseconds(anchor_tick_) + elapsed_us);
}

void RecordingController::toggle_recording() {
    if (!controller_)
        return;
    if (recorder_)
        finish_take(true);
    else
        start_take();
}

void RecordingController::start_take() {
    const auto active = controller_->active_track_index();
    if (!active) {
        emit status_message(tr("Select a track to record into"));
        return;
    }
    const Tick cursor = controller_->edit_cursor();
    const auto& meter = controller_->meter_map();
    const Tick count_in = meter.ticks_per_bar(meter.segment_at(cursor)) * count_in_bars_;
    const bool was_playing = playback_.display().state == TransportDisplay::State::playing;
    anchored_ = false;
    // The count-in is played by the engine (measured in time, so it also works from bar 1); the
    // metronome toggle is left as the user set it.
    const auto started = playback_.record(cursor, was_playing ? 0 : count_in_bars_);
    if (started) {
        record_start_ = *started;
        punched_in_ = was_playing;
    } else if (!playback_.has_device()) {
        // No audio output: there is no transport clock, so the take follows the system clock
        // from now, without a count-in.
        record_start_ = cursor;
        punched_in_ = false;
        anchor_ns_ = steady_now_ns();
        anchor_tick_ = cursor;
        anchored_ = true;
    } else {
        emit status_message(tr("Recording cannot start now; stop the transport first"));
        return;
    }
    before_start_ = record_start_ - std::max<Tick>(count_in, 0) - 1;
    app::RecordingOptions options;
    options.start_tick = record_start_;
    options.punch = TimeRange{record_start_, std::numeric_limits<Tick>::max() / 4};
    options.capture_held_notes = true; // notes held through the count-in start at the boundary
    options.latency_compensation_ns = latency_.count();
    // Loop takes (section 8.4 rule 6): the transport clock unfolds the passes; the recorder folds
    // them back into the loop and composes the last complete pass.
    if (const auto loop = playback_.loop(); loop && loop->length() > 0 && record_start_ < loop->end) {
        options.loop = *loop;
        options.punch = TimeRange{std::min(record_start_, loop->begin), loop->end};
        // Punching in while looping: the engine clock counts passes since playback started, so
        // the take begins in pass N, not pass 0 (batch 1 review R1).
        if (punched_in_)
            if (const auto now = playback_.transport_tick_at(steady_now_ns()); now && *now >= loop->begin)
                options.first_pass = (*now - loop->begin) / loop->length();
    }
    recorder_.emplace(controller_->active_track(), options, [this](std::int64_t ns) { return tick_for(ns); });
    journaled_chunks_ = 0;
    emit recording_changed(true);
    if (!started)
        emit status_message(tr("Recording without an audio device: timing follows the system clock"));
    else if (punched_in_)
        emit status_message(tr("Recording (punched in)"));
    else
        emit status_message(count_in_bars_ > 0 ? tr("Count-in %n bar(s), then recording", nullptr, count_in_bars_)
                                               : tr("Recording"));
}

void RecordingController::finish_take(bool stop_transport) {
    if (!recorder_ || !controller_)
        return;
    const Tick stop_tick = tick_for(steady_now_ns());
    auto recorder = std::move(*recorder_);
    recorder_.reset(); // before the transport changes, so transport_changed() does not finish twice
    // Stopped before capture began (during the count-in): there is no take to keep.
    const bool cancelled = stop_tick < record_start_;
    if (stop_transport) {
        if (punched_in_)
            playback_.punch_out(); // Recording -> Playing: playback goes on
        else
            playback_.stop(record_start_);
    }
    auto* journal = controller_->recovery_journal();
    if (cancelled) {
        emit recording_changed(false);
        if (journal)
            journal->discard_capture(recorder.take_id());
        emit status_message(tr("Take cancelled during the count-in"));
        return;
    }
    if (recorder.empty()) {
        // No input at all: no take, so no undo step and the document stays clean. The take
        // number is not used up either; the next real take gets it.
        emit recording_changed(false);
        if (journal)
            journal->discard_capture(recorder.take_id());
        emit status_message(tr("Nothing was played; no take kept"));
        return;
    }
    auto take = recorder.finish(controller_->document(), stop_tick, "Take " + std::to_string(take_number_++));
    emit recording_changed(false);
    if (!take) {
        if (journal)
            journal->discard_capture(recorder.take_id()); // nothing to recover: the user saw it rejected
        emit status_message(tr("Take not kept: %1").arg(QString::fromStdString(take.error().describe())));
        return;
    }
    // Journaling the take's TakeInsert removes its capture chunks in the same transaction.
    if (QString error; !controller_->apply(take->changes, &error)) {
        if (journal)
            journal->discard_capture(recorder.take_id());
        emit status_message(tr("Take not kept: %1").arg(error));
        return;
    }
    QString summary = tr("Recorded %1 note(s), %2 controller event(s)").arg(take->notes).arg(take->controllers);
    for (const auto& message : take->messages)
        summary += "; " + QString::fromStdString(message);
    emit status_message(summary);
}

void RecordingController::transport_changed() {
    if (!recorder_ || !playback_.has_device())
        return;
    // The transport left Recording on its own (end of the song, device loss): the take ends there.
    const auto state = playback_.display().state;
    if (state != TransportDisplay::State::recording && state != TransportDisplay::State::counting_in)
        finish_take(false);
}

void RecordingController::set_step_recording(bool enabled) {
    if (!enabled) {
        const bool was = step_.has_value();
        step_.reset();
        if (was)
            emit status_message(tr("Step recording off"));
        emit step_recording_changed(false);
        return;
    }
    if (!controller_ || !controller_->active_track_index()) {
        emit status_message(tr("Select a track to step-record into"));
        emit step_recording_changed(false);
        return;
    }
    const auto& sequence = controller_->active_sequence();
    const std::int32_t ppq = sequence.division.is_ppq() ? sequence.division.ppq : default_ppq;
    app::StepRecorder::Settings settings;
    settings.note_length = std::max<Tick>(1, 4 * ppq * sequence.settings.default_note_length.numerator /
                                                 sequence.settings.default_note_length.denominator);
    settings.advance = std::max<Tick>(1, 4 * ppq * sequence.settings.snap.numerator / sequence.settings.snap.denominator);
    settings.fixed_velocity = sequence.settings.default_velocity;
    step_.emplace(controller_->active_track(), controller_->edit_cursor(), settings);
    emit step_recording_changed(true);
    emit status_message(tr("Step recording at the edit cursor (move it to insert elsewhere): play notes; rest, tie and "
                           "back are in the Transport menu"));
}

void RecordingController::step_rest() {
    if (!step_ || !controller_)
        return;
    step_->rest();
    controller_->set_edit_cursor(step_->cursor());
}

void RecordingController::step_tie() {
    if (!step_ || !controller_)
        return;
    if (auto changes = step_->tie(controller_->document()))
        controller_->apply(*changes);
    controller_->set_edit_cursor(step_->cursor());
}

void RecordingController::step_back() {
    if (!step_ || !controller_)
        return;
    if (auto changes = step_->back(controller_->document()))
        controller_->apply(*changes);
    controller_->set_edit_cursor(step_->cursor());
}

void RecordingController::live_note(int pitch, int velocity, bool on) {
    app::CapturedMessage message;
    message.time_ns = steady_now_ns();
    message.port = 1;
    message.size = 3;
    message.bytes = {static_cast<std::uint8_t>(on ? 0x90 : 0x80), static_cast<std::uint8_t>(pitch),
                     static_cast<std::uint8_t>(on ? velocity : 0)};
    handle_message(message); // the caller already auditioned it
}

void RecordingController::poll_input() {
    if (!input_)
        return;
    if (!input_->connected()) {
        // Hot-unplug: close the take at the last known position and keep it recoverable.
        stop_monitor();
        if (recorder_) {
            recorder_->interrupt("MIDI input disconnected");
            finish_take(true);
        }
        playback_.panic(false);
        emit status_message(tr("MIDI input %1 disconnected").arg(input_name()));
        input_.reset();
        input_name_.clear();
        poll_timer_.stop();
        emit input_changed(QString(), tr("disconnected"));
        return;
    }
    std::vector<app::CapturedMessage> messages;
    {
        std::lock_guard lock(inbox_->mutex);
        messages.swap(inbox_->messages);
    }
    for (const auto& message : messages)
        handle_message(message); // already monitored by the monitor thread
    const auto statistics = input_->statistics();
    if (statistics.overflowed > last_overflow_) {
        if (recorder_)
            recorder_->report_lost(static_cast<std::size_t>(statistics.overflowed - last_overflow_));
        last_overflow_ = statistics.overflowed;
    }
}

void RecordingController::journal_full_chunks() {
    if (!recorder_ || !controller_)
        return;
    auto* journal = controller_->recovery_journal();
    if (!journal)
        return;
    const auto raw = recorder_->raw();
    // Only complete chunks: the same slicing finish() uses, so recovery and the final take agree.
    while ((journaled_chunks_ + 1) * app::take_chunk_messages <= raw.size()) {
        const auto slice = raw.subspan(journaled_chunks_ * app::take_chunk_messages, app::take_chunk_messages);
        document::CaptureChunk chunk;
        chunk.take = recorder_->take_id();
        chunk.track = recorder_->track(); // the take's track, even if another track was selected since
        chunk.ordinal = static_cast<std::int32_t>(journaled_chunks_);
        chunk.start_tick = record_start_;
        chunk.first_time_ns = std::max<std::int64_t>(0, slice.front().time_ns);
        chunk.payload = app::encode_take_chunk(slice);
        if (auto recorded = journal->record_capture_chunk(std::move(chunk)); !recorded) {
            emit status_message(tr("Recording is not protected against a crash: %1")
                                    .arg(QString::fromStdString(recorded.error().describe())));
            break;
        }
        ++journaled_chunks_;
    }
}

void RecordingController::handle_message(const app::CapturedMessage& message) {
    if (message.size == 0)
        return;
    const std::uint8_t kind = message.bytes[0] & 0xF0;
    const bool note_on = kind == 0x90 && message.bytes[2] > 0;
    const bool note_off = kind == 0x80 || (kind == 0x90 && message.bytes[2] == 0);
    if (keyboard_ && (note_on || note_off))
        keyboard_->set_key_state(message.bytes[1], KeySource::midi_input, note_on);
    if (recorder_) {
        recorder_->add(message);
        journal_full_chunks();
    }
    if (step_ && controller_) {
        if (note_on) {
            step_->key_down(message.bytes[1], message.bytes[2]);
        } else if (note_off) {
            if (auto changes = step_->key_up(controller_->document(), message.bytes[1])) {
                controller_->apply(*changes); // each committed step is one undo entry
                controller_->set_edit_cursor(step_->cursor());
            }
        }
    }
}

} // namespace mwb::ui
