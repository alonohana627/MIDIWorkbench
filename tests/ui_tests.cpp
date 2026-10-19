#include "test_support.hpp"

#include "mwb/core/note_edit.hpp"
#include "mwb/app/environment.hpp"
#include "mwb/core/random.hpp"
#include "mwb/core/serialization.hpp"
#include "mwb/document/native_format.hpp"
#include "mwb/document/recovery.hpp"
#include "mwb/document/session.hpp"
#include "mwb/platform/midi_backend.hpp"
#include "mwb/ui/clipboard.hpp"
#include "mwb/ui/document_controller.hpp"
#include "mwb/ui/drag_export.hpp"
#include "mwb/ui/drum_grid.hpp"
#include "mwb/ui/editor_math.hpp"
#include "mwb/ui/keyboard_widget.hpp"
#include "mwb/ui/controller_lane.hpp"
#include "mwb/ui/editor_tab.hpp"
#include "mwb/ui/event_list.hpp"
#include "mwb/ui/inspector.hpp"
#include "mwb/ui/library_panel.hpp"
#include "mwb/ui/main_window.hpp"
#include "mwb/ui/piano_roll.hpp"
#include "mwb/ui/playback_service.hpp"
#include "mwb/ui/recording_controller.hpp"
#include "mwb/ui/tempo_field.hpp"
#include "mwb/ui/theme.hpp"

#include <QApplication>
#include <QPlainTextEdit>

#include <chrono>
#include <deque>
#include <fstream>
#include <cmath>
#include <mutex>
#include <QAction>
#include <QCheckBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QLabel>
#include <QPushButton>
#include <QMessageBox>
#include <QSettings>
#include <QTimer>
#include <QHeaderView>
#include <QSpinBox>
#include <QStyleOption>
#include <QTableView>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolBar>
#include <QToolButton>
#include <QLineEdit>
#include <QMimeData>
#include <QSignalSpy>
#include <QTest>

using namespace mwb;
using namespace mwb::ui;

namespace {

QApplication& application() {
    static int argc = 1;
    static char name[] = "mwb_ui_tests";
    static char* argv[] = {name, nullptr};
    static QApplication instance(argc, argv);
    return instance;
}

std::unique_ptr<DocumentController> make_controller() {
    application();
    auto session = std::make_unique<document::WorkingSession>(make_blank_document("Test"),
                                                              document::SessionOrigin::new_document);
    return std::make_unique<DocumentController>(std::move(session));
}

NoteSpan span(Tick start, Tick end, std::uint8_t pitch) {
    NoteSpan note;
    note.on_id = EventId::generate();
    note.start = start;
    note.end = end;
    note.pitch = pitch;
    note.velocity = 100;
    return note;
}

} // namespace

TEST_SUITE("editor_math") {
    TEST_CASE("grids derive points from the origin and report inexact resolutions") {
        const auto sixteenth = Grid::make(Rational{1, 16}, 960);
        CHECK(sixteenth.exact);
        CHECK(sixteenth.point(3) == 720);
        CHECK(sixteenth.nearest(359) == 240);
        CHECK(sixteenth.nearest(360) == 240); // ties choose the earlier point
        CHECK(sixteenth.nearest(361) == 480);
        const auto quintuplet = Grid::make(Rational{1, 20}, 96);
        CHECK(quintuplet.exact == false);
        CHECK(quintuplet.point(5) == 96); // derived from the origin, no drift
        CHECK(quintuplet.point(10) == 192);
    }

    TEST_CASE("a move constrains the whole displacement at the range edges") {
        const auto snapshot = DragSnapshot::capture({span(100, 200, 120), span(300, 400, 60)});
        const auto grid = Grid::make(Rational{1, 16}, 960);
        auto result = compute_move(snapshot, -500, 20, grid, SnapMode::off);
        CHECK(result.constrained);
        CHECK(result.delta_ticks == -100); // earliest note lands on tick 0
        CHECK(result.delta_pitch == 7);    // highest note lands on 127; intervals preserved
        result = compute_move(snapshot, 130, 0, grid, SnapMode::absolute);
        CHECK(result.delta_ticks == 140);  // anchor 100 + 130 -> nearest grid point 240
        result = compute_move(snapshot, 130, 0, grid, SnapMode::relative);
        CHECK(result.delta_ticks == 240);  // displacement snapped to whole steps
    }

    TEST_CASE("group resize keeps every note at least one tick long") {
        const auto snapshot = DragSnapshot::capture({span(0, 100, 60), span(0, 30, 62)});
        const auto grid = Grid::make(Rational{1, 16}, 960);
        CHECK(compute_end_delta(snapshot, snapshot.notes[0], -500, grid, SnapMode::off) == -29);
        CHECK(compute_start_delta(snapshot, snapshot.notes[0], 500, grid, SnapMode::off) == 29);
    }

    TEST_CASE("duplicate-to-end uses the grid-aligned extent including note ends") {
        const auto grid = Grid::make(Rational{1, 4}, 960);
        const auto extent = grid_aligned_extent({span(100, 1000, 60), span(500, 1900, 64)}, grid);
        CHECK(extent.begin == 0);
        CHECK(extent.end == 1920);
    }

    TEST_CASE("zooming time keeps the tick under the pointer") {
        ViewMapping mapping;
        mapping.origin_tick = 1000;
        mapping.ticks_per_pixel = 10;
        const Tick before = mapping.x_to_tick(200);
        mapping.zoom_time_around(200, 2.0);
        CHECK(std::abs(mapping.x_to_tick(200) - before) <= 5);
    }
}

TEST_SUITE("controller") {
    TEST_CASE("an edit that pushes old undo steps out of the budget says so (code-document-17)") {
        application();
        auto session = std::make_unique<document::WorkingSession>(make_blank_document("Budget"),
                                                                  document::SessionOrigin::new_document, std::nullopt, 4096);
        DocumentController controller(std::move(session));
        QSignalSpy trimmed(&controller, &DocumentController::history_trimmed);
        for (int index = 0; index < 40 && trimmed.isEmpty(); ++index) {
            const NewNote note{controller.active_track(), index * 480, index * 480 + 240, 60, 100, 0, 0};
            auto changes = insert_notes(controller.document(), std::span(&note, 1));
            REQUIRE(changes);
            REQUIRE(controller.apply(*changes));
        }
        CHECK(trimmed.count() == 1);
        CHECK(controller.can_undo()); // the newest step is always kept
    }
}

TEST_SUITE("controller") {
    TEST_CASE("edits, undo and dirty state go through the working session") {
        auto controller = make_controller();
        CHECK_FALSE(controller->is_dirty());
        const auto before = semantic_hash(controller->document());
        const NewNote note{controller->active_track(), 0, 480, 60, 100, 0, 0};
        auto changes = insert_notes(controller->document(), std::span(&note, 1));
        REQUIRE(changes);
        QSignalSpy changed(controller.get(), &DocumentController::document_changed);
        CHECK(controller->apply(*changes));
        CHECK(changed.count() == 1);
        CHECK(controller->notes().notes.size() == 1);
        CHECK(controller->is_dirty());
        CHECK(controller->undo());
        CHECK(semantic_hash(controller->document()) == before);
        CHECK_FALSE(controller->is_dirty());
        CHECK(controller->redo());
        CHECK(controller->notes().notes.size() == 1);
    }

    TEST_CASE("a background save completes, keeps later edits dirty and can be awaited") {
        test::TemporaryDirectory directory;
        auto controller = make_controller();
        const NewNote note{controller->active_track(), 0, 480, 60, 100, 0, 0};
        REQUIRE(controller->apply(*insert_notes(controller->document(), std::span(&note, 1))));
        QSignalSpy finished(controller.get(), &DocumentController::save_finished);
        const auto path = directory.path() / "async.mwb";
        controller->save_native_async(path);
        CHECK(controller->saving());
        // An edit while saving stays dirty after the save of the earlier state lands.
        const NewNote later{controller->active_track(), 480, 960, 62, 100, 0, 0};
        REQUIRE(controller->apply(*insert_notes(controller->document(), std::span(&later, 1))));
        controller->wait_for_save();
        CHECK_FALSE(controller->saving());
        REQUIRE(finished.count() == 1);
        CHECK(finished.at(0).at(0).toString().isEmpty());
        CHECK(controller->is_dirty());
        CHECK(controller->file_path() == path);
        CHECK(std::filesystem::exists(path));
        // A second wait (or the queued completion) does nothing more.
        controller->wait_for_save();
        QCoreApplication::processEvents();
        CHECK(finished.count() == 1);
    }

    TEST_CASE("recovery flushes run in the background and report their outcome") {
        test::TemporaryDirectory directory;
        auto controller = make_controller();
        auto journal = document::RecoveryJournal::create(directory.path(), controller->document(), {});
        REQUIRE(journal);
        std::shared_ptr<document::RecoveryJournal> shared = std::move(*journal);
        controller->attach_recovery(shared);
        const NewNote note{controller->active_track(), 0, 480, 60, 100, 0, 0};
        REQUIRE(controller->apply(*insert_notes(controller->document(), std::span(&note, 1))));
        CHECK(shared->pending_commands() == 1);
        QSignalSpy flushed(controller.get(), &DocumentController::recovery_flushed);
        controller->flush_recovery_async();
        controller->flush_recovery_async(); // at most one flush in flight
        REQUIRE(flushed.wait(10000));
        CHECK(flushed.count() == 1);
        CHECK(flushed.at(0).at(0).toString().isEmpty());
        CHECK(controller->recovery_error().isEmpty());
        CHECK(shared->pending_commands() == 0);
        controller->close_recovery();
    }

    TEST_CASE("selection drops notes deleted by an edit") {
        auto controller = make_controller();
        const NewNote note{controller->active_track(), 0, 480, 60, 100, 0, 0};
        REQUIRE(controller->apply(*insert_notes(controller->document(), std::span(&note, 1))));
        const auto id = controller->notes().notes.front().on_id;
        controller->set_selection({id});
        auto removal = delete_notes(controller->document(), controller->active_sequence().id, std::vector<EventId>{id});
        REQUIRE(removal);
        REQUIRE(controller->apply(*removal));
        CHECK(controller->selection().empty());
    }
}

TEST_SUITE("piano_roll") {
    TEST_CASE("Alt+wheel makes the piano rows taller or shorter around the row under the mouse") {
        auto controller = make_controller();
        PianoRollView view(*controller);
        view.resize(800, 500);
        view.show();
        const auto alt_wheel = [&](QPoint delta, QPointF at) {
            QWheelEvent event(at, view.mapToGlobal(at), QPoint(), delta, Qt::NoButton, Qt::AltModifier,
                              Qt::NoScrollPhase, false);
            QApplication::sendEvent(&view, &event);
        };
        const double start = view.mapping().row_height;
        CHECK(start >= 16.0); // slightly larger default rows
        const int anchor = 64;
        const QPointF at(PianoRollView::keyboard_width + 200,
                         PianoRollView::ruler_height + view.mapping().pitch_to_y(anchor) + start / 2);
        const auto pitch_under = [&] {
            return view.mapping().y_to_pitch(at.y() - PianoRollView::ruler_height);
        };
        REQUIRE(pitch_under() == anchor);
        alt_wheel(QPoint(0, 120), at);
        CHECK(view.mapping().row_height > start);
        CHECK(std::abs(pitch_under() - anchor) <= 1);
        // Windows reports Alt+wheel on the horizontal axis; it zooms the same way.
        const double taller = view.mapping().row_height;
        alt_wheel(QPoint(120, 0), at);
        CHECK(view.mapping().row_height > taller);
        alt_wheel(QPoint(0, -120), at);
        alt_wheel(QPoint(0, -120), at);
        alt_wheel(QPoint(0, -120), at);
        CHECK(view.mapping().row_height < start);
        CHECK(std::abs(pitch_under() - anchor) <= 1);
        CHECK(controller->notes().notes.empty()); // zooming never edits
    }

    TEST_CASE("double-click inserts one note and a body drag moves it as one undo entry") {
        auto controller = make_controller();
        PianoRollView view(*controller);
        view.resize(800, 500);
        view.show();
        const auto& mapping = view.mapping();
        const QPoint target(PianoRollView::keyboard_width + static_cast<int>(mapping.tick_to_x(960)) + 5,
                            PianoRollView::ruler_height + static_cast<int>(mapping.pitch_to_y(72)) + 4);
        QTest::mouseDClick(&view, Qt::LeftButton, Qt::NoModifier, target);
        REQUIRE(controller->notes().notes.size() == 1);
        const auto inserted = controller->notes().notes.front();
        CHECK(inserted.pitch == 72);
        CHECK(inserted.start == 960);
        CHECK(controller->selection().contains(inserted.on_id));
        // Drag the body two rows up and right by roughly a beat.
        // Grab the left part: the rightmost third of a short note is its end-resize zone.
        const QPoint body = target + QPoint(-2, 0);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, body);
        QTest::mouseMove(&view, body + QPoint(20, -static_cast<int>(mapping.row_height * 2)));
        QTest::mouseMove(&view, body + QPoint(static_cast<int>(960 / mapping.ticks_per_pixel), -static_cast<int>(mapping.row_height * 2)));
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier,
                            body + QPoint(static_cast<int>(960 / mapping.ticks_per_pixel), -static_cast<int>(mapping.row_height * 2)));
        REQUIRE(controller->notes().notes.size() == 1);
        const auto moved = controller->notes().notes.front();
        CHECK(moved.pitch == 74);
        CHECK(moved.start == 1920);
        CHECK(moved.on_id == inserted.on_id); // stable identity
        CHECK(controller->undo());
        CHECK(controller->notes().notes.front().start == 960);
    }

    TEST_CASE("Escape during a drag restores the exact state with no undo entry") {
        auto controller = make_controller();
        const NewNote note{controller->active_track(), 960, 1440, 64, 100, 0, 0};
        REQUIRE(controller->apply(*insert_notes(controller->document(), std::span(&note, 1))));
        PianoRollView view(*controller);
        view.resize(800, 500);
        view.show();
        const auto& mapping = view.mapping();
        const QPoint body(PianoRollView::keyboard_width + static_cast<int>(mapping.tick_to_x(1100)),
                          PianoRollView::ruler_height + static_cast<int>(mapping.pitch_to_y(64)) + 4);
        const auto hash = semantic_hash(controller->document());
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, body);
        QTest::mouseMove(&view, body + QPoint(100, 0));
        QTest::keyClick(&view, Qt::Key_Escape);
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, body + QPoint(100, 0));
        CHECK(semantic_hash(controller->document()) == hash);
        CHECK(controller->undo_text() == QString("Insert notes"));
    }
}

TEST_SUITE("piano_roll") {
    TEST_CASE("Select Same Channel stays linear on a large selection") {
        auto controller = make_controller();
        std::vector<NewNote> notes;
        for (int index = 0; index < 20000; ++index)
            notes.push_back(NewNote{controller->active_track(), index * 10, index * 10 + 5,
                                    static_cast<std::uint8_t>(40 + index % 40), 100, 0, 0});
        REQUIRE(controller->apply(*insert_notes(controller->document(), notes)));
        PianoRollView view(*controller);
        view.select_all();
        const auto started = std::chrono::steady_clock::now();
        view.select_same_channel();
        view.select_same_pitch();
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
        CHECK(controller->selection().size() == 20000);
    }

    TEST_CASE("a Ctrl-drag that returns to its start creates no duplicates") {
        auto controller = make_controller();
        const NewNote note{controller->active_track(), 960, 1920, 64, 100, 0, 0};
        REQUIRE(controller->apply(*insert_notes(controller->document(), std::span(&note, 1))));
        PianoRollView view(*controller);
        view.resize(800, 500);
        view.show();
        const auto& mapping = view.mapping();
        const QPoint body(PianoRollView::keyboard_width + static_cast<int>(mapping.tick_to_x(1400)),
                          PianoRollView::ruler_height + static_cast<int>(mapping.pitch_to_y(64)) + 4);
        QTest::mousePress(&view, Qt::LeftButton, Qt::ControlModifier, body);
        QTest::mouseMove(&view, body + QPoint(30, 0));
        QTest::mouseMove(&view, body);
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::ControlModifier, body);
        CHECK(controller->notes().notes.size() == 1);
    }

    TEST_CASE("a draw click keeps the default length and selects the new note") {
        auto controller = make_controller();
        PianoRollView view(*controller);
        view.resize(800, 500);
        view.show();
        view.set_tool(EditTool::draw);
        const auto& mapping = view.mapping();
        const QPoint target(PianoRollView::keyboard_width + static_cast<int>(mapping.tick_to_x(1000)),
                            PianoRollView::ruler_height + static_cast<int>(mapping.pitch_to_y(70)) + 4);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, target);
        QTest::mouseMove(&view, target + QPoint(1, 0)); // jitter under the drag distance
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, target + QPoint(1, 0));
        REQUIRE(controller->notes().notes.size() == 1);
        const auto note = controller->notes().notes.front();
        CHECK(note.start == 960);
        CHECK(note.end - note.start == 240); // default 1/16 at PPQ 960
        CHECK(controller->selection().contains(note.on_id));
    }

    TEST_CASE("Loop Selection sets the loop to the grid-aligned extent") {
        auto controller = make_controller();
        const NewNote note{controller->active_track(), 1000, 1500, 60, 100, 0, 0};
        REQUIRE(controller->apply(*insert_notes(controller->document(), std::span(&note, 1))));
        PianoRollView view(*controller);
        view.select_all();
        view.loop_selection();
        const auto& sequence = controller->active_sequence();
        REQUIRE(sequence.settings.loop_region);
        for (const auto& region : sequence.regions)
            if (region.id == *sequence.settings.loop_region) {
                CHECK(region.start_tick == 960);
                CHECK(region.end_tick == 1680);
            }
        CHECK(controller->undo_text() == QString("Set loop"));
    }
}

TEST_SUITE("controller") {
    TEST_CASE("a pitch-bend curve on an empty lane ends with the reset value") {
        CHECK(controller_default_value(LaneTarget{LaneTarget::Kind::pitch_bend, 0, 0}) == 8192);
        CHECK(controller_default_value(LaneTarget{LaneTarget::Kind::controller, 7, 0}) == 100);
        CHECK(controller_default_value(LaneTarget{LaneTarget::Kind::controller, 11, 0}) == 127);
        CHECK(controller_default_value(LaneTarget{LaneTarget::Kind::controller, 1, 0}) == 0);
    }
}

TEST_SUITE("keyboard") {
    TEST_CASE("computer keyboard ignores auto-repeat and releases the captured pitch") {
        application();
        ComputerKeyboardPlayer player;
        player.set_enabled(true);
        QSignalSpy on(&player, &ComputerKeyboardPlayer::note_on);
        QSignalSpy off(&player, &ComputerKeyboardPlayer::note_off);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a");
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a", true);
        QKeyEvent octave(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, "x");
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier, "a");
        CHECK(player.handle_key(&press, true));
        CHECK(player.handle_key(&repeat, true));
        CHECK(player.handle_key(&octave, true)); // octave changes while held
        CHECK(player.handle_key(&release, false));
        REQUIRE(on.count() == 1);
        REQUIRE(off.count() == 1);
        CHECK(on.at(0).at(0).toInt() == 60);
        CHECK(off.at(0).at(0).toInt() == 60); // released with the note captured at key-down
    }

    TEST_CASE("deactivation releases only playing-mode notes") {
        application();
        ComputerKeyboardPlayer player;
        player.set_enabled(true);
        QSignalSpy off(&player, &ComputerKeyboardPlayer::note_off);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier, "d");
        REQUIRE(player.handle_key(&press, true));
        player.release_all();
        CHECK(off.count() == 1);
        CHECK(off.at(0).at(0).toInt() == 64);
    }
}

