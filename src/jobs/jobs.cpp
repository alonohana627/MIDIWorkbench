#include "mwb/jobs/jobs.hpp"

#include <BS_thread_pool.hpp>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <thread>

namespace mwb::jobs {

const char* job_state_name(JobState state) {
    switch (state) {
    case JobState::queued: return "queued";
    case JobState::running: return "running";
    case JobState::paused: return "paused";
    case JobState::complete: return "complete";
    case JobState::failed: return "failed";
    case JobState::cancelled: return "cancelled";
    case JobState::review: return "review";
    }
    return "failed";
}

JobRecord::JobRecord(std::uint64_t id, std::string name, JobPriority priority)
    : id_(id), name_(std::move(name)), priority_(priority) {}

JobProgress JobRecord::progress() const {
    std::lock_guard lock(mutex_);
    return progress_;
}

std::optional<Error> JobRecord::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

void JobRecord::report_progress(std::uint64_t completed, std::optional<std::uint64_t> total, std::string unit) {
    std::lock_guard lock(mutex_);
    progress_ = JobProgress{completed, total, std::move(unit)};
}

void JobRecord::set_error(Error error) {
    std::lock_guard lock(mutex_);
    error_ = std::move(error);
}

struct JobCoordinator::Implementation {
    explicit Implementation(unsigned workers) : pool(workers) {}

    struct Pending {
        std::shared_ptr<JobRecord> record;
        JobBody body;
    };
    mutable std::mutex mutex;
    std::condition_variable idle;
    std::deque<Pending> queue;
    std::vector<std::shared_ptr<JobRecord>> all;
    unsigned running = 0;
    unsigned capacity = 1;
    bool paused = false;
    std::uint64_t next_id = 1;
    // Declared last: destroyed first, joining tasks while the state above is still alive.
    BS::thread_pool<> pool;
};

JobCoordinator::JobCoordinator(unsigned workers) {
    if (workers == 0) {
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        workers = std::min(4u, std::max(1u, cores > 2 ? cores - 2 : 1u));
    }
    worker_count_ = workers;
    implementation_ = std::make_unique<Implementation>(workers);
    implementation_->capacity = workers;
}

JobCoordinator::~JobCoordinator() {
    cancel_all();
    wait_idle();
    // A finishing task calls dispatch() after it decrements `running`; wait for those calls too.
    implementation_->pool.wait();
}

std::shared_ptr<JobRecord> JobCoordinator::submit(std::string name, JobPriority priority, JobBody body) {
    std::shared_ptr<JobRecord> record;
    {
        std::lock_guard lock(implementation_->mutex);
        record = std::make_shared<JobRecord>(implementation_->next_id++, std::move(name), priority);
        // Stable insertion by priority keeps FIFO order within one priority class.
        auto position = std::find_if(implementation_->queue.begin(), implementation_->queue.end(),
                                     [&](const Implementation::Pending& pending) {
                                         return pending.record->priority() > priority;
                                     });
        implementation_->queue.insert(position, Implementation::Pending{record, std::move(body)});
        implementation_->all.push_back(record);
        if (implementation_->all.size() > 1000) {
            // Bounded history: evict the oldest finished job; running and queued jobs stay listed
            // so cancel_all() still reaches them.
            const auto finished = std::find_if(implementation_->all.begin(), implementation_->all.end(),
                                               [](const std::shared_ptr<JobRecord>& job) {
                                                   const auto state = job->state();
                                                   return state == JobState::complete || state == JobState::failed ||
                                                          state == JobState::cancelled;
                                               });
            if (finished != implementation_->all.end())
                implementation_->all.erase(finished);
        }
    }
    dispatch();
    return record;
}

void JobCoordinator::dispatch() {
    std::vector<Implementation::Pending> starting;
    {
        std::lock_guard lock(implementation_->mutex);
        while (!implementation_->paused && implementation_->running < implementation_->capacity &&
               !implementation_->queue.empty()) {
            starting.push_back(std::move(implementation_->queue.front()));
            implementation_->queue.pop_front();
            ++implementation_->running;
        }
    }
    for (auto& pending : starting) {
        implementation_->pool.detach_task([this, pending = std::move(pending)]() mutable {
            auto& record = *pending.record;
            if (record.stop_token().stop_requested()) {
                record.set_state(JobState::cancelled);
            } else {
                record.set_state(JobState::running);
                Status status = ok();
                try {
                    status = pending.body(record);
                } catch (const std::exception& error) {
                    status = make_error(ErrorCode::internal, std::string("job failed: ") + error.what());
                }
                if (status)
                    record.set_state(JobState::complete);
                else if (status.error().code == ErrorCode::cancelled)
                    record.set_state(JobState::cancelled);
                else {
                    record.set_error(status.error());
                    record.set_state(JobState::failed);
                }
            }
            {
                std::lock_guard lock(implementation_->mutex);
                --implementation_->running;
                implementation_->idle.notify_all();
            }
            dispatch();
        });
    }
}

void JobCoordinator::pause_dispatch(bool paused) {
    {
        std::lock_guard lock(implementation_->mutex);
        implementation_->paused = paused;
        for (auto& pending : implementation_->queue)
            pending.record->set_state(paused ? JobState::paused : JobState::queued);
    }
    if (!paused)
        dispatch();
}

void JobCoordinator::cancel_all() {
    std::lock_guard lock(implementation_->mutex);
    for (auto& record : implementation_->all)
        record->cancel();
    for (auto& pending : implementation_->queue)
        pending.record->set_state(JobState::cancelled);
    implementation_->queue.clear();
}

void JobCoordinator::wait_idle() {
    std::unique_lock lock(implementation_->mutex);
    implementation_->idle.wait(lock, [this] {
        return implementation_->running == 0 && (implementation_->queue.empty() || implementation_->paused);
    });
}

std::vector<std::shared_ptr<JobRecord>> JobCoordinator::jobs() const {
    std::lock_guard lock(implementation_->mutex);
    return implementation_->all;
}

} // namespace mwb::jobs
