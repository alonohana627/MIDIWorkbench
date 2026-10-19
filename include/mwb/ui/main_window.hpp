#pragma once

#include "mwb/app/document_io.hpp"
#include "mwb/app/environment.hpp"
#include "mwb/document/recovery.hpp"
#include "mwb/ui/task_thread.hpp"
#include "mwb/core/model.hpp"

#include <QMainWindow>
#include <QPointer>
#include <QTimer>

#include <chrono>
#include <filesystem>
#include <map>
#include <functional>
#include <vector>
#include <memory>
#include <optional>
#include <set>

class QAction;
class QDockWidget;
class QLabel;
class QTabWidget;
class QToolBar;
class QDoubleSpinBox;
class QActionGroup;
class QComboBox;
class QLineEdit;

namespace mwb::document {
class RecoveryJournal;
}

namespace mwb::ui {

class ComputerKeyboardPlayer;
class DocumentController;
class EditorTab;
class EventListPanel;
class InspectorPanel;
class LibraryPanel;
class PlaybackService;
class DragHandle;
class RecordingController;
class TempoField;

/// Startup options shared by the GUI and its headless test mode (section 32.2).
struct StartupOptions {
    std::filesystem::path workspace;          ///< --workspace: data directory override.
    std::vector<std::filesystem::path> files; ///< Documents to open.
    bool exit_when_ready = false;             ///< --exit-when-ready
    std::filesystem::path screenshot;         ///< --screenshot <file.png>
    bool skip_recovery_prompt = false;
};

/// Main window (section 6.1): document tabs, transport, library dock, inspector dock, event list
/// dock, center editor with lanes and keyboard, and the status area. Commands are QActions with
/// command IDs, shortcuts per section 7.5 and command palette entries.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(StartupOptions options, app::DataDirectories directories, QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Opens a document file (MIDI or native) in a new tab.
    bool open_path(const std::filesystem::path& path);
    /// Creates a document from a built-in template name ("blank", "melody", "chords", "bassline", "drums").
    void new_from_template(const QString& name);
    /// Imports or reopens a document already in memory (used by recovery and the library).
    void open_document(DocumentData document, std::optional<std::filesystem::path> path, bool recovered,
                       std::shared_ptr<document::RecoveryJournal> journal = nullptr, bool journal_prepared = false);
    [[nodiscard]] EditorTab* current_tab() const;
    /// Offers recoverable sessions left by a crash (section 5.5).
    void offer_recovery();
    void present_recovery(std::vector<document::RecoverableSession> sessions);
    /// Opens an in-memory document in a tab after its recovery journal (whose first checkpoint
    /// serializes the whole document) was created on the I/O thread; `opened` runs with the tab.
    void open_prepared(DocumentData document, bool recovered, std::function<void(EditorTab*)> opened = {});

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    QAction* add_command(const QString& id, const QString& text, const QKeySequence& shortcut,
                         std::function<void()> handler, Qt::ShortcutContext context = Qt::WindowShortcut);
    /// Keyboard contract for focused controls in this window (C2): Return/Enter press a focused
    /// button or toggle a check cell, Space does so for a control reached with the keyboard,
    /// navigation keys stay with lists, combos and fields, letters and digits stay only with text
    /// input. `override`: the event is the ShortcutOverride, else the KeyPress. Returns the
    /// eventFilter result when the key was routed, nullopt to continue normally.
    std::optional<bool> route_control_key(QKeyEvent& key, QWidget& focus, bool override);
    /// Line edits, spin boxes, text editors and an open combo popup: they keep typed characters.
    [[nodiscard]] static bool is_text_input(QWidget& focus);
    void build_actions();
    void build_menus();
    void build_docks();
    void build_toolbar();
    /// Sets every toolbar tooltip to "Name (shortcut)" from the current bindings.
    void refresh_tooltips();
    void set_theme(const QString& name);
    QAction* command(const char* id) const;
    void current_tab_changed();
    void update_command_state();
    void update_status_from_transport();
    void flush_recovery();
    /// Saves the current tab; `blocking` waits for durable completion (used when closing).
    bool save_current(bool save_as, bool blocking = false);
    bool save_tab(EditorTab* tab, bool save_as, bool blocking);
    void finish_open(const std::filesystem::path& path, Result<app::OpenedDocument> opened,
                     std::shared_ptr<document::RecoveryJournal> journal);
    void export_current_midi();
    void close_tab(int index);
    bool confirm_close(EditorTab* tab);
    void apply_layout(const QString& name);
    void reset_layout();
    void show_command_palette(const QString& query = {});
    void show_about();
    void show_diagnostics();
    void transpose_selection(int semitones);
    void move_selection_by_grid(int direction);
    void move_play_position(int direction);
    void transpose_selection_by_degree(int degrees);
    void delete_selection();
    void duplicate_selection();
    void copy_selection(bool cut);
    void paste_at_cursor();
    /// Space: stopped -> play from the edit cursor; rolling (or paused) -> stop and return to
    /// where playback started, like a DAW.
    void play_stop();
    /// Ctrl+Space: pause a running transport, resume a paused one (from the edit cursor when it
    /// was moved meanwhile), or start when stopped.
    void pause_resume();
    void stop_transport();
    void play_from_selection();
    void go_to_start();
    void go_to_end();
    /// Next (direction +1) or previous (-1) play position from `from`, by a unit that follows the
    /// zoom: the smallest of grid step, beat, bar, 4 bars and 16 bars that is at least
    /// `min_step_pixels` wide, raised by `acceleration` units (holding the key).
    [[nodiscard]] Tick play_position_step(EditorTab& tab, Tick from, int direction, int acceleration) const;
    /// Starts the transport for `tab` at `from` and remembers it as the return position.
    void start_playback(EditorTab& tab, Tick from);
    void connect_tab(EditorTab* tab);
    void publish_playback();
    /// Where the toolbar BPM field reads and writes the tempo: the playhead while this tab owns
    /// the rolling transport, otherwise the edit cursor.
    [[nodiscard]] Tick tempo_position(EditorTab& tab) const;
    void commit_tempo(double bpm);
    void set_snap_off(bool off);
    /// Off, relative or absolute snapping on every tab (a view setting, not part of the document).
    void apply_snap_mode();
    void update_sounding_notes(EditorTab& tab, std::optional<Tick> position);
    void write_screenshot_and_exit();