TEST_SUITE("clipboard") {
    TEST_CASE("drag files from long multi-byte titles get valid names") {
        test::TemporaryDirectory directory;
        std::string title;
        for (int index = 0; index < 40; ++index)
            title += "æ¼¢"; // 120 bytes of 3-byte characters: the 80-byte cut lands mid-character
        const std::vector<std::uint8_t> bytes{'M', 'T', 'h', 'd'};
        auto written = write_drag_file(directory.path(), "x" + title, bytes);
        REQUIRE_MESSAGE(written, (written ? "" : written.error().describe()));
        CHECK(std::filesystem::exists(*written));
        const auto stem = written->filename().u8string();
        CHECK(stem.size() <= 84); // 80 bytes of name plus ".mid"
    }


    TEST_CASE("selection payload round-trips and is validated like a file") {
        ClipboardPayload payload;
        payload.ppq = 480;
        payload.selection_length = 960;
        payload.tracks = {"Lead"};
        payload.notes = {{0, 240, 60, 100, 0, 0, 0}, {480, 960, 64, 90, 10, 1, 0}};
        auto parsed = clipboard_from_json(clipboard_to_json(payload));
        REQUIRE(parsed);
        CHECK(parsed->notes.size() == 2);
        CHECK(parsed->notes[1].release_velocity == 10);
        CHECK_FALSE(clipboard_from_json("{\"format\":\"mwb.selection\",\"schema_version\":1,\"ppq\":0}"));
        CHECK_FALSE(clipboard_from_json("not json"));
        // Foreign data under our MIME type: missing keys and wrong types are errors, not exceptions.
        CHECK_FALSE(clipboard_from_json("{\"format\":\"mwb.selection\",\"schema_version\":1,\"ppq\":480}"));
        CHECK_FALSE(clipboard_from_json("{\"format\":\"mwb.selection\",\"schema_version\":\"one\"}"));
        CHECK_FALSE(clipboard_from_json("{\"format\":\"mwb.selection\",\"schema_version\":1,\"ppq\":\"x\"}"));
        CHECK_FALSE(clipboard_from_json(
            "{\"format\":\"mwb.selection\",\"schema_version\":1,\"ppq\":480,\"tracks\":[],\"notes\":[],"
            "\"scale\":{\"root\":\"C\"}}"));
        auto bad = payload;
        bad.notes.push_back({0, 10, 200, 100, 0, 0, 0});
        CHECK_FALSE(clipboard_from_json(clipboard_to_json(bad)));
    }

    TEST_CASE("paste converts resolution per event from the exact source position") {
        auto controller = make_controller();
        ClipboardPayload payload;
        payload.ppq = 480;
        payload.tracks = {"Lead"};
        payload.notes = {{0, 240, 60, 100, 0, 0, 0}, {481, 721, 62, 100, 0, 0, 0}};
        QMimeData mime;
        mime.setData(clipboard_mime_type, QByteArray::fromStdString(clipboard_to_json(payload)));
        QString message;
        const auto created = paste_clipboard(*controller, &mime, 3840, &message);
        REQUIRE(created.size() == 2);
        const auto& notes = controller->notes().notes;
        CHECK(notes[0].start == 3840);
        CHECK(notes[0].end == 3840 + 480);
        CHECK(notes[1].start == 3840 + 962);
        CHECK(message.contains("PPQ 480"));
    }
}

namespace {

/// PlaybackService double for RecordingController: the test decides which transport tick the
/// "engine clock" reports for the next captured message.
class FakePlayback final : public PlaybackService {
public:
    void publish_snapshot(std::shared_ptr<const DocumentData>, std::size_t, std::uint64_t) override {}
    void play(Tick from) override {
        state = TransportDisplay::State::playing;
        position = from;
    }
    void pause() override { state = TransportDisplay::State::paused; }
    void stop(Tick return_to) override {
        ++stops;
        state = TransportDisplay::State::stopped;
        position = return_to;
        emit transport_state_changed();
    }
    void seek(Tick tick) override { position = tick; }
    std::optional<Tick> record(Tick from, int count_in_bars) override {
        count_in = count_in_bars;
        if (state == TransportDisplay::State::playing) {
            state = TransportDisplay::State::recording;
            emit transport_state_changed();
            return position;
        }
        state = count_in_bars > 0 ? TransportDisplay::State::counting_in : TransportDisplay::State::recording;
        position = from;
        emit transport_state_changed();
        return from;
    }
    void punch_out() override {
        ++punch_outs;
        state = TransportDisplay::State::playing;
        emit transport_state_changed();
    }
    std::optional<Tick> transport_tick_at(std::int64_t) const override { return clock; }
    void set_loop(std::optional<TimeRange> range) override { loop_range = range; }
    std::optional<TimeRange> loop() const override { return loop_range; }
    Rational tempo_multiplier() const override { return Rational{1, 1}; }
    void set_metronome(bool) override { ++metronome_calls; }
    void set_tempo_multiplier(double) override {}
    void audition_note_on(int, int, int, const std::string&) override {}
    void audition_note_off(int, int) override {}
    void set_live_timbre(const std::string&) override {}
    void monitor_message(std::span<const std::uint8_t> message) override {
        std::lock_guard lock(monitor_mutex); // called on the input monitor thread
        monitored.emplace_back(message.begin(), message.end());
    }
    std::vector<std::vector<std::uint8_t>> monitored_copy() {
        std::lock_guard lock(monitor_mutex);
        return monitored;
    }
    void panic(bool) override {}
    TransportMeters take_meters() override { return {}; }
    void reopen_device(std::optional<std::string>) override {}
    std::vector<AudioDeviceChoice> audio_devices() const override { return {}; }
    void simulate_device_loss() override {}
    TransportDisplay display() const override {
        TransportDisplay display;
        display.state = state;
        display.position = position;
        return display;
    }
    bool has_device() const override { return true; }

    TransportDisplay::State state = TransportDisplay::State::stopped;
    Tick position = 0;
    std::optional<Tick> clock;
    std::optional<TimeRange> loop_range;
    int count_in = -1;
    int stops = 0;
    int punch_outs = 0;
    int metronome_calls = 0;
    std::mutex monitor_mutex;
    std::vector<std::vector<std::uint8_t>> monitored;
};

/// Sends one live note message whose transport position is `tick`.
void live(RecordingController& recording, FakePlayback& playback, Tick tick, int pitch, bool on) {
    playback.clock = tick;
    recording.live_note(pitch, 90, on);
}

std::int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <typename Predicate>
bool wait_until(Predicate predicate, int milliseconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        QTest::qWait(10);
    }
    return true;
}

} // namespace

TEST_SUITE("recording") {
    TEST_CASE("takes are placed by the transport clock; count-in input before the start is dropped") {
        auto controller = make_controller();
        FakePlayback playback;
        RecordingController recording(playback);
        recording.set_target(controller.get(), nullptr);
        recording.set_count_in_bars(1);
        controller->set_edit_cursor(0);
        recording.toggle_recording();
        REQUIRE(recording.recording());
        CHECK(playback.count_in == 1);        // the engine plays the count-in, also from bar 1
        CHECK(playback.metronome_calls == 0); // recording leaves the Metronome toggle alone
        live(recording, playback, -1500, 50, true); // played and released during the count-in
        live(recording, playback, -1000, 50, false);
        live(recording, playback, -200, 52, true); // held into the take: starts at the boundary
        live(recording, playback, 300, 52, false);
        // At a practice tempo the engine clock already maps time through the multiplier; the take
        // uses those ticks unchanged (no wall-clock arithmetic in the controller).
        live(recording, playback, 480, 60, true);
        live(recording, playback, 960, 60, false);
        playback.clock = 1920;
        recording.toggle_recording();
        CHECK_FALSE(recording.recording());
        CHECK(playback.stops == 1);
        const auto& notes = controller->notes().notes;
        REQUIRE(notes.size() == 2);
        CHECK(notes[0].pitch == 52);
        CHECK(notes[0].start == 0);
        CHECK(notes[0].end == 300);
        CHECK(notes[1].start == 480);
        CHECK(notes[1].end == 960);
    }

    TEST_CASE("a take with no input is not inserted: no undo step, no unsaved change (final review N3)") {
        auto controller = make_controller();
        REQUIRE_FALSE(controller->is_dirty());
        const bool could_undo = controller->can_undo();
        const auto takes = controller->document().takes.size();
        FakePlayback playback;
        RecordingController recording(playback);
        recording.set_target(controller.get(), nullptr);
        recording.set_count_in_bars(0);
        QSignalSpy messages(&recording, &RecordingController::status_message);
        recording.toggle_recording();
        REQUIRE(recording.recording());
        playback.clock = 1920; // a bar passes with nothing played
        recording.toggle_recording();
        CHECK_FALSE(recording.recording());
        CHECK(playback.stops == 1); // the transport still stops
        CHECK(controller->document().takes.size() == takes);
        CHECK(controller->can_undo() == could_undo);
        CHECK_FALSE(controller->is_dirty());
        REQUIRE_FALSE(messages.isEmpty());
        CHECK(messages.last().at(0).toString().contains("no take"));

        // The next take that has input is still kept, and is "Take 1".
        recording.toggle_recording();
        live(recording, playback, 2000, 60, true);
        live(recording, playback, 2400, 60, false);
        playback.clock = 3840;
        recording.toggle_recording();
        REQUIRE(controller->document().takes.size() == takes + 1);
        CHECK(controller->document().takes.back().name == "Take 1");
        CHECK(controller->is_dirty());
    }

    TEST_CASE("loop takes fold the unfolded passes and keep the last complete one") {
        auto controller = make_controller();
        FakePlayback playback;
        playback.loop_range = TimeRange{0, 1920};
        RecordingController recording(playback);
        recording.set_target(controller.get(), nullptr);
        recording.set_count_in_bars(0);
        recording.toggle_recording();
        REQUIRE(recording.recording());
        live(recording, playback, 100, 60, true); // pass 1
        live(recording, playback, 200, 60, false);
        live(recording, playback, 1920 + 480, 64, true); // pass 2
        live(recording, playback, 1920 + 960, 64, false);
        playback.clock = 2 * 1920 + 100; // stopped early in pass 3
        recording.toggle_recording();
        const auto& notes = controller->notes().notes;
        REQUIRE(notes.size() == 1);
        CHECK(notes[0].pitch == 64);
        CHECK(notes[0].start == 480);
        CHECK(notes[0].end == 960);
    }

    TEST_CASE("record while playing punches in at the playhead and punches out on the second press") {
        auto controller = make_controller();
        FakePlayback playback;
        playback.play(3840);
        RecordingController recording(playback);
        recording.set_target(controller.get(), nullptr);
        controller->set_edit_cursor(0); // the edit cursor is not where a punch-in starts
        recording.toggle_recording();
        REQUIRE(recording.recording());
        CHECK(playback.count_in == 0);
        live(recording, playback, 3000, 60, true); // before the punch-in: dropped
        live(recording, playback, 3500, 60, false);
        live(recording, playback, 4000, 62, true);
        live(recording, playback, 4400, 62, false);
        playback.clock = 4800;
        recording.toggle_recording();
        CHECK(playback.punch_outs == 1);
        CHECK(playback.stops == 0); // playback goes on
        const auto& notes = controller->notes().notes;
        REQUIRE(notes.size() == 1);
        CHECK(notes[0].start == 4000);
    }

    TEST_CASE("a take ends with the transport when it leaves Recording on its own") {
        auto controller = make_controller();
        FakePlayback playback;
        RecordingController recording(playback);
        recording.set_target(controller.get(), nullptr);
        recording.set_count_in_bars(0);
        recording.toggle_recording();
        live(recording, playback, 0, 60, true);
        playback.clock = 480;
        playback.stop(0); // e.g. the end of the song or a lost device
        CHECK_FALSE(recording.recording());
        REQUIRE(controller->notes().notes.size() == 1);
        CHECK(controller->notes().notes[0].end == 480);
    }

    TEST_CASE("the engine counts in from tick 0, enters Recording and maps time through the speed") {
        application();
        auto playback = create_playback_service();
        REQUIRE(playback);
        REQUIRE(wait_until([&] { return playback->has_device(); }, 5000)); // opens in the background (null backend in CTest)
        auto document = make_blank_document("Count-in");
        playback->set_tempo_multiplier(4.0); // 1 bar of 4/4 at 120 bpm lasts 0.5 s
        playback->publish_document(document, 0, 1);
        const auto started = playback->record(0, 1);
        REQUIRE(started);
        CHECK(*started == 0);
        CHECK(playback->display().state == TransportDisplay::State::counting_in);
        REQUIRE(wait_until([&] { return playback->transport_tick_at(steady_ns()).has_value(); }, 3000));
        if (playback->display().state == TransportDisplay::State::counting_in) {
            const auto during = playback->transport_tick_at(steady_ns());
            REQUIRE(during);
            CHECK(*during < 0); // the count-in lies before the take start
        }
        REQUIRE(wait_until([&] { return playback->display().state == TransportDisplay::State::recording; }, 3000));
        // Recording follows the rendered block; what is heard lags by the output latency, so wait
        // until the heard position has passed the take start (review_2 T2).
        REQUIRE(wait_until([&] {
            const auto heard = playback->transport_tick_at(steady_ns());
            return heard && *heard >= 0;
        }, 2000));
        const std::int64_t now = steady_ns();
        const auto first = playback->transport_tick_at(now);
        const auto later = playback->transport_tick_at(now + 250'000'000); // 250 ms later
        REQUIRE(first);
        REQUIRE(later);
        // 250 ms at 120 bpm and 4x speed is two beats: 1920 ticks at PPQ 960. The two reads may
        // straddle a callback; the smoothed engine clock keeps that within a few ticks.
        CHECK(std::abs((*later - *first) - 1920) <= 20);
        playback->stop(0);
        CHECK(playback->display().state == TransportDisplay::State::stopped);
        CHECK_FALSE(playback->transport_tick_at(steady_ns()));
    }
}

