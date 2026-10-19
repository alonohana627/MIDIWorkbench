#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/model.hpp"
#include "mwb/midi/codec.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

/// Bounded parser worker (specification section 28.2): untrusted SMF/RMID bytes are parsed in
/// mwb-worker.exe under a Windows Job Object with memory and time limits. The worker reuses the
/// same midi adapter as the application; isolation is not a second parser. Results are fenced
/// by request id and content hash so a late result cannot overwrite newer data.
namespace mwb::worker {

/// Limits applied to one worker process.
struct WorkerLimits {
    std::uint64_t private_memory_bytes = 2ull * 1024ull * 1024ull * 1024ull; ///< Section 28.3 default.
    std::chrono::milliseconds parse_timeout{30000};                          ///< Default parse wall time.
    std::chrono::milliseconds heartbeat_timeout{10000};                      ///< Stall detection.
};

/// A parsed file returned by the worker: the document (transferred as a validated native
/// snapshot) and the import report JSON.
struct ParsedFile {
    DocumentData document;
    std::string report_json;
};

/// Worker side: serves frames from `read`/`write` until shutdown or a broken stream. Returns the
/// process exit code. `read` returns the number of bytes read (0 on end of stream).
int run_worker_service(const std::function<std::size_t(std::span<std::uint8_t>)>& read,
                       const std::function<bool(std::span<const std::uint8_t>)>& write);

/// Parent side: one live worker process.
class WorkerClient {
public:
    ~WorkerClient();
    WorkerClient(const WorkerClient&) = delete;
    WorkerClient& operator=(const WorkerClient&) = delete;

    /// Starts `executable` with inherited pipes, assigns it to a Job Object with the memory limit
    /// and kill-on-close, and completes the hello handshake with a random session nonce.
    [[nodiscard]] static Result<std::unique_ptr<WorkerClient>> start(const std::filesystem::path& executable,
                                                                     const WorkerLimits& limits = {});

    /// Parses bytes in the worker. A crash, memory-limit kill, malformed frame or timeout fails
    /// this item only; the client must then be restarted (is_alive() is false).
    [[nodiscard]] Result<ParsedFile> parse_midi(std::span<const std::uint8_t> bytes, const std::string& name,
                                                const midi::ParseLimits& limits, std::stop_token stop = {});

    [[nodiscard]] bool is_alive() const;
    /// Sends shutdown and waits briefly; terminates if needed.
    void stop();

    /// Isolation actually applied (documented honestly: a Job Object is not a full sandbox).
    [[nodiscard]] std::string isolation_description() const;

private:
    struct Process;
    WorkerClient(std::unique_ptr<Process> process, WorkerLimits limits);

    std::unique_ptr<Process> process_;
    WorkerLimits limits_;
    std::uint64_t next_request_ = 1;
};

} // namespace mwb::worker
