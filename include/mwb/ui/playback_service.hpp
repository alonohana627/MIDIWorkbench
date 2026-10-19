#pragma once

#include "mwb/core/model.hpp"

#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace mwb::ui {

/// Snapshot of the transport for display, sampled by a 30-60 Hz UI timer (section 13.4: no Qt
/// timer ever sends musical events). Reading it has no side effects (see take_meters()).
struct TransportDisplay {
    enum class State : std::uint8_t { stopped, playing, paused, counting_in, recording, device_lost };
    State state = State::stopped;
    Tick position = 0;
    std::uint64_t snapshot_generation = 0;   ///< Generation the scheduler is playing.
    bool playing_older_generation = false;   ///< Playback still uses an earlier edit (section 20.3).
    std::uint64_t underflows = 0;
    bool opening = false;                    ///< The audio device is being opened (in the background).
    QString device_description;              ///< Actual device, rate and period (never fabricated).
};

/// Peak meter readings since the previous take_meters() call.
struct TransportMeters {
    float peak_left = 0;
    float peak_right = 0;
    bool clipped = false;
};

/// An audio output the user can choose (id for reopening, display name).
struct AudioDeviceChoice {
    std::string id;
    QString name;
    bool is_default = false;
};

/// The GUI's single entry point to sound: internal playback of the transport owner, auditioning
/// from the keyboard and editor, metronome and panic. Implemented over the playback, audio and
/// platform modules; one owner of the main transport at a time (section 6.2). The audio device
/// is opened in the background (section 19.4: the GUI never waits for a device); requests made
/// meanwhile are kept and run once it is open.
class PlaybackService : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~PlaybackService() override = default;

    /// Compiles a playback snapshot of the document off the GUI thread and publishes it.
    virtual void publish_document(const DocumentData& document, std::size_t sequence_index, std::uint64_t generation) = 0;
    virtual void play(Tick from) = 0;
    virtual void pause() = 0;
    /// Stops and returns to `return_to`.
    virtual void stop(Tick return_to) = 0;
    virtual void seek(Tick tick) = 0;
    /// Starts a take (section 13.3). Stopped or Paused: plays from `from` after `count_in_bars`
    /// bars of count-in clicks (CountingIn, then Recording; the count-in is measured in time, so
    /// it also works from tick 0). Playing: punches in at the current position, no count-in.
    /// Returns the tick where capture starts, or nullopt when the transport refuses.
    [[nodiscard]] virtual std::optional<Tick> record(Tick from, int count_in_bars) = 0;
    /// Recording -> Playing: the take ends and playback continues.
    virtual void punch_out() = 0;
    /// Linear transport tick heard at `steady_ns` (steady_clock nanoseconds) in the current play
    /// run: the engine's audio clock minus output latency, through the playing snapshot's tempo
    /// map, tempo multiplier and loop, with later loop passes unfolded. nullopt while the
    /// transport is not rolling or the run has not started yet (section 8.4 rule 2).
    [[nodiscard]] virtual std::optional<Tick> transport_tick_at(std::int64_t steady_ns) const = 0;
    virtual void set_loop(std::optional<TimeRange> loop) = 0;
    /// Loop range of the published playback (nullopt when not looping).
    [[nodiscard]] virtual std::optional<TimeRange> loop() const = 0;
    /// Playback speed multiplier currently applied (1 = document tempo).
    [[nodiscard]] virtual Rational tempo_multiplier() const = 0;
    virtual void set_metronome(bool enabled) = 0;
    /// Takes effect at once, also while playing (the snapshot is recompiled; the musical position
    /// is kept).
    virtual void set_tempo_multiplier(double multiplier) = 0;
    /// Live audition through the private audition voice (never the document's channel state).
    virtual void audition_note_on(int pitch, int velocity, int channel, const std::string& synth_preset) = 0;
    virtual void audition_note_off(int pitch, int channel) = 0;
    /// Timbre of the audition voice for monitored MIDI input (GUI thread).
    virtual void set_live_timbre(const std::string& synth_preset) = 0;
    /// Monitors one MIDI input channel message (note, controller incl. sustain, bend, pressure)
    /// through the audition voice. Thread-safe, for exactly one producer thread: the input
    /// monitor (section 14.1: the input path does not go through the GUI).
    virtual void monitor_message(std::span<const std::uint8_t> message) = 0;
    /// Releases owned notes and sustain; with `broad` also all-notes-off/all-sound-off and bend
    /// reset on app-used channels (F12).
    virtual void panic(bool broad) = 0;
    [[nodiscard]] virtual TransportDisplay display() const = 0;
    /// Peak meters since the last call (only the display timer calls this, so no reader can
    /// swallow a CLIP another one should show).
    [[nodiscard]] virtual TransportMeters take_meters() = 0;
    /// True when an audio device is open and running (false while opening or after device loss).
    [[nodiscard]] virtual bool has_device() const = 0;
    /// Closes the current output and opens `device_id` (the default output when nullopt) in the
    /// background. After a device loss this also leaves DeviceLost (Reconfigure); while the
    /// device is lost the service retries the chosen output every 2 s by itself.
    virtual void reopen_device(std::optional<std::string> device_id) = 0;
    /// Outputs found when the device was last opened.
    [[nodiscard]] virtual std::vector<AudioDeviceChoice> audio_devices() const = 0;
    /// Test hook: makes the null audio output behave as if unplugged (no effect on real devices).
    virtual void simulate_device_loss() = 0;

signals:
    void transport_state_changed();
    void device_lost(const QString& reason);
    /// An open (or reopen) finished; has_device() and display().device_description tell the result.
    void device_changed();
    /// Playback could not start or continue with the current document (compile or preflight).
    void playback_refused(const QString& reason);
};

/// Creates the service over the configured audio backend (MWB_AUDIO_BACKEND=null for headless),
/// opening `device_id` (the default output when nullopt) in the background.
[[nodiscard]] std::unique_ptr<PlaybackService> create_playback_service(QObject* parent = nullptr,
                                                                       std::optional<std::string> device_id = std::nullopt);

} // namespace mwb::ui