TEST_SUITE("tempo") {
    TEST_CASE("the BPM edit replaces the bar-1 tempo as one undo entry, and undo restores it") {
        auto controller = make_controller();
        const auto before = controller->tempo_map().tempo_at(0);
        CHECK(before == 500000); // 120 BPM
        CHECK(controller->set_tempo_at(1920, 90.0));
        CHECK(controller->tempo_map().tempo_at(0) == 666667);
        CHECK(controller->tempo_map().tempo_at(100000) == 666667); // no later change: the whole song
        CHECK_FALSE(controller->set_tempo_at(0, 90.0));          // unchanged: no undo entry
        REQUIRE(controller->undo());
        CHECK(controller->tempo_map().tempo_at(0) == before);
        CHECK_FALSE(controller->can_undo());
    }

    TEST_CASE("with tempo changes the BPM edit changes the tempo in effect at the play position") {
        auto controller = make_controller();
        ChangeBuilder builder(controller->document(), "add tempo change");
        std::ignore = builder.insert(controller->active_track(), events::tempo(3840, 600000)); // 100 BPM at bar 2
        REQUIRE(controller->apply(builder.finish()));
        CHECK(controller->set_tempo_at(5000, 140.0)); // inside bar 2: edits the change at 3840
        CHECK(controller->tempo_map().tempo_at(0) == 500000);
        CHECK(controller->tempo_map().tempo_at(3840) == 428571);
        CHECK(controller->set_tempo_at(100, 80.0)); // before the change: edits bar 1 only
        CHECK(controller->tempo_map().tempo_at(0) == 750000);
        CHECK(controller->tempo_map().tempo_at(3840) == 428571);
    }

    TEST_CASE("the BPM field commits a drag, a wheel or arrow burst and typing once each") {
        application();
        TempoField field;
        field.show();
        std::vector<double> committed;
        QObject::connect(&field, &TempoField::tempo_committed, [&](double bpm) { committed.push_back(bpm); });
        field.show_tempo(120.0);
        CHECK(field.value() == doctest::Approx(120.0));
        CHECK(committed.empty()); // showing the song tempo is not an edit

        // Drag up 20 px: 5 BPM, one commit on release.
        QWidget* text = field.findChild<QLineEdit*>();
        REQUIRE(text);
        const QPoint start(text->width() / 2, text->height() / 2);
        QTest::mousePress(text, Qt::LeftButton, Qt::NoModifier, start);
        for (int step = 1; step <= 5; ++step) {
            QMouseEvent move(QEvent::MouseMove, QPointF(start.x(), start.y() - 4 * step), text->mapToGlobal(QPointF(start)),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(text, &move);
        }
        CHECK(field.value() == doctest::Approx(125.0));
        CHECK(committed.empty()); // nothing is written while dragging
        field.show_tempo(60.0);   // a display update never overwrites a drag in progress
        CHECK(field.value() == doctest::Approx(125.0));
        QTest::mouseRelease(text, Qt::LeftButton, Qt::NoModifier, QPoint(start.x(), start.y() - 20));
        REQUIRE(committed.size() == 1);
        CHECK(committed[0] == doctest::Approx(125.0));

        // Shift drag: 0.1 BPM per 4 px.
        QTest::mousePress(text, Qt::LeftButton, Qt::NoModifier, start);
        QMouseEvent fine(QEvent::MouseMove, QPointF(start.x(), start.y() + 8), text->mapToGlobal(QPointF(start)), Qt::NoButton,
                         Qt::LeftButton, Qt::ShiftModifier);
        QApplication::sendEvent(text, &fine);
        QTest::mouseRelease(text, Qt::LeftButton, Qt::NoModifier, QPoint(start.x(), start.y() + 8));
        REQUIRE(committed.size() == 2);
        CHECK(committed[1] == doctest::Approx(124.8));

        // Three arrow steps in a row are one edit, committed after the idle time.
        field.setFocus();
        QTest::keyClick(&field, Qt::Key_Up);
        QTest::keyClick(&field, Qt::Key_Up);
        QTest::keyClick(&field, Qt::Key_Up);
        CHECK(committed.size() == 2);
        QTest::qWait(TempoField::idle_commit_ms + 200);
        REQUIRE(committed.size() == 3);
        CHECK(committed[2] == doctest::Approx(127.8));

        // Typing commits on Enter.
        text->setFocus();
        QTest::keyClick(text, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(text, "90");
        QTest::keyClick(text, Qt::Key_Return);
        REQUIRE(committed.size() == 4);
        CHECK(committed[3] == doctest::Approx(90.0));
    }
}

namespace {

/// A real MainWindow in a fresh workspace (offscreen, null audio and MIDI backends).
struct WindowFixture {
    test::TemporaryDirectory directory;
    std::unique_ptr<MainWindow> window;

    WindowFixture() {
        application();
        // Settings (layout, expanded sections, shortcuts) stay in the temporary workspace.
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           QString::fromStdWString(directory.path().wstring()));
        StartupOptions options;
        options.workspace = directory.path();
        options.skip_recovery_prompt = true;
        const auto directories = app::resolve_data_directories(options.workspace);
        REQUIRE(app::ensure_data_directories(directories));
        window = std::make_unique<MainWindow>(options, directories);
        window->resize(1400, 860);
        window->show();
        window->activateWindow();
        std::ignore = QTest::qWaitForWindowActive(window.get(), 2000); // focus events need an active window
        QTest::qWait(300);
        REQUIRE(window->current_tab());
        for (auto* header : window->findChildren<QToolButton*>("sectionHeader"))
            if (header->text() != "Track")
                header->setChecked(true); // every Inspector section visible (Track is toggled below)
        QTest::qWait(50);
    }

    /// Sends a key through the window, so shortcuts and the focus routing apply as for a user.
    void key(Qt::Key code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::keyClick(window->windowHandle(), code, modifiers);
        QTest::qWait(20);
    }

    [[nodiscard]] TransportDisplay::State transport() const {
        // The readout names the state; "Stop" is the stopped state.
        auto* label = window->findChild<QLabel*>("transport.position");
        return label && label->text().contains("Stop") ? TransportDisplay::State::stopped : TransportDisplay::State::playing;
    }

    template <typename Widget>
    Widget* find(const std::function<bool(Widget*)>& match) {
        for (auto* widget : window->findChildren<Widget*>())
            if (match(widget))
                return widget;
        return nullptr;
    }
};

} // namespace

TEST_SUITE("window") {
    TEST_CASE("Return and keyboard-focused Space press buttons, check boxes, headers and Mute; a click leaves Space to Play") {
        WindowFixture fixture;
        auto* advanced = fixture.find<QCheckBox>([](QCheckBox* box) { return box->text().startsWith("Show all parameters"); });
        auto* preview = fixture.find<QPushButton>([](QPushButton* button) { return button->text() == "Preview"; });
        auto* track_header = fixture.find<QToolButton>(
            [](QToolButton* button) { return button->objectName() == "sectionHeader" && button->text() == "Track"; });
        REQUIRE(advanced);
        REQUIRE(preview);
        REQUIRE(track_header);
        CHECK(preview->focusPolicy() == Qt::TabFocus); // a click does not steal the focus

        // Tab to the check box: Space toggles it and does not start playback.
        const bool advanced_before = advanced->isChecked();
        advanced->setFocus(Qt::TabFocusReason);
        fixture.key(Qt::Key_Space);
        CHECK(advanced->isChecked() != advanced_before);
        CHECK(fixture.transport() == TransportDisplay::State::stopped);
        // Return toggles it back.
        fixture.key(Qt::Key_Return);
        CHECK(advanced->isChecked() == advanced_before);

        // Return presses a push button (Qt does not, outside a dialog).
        QSignalSpy clicked(preview, &QPushButton::clicked);
        preview->setFocus(Qt::TabFocusReason);
        fixture.key(Qt::Key_Return);
        QTest::qWait(250); // animateClick
        CHECK(clicked.count() == 1);

        // Section headers expand with Return.
        const bool expanded = track_header->isChecked();
        track_header->setFocus(Qt::TabFocusReason);
        fixture.key(Qt::Key_Return);
        CHECK(track_header->isChecked() != expanded);

        // After a mouse click on a control, Space is Play/Stop again.
        advanced->setFocus(Qt::MouseFocusReason);
        const bool advanced_now = advanced->isChecked();
        fixture.key(Qt::Key_Space);
        QTest::qWait(200);
        CHECK(advanced->isChecked() == advanced_now);
        CHECK(fixture.transport() != TransportDisplay::State::stopped);
        fixture.key(Qt::Key_Space); // stop
        QTest::qWait(100);
        CHECK(fixture.transport() == TransportDisplay::State::stopped);
    }

    TEST_CASE("track Mute toggles from the keyboard; tool keys in the track list never rename the track") {
        WindowFixture fixture;
        auto* tab = fixture.window->current_tab();
        auto* table = tab->track_list().findChild<QTableWidget*>();
        REQUIRE(table);
        auto& controller = tab->controller();
        const std::string name = controller.active_sequence().tracks.front().name;

        table->setCurrentCell(0, 1); // the M cell
        table->setFocus(Qt::TabFocusReason);
        fixture.key(Qt::Key_Space);
        CHECK(controller.active_sequence().tracks.front().muted);
        fixture.key(Qt::Key_Return);
        CHECK_FALSE(controller.active_sequence().tracks.front().muted);

        table->setCurrentCell(0, 0);
        table->setFocus(Qt::MouseFocusReason);
        fixture.key(Qt::Key_2); // Draw tool, not a rename seeded with "2"
        CHECK(tab->piano_roll().tool() == EditTool::draw);
        CHECK(QApplication::focusWidget() == table); // no editor opened
        tab->piano_roll().setFocus();
        QTest::qWait(20);
        CHECK(controller.active_sequence().tracks.front().name == name);
    }

    TEST_CASE("letters reach the commands while a closed combo box has focus") {
        WindowFixture fixture;
        auto* root = fixture.find<QComboBox>([](QComboBox* combo) { return combo->count() == 12 && combo->itemText(0) == "C"; });
        REQUIRE(root);
        const int before = root->currentIndex();
        const std::uint64_t generation = fixture.window->current_tab()->controller().generation();
        root->setFocus(Qt::MouseFocusReason);
        fixture.key(Qt::Key_F); // Fit, not "jump to F"
        CHECK(root->currentIndex() == before);
        CHECK(fixture.window->current_tab()->controller().generation() == generation); // no edit
    }
}

TEST_SUITE("event_list") {
    TEST_CASE("the event list shows the current selection when it is opened") {
        auto controller = make_controller();
        std::vector<NewNote> notes;
        for (int index = 0; index < 8; ++index)
            notes.push_back(NewNote{controller->active_track(), index * 480, index * 480 + 240,
                                    static_cast<std::uint8_t>(60 + index), 100, 0, 0});
        auto changes = insert_notes(controller->document(), notes);
        REQUIRE(changes);
        REQUIRE(controller->apply(*changes));
        const auto& all = controller->notes().notes;
        REQUIRE(all.size() == 8);
        controller->set_selection({all[2].on_id, all[5].on_id}); // selected before the list exists

        EventListPanel panel(*controller);
        auto* table = panel.findChild<QTableView*>();
        REQUIRE(table);
        panel.resize(800, 400);
        panel.show();
        CHECK(table->selectionModel()->selectedRows().size() == 2);

        // Hidden lists ignore selection changes; showing it again catches up.
        panel.hide();
        controller->set_selection({all[7].on_id});
        CHECK(table->selectionModel()->selectedRows().size() == 2); // stale while hidden
        panel.show();
        const auto rows = table->selectionModel()->selectedRows();
        REQUIRE(rows.size() == 1);
        CHECK(table->model()->index(rows[0].row(), 0).data().isValid());
        CHECK_FALSE(controller->selection().empty()); // showing the list did not change the selection
        CHECK(controller->selection().contains(all[7].on_id));
    }

    TEST_CASE("opening the Event List dock in the window highlights the notes already selected") {
        WindowFixture fixture;
        auto& controller = fixture.window->current_tab()->controller();
        std::vector<NewNote> notes;
        for (int index = 0; index < 4; ++index)
            notes.push_back(NewNote{controller.active_track(), index * 480, index * 480 + 240,
                                    static_cast<std::uint8_t>(60 + index), 100, 0, 0});
        auto changes = insert_notes(controller.document(), notes);
        REQUIRE(changes);
        REQUIRE(controller.apply(*changes));
        std::set<EventId> all;
        for (const auto& note : controller.notes().notes)
            all.insert(note.on_id);
        controller.set_selection(all); // the final review's case: 4 notes selected, then the list opened
        auto* dock = fixture.window->findChild<QDockWidget*>("dock.events");
        REQUIRE(dock);
        REQUIRE_FALSE(dock->isVisible());
        dock->show(); // the panel is created when the dock becomes visible
        QTest::qWait(100);
        auto* panel = qobject_cast<EventListPanel*>(dock->widget());
        REQUIRE(panel);
        auto* table = panel->findChild<QTableView*>();
        REQUIRE(table);
        CHECK(table->selectionModel()->selectedRows().size() == 4);
    }

    TEST_CASE("the filter matches cached type, data and track text after typing pauses") {
        auto controller = make_controller();
        std::vector<NewNote> notes;
        for (int index = 0; index < 3000; ++index)
            notes.push_back(NewNote{controller->active_track(), index * 120, index * 120 + 60,
                                    static_cast<std::uint8_t>(index % 2 == 0 ? 60 : 62), 100, 0, 0});
        auto changes = insert_notes(controller->document(), notes);
        REQUIRE(changes);
        REQUIRE(controller->apply(*changes));
        EventListPanel panel(*controller);
        panel.resize(800, 400);
        panel.show();
        QTest::qWait(200); // the model rebuilds after the edit burst settles
        auto* filter = panel.findChild<QLineEdit*>();
        auto* table = panel.findChild<QTableView*>();
        REQUIRE(filter);
        REQUIRE(table);
        const int all = table->model()->rowCount();
        CHECK(all >= 6000);

        const auto started = std::chrono::steady_clock::now();
        QTest::keyClicks(filter, "d4  vel"); // pitch 62 is D4 ("D4  vel 100")
        CHECK(table->model()->rowCount() == all); // nothing filtered while typing
        QTest::qWait(250);
        CHECK(table->model()->rowCount() == 3000); // 1500 note-ons and 1500 note-offs of D4
        filter->clear();
        QTest::keyClicks(filter, "NOTE OFF");
        QTest::qWait(250);
        CHECK(table->model()->rowCount() == 3000); // case-insensitive type match
        filter->clear();
        QTest::keyClicks(filter, "1 track"); // the track column ("1 <name>")
        QTest::qWait(250);
        CHECK(table->model()->rowCount() > 0);
        filter->clear();
        QTest::keyClicks(filter, "no such event");
        QTest::qWait(250);
        CHECK(table->model()->rowCount() == 0);
        filter->clear();
        QTest::qWait(250);
        CHECK(table->model()->rowCount() == all);
        // Generous bound for a debug build; re-formatting all 9 columns per keystroke took far longer.
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
    }
}

namespace {

/// Waits until the loopback input received a message matching `match` (event loop running).
template <typename Match>
bool wait_for_message(platform::IMidiInput& input, Match match, int milliseconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    std::array<platform::MidiInputMessage, 64> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        const std::size_t count = input.drain(buffer);
        for (std::size_t index = 0; index < count; ++index)
            if (match(buffer[index]))
                return true;
        QTest::qWait(10);
    }
    return false;
}

bool is_note_on(const platform::MidiInputMessage& message, int pitch) {
    return message.size == 3 && (message.bytes[0] & 0xF0) == 0x90 && message.bytes[1] == pitch && message.bytes[2] > 0;
}

bool is_note_off(const platform::MidiInputMessage& message, int pitch) {
    return message.size == 3 && message.bytes[1] == pitch &&
           ((message.bytes[0] & 0xF0) == 0x80 || ((message.bytes[0] & 0xF0) == 0x90 && message.bytes[2] == 0));
}

DocumentData external_document() {
    DocumentData document = make_blank_document("External");
    auto& track = document.sequences.front().tracks.front();
    track.routing.output = OutputKind::external_midi;
    track.routing.endpoint_id = "null:out:loopback";
    ChangeBuilder builder(document, "long note");
    builder.add_note(track.id, 0, 960 * 400, 0, 60, 100);
    REQUIRE(apply_change_set(document, builder.finish()));
    return document;
}

} // namespace

TEST_SUITE("external_midi") {
    TEST_CASE("a note sounding on an external route is released when the route disappears (mute)") {
        application();
        auto backend = platform::create_null_midi_backend();
        auto input = backend->open_input("null:in:loopback");
        REQUIRE(input);
        auto playback = create_playback_service();
        REQUIRE(wait_until([&] { return playback->has_device(); }, 5000)); // opens in the background (null backend in CTest)
        DocumentData document = external_document();
        playback->publish_document(document, 0, 1);
        playback->play(0);
        REQUIRE(wait_for_message(**input, [](const auto& message) { return is_note_on(message, 60); }, 3000));
        document.sequences.front().tracks.front().muted = true; // the only track on that route
        playback->publish_document(document, 0, 2);
        CHECK(wait_for_message(**input, [](const auto& message) { return is_note_off(message, 60); }, 3000));
        playback->stop(0);
    }

    TEST_CASE("quitting while an external note sounds still sends its note-off") {
        application();
        auto backend = platform::create_null_midi_backend();
        auto input = backend->open_input("null:in:loopback");
        REQUIRE(input);
        auto playback = create_playback_service();
        REQUIRE(wait_until([&] { return playback->has_device(); }, 5000)); // opens in the background (null backend in CTest)
        const DocumentData document = external_document();
        playback->publish_document(document, 0, 1);
        playback->play(0);
        REQUIRE(wait_for_message(**input, [](const auto& message) { return is_note_on(message, 60); }, 3000));
        playback.reset(); // the application quits mid-note
        CHECK(wait_for_message(**input, [](const auto& message) { return is_note_off(message, 60); }, 2000));
    }
}

TEST_SUITE("audio_device") {
    TEST_CASE("the device opens in the background; a lost output is reopened automatically and plays again") {
        application();
        auto playback = create_playback_service();
        CHECK(playback->display().opening); // the constructor never waits for the device
        REQUIRE(wait_until([&] { return playback->has_device(); }, 5000));
        QSignalSpy lost(playback.get(), &PlaybackService::device_lost);
        DocumentData document = make_blank_document("Device");
        ChangeBuilder builder(document, "note");
        builder.add_note(document.sequences.front().tracks.front().id, 0, 960 * 64, 0, 60, 100);
        REQUIRE(apply_change_set(document, builder.finish()));
        playback->publish_document(document, 0, 1);
        playback->play(0);
        REQUIRE(wait_until([&] { return playback->transport_tick_at(steady_ns()).has_value(); }, 3000));

        playback->simulate_device_loss();
        REQUIRE(wait_until([&] { return playback->display().state == TransportDisplay::State::device_lost; }, 2000));
        CHECK(lost.count() == 1);
        CHECK_FALSE(playback->has_device());
        playback->play(0); // refused while no output is open
        CHECK(playback->display().state == TransportDisplay::State::device_lost);

        // Retried every 2 s: the (null) output comes back without restarting the app.
        REQUIRE(wait_until([&] { return playback->has_device(); }, 6000));
        CHECK(playback->display().state == TransportDisplay::State::stopped); // DeviceLost -> Reconfigure -> Stopped
        playback->play(0);
        CHECK(wait_until([&] { return playback->transport_tick_at(steady_ns()).has_value(); }, 3000));
        playback->stop(0);
    }

    TEST_CASE("Play of a document the engine refuses ends at once with a reason") {
        application();
        auto playback = create_playback_service();
        REQUIRE(wait_until([&] { return playback->has_device(); }, 5000));
        QSignalSpy refused(playback.get(), &PlaybackService::playback_refused);
        DocumentData document = make_blank_document("Dense");
        std::vector<NewNote> notes;
        for (int index = 0; index < 9000; ++index) // more note-ons in one block than the real-time budget
            notes.push_back(NewNote{document.sequences.front().tracks.front().id, 0, 480,
                                    static_cast<std::uint8_t>(index % 128), 100, static_cast<std::uint8_t>(index % 16), 0});
        auto changes = insert_notes(document, notes);
        REQUIRE(changes);
        REQUIRE(apply_change_set(document, *changes));
        playback->publish_document(document, 0, 1);
        playback->play(0);
        REQUIRE(wait_until([&] { return refused.count() > 0; }, 5000));
        CHECK(playback->display().state == TransportDisplay::State::stopped); // not "Playing" with a frozen playhead
        CHECK_FALSE(playback->display().playing_older_generation);
    }

    TEST_CASE("changing the speed while playing is heard at once") {
        application();
        auto playback = create_playback_service();
        REQUIRE(wait_until([&] { return playback->has_device(); }, 5000));
        DocumentData document = make_blank_document("Speed");
        ChangeBuilder builder(document, "note");
        builder.add_note(document.sequences.front().tracks.front().id, 0, 960 * 400, 0, 60, 100);
        REQUIRE(apply_change_set(document, builder.finish()));
        playback->publish_document(document, 0, 1);
        playback->play(0);
        REQUIRE(wait_until([&] { return playback->transport_tick_at(steady_ns()).has_value(); }, 3000));
        const auto rate = [&] {
            const std::int64_t now = steady_ns();
            const auto first = playback->transport_tick_at(now);
            const auto later = playback->transport_tick_at(now + 250'000'000);
            return first && later ? *later - *first : Tick{-1};
        };
        CHECK(std::abs(rate() - 480) <= 2); // 120 BPM, PPQ 960: half a beat per 250 ms
        playback->set_tempo_multiplier(2.0);
        REQUIRE(wait_until([&] { return std::abs(rate() - 960) <= 2; }, 2000));
        CHECK(playback->display().state == TransportDisplay::State::playing); // no restart, no stop
        playback->stop(0);
    }
}

TEST_SUITE("recording") {
    TEST_CASE("MIDI input opens in the background and is monitored off the GUI thread, pedal and bend included") {
        auto controller = make_controller();
        FakePlayback playback;
        RecordingController recording(playback);
        recording.set_target(controller.get(), nullptr);
        QSignalSpy changed(&recording, &RecordingController::input_changed);
        recording.open_input("null:in:loopback");
        CHECK(changed.count() == 0); // the open runs on the device thread
        REQUIRE(wait_until([&] { return changed.count() > 0; }, 3000));
        CHECK(changed.at(0).at(1).toString().isEmpty()); // no error
        CHECK(recording.input_name() == "Null Loopback In");

        auto backend = platform::create_null_midi_backend();
        auto output = backend->open_output("null:out:loopback");
        REQUIRE(output);
        const std::array<std::array<std::uint8_t, 3>, 4> sent = {{{0x90, 60, 100}, {0xB0, 64, 127}, {0xE0, 0, 80}, {0x80, 60, 0}}};
        for (const auto& message : sent)
            REQUIRE((*output)->send(message));
        // Every channel message reaches the monitor (on its own thread), sustain and bend too.
        REQUIRE(wait_until([&] { return playback.monitored_copy().size() == sent.size(); }, 3000));
        const auto monitored = playback.monitored_copy();
        CHECK(monitored[1] == std::vector<std::uint8_t>{0xB0, 64, 127});
        CHECK(monitored[2] == std::vector<std::uint8_t>{0xE0, 0, 80});

        // The GUI still gets the messages for recording: a take started now records the input.
        QTest::qWait(50); // the earlier messages reach the GUI before the take starts
        recording.set_count_in_bars(0);
        recording.toggle_recording();
        playback.clock = 480;
        REQUIRE((*output)->send(std::array<std::uint8_t, 3>{0x90, 64, 90}));
        QTest::qWait(100);
        playback.clock = 960;
        REQUIRE((*output)->send(std::array<std::uint8_t, 3>{0x80, 64, 0}));
        QTest::qWait(100);
        recording.toggle_recording();
        REQUIRE(controller->notes().notes.size() == 1);
        CHECK(controller->notes().notes[0].pitch == 64);
        recording.open_input(QString()); // closes; the monitor thread stops
        CHECK(recording.input_name() == "none");
    }

    TEST_CASE("step recording follows the edit cursor and its armed state is signalled") {
        auto controller = make_controller();
        FakePlayback playback;
        RecordingController recording(playback);
        recording.set_target(controller.get(), nullptr);
        QSignalSpy armed(&recording, &RecordingController::step_recording_changed);
        controller->set_edit_cursor(0);
        recording.set_step_recording(true);
        REQUIRE(armed.count() == 1);
        CHECK(armed.at(0).at(0).toBool());
        controller->set_edit_cursor(3840); // the user clicks the ruler at bar 2
        recording.live_note(60, 100, true);
        recording.live_note(60, 0, false);
        REQUIRE(controller->notes().notes.size() == 1);
        CHECK(controller->notes().notes[0].start == 3840); // not at the cursor where it was armed
        auto other = make_controller();
        recording.set_target(other.get(), nullptr); // switching documents disarms
        CHECK_FALSE(recording.step_recording());
        CHECK_FALSE(armed.last().at(0).toBool());
    }

    TEST_CASE("full take chunks are journaled while recording, so a crash keeps the performance") {
        test::TemporaryDirectory directory;
        auto controller = make_controller();
        auto journal = document::RecoveryJournal::create(directory.path(), controller->document(),
                                                         document::SourceReference{"Take", {}, {}, {}});
        REQUIRE(journal);
        std::shared_ptr<document::RecoveryJournal> shared = std::move(*journal);
        controller->attach_recovery(shared);
        FakePlayback playback;
        RecordingController recording(playback);
        recording.set_target(controller.get(), nullptr);
        recording.set_count_in_bars(0);
        recording.toggle_recording();
        for (int index = 0; index < 2100; ++index) { // 4200 messages: one full chunk of 4096
            live(recording, playback, index * 10, 60 + index % 12, true);
            live(recording, playback, index * 10 + 5, 60 + index % 12, false);
        }
        REQUIRE(shared->flush(true));
        const Uuid session = shared->session_id();
        shared.reset();
        controller->attach_recovery(nullptr); // simulated crash: the journal is dropped, never closed
        const auto recovered = document::recover_session(directory.path(), session);
        REQUIRE_MESSAGE(recovered, (recovered ? "" : recovered.error().describe()));
        REQUIRE(recovered->document.takes.size() == 1);
        CHECK(recovered->document.takes[0].status == TakeStatus::interrupted);
        REQUIRE(recovered->document.takes[0].chunks.size() == 1);
        const auto raw = app::decode_take_chunk(recovered->document.takes[0].chunks[0].payload);
        REQUIRE(raw);
        CHECK(raw->size() == app::take_chunk_messages);
    }
}

