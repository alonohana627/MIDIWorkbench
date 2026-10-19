#include "mwb/ui/main_window.hpp"

#include "mwb/ui/icons.hpp"
#include "mwb/ui/theme.hpp"

#include "mwb/app/document_io.hpp"
#include "mwb/app/third_party.hpp"
#include "mwb/build_info.hpp"
#include "mwb/core/note_edit.hpp"
#include "mwb/core/pitch.hpp"
#include "mwb/document/native_format.hpp"
#include "mwb/document/file_system.hpp"
#include "mwb/library/storage.hpp"
#include "mwb/document/recovery.hpp"
#include "mwb/document/session.hpp"
#include "mwb/midi/codec.hpp"
#include "mwb/music/chord.hpp"
#include "mwb/music/scale.hpp"
#include "mwb/music/transform.hpp"
#include "mwb/ui/clipboard.hpp"
#include "mwb/ui/command_palette.hpp"
#include "mwb/ui/document_controller.hpp"
#include "mwb/ui/drag_export.hpp"
#include "mwb/ui/drum_grid.hpp"
#include "mwb/ui/editor_tab.hpp"
#include "mwb/ui/event_list.hpp"
#include "mwb/ui/inspector.hpp"
#include "mwb/ui/library_panel.hpp"
#include "mwb/ui/playback_service.hpp"
#include "mwb/ui/recording_controller.hpp"
#include "mwb/ui/shortcut_editor.hpp"
#include "mwb/ui/scale_bar.hpp"
#include "mwb/ui/tempo_field.hpp"

#include <QApplication>
#include <QDesktopServices>
#include <QLocale>
#include <QProgressDialog>
#include <QEventLoop>
#include <QClipboard>
#include <QDateTime>
#include <QCloseEvent>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QUrl>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGuiApplication>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QAbstractScrollArea>
#include <QActionGroup>
#include <QScrollBar>
#include <QWheelEvent>
#include <QComboBox>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QToolButton>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QToolBar>
#include <QVBoxLayout>

#include <utility>

namespace mwb::ui {

namespace {

constexpr int layout_schema_version = 2; // 2: toolbar and default dock sizes of the UI redesign

/// Item data of the toolbar Snap combo's "Off" entry (a view setting, not a grid).
constexpr const char* snap_off_value = "off";

std::filesystem::path to_path(const QString& text) { return std::filesystem::path(text.toStdWString()); }

QString to_qstring(const std::filesystem::path& path) { return QString::fromStdWString(path.wstring()); }

bool transport_display_is_stopped(const TransportDisplay& display) {
    return display.state == TransportDisplay::State::stopped || display.state == TransportDisplay::State::device_lost;
}

/// Built-in templates of section 5.1 (blank defaults plus optional starter notes).
DocumentData template_document(const QString& name) {
    DocumentData document = make_blank_document("Untitled");
    auto& sequence = document.sequences.front();
    auto& track = sequence.tracks.front();
    if (name == "melody") {
        track.name = "Melody";
        track.instrument.synth_preset = "square-lead";
        document.title = "Melody";
    } else if (name == "chords") {
        track.name = "Chords";
        track.instrument.synth_preset = "electric-piano";
        document.title = "Chords";
        // Starter progression I-V-vi-IV in C major, one chord per bar.
        const int roots[] = {60, 55, 57, 53};
        const music::ChordType types[] = {music::ChordType::major, music::ChordType::major, music::ChordType::minor,
                                          music::ChordType::major};
        for (int bar = 0; bar < 4; ++bar) {
            music::ChordSpec spec;
            spec.root_pitch = roots[bar];
            spec.type = types[bar];
            music::ChordRenderSpec render;
            render.start = bar * 3840;
            render.duration = 3840;
            render.velocity = 90;
            if (auto preview = music::insert_chord(document, track.id, spec, render))
                std::ignore = apply_change_set(document, preview->changes); // template content on a fresh document
        }
    } else if (name == "bassline") {
        track.name = "Bass";
        track.instrument.synth_preset = "bass";
        document.title = "Bassline";
    } else if (name == "drums") {
        track.name = "Drums";
        track.is_drum = true;
        document.title = "Drums";
        ChangeBuilder builder(document, "Template");
        for (int beat = 0; beat < 16; ++beat) {
            const Tick start = beat * 960;
            builder.add_note(track.id, start, start + 120, 9, beat % 2 == 0 ? 36 : 38, 100);
            builder.add_note(track.id, start, start + 120, 9, 42, 80);
            builder.add_note(track.id, start + 480, start + 600, 9, 42, 70);
        }
        std::ignore = apply_change_set(document, builder.finish()); // template content on a fresh document
    }
    return document;
}

} // namespace

MainWindow::MainWindow(StartupOptions options, app::DataDirectories directories, QWidget* parent)
    : QMainWindow(parent), options_(std::move(options)), directories_(std::move(directories)) {
    setWindowTitle(tr("MIDI Workbench"));
    // Below this the roll is squeezed between the panels and status messages overlap the
    // readouts (ux-visual-22).
    setMinimumSize(1024, 640);
    setDockNestingEnabled(true);
    setAcceptDrops(true); // .mid/.midi/.rmi/.mwb files can be dropped in (section 14.3)
    tabs_ = new QTabWidget(this);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    tabs_->setFocusPolicy(Qt::NoFocus); // its tab bar keeps its own focus; the widget is no dead stop
    tabs_->setAccessibleName(tr("Open documents"));
    setCentralWidget(tabs_);
    // The output opens in the background (the window never waits for a device); the chosen
    // output is remembered across sessions.
    {
        const QString saved = QSettings().value("audio/device").toString();
        playback_ = create_playback_service(nullptr, saved.isEmpty() ? std::nullopt : std::optional<std::string>(saved.toStdString()));
    }
    connect(playback_.get(), &PlaybackService::device_lost, this,
            [this](const QString& reason) { statusBar()->showMessage(reason, 15000); });
    connect(playback_.get(), &PlaybackService::playback_refused, this,
            [this](const QString& reason) { statusBar()->showMessage(reason, 10000); });
    connect(playback_.get(), &PlaybackService::device_changed, this, [this] {
        const auto display = playback_->display();
        if (!display.opening && playback_->has_device() && audio_was_lost_)
            statusBar()->showMessage(tr("Audio output reconnected: %1").arg(display.device_description), 6000);
        audio_was_lost_ = display.state == TransportDisplay::State::device_lost || (!display.opening && !playback_->has_device());
        update_status_from_transport();
    });
    computer_keyboard_ = new ComputerKeyboardPlayer(this);
    recording_ = new RecordingController(*playback_, this);
    connect(recording_, &RecordingController::status_message, this, [this](const QString& text) { statusBar()->showMessage(text, 8000); });
    connect(recording_, &RecordingController::input_changed, this, [this](const QString& name, const QString& error) {
        if (!error.isEmpty())
            statusBar()->showMessage(tr("MIDI input: %1").arg(error), 8000);
        else if (!name.isEmpty())
            statusBar()->showMessage(tr("MIDI input: %1").arg(name), 5000);
    });
    build_docks();
    library_->set_drag_cache(directories_.drag_cache);
    io_thread_ = std::make_unique<TaskThread>();
    // Old drag files are removed off the GUI thread: a slow or network profile never delays the
    // window (section 19.4, code-uiA-27).
    io_thread_->post([cache = directories_.drag_cache](std::stop_token) { clean_drag_cache(cache); });
    parser_ = std::make_shared<app::WorkerParser>(to_path(QCoreApplication::applicationDirPath()) / "mwb-worker.exe");
    import_parser_ = std::make_shared<app::WorkerParser>(to_path(QCoreApplication::applicationDirPath()) / "mwb-worker.exe");
    const auto bind = [](std::shared_ptr<app::WorkerParser> parser) {
        return [parser](std::span<const std::uint8_t> bytes, std::string_view name, const midi::ParseLimits& limits,
                        std::stop_token stop) { return parser->parse(bytes, name, limits, std::move(stop)); };
    };
    library_->set_parser(bind(parser_));
    library_->set_import_parser(bind(import_parser_));
    build_actions();
    build_toolbar();
    build_menus();
    qApp->installEventFilter(this); // focused controls keep their own keys (eventFilter)
    // At startup the editor has the keyboard (the window is shown after this constructor).
    QTimer::singleShot(0, this, [this] {
        if (auto* tab = current_tab())
            tab->piano_roll().setFocus(Qt::OtherFocusReason);
    });
    {
        // Saved shortcut profile (versioned JSON); a damaged profile keeps the defaults.
        const QString profile = QSettings().value("shortcuts/profile").toString();
        QStringList messages;
        if (!profile.isEmpty() && !apply_shortcuts_json(commands_, profile, &messages))
            statusBar()->showMessage(tr("Saved shortcuts could not be read; defaults are used"), 8000);
        // Profiles saved before Space became Play/Stop bound Ctrl+Space to Stop; it is now Pause/Resume.
        if (!profile.contains("transport.pause_resume"))
            if (auto* stop = command("transport.stop"); stop && stop->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_Space))
                stop->setShortcut({});
    }

    cursor_label_ = new QLabel(this);
    selection_label_ = new QLabel(this);
    snap_label_ = new QLabel(this);
    device_label_ = new QLabel(this);
    recovery_label_ = new QLabel(this);
    // Armed step recording is a mode: it stays visible while it lasts (section 8.5).
    step_label_ = new QLabel(tr("● STEP"), this);
    step_label_->setToolTip(tr("Step recording is armed: notes you play are entered at the edit cursor"));
    step_label_->setAccessibleName(tr("Step recording armed"));
    step_label_->setForegroundRole(QPalette::Highlight);
    step_label_->setContentsMargins(4, 0, 4, 0);
    statusBar()->addPermanentWidget(step_label_);
    step_label_->hide();
    // Readouts are permanent sections: transient messages never hide them.
    cursor_label_->setFont(mono_font());
    cursor_label_->setMinimumWidth(190);
    cursor_label_->setToolTip(tr("Position and pitch under the mouse"));
    cursor_label_->setText(tr("Pointer: —"));
    bool first_readout = true;
    for (auto* label : {cursor_label_, selection_label_, snap_label_, device_label_, recovery_label_}) {
        if (!first_readout) {
            auto* separator = new QFrame(this);
            separator->setFrameShape(QFrame::VLine);
            separator->setFrameShadow(QFrame::Plain);
            separator->setForegroundRole(QPalette::Mid);
            statusBar()->addPermanentWidget(separator);
        }
        first_readout = false;
        label->setContentsMargins(4, 0, 4, 0);
        statusBar()->addPermanentWidget(label);
    }
    {
        // The tip names the current bindings (they can be changed in Keyboard Shortcuts).
        const auto key = [this](const char* id) {
            const auto* action = command(id);
            return action ? action->shortcut().toString(QKeySequence::NativeText) : QString();
        };
        statusBar()->showMessage(tr("Tip: %1 finds any command; %2 plays and stops; press %3 to draw notes")
                                     .arg(key("view.command_palette"), key("transport.play_stop"), key("tool.draw")),
                                 15000);
    }

    connect(tabs_, &QTabWidget::currentChanged, this, &MainWindow::current_tab_changed);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &MainWindow::close_tab);
    connect(computer_keyboard_, &ComputerKeyboardPlayer::note_on, this, [this](int pitch, int velocity) {
        if (auto* tab = current_tab()) {
            tab->keyboard().set_key_state(pitch, KeySource::computer_keyboard, true);
            const auto active = tab->controller().active_track_index();
            const auto& sequence = tab->controller().active_sequence();
            playback_->audition_note_on(pitch, velocity, 0,
                                        active ? sequence.tracks[*active].instrument.synth_preset : "classic");
        }
        recording_->live_note(pitch, velocity, true);
    });
    connect(computer_keyboard_, &ComputerKeyboardPlayer::note_off, this, [this](int pitch) {
        if (auto* tab = current_tab())
            tab->keyboard().set_key_state(pitch, KeySource::computer_keyboard, false);
        playback_->audition_note_off(pitch, 0);
        recording_->live_note(pitch, 0, false);
    });
    connect(computer_keyboard_, &ComputerKeyboardPlayer::state_changed, this, [this](int root, int velocity) {
        statusBar()->showMessage(tr("Playing mode: octave root %1, velocity %2")
                                     .arg(QString::fromStdString(pitch_name(static_cast<std::uint8_t>(root))))
                                     .arg(velocity),
                                 3000);
        // The on-screen keyboard shows the octaves the letter keys now play (ux-editing-33).
        if (auto* tab = current_tab())
            tab->keyboard().set_view_start(std::clamp(root - 12, 0, 127));
    });
    transport_timer_.setInterval(33); // display sampling only (section 13.4)
    connect(&transport_timer_, &QTimer::timeout, this, &MainWindow::update_status_from_transport);
    transport_timer_.start();
    recovery_timer_.setInterval(2000); // flush completed edits within 2 s (section 21.2)
    connect(&recovery_timer_, &QTimer::timeout, this, &MainWindow::flush_recovery);
    recovery_timer_.start();

    QSettings settings;
    if (settings.value("layout/schema").toInt() == layout_schema_version) {
        restoreGeometry(settings.value("layout/geometry").toByteArray());
        restoreState(settings.value("layout/state").toByteArray(), layout_schema_version);
    } else {
        // First run (or an older layout): Compose layout with an editor-first split.
        resize(1400, 860);
        apply_layout("compose");
        resizeDocks({inspector_dock_}, {300}, Qt::Horizontal);
    }
    // Keep floating panels on an available monitor (section 6.1).
    for (auto* dock : {library_dock_, inspector_dock_, events_dock_}) {
        if (dock->isFloating()) {
            const auto* screen = QGuiApplication::screenAt(dock->geometry().center());
            if (!screen)
                dock->setFloating(false);
        }
    }

    for (const auto& file : options_.files)
        open_path(file);
    if (options_.files.empty())
        new_from_template("blank"); // with files, a blank appears only if every open fails
    update_status_from_transport(); // the position readout is filled before the first timer tick
    if (!options_.skip_recovery_prompt && !options_.exit_when_ready)
        QTimer::singleShot(0, this, [this] { offer_recovery(false); });
    if (options_.exit_when_ready || !options_.screenshot.empty())
        QTimer::singleShot(0, this, &MainWindow::write_screenshot_and_exit);
}

MainWindow::~MainWindow() {
    transport_timer_.stop();
    recovery_timer_.stop();
    // The recording controller's input monitor thread sends to the playback service: stop it
    // before the service (a member) goes, not later with the child objects.
    delete recording_;
    recording_ = nullptr;
    // Children are deleted after the members, playback_ among them: nothing that calls into
    // playback_ may still be connected then (code-uiA-15).
    if (computer_keyboard_)
        disconnect(computer_keyboard_, nullptr, this, nullptr);
    for (int index = 0; index < tabs_->count(); ++index)
        if (auto* tab = qobject_cast<EditorTab*>(tabs_->widget(index))) {
            disconnect(&tab->piano_roll(), nullptr, this, nullptr);
            disconnect(&tab->keyboard(), nullptr, this, nullptr);
            disconnect(tab, nullptr, this, nullptr);
        }
    if (tempo_multiplier_)
        disconnect(tempo_multiplier_, nullptr, this, nullptr);
    playback_->panic(false);
    io_thread_.reset(); // joins a pending open or export
    drain_recovery_thread(); // queued clean closes land before the process ends
}

EditorTab* MainWindow::current_tab() const { return qobject_cast<EditorTab*>(tabs_->currentWidget()); }

// --- Commands ------------------------------------------------------------------------------------

QAction* MainWindow::add_command(const QString& id, const QString& text, const QKeySequence& shortcut,
                                 std::function<void()> handler, Qt::ShortcutContext context) {
    auto* action = new QAction(text, this);
    action->setObjectName(id);
    action->setData(id);
    if (!shortcut.isEmpty())
        action->setShortcut(shortcut);
    action->setProperty("mwb_default_shortcut", QVariant::fromValue(shortcut));
    action->setShortcutContext(context);
    connect(action, &QAction::triggered, this, [handler = std::move(handler)] { handler(); });
    addAction(action);
    commands_.push_back(action);
    return action;
}

