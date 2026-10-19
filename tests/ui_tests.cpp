#include "test_support.hpp"

#include "mwb/core/note_edit.hpp"
#include "mwb/core/serialization.hpp"
#include "mwb/document/recovery.hpp"
#include "mwb/document/session.hpp"
#include "mwb/ui/clipboard.hpp"
#include "mwb/ui/document_controller.hpp"
#include "mwb/ui/editor_math.hpp"
#include "mwb/ui/keyboard_widget.hpp"
#include "mwb/ui/piano_roll.hpp"

#include <QApplication>
#include <QKeyEvent>
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
        const QPoint body = target + QPoint(4, 0);
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