TEST_SUITE("window") {
    TEST_CASE("playing mode plays from the keyboard while the track list has focus") {
        WindowFixture fixture;
        auto* tab = fixture.window->current_tab();
        auto* player = fixture.window->findChild<ComputerKeyboardPlayer*>();
        auto* mode = fixture.window->findChild<QAction*>("view.playing_mode");
        REQUIRE(player);
        REQUIRE(mode);
        mode->trigger();
        REQUIRE(player->enabled());
        QSignalSpy on(player, &ComputerKeyboardPlayer::note_on);
        QSignalSpy off(player, &ComputerKeyboardPlayer::note_off);
        auto* table = tab->track_list().findChild<QTableWidget*>();
        REQUIRE(table);
        table->setCurrentCell(0, 0);
        table->setFocus(Qt::MouseFocusReason);
        const std::string name = tab->controller().active_sequence().tracks.front().name;
        fixture.key(Qt::Key_A);
        CHECK(on.count() == 1);
        CHECK(off.count() == 1);
        CHECK(tab->controller().active_sequence().tracks.front().name == name); // no rename, no type-ahead
        mode->trigger(); // off again
        CHECK_FALSE(player->enabled());
    }

    TEST_CASE("a ruler click while playing moves playback there") {
        WindowFixture fixture;
        auto* tab = fixture.window->current_tab();
        auto* label = fixture.window->findChild<QLabel*>("transport.position");
        REQUIRE(label);
        // A song long enough to keep playing (a blank song ends at once: review_2 T1).
        const NewNote note{tab->controller().active_track(), 0, 3840 * 32, 60, 100, 0, 0};
        auto changes = insert_notes(tab->controller().document(), std::span(&note, 1));
        REQUIRE(changes);
        REQUIRE(tab->controller().apply(*changes));
        tab->piano_roll().setFocus();
        fixture.key(Qt::Key_Space);
        REQUIRE(wait_until([&] { return label->text().contains("Play"); }, 3000));
        emit tab->piano_roll().seek_requested(8 * 3840); // what a click on bar 9 of the ruler sends
        REQUIRE(wait_until([&] { return label->text().startsWith("9.") || label->text().startsWith("10."); }, 3000));
        CHECK(label->text().contains("Play")); // still playing, from the clicked bar
        fixture.key(Qt::Key_Space); // Space now stops back at the clicked position
        REQUIRE(wait_until([&] { return label->text().contains("Stop"); }, 2000));
        CHECK(label->text().startsWith("9.1.000"));
    }
}

// Reviewer probes for batch 1 (append to tests/ui_tests.cpp at commit d31afc4; they use the
// FakePlayback, live() and make_controller() helpers defined there). Both FAIL at d31afc4.

TEST_SUITE("reviewer_probe") {
    // R1: the engine clock counts loop passes since PLAYBACK started; a punch-in in pass >= 1 that
    // punches out in the same pass keeps pass (stop_pass - 1), which the take never saw.
    TEST_CASE("probe: punch-in while looping in the third pass keeps the notes played in that pass") {
        auto controller = make_controller();
        FakePlayback playback;
        playback.loop_range = TimeRange{0, 1920};
        playback.play(500); // the loop is in its third pass; the folded playhead is at 500
        RecordingController recording(playback);
        recording.set_target(controller.get(), nullptr);
        recording.toggle_recording(); // punch in at 500
        REQUIRE(recording.recording());
        live(recording, playback, 2 * 1920 + 600, 60, true); // linear engine ticks: two passes played
        live(recording, playback, 2 * 1920 + 900, 60, false);
        playback.clock = 2 * 1920 + 1200; // punch out in the same pass
        recording.toggle_recording();
        CHECK(controller->notes().notes.size() == 1); // d31afc4: 0
    }

    // R2: stop during the count-in from bar 1 -> stop_tick < 0 -> RecordingTake::start_tick < 0,
    // which native validation rejects: Save, Save As and recovery checkpoints fail until undo.
    TEST_CASE("probe: stopping during the count-in from bar 1 leaves a savable document") {
        auto controller = make_controller();
        FakePlayback playback;
        RecordingController recording(playback);
        recording.set_target(controller.get(), nullptr);
        recording.set_count_in_bars(1);
        controller->set_edit_cursor(0);
        recording.toggle_recording();
        REQUIRE(recording.recording());
        live(recording, playback, -1500, 60, true);
        playback.clock = -1000; // the user stops during the count-in
        recording.toggle_recording();
        test::TemporaryDirectory directory;
        const auto saved = document::save_native(controller->document(), directory.path() / "probe.mwb");
        CHECK(saved.has_value()); // d31afc4: "recording take has an invalid range" (take start -1000)
    }
}

TEST_SUITE("recording") {
    TEST_CASE("a punch-in while looping takes its first pass from the engine clock at the punch") {
        auto controller = make_controller();
        FakePlayback playback;
        playback.loop_range = TimeRange{0, 1920};
        playback.play(500);
        RecordingController recording(playback);
        recording.set_target(controller.get(), nullptr);
        playback.clock = 3 * 1920 + 500; // the engine is in pass 3 when Record is pressed
        recording.toggle_recording();
        live(recording, playback, 4 * 1920 + 100, 62, true); // pass 4, the only complete pass
        live(recording, playback, 4 * 1920 + 300, 62, false);
        playback.clock = 5 * 1920 + 50; // punch out early in pass 5
        recording.toggle_recording();
        REQUIRE(controller->notes().notes.size() == 1);
        CHECK(controller->notes().notes[0].start == 100);
    }

    TEST_CASE("stopping during the count-in keeps no take and leaves no undo entry") {
        auto controller = make_controller();
        FakePlayback playback;
        RecordingController recording(playback);
        recording.set_target(controller.get(), nullptr);
        recording.set_count_in_bars(1);
        controller->set_edit_cursor(0);
        recording.toggle_recording();
        playback.clock = -500;
        recording.toggle_recording();
        CHECK(controller->document().takes.empty());
        CHECK_FALSE(controller->can_undo());
    }
}

TEST_SUITE("audio_device") {
    TEST_CASE("losing the device during the count-in shows the loss and plays nothing") {
        application();
        auto playback = create_playback_service();
        REQUIRE(wait_until([&] { return playback->has_device(); }, 5000));
        playback->set_tempo_multiplier(0.25); // a long count-in
        playback->publish_document(make_blank_document("Count-in loss"), 0, 1);
        REQUIRE(playback->record(0, 2));
        REQUIRE(wait_until([&] { return playback->transport_tick_at(steady_ns()).has_value(); }, 3000));
        CHECK(playback->display().state == TransportDisplay::State::counting_in);
        playback->simulate_device_loss();
        REQUIRE(wait_until([&] { return playback->display().state == TransportDisplay::State::device_lost; }, 2000));
        CHECK_FALSE(playback->has_device());
        CHECK_FALSE(playback->transport_tick_at(steady_ns()));
        playback->stop(0);
    }
}

TEST_SUITE("keyboard") {
    TEST_CASE("a held computer-keyboard note ends on release even with Ctrl held by then") {
        application();
        ComputerKeyboardPlayer player;
        player.set_enabled(true);
        QSignalSpy on(&player, &ComputerKeyboardPlayer::note_on);
        QSignalSpy off(&player, &ComputerKeyboardPlayer::note_off);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
        CHECK(player.handle_key(&press, true));
        QKeyEvent ctrl_press(QEvent::KeyPress, Qt::Key_S, Qt::ControlModifier); // Ctrl+S is a command
        CHECK_FALSE(player.handle_key(&ctrl_press, true));
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_A, Qt::ControlModifier); // Ctrl still down
        CHECK(player.handle_key(&release, false));
        CHECK(on.count() == 1);
        REQUIRE(off.count() == 1);
        CHECK(off.at(0).at(0).toInt() == on.at(0).at(0).toInt());
    }

    TEST_CASE("the on-screen keyboard scrolls by accumulated wheel notches") {
        application();
        KeyboardWidget keyboard;
        keyboard.resize(600, 80);
        const int start = keyboard.view_start();
        const auto wheel = [&](QPoint delta) {
            QWheelEvent event(QPointF(10, 10), QPointF(10, 10), QPoint(), delta, Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QApplication::sendEvent(&keyboard, &event);
        };
        wheel(QPoint(0, 0)); // no delta: nothing
        CHECK(keyboard.view_start() == start);
        wheel(QPoint(0, -40)); // touchpad steps add up to one notch
        wheel(QPoint(0, -40));
        CHECK(keyboard.view_start() == start);
        wheel(QPoint(0, -40));
        CHECK(keyboard.view_start() == start + 7);
        wheel(QPoint(120, 0)); // a horizontal swipe scrolls too (left)
        CHECK(keyboard.view_start() == start);
    }
}

TEST_SUITE("clipboard") {
    TEST_CASE("Drag MIDI of a selection exports exactly the selected notes") {
        auto controller = make_controller();
        std::vector<NewNote> notes = {{controller->active_track(), 0, 480, 60, 100, 0, 0},
                                      {controller->active_track(), 240, 720, 64, 100, 0, 0},
                                      {controller->active_track(), 480, 960, 67, 100, 0, 0}};
        auto changes = insert_notes(controller->document(), notes);
        REQUIRE(changes);
        REQUIRE(controller->apply(*changes));
        const auto& all = controller->notes().notes;
        REQUIRE(all.size() == 3);
        controller->set_selection({all[0].on_id, all[2].on_id}); // 64 lies inside their span but is not selected
        std::string title;
        const auto options = drag_export_options(*controller, &title);
        CHECK(QString::fromStdString(title).endsWith("selection"));
        auto bytes = midi::export_midi(controller->document(), options);
        REQUIRE(bytes);
        auto imported = midi::import_midi(*bytes, "drag.mid", {}, {});
        REQUIRE(imported);
        std::vector<int> pitches;
        for (const auto& sequence : imported->document.sequences)
            for (const auto& note : pair_notes(sequence).notes)
                pitches.push_back(note.pitch);
        std::ranges::sort(pitches);
        CHECK(pitches == std::vector<int>{60, 67});
    }
}

TEST_SUITE("event_list") {
    TEST_CASE("between an edit and the rebuild a row never shows or edits another event") {
        auto controller = make_controller();
        const NewNote note{controller->active_track(), 960, 1440, 60, 100, 0, 0};
        auto changes = insert_notes(controller->document(), std::span(&note, 1));
        REQUIRE(changes);
        REQUIRE(controller->apply(*changes));
        EventTableModel model(*controller);
        QTest::qWait(200);
        int row = -1;
        for (int index = 0; index < model.rowCount(); ++index)
            if (const Event* event = model.event_at(index); event && event->is_note_start())
                row = index;
        REQUIRE(row >= 0);
        const EventId shown = model.event_at(row)->id;
        // An edit that shifts the track's events: a note before the shown one.
        const NewNote earlier{controller->active_track(), 0, 120, 72, 100, 0, 0};
        auto shift = insert_notes(controller->document(), std::span(&earlier, 1));
        REQUIRE(shift);
        REQUIRE(controller->apply(*shift));
        const Event* now = model.event_at(row); // the rebuild has not run yet
        CHECK((now == nullptr || now->id == shown));
        if (now == nullptr) {
            const bool edited = model.setData(model.index(row, EventTableModel::column_tick), 5, Qt::EditRole);
            CHECK_FALSE(edited); // a stale row refuses edits
        }
        QTest::qWait(200);
        CHECK(model.row_of(shown) >= 0);
        CHECK(model.event_at(model.row_of(shown))->id == shown);
    }
}

