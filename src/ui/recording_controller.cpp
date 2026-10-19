#include "mwb/ui/recording_controller.hpp"

#include "mwb/platform/midi_backend.hpp"
#include "mwb/ui/document_controller.hpp"
#include "mwb/ui/keyboard_widget.hpp"
#include "mwb/ui/playback_service.hpp"

#include <array>
#include <limits>

namespace mwb::ui {

namespace {

std::int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

RecordingController::RecordingController(PlaybackService& playback, QObject* parent)
    : QObject(parent), playback_(playback) {
    poll_timer_.setInterval(4);
    connect(&poll_timer_, &QTimer::timeout, this, &RecordingController::poll_input);
    connect(&playback_, &PlaybackService::transport_state_changed, this, &RecordingController::transport_changed);
}

RecordingController::~RecordingController() = default;

std::vector<std::pair<QString, QString>> RecordingController::input_ports() {
    std::vector<std::pair<QString, QString>> ports;
    if (!backend_) {
        auto backend = platform::create_midi_backend();
        if (!backend) {
            emit status_message(tr("MIDI input unavailable: %1").arg(QString::fromStdString(backend.error().describe())));
            return ports;
        }
        backend_ = std::move(*backend);
    }
    auto inputs = backend_->enumerate_inputs();
    if (!inputs)
        return ports;
    for (const auto& port : *inputs)
        ports.emplace_back(QString::fromStdString(port.stable_id), QString::fromStdString(port.display_name));
    return ports;
}

bool RecordingController::open_input(const QString& stable_id, QString* error) {
    poll_timer_.stop();
    input_.reset();
    if (stable_id.isEmpty())
        return true;
    if (!backend_)
        std::ignore = input_ports(); // creates the backend; the list itself is not needed here
    if (!backend_)
        return false;
    // Opened by stable identity; a missing or busy endpoint is reported, never substituted.
    auto opened = backend_->open_input(stable_id.toStdString());
    if (!opened) {
        if (error)
            *error = QString::fromStdString(opened.error().describe());
        return false;
    }
    input_ = std::move(*opened);
    last_overflow_ = 0;
    poll_timer_.start();
    return true;
}

QString RecordingController::input_name() const {
    return input_ ? QString::fromStdString(input_->info().display_name) : tr("none");
}

void RecordingController::set_target(DocumentController* controller, KeyboardWidget* keyboard) {
    if (recorder_ && controller != controller_)
        finish_take(true); // switching documents ends the take at the current position
    controller_ = controller;
    keyboard_ = keyboard;
    if (step_)
        set_step_recording(false);
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
    }
    recorder_.emplace(controller_->active_track(), options, [this](std::int64_t ns) { return tick_for(ns); });
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
    if (stop_transport) {
        if (punched_in_)
            playback_.punch_out(); // Recording -> Playing: playback goes on
        else
            playback_.stop(record_start_);
    }
    auto take = recorder.finish(controller_->document(), stop_tick, "Take " + std::to_string(take_number_++));
    emit recording_changed(false);
    if (!take) {
        emit status_message(tr("Take not kept: %1").arg(QString::fromStdString(take.error().describe())));
        return;
    }
    controller_->apply(take->changes);
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
        step_.reset();
        emit status_message(tr("Step recording off"));
        return;
    }
    if (!controller_ || !controller_->active_track_index())
        return;
    const auto& sequence = controller_->active_sequence();
    const std::int32_t ppq = sequence.division.is_ppq() ? sequence.division.ppq : default_ppq;
    app::StepRecorder::Settings settings;
    settings.note_length = std::max<Tick>(1, 4 * ppq * sequence.settings.default_note_length.numerator /
                                                 sequence.settings.default_note_length.denominator);
    settings.advance = std::max<Tick>(1, 4 * ppq * sequence.settings.snap.numerator / sequence.settings.snap.denominator);
    settings.fixed_velocity = sequence.settings.default_velocity;
    step_.emplace(controller_->active_track(), controller_->edit_cursor(), settings);
    emit status_message(tr("Step recording at the edit cursor: play notes; rest, tie and back are in the Transport menu"));
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
    handle_message(message, false); // the caller already auditioned it
}

void RecordingController::poll_input() {
    if (!input_)
        return;
    if (!input_->connected()) {
        // Hot-unplug: close the take at the last known position and keep it recoverable.
        if (recorder_) {
            recorder_->interrupt("MIDI input disconnected");
            finish_take(true);
        }
        playback_.panic(false);
        emit status_message(tr("MIDI input %1 disconnected").arg(input_name()));
        input_.reset();
        poll_timer_.stop();
        return;
    }
    std::array<platform::MidiInputMessage, 256> buffer{};
    const std::size_t count = input_->drain(buffer);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& received = buffer[index];
        app::CapturedMessage message;
        message.time_ns = received.arrival_ns;
        message.size = received.size;
        message.bytes = received.bytes;
        message.quality = std::string(platform::timestamp_quality_name(received.quality));
        handle_message(message, true);
    }
    const auto statistics = input_->statistics();
    if (statistics.overflowed > last_overflow_) {
        if (recorder_)
            recorder_->report_lost(static_cast<std::size_t>(statistics.overflowed - last_overflow_));
        last_overflow_ = statistics.overflowed;
    }
}

void RecordingController::handle_message(const app::CapturedMessage& message, bool monitor) {
    if (message.size == 0)
        return;
    const std::uint8_t kind = message.bytes[0] & 0xF0;
    const bool note_on = kind == 0x90 && message.bytes[2] > 0;
    const bool note_off = kind == 0x80 || (kind == 0x90 && message.bytes[2] == 0);
    if (monitor && monitoring_ && (note_on || note_off)) {
        const auto active = controller_ ? controller_->active_track_index() : std::nullopt;
        const std::string preset = active ? controller_->active_sequence().tracks[*active].instrument.synth_preset : "classic";
        if (note_on)
            playback_.audition_note_on(message.bytes[1], message.bytes[2], 0, preset);
        else
            playback_.audition_note_off(message.bytes[1], 0);
    }
    if (keyboard_ && (note_on || note_off))
        keyboard_->set_key_state(message.bytes[1], KeySource::midi_input, note_on);
    if (recorder_)
        recorder_->add(message);
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
