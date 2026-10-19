#include "mwb/ui/main_window.hpp"

#include "mwb/ui/icons.hpp"
#include "mwb/ui/theme.hpp"

#include "mwb/app/document_io.hpp"
#include "mwb/app/third_party.hpp"
#include "mwb/build_info.hpp"
#include "mwb/core/note_edit.hpp"
#include "mwb/core/pitch.hpp"
#include "mwb/document/native_format.hpp"
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
#include "mwb/ui/editor_tab.hpp"
#include "mwb/ui/event_list.hpp"
#include "mwb/ui/inspector.hpp"
#include "mwb/ui/library_panel.hpp"
#include "mwb/ui/playback_service.hpp"
#include "mwb/ui/recording_controller.hpp"
#include "mwb/ui/shortcut_editor.hpp"

#include <QApplication>
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

namespace mwb::ui {

namespace {

constexpr int layout_schema_version = 2; // 2: toolbar and default dock sizes of the UI redesign

/// Snap values offered on the toolbar (the inspector offers the full list).
const std::pair<const char*, const char*> toolbar_snaps[] = {{"1/4", "1/4"}, {"1/8", "1/8"}, {"1/16", "1/16"},
                                                             {"1/32", "1/32"}, {"1/12", "1/8 triplet"},
                                                             {"1/24", "1/16 triplet"}};

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
    setDockNestingEnabled(true);
    setAcceptDrops(true); // .mid/.midi/.rmi/.mwb files can be dropped in (section 14.3)
    clean_drag_cache(directories_.drag_cache);
    tabs_ = new QTabWidget(this);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    tabs_->setAccessibleName(tr("Open documents"));
    setCentralWidget(tabs_);
    playback_ = create_playback_service(nullptr); // owned by the unique_ptr
    computer_keyboard_ = new ComputerKeyboardPlayer(this);
    recording_ = new RecordingController(*playback_, this);
    connect(recording_, &RecordingController::status_message, this, [this](const QString& text) { statusBar()->showMessage(text, 8000); });
    build_docks();
    library_->set_drag_cache(directories_.drag_cache);
    io_thread_ = std::make_unique<TaskThread>();
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
    {
        // Saved shortcut profile (versioned JSON); a damaged profile keeps the defaults.
        const QString profile = QSettings().value("shortcuts/profile").toString();
        QStringList messages;
        if (!profile.isEmpty() && !apply_shortcuts_json(commands_, profile, &messages))
            statusBar()->showMessage(tr("Saved shortcuts could not be read; defaults are used"), 8000);
    }

