#pragma once

#include "mwb/core/model.hpp"

#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>

namespace mwb::ui {

/// Snapshot of the transport for display, sampled by a 30-60 Hz UI timer (section 13.4: no Qt
/// timer ever sends musical events).
struct TransportDisplay {
    enum class State : std::uint8_t { stopped, playing, paused, counting_in, recording, device_lost };
    State state = State::stopped;
    Tick position = 0;
    std::uint64_t snapshot_generation = 0;   ///< Generation the scheduler is playing.
    bool playing_older_generation = false;   ///< Playback still uses an earlier edit (section 20.3).
    float peak_left = 0;
    float peak_right = 0;
    bool clipped = false;
    std::uint64_t underflows = 0;
    QString device_description;              ///< Actual device, rate and period (never fabricated).
};

/// The GUI's single entry point to sound: internal playback of the transport owner, auditioning
/// from the keyboard and editor, metronome and panic. Implemented over the playback, audio and
/// platform modules; one owner of the main transport at a time (section 6.2).
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
    virtual void set_loop(std::optional<TimeRange> loop) = 0;
    virtual void set_metronome(bool enabled) = 0;
    virtual void set_tempo_multiplier(double multiplier) = 0;
    /// Live audition through the private audition voice (never the document's channel state).
    virtual void audition_note_on(int pitch, int velocity, int channel, const std::string& synth_preset) = 0;
    virtual void audition_note_off(int pitch, int channel) = 0;
    /// Releases owned notes and sustain; with `broad` also all-notes-off/all-sound-off and bend
    /// reset on app-used channels (F12).
    virtual void panic(bool broad) = 0;
    [[nodiscard]] virtual TransportDisplay display() const = 0;
    /// True when a real audio device is open (false for the null backend or after device loss).
    [[nodiscard]] virtual bool has_device() const = 0;

signals:
    void transport_state_changed();
    void device_lost(const QString& reason);
};

/// Creates the service over the configured audio backend (MWB_AUDIO_BACKEND=null for headless).
[[nodiscard]] std::unique_ptr<PlaybackService> create_playback_service(QObject* parent = nullptr);

} // namespace mwb::ui