TEST_SUITE("inspector") {
    TEST_CASE("a Song field writes only itself, keeps unusual meters and commits a step burst once") {
        auto controller = make_controller();
        // 3/64 with non-default clock bytes, and an exact tempo the 2-decimal display would round.
        ChangeBuilder builder(controller->document(), "setup");
        const TrackId track = controller->active_track();
        for (const auto& event : controller->active_sequence().tracks.front().events)
            if (event.tick == 0 && (event.is_meta(meta_type::time_signature) || event.is_meta(meta_type::tempo)))
                builder.remove(track, event.id);
        Event meter = events::time_signature(0, 3, 6);
        meter.payload = Payload(std::vector<std::uint8_t>{3, 6, 12, 4});
        std::ignore = builder.insert(track, meter, OrderPlacement::before_existing);
        std::ignore = builder.insert(track, events::tempo(0, 523'457), OrderPlacement::before_existing);
        REQUIRE(controller->apply(builder.finish()));
        const auto before = controller->document().sequences.front().tracks.front().events;

        InspectorPanel inspector;
        inspector.set_controller(controller.get());
        inspector.show();
        QSpinBox* loop = nullptr;
        for (auto* box : inspector.findChildren<QSpinBox*>())
            if (box->suffix().contains("bars"))
                loop = box;
        REQUIRE(loop);
        const auto loop_before = controller->active_sequence().settings.loop_region;
        // Three arrow steps: one edit after the pause.
        loop->stepBy(1);
        loop->stepBy(1);
        loop->stepBy(1);
        QTest::qWait(600);
        REQUIRE(controller->can_undo());
        CHECK(controller->undo_text().contains("loop", Qt::CaseInsensitive));
        // Tempo and the 3/64 meter (with its clock bytes) are untouched by a loop edit.
        const auto& after = controller->document().sequences.front().tracks.front().events;
        for (const auto& event : before)
            if (event.is_meta(meta_type::time_signature) || event.is_meta(meta_type::tempo)) {
                bool kept = false;
                for (const auto& now : after)
                    kept = kept || (now.id == event.id && now.payload == event.payload);
                CHECK(kept);
            }
        REQUIRE(controller->undo()); // one undo removes the whole burst
        CHECK(controller->active_sequence().settings.loop_region == loop_before);
        CHECK(controller->undo_text() == "setup");
    }

    TEST_CASE("new-note length and velocity are set in the inspector and used for drawing") {
        auto controller = make_controller();
        InspectorPanel inspector;
        inspector.set_controller(controller.get());
        QComboBox* length = nullptr;
        for (auto* combo : inspector.findChildren<QComboBox*>())
            if (combo->toolTip().startsWith("Length of new notes"))
                length = combo;
        REQUIRE(length);
        const int quarter = length->findData("1/4");
        REQUIRE(quarter >= 0);
        length->setCurrentIndex(quarter);
        emit length->activated(quarter);
        CHECK(controller->active_sequence().settings.default_note_length == Rational{1, 4});
    }
}

TEST_SUITE("window") {
    TEST_CASE("toolbar Snap shows custom grids, offers Off, and the view follows and lights the playhead") {
        WindowFixture fixture;
        auto* tab = fixture.window->current_tab();
        auto& controller = tab->controller();
        QComboBox* snap = nullptr;
        for (auto* combo : fixture.window->findChildren<QComboBox*>())
            if (combo->accessibleName() == "Snap")
                snap = combo;
        REQUIRE(snap);
        // A grid the list does not offer is shown as itself, not a stale entry.
        Sequence updated = sequence_header(controller.active_sequence());
        updated.settings.snap = Rational{1, 96};
        ChangeBuilder builder(controller.document(), "snap");
        builder.set_sequence_properties(updated.id, updated);
        REQUIRE(controller.apply(builder.finish()));
        QTest::qWait(50);
        CHECK(snap->currentText().contains("1/96"));
        // Off is a view setting: the roll stops snapping.
        const int off = snap->findData("off");
        REQUIRE(off >= 0);
        snap->setCurrentIndex(off);
        emit snap->activated(off);
        CHECK(tab->piano_roll().snap_mode() == SnapMode::off);
        auto* off_action = fixture.window->findChild<QAction*>("view.snap_off");
        REQUIRE(off_action);
        CHECK(off_action->isChecked());
        off_action->trigger(); // Ctrl+4 toggles it back
        CHECK(tab->piano_roll().snap_mode() != SnapMode::off);

        // Follow and sounding notes: a long note at the start sounds while playing.
        const NewNote note{controller.active_track(), 0, 960 * 64, 62, 100, 0, 0};
        auto changes = insert_notes(controller.document(), std::span(&note, 1));
        REQUIRE(changes);
        REQUIRE(controller.apply(*changes));
        auto* follow = fixture.window->findChild<QAction*>("view.follow_playhead");
        REQUIRE(follow);
        CHECK(follow->isChecked());
        tab->piano_roll().setFocus();
        controller.set_edit_cursor(0);
        fixture.key(Qt::Key_Space);
        REQUIRE(wait_until([&] { return tab->piano_roll().sounding_pitches().contains(62); }, 3000));
        CHECK(tab->keyboard().key_held(62, KeySource::playback));
        fixture.key(Qt::Key_Space);
        REQUIRE(wait_until([&] { return tab->piano_roll().sounding_pitches().empty(); }, 2000));
        CHECK_FALSE(tab->keyboard().key_held(62, KeySource::playback));
    }
}

// Reviewer probes, batch 2 (probe_batch2); the stray closing braces of the file and the
// reviewer-local demo.mid path were adjusted.

TEST_SUITE("reviewer_probe2") {
    TEST_CASE("probe2: closing the window with several tabs and an open take does not crash") {
        WindowFixture fixture;
        fixture.window->new_from_template("blank");
        fixture.window->new_from_template("blank");
        QTest::qWait(300);
        auto* tabs = fixture.window->findChild<QTabWidget*>();
        REQUIRE(tabs);
        MESSAGE("tabs " << tabs->count());
        tabs->setCurrentIndex(0);
        QTest::qWait(50);
        fixture.key(Qt::Key_R); // a take is running when the window goes
        QTest::qWait(200);
        fixture.window.reset();
        CHECK(true);
    }
    TEST_CASE("probe2: punch-in in pass 2, play only in pass 3, stop in pass 4 keeps pass 3") {
        auto controller = make_controller();
        FakePlayback playback;
        playback.loop_range = TimeRange{0, 1920};
        playback.play(500);
        playback.clock = 2 * 1920 + 500; // engine clock at the punch
        RecordingController recording(playback);
        recording.set_target(controller.get(), nullptr);
        recording.toggle_recording();
        live(recording, playback, 3 * 1920 + 100, 64, true);
        live(recording, playback, 3 * 1920 + 300, 64, false);
        playback.clock = 4 * 1920 + 50;
        recording.toggle_recording();
        REQUIRE(controller->notes().notes.size() == 1);
        CHECK(controller->notes().notes[0].start == 100);
    }
}

TEST_SUITE("reviewer_probe3") {
    TEST_CASE("probe3: ruler seek trace") {
        WindowFixture fixture;
        auto* tab = fixture.window->current_tab();
        auto* label = fixture.window->findChild<QLabel*>("transport.position");
        REQUIRE(label);
        tab->piano_roll().setFocus();
        fixture.key(Qt::Key_Space);
        REQUIRE(wait_until([&] { return label->text().contains("Play"); }, 3000));
        MESSAGE("after play: " << label->text().toStdString());
        emit tab->piano_roll().seek_requested(8 * 3840);
        for (int i = 0; i < 10; ++i) {
            QTest::qWait(100);
            MESSAGE(i << ": " << label->text().toStdString());
        }
        fixture.key(Qt::Key_Space);
        for (int i = 0; i < 4; ++i) {
            QTest::qWait(100);
            MESSAGE("stopped " << i << ": " << label->text().toStdString() << " cursor " << tab->controller().edit_cursor());
        }
    }
}

TEST_SUITE("reviewer_probe4") {
    TEST_CASE("probe4: ruler seek while playing a real song") {
        WindowFixture fixture;
        REQUIRE(fixture.window->open_path(std::filesystem::path(__FILE__).parent_path().parent_path() / "docs" / "images" / "demo.mid"));
        QTest::qWait(500);
        auto* tab = fixture.window->current_tab();
        auto* label = fixture.window->findChild<QLabel*>("transport.position");
        REQUIRE(label);
        tab->piano_roll().setFocus();
        fixture.key(Qt::Key_Space);
        REQUIRE(wait_until([&] { return label->text().contains("Play"); }, 3000));
        QTest::qWait(200);
        emit tab->piano_roll().seek_requested(2 * 3840); // bar 3
        QTest::qWait(150);
        MESSAGE("after seek: " << label->text().toStdString());
        CHECK((label->text().startsWith("3.") && label->text().contains("Play")));
        fixture.key(Qt::Key_Space);
        QTest::qWait(150);
        MESSAGE("after stop: " << label->text().toStdString());
        CHECK(label->text().startsWith("3.1.000"));
    }
}

TEST_SUITE("window") {
    TEST_CASE("Drag MIDI, the readout, the BPM field and the panel toggles stay visible at laptop widths") {
        WindowFixture fixture;
        auto* drag = fixture.window->findChild<DragHandle*>();
        auto* readout = fixture.window->findChild<QLabel*>("transport.position");
        auto* tempo = fixture.window->findChild<TempoField*>();
        auto* panels = fixture.window->findChild<QToolBar*>("toolbar.panels");
        REQUIRE(drag);
        REQUIRE(readout);
        REQUIRE(tempo);
        REQUIRE(panels);
        for (const int width : {1400, 1366, 1280}) {
            fixture.window->resize(width, 800);
            QTest::qWait(100);
            INFO("window width " << width);
            // An overflowed toolbar widget is hidden (it moves into the >> popup).
            CHECK(drag->isVisible());
            CHECK(readout->isVisible());
            CHECK(tempo->isVisible());
            CHECK(drag->mapTo(fixture.window.get(), QPoint(drag->width(), 0)).x() <= fixture.window->width());
            CHECK(panels->isVisible());
            for (auto* action : panels->actions())
                CHECK(panels->widgetForAction(action)->isVisible());
        }
    }
}

// Reviewer probes, batch 3 (probe_batch3).

TEST_SUITE("reviewer_probe3") {
    TEST_CASE("probe3: drum grid of the playing tab gets the playhead and follows it") {
        WindowFixture fixture;
        auto* tab = fixture.window->current_tab();
        auto& controller = tab->controller();
        const NewNote note{controller.active_track(), 0, 960 * 256, 62, 100, 0, 0};
        auto changes = insert_notes(controller.document(), std::span(&note, 1));
        REQUIRE(changes);
        REQUIRE(controller.apply(*changes));
        tab->piano_roll().setFocus();
        fixture.key(Qt::Key_Space);
        REQUIRE(wait_until([&] { return tab->drum_grid().playhead().has_value() && *tab->drum_grid().playhead() > 0; }, 3000));
        fixture.key(Qt::Key_Space);
        REQUIRE(wait_until([&] { return !tab->drum_grid().playhead().has_value(); }, 2000));
    }
    TEST_CASE("probe3: changing the meter numerator keeps the clock bytes and an x/64 denominator") {
        auto controller = make_controller();
        ChangeBuilder builder(controller->document(), "setup");
        const TrackId track = controller->active_track();
        for (const auto& event : controller->active_sequence().tracks.front().events)
            if (event.tick == 0 && event.is_meta(meta_type::time_signature))
                builder.remove(track, event.id);
        Event meter = events::time_signature(0, 3, 6);
        meter.payload = Payload(std::vector<std::uint8_t>{3, 6, 12, 4});
        std::ignore = builder.insert(track, meter, OrderPlacement::before_existing);
        REQUIRE(controller->apply(builder.finish()));
        InspectorPanel inspector;
        inspector.set_controller(controller.get());
        inspector.show();
        QSpinBox* numerator = nullptr;
        for (auto* box : inspector.findChildren<QSpinBox*>())
            if (box->value() == 3 && box->maximum() == 255)
                numerator = box;
        REQUIRE(numerator);
        numerator->setFocus();
        numerator->stepBy(2); // 5/64
        QTest::qWait(600);
        bool found = false;
        for (const auto& event : controller->active_sequence().tracks.front().events)
            if (event.tick == 0 && event.is_meta(meta_type::time_signature)) {
                const auto bytes = event.payload.bytes();
                MESSAGE("bytes " << int(bytes[0]) << " " << int(bytes[1]) << " " << int(bytes[2]) << " " << int(bytes[3]));
                found = bytes.size() == 4 && bytes[0] == 5 && bytes[1] == 6 && bytes[2] == 12 && bytes[3] == 4;
            }
        CHECK(found);
        CHECK(controller->undo_text().contains("meter", Qt::CaseInsensitive));
        numerator->clearFocus();
        inspector.hide();
    }
    TEST_CASE("probe3: inspector tempo with a later tempo change edits bar 1 only") {
        auto controller = make_controller();
        ChangeBuilder builder(controller->document(), "add tempo change");
        std::ignore = builder.insert(controller->active_track(), events::tempo(3840, 600000));
        REQUIRE(controller->apply(builder.finish()));
        InspectorPanel inspector;
        inspector.set_controller(controller.get());
        inspector.show();
        QDoubleSpinBox* tempo = nullptr;
        for (auto* box : inspector.findChildren<QDoubleSpinBox*>())
            if (box->suffix().contains("BPM"))
                tempo = box;
        REQUIRE(tempo);
        tempo->setValue(90.0);
        emit tempo->editingFinished();
        CHECK(controller->tempo_map().tempo_at(0) == 666667);
        CHECK(controller->tempo_map().tempo_at(3840) == 600000);
    }
}

TEST_SUITE("inspector") {
    TEST_CASE("a meter change keeps the loop the same number of bars") {
        auto controller = make_controller();
        InspectorPanel inspector;
        inspector.set_controller(controller.get());
        QSpinBox* loop = nullptr;
        QSpinBox* numerator = nullptr;
        for (auto* box : inspector.findChildren<QSpinBox*>()) {
            if (box->suffix().contains("bars"))
                loop = box;
            if (box->maximum() == 255 && box->minimum() == 1)
                numerator = box;
        }
        REQUIRE(loop);
        REQUIRE(numerator);
        loop->setValue(4);
        emit loop->editingFinished();
        const auto loop_length = [&] {
            const auto& sequence = controller->active_sequence();
            for (const auto& region : sequence.regions)
                if (sequence.settings.loop_region && region.id == *sequence.settings.loop_region)
                    return region.end_tick - region.start_tick;
            return Tick{0};
        };
        CHECK(loop_length() == 4 * 3840);
        numerator->setValue(3);
        emit numerator->editingFinished();
        CHECK(loop_length() == 4 * 2880); // still 4 bars, now of 3/4
        CHECK(loop->value() == 4);
        REQUIRE(controller->undo()); // meter and loop come back together
        CHECK(loop_length() == 4 * 3840);
    }
}

namespace {

double relative_luminance(const QColor& color) {
    const auto channel = [](double value) {
        value /= 255.0;
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.red()) + 0.7152 * channel(color.green()) + 0.0722 * channel(color.blue());
}

double contrast(const QColor& a, const QColor& b) {
    const double high = std::max(relative_luminance(a), relative_luminance(b));
    const double low = std::min(relative_luminance(a), relative_luminance(b));
    return (high + 0.05) / (low + 0.05);
}

} // namespace

TEST_SUITE("theme") {
    TEST_CASE("dark and light spin boxes have a 3:1 frame and keep their arrow buttons") {
        application();
        for (const Theme theme : {Theme::dark, Theme::light}) {
            apply_theme(theme);
            QWidget panel;
            auto* spin = new QSpinBox(&panel);
            spin->setGeometry(10, 10, 120, 26);
            auto* other = new QLineEdit(&panel); // takes the focus: the unfocused frame is measured
            other->setGeometry(10, 50, 120, 26);
            panel.resize(140, 86);
            panel.show();
            other->setFocus();
            QTest::qWait(30);
            REQUIRE_FALSE(spin->hasFocus());
            const QImage image = spin->grab().toImage();
            const QColor window = panel.palette().color(QPalette::Window);
            // The left edge, halfway down: the frame line.
            const QColor edge = image.pixelColor(0, image.height() / 2);
            INFO("theme " << static_cast<int>(theme) << " edge " << edge.name().toStdString() << " window "
                          << window.name().toStdString());
            CHECK(contrast(edge, window) >= 3.0);
            // Fusion's up/down buttons are still there (a style-sheet border would remove them).
            QStyleOptionSpinBox option;
            option.initFrom(spin);
            option.subControls = QStyle::SC_SpinBoxUp | QStyle::SC_SpinBoxDown;
            CHECK(spin->style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxUp, spin).width() > 0);
        }
        apply_theme(Theme::light);
    }
}

namespace {

/// A piano roll over a controller holding `notes`, shown at a fixed size.
struct RollFixture {
    std::unique_ptr<DocumentController> controller = make_controller();
    std::unique_ptr<PianoRollView> view;

    /// Notes as (start, end, pitch) on the controller's active track.
    explicit RollFixture(const std::vector<std::tuple<Tick, Tick, int>>& specs) {
        std::vector<NewNote> notes;
        for (const auto& [start, end, pitch] : specs)
            notes.push_back(NewNote{controller->active_track(), start, end, static_cast<std::uint8_t>(pitch), 100, 0, 0});
        if (!notes.empty()) {
            auto changes = insert_notes(controller->document(), notes);
            REQUIRE(changes);
            REQUIRE(controller->apply(*changes));
        }
        view = std::make_unique<PianoRollView>(*controller);
        view->resize(900, 500);
        view->show();
        QTest::qWait(20);
    }

    [[nodiscard]] QPoint at(Tick tick, int pitch) const {
        const auto& mapping = view->mapping();
        return {PianoRollView::keyboard_width + static_cast<int>(std::lround(mapping.tick_to_x(tick))),
                PianoRollView::ruler_height + static_cast<int>(mapping.pitch_to_y(pitch)) + 4};
    }
    [[nodiscard]] int pixels(Tick ticks) const {
        return static_cast<int>(std::lround(static_cast<double>(ticks) / view->mapping().ticks_per_pixel));
    }
    void drag(QPoint from, QPoint to, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::mousePress(view.get(), Qt::LeftButton, modifiers, from);
        // QTest::mouseMove carries no modifiers; Shift and Alt act during the move.
        for (const QPoint point : {from + (to - from) / 4, from + (to - from) / 2, to}) {
            QMouseEvent move(QEvent::MouseMove, QPointF(point), view->mapToGlobal(QPointF(point)), Qt::NoButton,
                             Qt::LeftButton, modifiers);
            QApplication::sendEvent(view.get(), &move);
        }
        QTest::mouseRelease(view.get(), Qt::LeftButton, modifiers, to);
    }
    [[nodiscard]] std::vector<NoteSpan> notes() const { return controller->notes().notes; }
};


} // namespace

TEST_SUITE("gestures") {
    TEST_CASE("dragging a note's end resizes it on the grid as one undo entry") {
        RollFixture roll({{960, 1920, 64}});
        const auto before = semantic_hash(roll.controller->document());
        const QPoint end = roll.at(1920, 64) + QPoint(-2, 0);
        roll.drag(end, end + QPoint(roll.pixels(480), 0));
        REQUIRE(roll.notes().size() == 1);
        CHECK(roll.notes()[0].start == 960);
        CHECK(roll.notes()[0].end == 2400);
        REQUIRE(roll.controller->undo());
        CHECK(semantic_hash(roll.controller->document()) == before); // exactly one entry
    }

    TEST_CASE("a rubber band selects the notes it touches; Ctrl adds to the selection") {
        RollFixture roll({{0, 480, 60}, {960, 1440, 62},
                          {3840, 4320, 64}});
        // Empty space above the first two notes, dragged down across them.
        roll.drag(roll.at(0, 66) + QPoint(2, 0), roll.at(1440, 59) + QPoint(10, 0));
        CHECK(roll.controller->selection().size() == 2);
        roll.drag(roll.at(3700, 66), roll.at(4400, 63), Qt::ControlModifier);
        CHECK(roll.controller->selection().size() == 3);
        CHECK(roll.controller->undo_text() == QString("Insert notes")); // selection is not an edit
    }

    TEST_CASE("Ctrl-click toggles one note in or out of the selection") {
        RollFixture roll({{0, 960, 60}, {1920, 2880, 67}});
        const auto first = roll.notes()[0].on_id;
        const auto second = roll.notes()[1].on_id;
        QTest::mouseClick(roll.view.get(), Qt::LeftButton, Qt::NoModifier, roll.at(200, 60));
        CHECK(roll.controller->selection() == std::set<EventId>{first});
        QTest::mouseClick(roll.view.get(), Qt::LeftButton, Qt::ControlModifier, roll.at(2100, 67));
        CHECK(roll.controller->selection() == std::set<EventId>{first, second});
        QTest::mouseClick(roll.view.get(), Qt::LeftButton, Qt::ControlModifier, roll.at(200, 60));
        CHECK(roll.controller->selection() == std::set<EventId>{second});
    }

    TEST_CASE("the erase and split tools change exactly the clicked note") {
        RollFixture roll({{0, 1920, 60}, {0, 1920, 64}});
        roll.view->set_tool(EditTool::split);
        QTest::mouseClick(roll.view.get(), Qt::LeftButton, Qt::NoModifier, roll.at(960, 60));
        auto notes = roll.notes();
        REQUIRE(notes.size() == 3);
        CHECK(std::ranges::count_if(notes, [](const NoteSpan& note) { return note.pitch == 60; }) == 2);
        roll.view->set_tool(EditTool::erase);
        QTest::mouseClick(roll.view.get(), Qt::LeftButton, Qt::NoModifier, roll.at(500, 64));
        notes = roll.notes();
        REQUIRE(notes.size() == 2);
        CHECK(std::ranges::none_of(notes, [](const NoteSpan& note) { return note.pitch == 64; }));
        REQUIRE(roll.controller->undo()); // the erase is one entry
        CHECK(roll.notes().size() == 3);
    }

    TEST_CASE("Shift locks a drag to pitch; Alt moves off the grid") {
        RollFixture roll({{960, 1920, 60}});
        const QPoint body = roll.at(1100, 60);
        roll.drag(body, body + QPoint(roll.pixels(300), -static_cast<int>(roll.view->mapping().row_height * 3)),
                  Qt::ShiftModifier);
        REQUIRE(roll.notes().size() == 1);
        CHECK(roll.notes()[0].start == 960); // horizontal jitter ignored
        CHECK(roll.notes()[0].pitch == 63);
        const QPoint moved = roll.at(1100, 63);
        roll.drag(moved, moved + QPoint(roll.pixels(330), 0), Qt::AltModifier); // past the drag threshold
        const Tick start = roll.notes()[0].start;
        CHECK(start != 960);
        CHECK(start % 240 != 0); // not snapped to the 1/16 grid
    }

    TEST_CASE("a draw-tool drag sets the length on the grid; a click keeps the default") {
        RollFixture roll({});
        roll.view->set_tool(EditTool::draw);
        roll.drag(roll.at(0, 60) + QPoint(2, 0), roll.at(960, 60) - QPoint(2, 0)); // inside the cell ending at 960
        REQUIRE(roll.notes().size() == 1);
        CHECK(roll.notes()[0].end - roll.notes()[0].start == 960);
        QTest::mouseClick(roll.view.get(), Qt::LeftButton, Qt::NoModifier, roll.at(1920, 62) + QPoint(2, 0));
        REQUIRE(roll.notes().size() == 2);
        CHECK(roll.notes()[1].end - roll.notes()[1].start == 240); // default 1/16 at PPQ 960
    }
}

namespace {

/// Position of `target` in the Tab chain after `from` (only widgets Tab can reach), or -1.
int tab_distance(QWidget* from, QWidget* target) {
    int steps = 0;
    for (QWidget* widget = from->nextInFocusChain(); widget && widget != from; widget = widget->nextInFocusChain()) {
        if (!(widget->focusPolicy() & Qt::TabFocus) || !widget->isVisibleTo(from->window()))
            continue;
        ++steps;
        if (widget == target)
            return steps;
    }
    return -1;
}

template <typename Widget>
Widget* find_by(QWidget& root, const std::function<bool(Widget*)>& match) {
    for (auto* widget : root.findChildren<Widget*>())
        if (match(widget))
            return widget;
    return nullptr;
}

} // namespace

TEST_SUITE("inspector") {
    TEST_CASE("Apply runs off the GUI thread and reports what it applied, not a stale preview") {
        auto controller = make_controller();
        std::vector<NewNote> notes = {{controller->active_track(), 13, 253, 60, 100, 0, 0},
                                      {controller->active_track(), 487, 700, 64, 100, 0, 0}};
        REQUIRE(controller->apply(*insert_notes(controller->document(), notes)));
        std::set<EventId> all;
        for (const auto& note : controller->notes().notes)
            all.insert(note.on_id);
        controller->set_selection(all);
        InspectorPanel inspector;
        inspector.set_controller(controller.get());
        inspector.show();
        auto* apply = find_by<QPushButton>(inspector, [](QPushButton* button) { return button->text() == "Apply"; });
        REQUIRE(apply);
        const auto generation = controller->generation();
        apply->click();
        CHECK(controller->generation() == generation); // the transform is computed in the background
        REQUIRE(wait_until([&] { return controller->generation() != generation; }, 3000));
        CHECK(controller->notes().notes[0].start % 240 == 0); // quantized
        QTest::qWait(50);
        auto* report = find_by<QLabel>(inspector, [](QLabel* label) { return label->text().startsWith("Applied"); });
        CHECK(report != nullptr);
        CHECK(find_by<QLabel>(inspector, [](QLabel* label) { return label->text().contains("preview again"); }) == nullptr);
    }

    TEST_CASE("Rotate phrase works from the quick form, inside the selection's extent") {
        auto controller = make_controller();
        std::vector<NewNote> notes = {{controller->active_track(), 0, 240, 60, 100, 0, 0},
                                      {controller->active_track(), 720, 960, 64, 100, 0, 0}};
        REQUIRE(controller->apply(*insert_notes(controller->document(), notes)));
        std::set<EventId> all;
        for (const auto& note : controller->notes().notes)
            all.insert(note.on_id);
        controller->set_selection(all);
        InspectorPanel inspector;
        inspector.set_controller(controller.get());
        auto* tool = find_by<QComboBox>(inspector, [](QComboBox* combo) { return combo->findData("rotate") >= 0; });
        REQUIRE(tool);
        tool->setCurrentIndex(tool->findData("rotate"));
        emit tool->activated(tool->currentIndex());
        auto* apply = find_by<QPushButton>(inspector, [](QPushButton* button) { return button->text() == "Apply"; });
        REQUIRE(apply);
        const auto generation = controller->generation();
        apply->click();
        REQUIRE(wait_until([&] { return controller->generation() != generation; }, 3000));
        // Default shift 240 ticks inside [0, 960): 0 -> 240 and 720 -> 960 wraps to 0.
        std::vector<Tick> starts;
        for (const auto& note : controller->notes().notes)
            starts.push_back(note.start);
        std::ranges::sort(starts);
        CHECK(starts == std::vector<Tick>{0, 240});
    }

    TEST_CASE("Tab walks the inspector in screen order") {
        auto controller = make_controller();
        InspectorPanel inspector;
        inspector.set_controller(controller.get());
        inspector.show();
        auto* advanced = find_by<QCheckBox>(inspector, [](QCheckBox* box) { return box->text().startsWith("Show all"); });
        auto* preview = find_by<QPushButton>(inspector, [](QPushButton* button) { return button->text() == "Preview"; });
        auto* song = find_by<QToolButton>(inspector, [](QToolButton* button) { return button->text() == "Song"; });
        auto* tempo = find_by<QDoubleSpinBox>(inspector, [](QDoubleSpinBox* box) { return box->suffix() == " BPM"; });
        REQUIRE(advanced);
        REQUIRE(preview);
        REQUIRE(song);
        REQUIRE(tempo);
        CHECK(tab_distance(advanced, preview) > 0);
        CHECK(tab_distance(advanced, preview) < tab_distance(advanced, song)); // Transform before Song
        CHECK(tab_distance(song, tempo) == 1);                                  // header right before its fields
    }
}

TEST_SUITE("track_list") {
    TEST_CASE("Mute on another track does not switch the edited track; Delete deletes the active track") {
        auto controller = make_controller();
        ChangeBuilder builder(controller->document(), "second track");
        Track second;
        second.id = TrackId::generate();
        second.name = "Second";
        builder.append(TrackInsert{controller->active_sequence().id, 1, second});
        REQUIRE(controller->apply(builder.finish()));
        const TrackId first = controller->active_sequence().tracks[0].id;
        controller->set_active_track(first);
        TrackListWidget list(*controller);
        list.show();
        auto* table = list.findChild<QTableWidget*>();
        REQUIRE(table);
        table->setCurrentCell(1, 1); // the M cell of the second track
        CHECK(controller->active_track() == first);
        table->setCurrentCell(1, 0); // its name: now it is the edited track
        CHECK(controller->active_track() == second.id);
        table->setFocus();
        QTest::keyClick(table, Qt::Key_Delete);
        REQUIRE(controller->active_sequence().tracks.size() == 1);
        CHECK(controller->active_sequence().tracks[0].id == first);
    }

    TEST_CASE("a multi-sequence document shows a sequence selector") {
        DocumentData document = make_blank_document("Two sequences");
        Sequence other = document.sequences.front();
        other.id = SequenceId::generate();
        other.name = "Verse";
        for (auto& track : other.tracks) {
            track.id = TrackId::generate();
            for (auto& event : track.events)
                event.id = EventId::generate();
        }
        document.sequences.push_back(other);
        application();
        auto session = std::make_unique<document::WorkingSession>(document, document::SessionOrigin::new_document);
        DocumentController controller(std::move(session));
        TrackListWidget list(controller);
        list.show();
        auto* selector = list.findChild<QComboBox*>();
        REQUIRE(selector);
        CHECK(selector->isVisible());
        REQUIRE(selector->count() == 2);
        CHECK(selector->itemText(1).contains("Verse"));
        selector->setCurrentIndex(1);
        emit selector->activated(1);
        CHECK(controller.active_sequence_index() == 1);
    }
}

TEST_SUITE("window") {
    TEST_CASE("Delete in the track list deletes the track, not the selected notes") {
        WindowFixture fixture;
        auto* tab = fixture.window->current_tab();
        auto& controller = tab->controller();
        const NewNote note{controller.active_track(), 0, 480, 60, 100, 0, 0};
        REQUIRE(controller.apply(*insert_notes(controller.document(), std::span(&note, 1))));
        ChangeBuilder builder(controller.document(), "second track");
        Track second;
        second.id = TrackId::generate();
        second.name = "Second";
        builder.append(TrackInsert{controller.active_sequence().id, 1, second});
        REQUIRE(controller.apply(builder.finish()));
        controller.set_selection({controller.notes().notes[0].on_id});
        auto* table = tab->track_list().findChild<QTableWidget*>();
        REQUIRE(table);
        table->setCurrentCell(1, 0); // the second (empty) track is active
        table->setFocus(Qt::MouseFocusReason);
        fixture.key(Qt::Key_Delete);
        CHECK(controller.active_sequence().tracks.size() == 1);
        CHECK(controller.notes().notes.size() == 1); // the selected note is still there
    }
}

namespace {

/// A library panel over a fresh library holding `count` small songs; settings stay in the temp dir.
struct LibraryFixture {
    test::TemporaryDirectory directory;
    std::unique_ptr<LibraryPanel> panel;
    QSignalSpy* opened = nullptr;

    explicit LibraryFixture(int count) {
        application();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdWString(directory.path().wstring()));
        QSettings().remove("library");
        panel = std::make_unique<LibraryPanel>();
        panel->resize(420, 600);
        panel->show();
        opened = new QSignalSpy(panel.get(), &LibraryPanel::opened);
        panel->open_library(directory.path() / "lib", true, true);
        REQUIRE(wait_until([&] { return opened->count() > 0; }, 5000));
        for (int index = 0; index < count; ++index) {
            DocumentData document = make_blank_document("Song " + std::to_string(index));
            ChangeBuilder builder(document, "note");
            builder.add_note(document.sequences.front().tracks.front().id, 0, 480, 0,
                             static_cast<std::uint8_t>(60 + index), 100);
            REQUIRE(apply_change_set(document, builder.finish()));
            panel->save_document(std::make_shared<const DocumentData>(document), std::nullopt, std::nullopt,
                                 QString("Song %1").arg(index), false);
        }
        REQUIRE(wait_until([&] { return table()->model()->rowCount() == count; }, 5000));
    }
    ~LibraryFixture() {
        panel.reset();
        delete opened;
    }
    [[nodiscard]] QTableView* table() const { return panel->findChild<QTableView*>(); }
    [[nodiscard]] QString status() const {
        QString text;
        for (auto* label : panel->findChildren<QLabel*>())
            if (label->isVisible() && label->wordWrap())
                text += label->text() + "\\n";
        return text;
    }
};

/// Answers the next modal message box with `button` (QMessageBox::question/warning block).
void answer_next_message_box(QMessageBox::StandardButton button) {
    QTimer::singleShot(100, [button] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            box->button(button)->click();
    });
}

} // namespace