    cursor_label_ = new QLabel(this);
    selection_label_ = new QLabel(this);
    snap_label_ = new QLabel(this);
    device_label_ = new QLabel(this);
    recovery_label_ = new QLabel(this);
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
    statusBar()->showMessage(tr("Tip: Ctrl+Shift+P finds any command; press 2 to draw notes"), 15000);

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
        QTimer::singleShot(0, this, &MainWindow::offer_recovery);
    if (options_.exit_when_ready || !options_.screenshot.empty())
        QTimer::singleShot(0, this, &MainWindow::write_screenshot_and_exit);
}

MainWindow::~MainWindow() {
    transport_timer_.stop();
    recovery_timer_.stop();
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
    add_command("file.recover", tr("Recover Sessions…"), {}, [this] { offer_recovery(); });
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
    // line (the play position) one grid step, also while playing.
    add_command("edit.move_left", tr("Move Notes or Play Position Left"), QKeySequence(Qt::Key_Left),
                [this] { move_selection_by_grid(-1); });
    add_command("edit.move_right", tr("Move Notes or Play Position Right"), QKeySequence(Qt::Key_Right),
                [this] { move_selection_by_grid(1); });
    // Ctrl+Left/Right always move the play position one grid cell (Space then plays from there),
    // whether or not notes are selected.
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
        const QStringList kinds = {tr("CC number…"), tr("Pitch bend"), tr("Channel pressure"), tr("Release velocity")};
        const QString kind = QInputDialog::getItem(this, tr("Add lane"), tr("Lane"), kinds, 0, false, &ok_value);
        if (!ok_value)
            return;
        LaneTarget target;
        const auto selected = tab.controller().selected_notes();
        target.channel = selected.empty() ? 0 : selected.front().channel;
        if (kind == kinds[0]) {
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
        library_->save_document(std::make_shared<const DocumentData>(tab->controller().document()),
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
    });
    playing_mode_action_->setCheckable(true);

    add_command("transport.play_pause", tr("Play/Pause"), QKeySequence(Qt::Key_Space), [this] { toggle_play(); });
    add_command("transport.play_selection", tr("Play from Selection"), QKeySequence(Qt::SHIFT | Qt::Key_Space),
                [this] { play_from_selection(); });
    add_command("transport.stop", tr("Stop and Return"), QKeySequence(Qt::CTRL | Qt::Key_Space), [this] { stop_transport(); });
    add_command("transport.go_to_start", tr("Go to Start"), QKeySequence(Qt::Key_Home), [this] {
        if (auto* tab = current_tab()) {
            if (!transport_display_is_stopped(playback_->display()))
                playback_->stop(0);
            tab->controller().set_edit_cursor(0);
            tab->piano_roll().scroll_to_tick(0);
        }
    });
    loop_action_ = add_command("transport.loop", tr("Loop"), QKeySequence(Qt::Key_L), [this] { publish_playback(); });
    loop_action_->setCheckable(true);
    loop_action_->setChecked(true);
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
        if (auto* tab = current_tab(); tab && transport_display_is_stopped(playback_->display())) {
            transport_owner_ = &tab->controller();
            publish_playback(); // recording plays the current document from the count-in
        }
        recording_->toggle_recording();
    });
    record_action_->setCheckable(true);
    connect(recording_, &RecordingController::recording_changed, record_action_, &QAction::setChecked);
    step_action_ = add_command("transport.step_record", tr("Step Recording"), {}, [this] {
        recording_->set_step_recording(step_action_->isChecked());
    });
    step_action_->setCheckable(true);
    add_command("transport.step_rest", tr("Step Rest"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Right), [this] { recording_->step_rest(); });
    add_command("transport.step_tie", tr("Step Tie"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_T), [this] { recording_->step_tie(); });
    add_command("transport.step_back", tr("Step Back"), QKeySequence(Qt::CTRL | Qt::Key_Backspace), [this] { recording_->step_back(); });
    for (int bars = 0; bars <= 2; ++bars)
        add_command(QString("transport.count_in_%1").arg(bars), tr("Count-in %n bar(s)", nullptr, bars), {},
                    [this, bars] { recording_->set_count_in_bars(bars); });
    add_command("transport.midi_input", tr("Choose MIDI Input…"), {}, [this] {
        const auto ports = recording_->input_ports();
        QStringList names{tr("None")};
        for (const auto& [id, name] : ports)
            names << name;
        bool chosen = false;
        const QString name = QInputDialog::getItem(this, tr("MIDI Input"), tr("Input device"), names, 0, false, &chosen);
        if (!chosen)
            return;
        QString id;
        for (const auto& [port_id, port_name] : ports)
            if (port_name == name)
                id = port_id;
        QString error;
        if (!recording_->open_input(id, &error))
            QMessageBox::warning(this, tr("MIDI Input"), tr("Could not open %1: %2").arg(name, error));
        else
            statusBar()->showMessage(tr("MIDI input: %1").arg(recording_->input_name()), 5000);
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
    add(menuBar()->addMenu(tr("T&ransport")), {"transport.play_pause", "transport.play_selection", "transport.stop",
                                                "transport.go_to_start", "transport.position_left", "transport.position_right",
                                                nullptr, "transport.loop", "transport.metronome",
                                                nullptr, "transport.record", "transport.count_in_0", "transport.count_in_1",
                                                "transport.count_in_2", "transport.midi_input", nullptr,
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
    connect(library_, &LibraryPanel::open_requested, this, [this](DocumentData document, qint64 asset, qint64 revision) {
        open_prepared(std::move(document), false, [this, asset, revision](EditorTab* tab) {
            library_sources_[tab] = {asset, revision};
        });
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
    connect(library_, &LibraryPanel::opened, this, [this] {
        if (!library_dock_->isVisible()) {
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
        if (visible && tab && !events_dock_->widget())
            events_dock_->setWidget(new EventListPanel(tab->controller(), events_dock_));
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
    transport_bar_->addWidget(snap_label);
    snap_combo_ = new QComboBox(this);
    snap_combo_->setAccessibleName(tr("Snap"));
    snap_combo_->setToolTip(tr("Grid used for drawing, moving and resizing notes"));
    for (const auto& [value, label] : toolbar_snaps)
        snap_combo_->addItem(tr(label), QString::fromLatin1(value));
    snap_combo_->setMinimumWidth(96);
    transport_bar_->addWidget(snap_combo_);
    connect(snap_combo_, &QComboBox::activated, this, [this] {
        auto* tab = current_tab();
        if (!tab || updating_toolbar_)
            return;
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
    add("transport.stop", IconId::stop);
    if (auto* play = add("transport.play_pause", IconId::play)) {
        if (auto* button = qobject_cast<QToolButton*>(transport_bar_->widgetForAction(play))) {
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        }
        play->setIconText(tr("Play"));
    }
    add("transport.record", IconId::record);
    add("transport.loop", IconId::loop);
    add("transport.metronome", IconId::metronome);
    auto* tempo_label = new QLabel(tr(" Speed × "), this);
    tempo_label->setForegroundRole(QPalette::PlaceholderText);
    transport_bar_->addWidget(tempo_label);
    tempo_multiplier_ = new QDoubleSpinBox(this);
    tempo_multiplier_->setRange(0.25, 4.0);
    tempo_multiplier_->setSingleStep(0.05);
    tempo_multiplier_->setValue(1.0);
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

    auto* spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    transport_bar_->addWidget(spacer);
    drag_handle_ = new DragHandle(directories_.drag_cache, this);
    connect(drag_handle_, &DragHandle::status_message, this, [this](const QString& text) { statusBar()->showMessage(text, 6000); });
    transport_bar_->addWidget(drag_handle_);
    transport_bar_->addSeparator();
    for (auto [dock, icon, key] : {std::tuple{library_dock_, IconId::library, Qt::Key_1},
                                   std::tuple{inspector_dock_, IconId::inspector, Qt::Key_2}}) {
        QAction* toggle = dock->toggleViewAction();
        toggle->setIcon(make_icon(icon));
        toggle->setProperty("mwb_icon", static_cast<int>(icon));
        toggle->setShortcut(QKeySequence(Qt::CTRL | key));
        transport_bar_->addAction(toggle);
    }
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
    tab->set_keyboard_visible(keyboard_action_ && keyboard_action_->isChecked());
    tab->piano_roll().fit_all();
    QPointer<EditorTab> fit_target(tab);
    QTimer::singleShot(0, this, [fit_target] {
        if (fit_target)
            fit_target->piano_roll().fit_all(); // again once the layout has its final size
    });
}

bool MainWindow::open_path(const std::filesystem::path& path) {
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
    connect(&controller, &DocumentController::dirty_changed, this, [this, tab](bool dirty) {
        const int index = tabs_->indexOf(tab);
        if (index >= 0)
            tabs_->setTabText(index, QString::fromStdString(tab->controller().document().title) + (dirty ? "*" : ""));
    });
    connect(&controller, &DocumentController::selection_changed, this, &MainWindow::update_command_state);
    connect(&controller, &DocumentController::save_finished, this, [this, tab](const QString& error) {
        if (!error.isEmpty()) {
            QMessageBox::warning(this, tr("Save"), tr("The document was not saved:\n%1").arg(error));
            return;
        }
        statusBar()->showMessage(tr("Saved %1").arg(to_qstring(tab->controller().file_path().filename())), 4000);
    });
    connect(&controller, &DocumentController::edit_rejected, this,
            [this](const QString& message) { statusBar()->showMessage(message, 8000); });
    connect(&view, &PianoRollView::status_message, this, [this](const QString& text) { statusBar()->showMessage(text, 4000); });
    connect(&view, &PianoRollView::tool_changed, this, &MainWindow::update_command_state);
    connect(tab, &EditorTab::status_message, this, [this](const QString& text) { statusBar()->showMessage(text, 6000); });
    connect(&view, &PianoRollView::hover_changed, this, [this, tab](Tick tick, int pitch) {
        cursor_label_->setText(QString("%1  %2 (%3)")
                                   .arg(QString::fromStdString(tab->controller().meter_map().format(std::max<Tick>(0, tick))))
                                   .arg(QString::fromStdString(pitch_name(static_cast<std::uint8_t>(std::clamp(pitch, 0, 127)))))
                                   .arg(pitch));
    });
    const auto preset = [tab]() {
        const auto active = tab->controller().active_track_index();
        return active ? tab->controller().active_sequence().tracks[*active].instrument.synth_preset : std::string("classic");
    };
    connect(&view, &PianoRollView::audition_note_on, this, [this, preset](int pitch, int velocity) {
        playback_->audition_note_on(pitch, velocity, 0, preset());
    });
    connect(&view, &PianoRollView::audition_note_off, this, [this](int pitch) { playback_->audition_note_off(pitch, 0); });
    connect(&tab->keyboard(), &KeyboardWidget::note_on, this, [this, preset](int pitch, int velocity) {
        playback_->audition_note_on(pitch, velocity, 0, preset());
        recording_->live_note(pitch, velocity, true);
    });
    connect(&tab->keyboard(), &KeyboardWidget::note_off, this, [this](int pitch) {
        playback_->audition_note_off(pitch, 0);
        recording_->live_note(pitch, 0, false);
    });
}

void MainWindow::current_tab_changed() {
    auto* tab = current_tab();
    inspector_->set_controller(tab ? &tab->controller() : nullptr);
    recording_->set_target(tab ? &tab->controller() : nullptr, tab ? &tab->keyboard() : nullptr);
    drag_handle_->set_controller(tab ? &tab->controller() : nullptr);
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
        const auto snap_index = snap_combo_->findData(QString::fromStdString(tab->controller().active_sequence().settings.snap.to_string()));
        if (snap_index >= 0)
            snap_combo_->setCurrentIndex(snap_index);
        updating_toolbar_ = false;
        const bool has_loop = tab->controller().active_sequence().settings.loop_region.has_value();
        loop_action_->setEnabled(has_loop);
        loop_action_->setToolTip(has_loop ? tr("Loop the loop region (L)")
                                          : tr("No loop region: set a loop length in the Inspector's Song section"));
        const auto count = tab->controller().selection().size();
        selection_label_->setText(count == 0 ? tr("Nothing selected") : tr("%n note(s) selected", nullptr, static_cast<int>(count)));
        const auto grid = tab->piano_roll().grid();
        snap_label_->setText(tr("Snap %1%2").arg(QString::fromStdString(grid.fraction.to_string()))
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
    if (blocking || options_.exit_when_ready || isHidden()) {
        // Closing the window must not race a background save.
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

bool MainWindow::confirm_close(EditorTab* tab) {
    tab->controller().wait_for_save(); // a save in flight lands (or fails) before the question
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
    if (auto* tab = current_tab(); tab && tab->controller().recovery_journal()) {
        const auto ms = tab->controller().recovery_journal()->last_durable_ms();
        const QString& error = tab->controller().recovery_error();
        recovery_label_->show();
        recovery_label_->setText(error.isEmpty()
                                     ? tr("Recovery saved %1").arg(QDateTime::fromMSecsSinceEpoch(ms).toString("HH:mm:ss"))
                                     : tr("! Recovery not saved: %1").arg(error));
    }
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
    });
}

void MainWindow::offer_recovery() {
    // Listing reads SQLite: done on the I/O thread; the dialog appears when the list arrives.
    QPointer<MainWindow> guard(this);
    const auto recovery_directory = directories_.recovery;
    io_thread_->post([guard, recovery_directory](std::stop_token) {
        auto sessions = std::make_shared<Result<std::vector<document::RecoverableSession>>>(
            document::list_recoverable_sessions(recovery_directory));
        QMetaObject::invokeMethod(
            qApp,
            [guard, sessions] {
                if (!guard)
                    return;
                if (!*sessions) {
                    guard->statusBar()->showMessage(
                        tr("Could not read recovery data: %1").arg(QString::fromStdString(sessions->error().describe())), 8000);
                    return;
                }
                guard->present_recovery(std::move(**sessions));
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::present_recovery(std::vector<document::RecoverableSession> sessions) {
    // Exclude sessions this process owns.
    std::vector<document::RecoverableSession> candidates;
    for (const auto& session : sessions) {
        bool own = false;
        for (int index = 0; index < tabs_->count(); ++index)
            if (auto* tab = qobject_cast<EditorTab*>(tabs_->widget(index)); tab && tab->controller().recovery_journal() &&
                                                                              tab->controller().recovery_journal()->session_id() == session.session_id)
                own = true;
        if (!own && !session.live) // another running instance's sessions are not crashed work
            candidates.push_back(session);
    }
    if (candidates.empty())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Recover unsaved work"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("These sessions were not closed normally. Recovery opens a new unsaved document; "
                                    "it never replaces a saved file."),
                                 &dialog));
    auto* list = new QListWidget(&dialog);
    for (const auto& session : candidates) {
        const QString saved = session.source.last_explicit_save_ms
                                  ? QDateTime::fromMSecsSinceEpoch(*session.source.last_explicit_save_ms).toString()
                                  : tr("never saved");
        auto* item = new QListWidgetItem(tr("%1 — last durable edit %2, last save %3%4")
                                             .arg(QString::fromStdString(session.source.document_name))
                                             .arg(QDateTime::fromMSecsSinceEpoch(session.last_durable_edit_ms).toString())
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
    connect(discard, &QPushButton::clicked, &dialog, [&] {
        discard_chosen = true;
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
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
                if (*recovered) {
                    document::SourceReference source;
                    source.document_name = (*recovered)->document.title;
                    if (auto created = document::RecoveryJournal::create(recovery_directory, (*recovered)->document, source))
                        journal = std::move(*created);
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
                if (!*recovered || !journal)
                    continue; // keep the old evidence until the recovered document is protected again
            }
            std::ignore = document::discard_recovery_session(recovery_directory, session_id); // evidence no longer needed
        }
    });
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
    auto preview = music::move_notes(controller.document(), selection, parameters);
    if (preview)
        controller.apply(preview->changes);
    else
        statusBar()->showMessage(QString::fromStdString(preview.error().message), 5000);
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
    auto preview = music::transpose(controller.document(), selection, parameters);
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

void MainWindow::move_play_position(int direction) {
    auto* tab = current_tab();
    if (!tab)
        return;
    auto& controller = tab->controller();
    const auto grid = tab->piano_roll().grid();
    const auto display = playback_->display();
    const bool rolling = !transport_display_is_stopped(display) && transport_owner_ == &controller;
    const Tick from = rolling ? display.position : controller.edit_cursor();
    const Tick to = direction < 0 ? std::max<Tick>(0, grid.floor(from > 0 ? from - 1 : 0)) : grid.next(from);
    if (rolling)
        playback_->seek(to); // the playhead jumps; playback continues from there
    controller.set_edit_cursor(to);
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
    auto preview = music::move_notes(controller.document(), selection, parameters);
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
    playback_->publish_document(transport_owner_->document(), transport_owner_->active_sequence_index(),
                                transport_owner_->generation());
}

void MainWindow::toggle_play() {
    auto* tab = current_tab();
    if (!tab)
        return;
    const auto state = playback_->display().state;
    if (state == TransportDisplay::State::playing) {
        playback_->pause();
        return;
    }
    if (transport_owner_ != &tab->controller()) {
        playback_->stop(0);
        transport_owner_ = &tab->controller();
    }
    publish_playback();
    playback_->play(state == TransportDisplay::State::paused ? playback_->display().position : tab->controller().edit_cursor());
}

void MainWindow::play_from_selection() {
    auto* tab = current_tab();
    if (!tab)
        return;
    const auto notes = tab->controller().selected_notes();
    Tick start = tab->controller().edit_cursor();
    if (!notes.empty())
        start = DragSnapshot::capture(notes).earliest_start;
    transport_owner_ = &tab->controller();
    publish_playback();
    playback_->play(start);
}

void MainWindow::stop_transport() {
    auto* tab = current_tab();
    playback_->stop(tab ? tab->controller().edit_cursor() : 0);
}

void MainWindow::update_status_from_transport() {
    const auto display = playback_->display();
    // The Play button shows what pressing it does: Pause while the transport runs.
    const bool rolling = display.state == TransportDisplay::State::playing ||
                         display.state == TransportDisplay::State::recording ||
                         display.state == TransportDisplay::State::counting_in;
    if (rolling != showing_pause_) {
        showing_pause_ = rolling;
        if (auto* play = command("transport.play_pause")) {
            const IconId icon = rolling ? IconId::pause : IconId::play;
            play->setIcon(make_icon(icon));
            play->setProperty("mwb_icon", static_cast<int>(icon));
            play->setIconText(rolling ? tr("Pause") : tr("Play"));
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
        const double bpm = 60'000'000.0 / static_cast<double>(transport_owner_->tempo_map().tempo_at(display.position));
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
        for (int index = 0; index < tabs_->count(); ++index)
            if (auto* tab = qobject_cast<EditorTab*>(tabs_->widget(index)))
                tab->piano_roll().set_playhead(&tab->controller() == transport_owner_ &&
                                                       display.state != TransportDisplay::State::stopped
                                                   ? std::optional<Tick>(display.position)
                                                   : std::nullopt);
    }
    // Short readout; the full device description is in the tooltip. Warnings use "!" not color alone.
    const QString device = display.device_description;
    device_label_->setText(tr("Audio: %1").arg(device.section(QString::fromUtf8(" · "), 0, 0)) +
                           (display.clipped ? tr("  ! CLIP") : QString()) +
                           (display.underflows ? tr("  ! %1 underflows").arg(display.underflows) : QString()));
    device_label_->setToolTip(device);
}

// --- Keyboard playing mode -----------------------------------------------------------------------

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
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
    // Single-key window shortcuts (Space, arrows, letters, digits) must not take keys from a
    // focused button, check box, combo box, list, table or spin box: accepting ShortcutOverride
    // hands the key to the control. Modified shortcuts, F-keys and Escape still work everywhere,
    // so Stop (Ctrl+Space) and Panic (F12) are always available.
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        QWidget* focus = QApplication::focusWidget();
        const auto modifiers = key->modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier);
        const int code = key->key();
        const bool plain_key = modifiers == Qt::NoModifier &&
                               (code == Qt::Key_Up || code == Qt::Key_Down || code == Qt::Key_Left ||
                                code == Qt::Key_Right || code == Qt::Key_Home || code == Qt::Key_End ||
                                code == Qt::Key_PageUp || code == Qt::Key_PageDown || // not Delete: it deletes notes everywhere
                                (code >= Qt::Key_0 && code <= Qt::Key_9) || (code >= Qt::Key_A && code <= Qt::Key_Z));
        const bool control = focus && (qobject_cast<QAbstractButton*>(focus) || qobject_cast<QComboBox*>(focus) ||
                                       qobject_cast<QAbstractItemView*>(focus) || qobject_cast<QAbstractSpinBox*>(focus) ||
                                       qobject_cast<QLineEdit*>(focus));
        // Space is Play/Pause everywhere except text fields (which keep it themselves): buttons are
        // pressed with Enter, so pressing Space after Apply listens instead of applying again.
        if (plain_key && control && watched == focus) {
            event->accept();
            return false;
        }
    }
    return QMainWindow::eventFilter(watched, event);
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

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::ActivationChange && !isActiveWindow())
        computer_keyboard_->release_all(); // deactivation releases only playing-mode notes
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
    browser->setOpenExternalLinks(false);
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