void MainWindow::build_actions() {
    const auto with_tab = [this](auto function) {
        return [this, function] {
            if (auto* tab = current_tab())
                function(*tab);
        };
    };
    add_command("file.new", tr("&New Blank Document"), QKeySequence::New, [this] { new_from_template("blank"); });
    for (const auto& [name, label] : {std::pair{"melody", tr("New Melody")}, std::pair{"chords", tr("New Chords")},
                                      std::pair{"bassline", tr("New Bassline")}, std::pair{"drums", tr("New Drums")}})
        add_command(QString("file.new_%1").arg(name), label, {}, [this, template_name = QString(name)] {
            new_from_template(template_name);
        });
    add_command("file.open", tr("&Open…"), QKeySequence::Open, [this] {
        const auto files = QFileDialog::getOpenFileNames(this, tr("Open"), {},
                                                         tr("MIDI and MIDI Workbench files (*.mid *.midi *.rmi *.mwb);;All files (*)"));
        for (const auto& file : files)
            open_path(to_path(file));
    });
    add_command("file.save", tr("&Save"), QKeySequence(Qt::CTRL | Qt::Key_S), [this] { save_current(false); });
    add_command("file.save_as", tr("Save &As…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), [this] { save_current(true); });
    add_command("file.export_midi", tr("&Export MIDI…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E),
                [this] { export_current_midi(); });
    add_command("file.close", tr("&Close Document"), QKeySequence::Close, [this] { close_tab(tabs_->currentIndex()); });
    add_command("file.recover", tr("Recover Sessions…"), {}, [this] { offer_recovery(true); });
    add_command("file.quit", tr("E&xit"), QKeySequence::Quit, [this] { close(); });

    undo_action_ = add_command("edit.undo", tr("&Undo"), QKeySequence(Qt::CTRL | Qt::Key_Z),
                               with_tab([](EditorTab& tab) { tab.controller().undo(); }));
    redo_action_ = add_command("edit.redo", tr("&Redo"), QKeySequence(Qt::CTRL | Qt::Key_Y),
                               with_tab([](EditorTab& tab) { tab.controller().redo(); }));
    add_command("edit.cut", tr("Cu&t"), QKeySequence(Qt::CTRL | Qt::Key_X), [this] { copy_selection(true); });
    add_command("edit.copy", tr("&Copy"), QKeySequence(Qt::CTRL | Qt::Key_C), [this] { copy_selection(false); });
    add_command("edit.paste", tr("&Paste at Edit Cursor"), QKeySequence(Qt::CTRL | Qt::Key_V), [this] { paste_at_cursor(); });
    add_command("edit.duplicate", tr("&Duplicate After Selection"), QKeySequence(Qt::CTRL | Qt::Key_D),
                [this] { duplicate_selection(); });
    add_command("edit.delete", tr("De&lete"), QKeySequence(Qt::Key_Delete), [this] { delete_selection(); })
        ->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
    add_command("edit.select_all", tr("Select &All"), QKeySequence(Qt::CTRL | Qt::Key_A),
                with_tab([](EditorTab& tab) { tab.piano_roll().select_all(); }));
    add_command("edit.invert_selection", tr("&Invert Selection"), {},
                with_tab([](EditorTab& tab) { tab.piano_roll().invert_selection(); }));
    add_command("edit.select_out_of_scale", tr("Select Out-of-Scale Notes"), {},
                with_tab([](EditorTab& tab) { tab.piano_roll().select_out_of_scale(); }));
    add_command("edit.transpose_up", tr("Transpose Up a Semitone"), QKeySequence(Qt::Key_Up), [this] { transpose_selection(1); });
    add_command("edit.transpose_down", tr("Transpose Down a Semitone"), QKeySequence(Qt::Key_Down),
                [this] { transpose_selection(-1); });
    add_command("edit.octave_up", tr("Transpose Up an Octave"), QKeySequence(Qt::SHIFT | Qt::Key_Up),
                [this] { transpose_selection(12); });
    add_command("edit.octave_down", tr("Transpose Down an Octave"), QKeySequence(Qt::SHIFT | Qt::Key_Down),
                [this] { transpose_selection(-12); });
    add_command("edit.degree_up", tr("Move Up a Scale Degree"), QKeySequence(Qt::CTRL | Qt::Key_Up),
                [this] { transpose_selection_by_degree(1); });
    add_command("edit.degree_down", tr("Move Down a Scale Degree"), QKeySequence(Qt::CTRL | Qt::Key_Down),
                [this] { transpose_selection_by_degree(-1); });
    // Left/Right: with notes selected they move the notes; otherwise they move the red transport
    // line (the play position) by a step that follows the zoom, also while playing.
    add_command("edit.move_left", tr("Move Notes or Play Position Left"), QKeySequence(Qt::Key_Left),
                [this] { move_selection_by_grid(-1); });
    add_command("edit.move_right", tr("Move Notes or Play Position Right"), QKeySequence(Qt::Key_Right),
                [this] { move_selection_by_grid(1); });
    // Keyboard selection and length (ux-editing-26): Alt+Left/Right step through the notes,
    // Shift+Left/Right change the selected notes' length by the grid.
    add_command("edit.select_previous_note", tr("Select Previous Note"), QKeySequence(Qt::ALT | Qt::Key_Left), [this] {
        if (auto* tab = current_tab())
            tab->piano_roll().select_adjacent_note(-1);
    });
    add_command("edit.select_next_note", tr("Select Next Note"), QKeySequence(Qt::ALT | Qt::Key_Right), [this] {
        if (auto* tab = current_tab())
            tab->piano_roll().select_adjacent_note(1);
    });
    add_command("edit.shorten", tr("Shorten Notes by Grid"), QKeySequence(Qt::SHIFT | Qt::Key_Left), [this] {
        if (auto* tab = current_tab())
            tab->piano_roll().change_length_by_grid(-1);
    });
    add_command("edit.lengthen", tr("Lengthen Notes by Grid"), QKeySequence(Qt::SHIFT | Qt::Key_Right), [this] {
        if (auto* tab = current_tab())
            tab->piano_roll().change_length_by_grid(1);
    });
    // Ctrl+Left/Right always move the play position (Space then plays from there), whether or not
    // notes are selected. The step follows the zoom and grows while the key is held.
    add_command("transport.position_left", tr("Play Position Left"), QKeySequence(Qt::CTRL | Qt::Key_Left),
                [this] { move_play_position(-1); });
    add_command("transport.position_right", tr("Play Position Right"), QKeySequence(Qt::CTRL | Qt::Key_Right),
                [this] { move_play_position(1); });
    add_command("edit.quantize_last", tr("Apply Last &Quantize"), QKeySequence(Qt::Key_Q), [this] { inspector_->apply_last_quantize(); });
    add_command("edit.quantize_panel", tr("Quantize Panel…"), QKeySequence(Qt::CTRL | Qt::Key_Q), [this] {
        inspector_dock_->show();
        inspector_->focus_tool("quantize");
    });

    add_command("tool.toggle_draw", tr("Toggle Draw/Select Tool"), QKeySequence(Qt::Key_B), with_tab([](EditorTab& tab) {
        tab.piano_roll().set_tool(tab.piano_roll().tool() == EditTool::draw ? EditTool::select : EditTool::draw);
    }));
    tool_group_ = new QActionGroup(this);
    tool_group_->setExclusive(true);
    const std::tuple<const char*, QString, Qt::Key, IconId, EditTool> tools[] = {
        {"tool.select", tr("Select Tool"), Qt::Key_1, IconId::select, EditTool::select},
        {"tool.draw", tr("Draw Tool"), Qt::Key_2, IconId::draw, EditTool::draw},
        {"tool.erase", tr("Erase Tool"), Qt::Key_3, IconId::erase, EditTool::erase},
        {"tool.split", tr("Split Tool"), Qt::Key_4, IconId::split, EditTool::split},
        {"tool.audition", tr("Audition Tool"), Qt::Key_5, IconId::audition, EditTool::audition},
    };
    for (const auto& [id, text, key, icon, tool] : tools) {
        auto* action = add_command(id, text, QKeySequence(key), with_tab([tool](EditorTab& tab) { tab.piano_roll().set_tool(tool); }));
        action->setCheckable(true);
        action->setIcon(make_icon(icon));
        action->setProperty("mwb_icon", static_cast<int>(icon));
        action->setProperty("mwb_tool", static_cast<int>(tool));
        tool_group_->addAction(action);
    }

    add_command("view.fit", tr("&Fit Selection or All"), QKeySequence(Qt::Key_F),
                with_tab([](EditorTab& tab) { tab.piano_roll().fit_selection_or_all(); }));
    add_command("view.fit_all", tr("Fit All"), {}, with_tab([](EditorTab& tab) { tab.piano_roll().fit_all(); }));
    add_command("view.previous_zoom", tr("Previous Zoom"), {}, with_tab([](EditorTab& tab) { tab.piano_roll().return_to_previous_zoom(); }));
    add_command("view.zoom_in", tr("Zoom In"), QKeySequence::ZoomIn, with_tab([](EditorTab& tab) { tab.piano_roll().zoom_time(1.5); }));
    add_command("view.zoom_out", tr("Zoom Out"), QKeySequence::ZoomOut, with_tab([](EditorTab& tab) { tab.piano_roll().zoom_time(1 / 1.5); }));
    add_command("view.drum_grid", tr("Toggle Drum Grid"), QKeySequence(Qt::CTRL | Qt::Key_G), with_tab([](EditorTab& tab) { tab.show_drum_grid(!tab.showing_drum_grid()); }));
    add_command("view.add_cc_lane", tr("Add Controller Lane…"), {}, with_tab([this](EditorTab& tab) {
        bool ok_value = false;
        // Velocity can come back after it was removed; lanes start on the track's own channel
        // (r5ui-16).
        const QStringList kinds = {tr("CC number…"), tr("Pitch bend"), tr("Channel pressure"), tr("Release velocity"),
                                   tr("Velocity")};
        const QString kind = QInputDialog::getItem(this, tr("Add lane"), tr("Lane"), kinds, 0, false, &ok_value);
        if (!ok_value)
            return;
        LaneTarget target;
        const auto selected = tab.controller().selected_notes();
        target.channel = selected.empty() ? default_channel_for(tab.controller()) : selected.front().channel;
        if (kind == kinds[4]) {
            target.kind = LaneTarget::Kind::velocity;
        } else if (kind == kinds[0]) {
            target.kind = LaneTarget::Kind::controller;
            target.controller = static_cast<std::uint8_t>(QInputDialog::getInt(this, tr("Controller"), tr("CC number"), 1, 0, 127, 1, &ok_value));
            if (!ok_value)
                return;
        } else if (kind == kinds[1]) {
            target.kind = LaneTarget::Kind::pitch_bend;
        } else if (kind == kinds[2]) {
            target.kind = LaneTarget::Kind::channel_pressure;
        } else {
            target.kind = LaneTarget::Kind::release_velocity;
        }
        tab.add_lane(target);
    }));
    add_command("view.remove_lane", tr("Remove Last Lane"), {}, with_tab([](EditorTab& tab) { tab.remove_last_lane(); }));
    add_command("view.layout_compose", tr("Compose Layout"), {}, [this] { apply_layout("compose"); });
    add_command("view.layout_library", tr("Library Layout"), {}, [this] { apply_layout("library"); });
    add_command("view.layout_inspect", tr("Inspect Layout"), {}, [this] { apply_layout("inspect"); });
    add_command("view.layout_reset", tr("Reset Layout"), {}, [this] { reset_layout(); });
    add_command("view.command_palette", tr("Command Palette…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P),
                [this] { show_command_palette(); });
    for (const bool similar : {false, true})
        add_command(similar ? "library.find_similar" : "library.find_melody",
                    similar ? tr("Find Similar Phrases in Library") : tr("Find Selected Melody in Library"), {},
                    [this, similar] {
                        auto* tab = current_tab();
                        if (!tab)
                            return;
                        auto notes = tab->controller().selected_notes();
                        std::sort(notes.begin(), notes.end(), [](const NoteSpan& a, const NoteSpan& b) {
                            return a.start != b.start ? a.start < b.start : a.pitch > b.pitch;
                        });
                        // One note per onset (the top voice) forms the melody query.
                        std::vector<std::pair<Tick, std::uint8_t>> onsets;
                        std::vector<Tick> durations;
                        for (const auto& note : notes) {
                            if (!onsets.empty() && onsets.back().first == note.start)
                                continue;
                            onsets.emplace_back(note.start, note.pitch);
                            durations.push_back(note.duration());
                        }
                        library_dock_->show();
                        library_->search_melody(std::move(onsets), std::move(durations), similar);
                    });
    add_command("library.save", tr("Save to Library"), {}, [this] {
        auto* tab = current_tab();
        if (!tab)
            return;
        const auto source = library_sources_.find(tab);
        const bool known = source != library_sources_.end();
        library_->save_document(tab->controller().shared_snapshot(),
                                known ? std::optional<std::int64_t>(source->second.first) : std::nullopt,
                                known ? std::optional<std::int64_t>(source->second.second) : std::nullopt,
                                QString::fromStdString(tab->controller().document().title), false, tab);
    });
    add_command("library.save_selection", tr("Save Selection to Library as Phrase"), {}, [this] {
        auto* tab = current_tab();
        if (!tab || tab->controller().selection().empty())
            return;
        auto& controller = tab->controller();
        const auto notes = controller.selected_notes();
        const auto extent = grid_aligned_extent(notes, tab->piano_roll().grid());
        DocumentData phrase = make_blank_document(controller.document().title + " phrase");
        auto& sequence = phrase.sequences.front();
        sequence.division = controller.active_sequence().division;
        sequence.root_pc = controller.active_sequence().root_pc;
        sequence.scale_mask = controller.active_sequence().scale_mask;
        std::vector<NewNote> created;
        for (const auto& note : notes)
            created.push_back(NewNote{sequence.tracks.front().id, note.start - extent.begin, note.end - extent.begin,
                                      note.pitch, note.velocity, note.channel, note.release_velocity});
        auto changes = insert_notes(phrase, created, "Extract phrase");
        if (!changes || !apply_change_set(phrase, *changes)) {
            statusBar()->showMessage(tr("Could not extract the selection"), 5000);
            return;
        }
        sequence.end_tick = std::max(sequence.end_tick, extent.length());
        sequence.regions.front().end_tick = extent.length();
        library_->save_document(std::make_shared<const DocumentData>(std::move(phrase)), std::nullopt, std::nullopt,
                                QString::fromStdString(controller.document().title + " phrase"), true);
    });
    add_command("library.revisions", tr("Revisions of Selected Library Item…"), {}, [this] { library_->show_revisions(); });
    add_command("view.focus_search", tr("Focus Search"), QKeySequence(Qt::CTRL | Qt::Key_F), [this] {
        library_dock_->show();
        library_->focus_search();
    });
    playing_mode_action_ = add_command("view.playing_mode", tr("Computer Keyboard Playing Mode"), {}, [this] {
        computer_keyboard_->set_enabled(playing_mode_action_->isChecked());
        // Playing mode visibly suppresses unmodified letter commands (section 7.5).
        for (auto* action : commands_) {
            const auto shortcut = action->shortcut();
            const bool letter = shortcut.count() == 1 && (shortcut[0].keyboardModifiers() == Qt::NoModifier) &&
                                shortcut[0].key() >= Qt::Key_A && shortcut[0].key() <= Qt::Key_Z;
            if (letter)
                action->setEnabled(!playing_mode_action_->isChecked());
        }
        if (keyboard_action_ && playing_mode_action_->isChecked())
            keyboard_action_->setChecked(true); // show the keys being played
        statusBar()->showMessage(playing_mode_action_->isChecked()
                                     ? tr("Playing mode on: A W S E D F T G Y H U J K play notes; Z/X octave; C/V velocity")
                                     : tr("Playing mode off"),
                                 5000);
        update_command_state(); // letters re-enabled only where they apply (Loop needs a loop region)
    });
    playing_mode_action_->setCheckable(true);

    // Space plays and stops (returning to where playback started), as in a DAW; Ctrl+Space pauses
    // and resumes in place.
    add_command("transport.play_stop", tr("Play/Stop"), QKeySequence(Qt::Key_Space), [this] { play_stop(); });
    add_command("transport.pause_resume", tr("Pause/Resume"), QKeySequence(Qt::CTRL | Qt::Key_Space),
                [this] { pause_resume(); });
    add_command("transport.play_selection", tr("Play from Selection"), QKeySequence(Qt::SHIFT | Qt::Key_Space),
                [this] { play_from_selection(); });
    add_command("transport.stop", tr("Stop and Return"), {}, [this] { stop_transport(); });
    add_command("transport.go_to_start", tr("Go to Start"), QKeySequence(Qt::Key_Home), [this] { go_to_start(); });
    add_command("transport.go_to_end", tr("Go to End"), QKeySequence(Qt::Key_End), [this] { go_to_end(); });
    loop_action_ = add_command("transport.loop", tr("Loop"), QKeySequence(Qt::Key_L), [this] { publish_playback(); });
    loop_action_->setCheckable(true);
    loop_action_->setChecked(true);
    add_command("transport.loop_selection", tr("Loop Selection"), QKeySequence(Qt::CTRL | Qt::Key_L), [this] {
        if (auto* tab = current_tab())
            tab->loop_selection(); // the grid-aligned extent of the selected notes (ux-editing-9)
    });
    loop_action_->setProperty("mwb_keep_tooltip", true);
    metronome_action_ = add_command("transport.metronome", tr("Metronome"), QKeySequence(Qt::Key_K), [this] {
        playback_->set_metronome(metronome_action_->isChecked());
    });
    metronome_action_->setCheckable(true);
    metronome_action_->setToolTip(tr("Metronome click during playback (K)"));
    metronome_action_->setProperty("mwb_keep_tooltip", true);
    add_command("transport.panic", tr("Panic (all app notes off)"), QKeySequence(Qt::Key_F12), [this] {
        playback_->panic(true);
        statusBar()->showMessage(tr("Panic sent to app-owned routes"), 3000);
    }, Qt::ApplicationShortcut);
    record_action_ = add_command("transport.record", tr("Record"), QKeySequence(Qt::Key_R), [this] {
        if (auto* tab = current_tab(); tab && !recording_->recording()) {
            if (!transport_display_is_stopped(playback_->display()) && transport_owner_ != &tab->controller())
                stop_transport(); // another document owns the running transport
            const auto state = playback_->display().state;
            if (transport_display_is_stopped(playback_->display()) || state == TransportDisplay::State::paused) {
                transport_owner_ = &tab->controller();
                publish_playback(); // recording plays the current document from the count-in
                play_started_from_ = tab->controller().edit_cursor(); // Space returns here
                paused_cursor_.reset();
            }
            // Otherwise this document is playing: the take punches in at the playhead.
        }
        recording_->toggle_recording();
        record_action_->setChecked(recording_->recording()); // also when the take could not start
    });
    record_action_->setCheckable(true);
    connect(recording_, &RecordingController::recording_changed, record_action_, &QAction::setChecked);
    step_action_ = add_command("transport.step_record", tr("Step Recording"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R),
                               [this] { recording_->set_step_recording(step_action_->isChecked()); });
    step_action_->setCheckable(true);
    step_action_->setToolTip(tr("Step recording: enter notes one step at a time at the edit cursor"));
    // The check mark always shows the real state (arming can fail; switching documents disarms).
    connect(recording_, &RecordingController::step_recording_changed, this, [this](bool armed) {
        step_action_->setChecked(armed);
        if (step_label_)
            step_label_->setVisible(armed);
    });
    add_command("transport.step_rest", tr("Step Rest"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Right), [this] { recording_->step_rest(); });
    add_command("transport.step_tie", tr("Step Tie"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_T), [this] { recording_->step_tie(); });
    add_command("transport.step_back", tr("Step Back"), QKeySequence(Qt::CTRL | Qt::Key_Backspace), [this] { recording_->step_back(); });
    {
        // One checked choice, remembered across sessions, so the menu shows the current count-in.
        auto* count_in_group = new QActionGroup(this);
        const int saved = std::clamp(QSettings().value("recording/count_in_bars", 1).toInt(), 0, 2);
        recording_->set_count_in_bars(saved);
        for (int bars = 0; bars <= 2; ++bars) {
            auto* action = add_command(QString("transport.count_in_%1").arg(bars), tr("Count-in %n bar(s)", nullptr, bars), {},
                                       [this, bars] {
                                           recording_->set_count_in_bars(bars);
                                           QSettings().setValue("recording/count_in_bars", bars);
                                       });
            action->setCheckable(true);
            action->setChecked(bars == saved);
            count_in_group->addAction(action);
        }
    }
    {
        // Measured input latency (section 8.4 rule 4), applied once to every recorded message.
        const int saved = std::clamp(QSettings().value("recording/latency_ms", 0).toInt(), 0, 500);
        recording_->set_latency_compensation(std::chrono::milliseconds(saved));
        add_command("transport.input_latency", tr("Input Latency Compensation…"), {}, [this] {
            bool accepted = false;
            const int current = QSettings().value("recording/latency_ms", 0).toInt();
            const int value = QInputDialog::getInt(this, tr("Input Latency Compensation"),
                                                   tr("Milliseconds to move recorded notes earlier (0 when unsure):"),
                                                   current, 0, 500, 1, &accepted);
            if (!accepted)
                return;
            QSettings().setValue("recording/latency_ms", value);
            recording_->set_latency_compensation(std::chrono::milliseconds(value));
            statusBar()->showMessage(tr("Input latency compensation: %1 ms").arg(value), 4000);
        });
    }
    add_command("transport.midi_input", tr("Choose MIDI Input…"), {}, [this] {
        // Listing and opening run on the recording controller's device thread; the dialog opens
        // when the list arrives (the window never waits for a MIDI driver).
        statusBar()->showMessage(tr("Looking for MIDI inputs…"), 3000);
        recording_->list_input_ports([this](RecordingController::PortList ports, QString error) {
            if (ports.empty() && !error.isEmpty()) {
                QMessageBox::warning(this, tr("MIDI Input"), error);
                return;
            }
            QStringList names{tr("None")};
            for (const auto& [id, name] : ports)
                names << name;
            // The input in use is preselected, not "None" (ux-panels-42).
            const int current = std::max<int>(0, static_cast<int>(names.indexOf(recording_->input_name())));
            bool chosen = false;
            const QString name = QInputDialog::getItem(this, tr("MIDI Input"), tr("Input device"), names, current, false, &chosen);
            if (!chosen)
                return;
            QString id;
            for (const auto& [port_id, port_name] : ports)
                if (port_name == name)
                    id = port_id;
            recording_->open_input(id);
        });
    });
    add_command("transport.audio_output", tr("Audio &Output…"), {}, [this] {
        const auto devices = playback_->audio_devices();
        QStringList names{tr("Default output")};
        for (const auto& device : devices)
            names << device.name;
        const QString saved = QSettings().value("audio/device").toString();
        int current = 0;
        for (std::size_t index = 0; index < devices.size(); ++index)
            if (QString::fromStdString(devices[index].id) == saved)
                current = static_cast<int>(index) + 1;
        bool chosen = false;
        const QString name = QInputDialog::getItem(this, tr("Audio Output"), tr("Output device"), names, current, false, &chosen);
        if (!chosen)
            return;
        std::optional<std::string> id;
        for (const auto& device : devices)
            if (device.name == name)
                id = device.id;
        QSettings().setValue("audio/device", id ? QString::fromStdString(*id) : QString());
        playback_->reopen_device(id); // opens in the background; the status bar reports the result
    });
    add_command("transport.reconnect_audio", tr("Reconnect Audio Output"), {}, [this] {
        const QString saved = QSettings().value("audio/device").toString();
        playback_->reopen_device(saved.isEmpty() ? std::nullopt : std::optional<std::string>(saved.toStdString()));
    });
    add_command("help.about", tr("&About MIDI Workbench"), {}, [this] { show_about(); });
    add_command("help.shortcuts", tr("Keyboard &Shortcuts…"), {}, [this] {
        ShortcutEditor editor(commands_, this);
        if (editor.exec() == QDialog::Accepted) {
            QSettings().setValue("shortcuts/profile", shortcuts_to_json(commands_));
            refresh_tooltips();
        }
    });
    add_command("help.diagnostics", tr("&Diagnostics…"), {}, [this] { show_diagnostics(); });
}

void MainWindow::build_menus() {
    const auto find = [this](const char* id) -> QAction* {
        for (auto* action : commands_)
            if (action->data().toString() == QLatin1String(id))
                return action;
        return nullptr;
    };
    const auto add = [&](QMenu* menu, std::initializer_list<const char*> ids) {
        menu->setToolTipsVisible(true); // disabled commands explain why
        for (const auto* id : ids) {
            if (id == nullptr)
                menu->addSeparator();
            else if (auto* action = find(id))
                menu->addAction(action);
        }
    };
    add(menuBar()->addMenu(tr("&File")), {"file.new", "file.new_melody", "file.new_chords", "file.new_bassline", "file.new_drums",
                                          nullptr, "file.open", "file.save", "file.save_as", "file.export_midi", nullptr,
                                          "file.recover", "file.close", "file.quit"});
    add(menuBar()->addMenu(tr("&Edit")), {"edit.undo", "edit.redo", nullptr, "edit.cut", "edit.copy", "edit.paste",
                                          "edit.duplicate", "edit.delete", nullptr, "edit.select_all", "edit.invert_selection",
                                          "edit.select_out_of_scale", nullptr, "edit.transpose_up", "edit.transpose_down",
                                          "edit.octave_up", "edit.octave_down", "edit.degree_up", "edit.degree_down",
                                          "edit.move_left", "edit.move_right", nullptr, "edit.quantize_last",
                                          "edit.quantize_panel"});
    add(menuBar()->addMenu(tr("&Tools")), {"tool.select", "tool.draw", "tool.erase", "tool.split", "tool.audition",
                                           "tool.toggle_draw", nullptr, "view.playing_mode"});
    auto* view = menuBar()->addMenu(tr("&View"));
    add(view, {"view.fit", "view.fit_all", "view.previous_zoom", "view.zoom_in", "view.zoom_out", nullptr,
               "view.drum_grid", "view.add_cc_lane", "view.remove_lane", nullptr, "view.layout_compose",
               "view.layout_library", "view.layout_inspect", "view.layout_reset", nullptr, "view.command_palette",
               "view.focus_search"});
    add(menuBar()->addMenu(tr("&Library")), {"library.save", "library.save_selection", "library.revisions",
                                             "library.find_melody", "library.find_similar"});
    view->addSeparator();
    auto* theme_menu = view->addMenu(tr("&Theme"));
    auto* theme_group = new QActionGroup(theme_menu);
    for (const auto& [name, label] : {std::pair{"system", tr("Follow Windows")}, std::pair{"dark", tr("Dark")},
                                      std::pair{"light", tr("Light")}, std::pair{"high_contrast", tr("High Contrast")}}) {
        auto* action = add_command(QString("view.theme_%1").arg(name), tr("Theme: %1").arg(label), {},
                                   [this, theme = QString(name)] { set_theme(theme); });
        action->setCheckable(true);
        action->setChecked(theme_name(current_theme()) == QLatin1String(name));
        theme_group->addAction(action);
        theme_menu->addAction(action);
    }
    auto* colors_menu = view->addMenu(tr("Note &Colors"));
    auto* colors_group = new QActionGroup(colors_menu);
    for (const auto& [mode, label] : {std::pair{NoteColorMode::track, tr("By Track")}, std::pair{NoteColorMode::channel, tr("By Channel")},
                                      std::pair{NoteColorMode::velocity, tr("By Velocity")},
                                      std::pair{NoteColorMode::scale_degree, tr("By Scale Degree")}}) {
        auto* action = add_command(QString("view.note_colors_%1").arg(static_cast<int>(mode)), tr("Note Colors: %1").arg(label), {},
                                   [this, mode] {
                                       for (int index = 0; index < tabs_->count(); ++index)
                                           if (auto* tab = qobject_cast<EditorTab*>(tabs_->widget(index)))
                                               tab->piano_roll().set_color_mode(mode);
                                   });
        action->setCheckable(true);
        action->setChecked(mode == NoteColorMode::track);
        colors_group->addAction(action);
        colors_menu->addAction(action);
    }
    // Snap mode (section 7.3: absolute and relative are named and selectable) and Off.
    snap_off_action_ = add_command("view.snap_off", tr("Snap Off"), QKeySequence(Qt::CTRL | Qt::Key_4),
                                   [this] { set_snap_off(snap_off_action_->isChecked()); });
    snap_off_action_->setCheckable(true);
    snap_relative_action_ = add_command("view.snap_relative", tr("Relative Snap (keep offsets from the grid)"), {}, [this] {
        snap_relative_ = snap_relative_action_->isChecked();
        QSettings().setValue("editor/snap_relative", snap_relative_);
        apply_snap_mode();
    });
    snap_relative_action_->setCheckable(true);
    snap_relative_ = QSettings().value("editor/snap_relative", false).toBool();
    snap_relative_action_->setChecked(snap_relative_);
    view->addSeparator();
    view->addAction(snap_off_action_);
    view->addAction(snap_relative_action_);
    follow_action_ = add_command("view.follow_playhead", tr("Follow Playhead"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), [this] {
        QSettings().setValue("editor/follow_playhead", follow_action_->isChecked());
        for (int index = 0; index < tabs_->count(); ++index)
            if (auto* tab = qobject_cast<EditorTab*>(tabs_->widget(index)))
                tab->set_follow_playhead(follow_action_->isChecked());
    });
    follow_action_->setCheckable(true);
    follow_action_->setChecked(QSettings().value("editor/follow_playhead", true).toBool());
    view->addAction(follow_action_);
    keyboard_action_ = add_command("view.onscreen_keyboard", tr("On-screen &Keyboard"), {}, [] {});
    keyboard_action_->setCheckable(true);
    view->addAction(keyboard_action_);
    connect(keyboard_action_, &QAction::toggled, this, [this](bool on) {
        for (int index = 0; index < tabs_->count(); ++index)
            if (auto* tab = qobject_cast<EditorTab*>(tabs_->widget(index)))
                tab->set_keyboard_visible(on);
    });
    view->addSeparator();
    view->addAction(library_dock_->toggleViewAction());
    view->addAction(inspector_dock_->toggleViewAction());
    view->addAction(events_dock_->toggleViewAction());
    add(menuBar()->addMenu(tr("T&ransport")), {"transport.play_stop", "transport.pause_resume", "transport.play_selection",
                                                "transport.stop", "transport.go_to_start", "transport.go_to_end",
                                                "transport.position_left", "transport.position_right",
                                                nullptr, "transport.loop", "transport.loop_selection", "transport.metronome",
                                                nullptr, "transport.record", "transport.count_in_0", "transport.count_in_1",
                                                "transport.count_in_2", "transport.input_latency", "transport.midi_input", nullptr,
                                                "transport.audio_output", "transport.reconnect_audio", nullptr,
                                                "transport.step_record", "transport.step_rest", "transport.step_tie",
                                                "transport.step_back", nullptr, "transport.panic"});
    add(menuBar()->addMenu(tr("&Help")), {"help.shortcuts", "help.diagnostics", "help.about"});
}

void MainWindow::build_docks() {
    library_ = new LibraryPanel(this);
    library_dock_ = new QDockWidget(tr("Library"), this);
    library_dock_->setObjectName("dock.library");
    library_dock_->setWidget(library_);
    addDockWidget(Qt::LeftDockWidgetArea, library_dock_);
    connect(library_, &LibraryPanel::opened, this, [this](bool) { update_command_state(); });
    connect(library_, &LibraryPanel::open_requested, this,
            [this](DocumentData document, qint64 asset, qint64 revision, std::optional<TimeRange> reveal) {
                // An item already open is shown, not opened again: two tabs saving revisions of one
                // item would conflict (ux-panels-33). A melody hit still moves to its match.
                for (int index = 0; index < tabs_->count(); ++index) {
                    auto* existing = qobject_cast<EditorTab*>(tabs_->widget(index));
                    const auto source = library_sources_.find(existing);
                    if (!existing || source == library_sources_.end() || source->second.first != asset)
                        continue;
                    tabs_->setCurrentWidget(existing);
                    if (reveal) {
                        existing->controller().set_edit_cursor(reveal->begin);
                        existing->piano_roll().scroll_to_tick(reveal->begin);
                    }
                    statusBar()->showMessage(tr("This library item is already open"), 4000);
                    return;
                }
                open_prepared(std::move(document), false, [this, asset, revision, reveal](EditorTab* tab) {
                    library_sources_[tab] = {asset, revision};
                    if (!reveal)
                        return;
                    // A melody result opens at its match, with the matched notes selected.
                    auto& controller = tab->controller();
                    std::set<EventId> matched;
                    for (const auto& note : controller.notes().notes)
                        if (note.start >= reveal->begin && note.start < reveal->end)
                            matched.insert(note.on_id);
                    controller.set_selection(std::move(matched));
                    controller.set_edit_cursor(reveal->begin);
                    tab->piano_roll().scroll_to_tick(reveal->begin);
                });
            });
    connect(library_, &LibraryPanel::preview_requested, this,
            [this](DocumentData document, QString title, std::optional<TimeRange> range) {
                preview_library_item(std::move(document), title, range);
            });
    connect(library_, &LibraryPanel::insert_requested, this, [this](DocumentData document, QString title) {
        auto* tab = current_tab();
        if (!tab) {
            statusBar()->showMessage(tr("Open a document to insert into"), 5000);
            return;
        }
        std::unique_ptr<QMimeData> mime(make_document_mime(document));
        if (!mime) {
            statusBar()->showMessage(tr("\"%1\" has no notes to insert").arg(title), 5000);
            return;
        }
        QString error;
        auto& controller = tab->controller();
        const auto created = paste_clipboard(controller, mime.get(), controller.edit_cursor(), &error);
        if (!error.isEmpty())
            statusBar()->showMessage(error, 6000);
        if (!created.empty()) {
            controller.set_selection(std::set<EventId>(created.begin(), created.end()));
            statusBar()->showMessage(tr("Inserted \"%1\" at the edit cursor (%n note(s))", nullptr,
                                        static_cast<int>(created.size()))
                                         .arg(title),
                                     5000);
        }
    });
    connect(library_, &LibraryPanel::saved, this, [this](qint64 asset, qint64 revision, bool phrase, QObject* origin) {
        // Only a whole-document save maps a tab to a library item, and only the tab that asked.
        if (phrase || !origin)
            return;
        for (int index = 0; index < tabs_->count(); ++index)
            if (tabs_->widget(index) == origin)
                library_sources_[static_cast<EditorTab*>(origin)] = {asset, revision};
    });
    connect(library_, &LibraryPanel::status_message, this, [this](const QString& text) { statusBar()->showMessage(text, 6000); });
    connect(library_, &LibraryPanel::opened, this, [this](bool interactive) {
        // Only a library the user just chose opens the panel; the one reopened at startup keeps
        // the saved layout (ux-panels-16).
        if (interactive && !library_dock_->isVisible()) {
            library_dock_->show();
            resizeDocks({library_dock_}, {280}, Qt::Horizontal);
        }
    });

    inspector_ = new InspectorPanel(this);
    inspector_dock_ = new QDockWidget(tr("Inspector"), this);
    inspector_dock_->setObjectName("dock.inspector");
    inspector_dock_->setWidget(inspector_);
    inspector_->setMinimumWidth(260);
    addDockWidget(Qt::RightDockWidgetArea, inspector_dock_);
    connect(inspector_, &InspectorPanel::status_message, this, [this](const QString& text) { statusBar()->showMessage(text, 6000); });

    events_dock_ = new QDockWidget(tr("Event List"), this);
    events_dock_->setObjectName("dock.events");
    addDockWidget(Qt::BottomDockWidgetArea, events_dock_);
    events_dock_->hide();
    connect(events_dock_, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        auto* tab = current_tab();
        if (visible && tab && !events_dock_->widget()) {
            events_dock_->setWidget(new EventListPanel(tab->controller(), events_dock_));
            // Opens with room for about ten events, not two (ux-visual-24).
            if (events_dock_->height() < 200)
                resizeDocks({events_dock_}, {240}, Qt::Vertical);
        }
        else if (!visible && events_dock_->widget()) {
            auto* old = events_dock_->widget();
            events_dock_->setWidget(nullptr);
            old->deleteLater();
        }
    });
}

QAction* MainWindow::command(const char* id) const {
    for (auto* action : commands_)
        if (action->data().toString() == QLatin1String(id))
            return action;
    return nullptr;
}

void MainWindow::build_toolbar() {
    // One main toolbar built from the commands, so shortcuts, menus and tooltips agree.
    transport_bar_ = addToolBar(tr("Main"));
    transport_bar_->setObjectName("toolbar.main");
    transport_bar_->setMovable(false);
    transport_bar_->setIconSize(QSize(20, 20));
    transport_bar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    const auto add = [this](const char* id, IconId icon) -> QAction* {
        QAction* action = command(id);
        if (!action)
            return nullptr;
        action->setIcon(make_icon(icon));
        action->setProperty("mwb_icon", static_cast<int>(icon));
        transport_bar_->addAction(action);
        if (auto* button = qobject_cast<QToolButton*>(transport_bar_->widgetForAction(action)))
            button->setAccessibleName(action->text().remove('&'));
        return action;
    };
    add("file.new", IconId::new_document);
    add("file.open", IconId::open);
    add("file.save", IconId::save);
    transport_bar_->addSeparator();
    add("edit.undo", IconId::undo);
    add("edit.redo", IconId::redo);
    transport_bar_->addSeparator();
    for (const char* id : {"tool.select", "tool.draw", "tool.erase", "tool.split", "tool.audition"})
        transport_bar_->addAction(command(id));
    auto* snap_label = new QLabel(tr(" Snap "), this);
    snap_label->setForegroundRole(QPalette::PlaceholderText);
    snap_label_action_ = transport_bar_->addWidget(snap_label);
    snap_combo_ = new QComboBox(this);
    snap_combo_->setAccessibleName(tr("Snap"));
    snap_combo_->setToolTip(tr("Grid used for drawing, moving and resizing notes"));
    // The same grids as the Inspector (ux-editing-14), then Off; a custom snap from a file is
    // added as its own item by update_command_state.
    for (const auto& choice : snap_choices())
        snap_combo_->addItem(tr(choice.label), QString::fromLatin1(choice.value));
    snap_combo_->insertSeparator(snap_combo_->count());
    snap_combo_->addItem(tr("Off"), QString::fromLatin1(snap_off_value));
    snap_combo_->setMinimumWidth(96);
    // Long labels ("1/16 quintuplet") must not widen the toolbar; the list shows them in full.
    snap_combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    snap_combo_->setMinimumContentsLength(8);
    transport_bar_->addWidget(snap_combo_);
    connect(snap_combo_, &QComboBox::activated, this, [this] {
        auto* tab = current_tab();
        if (!tab || updating_toolbar_)
            return;
        if (snap_combo_->currentData().toString() == QLatin1String(snap_off_value)) {
            set_snap_off(true);
            tab->piano_roll().setFocus();
            return;
        }
        set_snap_off(false);
        const auto snap = Rational::parse(snap_combo_->currentData().toString().toStdString());
        if (!snap)
            return;
        auto& controller = tab->controller();
        Sequence updated = sequence_header(controller.active_sequence()); // properties only, no events
        updated.settings.snap = *snap;
        ChangeBuilder builder(controller.document(), "Change grid");
        builder.set_sequence_properties(updated.id, updated);
        controller.apply(builder.finish()); // display setting only; no event changes
        tab->piano_roll().setFocus();
    });
    transport_bar_->addSeparator();
    add("transport.go_to_start", IconId::rewind);
    if (auto* play = add("transport.play_stop", IconId::play)) {
        if (auto* button = qobject_cast<QToolButton*>(transport_bar_->widgetForAction(play))) {
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        }
        play->setIconText(tr("Play"));
        play->setProperty("mwb_keep_tooltip", true); // says what Space does now (update_status_from_transport)
        play->setToolTip(tr("Play from the edit cursor (%1)").arg(play->shortcut().toString(QKeySequence::NativeText)));
    }
    add("transport.pause_resume", IconId::pause);
    add("transport.record", IconId::record);
    add("transport.step_record", IconId::step_record);
    add("transport.loop", IconId::loop);
    add("transport.metronome", IconId::metronome);
    tempo_multiplier_ = new QDoubleSpinBox(this);
    tempo_multiplier_->setRange(0.25, 4.0);
    tempo_multiplier_->setSingleStep(0.05);
    tempo_multiplier_->setValue(1.0);
    tempo_multiplier_->setPrefix(tr("× ")); // compact: the tooltip and accessible name say what it is
    tempo_multiplier_->setAccessibleName(tr("Playback tempo multiplier"));
    tempo_multiplier_->setToolTip(tr("Playback speed (does not edit the document; set the song tempo in the Inspector)"));
    transport_bar_->addWidget(tempo_multiplier_);
    connect(tempo_multiplier_, &QDoubleSpinBox::valueChanged, this, [this](double value) { playback_->set_tempo_multiplier(value); });
    transport_bar_->addSeparator();
    auto* position = new QLabel(this);
    position->setObjectName("transport.position");
    position->setFont(mono_font());
    // Fixed width: the toolbar never reflows while the readout changes.
    position->setFixedWidth(QFontMetrics(mono_font()).horizontalAdvance(QStringLiteral("000.00.000  ♩=000.0  00/00  Pause *")) + 16);
    position->setToolTip(tr("Transport position (bar.beat.tick) and state"));
    transport_bar_->addWidget(position);
    position_label_ = position;
    // Editable song tempo next to the readout: the BPM is the setting people change most.
    tempo_field_ = new TempoField(this);
    tempo_field_->setMinimumWidth(QFontMetrics(tempo_field_->font()).horizontalAdvance(QStringLiteral("000.0 BPM")) + 36);
    transport_bar_->addWidget(tempo_field_);
    connect(tempo_field_, &QDoubleSpinBox::valueChanged, this, [this] {
        // The first change of a burst fixes which tempo event the burst edits, even while playing.
        if (!tempo_edit_at_ && tempo_field_->editing())
            if (auto* tab = current_tab())
                tempo_edit_at_ = tempo_position(*tab);
    });
    connect(tempo_field_, &TempoField::tempo_committed, this, &MainWindow::commit_tempo);

    auto* spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    transport_bar_->addWidget(spacer);
    drag_handle_ = new DragHandle(directories_.drag_cache, this);
    connect(drag_handle_, &DragHandle::status_message, this, [this](const QString& text) { statusBar()->showMessage(text, 6000); });
    transport_bar_->addWidget(drag_handle_);
    // The panel toggles sit at the right end of the document tab row, where they stay visible
    // however narrow the window is (the transport toolbar no longer fits them at 1400 px).
    auto* panels = new QToolBar(tr("Panels"), this);
    panels->setObjectName("toolbar.panels");
    panels->setIconSize(QSize(16, 16));
    panels->setMovable(false);
    for (auto [dock, icon, key] : {std::tuple{library_dock_, IconId::library, Qt::Key_1},
                                   std::tuple{inspector_dock_, IconId::inspector, Qt::Key_2}}) {
        QAction* toggle = dock->toggleViewAction();
        toggle->setIcon(make_icon(icon));
        toggle->setProperty("mwb_icon", static_cast<int>(icon));
        toggle->setShortcut(QKeySequence(Qt::CTRL | key));
        panels->addAction(toggle);
        if (auto* button = qobject_cast<QToolButton*>(panels->widgetForAction(toggle)))
            button->setAccessibleName(tr("%1 panel").arg(dock->windowTitle()));
    }
    // The scale controls share the tab row: always in view, no extra toolbar height.
    scale_bar_ = new ScaleBar(this);
    connect(scale_bar_, &ScaleBar::select_outside_requested, this, [this] {
        if (auto* tab = current_tab())
            tab->piano_roll().select_out_of_scale();
    });
    auto* tab_corner = new QWidget(this);
    auto* tab_corner_layout = new QHBoxLayout(tab_corner);
    tab_corner_layout->setContentsMargins(0, 0, 0, 0);
    tab_corner_layout->setSpacing(0);
    tab_corner_layout->addWidget(scale_bar_);
    tab_corner_layout->addWidget(panels);
    tabs_->setCornerWidget(tab_corner, Qt::TopRightCorner);
    events_dock_->toggleViewAction()->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_3));
    // Panel toggles are commands too, so the command palette finds them.
    for (auto [dock, id] : {std::pair{library_dock_, "view.panel_library"}, std::pair{inspector_dock_, "view.panel_inspector"},
                            std::pair{events_dock_, "view.panel_events"}}) {
        QAction* toggle = dock->toggleViewAction();
        toggle->setObjectName(id);
        toggle->setData(QString::fromLatin1(id));
        toggle->setText(tr("Show %1 Panel").arg(dock->windowTitle()));
        commands_.push_back(toggle);
    }
    command_search_ = new QLineEdit(this);
    command_search_->setPlaceholderText(tr("Search commands  (Ctrl+Shift+P)"));
    command_search_->setAccessibleName(tr("Search commands"));
    command_search_->setFixedWidth(230);
    command_search_->setFixedHeight(std::max(22, menuBar()->sizeHint().height() - 4));
    command_search_->setClearButtonEnabled(false);
    // In the menu bar corner (keeps the toolbar uncrowded), inside margins so it is never clipped.
    auto* corner = new QWidget(this);
    auto* corner_layout = new QHBoxLayout(corner);
    corner_layout->setContentsMargins(0, 2, 8, 2);
    corner_layout->addWidget(command_search_);
    menuBar()->setCornerWidget(corner, Qt::TopRightCorner);
    // Typing or clicking here opens the command palette (the palette does the searching).
    connect(command_search_, &QLineEdit::textEdited, this, [this](const QString& text) {
        command_search_->clear();
        show_command_palette(text);
    });
    connect(command_search_, &QLineEdit::returnPressed, this, [this] { show_command_palette(); });
    refresh_tooltips();
}

void MainWindow::refresh_tooltips() {
    for (QAction* action : transport_bar_->actions()) {
        if (action->isSeparator() || action->text().isEmpty())
            continue;
        const QString name = action->text().remove('&');
        const QKeySequence shortcut = action->shortcut();
        if (!action->isEnabled() || action->property("mwb_keep_tooltip").toBool())
            continue; // a disabled command keeps its explanation (update_command_state)
        action->setToolTip(shortcut.isEmpty() ? name : QString("%1 (%2)").arg(name, shortcut.toString(QKeySequence::NativeText)));
    }
}

void MainWindow::set_theme(const QString& name) {
    apply_theme(theme_from_name(name));
    QSettings().setValue("ui/theme", name);
    // Icons are painted in the palette's text color: repaint them for the new theme.
    const auto refresh = [](QAction* action) {
        bool ok = false;
        const int icon = action->property("mwb_icon").toInt(&ok);
        if (ok && action->property("mwb_icon").isValid())
            action->setIcon(make_icon(static_cast<IconId>(icon)));
    };
    for (auto* action : commands_)
        refresh(action);
    for (auto* action : transport_bar_->actions())
        refresh(action);
    for (auto* widget : findChildren<QWidget*>())
        widget->update();
}

// --- Documents -----------------------------------------------------------------------------------

void MainWindow::open_document(DocumentData document, std::optional<std::filesystem::path> path, bool recovered,
                               std::shared_ptr<document::RecoveryJournal> prepared_journal, bool journal_prepared) {
    const auto origin = recovered ? document::SessionOrigin::recovered
                        : path   ? document::SessionOrigin::opened_file
                                 : document::SessionOrigin::new_document;
    const QString title = QString::fromStdString(document.title);
    auto session = std::make_unique<document::WorkingSession>(std::move(document), origin, path);
    auto controller = std::make_unique<DocumentController>(std::move(session));
    document::SourceReference source;
    source.document_name = title.toStdString();
    if (path) {
        const auto utf8 = path->u8string();
        source.file_path_utf8 = std::string(utf8.begin(), utf8.end());
    }
    // A journal prepared on the I/O thread (its first checkpoint serializes the whole document)
    // is used as is; small new documents create theirs here.
    // Documents prepared on the I/O thread never create their journal here, even when that failed:
    // the GUI thread would serialize the whole document.
    auto journal = prepared_journal || journal_prepared
                       ? Result<std::unique_ptr<document::RecoveryJournal>>(nullptr)
                       : document::RecoveryJournal::create(directories_.recovery, controller->document(), source);
    if (prepared_journal)
        controller->attach_recovery(std::move(prepared_journal));
    else if (journal_prepared)
        statusBar()->showMessage(tr("Recovery is unavailable for this document (the recovery folder could not be written)"), 10000);
    else if (journal)
        controller->attach_recovery(std::shared_ptr<document::RecoveryJournal>(std::move(*journal)));
    else
        statusBar()->showMessage(tr("Recovery journal unavailable: %1").arg(QString::fromStdString(journal.error().describe())), 10000);
    auto* tab = new EditorTab(std::move(controller));
    connect_tab(tab);
    const int index = tabs_->addTab(tab, title + (recovered ? tr(" (recovered)") : QString()));
    // A quiet close button in the theme's text color (the style's default is a red square).
    auto* close = new QToolButton(tabs_);
    close->setAutoRaise(true);
    close->setIcon(make_icon(IconId::close));
    close->setIconSize(QSize(14, 14));
    close->setToolTip(tr("Close %1").arg(title));
    close->setAccessibleName(tr("Close %1").arg(title));
    connect(close, &QToolButton::clicked, this, [this, guard = QPointer<EditorTab>(tab)] {
        if (guard)
            close_tab(tabs_->indexOf(guard));
    });
    tabs_->tabBar()->setTabButton(index, QTabBar::RightSide, close);
    tabs_->setCurrentIndex(index);
    // Keys go to the editor of a document that just opened: never to the first focusable
    // Inspector field, where an arrow key would change a setting (review_5).
    tab->piano_roll().setFocus(Qt::OtherFocusReason);
    tab->set_keyboard_visible(keyboard_action_ && keyboard_action_->isChecked());
    tab->piano_roll().fit_all();
    QPointer<EditorTab> fit_target(tab);
    QTimer::singleShot(0, this, [fit_target] {
        if (fit_target)
            fit_target->piano_roll().fit_all(); // again once the layout has its final size
    });
}

bool MainWindow::open_path(const std::filesystem::path& path) {
    // A document that is already open is shown, not opened twice: two tabs saving to one file
    // would overwrite each other's changes (code-uiA-28).
    std::error_code absolute_error;
    const auto wanted = std::filesystem::absolute(path, absolute_error);
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* tab = qobject_cast<EditorTab*>(tabs_->widget(index));
        if (!tab || tab->controller().file_path().empty())
            continue;
        const auto same = library::relative_inside(wanted, std::filesystem::absolute(tab->controller().file_path(), absolute_error));
        if (same && same->empty()) {
            tabs_->setCurrentWidget(tab);
            statusBar()->showMessage(tr("%1 is already open").arg(to_qstring(path.filename())), 4000);
            return true;
        }
    }
    // Reading and parsing (in the worker process) happen on the I/O thread; the tab opens when
    // the result arrives, and the window keeps responding meanwhile.
    ++pending_opens_;
    statusBar()->showMessage(tr("Opening %1…").arg(to_qstring(path.filename())));
    QPointer<MainWindow> guard(this);
    auto parser = parser_;
    const auto recovery_directory = directories_.recovery;
    io_thread_->post([guard, parser, path, recovery_directory](std::stop_token stop) {
        auto opened = std::make_shared<Result<app::OpenedDocument>>(app::open_document(
            path, {}, stop,
            [parser](std::span<const std::uint8_t> bytes, std::string_view name, const midi::ParseLimits& limits,
                     std::stop_token token) { return parser->parse(bytes, name, limits, std::move(token)); }));
        std::shared_ptr<document::RecoveryJournal> journal;
        if (*opened) {
            document::SourceReference source;
            source.document_name = (*opened)->document.title;
            if ((*opened)->kind == app::FileKind::native) {
                const auto utf8 = path.u8string();
                source.file_path_utf8 = std::string(utf8.begin(), utf8.end());
            }
            if (auto created = document::RecoveryJournal::create(recovery_directory, (*opened)->document, source)) {
                journal = std::move(*created);
                journal->set_discard_if_unused(true); // an untouched opened file needs no recovery
            }
        }
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, path, opened, journal] {
                if (guard)
                    guard->finish_open(path, std::move(*opened), journal);
            },
            Qt::QueuedConnection);
    }, [guard, path] {
        // The task threw (an unwrapped storage exception): the open still finishes, as a failure,
        // so pending_opens_ never stays raised (code-uiA-23).
        QMetaObject::invokeMethod(
            qApp,
            [guard, path] {
                if (guard)
                    guard->finish_open(path, make_error(ErrorCode::internal, "the file could not be opened"), nullptr);
            },
            Qt::QueuedConnection);
    });
    return true;
}