    StartupOptions options_;
    app::DataDirectories directories_;
    QTabWidget* tabs_ = nullptr;
    QDockWidget* library_dock_ = nullptr;
    QDockWidget* inspector_dock_ = nullptr;
    QDockWidget* events_dock_ = nullptr;
    InspectorPanel* inspector_ = nullptr;
    LibraryPanel* library_ = nullptr;
    QToolBar* transport_bar_ = nullptr;
    QLabel* position_label_ = nullptr;
    QLabel* device_label_ = nullptr;
    QLabel* cursor_label_ = nullptr;
    QLabel* selection_label_ = nullptr;
    QLabel* snap_label_ = nullptr;
    QLabel* recovery_label_ = nullptr;
    QComboBox* snap_combo_ = nullptr;
    QActionGroup* tool_group_ = nullptr;
    QAction* keyboard_action_ = nullptr;
    QLineEdit* command_search_ = nullptr;
    bool updating_toolbar_ = false;
    bool showing_pause_ = false;
    QDoubleSpinBox* tempo_multiplier_ = nullptr;
    TempoField* tempo_field_ = nullptr;
    std::optional<Tick> tempo_edit_at_; ///< Position captured when a BPM edit burst started.
    QAction* loop_action_ = nullptr;
    QAction* metronome_action_ = nullptr;
    QAction* playing_mode_action_ = nullptr;
    QAction* undo_action_ = nullptr;
    QAction* redo_action_ = nullptr;
    std::vector<QAction*> commands_;
    std::unique_ptr<PlaybackService> playback_;
    ComputerKeyboardPlayer* computer_keyboard_ = nullptr;
    RecordingController* recording_ = nullptr;
    DragHandle* drag_handle_ = nullptr;
    QAction* record_action_ = nullptr;
    QAction* step_action_ = nullptr;
    QTimer transport_timer_;
    QTimer recovery_timer_;
    QPointer<DocumentController> transport_owner_;
    /// Where the current playback started; Space (stop) returns the playhead here.
    Tick play_started_from_ = 0;
    /// Edit cursor when the transport paused; a different cursor on resume means "play from here".
    std::optional<Tick> paused_cursor_;
    /// Left/Right (with or without Ctrl) held down: the key and when it was first pressed, so
    /// auto-repeat accelerates the play-position step (reset on release).
    int held_position_key_ = 0;
    /// Widget that last received focus from the keyboard (Tab, Backtab, a shortcut); Space
    /// activates it when it is a button or a check cell, a mouse-focused control leaves Space to Play.
    QPointer<QWidget> keyboard_focus_;
    QLabel* step_label_ = nullptr; ///< "STEP" while step recording is armed.
    QAction* snap_off_action_ = nullptr;
    QAction* snap_relative_action_ = nullptr;
    QAction* follow_action_ = nullptr;
    bool snap_off_ = false;
    bool snap_relative_ = false;
    std::map<EditorTab*, std::set<int>> sounding_by_tab_; ///< Pitches lit by playback per tab.
    std::chrono::steady_clock::time_point clip_until_{}; ///< "! CLIP" stays visible until then.
    bool audio_was_lost_ = false;
    std::chrono::steady_clock::time_point position_key_down_{};
    /// Library item and revision each tab was opened from (for saving new revisions).
    std::map<const EditorTab*, std::pair<std::int64_t, std::int64_t>> library_sources_;
    /// MIDI parsing in the isolated worker process, shared with the library panel.
    std::shared_ptr<app::WorkerParser> parser_;
    /// A second worker for library imports, so an interactive open never waits behind one.
    std::shared_ptr<app::WorkerParser> import_parser_;
    /// Files being opened in the background; the screenshot and the startup blank document wait.
    int pending_opens_ = 0;
    /// File open and export run here, off the GUI thread (declared last: joined first).
    std::unique_ptr<TaskThread> io_thread_;
};

} // namespace mwb::ui
