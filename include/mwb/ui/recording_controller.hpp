#pragma once

#include "mwb/app/recording.hpp"

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <chrono>
#include <memory>
#include <optional>

namespace mwb::platform {
class IMidiBackend;
class IMidiInput;
} // namespace mwb::platform

namespace mwb::ui {

class DocumentController;
class KeyboardWidget;
class PlaybackService;

/// Live input and recording for the GUI (sections 8.3-8.5): opens a physical MIDI input by its
/// stable identity, monitors it through the audition voice, and drives real-time takes (count-in,
/// punch from the edit cursor or punch-in while playing, loop takes, held notes closed at stop)
/// and step recording. Input callbacks only fill the platform queue; this object drains it on the
/// GUI thread with a short timer, so timestamps stay those captured in the callback. Each message
/// is placed on the transport through the engine's audio clock (PlaybackService::
/// transport_tick_at), so the tempo multiplier, loop passes and a deferred or already running
/// start are all accounted for (section 8.4 rule 2).
class RecordingController : public QObject {
    Q_OBJECT

public:
    RecordingController(PlaybackService& playback, QObject* parent = nullptr);
    ~RecordingController() override;

    /// Lists input ports as (stable id, display name).
    [[nodiscard]] std::vector<std::pair<QString, QString>> input_ports();
    /// Opens an input by stable id (empty closes the current input).
    bool open_input(const QString& stable_id, QString* error);
    [[nodiscard]] QString input_name() const;

    void set_target(DocumentController* controller, KeyboardWidget* keyboard);
    void set_count_in_bars(int bars) { count_in_bars_ = std::clamp(bars, 0, 2); }
    void set_monitoring(bool monitoring) { monitoring_ = monitoring; }
    void set_latency_compensation(std::chrono::nanoseconds latency) { latency_ = latency; }

    /// Starts a take (stopped: count-in, then capture from the edit cursor; playing: punch-in at
    /// the playhead) or ends and commits it (a punch-in punches out and keeps playing; otherwise
    /// the transport stops and returns to where the take started).
    void toggle_recording();
    [[nodiscard]] bool recording() const { return recorder_.has_value(); }
    void set_step_recording(bool enabled);
    [[nodiscard]] bool step_recording() const { return step_.has_value(); }
    void step_rest();
    void step_tie();
    void step_back();

    /// Live notes from the computer keyboard or on-screen keyboard (port 1).
    void live_note(int pitch, int velocity, bool on);

signals:
    void status_message(const QString& text);
    void recording_changed(bool recording);

private:
    void poll_input();
    void handle_message(const app::CapturedMessage& message, bool monitor);
    /// Transport tick for a capture timestamp (see the class comment). Not const: it remembers the
    /// last engine mapping to extrapolate from once the engine stops mid-take.
    [[nodiscard]] Tick tick_for(std::int64_t steady_ns);
    void start_take();
    /// Ends the take. `stop_transport`: stop (or punch out); false when the transport already left
    /// Recording on its own (end of the song, device loss).
    void finish_take(bool stop_transport);
    void transport_changed();

    PlaybackService& playback_;
    QPointer<DocumentController> controller_;
    QPointer<KeyboardWidget> keyboard_;
    std::unique_ptr<platform::IMidiBackend> backend_;
    std::unique_ptr<platform::IMidiInput> input_;
    QTimer poll_timer_;
    std::optional<app::TakeRecorder> recorder_;
    std::optional<app::StepRecorder> step_;
    std::int64_t anchor_ns_ = 0;   ///< Last capture time placed by the engine clock (or the take start).
    Tick anchor_tick_ = 0;         ///< Its tick.
    bool anchored_ = false;        ///< anchor_ns_/anchor_tick_ are valid.
    Tick before_start_ = 0;        ///< Tick for input that arrives before the transport has started.
    bool punched_in_ = false;      ///< The take began while the transport was playing.
    Tick record_start_ = 0;
    int count_in_bars_ = 1;
    bool monitoring_ = true;
    std::chrono::nanoseconds latency_{0};
    std::uint64_t last_overflow_ = 0;
    int take_number_ = 1;
};

} // namespace mwb::ui