void MainWindow::finish_open(const std::filesystem::path& path, Result<app::OpenedDocument> opened,
                             std::shared_ptr<document::RecoveryJournal> journal) {
    --pending_opens_;
    if (!opened) {
        if (pending_opens_ == 0 && tabs_->count() == 0)
            new_from_template("blank");
        statusBar()->clearMessage();
        if (!options_.exit_when_ready)
            QMessageBox::warning(this, tr("Open"), tr("Could not open %1:\n%2").arg(to_qstring(path.filename()),
                                                                                   QString::fromStdString(opened.error().describe())));
        return;
    }
    const bool native = opened->kind == app::FileKind::native;
    QStringList warnings;
    for (const auto& diagnostic : opened->diagnostics)
        if (diagnostic.severity != Severity::info)
            warnings << QString::fromStdString(diagnostic.message);
    open_document(std::move(opened->document), native ? std::optional(path) : std::nullopt, false, std::move(journal), true);
    if (!warnings.isEmpty())
        statusBar()->showMessage(tr("Opened with %n warning(s): %1", nullptr, static_cast<int>(warnings.size())).arg(warnings.front()), 10000);
    else
        statusBar()->showMessage(tr("Opened %1").arg(to_qstring(path.filename())), 4000);
}

void MainWindow::new_from_template(const QString& name) { open_document(template_document(name), std::nullopt, false); }