TEST_SUITE("library_ui") {
    TEST_CASE("the result table gives Title the room and hides secondary columns behind the header menu") {
        LibraryFixture fixture(2);
        auto* header = fixture.table()->horizontalHeader();
        CHECK(header->sectionResizeMode(0) == QHeaderView::Stretch);
        CHECK(header->isSectionHidden(1)); // Tags
        CHECK(header->isSectionHidden(6)); // Status
        CHECK_FALSE(header->isSectionHidden(3)); // BPM
        CHECK(fixture.table()->model()->data(fixture.table()->model()->index(0, 0), Qt::ToolTipRole).toString().contains("Song"));
        CHECK(fixture.opened->at(0).at(0).toBool()); // chosen now: interactive
    }

    TEST_CASE("trash works on every selected row at once, shows at once, and can be restored from the Trash view") {
        LibraryFixture fixture(3);
        auto* table = fixture.table();
        table->selectAll();
        table->selectionModel()->select(table->model()->index(2, 0), QItemSelectionModel::Deselect | QItemSelectionModel::Rows);
        answer_next_message_box(QMessageBox::Yes);
        fixture.panel->trash_selected();
        CHECK(table->model()->rowCount() == 1); // removed at once, before the write commits
        QTest::qWait(300);
        CHECK(table->model()->rowCount() == 1); // and the library agreed
        fixture.panel->show_trash(true);
        REQUIRE(wait_until([&] { return table->model()->rowCount() == 2; }, 3000));
        CHECK(fixture.status().contains("2 result"));
        table->selectAll();
        fixture.panel->restore_selected_from_trash();
        CHECK(table->model()->rowCount() == 0);
        fixture.panel->show_trash(false); // back to the search
        REQUIRE(wait_until([&] { return table->model()->rowCount() == 3; }, 3000));
    }

    TEST_CASE("an empty melody search says so and is not replaced by later refreshes") {
        LibraryFixture fixture(1);
        fixture.panel->search_melody({{0, 90}, {240, 91}, {480, 92}, {720, 93}}, {200, 200, 200, 200}, false);
        const bool said = wait_until([&] { return fixture.status().contains("No phrase matches"); }, 3000);
        INFO("status: " << fixture.status().toStdString());
        REQUIRE(said);
        CHECK_FALSE(fixture.status().contains("library is empty"));
        CHECK_FALSE(fixture.table()->horizontalHeader()->isSectionHidden(6)); // match details shown for melody hits
        CHECK(fixture.table()->horizontalHeader()->isSectionHidden(3)); // BPM, Key, Length make room for them
        CHECK(fixture.table()->horizontalHeader()->isSectionHidden(4));
        CHECK(fixture.table()->horizontalHeader()->isSectionHidden(5));
        // A save would have re-run the text search before; melody results stay.
        DocumentData document = make_blank_document("Later");
        fixture.panel->save_document(std::make_shared<const DocumentData>(document), std::nullopt, std::nullopt, "Later",
                                     false);
        QTest::qWait(500);
        CHECK(fixture.table()->model()->rowCount() == 0);
        auto* back = find_by<QPushButton>(*fixture.panel, [](QPushButton* button) { return button->text() == "Back to Search"; });
        REQUIRE(back);
        CHECK(back->isVisible());
        back->click();
        REQUIRE(wait_until([&] { return fixture.table()->model()->rowCount() == 2; }, 3000));
        CHECK(fixture.table()->horizontalHeader()->isSectionHidden(6)); // the user's column choice again
        CHECK_FALSE(fixture.table()->horizontalHeader()->isSectionHidden(3));
    }

    TEST_CASE("an open library builds its browse index in the background and searches then use it") {
        LibraryFixture fixture(3);
        auto library = fixture.panel->library();
        REQUIRE(library);
        REQUIRE(wait_until([&] { return library->browse_index_status().ready; }, 5000));
        CHECK(library->browse_index_status().rows == 3);
        library::SearchRequest request;
        auto page = library->search(request);
        REQUIRE(page.has_value());
        CHECK(page->from_browse_index);
        CHECK(page->rows.size() == 3);
    }

    TEST_CASE("a library reopened at startup does not count as chosen by the user") {
        LibraryFixture fixture(0);
        fixture.panel.reset(); // the app quits; the next start reopens the last library by itself
        LibraryPanel restarted;
        QSignalSpy reopened(&restarted, &LibraryPanel::opened);
        REQUIRE(wait_until([&] { return reopened.count() > 0; }, 3000));
        CHECK_FALSE(reopened.at(0).at(0).toBool());
    }
}

TEST_SUITE("library_ui") {
    TEST_CASE("a prepared drag file belongs to the revision it was made from") {
        application();
        LibraryResultModel model;
        LibraryRow row;
        row.asset_id = 7;
        row.title = "Riff";
        row.modified_ms = 100;
        model.set_rows({row});
        model.set_drag_file(7, 99, "C:/old.mid"); // prepared from an older revision: ignored
        CHECK_FALSE(model.flags(model.index(0, 0)).testFlag(Qt::ItemIsDragEnabled));
        model.set_drag_file(7, 100, "C:/riff.mid");
        CHECK(model.flags(model.index(0, 0)).testFlag(Qt::ItemIsDragEnabled));
        row.modified_ms = 200; // saved again: the file is stale
        model.replace_row(7, row);
        CHECK_FALSE(model.flags(model.index(0, 0)).testFlag(Qt::ItemIsDragEnabled));
        model.set_drag_file(7, 200, "C:/riff2.mid");
        model.clear_drag_files(); // another library opened
        CHECK_FALSE(model.flags(model.index(0, 0)).testFlag(Qt::ItemIsDragEnabled));
    }

    TEST_CASE("Insert at Edit Cursor pastes a library item's notes at the cursor") {
        auto controller = make_controller();
        DocumentData item = make_blank_document("Phrase");
        ChangeBuilder builder(item, "notes");
        const TrackId track = item.sequences.front().tracks.front().id;
        builder.add_note(track, 0, 240, 0, 60, 100);
        builder.add_note(track, 480, 720, 0, 64, 100);
        REQUIRE(apply_change_set(item, builder.finish()));
        std::unique_ptr<QMimeData> mime(make_document_mime(item));
        REQUIRE(mime);
        QString error;
        const auto created = paste_clipboard(*controller, mime.get(), 3840, &error);
        REQUIRE(created.size() == 2);
        const auto& notes = controller->notes().notes;
        CHECK(notes[0].start == 3840);
        CHECK(notes[1].start == 3840 + 480);
    }
}

// Reviewer probes, batch 4 (probe_batch4); the screenshot folder is the test's temporary workspace.

TEST_SUITE("reviewer_probe5") {
    TEST_CASE("probe5: toolbar overflow and screenshots at 1400, 1366, 1280 px") {
        WindowFixture fixture;
        REQUIRE(fixture.window->open_path(std::filesystem::path(__FILE__).parent_path().parent_path() / "docs" / "images" / "demo.mid"));
        QTest::qWait(300);
        for (const int width : {1400, 1366, 1280, 1200}) {
            fixture.window->resize(width, 860);
            QTest::qWait(200);
            bool overflow = false;
            for (auto* bar : fixture.window->findChildren<QToolBar*>())
                for (auto* button : bar->findChildren<QToolButton*>("qt_toolbar_ext_button"))
                    if (button->isVisible()) {
                        overflow = true;
                        MESSAGE("width " << width << ": overflow button visible on " << bar->objectName().toStdString());
                    }
            if (!overflow)
                MESSAGE("width " << width << ": no overflow");
            fixture.window->grab().save(QString::fromStdWString((fixture.directory.path() / "dark_").wstring()) + QString("%1.png").arg(width));
        }
    }
    TEST_CASE("probe5: destroying a focused standalone inspector does not abort") {
        auto controller = make_controller();
        {
            InspectorPanel inspector;
            inspector.set_controller(controller.get());
            inspector.show();
            QSpinBox* numerator = nullptr;
            for (auto* box : inspector.findChildren<QSpinBox*>())
                if (box->maximum() == 255)
                    numerator = box;
            REQUIRE(numerator);
            inspector.activateWindow();
            numerator->setFocus();
            numerator->stepBy(1);
            QTest::qWait(50);
        }
        CHECK(true);
    }
    TEST_CASE("probe5: Q applies the last quantize asynchronously from the roll") {
        WindowFixture fixture;
        auto* tab = fixture.window->current_tab();
        auto& controller = tab->controller();
        const NewNote note{controller.active_track(), 130, 400, 60, 100, 0, 0};
        auto changes = insert_notes(controller.document(), std::span(&note, 1));
        REQUIRE(changes);
        REQUIRE(controller.apply(*changes));
        controller.set_selection({controller.notes().notes.front().on_id});
        tab->piano_roll().setFocus();
        fixture.key(Qt::Key_Q);
        REQUIRE(wait_until([&] { return controller.notes().notes.front().start != 130; }, 3000));
        MESSAGE("quantized start " << controller.notes().notes.front().start << " undo " << controller.undo_text().toStdString());
        CHECK(controller.notes().notes.front().start % 240 == 0);
    }
}

TEST_SUITE("track_list") {
    TEST_CASE("Delete removes the highlighted track even when another track is the edited one") {
        auto controller = make_controller();
        ChangeBuilder builder(controller->document(), "second track");
        Track second;
        second.id = TrackId::generate();
        second.name = "Second";
        builder.append(TrackInsert{controller->active_sequence().id, 1, second});
        REQUIRE(controller->apply(builder.finish()));
        const TrackId first = controller->active_sequence().tracks[0].id;
        controller->set_active_track(first);
        TrackListWidget list(*controller);
        list.show();
        auto* table = list.findChild<QTableWidget*>();
        REQUIRE(table);
        table->setCurrentCell(1, 1); // M of the second track: highlighted, not edited
        CHECK(controller->active_track() == first);
        table->setFocus();
        QTest::keyClick(table, Qt::Key_Delete);
        REQUIRE(controller->active_sequence().tracks.size() == 1);
        CHECK(controller->active_sequence().tracks[0].id == first); // the highlighted one went
    }
}

TEST_SUITE("controller") {
    TEST_CASE("the shared snapshot is copied once per generation") {
        auto controller = make_controller();
        const auto first = controller->shared_snapshot();
        CHECK(controller->shared_snapshot() == first); // same generation: the same copy
        const NewNote note{controller->active_track(), 0, 480, 60, 100, 0, 0};
        REQUIRE(controller->apply(*insert_notes(controller->document(), std::span(&note, 1))));
        const auto second = controller->shared_snapshot();
        CHECK(second != first);
        CHECK(semantic_hash(*second) == semantic_hash(controller->document()));
        CHECK(semantic_hash(*first) != semantic_hash(*second)); // the old copy stays as it was
    }
}

TEST_SUITE("window") {
    TEST_CASE("Preview plays a library item without opening it; Space stops it") {
        WindowFixture fixture;
        auto* library = fixture.window->findChild<LibraryPanel*>();
        REQUIRE(library);
        DocumentData item = make_blank_document("Riff");
        ChangeBuilder builder(item, "note");
        builder.add_note(item.sequences.front().tracks.front().id, 0, 3840 * 8, 0, 60, 100);
        REQUIRE(apply_change_set(item, builder.finish()));
        auto* tabs = fixture.window->findChild<QTabWidget*>();
        REQUIRE(tabs);
        const int open_tabs = tabs->count();
        auto* play = fixture.window->findChild<QAction*>("transport.play_stop");
        REQUIRE(play);
        emit library->preview_requested(item, "Riff", std::nullopt);
        REQUIRE(wait_until([&] { return play->iconText() == "Stop"; }, 3000)); // the transport runs
        CHECK(tabs->count() == open_tabs);                                     // nothing was opened
        fixture.window->current_tab()->piano_roll().setFocus();
        fixture.key(Qt::Key_Space);
        CHECK(wait_until([&] { return play->iconText() == "Play"; }, 2000));
    }
}

// Reviewer probes, batch 5 (probe_batch5); the library screenshot probe runs only where the
// reviewer's library exists and saves into the test's temporary workspace.

