#include "test_support.hpp"

#include "mwb/jobs/jobs.hpp"

#include <atomic>

using namespace mwb;
using namespace mwb::jobs;

TEST_SUITE("jobs") {
    TEST_CASE("jobs complete, fail and report progress in units") {
        JobCoordinator coordinator(2);
        auto good = coordinator.submit("count", JobPriority::import, [](JobRecord& record) {
            for (std::uint64_t index = 1; index <= 10; ++index)
                record.report_progress(index, 10, "files");
            return ok();
        });
        auto bad = coordinator.submit("fail", JobPriority::analysis, [](JobRecord&) -> Status {
            return make_error(ErrorCode::io, "disk unplugged");
        });
        coordinator.wait_idle();
        CHECK(good->state() == JobState::complete);
        CHECK(good->progress().completed == 10);
        CHECK(good->progress().unit == "files");
        CHECK(bad->state() == JobState::failed);
        REQUIRE(bad->error());
        CHECK(bad->error()->code == ErrorCode::io);
    }

    TEST_CASE("cancellation is cooperative and observed") {
        JobCoordinator coordinator(1);
        std::atomic<bool> started{false};
        auto job = coordinator.submit("long", JobPriority::maintenance, [&](JobRecord& record) -> Status {
            started = true;
            while (!record.stop_token().stop_requested())
                std::this_thread::yield();
            return make_error(ErrorCode::cancelled, "cancelled");
        });
        REQUIRE(test::wait_until([&] { return started.load(); }));
        job->cancel();
        coordinator.wait_idle();
        CHECK(job->state() == JobState::cancelled);
    }

    TEST_CASE("higher priority queued jobs run first") {
        JobCoordinator coordinator(1);
        coordinator.pause_dispatch(true);
        std::vector<int> order;
        std::mutex mutex;
        const auto body = [&](int value) {
            return [&, value](JobRecord&) {
                std::lock_guard lock(mutex);
                order.push_back(value);
                return ok();
            };
        };
        coordinator.submit("analysis", JobPriority::analysis, body(3));
        coordinator.submit("import", JobPriority::import, body(2));
        coordinator.submit("preview", JobPriority::requested_preview, body(1));
        coordinator.pause_dispatch(false);
        coordinator.wait_idle();
        CHECK(order == std::vector<int>{1, 2, 3});
    }

    TEST_CASE("default worker count is bounded") {
        JobCoordinator coordinator;
        CHECK(coordinator.worker_count() >= 1);
        CHECK(coordinator.worker_count() <= 4);
    }
}