void MainWindow::connect_tab(EditorTab* tab) {
    auto& controller = tab->controller();
    auto& view = tab->piano_roll();
    connect(&controller, &DocumentController::document_changed, this, [this, tab] {
        update_command_state();
        // A stopped transport needs no snapshot: play, play-from-selection and record publish
        // before they start. Publishing copies the document, so it is skipped per edit when idle.
        if (&tab->controller() == transport_owner_ && !transport_display_is_stopped(playback_->display()))
            publish_playback();
        const int index = tabs_->indexOf(tab);
        if (index >= 0)
            tabs_->setTabText(index, QString::fromStdString(tab->controller().document().title) +
                                         (tab->controller().is_dirty() ? "*" : ""));
    });
    // The tab text, its close button's tooltip and accessible name follow the title and dirty
    // state (a renamed document is not announced under its old name, code-uiA-34).
    const auto refresh_label = [this, tab] {
        const int index = tabs_->indexOf(tab);
        if (index < 0)
            return;
        const QString title = QString::fromStdString(tab->controller().document().title);
        tabs_->setTabText(index, title + (tab->controller().is_dirty() ? "*" : ""));
        // The tab names its file; the window title (taskbar, Alt+Tab, screen readers) names the
        // current document and its unsaved state (ux-visual-21/30).
        const auto& path = tab->controller().file_path();
        tabs_->setTabToolTip(index, path.empty() ? tr("%1 (not saved to a file)").arg(title) : to_qstring(path));
        if (tab == current_tab())
            update_window_title();
        if (auto* close = tabs_->tabBar()->tabButton(index, QTabBar::RightSide)) {
            close->setToolTip(tr("Close %1").arg(title));
            close->setAccessibleName(tr("Close %1").arg(title));
        }
    };
    connect(&controller, &DocumentController::dirty_changed, this, refresh_label);
    connect(&controller, &DocumentController::document_changed, this, refresh_label);
    connect(&controller, &DocumentController::selection_changed, this, &MainWindow::update_command_state);
    connect(&controller, &DocumentController::save_finished, this, [this, tab](const QString& error) {
        if (!error.isEmpty()) {
            QMessageBox::warning(this, tr("Save"), tr("The document was not saved:\n%1").arg(error));
            return;
        }
        const auto path = tab->controller().file_path();
        // A network or removable target is saved, but the user is told the limitation and the
        // recovery session stays as a second copy (spec 21.3, code-document-22).
        if (const auto limitation = document::file_system::destination_limitation(path)) {
            statusBar()->showMessage(tr("Saved %1. %2").arg(to_qstring(path.filename()), QString::fromStdString(*limitation)), 10000);
            return;
        }
        statusBar()->showMessage(tr("Saved %1").arg(to_qstring(path.filename())), 4000);
    });
    connect(&controller, &DocumentController::history_trimmed, this, [this] {
        statusBar()->showMessage(tr("Undo history reached its memory budget: the oldest steps can no longer be undone"), 8000);
    });
    connect(&controller, &DocumentController::edit_rejected, this,
            [this](const QString& message) { statusBar()->showMessage(message, 8000); });
    connect(&view, &PianoRollView::status_message, this, [this](const QString& text) { statusBar()->showMessage(text, 4000); });
    connect(&view, &PianoRollView::tool_changed, this, &MainWindow::update_command_state);
    // A new tab takes the window's view settings (follow, snap mode).
    if (follow_action_)
        tab->set_follow_playhead(follow_action_->isChecked());
    view.set_snap_mode(snap_off_ ? SnapMode::off : snap_relative_ ? SnapMode::relative : SnapMode::absolute);
    connect(&view, &PianoRollView::seek_requested, this, [this, tab](Tick tick) {
        // A ruler click moves playback too, as in a DAW (ux-editing-8); never while recording.
        const auto display = playback_->display();
        if (&tab->controller() != transport_owner_ || display.state != TransportDisplay::State::playing)
            return;
        playback_->seek(tick);
        play_started_from_ = tick; // Space now stops back here
    });
    connect(tab, &EditorTab::status_message, this, [this](const QString& text) { statusBar()->showMessage(text, 6000); });
    connect(&view, &PianoRollView::hover_changed, this, [this, tab](Tick tick, int pitch) {
        cursor_label_->setText(QString("%1  %2 (%3)")
                                   .arg(QString::fromStdString(tab->controller().meter_map().format(std::max<Tick>(0, tick))))
                                   .arg(QString::fromStdString(pitch_name(static_cast<std::uint8_t>(std::clamp(pitch, 0, 127)))))
                                   .arg(pitch));
    });
    // The readout never shows a stale position after the pointer left the roll (ux-panels-43).
    connect(&view, &PianoRollView::hover_left, this, [this] { cursor_label_->setText(tr("Pointer: —")); });
    const auto preset = [tab]() {
        const auto active = tab->controller().active_track_index();
        return active ? tab->controller().active_sequence().tracks[*active].instrument.synth_preset : std::string("classic");
    };
    connect(&view, &PianoRollView::audition_note_on, this, [this, preset](int pitch, int velocity) {
        playback_->audition_note_on(pitch, velocity, 0, preset());
    });
    connect(&view, &PianoRollView::audition_note_off, this, [this](int pitch) { playback_->audition_note_off(pitch, 0); });
    // Setting a drum step plays it, on the track's channel and instrument (ux-editing-21).
    connect(&tab->drum_grid(), &DrumGridView::audition_note_on, this, [this, tab, preset](int pitch, int velocity) {
        playback_->audition_note_on(pitch, velocity, default_channel_for(tab->controller()), preset());
    });
    connect(&tab->drum_grid(), &DrumGridView::audition_note_off, this,
            [this, tab](int pitch) { playback_->audition_note_off(pitch, default_channel_for(tab->controller())); });
    connect(&tab->keyboard(), &KeyboardWidget::note_on, this, [this, preset](int pitch, int velocity) {
        playback_->audition_note_on(pitch, velocity, 0, preset());
        recording_->live_note(pitch, velocity, true);
    });
    connect(&tab->keyboard(), &KeyboardWidget::note_off, this, [this](int pitch) {
        playback_->audition_note_off(pitch, 0);
        recording_->live_note(pitch, 0, false);
    });
}

