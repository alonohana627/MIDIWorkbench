// Release-only UI performance checks on the E100K corpus (specification section 33.3): visible
// editor interaction <= 16.7 ms p95 per frame, note drag feedback <= 33 ms p95. Registered only
// for optimized builds; every measured value is printed so a run records it (uiB-23, r5c-13,
// ux-editing-18).
#include "test_support.hpp"

#include "mwb/core/notes.hpp"
#include "mwb/core/random.hpp"
#include "mwb/document/session.hpp"
#include "mwb/ui/document_controller.hpp"
#include "mwb/ui/event_list.hpp"
#include "mwb/ui/piano_roll.hpp"

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QTest>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

using namespace mwb;
using namespace mwb::ui;

namespace {

QApplication& application() {
    static int argc = 1;
    static char name[] = "mwb_uiperf_tests";
    static char* argv[] = {name, nullptr};
    static QApplication app(argc, argv);
    return app;
}

/// The E100K corpus of perf_tests.cpp: 32 tracks, 100,000 notes, controllers.
DocumentData make_e100k() {
    DocumentData document = make_blank_document("E100K");
    auto& sequence = document.sequences.front();
    DeterministicRandom random(100000);
    for (int index = 1; index < 32; ++index) {
        Track track;
        track.id = TrackId::generate();
        sequence.tracks.push_back(std::move(track));
    }
    for (std::size_t track_index = 0; track_index < sequence.tracks.size(); ++track_index) {
        auto& track = sequence.tracks[track_index];
        std::vector<Event> events = track.events;
        const auto channel = static_cast<std::uint8_t>(track_index % 16);
        for (int note = 0; note < 3125; ++note) {
            const Tick start = note * 240 + static_cast<Tick>(random.bounded(60));
            const Tick length = note % 500 == 0 ? 96000 : 120 + static_cast<Tick>(random.bounded(400));
            const auto pitch = static_cast<std::uint8_t>(36 + random.bounded(48));
            events.push_back(events::note_on(start, channel, pitch, static_cast<std::uint8_t>(40 + random.bounded(80))));
            events.push_back(events::note_off(start + length, channel, pitch, 0));
            if (note % 8 == 0)
                events.push_back(events::control_change(start, channel, 1, static_cast<std::uint8_t>(random.bounded(128))));
        }
        std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.tick < b.tick; });
        for (std::size_t index = 0; index < events.size(); ++index)
            events[index].order_key = (index + 1) * order_key_spacing;
        track.events = std::move(events);
        track.end_tick = track.events.back().tick;
    }
    sequence.end_tick = sequence.effective_end();
    return document;
}

struct Timing {
    double median = 0.0;
    double p95 = 0.0;
};

Timing measure(int runs, const std::function<void()>& work) {
    std::vector<double> samples;
    for (int run = 0; run < runs; ++run) {
        const auto begin = std::chrono::steady_clock::now();
        work();
        samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
    }
    std::ranges::sort(samples);
    const auto rank = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(samples.size())));
    return Timing{samples[samples.size() / 2], samples[std::clamp<std::size_t>(rank, 1, samples.size()) - 1]};
}

void report(const char* name, const Timing& timing) {
    std::printf("%-44s median %8.2f ms  p95 %8.2f ms\n", name, timing.median, timing.p95);
}

std::unique_ptr<DocumentController> e100k_controller() {
    application();
    auto session = std::make_unique<document::WorkingSession>(make_e100k(), document::SessionOrigin::opened_file);
    return std::make_unique<DocumentController>(std::move(session));
}

} // namespace

