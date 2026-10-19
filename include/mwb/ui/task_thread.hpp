#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace mwb::ui {

/// One background thread with a FIFO task queue, so the GUI thread never waits for SQLite, the
/// filesystem or parsing (section 19.4). stop() (and destruction) requests stop and joins: the
/// running task sees its stop token; queued tasks are dropped, or run first when `drain` is set
/// (used where queued work must land, such as closing recovery sessions). Tasks deliver results
/// with QMetaObject::invokeMethod(qApp, ...) and check a QPointer inside the lambda.
class TaskThread {
public:
    explicit TaskThread(bool drain = false) : drain_(drain) {
        thread_ = std::jthread([this](std::stop_token stop) {
            while (true) {
                Entry entry;
                {
                    std::unique_lock lock(mutex_);
                    wake_.wait(lock, stop, [this] { return !tasks_.empty(); });
                    if (stop.stop_requested() && (!drain_ || tasks_.empty()))
                        return;
                    entry = std::move(tasks_.front());
                    tasks_.pop_front();
                    running_ = true;
                }
                try {
                    entry.task(stop);
                } catch (...) {
                    // A failing task must not terminate the application. Its own completion never
                    // runs, so the poster's failure handler clears "in flight" state instead
                    // (SQLiteCpp throws on storage errors the library does not wrap).
                    if (entry.on_failure) {
                        try {
                            entry.on_failure();
                        } catch (...) {
                            // nothing more can be reported from here
                        }
                    }
                }
                {
                    std::lock_guard lock(mutex_);
                    running_ = false;
                }
                idle_.notify_all();
            }
        });
    }
    ~TaskThread() { stop(); }
    void stop() {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
        }
        thread_.request_stop();
        wake_.notify_all();
        if (thread_.joinable())
            thread_.join();
        {
            std::lock_guard lock(mutex_);
            tasks_.clear(); // never run after stop: post() refuses new ones
            running_ = false;
        }
        idle_.notify_all();
    }
    TaskThread(const TaskThread&) = delete;
    TaskThread& operator=(const TaskThread&) = delete;

    /// Blocks until every queued task has run; the thread stays available afterwards. Returns at
    /// once after stop(), when queued tasks will never run.
    void wait_idle() {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return stopped_ || (tasks_.empty() && !running_); });
    }

    /// Queues a task. `on_failure` runs on this thread when the task throws, so a poster that set
    /// an "in flight" flag can post its reset (and an error text) back to the GUI. Returns false,
    /// and drops the task, after stop().
    bool post(std::function<void(std::stop_token)> task, std::function<void()> on_failure = {}) {
        {
            std::lock_guard lock(mutex_);
            if (stopped_)
                return false;
            tasks_.push_back(Entry{std::move(task), std::move(on_failure)});
        }
        wake_.notify_all();
        return true;
    }

private:
    struct Entry {
        std::function<void(std::stop_token)> task;
        std::function<void()> on_failure;
    };

    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::condition_variable_any idle_;
    bool running_ = false;
    bool stopped_ = false;
    std::deque<Entry> tasks_;
    bool drain_ = false;
    std::jthread thread_; // declared last: joined before the queue is destroyed
};

} // namespace mwb::ui