void MainWindow::update_window_title() {
    auto* tab = current_tab();
    if (!tab) {
        setWindowTitle(tr("MIDI Workbench"));
        setWindowModified(false);
        return;
    }
    setWindowTitle(tr("%1[*] - MIDI Workbench").arg(QString::fromStdString(tab->controller().document().title)));
    setWindowModified(tab->controller().is_dirty());
}

void MainWindow::current_tab_changed() {
    auto* tab = current_tab();
    update_window_title();
    if (cursor_label_)
        cursor_label_->setText(tr("Pointer: —")); // the previous tab's position is not this one's
    if (recovery_label_)
        update_recovery_label();
    inspector_->set_controller(tab ? &tab->controller() : nullptr);
    recording_->set_target(tab ? &tab->controller() : nullptr, tab ? &tab->keyboard() : nullptr);
    drag_handle_->set_controller(tab ? &tab->controller() : nullptr);
    if (scale_bar_)
        scale_bar_->set_controller(tab ? &tab->controller() : nullptr);
    if (auto* old = events_dock_->widget()) {
        events_dock_->setWidget(nullptr);
        old->deleteLater();
    }
    // The event list is built only while its panel is visible (it rebuilds on every edit).
    if (tab && events_dock_->isVisible())
        events_dock_->setWidget(new EventListPanel(tab->controller(), events_dock_));
    if (tab) {
        const auto& sequence = tab->controller().active_sequence();
        auto scale = music::Scale::custom(sequence.root_pc, sequence.scale_mask);
        if (scale)
            tab->keyboard().set_scale(sequence.root_pc, scale->absolute_pitch_class_mask(), sequence.settings.highlight_scale);
        if (!transport_owner_ || transport_display_is_stopped(playback_->display())) {
            transport_owner_ = &tab->controller(); // the active editor owns the transport when idle
            publish_playback();
        }
    }
    update_command_state();
}

void MainWindow::update_command_state() {
    auto* tab = current_tab();
    const bool has_tab = tab != nullptr;
    undo_action_->setEnabled(has_tab && tab->controller().can_undo());
    redo_action_->setEnabled(has_tab && tab->controller().can_redo());
    undo_action_->setText(has_tab && tab->controller().can_undo() ? tr("&Undo %1").arg(tab->controller().undo_text())
                                                                  : tr("&Undo"));
    undo_action_->setToolTip(has_tab && tab->controller().can_undo() ? QString() : tr("Nothing to undo"));
    redo_action_->setText(has_tab && tab->controller().can_redo() ? tr("&Redo %1").arg(tab->controller().redo_text())
                                                                  : tr("&Redo"));
    if (tab) {
        updating_toolbar_ = true;
        for (auto* action : tool_group_->actions())
            action->setChecked(action->property("mwb_tool").toInt() == static_cast<int>(tab->piano_roll().tool()));
        // The combo always shows the grid in use: Off, a listed grid, or the document's own value
        // as a "(custom)" item (ux-editing-14).
        const QString snap_value = QString::fromStdString(tab->controller().active_sequence().settings.snap.to_string());
        for (int index = snap_combo_->count() - 1; index >= 0; --index)
            if (snap_combo_->itemData(index, Qt::UserRole + 1).toBool() &&
                snap_combo_->itemData(index).toString() != snap_value)
                snap_combo_->removeItem(index);
        int snap_index = snap_combo_->findData(snap_off_ ? QString::fromLatin1(snap_off_value) : snap_value);
        if (snap_index < 0) {
            snap_combo_->insertItem(0, tr("%1 (custom)").arg(snap_value), snap_value);
            snap_combo_->setItemData(0, true, Qt::UserRole + 1);
            snap_index = 0;
        }
        snap_combo_->setCurrentIndex(snap_index);
        updating_toolbar_ = false;
        const bool has_loop = tab->controller().active_sequence().settings.loop_region.has_value();
        loop_action_->setEnabled(has_loop);
        loop_action_->setToolTip(has_loop ? tr("Loop the loop region (L)")
                                          : tr("No loop region: set a loop length in the Inspector's Song section"));
        const auto count = tab->controller().selection().size();
        selection_label_->setText(count == 0 ? tr("Nothing selected") : tr("%n note(s) selected", nullptr, static_cast<int>(count)));
        const auto grid = tab->piano_roll().grid();
        snap_label_->setText(snap_off_ ? tr("Snap off")
                                       : tr("Snap %1%2%3")
                                             .arg(QString::fromStdString(grid.fraction.to_string()))
                                             .arg(snap_relative_ ? tr(" relative") : QString())
                                             .arg(grid.exact ? QString() : tr(" (rounded to project ticks)")));
        const auto& sequence = tab->controller().active_sequence();
        if (auto scale = music::Scale::custom(sequence.root_pc, sequence.scale_mask))
            tab->keyboard().set_scale(sequence.root_pc, scale->absolute_pitch_class_mask(), sequence.settings.highlight_scale);
    }
    // Commands with unmet preconditions are disabled with an explanation (section 6.3).
    for (auto* action : commands_) {
        const QString id = action->data().toString();
        const bool needs_selection = id.startsWith("edit.transpose") || id.startsWith("edit.octave") ||
                                     id.startsWith("edit.degree") || id == "edit.cut" ||
                                     id == "edit.copy" || id == "edit.duplicate" || id == "edit.delete";
        if (needs_selection) {
            const bool enabled = has_tab && !tab->controller().selection().empty();
            action->setEnabled(enabled);
            action->setToolTip(enabled ? QString() : tr("Select notes first"));
        }
        // Library commands say what they need instead of silently doing nothing (ux-panels-36).
        if (id.startsWith("library.") && library_) {
            const bool open = library_->is_open();
            const bool selection_needed = id == "library.save_selection" || id == "library.find_melody" ||
                                          id == "library.find_similar";
            const bool has_selection = has_tab && !tab->controller().selection().empty();
            const bool enabled = open && (!selection_needed || has_selection) && (id == "library.revisions" || has_tab);
            action->setEnabled(enabled);
            action->setToolTip(enabled            ? QString()
                               : !open            ? tr("Open or create a library first (Open Library… or New Library… in the library panel)")
                               : selection_needed ? tr("Select notes first")
                                                  : QString());
        }
    }
    if (transport_bar_)
        refresh_tooltips();
}

bool MainWindow::save_current(bool save_as, bool blocking) {
    return save_tab(current_tab(), save_as, blocking);
}

bool MainWindow::save_tab(EditorTab* tab, bool save_as, bool blocking) {
    if (!tab)
        return false;
    auto& controller = tab->controller();
    std::filesystem::path path = controller.file_path();
    if (save_as || path.empty()) {
        const QString chosen = QFileDialog::getSaveFileName(this, tr("Save"), QString::fromStdString(controller.document().title) + ".mwb",
                                                            tr("MIDI Workbench documents (*.mwb)"));
        if (chosen.isEmpty())
            return false;
        path = to_path(chosen);
    }
    if (controller.saving()) {
        statusBar()->showMessage(tr("A save of this document is still in progress"), 4000);
        return false;
    }
    if (blocking && !options_.exit_when_ready && !isHidden()) {
        // Closing waits for the durable save, but in the modal progress loop, not by blocking the
        // GUI thread; a failure is reported by the save_finished handler (code-uiA-13).
        QPointer<EditorTab> guard(tab);
        controller.save_native_async(path);
        const QString error = wait_for_save_modal(tab);
        return guard && error.isEmpty() && !controller.saving();
    }
    if (options_.exit_when_ready || isHidden()) {
        // No event loop to wait in (headless runs, a hidden window): a direct save.
        QString error;
        if (!controller.save_native(path, &error)) {
            QMessageBox::warning(this, tr("Save"), tr("The document was not saved:\n%1").arg(error));
            return false;
        }
        return true;
    }
    statusBar()->showMessage(tr("Saving %1…").arg(to_qstring(path.filename())));
    controller.save_native_async(path); // the GUI keeps responding; "Saved" appears only when durable
    return true;
}

void MainWindow::export_current_midi() {
    auto* tab = current_tab();
    if (!tab)
        return;
    const QString chosen = QFileDialog::getSaveFileName(this, tr("Export MIDI"),
                                                        QString::fromStdString(tab->controller().document().title) + ".mid",
                                                        tr("Standard MIDI files (*.mid)"));
    if (chosen.isEmpty())
        return;
    midi::ExportOptions options;
    options.mode = midi::ExportMode::compatibility; // canonical type 1 with an explicit report
    statusBar()->showMessage(tr("Exporting %1…").arg(QFileInfo(chosen).fileName()));
    // Encoding and the durable write run on the I/O thread from an immutable copy.
    auto snapshot = std::make_shared<const DocumentData>(tab->controller().document());
    QPointer<MainWindow> guard(this);
    const auto target = to_path(chosen);
    io_thread_->post([guard, snapshot, options, target](std::stop_token) {
        auto report = std::make_shared<midi::ExportReport>();
        auto bytes = midi::export_midi(*snapshot, options, report.get());
        QString error;
        if (!bytes)
            error = QString::fromStdString(bytes.error().describe());
        // The file dialog already confirmed any overwrite with the standard dialog.
        else if (auto written = app::write_file_atomically(target, *bytes, true); !written)
            error = QString::fromStdString(written.error().describe());
        QMetaObject::invokeMethod(
            qApp, // context outlives the task; the lambda checks the guard
            [guard, report, error] {
                if (!guard)
                    return;
                if (!error.isEmpty()) {
                    guard->statusBar()->clearMessage();
                    QMessageBox::warning(guard, tr("Export MIDI"), error);
                    return;
                }
                QString summary = tr("Exported %1 events in %2 tracks (type %3)")
                                      .arg(report->events_written)
                                      .arg(report->track_count)
                                      .arg(report->format);
                for (const auto& diagnostic : report->diagnostics)
                    summary += "\n" + QString::fromStdString(diagnostic.message);
                guard->statusBar()->showMessage(summary.section('\n', 0, 0), 6000);
                if (!report->diagnostics.empty())
                    QMessageBox::information(guard, tr("Export MIDI"), summary);
            },
            Qt::QueuedConnection);
    });
}