namespace {

/// The reviewer's 12-file test set (probe_batch5/mkset.py), generated in the test's own folder:
/// varied tempos, a 3/4 waltz, a 76-character title, and the motif 0 2 4 5 7 5 4 2 in G plus the
/// same motif transposed to E. Each probe builds its own library from it, so probes never share
/// a library (and its writer lock) or depend on files outside the test (review_7 T3).
std::filesystem::path write_probe_library_sources(const std::filesystem::path& directory) {
    const auto folder = directory / "probe midi";
    std::filesystem::create_directories(folder);
    const std::vector<std::string> titles{
        "Bass groove in D minor take 3 with a very long descriptive title for elision", "Lead hook", "Pad chords C major",
        "Arp study 120", "Ballad melody", "Funk bass", "Waltz in 3-4", "Motif in G", "Motif in E (transposed)",
        "Drum-free sketch", "Chorale", "Riff 7"};
    const int motif[] = {0, 2, 4, 5, 7, 5, 4, 2};
    DeterministicRandom random(7);
    for (std::size_t index = 0; index < titles.size(); ++index) {
        const auto& title = titles[index];
        DocumentData document = make_blank_document(title);
        auto& sequence = document.sequences.front();
        const auto track = sequence.tracks.front().id;
        ChangeBuilder builder(document, "probe set");
        builder.insert(sequence.tracks.front().id,
                       events::tempo(0, static_cast<std::uint32_t>(60'000'000 / (90 + static_cast<int>(index) * 7))));
        builder.insert(sequence.tracks.front().id, events::time_signature(0, title.starts_with("Waltz") ? 3 : 4, 2));
        if (title.starts_with("Motif")) {
            const int base = title.find(" G") != std::string::npos ? 67 : 64;
            for (int k = 0; k < 8; ++k)
                builder.add_note(track, k * 240, k * 240 + 220, 0, static_cast<std::uint8_t>(base + motif[k]), 100);
        } else {
            const int base = 48 + static_cast<int>(index % 12);
            const int steps[] = {0, 3, 5, 7, 10, 12};
            for (int bar = 0; bar < 4; ++bar)
                for (int step = 0; step < 8; ++step)
                    if (random.bounded(4) != 0)
                        builder.add_note(track, bar * 1920 + step * 240, bar * 1920 + step * 240 + 200, 0,
                                         static_cast<std::uint8_t>(base + steps[random.bounded(6)]),
                                         static_cast<std::uint8_t>(60 + random.bounded(51)));
        }
        REQUIRE(apply_change_set(document, builder.finish()));
        const auto bytes = midi::export_midi(document, {});
        REQUIRE(bytes);
        std::ofstream file(folder / (title + ".mid"), std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
    }
    return folder;
}

/// Opens a fresh library in the test folder with the probe set imported and analyzed.
void open_probe_library(LibraryPanel& library, const std::filesystem::path& directory) {
    QSignalSpy messages(&library, &LibraryPanel::status_message);
    library.open_library(directory / "Test Library", true, true, write_probe_library_sources(directory));
    REQUIRE(wait_until([&] {
        for (const auto& message : messages)
            if (message.at(0).toString().startsWith("Imported"))
                return true; // the import task analyzes before it reports
        return false;
    }, 20000));
}

} // namespace

TEST_SUITE("reviewer_probe6") {
    TEST_CASE("probe6: where the focus is at startup") {
        WindowFixture fixture;
        QWidget* focus = QApplication::focusWidget();
        MESSAGE("startup focus: " << (focus ? focus->metaObject()->className() : "none") << " '"
                << (focus ? focus->accessibleName().toStdString() : "") << "' tooltip '"
                << (focus ? focus->toolTip().left(40).toStdString() : "") << "'");
        CHECK(qobject_cast<PianoRollView*>(focus) != nullptr);
    }
    TEST_CASE("probe6: library UI screenshots (CLI-made library), melody and trash modes") {
        WindowFixture fixture;
        auto* library = fixture.window->findChild<LibraryPanel*>();
        REQUIRE(library);
        open_probe_library(*library, fixture.directory.path());
        auto* table = library->findChild<QTableView*>();
        REQUIRE(table);
        REQUIRE(wait_until([&] { return table->model()->rowCount() >= 12; }, 8000));
        QTest::qWait(300);
        fixture.window->grab().save(QString::fromStdWString((fixture.directory.path() / "dark_library.png").wstring()));
        library->grab().save(QString::fromStdWString((fixture.directory.path() / "dark_library_panel.png").wstring()));
        MESSAGE("title width " << table->columnWidth(0) << " of viewport " << table->viewport()->width());
        // Melody search for the motif 0 2 4 5 7 (G major start).
        std::vector<std::pair<Tick, std::uint8_t>> onsets;
        std::vector<Tick> durations;
        const int motif[] = {0, 2, 4, 5, 7};
        for (int k = 0; k < 5; ++k) {
            onsets.emplace_back(k * 240, static_cast<std::uint8_t>(67 + motif[k]));
            durations.push_back(220);
        }
        library->search_melody(onsets, durations, false);
        QTest::qWait(1500);
        MESSAGE("melody rows " << table->model()->rowCount());
        for (int row = 0; row < table->model()->rowCount(); ++row)
            MESSAGE("  " << table->model()->index(row, 0).data().toString().toStdString() << " | status hidden: "
                    << table->horizontalHeader()->isSectionHidden(6) << " | tooltip: "
                    << table->model()->index(row, 0).data(Qt::ToolTipRole).toString().replace('\n', " / ").toStdString());
        library->grab().save(QString::fromStdWString((fixture.directory.path() / "dark_library_melody.png").wstring()));
        library->show_trash(true);
        QTest::qWait(800);
        library->grab().save(QString::fromStdWString((fixture.directory.path() / "dark_library_trash.png").wstring()));
        MESSAGE("trash rows " << table->model()->rowCount());
    }
    TEST_CASE("probe6: the readout during a library preview") {
        WindowFixture fixture;
        auto* library = fixture.window->findChild<LibraryPanel*>();
        REQUIRE(library);
        DocumentData item = make_blank_document("Riff");
        ChangeBuilder builder(item, "note");
        builder.add_note(item.sequences.front().tracks.front().id, 0, 3840 * 8, 0, 60, 100);
        REQUIRE(apply_change_set(item, builder.finish()));
        auto* label = fixture.window->findChild<QLabel*>("transport.position");
        REQUIRE(label);
        const QString before = label->text();
        emit library->preview_requested(item, "Riff", std::nullopt);
        QTest::qWait(1200);
        MESSAGE("readout before '" << before.toStdString() << "' during '" << label->text().toStdString() << "'");
        CHECK(label->text() != before);
        fixture.window->current_tab()->piano_roll().setFocus();
        fixture.key(Qt::Key_Space);
    }
}

TEST_SUITE("reviewer_probe6") {
    TEST_CASE("probe6: Down right after startup") {
        WindowFixture fixture;
        auto& controller = fixture.window->current_tab()->controller();
        const auto before = controller.active_sequence().settings.default_note_length;
        const auto generation = controller.generation();
        fixture.key(Qt::Key_Down);
        QTest::qWait(100);
        MESSAGE("default length " << before.to_string() << " -> " << controller.active_sequence().settings.default_note_length.to_string()
                << ", generation " << generation << " -> " << controller.generation() << ", undo '" << controller.undo_text().toStdString() << "'");
        CHECK(controller.generation() == generation);
    }
}

TEST_SUITE("window") {
    TEST_CASE("the piano roll has the keyboard at startup and after opening a file") {
        WindowFixture fixture;
        CHECK(qobject_cast<PianoRollView*>(QApplication::focusWidget()) != nullptr);
        REQUIRE(fixture.window->open_path(std::filesystem::path(__FILE__).parent_path().parent_path() / "docs" / "images" /
                                          "demo.mid"));
        REQUIRE(wait_until([&] { return fixture.window->current_tab()->controller().notes().notes.size() > 0; }, 5000));
        QTest::qWait(100);
        CHECK(QApplication::focusWidget() == &fixture.window->current_tab()->piano_roll());
        auto& controller = fixture.window->current_tab()->controller();
        const auto generation = controller.generation();
        fixture.key(Qt::Key_Down); // no selection: never an inspector setting
        CHECK(controller.generation() == generation);
    }

    TEST_CASE("the readout follows a library preview") {
        WindowFixture fixture;
        auto* library = fixture.window->findChild<LibraryPanel*>();
        auto* label = fixture.window->findChild<QLabel*>("transport.position");
        REQUIRE(library);
        REQUIRE(label);
        DocumentData item = make_blank_document("Riff");
        ChangeBuilder builder(item, "tempo and note");
        builder.add_note(item.sequences.front().tracks.front().id, 0, 3840 * 8, 0, 60, 100);
        REQUIRE(apply_change_set(item, builder.finish()));
        emit library->preview_requested(item, "Riff", std::nullopt);
        REQUIRE(wait_until([&] { return label->text().contains("Play") && !label->text().startsWith("1.1.000"); }, 3000));
        CHECK(label->toolTip().contains("Riff"));
        fixture.window->current_tab()->piano_roll().setFocus();
        fixture.key(Qt::Key_Space);
        CHECK(wait_until([&] { return label->text().contains("Stop"); }, 2000));
    }
}

TEST_SUITE("reviewer_probe7") {
    TEST_CASE("probe7: preview readout and melody Status column, with screenshots") {
        WindowFixture fixture;
        auto* library = fixture.window->findChild<LibraryPanel*>();
        REQUIRE(library);
        open_probe_library(*library, fixture.directory.path());
        auto* table = library->findChild<QTableView*>();
        REQUIRE(table);
        REQUIRE(wait_until([&] { return table->model()->rowCount() >= 12; }, 8000));
        std::vector<std::pair<Tick, std::uint8_t>> onsets;
        std::vector<Tick> durations;
        const int motif[] = {0, 2, 4, 5, 7};
        for (int k = 0; k < 5; ++k) {
            onsets.emplace_back(k * 240, static_cast<std::uint8_t>(67 + motif[k]));
            durations.push_back(220);
        }
        library->search_melody(onsets, durations, false);
        REQUIRE(wait_until([&] { return table->model()->rowCount() == 2; }, 5000));
        QTest::qWait(300);
        CHECK_FALSE(table->horizontalHeader()->isSectionHidden(6));
        MESSAGE("status of row 0: " << table->model()->index(0, 6).data().toString().toStdString());
        fixture.window->grab().save(QString::fromStdWString((fixture.directory.path() / "melody_window.png").wstring()));
        // Preview the first hit (the melody match plays with a lead-in).
        table->setCurrentIndex(table->model()->index(0, 0));
        table->setFocus();
        QTest::keyClick(table, Qt::Key_Return, Qt::ControlModifier); // Ctrl+Enter = Preview
        auto* label = fixture.window->findChild<QLabel*>("transport.position");
        REQUIRE(label);
        const bool moved = wait_until([&] { return label->text().contains("Play") && !label->text().startsWith("1.1.000"); }, 4000);
        MESSAGE("readout during preview: '" << label->text().toStdString() << "' tooltip '" << label->toolTip().toStdString() << "'");
        CHECK(moved);
        fixture.window->grab().save(QString::fromStdWString((fixture.directory.path() / "preview_window.png").wstring()));
        // Back to Search hides Status again (saved choice).
        for (auto* button : library->findChildren<QPushButton*>())
            if (button->text() == "Back to Search")
                button->click();
        QTest::qWait(500);
        CHECK(table->horizontalHeader()->isSectionHidden(6));
        // Space stops the preview; the readout says Stop.
        fixture.window->current_tab()->piano_roll().setFocus();
        fixture.key(Qt::Key_Space);
        QTest::qWait(300);
        MESSAGE("readout after stop: '" << label->text().toStdString() << "'");
    }
}

TEST_SUITE("library_ui") {
    TEST_CASE("an added folder is watched: edits create revisions, deletions mark the source missing (code-runtime-15)") {
        application();
        test::TemporaryDirectory directory;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdWString(directory.path().wstring()));
        QSettings().remove("library");
        const auto write_phrase = [](const std::filesystem::path& path, std::uint8_t first_pitch) {
            DocumentData document = make_blank_document("phrase");
            ChangeBuilder builder(document, "notes");
            for (std::uint8_t index = 0; index < 4; ++index)
                builder.add_note(document.sequences.front().tracks.front().id, index * 480, index * 480 + 400, 0,
                                 static_cast<std::uint8_t>(first_pitch + index), 100);
            REQUIRE(apply_change_set(document, builder.finish()));
            const auto bytes = midi::export_midi(document, {});
            REQUIRE(bytes);
            std::filesystem::create_directories(path.parent_path());
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
        };
        const auto folder = directory.path() / "watched";
        write_phrase(folder / "a.mid", 60);
        write_phrase(folder / "b.mid", 48);
        LibraryPanel panel;
        panel.show();
        QSignalSpy messages(&panel, &LibraryPanel::status_message);
        const auto said = [&](const QString& prefix) {
            for (const auto& message : messages)
                if (message.at(0).toString().startsWith(prefix))
                    return true;
            return false;
        };
        panel.open_library(directory.path() / "lib", true, true, folder);
        // The folder is watched from the start of its import; edits are made once it finished.
        REQUIRE(wait_until([&] { return said("Imported") && panel.watched_folders().size() == 1; }, 10000));
        auto library = panel.library();
        REQUIRE(library);
        const auto asset_of = [&](const std::string& name) -> std::int64_t {
            library::SearchRequest request;
            request.query_text = "source:" + name;
            auto page = library->search(request);
            return page && page->rows.size() == 1 ? page->rows.front().id : 0;
        };
        REQUIRE(wait_until([&] { return asset_of("a") != 0 && asset_of("b") != 0; }, 5000));
        const auto a = asset_of("a");
        const auto b = asset_of("b");
        // Edited in another program: a new revision of the same item, no new item.
        write_phrase(folder / "b.mid", 72);
        CHECK(wait_until([&] { return library->revisions(b) && library->revisions(b)->size() == 2; }, 10000));
        CHECK(wait_until([&] { return said("Watched folders"); }, 5000)); // the user is told what changed

        // Deleted: the item stays, its source is reported missing.
        std::filesystem::remove(folder / "a.mid");
        CHECK(wait_until([&] {
            auto rows = library->browse_rows_for({a});
            return rows && rows->size() == 1 && rows->front().source_missing;
        }, 10000));
        CHECK_FALSE(library->browse_rows_for({a})->front().deleted);
        CHECK(library->source_roots()->size() == 1); // updates went to the folder's own locations
    }
}

// --- Window-level data-loss, recording and library paths (code-uiA-12) ---------------------------

namespace {

/// Answers the modal message boxes that appear, one button each, in order (a prompt, then the
/// error it may lead to). Stops when the list is used up.
void answer_message_boxes(std::vector<QMessageBox::StandardButton> buttons) {
    auto queue = std::make_shared<std::deque<QMessageBox::StandardButton>>(buttons.begin(), buttons.end());
    auto* timer = new QTimer(qApp);
    timer->setInterval(50);
    QObject::connect(timer, &QTimer::timeout, timer, [queue, timer] {
        if (queue->empty()) {
            timer->deleteLater();
            return;
        }
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box || !box->isVisible())
            return;
        auto* button = box->button(queue->front());
        queue->pop_front();
        if (button)
            button->click();
    });
    timer->start();
}

/// Adds one note to the current tab's first track (a real, undoable edit).
void edit_current(MainWindow& window, std::uint8_t pitch) {
    auto* tab = window.current_tab();
    REQUIRE(tab);
    ChangeBuilder builder(tab->controller().document(), "note");
    builder.add_note(tab->controller().document().sequences.front().tracks.front().id, 0, 480, 0, pitch, 100);
    REQUIRE(tab->controller().apply(builder.finish()));
}

std::size_t note_count(const DocumentData& document) { return pair_notes(document.sequences.front()).notes.size(); }

} // namespace

TEST_SUITE("window") {
    TEST_CASE("closing a dirty document: Cancel keeps it, a failed save keeps it, Save writes it (code-uiA-12)") {
        WindowFixture fixture;
        const auto path = fixture.directory.path() / "close me.mwb";
        REQUIRE(document::save_native(make_blank_document("Close me"), path));
        REQUIRE(fixture.window->open_path(path));
        REQUIRE(wait_until([&] {
            auto* tab = fixture.window->current_tab();
            return tab && tab->controller().file_path() == path;
        }, 5000));
        QPointer<EditorTab> tab = fixture.window->current_tab();
        auto* tabs = fixture.window->findChild<QTabWidget*>();
        REQUIRE(tabs);
        edit_current(*fixture.window, 60);
        REQUIRE(tab->controller().is_dirty());

        answer_message_boxes({QMessageBox::Cancel});
        emit tabs->tabCloseRequested(tabs->indexOf(tab));
        QTest::qWait(100);
        REQUIRE(tab);
        CHECK(tabs->indexOf(tab) >= 0);
        CHECK(tab->controller().is_dirty());

#ifdef _WIN32
        // The file cannot be written: the user is told, and the document stays open and dirty.
        std::filesystem::permissions(path, std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                                               std::filesystem::perms::others_write,
                                     std::filesystem::perm_options::remove);
        answer_message_boxes({QMessageBox::Save, QMessageBox::Ok});
        emit tabs->tabCloseRequested(tabs->indexOf(tab));
        QTest::qWait(200);
        std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
        REQUIRE(tab);
        CHECK(tabs->indexOf(tab) >= 0);
        CHECK(tab->controller().is_dirty());
        CHECK(note_count(document::load_native(path).value()) == 0); // the file was not touched
#endif

        answer_message_boxes({QMessageBox::Save});
        emit tabs->tabCloseRequested(tabs->indexOf(tab));
        REQUIRE(wait_until([&] { return tab.isNull(); }, 5000));
        const auto saved = document::load_native(path);
        REQUIRE(saved);
        CHECK(note_count(*saved) == 1);
        CHECK(fixture.window->current_tab() != nullptr); // the window never ends up without a document
    }

    TEST_CASE("Discard closes without writing and removes the document's recovery session (code-uiA-12)") {
        WindowFixture fixture;
        const auto path = fixture.directory.path() / "discard.mwb";
        REQUIRE(document::save_native(make_blank_document("Discard me"), path));
        REQUIRE(fixture.window->open_path(path));
        REQUIRE(wait_until([&] {
            auto* tab = fixture.window->current_tab();
            return tab && tab->controller().file_path() == path && tab->controller().recovery_journal();
        }, 5000));
        QPointer<EditorTab> tab = fixture.window->current_tab();
        edit_current(*fixture.window, 64);
        const auto session = tab->controller().recovery_journal()->session_id();
        const auto recovery = app::resolve_data_directories(fixture.directory.path()).recovery;
        auto* tabs = fixture.window->findChild<QTabWidget*>();
        answer_message_boxes({QMessageBox::Discard});
        emit tabs->tabCloseRequested(tabs->indexOf(tab));
        REQUIRE(wait_until([&] { return tab.isNull(); }, 5000));
        CHECK(note_count(document::load_native(path).value()) == 0);
        // Closing is a clean close: the session is not offered for recovery next time.
        CHECK(wait_until([&] {
            auto sessions = document::list_recoverable_sessions(recovery);
            return sessions && std::ranges::none_of(*sessions, [&](const auto& listed) { return listed.session_id == session; });
        }, 5000));
    }

    TEST_CASE("closing while a background save runs waits for it and does not ask again (code-uiA-12)") {
        WindowFixture fixture;
        const auto path = fixture.directory.path() / "saving.mwb";
        REQUIRE(document::save_native(make_blank_document("Saving"), path));
        REQUIRE(fixture.window->open_path(path));
        REQUIRE(wait_until([&] {
            auto* tab = fixture.window->current_tab();
            return tab && tab->controller().file_path() == path;
        }, 5000));
        QPointer<EditorTab> tab = fixture.window->current_tab();
        edit_current(*fixture.window, 67);
        tab->controller().save_native_async(path);
        bool asked = false;
        QTimer watch;
        QObject::connect(&watch, &QTimer::timeout, [&] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                asked = true;
                box->button(QMessageBox::Cancel)->click();
            }
        });
        watch.start(20);
        auto* tabs = fixture.window->findChild<QTabWidget*>();
        emit tabs->tabCloseRequested(tabs->indexOf(tab));
        REQUIRE(wait_until([&] { return tab.isNull(); }, 5000));
        watch.stop();
        CHECK_FALSE(asked); // the save in flight made it clean before the question
        CHECK(note_count(document::load_native(path).value()) == 1);
    }

    TEST_CASE("closing a document while recording ends the take first, so the prompt covers it (code-uiA-12)") {
        WindowFixture fixture;
        auto* recording = fixture.window->findChild<RecordingController*>();
        REQUIRE(recording);
        auto* record = fixture.window->findChild<QAction*>("transport.record");
        REQUIRE(record);
        QPointer<EditorTab> tab = fixture.window->current_tab();
        REQUIRE(tab);
        edit_current(*fixture.window, 60); // unsaved work: closing must ask
        record->trigger();
        if (!recording->recording()) {
            MESSAGE("recording could not start here (no audio device); skipped");
            return;
        }
        auto* tabs = fixture.window->findChild<QTabWidget*>();
        answer_message_boxes({QMessageBox::Cancel});
        emit tabs->tabCloseRequested(tabs->indexOf(tab));
        QTest::qWait(200);
        CHECK_FALSE(recording->recording()); // never recording into a document that is closing
        CHECK(tab);                          // Cancel keeps it open, with the take in its history
    }
}

