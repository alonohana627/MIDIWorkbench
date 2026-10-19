// mwb-worker: bounded parser process (specification section 28.2). Frames arrive on stdin and
// results leave on stdout; the process never opens output paths or the network.
#include "mwb/worker/worker.hpp"

#include <cstdio>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

int main() {
#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    const auto read = [input](std::span<std::uint8_t> buffer) -> std::size_t {
        DWORD count = 0;
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
            return 0;
        return count;
    };
    const auto write = [output](std::span<const std::uint8_t> bytes) {
        while (!bytes.empty()) {
            DWORD written = 0;
            if (!WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written == 0)
                return false;
            bytes = bytes.subspan(written);
        }
        return true;
    };
#else
    const auto read = [](std::span<std::uint8_t> buffer) -> std::size_t {
        const auto count = ::read(0, buffer.data(), buffer.size());
        return count > 0 ? static_cast<std::size_t>(count) : 0;
    };
    const auto write = [](std::span<const std::uint8_t> bytes) {
        while (!bytes.empty()) {
            const auto written = ::write(1, bytes.data(), bytes.size());
            if (written <= 0)
                return false;
            bytes = bytes.subspan(static_cast<std::size_t>(written));
        }
        return true;
    };
#endif
    return mwb::worker::run_worker_service(read, write);
}