QString MainWindow::wait_for_save_modal(EditorTab* tab) {
    // The GUI never blocks on the file system (section 19.4, code-uiA-13): a small modal
    // "Saving..." window keeps the event loop (and painting) running until the background save
    // reports, and the user cannot start another action meanwhile.
    auto& controller = tab->controller();
    if (!controller.saving())
        return {};
    QString error;
    QEventLoop loop;
    const auto connection = connect(&controller, &DocumentController::save_finished, &loop, [&](const QString& text) {
        error = text;
        loop.quit();
    });
    QProgressDialog progress(tr("Saving %1…").arg(QString::fromStdString(controller.document().title)), QString(), 0, 0,
                             this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setCancelButton(nullptr);
    progress.setMinimumDuration(400); // quick saves never flash a window
    // User input waits until the save reports, also in the 400 ms before the dialog shows: no
    // second close, edit or shortcut can run inside this wait (final review N2). Painting and the
    // save's own completion still run; the input is delivered afterwards.
    const bool nested = std::exchange(waiting_for_save_, true);
    if (controller.saving())
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    waiting_for_save_ = nested;
    disconnect(connection);
    return error;
}

bool MainWindow::confirm_close(EditorTab* tab) {
    QPointer<EditorTab> waited(tab);
    std::ignore = wait_for_save_modal(tab); // a save in flight lands (or fails) before the question
    if (!waited)
        return false;
    if (!tab->controller().is_dirty())
        return true;
    QPointer<EditorTab> guard(tab);
    tabs_->setCurrentWidget(tab);
    const auto answer = QMessageBox::question(this, tr("Unsaved changes"),
                                              tr("Save changes to %1?").arg(QString::fromStdString(tab->controller().document().title)),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    // The prompt runs a nested event loop: the tab may have closed or another tab become current.
    if (answer == QMessageBox::Cancel || !guard)
        return false;
    if (answer == QMessageBox::Save)
        return save_tab(guard.data(), false, true); // this tab, and closing waits for the durable save
    return true;
}

void MainWindow::close_tab(int index) {
    if (waiting_for_save_)
        return; // a close that arrives while a close is waiting for its save is not started twice
    QPointer<EditorTab> tab = qobject_cast<EditorTab*>(tabs_->widget(index));
    if (tab && recording_->recording() && tab == current_tab())
        recording_->toggle_recording(); // the take lands in this document before it is asked about
    if (!tab || !confirm_close(tab) || !tab)
        return;
    index = tabs_->indexOf(tab); // tabs may have moved while the prompt was open
    if (index < 0)
        return;
    if (&tab->controller() == transport_owner_) {
        playback_->stop(0);
        transport_owner_ = nullptr;
    }
    tab->controller().close_recovery();
    library_sources_.erase(tab);
    tabs_->removeTab(index);
    tab->deleteLater();
    if (tabs_->count() == 0)
        new_from_template("blank");
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (waiting_for_save_) {
        event->ignore(); // the window closes after the running save, when asked again
        return;
    }
    if (recording_->recording())
        recording_->toggle_recording(); // keep the take: it becomes an unsaved edit the prompt covers
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* tab = qobject_cast<EditorTab*>(tabs_->widget(index));
        if (tab && !options_.exit_when_ready && !confirm_close(tab)) {
            event->ignore();
            return;
        }
    }
    playback_->panic(false); // shutdown must not leave stuck notes
    for (int index = 0; index < tabs_->count(); ++index)
        if (auto* tab = qobject_cast<EditorTab*>(tabs_->widget(index)))
            tab->controller().close_recovery();
    QSettings settings;
    settings.setValue("layout/schema", layout_schema_version);
    settings.setValue("layout/geometry", saveGeometry());
    settings.setValue("layout/state", saveState(layout_schema_version));
    event->accept();
}

void MainWindow::flush_recovery() {
    // Every 2 s (section 21.2): flushes run on the recovery thread; the label shows the outcome of
    // the latest completed flush of the current document.
    for (int index = 0; index < tabs_->count(); ++index)
        if (auto* tab = qobject_cast<EditorTab*>(tabs_->widget(index)))
            tab->controller().flush_recovery_async();
    update_recovery_label();
}

void MainWindow::update_recovery_label() {
    auto* tab = current_tab();
    if (!tab)
        return;
    recovery_label_->show();
    // Always about the current document: a tab without a journal never shows another tab's time
    // (code-uiA-19).
    if (!tab->controller().recovery_journal()) {
        recovery_label_->setText(tr("! Recovery unavailable for this document"));
        return;
    }
    const auto ms = tab->controller().recovery_journal()->last_durable_ms();
    const QString& error = tab->controller().recovery_error();
    recovery_label_->setText(error.isEmpty()
                                 ? tr("Recovery saved %1").arg(QDateTime::fromMSecsSinceEpoch(ms).toString("HH:mm:ss"))
                                 : tr("! Recovery not saved: %1").arg(error));
}

void MainWindow::open_prepared(DocumentData document, bool recovered, std::function<void(EditorTab*)> opened) {
    ++pending_opens_;
    QPointer<MainWindow> guard(this);
    const auto recovery_directory = directories_.recovery;
    auto shared = std::make_shared<DocumentData>(std::move(document));
    io_thread_->post([guard, shared, recovered, recovery_directory, opened = std::move(opened)](std::stop_token) {
        document::SourceReference source;
        source.document_name = shared->title;
        std::shared_ptr<document::RecoveryJournal> journal;
        if (auto created = document::RecoveryJournal::create(recovery_directory, *shared, source)) {
            journal = std::move(*created);
            journal->set_discard_if_unused(!recovered);
        }
        QMetaObject::invokeMethod(
            qApp,
            [guard, shared, recovered, journal, opened] {
                if (!guard)
                    return;
                --guard->pending_opens_;
                guard->open_document(std::move(*shared), std::nullopt, recovered, journal, true);
                if (opened)
                    if (auto* tab = guard->current_tab())
                        opened(tab);
            },
            Qt::QueuedConnection);
    }, [guard, shared, recovered] {
        // Creating the journal threw: open the document without recovery rather than never.
        QMetaObject::invokeMethod(
            qApp,
            [guard, shared, recovered] {
                if (!guard)
                    return;
                --guard->pending_opens_;
                guard->open_document(std::move(*shared), std::nullopt, recovered, nullptr, true);
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::offer_recovery(bool asked) {
    // Listing reads SQLite: done on the I/O thread; the dialog appears when the list arrives.
    QPointer<MainWindow> guard(this);
    const auto recovery_directory = directories_.recovery;
    io_thread_->post([guard, recovery_directory, asked](std::stop_token) {
        auto sessions = std::make_shared<Result<std::vector<document::RecoverableSession>>>(
            document::list_recoverable_sessions(recovery_directory));
        QMetaObject::invokeMethod(
            qApp,
            [guard, sessions, asked] {
                if (!guard)
                    return;
                if (!*sessions) {
                    guard->statusBar()->showMessage(
                        tr("Could not read recovery data: %1").arg(QString::fromStdString(sessions->error().describe())), 8000);
                    return;
                }
                // A damaged or foreign recovery database was set aside and a fresh one started
                // (code-document-2): said once, so the user knows older crash data was not used.
                if (const auto notice = document::take_recovery_database_notice())
                    guard->statusBar()->showMessage(QString::fromStdString(*notice), 12000);
                const bool offered = guard->present_recovery(std::move(**sessions));
                // Asked from the File menu: an empty answer is still an answer (ux-panels-40).
                if (asked && !offered)
                    QMessageBox::information(guard, tr("Recover Sessions"), tr("There is no unsaved work to recover."));
            },
            Qt::QueuedConnection);
    });
}

bool MainWindow::present_recovery(std::vector<document::RecoverableSession> sessions) {
    // Exclude sessions this process owns.
    std::vector<document::RecoverableSession> candidates;
    std::vector<Uuid> settled;
    for (const auto& session : sessions) {
        bool own = false;
        for (int index = 0; index < tabs_->count(); ++index)
            if (auto* tab = qobject_cast<EditorTab*>(tabs_->widget(index)); tab && tab->controller().recovery_journal() &&
                                                                              tab->controller().recovery_journal()->session_id() == session.session_id)
                own = true;
        if (own || session.live) // another running instance's sessions are not crashed work
            continue;
        if (!session.has_unsaved_work()) {
            // Never edited, or saved after its last edit: the file already holds everything, so
            // it is not offered (code-document-16) and its leftover data is removed.
            settled.push_back(session.session_id);
            continue;
        }
        candidates.push_back(session);
    }
    if (!settled.empty()) {
        const auto recovery_directory = directories_.recovery;
        io_thread_->post([recovery_directory, settled](std::stop_token) {
            for (const auto& session_id : settled)
                std::ignore = document::discard_recovery_session(recovery_directory, session_id);
        });
    }
    if (candidates.empty())
        return false;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Recover unsaved work"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(tr("These sessions were not closed normally. Recovery opens a new unsaved document; "
                                      "it never replaces a saved file."),
                                   &dialog);
    explanation->setWordWrap(true); // the dialog keeps a readable width (ux-visual-26)
    layout->addWidget(explanation);
    auto* list = new QListWidget(&dialog);
    for (const auto& session : candidates) {
        const QString saved = session.source.last_explicit_save_ms
                                  ? QLocale().toString(QDateTime::fromMSecsSinceEpoch(*session.source.last_explicit_save_ms), QLocale::ShortFormat)
                                  : tr("never saved");
        auto* item = new QListWidgetItem(tr("%1 — last durable edit %2, last save %3%4")
                                             .arg(QString::fromStdString(session.source.document_name))
                                             .arg(QLocale().toString(QDateTime::fromMSecsSinceEpoch(session.last_durable_edit_ms), QLocale::ShortFormat))
                                             .arg(saved)
                                             .arg(session.source.file_path_utf8
                                                      ? " — " + QString::fromStdString(*session.source.file_path_utf8)
                                                      : QString()),
                                         list);
        item->setCheckState(Qt::Checked);
    }
    layout->addWidget(list);
    auto* buttons = new QDialogButtonBox(&dialog);
    auto* recover = buttons->addButton(tr("Recover Checked"), QDialogButtonBox::AcceptRole);
    auto* discard = buttons->addButton(tr("Discard Checked"), QDialogButtonBox::DestructiveRole);
    buttons->addButton(tr("Decide Later"), QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    bool discard_chosen = false;
    connect(recover, &QPushButton::clicked, &dialog, &QDialog::accept);
    recover->setDefault(true);
    connect(discard, &QPushButton::clicked, &dialog, [&] {
        int checked = 0;
        for (int row = 0; row < list->count(); ++row)
            checked += list->item(row)->checkState() == Qt::Checked ? 1 : 0;
        if (checked == 0)
            return;
        // Recovery data is the only copy of that work: never deleted by one click (ux-panels-17).
        if (QMessageBox::warning(&dialog, tr("Discard recovery data"),
                                 tr("Permanently delete the unsaved changes of %n session(s)? This cannot be undone.",
                                    nullptr, checked),
                                 QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Discard)
            return;
        discard_chosen = true;
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return true;
    std::vector<Uuid> chosen;
    for (int row = 0; row < list->count(); ++row)
        if (list->item(row)->checkState() == Qt::Checked)
            chosen.push_back(candidates[static_cast<std::size_t>(row)].session_id);
    // Replay and discard read and write SQLite: the I/O thread does them, then tabs open here.
    QPointer<MainWindow> guard(this);
    const auto recovery_directory = directories_.recovery;
    io_thread_->post([guard, recovery_directory, chosen, discard_chosen](std::stop_token) {
        for (const auto& session_id : chosen) {
            if (!discard_chosen) {
                auto recovered = std::make_shared<Result<document::RecoveredDocument>>(
                    document::recover_session(recovery_directory, session_id));
                std::shared_ptr<document::RecoveryJournal> journal;
                bool protected_again = false;
                if (*recovered) {
                    document::SourceReference source;
                    source.document_name = (*recovered)->document.title;
                    if (auto created = document::RecoveryJournal::create(recovery_directory, (*recovered)->document, source)) {
                        journal = std::move(*created);
                        // create() only queues the first checkpoint. The old session is the only
                        // durable copy until that checkpoint has committed, so wait for it here
                        // (this is the I/O thread) before the old evidence may be deleted.
                        protected_again = journal->flush(true).has_value() && journal->checkpoint_committed();
                    }
                }
                QMetaObject::invokeMethod(
                    qApp,
                    [guard, recovered, journal] {
                        if (!guard)
                            return;
                        if (!*recovered) {
                            QMessageBox::warning(guard, tr("Recovery"), QString::fromStdString(recovered->error().describe()));
                            return;
                        }
                        const auto report = (*recovered)->report;
                        guard->open_document(std::move((*recovered)->document), std::nullopt, true, journal, true);
                        guard->statusBar()->showMessage(
                            tr("Recovered %1 edit(s)%2")
                                .arg(report.applied_commands)
                                .arg(report.stop_reason ? tr("; stopped: %1").arg(QString::fromStdString(*report.stop_reason))
                                                        : QString()),
                            10000);
                    },
                    Qt::QueuedConnection);
                if (!*recovered || !protected_again)
                    continue; // keep the old evidence until the recovered document is protected again
            }
            std::ignore = document::discard_recovery_session(recovery_directory, session_id); // evidence no longer needed
        }
    });
    return true;
}

// --- Editing commands ----------------------------------------------------------------------------

void MainWindow::transpose_selection(int semitones) {
    auto* tab = current_tab();
    if (!tab || tab->controller().selection().empty())
        return;
    auto& controller = tab->controller();
    music::TargetSelection selection{controller.active_sequence().id, {controller.active_track()},
                                     std::vector<EventId>(controller.selection().begin(), controller.selection().end()), {}};
    music::MoveParameters parameters;
    parameters.semitones = semitones;
    parameters.range_policy = music::RangePolicy::constrain; // constrain the whole displacement (section 7.3)
    // The controller's cached pairing of the active sequence: a key press does not re-pair the
    // whole sequence (code-core-2).
    auto preview = music::move_notes(controller.document(), selection, parameters, &controller.notes());
    if (!preview) {
        statusBar()->showMessage(QString::fromStdString(preview.error().message), 5000);
        return;
    }
    if (!controller.apply(preview->changes))
        return;
    // A key transpose is heard like a mouse move (ux-editing-26): the top selected note, briefly.
    const auto moved = controller.selected_notes();
    if (moved.empty())
        return;
    const auto top = std::ranges::max_element(moved, {}, &NoteSpan::pitch);
    const int channel = top->channel;
    const int pitch = top->pitch;
    const auto active = controller.active_track_index();
    const std::string preset =
        active ? controller.active_sequence().tracks[*active].instrument.synth_preset : std::string("classic");
    playback_->audition_note_on(pitch, top->velocity, channel, preset);
    QTimer::singleShot(150, this, [this, pitch, channel] { playback_->audition_note_off(pitch, channel); });
}

void MainWindow::transpose_selection_by_degree(int degrees) {
    auto* tab = current_tab();
    if (!tab || tab->controller().selection().empty())
        return;
    auto& controller = tab->controller();
    const auto& sequence = controller.active_sequence();
    music::TargetSelection selection{sequence.id, {controller.active_track()},
                                     std::vector<EventId>(controller.selection().begin(), controller.selection().end()), {}};
    music::TransposeParameters parameters;
    parameters.mode = music::TransposeMode::scale_degrees;
    parameters.amount = degrees;
    parameters.scale_root = sequence.root_pc;
    parameters.scale_mask = sequence.scale_mask;
    parameters.exclude_drums = false;
    auto preview = music::transpose(controller.document(), selection, parameters, &controller.notes());
    if (!preview) {
        statusBar()->showMessage(QString::fromStdString(preview.error().message), 5000);
        return;
    }
    if (preview->report.ambiguous > 0 || preview->report.skipped > 0) {
        // Ambiguous degree moves (out-of-scale sources) are previewed before applying.
        const auto answer = QMessageBox::question(this, tr("Move by scale degree"),
                                                  tr("%1 note(s) are outside the scale and keep their chromatic offset. Apply?")
                                                      .arg(preview->report.ambiguous + preview->report.skipped));
        if (answer != QMessageBox::Yes)
            return;
    }
    controller.apply(preview->changes);
}

Tick MainWindow::play_position_step(EditorTab& tab, Tick from, int direction, int acceleration) const {
    // Units from fine to coarse; the first one at least this wide on screen is used, so a step is
    // always visible at the current zoom. Holding the key moves up one or two units.
    constexpr double min_step_pixels = 48.0;
    auto& controller = tab.controller();
    const auto grid = tab.piano_roll().grid();
    const double ticks_per_pixel = std::max(1e-6, tab.piano_roll().mapping().ticks_per_pixel);
    const auto& meter = controller.meter_map();
    const auto& segment = meter.segment_at(from);
    const double lengths[] = {grid.step(), static_cast<double>(meter.ticks_per_beat(segment)),
                              static_cast<double>(meter.ticks_per_bar(segment)),
                              4.0 * static_cast<double>(meter.ticks_per_bar(segment)),
                              16.0 * static_cast<double>(meter.ticks_per_bar(segment))};
    constexpr int unit_count = 5;
    int unit = unit_count - 1;
    for (int index = 0; index < unit_count; ++index)
        if (lengths[index] / ticks_per_pixel >= min_step_pixels) {
            unit = index;
            break;
        }
    unit = std::min(unit_count - 1, unit + std::max(0, acceleration));
    if (unit == 0) // the snap grid
        return direction < 0 ? std::max<Tick>(0, grid.floor(from > 0 ? from - 1 : 0)) : grid.next(from);
    if (unit == 1) {
        // Beats of the meter segment at the position; a step never crosses a meter change.
        const Tick beat = std::max<Tick>(1, meter.ticks_per_beat(segment));
        const Tick offset = from - segment.tick;
        if (direction > 0) {
            Tick next = segment.tick + (offset / beat + 1) * beat;
            if (meter.segment_at(next).tick != segment.tick)
                next = meter.segment_at(next).tick;
            return next;
        }
        if (offset <= 0) {
            if (from <= 0)
                return 0;
            const auto& previous = meter.segment_at(from - 1);
            const Tick previous_beat = std::max<Tick>(1, meter.ticks_per_beat(previous));
            return previous.tick + ((from - 1 - previous.tick) / previous_beat) * previous_beat;
        }
        return segment.tick + ((offset - 1) / beat) * beat;
    }
    // Bars, or groups of 4 or 16 bars counted from bar 1.
    const std::int64_t bars = unit == 2 ? 1 : unit == 3 ? 4 : 16;
    const std::int64_t bar = meter.position(from).bar;
    if (direction > 0)
        return meter.bar_start((bar / bars + 1) * bars);
    std::int64_t group = bar / bars;
    if (meter.bar_start(group * bars) >= from)
        --group;
    return group < 0 ? 0 : meter.bar_start(group * bars);
}

void MainWindow::move_play_position(int direction) {
    auto* tab = current_tab();
    if (!tab)
        return;
    auto& controller = tab->controller();
    const auto display = playback_->display();
    const bool rolling = !transport_display_is_stopped(display) && transport_owner_ == &controller;
    if (rolling && recording_->recording()) {
        statusBar()->showMessage(tr("Stop recording to move the playhead"), 4000); // section 13.3
        return;
    }
    // Holding the key accelerates: after 0.4 s one unit coarser, after 1.2 s two units coarser.
    int acceleration = 0;
    if (held_position_key_ != 0) {
        const auto held = std::chrono::steady_clock::now() - position_key_down_;
        acceleration = held >= std::chrono::milliseconds(1200) ? 2 : held >= std::chrono::milliseconds(400) ? 1 : 0;
    }
    const Tick from = rolling ? display.position : controller.edit_cursor();
    const Tick to = std::max<Tick>(0, play_position_step(*tab, from, direction, acceleration));
    if (rolling)
        playback_->seek(to); // the playhead jumps; playback continues from there
    controller.set_edit_cursor(to);
    if (rolling)
        play_started_from_ = to; // Space now stops back here
    tab->piano_roll().scroll_to_tick(to);
}

void MainWindow::move_selection_by_grid(int direction) {
    auto* tab = current_tab();
    if (!tab)
        return;
    auto& controller = tab->controller();
    if (controller.selection().empty()) {
        move_play_position(direction);
        return;
    }
    music::TargetSelection selection{controller.active_sequence().id, {controller.active_track()},
                                     std::vector<EventId>(controller.selection().begin(), controller.selection().end()), {}};
    music::MoveParameters parameters;
    parameters.ticks = direction * std::max<Tick>(1, tab->piano_roll().grid().point(1));
    auto preview = music::move_notes(controller.document(), selection, parameters, &controller.notes());
    if (preview)
        controller.apply(preview->changes);
}

void MainWindow::delete_selection() {
    auto* tab = current_tab();
    if (!tab || tab->controller().selection().empty())
        return;
    auto& controller = tab->controller();
    const std::vector<EventId> ids(controller.selection().begin(), controller.selection().end());
    auto changes = delete_notes(controller.document(), controller.active_sequence().id, ids, "Delete notes");
    if (changes && controller.apply(*changes))
        controller.set_selection({});
}

void MainWindow::duplicate_selection() {
    auto* tab = current_tab();
    if (!tab || tab->controller().selection().empty())
        return;
    auto& controller = tab->controller();
    const auto notes = controller.selected_notes();
    // Duplicate-to-end uses the smallest grid-aligned range containing the notes including ends.
    const auto extent = grid_aligned_extent(notes, tab->piano_roll().grid());
    const std::vector<EventId> ids(controller.selection().begin(), controller.selection().end());
    auto duplicated = duplicate_notes(controller.document(), controller.active_sequence().id, ids, extent.length(), 0,
                                      controller.active_track(), "Duplicate");
    if (duplicated && controller.apply(duplicated->changes)) {
        controller.set_selection(std::set<EventId>(duplicated->created.begin(), duplicated->created.end()));
        statusBar()->showMessage(tr("Duplicated after %1 ticks").arg(extent.length()), 3000);
    }
}

void MainWindow::copy_selection(bool cut) {
    auto* tab = current_tab();
    if (!tab || tab->controller().selection().empty())
        return;
    auto& controller = tab->controller();
    auto* mime = make_clipboard_mime(controller);
    if (!mime)
        return;
    QApplication::clipboard()->setMimeData(mime);
    if (cut)
        delete_selection();
}

void MainWindow::paste_at_cursor() {
    auto* tab = current_tab();
    if (!tab)
        return;
    auto& controller = tab->controller();
    QString error;
    auto created = paste_clipboard(controller, QApplication::clipboard()->mimeData(), controller.edit_cursor(), &error);
    if (!error.isEmpty())
        statusBar()->showMessage(error, 6000);
    if (!created.empty())
        controller.set_selection(std::set<EventId>(created.begin(), created.end()));
}

// --- Transport -----------------------------------------------------------------------------------

void MainWindow::publish_playback() {
    if (!transport_owner_)
        return;
    const auto& sequence = transport_owner_->active_sequence();
    std::optional<TimeRange> loop;
    if (loop_action_ && loop_action_->isChecked() && sequence.settings.loop_region)
        for (const auto& region : sequence.regions)
            if (region.id == *sequence.settings.loop_region)
                loop = TimeRange{region.start_tick, region.end_tick};
    playback_->set_loop(loop);
    playback_->publish_snapshot(transport_owner_->shared_snapshot(), transport_owner_->active_sequence_index(),
                                transport_owner_->generation());
}

void MainWindow::set_snap_off(bool off) {
    snap_off_ = off;
    snap_off_action_->setChecked(off);
    apply_snap_mode();
    update_command_state();
}

void MainWindow::apply_snap_mode() {
    const SnapMode mode = snap_off_ ? SnapMode::off : snap_relative_ ? SnapMode::relative : SnapMode::absolute;
    for (int index = 0; index < tabs_->count(); ++index)
        if (auto* tab = qobject_cast<EditorTab*>(tabs_->widget(index)))
            tab->piano_roll().set_snap_mode(mode);
}

void MainWindow::update_sounding_notes(EditorTab& tab, std::optional<Tick> position) {
    // Notes under the playhead on audible tracks light up in the roll and on the keyboard
    // (spec 7.1 "currently sounding", ux-editing-24).
    std::set<int> sounding;
    if (position) {
        const auto& sequence = tab.controller().active_sequence();
        bool any_solo = false;
        for (const auto& track : sequence.tracks)
            any_solo = any_solo || track.solo;
        const auto& notes = tab.controller().notes().notes;
        for (const std::size_t index : tab.controller().note_index().query(TimeRange{*position, *position + 1})) {
            if (index >= notes.size() || sequence.tracks.empty())
                continue;
            const auto& note = notes[index];
            const auto& track = sequence.tracks[std::min<std::size_t>(note.on_track, sequence.tracks.size() - 1)];
            if (!track.muted && (!any_solo || track.solo))
                sounding.insert(note.pitch);
        }
    }
    auto& previous = sounding_by_tab_[&tab];
    if (previous == sounding)
        return;
    for (const int pitch : previous)
        if (!sounding.contains(pitch))
            tab.keyboard().set_key_state(pitch, KeySource::playback, false);
    for (const int pitch : sounding)
        if (!previous.contains(pitch))
            tab.keyboard().set_key_state(pitch, KeySource::playback, true);
    tab.piano_roll().set_sounding_pitches(sounding);
    previous = std::move(sounding);
}

void MainWindow::preview_library_item(DocumentData document, const QString& title, std::optional<TimeRange> range) {
    if (recording_->recording()) {
        statusBar()->showMessage(tr("Stop recording to preview a library item"), 4000);
        return;
    }
    if (!transport_display_is_stopped(playback_->display()))
        stop_transport();
    // The preview is not one of the open documents: no tab owns the transport while it plays,
    // and the next Play of a tab publishes that tab again (start_playback).
    transport_owner_ = nullptr;
    // The readout follows the preview through its own tempo and meter maps (review_5).
    if (!document.sequences.empty()) {
        const auto& sequence = document.sequences.front();
        preview_ = Preview{title, TempoMap::build(sequence), MeterMap::build(sequence)};
    }
    Tick from = 0;
    if (range && !document.sequences.empty()) {
        // A melody match plays with a one-bar lead-in.
        const auto& sequence = document.sequences.front();
        const Tick bar = 4 * (sequence.division.is_ppq() ? sequence.division.ppq : default_ppq);
        from = std::max<Tick>(0, range->begin - bar);
    }
    playback_->set_loop(std::nullopt);
    const auto generation = document.generation;
    playback_->publish_snapshot(std::make_shared<const DocumentData>(std::move(document)), 0, generation);
    play_started_from_ = from;
    paused_cursor_.reset();
    playback_->play(from);
    statusBar()->showMessage(tr("Previewing \"%1\"; %2 stops")
                                 .arg(title, command("transport.play_stop")->shortcut().toString(QKeySequence::NativeText)),
                             8000);
}

Tick MainWindow::tempo_position(EditorTab& tab) const {
    const auto display = playback_->display();
    if (transport_owner_ == &tab.controller() && !transport_display_is_stopped(display) &&
        display.state != TransportDisplay::State::paused)
        return display.position;
    return tab.controller().edit_cursor();
}

void MainWindow::commit_tempo(double bpm) {
    auto* tab = current_tab();
    const std::optional<Tick> at = std::exchange(tempo_edit_at_, std::nullopt);
    if (!tab)
        return;
    auto& controller = tab->controller();
    if (!controller.active_sequence().division.is_ppq()) {
        statusBar()->showMessage(tr("This sequence uses SMPTE time; its tempo cannot be changed"), 5000);
        return;
    }
    // One undo entry; while playing, document_changed republishes the snapshot and the
    // scheduler continues from the same musical position at the new tempo.
    if (controller.set_tempo_at(at.value_or(tempo_position(*tab)), bpm))
        statusBar()->showMessage(tr("Tempo %1 BPM").arg(tempo_field_->textFromValue(tempo_field_->value())), 3000);
}

void MainWindow::start_playback(EditorTab& tab, Tick from) {
    preview_.reset(); // a document takes the transport back from a library preview
    if (transport_owner_ != &tab.controller()) {
        playback_->stop(0); // another document owned the transport
        transport_owner_ = &tab.controller();
    }
    publish_playback();
    play_started_from_ = from;
    paused_cursor_.reset();
    playback_->play(from);
}

void MainWindow::play_stop() {
    auto* tab = current_tab();
    if (!tab)
        return;
    const auto display = playback_->display();
    if (transport_display_is_stopped(display)) {
        start_playback(*tab, tab->controller().edit_cursor());
        return;
    }
    // Rolling or paused: stop and return the playhead to where playback started (a DAW's Space).
    stop_transport();
}

void MainWindow::pause_resume() {
    auto* tab = current_tab();
    if (!tab)
        return;
    const auto display = playback_->display();
    if (display.state == TransportDisplay::State::playing) {
        playback_->pause();
        paused_cursor_ = tab->controller().edit_cursor();
        return;
    }
    if (display.state == TransportDisplay::State::recording || display.state == TransportDisplay::State::counting_in) {
        statusBar()->showMessage(tr("Recording cannot pause; press %1 to stop")
                                     .arg(command("transport.play_stop")->shortcut().toString(QKeySequence::NativeText)),
                                 4000);
        return;
    }
    if (display.state == TransportDisplay::State::paused && transport_owner_ == &tab->controller()) {
        // Resume where it paused, unless the edit cursor was moved meanwhile: then play from there.
        const Tick cursor = tab->controller().edit_cursor();
        const bool moved = paused_cursor_ && *paused_cursor_ != cursor;
        const Tick from = moved ? cursor : display.position;
        const Tick started = play_started_from_;
        publish_playback();
        paused_cursor_.reset();
        playback_->play(from);
        play_started_from_ = moved ? from : started; // a plain resume still stops back at the start
        return;
    }
    start_playback(*tab, tab->controller().edit_cursor());
}

void MainWindow::play_from_selection() {
    auto* tab = current_tab();
    if (!tab)
        return;
    if (recording_->recording()) {
        statusBar()->showMessage(tr("Stop recording to move the playhead"), 4000); // section 13.3
        return;
    }
    const auto notes = tab->controller().selected_notes();
    Tick start = tab->controller().edit_cursor();
    if (!notes.empty())
        start = DragSnapshot::capture(notes).earliest_start;
    if (transport_owner_ == &tab->controller() && playback_->display().state == TransportDisplay::State::playing) {
        if (recording_->recording())
            return; // no seek while recording (section 13.3)
        playback_->seek(start); // play() would be ignored while playing
        play_started_from_ = start;
        return;
    }
    start_playback(*tab, start);
}

void MainWindow::stop_transport() {
    if (recording_->recording())
        recording_->toggle_recording(); // Stop ends the take; it lands before the transport stops
    auto* tab = current_tab();
    const bool owned = tab && transport_owner_ == &tab->controller();
    const Tick home = owned ? play_started_from_ : (tab ? tab->controller().edit_cursor() : 0);
    playback_->stop(home);
    paused_cursor_.reset();
    if (owned)
        tab->controller().set_edit_cursor(home);
}

void MainWindow::go_to_start() {
    auto* tab = current_tab();
    if (!tab)
        return;
    const bool rolling = !transport_display_is_stopped(playback_->display()) && transport_owner_ == &tab->controller();
    if (rolling && recording_->recording()) {
        statusBar()->showMessage(tr("Stop recording to move the playhead"), 4000);
        return;
    }
    if (rolling) {
        playback_->seek(0); // keeps playing from the start (section 13.3 accepts seeks while playing)
        play_started_from_ = 0;
    }
    tab->controller().set_edit_cursor(0);
    tab->piano_roll().scroll_to_tick(0);
}

void MainWindow::go_to_end() {
    auto* tab = current_tab();
    if (!tab)
        return;
    const bool rolling = !transport_display_is_stopped(playback_->display()) && transport_owner_ == &tab->controller();
    if (rolling && recording_->recording()) {
        statusBar()->showMessage(tr("Stop recording to move the playhead"), 4000);
        return;
    }
    // The end of the song: the last note end, or the sequence end when it is later.
    Tick end = tab->controller().active_sequence().effective_end();
    for (const auto& note : tab->controller().notes().notes)
        end = std::max(end, note.end);
    if (rolling) {
        playback_->seek(end);
        play_started_from_ = end;
    }
    tab->controller().set_edit_cursor(end);
    tab->piano_roll().scroll_to_tick(end);
}

void MainWindow::update_status_from_transport() {
    const auto display = playback_->display();
    // The Play button shows what Space does now: Stop (and return) while the transport runs or is
    // paused, Play when stopped.
    const bool stoppable = !transport_display_is_stopped(display);
    if (stoppable != showing_pause_) {
        showing_pause_ = stoppable;
        if (auto* play = command("transport.play_stop")) {
            const IconId icon = stoppable ? IconId::stop : IconId::play;
            const QString key = play->shortcut().toString(QKeySequence::NativeText);
            play->setIcon(make_icon(icon));
            play->setProperty("mwb_icon", static_cast<int>(icon));
            play->setIconText(stoppable ? tr("Stop") : tr("Play"));
            play->setToolTip(stoppable ? tr("Stop and return to %1 (%2)").arg(tr("where playback started"), key)
                                       : tr("Play from the edit cursor (%1)").arg(key));
        }
    }
    QString state;
    switch (display.state) {
    case TransportDisplay::State::stopped: state = tr("Stop"); break;
    case TransportDisplay::State::playing: state = tr("Play"); break;
    case TransportDisplay::State::paused: state = tr("Pause"); break;
    case TransportDisplay::State::counting_in: state = tr("Count"); break;
    case TransportDisplay::State::recording: state = tr("● Rec"); break;
    case TransportDisplay::State::device_lost: state = tr("! No audio"); break;
    }
    if (transport_owner_) {
        const auto& meter = transport_owner_->meter_map().segment_at(display.position);
        // Rounded to the 0.1 BPM the field edits, so 90 BPM (666667 us) reads 90, not 90.0.
        const double bpm = std::round(600'000'000.0 / static_cast<double>(transport_owner_->tempo_map().tempo_at(display.position))) / 10.0;
        position_label_->setText(QString("%1  %2  %3/%4  %5%6")
                                     .arg(QString::fromStdString(transport_owner_->meter_map().format(display.position)))
                                     .arg(tr("♩=%1").arg(QString::number(bpm, 'f', bpm == std::floor(bpm) ? 0 : 1)))
                                     .arg(meter.numerator)
                                     .arg(meter.denominator)
                                     .arg(state)
                                     .arg(display.playing_older_generation && display.state == TransportDisplay::State::playing
                                              ? tr(" *")
                                              : QString()));
        position_label_->setToolTip(display.playing_older_generation && display.state == TransportDisplay::State::playing
                                        ? tr("Playing the version before your latest edit; it updates in a moment")
                                        : tr("Transport position (bar.beat.tick), tempo, meter and state"));
    } else if (preview_) {
        // A library preview owns the transport: its own position, tempo and meter.
        const auto& meter = preview_->meter.segment_at(display.position);
        const double bpm =
            std::round(600'000'000.0 / static_cast<double>(std::max<std::int64_t>(1, preview_->tempo.tempo_at(display.position)))) / 10.0;
        position_label_->setText(QString("%1  %2  %3/%4  %5")
                                     .arg(QString::fromStdString(preview_->meter.format(display.position)))
                                     .arg(tr("♩=%1").arg(QString::number(bpm, 'f', bpm == std::floor(bpm) ? 0 : 1)))
                                     .arg(meter.numerator)
                                     .arg(meter.denominator)
                                     .arg(state));
        position_label_->setToolTip(tr("Previewing \"%1\" from the library").arg(preview_->title));
    }
    {
        for (int index = 0; index < tabs_->count(); ++index)
            if (auto* tab = qobject_cast<EditorTab*>(tabs_->widget(index))) {
                const bool owner = &tab->controller() == transport_owner_;
                const bool shown = owner && display.state != TransportDisplay::State::stopped &&
                                   display.state != TransportDisplay::State::device_lost;
                // Piano roll and drum grid both draw (and follow) the playhead (C1).
                tab->set_playhead(shown ? std::optional<Tick>(display.position) : std::nullopt);
                update_sounding_notes(*tab, shown && display.state != TransportDisplay::State::paused
                                                ? std::optional<Tick>(display.position)
                                                : std::nullopt);
            }
        // Forget closed tabs (their address may be reused by a new tab).
        std::erase_if(sounding_by_tab_, [this](const auto& entry) { return tabs_->indexOf(entry.first) < 0; });
    }
    if (auto* tab = current_tab(); tab && tempo_field_) {
        const bool ppq = tab->controller().active_sequence().division.is_ppq();
        tempo_field_->setEnabled(ppq);
        const auto microseconds = tab->controller().tempo_map().tempo_at(tempo_position(*tab));
        if (ppq && microseconds > 0)
            tempo_field_->show_tempo(60'000'000.0 / static_cast<double>(microseconds)); // skipped while being edited
    }
    // Short readout; the full device description is in the tooltip. Warnings use "!" not color alone.
    const QString device = display.device_description;
    // Only this timer reads the meters, so a clip is never swallowed by another caller; the
    // warning stays up for 2 s.
    if (playback_->take_meters().clipped)
        clip_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    const bool clipped = std::chrono::steady_clock::now() < clip_until_;
    device_label_->setText(tr("Audio: %1").arg(device.section(QString::fromUtf8(" · "), 0, 0)) +
                           (clipped ? tr("  ! CLIP") : QString()) +
                           (display.underflows ? tr("  ! %1 underflows").arg(display.underflows) : QString()));
    device_label_->setToolTip(device);
}

// --- Keyboard playing mode -----------------------------------------------------------------------

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // Holding Left/Right accelerates play-position steps: remember when the key went down (the
    // first non-repeated event) and forget it on the real release.
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress ||
        event->type() == QEvent::KeyRelease) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right) {
            if (event->type() == QEvent::KeyRelease) {
                if (!key->isAutoRepeat())
                    held_position_key_ = 0;
            } else if (held_position_key_ != key->key()) {
                held_position_key_ = key->key(); // a repeat without a seen press still starts the hold
                position_key_down_ = std::chrono::steady_clock::now();
            }
        }
    }
    // The mouse wheel over a spin box or combo box that is not being edited scrolls the panel
    // instead of silently changing the value (each change would be a document edit).
    if (event->type() == QEvent::Wheel) {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget && !widget->hasFocus() && (qobject_cast<QAbstractSpinBox*>(widget) || qobject_cast<QComboBox*>(widget))) {
            for (QWidget* parent = widget->parentWidget(); parent; parent = parent->parentWidget())
                if (auto* area = qobject_cast<QAbstractScrollArea*>(parent)) {
                    QCoreApplication::sendEvent(area->verticalScrollBar(), event);
                    break;
                }
            return true;
        }
    }
    // Which control got focus from the keyboard: Space activates a button or check cell reached
    // with Tab, while after a mouse click Space stays Play/Stop (a click on Apply followed by
    // Space listens instead of applying again).
    if (event->type() == QEvent::FocusIn && watched->isWidgetType()) {
        const auto reason = static_cast<QFocusEvent*>(event)->reason();
        if (reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason || reason == Qt::ShortcutFocusReason)
            keyboard_focus_ = qobject_cast<QWidget*>(watched);
        else if (reason == Qt::MouseFocusReason || reason == Qt::OtherFocusReason)
            keyboard_focus_ = nullptr; // window activation and popups keep the previous state
    }
    // Playing mode plays from the computer keyboard wherever the focus is in this window (track
    // list, panels, buttons), except in text input and modal dialogs (section 8.2). Releases are
    // always delivered, so a note never sticks when the focus moves while a key is held.
    if ((event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) && computer_keyboard_ &&
        computer_keyboard_->enabled()) {
        QWidget* focus = QApplication::focusWidget();
        const bool press = event->type() == QEvent::KeyPress;
        const bool ours = focus && (isAncestorOf(focus) || (qobject_cast<QDockWidget*>(focus->window()) &&
                                                            focus->window()->parentWidget() == this));
        if (ours && watched == focus && (!press || (!is_text_input(*focus) && !QApplication::activeModalWidget())))
            if (computer_keyboard_->handle_key(static_cast<QKeyEvent*>(event), press))
                return true;
    }
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        QWidget* focus = QApplication::focusWidget();
        // This window and its docks (also floating ones), never dialogs or popups.
        const bool ours = focus && (isAncestorOf(focus) || (qobject_cast<QDockWidget*>(focus->window()) &&
                                                            focus->window()->parentWidget() == this));
        if (ours && watched == focus)
            if (const auto handled = route_control_key(*key, *focus, event->type() == QEvent::ShortcutOverride))
                return *handled;
    }
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::is_text_input(QWidget& focus) {
    // An item view's cell editor is a QLineEdit, so editing a cell counts too.
    auto* combo = qobject_cast<QComboBox*>(&focus);
    return qobject_cast<QLineEdit*>(&focus) || qobject_cast<QAbstractSpinBox*>(&focus) || focus.inherits("QPlainTextEdit") ||
           focus.inherits("QTextEdit") || (combo && combo->view() && combo->view()->isVisible());
}

std::optional<bool> MainWindow::route_control_key(QKeyEvent& key, QWidget& focus, bool override) {
    const auto modifiers = key.modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier);
    const int code = key.key();
    if (modifiers != Qt::NoModifier)
        return std::nullopt; // modified shortcuts (Ctrl+Space, Ctrl+Z, ...) work everywhere
    // Delete/Backspace act on the focused panel's own content (C2, ux-panels-5): the library
    // trashes the chosen item, the track list deletes the active track; only the editors (roll,
    // lanes, drum grid, event list) reach the window's Delete Notes.
    if (override && (code == Qt::Key_Delete || code == Qt::Key_Backspace))
        for (QWidget* widget = &focus; widget; widget = widget->parentWidget())
            if (widget == library_ || qobject_cast<TrackListWidget*>(widget)) {
                key.accept();
                return false;
            }
    auto* button = qobject_cast<QAbstractButton*>(&focus);
    auto* view = qobject_cast<QAbstractItemView*>(&focus);
    auto* combo = qobject_cast<QComboBox*>(&focus);
    const bool text_input = is_text_input(focus);
    const QModelIndex current = view ? view->currentIndex() : QModelIndex();
    const bool check_cell = current.isValid() && (current.flags() & Qt::ItemIsUserCheckable) &&
                            (current.flags() & Qt::ItemIsEnabled);
    const bool by_keyboard = keyboard_focus_ == &focus;
    const bool enter = code == Qt::Key_Return || code == Qt::Key_Enter;
    const bool space = code == Qt::Key_Space;

    // Return/Enter press the focused button (check boxes and section headers toggle) and toggle a
    // focused check cell (track Mute/Solo); outside a QDialog Qt itself does neither.
    // Space does the same for a control reached with the keyboard; otherwise it plays.
    if ((button || check_cell) && (enter || (space && by_keyboard)) && !text_input) {
        if (override) {
            key.accept(); // the key goes to the control, not to a window shortcut
            return false;
        }
        if (key.isAutoRepeat())
            return true;
        if (button) {
            if (button->isEnabled())
                button->click(); // immediate: the key press is the feedback
        } else {
            const auto state = current.data(Qt::CheckStateRole).value<Qt::CheckState>();
            view->model()->setData(current, state == Qt::Checked ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
        }
        return true;
    }
    if (!override)
        return std::nullopt;
    // Navigation keys stay with every control that uses them (lists, combos, fields); buttons do
    // not, so the arrow commands still move notes after a click on a panel button.
    const bool navigation = code == Qt::Key_Up || code == Qt::Key_Down || code == Qt::Key_Left || code == Qt::Key_Right ||
                            code == Qt::Key_Home || code == Qt::Key_End || code == Qt::Key_PageUp ||
                            code == Qt::Key_PageDown;
    if (navigation && (text_input || view || combo)) {
        key.accept();
        return false;
    }
    // A number field never takes a space: Space plays even after a click on a spin-box arrow
    // left the focus there (ux-panels-20).
    const bool numeric = qobject_cast<QAbstractSpinBox*>(&focus) ||
                         (focus.parentWidget() && qobject_cast<QAbstractSpinBox*>(focus.parentWidget()));
    if (space && numeric) {
        key.ignore(); // not taken by the field: the Play/Stop shortcut fires
        return true;
    }
    // Letters and digits are the tool and transport commands: only real text input keeps them.
    // A closed combo would jump to a matching item (a document edit), a table would start renaming.
    const bool character = (code >= Qt::Key_0 && code <= Qt::Key_9) || (code >= Qt::Key_A && code <= Qt::Key_Z) || space;
    if (character && text_input && !(space && !focus.inherits("QLineEdit") && !focus.inherits("QPlainTextEdit") &&
                                      !focus.inherits("QTextEdit"))) {
        key.accept();
        return false;
    }
    return std::nullopt;
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    // Text fields receive typing first; playing mode suspends while one has focus (section 8.2).
    const auto* focus = QApplication::focusWidget();
    const bool text_focus = focus && (focus->inherits("QLineEdit") || focus->inherits("QPlainTextEdit") ||
                                      focus->inherits("QAbstractSpinBox") || focus->inherits("QTextEdit"));
    if (!text_focus && computer_keyboard_->handle_key(event, true))
        return;
    QMainWindow::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent* event) {
    if (computer_keyboard_->handle_key(event, false))
        return;
    QMainWindow::keyReleaseEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    // Narrow windows (below about 1300 px) get a compact toolbar so Drag MIDI and the readout stay
    // visible: the Play button shows only its icon and the "Snap" caption goes (the combo keeps
    // its accessible name and tooltip) (ux-visual-6).
    const bool compact = width() < 1300;
    if (compact == compact_toolbar_ || !transport_bar_)
        return;
    compact_toolbar_ = compact;
    if (snap_label_action_)
        snap_label_action_->setVisible(!compact);
    if (auto* play = command("transport.play_stop"))
        if (auto* button = qobject_cast<QToolButton*>(transport_bar_->widgetForAction(play)))
            button->setToolButtonStyle(compact ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
}

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::ActivationChange && !isActiveWindow()) {
        computer_keyboard_->release_all(); // deactivation releases only playing-mode notes
        held_position_key_ = 0;            // a release may never arrive
    }
    QMainWindow::changeEvent(event);
}

// --- Layouts, palette, about ---------------------------------------------------------------------

void MainWindow::apply_layout(const QString& name) {
    library_dock_->setFloating(false);
    inspector_dock_->setFloating(false);
    events_dock_->setFloating(false);
    if (name == "compose") {
        library_dock_->hide();
        inspector_dock_->show();
        events_dock_->hide();
    } else if (name == "library") {
        library_dock_->show();
        inspector_dock_->hide();
        events_dock_->hide();
        resizeDocks({library_dock_}, {420}, Qt::Horizontal);
    } else {
        library_dock_->hide();
        inspector_dock_->show();
        events_dock_->show();
    }
}

void MainWindow::reset_layout() {
    removeDockWidget(library_dock_);
    removeDockWidget(inspector_dock_);
    removeDockWidget(events_dock_);
    addDockWidget(Qt::LeftDockWidgetArea, library_dock_);
    addDockWidget(Qt::RightDockWidgetArea, inspector_dock_);
    addDockWidget(Qt::BottomDockWidgetArea, events_dock_);
    library_dock_->setVisible(library_->is_open());
    inspector_dock_->show();
    events_dock_->hide();
    resizeDocks({inspector_dock_}, {300}, Qt::Horizontal);
    if (library_->is_open())
        resizeDocks({library_dock_}, {280}, Qt::Horizontal);
}

void MainWindow::show_command_palette(const QString& query) {
    CommandPalette palette(commands_, this);
    if (!query.isEmpty())
        palette.set_query(query); // the characters typed into the search box are kept
    palette.exec();
}

void MainWindow::show_about() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("About MIDI Workbench"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* browser = new QTextBrowser(&dialog);
    QString html = tr("<h2>MIDI Workbench %1</h2><p>Offline MIDI composition and library workbench. "
                      "License: GPL-3.0.</p><p>Built with %2, Qt %3.</p><h3>Third-party software</h3>")
                       .arg(build_info::version, build_info::compiler, build_info::qt_version);
    for (const auto& notice : app::third_party_notices())
        html += QString("<h4>%1 %2 (%3)</h4><p>%4<br><a href=\"%5\">%5</a></p><pre>%6</pre>")
                    .arg(QString::fromUtf8(notice.name.data(), static_cast<qsizetype>(notice.name.size())),
                         QString::fromUtf8(notice.version.data(), static_cast<qsizetype>(notice.version.size())),
                         QString::fromUtf8(notice.license.data(), static_cast<qsizetype>(notice.license.size())),
                         QString::fromUtf8(notice.purpose.data(), static_cast<qsizetype>(notice.purpose.size())),
                         QString::fromUtf8(notice.homepage.data(), static_cast<qsizetype>(notice.homepage.size())),
                         QString::fromUtf8(notice.license_text.data(), static_cast<qsizetype>(notice.license_text.size())).toHtmlEscaped());
    browser->setHtml(html);
    // Links open in the browser; the dialog never tries to load a web page itself, which would
    // replace the license text with an error (ux-panels-41).
    browser->setOpenExternalLinks(false);
    browser->setOpenLinks(false);
    connect(browser, &QTextBrowser::anchorClicked, browser, [](const QUrl& url) { QDesktopServices::openUrl(url); });
    layout->addWidget(browser);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(720, 640);
    dialog.exec();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (!event->mimeData()->hasUrls())
        return;
    for (const auto& url : event->mimeData()->urls()) {
        const QString file = url.toLocalFile().toLower();
        if (file.endsWith(".mid") || file.endsWith(".midi") || file.endsWith(".rmi") || file.endsWith(".mwb")) {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    // Files are identified by content when opened; the extension only filters the drag.
    for (const auto& url : event->mimeData()->urls())
        if (url.isLocalFile())
            open_path(to_path(url.toLocalFile()));
    event->acceptProposedAction();
}

void MainWindow::show_diagnostics() {
    // Section 30.4: device configuration, actual audio period and rate, underflows, snapshot
    // generation, SQLite and build identity. Nothing leaves the machine.
    const auto display = playback_->display();
    QString text = tr("<h3>Audio</h3><p>%1</p><p>Deadline misses (underflow estimate): %2<br>Playback snapshot generation: %3%4</p>")
                       .arg(display.device_description.toHtmlEscaped())
                       .arg(display.underflows)
                       .arg(display.snapshot_generation)
                       .arg(display.playing_older_generation ? tr(" (an earlier edit is still playing)") : QString());
    text += tr("<h3>Files</h3><p>Data: %1<br>Recovery: %2<br>Logs: %3</p>")
                .arg(to_qstring(directories_.root).toHtmlEscaped(), to_qstring(directories_.recovery).toHtmlEscaped(),
                     to_qstring(directories_.logs).toHtmlEscaped());
    if (auto* tab = current_tab(); tab && tab->controller().recovery_journal())
        text += tr("<p>Last durable recovery write: %1</p>")
                    .arg(QDateTime::fromMSecsSinceEpoch(tab->controller().recovery_journal()->last_durable_ms()).toString());
    text += tr("<h3>Build and dependencies</h3><pre>%1</pre>").arg(QString::fromStdString(app::capabilities_json()).toHtmlEscaped());
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Diagnostics"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* browser = new QTextBrowser(&dialog);
    browser->setHtml(text);
    layout->addWidget(browser);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    // One click puts the whole report on the clipboard for a support request (ux-panels-41).
    auto* copy = buttons->addButton(tr("Copy to Clipboard"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, browser, [browser] { QApplication::clipboard()->setText(browser->toPlainText()); });
    layout->addWidget(buttons);
    dialog.resize(720, 600);
    dialog.exec();
}

void MainWindow::write_screenshot_and_exit() {
    if (pending_opens_ > 0) {
        QTimer::singleShot(20, this, &MainWindow::write_screenshot_and_exit); // files still opening
        return;
    }
    if (!options_.screenshot.empty()) {
        if (auto* tab = current_tab())
            tab->piano_roll().fit_all();
        update_status_from_transport();
        // Let queued layout requests settle (text set after a file opened changes size hints).
        for (int pass = 0; pass < 3; ++pass)
            QApplication::processEvents();
        repaint();
        const QPixmap pixmap = grab();
        if (!pixmap.save(to_qstring(options_.screenshot)))
            std::fprintf(stderr, "error: could not write screenshot\n");
    }
    if (options_.exit_when_ready) {
        for (int index = 0; index < tabs_->count(); ++index)
            if (auto* tab = qobject_cast<EditorTab*>(tabs_->widget(index)))
                tab->controller().close_recovery();
        QApplication::exit(0);
    }
}

} // namespace mwb::ui