namespace {
int tabs_titled(MainWindow& window, const std::string& title) {
    int count = 0;
    if (auto* tabs = window.findChild<QTabWidget*>())
        for (int index = 0; index < tabs->count(); ++index)
            if (auto* tab = qobject_cast<EditorTab*>(tabs->widget(index)); tab && tab->controller().document().title == title)
                ++count;
    return count;
}
} // namespace

TEST_SUITE("window") {
    TEST_CASE("crashed work is offered: Recover opens it as a new document, Discard deletes it after asking (code-uiA-12)") {
        WindowFixture fixture;
        const auto recovery = app::resolve_data_directories(fixture.directory.path()).recovery;
        const auto crash = [&](const std::string& title, int notes) {
            DocumentData document = make_blank_document(title);
            ChangeBuilder builder(document, "notes");
            for (int index = 0; index < notes; ++index)
                builder.add_note(document.sequences.front().tracks.front().id, index * 480, index * 480 + 400, 0,
                                 static_cast<std::uint8_t>(60 + index), 100);
            REQUIRE(apply_change_set(document, builder.finish()));
            auto journal = document::RecoveryJournal::create(recovery, document, document::SourceReference{title, {}, {}, {}});
            REQUIRE(journal);
            REQUIRE((*journal)->flush(true));
            return (*journal)->session_id(); // destroyed without a clean close: a crash
        };
        const auto pick = [](const QString& button_text, bool confirm_discard) {
            QTimer::singleShot(200, [button_text, confirm_discard] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog)
                    return; // the wait below then fails with a clear message
                if (confirm_discard)
                    answer_message_boxes({QMessageBox::Discard});
                for (auto* button : dialog->findChildren<QPushButton*>())
                    if (button->text() == button_text)
                        button->click();
            });
        };
        const auto recovered_session = crash("Crashed song", 3);
        // A session saved after its last edit holds nothing the file lacks: it is not offered,
        // and its data is removed (code-document-16).
        Uuid saved_session;
        {
            document::WorkingSession session(make_blank_document("Saved song"), document::SessionOrigin::opened_file);
            auto created = document::RecoveryJournal::create(recovery, session.document(),
                                                             document::SourceReference{"Saved song", {}, {}, {}});
            REQUIRE(created);
            std::shared_ptr<document::RecoveryJournal> journal = std::move(*created);
            std::ignore = document::connect_recovery(session, journal);
            ChangeBuilder builder(session.document(), "note");
            builder.add_note(session.document().sequences.front().tracks.front().id, 0, 480, 0, 60, 100);
            REQUIRE(session.apply(builder.finish()));
            journal->mark_saved(1234, session.generation());
            REQUIRE(journal->flush(true));
            saved_session = journal->session_id();
        } // crash after the save
        auto sessions = document::list_recoverable_sessions(recovery);
        REQUIRE(sessions);
        REQUIRE(sessions->size() == 2);
        pick("Recover Checked", false);
        fixture.window->present_recovery(*sessions);
        REQUIRE(wait_until([&] {
            auto* tab = fixture.window->current_tab();
            return tab && tab->controller().document().title == "Crashed song";
        }, 5000));
        auto* tab = fixture.window->current_tab();
        CHECK(note_count(tab->controller().document()) == 3);
        CHECK(tab->controller().is_dirty());          // a new unsaved document, never the saved file
        CHECK(tab->controller().file_path().empty());
        CHECK(tabs_titled(*fixture.window, "Saved song") == 0);
        CHECK(wait_until([&] {
            auto listed = document::list_recoverable_sessions(recovery);
            return listed && std::ranges::none_of(*listed, [&](const auto& item) { return item.session_id == saved_session; });
        }, 5000));
        // Its evidence is replaced by the new document's own journal.
        CHECK(wait_until([&] {
            auto listed = document::list_recoverable_sessions(recovery);
            return listed && std::ranges::none_of(*listed, [&](const auto& item) { return item.session_id == recovered_session; });
        }, 5000));

        const auto discarded_session = crash("Throw away", 1);
        sessions = document::list_recoverable_sessions(recovery);
        REQUIRE(sessions);
        std::erase_if(*sessions, [&](const auto& item) { return item.session_id != discarded_session; });
        REQUIRE(sessions->size() == 1);
        pick("Discard Checked", true);
        fixture.window->present_recovery(*sessions);
        CHECK(wait_until([&] {
            auto listed = document::list_recoverable_sessions(recovery);
            return listed && std::ranges::none_of(*listed, [&](const auto& item) { return item.session_id == discarded_session; });
        }, 5000));
        CHECK(fixture.window->current_tab()->controller().document().title == "Crashed song"); // nothing opened
    }
}

TEST_SUITE("library_ui") {
    TEST_CASE("an item added elsewhere is announced without moving the current row (code-uiA-12)") {
        LibraryFixture fixture(3);
        auto* table = fixture.table();
        table->setCurrentIndex(table->model()->index(1, 0));
        const auto current = table->model()->index(1, 0).data().toString();
        auto library = fixture.panel->library();
        REQUIRE(library);
        QTest::qWait(2500); // the first change poll has learned where the outbox stands
        // Another writer (the CLI, another window) adds an item.
        DocumentData document = make_blank_document("From elsewhere");
        REQUIRE(library->create_asset(document, "From elsewhere", library::AssetKind::phrase).has_value());
        REQUIRE(wait_until([&] { return fixture.status().contains("not in this list yet"); }, 6000));
        CHECK(table->model()->rowCount() == 3);
        CHECK(table->currentIndex().row() == 1);
        CHECK(table->currentIndex().data().toString() == current);
    }
}

TEST_SUITE("window") {
    TEST_CASE("an open document is shown again instead of opened twice (code-uiA-28)") {
        WindowFixture fixture;
        const auto path = fixture.directory.path() / "once.mwb";
        REQUIRE(document::save_native(make_blank_document("Once"), path));
        auto* tabs = fixture.window->findChild<QTabWidget*>();
        REQUIRE(fixture.window->open_path(path));
        REQUIRE(wait_until([&] {
            auto* tab = fixture.window->current_tab();
            return tab && tab->controller().file_path() == path;
        }, 5000));
        const int count = tabs->count();
        fixture.window->new_from_template("blank"); // another tab is current now
        QTest::qWait(100);
        REQUIRE(fixture.window->open_path(fixture.directory.path() / ".." / fixture.directory.path().filename() / "once.mwb"));
        QTest::qWait(300);
        CHECK(tabs->count() == count + 1); // only the blank was added
        CHECK(fixture.window->current_tab()->controller().file_path() == path);
    }
}

TEST_SUITE("keyboard") {
    TEST_CASE("releasing the mouse leaves a note another live input still holds (uiB-41)") {
        application();
        KeyboardWidget keyboard;
        keyboard.resize(700, 80);
        keyboard.show();
        QTest::qWait(20);
        QSignalSpy on(&keyboard, &KeyboardWidget::note_on);
        QSignalSpy off(&keyboard, &KeyboardWidget::note_off);
        const QPoint point(100, keyboard.height() - 6); // the lower half of a white key
        QTest::mousePress(&keyboard, Qt::LeftButton, Qt::NoModifier, point);
        REQUIRE(on.count() == 1);
        const int pitch = on.at(0).at(0).toInt();
        keyboard.set_key_state(pitch, KeySource::computer_keyboard, true); // also held on the computer keyboard
        QTest::mouseRelease(&keyboard, Qt::LeftButton, Qt::NoModifier, point);
        CHECK(off.isEmpty()); // still sounding for the computer keyboard
        keyboard.set_key_state(pitch, KeySource::computer_keyboard, false);
        keyboard.set_key_state(pitch, KeySource::playback, true); // playback only lights the key
        QTest::mousePress(&keyboard, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseRelease(&keyboard, Qt::LeftButton, Qt::NoModifier, point);
        REQUIRE(off.count() == 1);
        CHECK(off.at(0).at(0).toInt() == pitch);
    }
}

TEST_SUITE("inspector") {
    TEST_CASE("a mixed-velocity selection shows \"mixed\" and can be set to the first note's value (uiB-33)") {
        auto controller = make_controller();
        const std::array<NewNote, 2> notes{NewNote{controller->active_track(), 0, 480, 60, 80, 0, 0},
                                          NewNote{controller->active_track(), 480, 960, 62, 100, 0, 0}};
        auto changes = insert_notes(controller->document(), notes);
        REQUIRE(changes);
        REQUIRE(controller->apply(*changes));
        std::set<EventId> all;
        for (const auto& note : controller->notes().notes)
            all.insert(note.on_id);
        controller->set_selection(all);
        InspectorPanel inspector;
        inspector.set_controller(controller.get());
        inspector.show();
        QTest::qWait(20);
        QSpinBox* velocity = nullptr;
        for (auto* box : inspector.findChildren<QSpinBox*>())
            if (box->specialValueText() == "mixed")
                velocity = box;
        REQUIRE(velocity);
        CHECK(velocity->value() == 0); // shown as "mixed"
        velocity->setValue(80);         // the first note's velocity
        emit velocity->editingFinished();
        QTest::qWait(20);
        for (const auto& note : controller->notes().notes)
            CHECK(note.velocity == 80);
        CHECK(velocity->value() == 80); // all the same now
    }
}

TEST_SUITE("track_list") {
    TEST_CASE("note edits update the track list in place, without recreating its items (uiB-35)") {
        auto controller = make_controller();
        TrackListWidget list(*controller);
        list.show();
        auto* table = list.findChild<QTableWidget*>();
        REQUIRE(table);
        auto* name = table->item(0, 0);
        auto* mute = table->item(0, 1);
        REQUIRE(name);
        const NewNote note{controller->active_track(), 0, 480, 60, 100, 0, 0};
        auto changes = insert_notes(controller->document(), std::span(&note, 1));
        REQUIRE(changes);
        REQUIRE(controller->apply(*changes));
        CHECK(table->item(0, 0) == name); // the same items, updated where something changed
        CHECK(table->item(0, 1) == mute);
        // A rename through the table still works and shows the new name.
        name->setText("Lead");
        CHECK(controller->active_sequence().tracks.front().name == "Lead");
        CHECK(table->item(0, 0)->text() == "Lead");
    }
}

TEST_SUITE("drum_grid") {
    TEST_CASE("new drum steps use the sequence's new-note velocity (uiB-19)") {
        auto controller = make_controller();
        {
            ChangeBuilder builder(controller->document(), "velocity");
            Sequence updated = sequence_header(controller->active_sequence());
            updated.settings.default_velocity = 77;
            builder.set_sequence_properties(updated.id, updated);
            REQUIRE(controller->apply(builder.finish()));
        }
        DrumGridView grid(*controller);
        grid.resize(800, 400);
        grid.show();
        grid.setFocus();
        QTest::qWait(20);
        QSignalSpy heard(&grid, &DrumGridView::audition_note_on);
        QSignalSpy released(&grid, &DrumGridView::audition_note_off);
        QTest::keyClick(&grid, Qt::Key_Return); // toggles the cell under the cursor
        const auto notes = controller->notes().notes;
        REQUIRE(notes.size() == 1);
        CHECK(notes.front().velocity == 77);
        CHECK(heard.count() == 1); // the new hit is heard (ux-editing-21)
        CHECK(wait_until([&] { return released.count() == 1; }, 1000));
    }
}

TEST_SUITE("window") {
    TEST_CASE("keyboard: Alt+Left/Right step through notes, Shift+Left/Right change their length (ux-editing-26)") {
        WindowFixture fixture;
        auto* tab = fixture.window->current_tab();
        REQUIRE(tab);
        auto& controller = tab->controller();
        const std::array<NewNote, 3> notes{NewNote{controller.active_track(), 0, 240, 60, 100, 0, 0},
                                          NewNote{controller.active_track(), 480, 720, 64, 100, 0, 0},
                                          NewNote{controller.active_track(), 960, 1200, 67, 100, 0, 0}};
        auto changes = insert_notes(controller.document(), notes);
        REQUIRE(changes);
        REQUIRE(controller.apply(*changes));
        controller.set_edit_cursor(0);
        tab->piano_roll().setFocus();
        const auto selected_start = [&] {
            const auto selected = controller.selected_notes();
            return selected.size() == 1 ? selected.front().start : Tick{-1};
        };
        fixture.key(Qt::Key_Right, Qt::AltModifier);
        CHECK(selected_start() == 0);
        fixture.key(Qt::Key_Right, Qt::AltModifier);
        CHECK(selected_start() == 480);
        fixture.key(Qt::Key_Left, Qt::AltModifier);
        CHECK(selected_start() == 0);
        const auto length = [&] { return controller.selected_notes().front().duration(); };
        const Tick before = length();
        fixture.key(Qt::Key_Right, Qt::ShiftModifier);
        CHECK(length() > before);
        fixture.key(Qt::Key_Left, Qt::ShiftModifier);
        CHECK(length() == before);
        for (int index = 0; index < 20; ++index)
            fixture.key(Qt::Key_Left, Qt::ShiftModifier);
        CHECK(length() >= 1); // never below one tick
    }
}

TEST_SUITE("inspector") {
    TEST_CASE("the transform form keeps its values across tab switches and after Q (ux-panels-22/23)") {
        auto first = make_controller();
        auto second = make_controller();
        const NewNote note{first->active_track(), 0, 480, 60, 100, 0, 0};
        auto changes = insert_notes(first->document(), std::span(&note, 1));
        REQUIRE(changes);
        REQUIRE(first->apply(*changes));
        InspectorPanel inspector;
        inspector.set_controller(first.get());
        inspector.show();
        inspector.focus_tool("transpose");
        QSpinBox* amount = nullptr;
        for (auto* box : inspector.findChildren<QSpinBox*>())
            if (box->minimum() == -100000)
                amount = box;
        REQUIRE(amount);
        amount->setValue(7); // the user's value
        auto* json = inspector.findChild<QPlainTextEdit*>();
        REQUIRE(json);
        const QString parameters = json->toPlainText();
        inspector.set_controller(second.get()); // another tab
        inspector.set_controller(first.get());  // and back
        CHECK(amount->value() == 7);
        std::set<EventId> all;
        for (const auto& existing : first->notes().notes)
            all.insert(existing.on_id);
        first->set_selection(all);
        inspector.apply_last_quantize(); // Q from the Transpose tool
        QTest::qWait(300);
        CHECK(amount->value() == 7);
        CHECK(json->toPlainText() == parameters); // form and JSON agree again
    }
}

TEST_SUITE("track_list") {
    TEST_CASE("a track cannot be renamed to nothing; drag files avoid reserved device names (ux-panels-44/45)") {
        auto controller = make_controller();
        TrackListWidget list(*controller);
        list.show();
        auto* table = list.findChild<QTableWidget*>();
        REQUIRE(table);
        const auto before = controller->active_sequence().tracks.front().name;
        table->item(0, 0)->setText("   ");
        CHECK(controller->active_sequence().tracks.front().name == before);
        CHECK(table->item(0, 0)->text() == QString::fromStdString(before));
        CHECK_FALSE(controller->can_undo());

        test::TemporaryDirectory directory;
        const std::vector<std::uint8_t> bytes{'M', 'T', 'h', 'd'};
        for (const auto* title : {"CON", "com1", "Nul.v2"}) {
            CAPTURE(title);
            auto written = write_drag_file(directory.path(), title, bytes);
            REQUIRE_MESSAGE(written, (written ? "" : written.error().describe()));
            CHECK(std::filesystem::exists(*written));
            CHECK(written->stem().string().find('_') != std::string::npos);
        }
    }
}

TEST_SUITE("window") {
    TEST_CASE("Space plays while a number field has the focus (ux-panels-20)") {
        WindowFixture fixture;
        auto* tempo = fixture.find<QDoubleSpinBox>([](QDoubleSpinBox* box) { return box->accessibleName() == "Tempo"; });
        REQUIRE(tempo);
        tempo->setFocus();
        QTest::qWait(20);
        const double before = tempo->value();
        fixture.key(Qt::Key_Space);
        CHECK(wait_until([&] { return fixture.transport() == TransportDisplay::State::playing; }, 3000));
        CHECK(tempo->value() == before); // the field did not take the key
        fixture.key(Qt::Key_Space);
        CHECK(wait_until([&] { return fixture.transport() == TransportDisplay::State::stopped; }, 3000));
    }
}

TEST_SUITE("window") {
    TEST_CASE("the window title names the current document and its unsaved state (ux-visual-21)") {
        WindowFixture fixture;
        auto* tab = fixture.window->current_tab();
        REQUIRE(tab);
        const QString title = QString::fromStdString(tab->controller().document().title);
        CHECK(fixture.window->windowTitle().contains(title));
        CHECK_FALSE(fixture.window->isWindowModified());
        edit_current(*fixture.window, 60);
        CHECK(fixture.window->isWindowModified());
        auto* tabs = fixture.window->findChild<QTabWidget*>();
        CHECK(tabs->tabToolTip(tabs->currentIndex()).contains(title));
    }
}

TEST_SUITE("theme") {
    TEST_CASE("the Drag MIDI label is readable in every theme (final review N1)") {
        application();
        for (const Theme theme : {Theme::dark, Theme::light, Theme::high_contrast}) {
            CAPTURE(static_cast<int>(theme));
            apply_theme(theme);
            test::TemporaryDirectory directory;
            DragHandle handle(directory.path());
            handle.resize(handle.sizeHint() + QSize(20, 8));
            QImage image(handle.size(), QImage::Format_ARGB32);
            handle.render(&image);
            // The handle's own background, inside the border, beside the label.
            const QColor background = image.pixelColor(4, image.height() / 2);
            double best = 1.0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 6; x < image.width() - 6; ++x)
                    best = std::max(best, contrast(image.pixelColor(x, y), background));
            CHECK(best >= 4.5); // the text's strongest pixels against the fill
            CHECK(contrast(handle.palette().color(QPalette::ButtonText), handle.palette().color(QPalette::Button)) >= 4.5);
        }
        apply_theme(Theme::dark);
    }
}

TEST_SUITE("window") {
    TEST_CASE("a close waiting for its save cannot be started again (final review N2)") {
        WindowFixture fixture;
        const auto path = fixture.directory.path() / "slow.mwb";
        REQUIRE(document::save_native(make_blank_document("Slow"), path));
        REQUIRE(fixture.window->open_path(path));
        REQUIRE(wait_until([&] {
            auto* tab = fixture.window->current_tab();
            return tab && tab->controller().file_path() == path;
        }, 5000));
        QPointer<EditorTab> tab = fixture.window->current_tab();
        edit_current(*fixture.window, 61);
        tab->controller().save_native_async(path);
        auto* tabs = fixture.window->findChild<QTabWidget*>();
        const int count = tabs->count();
        int reentered = 0;
        bool still_open_inside = false;
        bool window_still_open = false;
        // Fires inside the wait: a second close request, and a user key queued for the window.
        QTimer::singleShot(0, fixture.window.get(), [&] {
            if (!tab || !tab->controller().saving())
                return; // the save already finished: nothing to probe
            ++reentered;
            emit tabs->tabCloseRequested(tabs->indexOf(tab));
            still_open_inside = !tab.isNull() && tabs->indexOf(tab) >= 0;
            // Window-system input (keys, clicks) is held by the wait's ExcludeUserInputEvents
            // loop; a programmatic request like this one is what the re-entry guard stops.
            fixture.window->close(); // closing the window meanwhile is refused too
            window_still_open = fixture.window->isVisible();
        });
        emit tabs->tabCloseRequested(tabs->indexOf(tab));
        CHECK((tab.isNull() || tabs->indexOf(tab) < 0)); // closed exactly once
        CHECK(tabs->count() == count - 1);
        if (reentered > 0) {
            CHECK(still_open_inside); // the second request did nothing
            CHECK(window_still_open); // nor did closing the window
        }
    }
}
