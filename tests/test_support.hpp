#pragma once

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

namespace mwb::test {

/// Creates a unique empty directory under the system temporary path and removes it afterwards.
class TemporaryDirectory {
public:
    TemporaryDirectory();
    ~TemporaryDirectory();
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

/// Polls `condition` until it holds or the deadline passes (tests never sleep a fixed time).
inline bool wait_until(const std::function<bool()>& condition,
                       std::chrono::milliseconds deadline = std::chrono::milliseconds(5000)) {
    const auto end = std::chrono::steady_clock::now() + deadline;
    while (std::chrono::steady_clock::now() < end) {
        if (condition())
            return true;
        std::this_thread::yield();
    }
    return condition();
}

} // namespace mwb::test
