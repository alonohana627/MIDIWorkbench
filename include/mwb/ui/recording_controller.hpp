#pragma once

#include "mwb/app/recording.hpp"
#include "mwb/ui/task_thread.hpp"

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

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
/// and step recording. Input devices are listed and opened on a background thread (the GUI never
/// waits for a driver, section 19.4). A monitor thread blocks on the input queue and sends every
/// channel message (notes, sustain and other controllers, bend, pressure) straight to the
/// engine's monitor queue, so monitoring never waits for the GUI (section 14.1); it hands copies to
/// the GUI, which records them with the timestamps captured in the callback. Each message
/// is placed on the transport through the engine's audio clock (PlaybackService::
/// transport_tick_at), so the tempo multiplier, loop passes and a deferred or already running
/// start are all accounted for (section 8.4 rule 2).
class RecordingController : public QObject {
    Q_OBJECT

public:
    RecordingController(PlaybackService& playback, QObject* parent = nullptr);
    ~RecordingController() override;

    using PortList = std::vector<std::pair<QString, QString>>; ///< (stable id, display name)
    /// Lists input ports in the background; `done` runs on the GUI thread with the ports, or an
    /// empty list and the reason.
    void list_input_ports(std::function<void(PortList ports, QString error)> done);
    /// Opens an input by stable id in the background (empty closes the current input). The
    /// result arrives as input_changed().
    void open_input(const QString& stable_id);
    [[nodiscard]] QString input_name() const;

    void set_target(DocumentController* controller, KeyboardWidget* keyboard);
    void set_count_in_bars(int bars) { count_in_bars_ = std::clamp(bars, 0, 2); }
    void set_monitoring(bool monitoring);
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
    /// Step recording was armed or disarmed (also when switching documents disarms it).
    void step_recording_changed(bool armed);
    /// An input open finished: `name` of the open input (empty when none) and the error, if any.
    void input_changed(const QString& name, const QString& error);

private:
    struct Inbox;
    struct Devices;
    void install_input(std::shared_ptr<platform::IMidiInput> input, const QString& stable_id, const QString& error);
    void stop_monitor();
    /// Journals each completed take chunk as soon as it fills (crash protection, section 21.2).
    void journal_full_chunks();
    void update_live_timbre();
    /// The user moved the edit cursor: step recording inserts there next (section 8.5).
    void follow_edit_cursor();
    void poll_input();
    /// Recording, step entry and the key display for one input message (GUI thread).
    void handle_message(const app::CapturedMessage& message);
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
    std::shared_ptr<platform::IMidiInput> input_;
    QString input_name_;
    std::shared_ptr<Inbox> inbox_;        ///< Messages the monitor thread passed on, and the monitoring flag.
    std::shared_ptr<Devices> devices_;    ///< MIDI backend, used only on device_thread_.
    std::jthread monitor_;                ///< Blocks on input_ and forwards to the engine.
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
    std::size_t journaled_chunks_ = 0;    ///< Full take chunks already given to the recovery journal.
    std::vector<QMetaObject::Connection> target_connections_; ///< To the current DocumentController.
    std::chrono::nanoseconds latency_{0};
    std::uint64_t last_overflow_ = 0;
    int take_number_ = 1;
    TaskThread device_thread_;            ///< Lists and opens inputs (stopped first on destruction).
};

} // namespace mwb::ui
