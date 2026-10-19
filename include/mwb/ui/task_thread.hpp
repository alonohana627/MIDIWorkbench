#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace mwb::ui {

/// One background thread with a FIFO task queue, so the GUI thread never waits for SQLite, the
/// filesystem or parsing (section 19.4). Destruction requests stop and joins the running task;
/// tasks still queued are dropped. Tasks deliver results with QMetaObject::invokeMethod.
class TaskThread {
public:
    TaskThread() {
        thread_ = std::jthread([this](std::stop_token stop) {
            while (true) {
                std::function<void(std::stop_token)> task;
                {
                    std::unique_lock lock(mutex_);
                    wake_.wait(lock, stop, [this] { return !tasks_.empty(); });
                    if (stop.stop_requested())
                        return;
                    task = std::move(tasks_.front());
                    tasks_.pop_front();
                }
                task(stop);
            }
        });
    }
    ~TaskThread() {
        thread_.request_stop();
        wake_.notify_all();
    }
    TaskThread(const TaskThread&) = delete;
    TaskThread& operator=(const TaskThread&) = delete;

    void post(std::function<void(std::stop_token)> task) {
        {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        wake_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<std::function<void(std::stop_token)>> tasks_;
    std::jthread thread_; // declared last: joined before the queue is destroyed
};

} // namespace mwb::ui
