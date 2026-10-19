#include "test_support.hpp"

#include "mwb/core/note_edit.hpp"
#include "mwb/app/environment.hpp"
#include "mwb/core/serialization.hpp"
#include "mwb/document/native_format.hpp"
#include "mwb/document/recovery.hpp"
#include "mwb/document/session.hpp"
#include "mwb/platform/midi_backend.hpp"
#include "mwb/ui/clipboard.hpp"
#include "mwb/ui/document_controller.hpp"
#include "mwb/ui/drag_export.hpp"
#include "mwb/ui/editor_math.hpp"
#include "mwb/ui/keyboard_widget.hpp"
#include "mwb/ui/controller_lane.hpp"
#include "mwb/ui/editor_tab.hpp"
#include "mwb/ui/event_list.hpp"
#include "mwb/ui/inspector.hpp"
#include "mwb/ui/main_window.hpp"
#include "mwb/ui/piano_roll.hpp"
#include "mwb/ui/playback_service.hpp"
#include "mwb/ui/recording_controller.hpp"
#include "mwb/ui/tempo_field.hpp"

#include <QApplication>

#include <chrono>
#include <mutex>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTableView>
#include <QTableWidget>
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
    void publish_document(const DocumentData&, std::size_t, std::uint64_t) override {}
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
        const std::int64_t now = steady_ns();
        const auto first = playback->transport_tick_at(now);
        const auto later = playback->transport_tick_at(now + 250'000'000); // 250 ms later
        REQUIRE(first);
        REQUIRE(later);
        CHECK(*first >= 0);
        // 250 ms at 120 bpm and 4x speed is two beats: 1920 ticks at PPQ 960.
        CHECK(std::abs((*later - *first) - 1920) <= 2);
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
        tab->piano_roll().setFocus();
        fixture.key(Qt::Key_Space);
        REQUIRE(wait_until([&] { return label->text().contains("Play"); }, 3000));
        emit tab->piano_roll().seek_requested(8 * 3840); // what a click on bar 9 of the ruler sends
        REQUIRE(wait_until([&] { return label->text().startsWith("9.") || label->text().startsWith("10."); }, 3000));
        fixture.key(Qt::Key_Space); // Space now stops back at the clicked position
        QTest::qWait(100);
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