TEST_SUITE("uiperf") {
    TEST_CASE("E100K piano roll: frames at fit-all and 8 bars, playback, selection and drag feedback") {
        auto controller = e100k_controller();
        PianoRollView view(*controller);
        view.resize(1600, 900);
        view.show();
        QTest::qWait(50);
        QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
        const auto frame = [&] { view.render(&image); };

        view.fit_all(); // the whole piece: summarized by density
        const auto fit_all = measure(20, frame);
        report("frame, fit-all (100,000 notes)", fit_all);
        CHECK(fit_all.p95 <= 16.7);

        // Eight bars of all 32 tracks: every note drawn.
        view.set_origin(0);
        const double area = static_cast<double>(view.width() - PianoRollView::keyboard_width);
        view.zoom_time(view.mapping().ticks_per_pixel * area / (8.0 * 4 * 960));
        view.set_top_pitch(90);
        const auto bars = measure(30, frame);
        report("frame, 8 bars", bars);
        CHECK(bars.p95 <= 16.7);

        // Playback: the playhead moves 30 times a second; only its columns repaint.
        Tick tick = 0;
        const auto playback = measure(60, [&] {
            tick += 32;
            view.set_playhead(tick);
            view.repaint(); // what the next frame paints
        });
        report("playhead step + repaint, 8 bars", playback);
        CHECK(playback.p95 <= 16.7);
        view.set_playhead(std::nullopt);

        view.select_all();
        QApplication::processEvents();
        const auto selected = measure(30, frame);
        report("frame, 8 bars, everything selected", selected);
        CHECK(selected.p95 <= 16.7);

        // Drag feedback: moving every note of the active track (3,125, select all) follows the
        // pointer while the other 31 tracks are drawn as ghosts.
        REQUIRE(controller->selection().size() == 3125);
        const auto& all = controller->notes().notes;
        const auto first = std::ranges::min_element(all, [&](const NoteSpan& a, const NoteSpan& b) {
            const bool a_selected = controller->selection().contains(a.on_id);
            const bool b_selected = controller->selection().contains(b.on_id);
            return a_selected != b_selected ? a_selected : a.start < b.start;
        });
        REQUIRE(first != all.end());
        const auto& mapping = view.mapping();
        const QPoint press(PianoRollView::keyboard_width + static_cast<int>(std::lround(mapping.tick_to_x(first->start))) + 2,
                           PianoRollView::ruler_height + static_cast<int>(mapping.pitch_to_y(first->pitch)) + 2);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, press);
        int step = 0;
        const auto drag = measure(30, [&] {
            ++step;
            const QPoint point = press + QPoint(step * 3, (step % 4) * 2);
            QMouseEvent move(QEvent::MouseMove, QPointF(point), view.mapToGlobal(QPointF(point)), Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&view, &move);
            view.repaint();
        });
        report("drag feedback, 3,125 selected notes", drag);
        CHECK(drag.p95 <= 33.0);
        QTest::keyClick(&view, Qt::Key_Escape); // cancel: the final move is not part of the feedback budget
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, press);
    }

    TEST_CASE("E100K event list rebuild, filter and select-same commands") {
        auto controller = e100k_controller();
        std::unique_ptr<EventTableModel> model;
        const auto rebuild = measure(5, [&] { model = std::make_unique<EventTableModel>(*controller); });
        report("event list rebuild (212,000 events)", rebuild);
        CHECK(model->rowCount() > 0);
        // Not a frame: an edit's list refresh runs after the burst settles. Bounded by the
        // ordinary-edit budget of section 33.3 (50 ms).
        CHECK(rebuild.p95 <= 50.0);

        EventFilterProxy proxy;
        proxy.setSourceModel(model.get());
        const auto first_filter = measure(1, [&] {
            proxy.set_needle("note on");
            std::ignore = proxy.rowCount();
        });
        report("event filter, first (builds search keys)", first_filter);
        const auto filter = measure(10, [&] {
            proxy.set_needle(proxy.needle() == "cc 1" ? "note off" : "cc 1");
            std::ignore = proxy.rowCount();
        });
        report("event filter, typed", filter);
        CHECK(first_filter.p95 <= 150.0); // the first typed character (text search budget)
        CHECK(filter.p95 <= 50.0);

        PianoRollView view(*controller);
        view.resize(1600, 900);
        const auto& notes = controller->notes().notes;
        REQUIRE_FALSE(notes.empty());
        const auto active = std::ranges::find_if(notes, [&](const NoteSpan& note) { return note.on_track == 0; });
        REQUIRE(active != notes.end());
        const auto same_channel = measure(10, [&] {
            controller->set_selection({active->on_id});
            view.select_same_channel();
        });
        report("select same channel", same_channel);
        CHECK(controller->selection().size() == 3125); // the active track's notes on that channel
        CHECK(same_channel.p95 <= 50.0);
    }
}
