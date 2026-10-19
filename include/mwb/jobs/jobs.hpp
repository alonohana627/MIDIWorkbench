#pragma once

#include "mwb/core/error.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

/// Background jobs (specification sections 20.1 and 28.1): a bounded BS::thread_pool with
/// min(4, max(1, cores - 2)) workers, priorities, honest progress in meaningful units, and
/// cooperative cancellation. Never unbounded thread creation.
namespace mwb::jobs {

/// Job states of section 28.1.
enum class JobState : std::uint8_t { queued, running, paused, complete, failed, cancelled, review };

/// Priority order of section 28.1 (lower value runs first).
enum class JobPriority : std::uint8_t {
    requested_preview = 0,
    interactive_search = 1,
    explicit_export = 2,
    import = 3,
    analysis = 4,
    maintenance = 5,
};

/// Progress measured in units (files, events, rows). `total` is empty when unknown: no fake
/// percentage is shown then.
struct JobProgress {
    std::uint64_t completed = 0;
    std::optional<std::uint64_t> total;
    std::string unit = "items";
};

/// Shared state of one job, readable from any thread.
class JobRecord {
public:
    JobRecord(std::uint64_t id, std::string name, JobPriority priority);

    [[nodiscard]] std::uint64_t id() const { return id_; }
    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] JobPriority priority() const { return priority_; }
    [[nodiscard]] JobState state() const { return state_.load(); }
    [[nodiscard]] JobProgress progress() const;
    [[nodiscard]] std::optional<Error> error() const;
    /// Requests cooperative cancellation; long loops check it within about 50 ms of work.
    void cancel() { stop_.request_stop(); }
    [[nodiscard]] std::stop_token stop_token() const { return stop_.get_token(); }

    /// Called by the job body.
    void report_progress(std::uint64_t completed, std::optional<std::uint64_t> total, std::string unit = "items");

private:
    friend class JobCoordinator;
    void set_state(JobState state) { state_.store(state); }
    void set_error(Error error);

    std::uint64_t id_;
    std::string name_;
    JobPriority priority_;
    std::atomic<JobState> state_{JobState::queued};
    std::stop_source stop_;
    mutable std::mutex mutex_;
    JobProgress progress_;
    std::optional<Error> error_;
};

/// The job body: returns success, an error, or ErrorCode::cancelled when it observed the stop.
using JobBody = std::function<Status(JobRecord&)>;

/// Bounded job pool with a priority queue in front of it.
class JobCoordinator {
public:
    /// `workers` 0 means min(4, max(1, hardware threads - 2)).
    explicit JobCoordinator(unsigned workers = 0);
    ~JobCoordinator();
    JobCoordinator(const JobCoordinator&) = delete;
    JobCoordinator& operator=(const JobCoordinator&) = delete;

    /// Queues a job and returns its record.
    std::shared_ptr<JobRecord> submit(std::string name, JobPriority priority, JobBody body);
    /// Pauses dispatch of queued jobs (running jobs continue to their next check).
    void pause_dispatch(bool paused);
    /// Cancels every queued and running job.
    void cancel_all();
    /// Blocks until every queued and running job finished (tests, shutdown).
    void wait_idle();
    [[nodiscard]] unsigned worker_count() const { return worker_count_; }
    /// Snapshot of known jobs, newest last.
    [[nodiscard]] std::vector<std::shared_ptr<JobRecord>> jobs() const;

private:
    struct Implementation;
    void dispatch();

    std::unique_ptr<Implementation> implementation_;
    unsigned worker_count_ = 1;
};

[[nodiscard]] const char* job_state_name(JobState state);

} // namespace mwb::jobs
