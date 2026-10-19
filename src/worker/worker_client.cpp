#include "mwb/worker/worker.hpp"

#include "mwb/core/hashing.hpp"
#include "mwb/core/ids.hpp"
#include "mwb/document/native_format.hpp"
#include "mwb/worker/protocol.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace mwb::worker {

/// Platform process plus a reader thread that copies pipe output into a bounded queue.
struct WorkerClient::Process {
#ifdef _WIN32
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE to_child = nullptr;   // parent writes
    HANDLE from_child = nullptr; // parent reads
#else
    pid_t pid = -1;
    int to_child = -1;
    int from_child = -1;
#endif
    std::mutex mutex;
    std::condition_variable available;
    std::deque<std::vector<std::uint8_t>> received;
    bool ended = false;
    std::jthread reader;
    FrameDecoder decoder;

    ~Process() { terminate(); }

    bool write(std::span<const std::uint8_t> bytes) {
#ifdef _WIN32
        while (!bytes.empty()) {
            DWORD written = 0;
            if (!WriteFile(to_child, bytes.data(), static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1u << 20)), &written,
                           nullptr) ||
                written == 0)
                return false;
            bytes = bytes.subspan(written);
        }
        return true;
#else
        while (!bytes.empty()) {
            const auto written = ::write(to_child, bytes.data(), bytes.size());
            if (written <= 0)
                return false;
            bytes = bytes.subspan(static_cast<std::size_t>(written));
        }
        return true;
#endif
    }

    void start_reader() {
        reader = std::jthread([this](std::stop_token) {
            std::vector<std::uint8_t> buffer(64 * 1024);
            while (true) {
#ifdef _WIN32
                DWORD count = 0;
                if (!ReadFile(from_child, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) || count == 0)
                    break;
                const std::size_t got = count;
#else
                const auto result = ::read(from_child, buffer.data(), buffer.size());
                if (result <= 0)
                    break;
                const auto got = static_cast<std::size_t>(result);
#endif
                std::lock_guard lock(mutex);
                received.emplace_back(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(got));
                available.notify_all();
            }
            std::lock_guard lock(mutex);
            ended = true;
            available.notify_all();
        });
    }

    /// Next frame before the deadline; errors when the worker died or the stream is malformed.
    Result<Frame> next_frame(std::chrono::steady_clock::time_point deadline, std::stop_token stop) {
        while (true) {
            auto frame = decoder.next();
            if (!frame)
                return std::unexpected(frame.error());
            if (*frame)
                return std::move(**frame);
            std::unique_lock lock(mutex);
            if (!available.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(50)),
                                      [this] { return !received.empty() || ended; })) {
                if (stop.stop_requested())
                    return make_error(ErrorCode::cancelled, "worker request cancelled");
                if (std::chrono::steady_clock::now() >= deadline)
                    return make_error(ErrorCode::limit_exceeded, "the worker did not answer within the time limit");
                continue;
            }
            while (!received.empty()) {
                decoder.feed(received.front());
                received.pop_front();
            }
            if (ended && decoder.buffered() == 0)
                return make_error(ErrorCode::internal, "the worker process ended (crash or memory limit)");
        }
    }

    void terminate() {
#ifdef _WIN32
        if (job) {
            TerminateJobObject(job, 1);
            CloseHandle(job);
            job = nullptr;
        }
        if (to_child) {
            CloseHandle(to_child);
            to_child = nullptr;
        }
        if (reader.joinable())
            reader.join();
        if (from_child) {
            CloseHandle(from_child);
            from_child = nullptr;
        }
        if (process) {
            CloseHandle(process);
            process = nullptr;
        }
#else
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
        if (to_child >= 0) {
            ::close(to_child);
            to_child = -1;
        }
        if (reader.joinable())
            reader.join();
        if (from_child >= 0) {
            ::close(from_child);
            from_child = -1;
        }
#endif
    }

    [[nodiscard]] bool alive() {
        std::lock_guard lock(mutex);
        return !ended;
    }
};

WorkerClient::WorkerClient(std::unique_ptr<Process> process, WorkerLimits limits)
    : process_(std::move(process)), limits_(limits) {}

WorkerClient::~WorkerClient() { stop(); }

Result<std::unique_ptr<WorkerClient>> WorkerClient::start(const std::filesystem::path& executable,
                                                          const WorkerLimits& limits) {
    auto process = std::make_unique<Process>();
#ifdef _WIN32
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE child_stdin = nullptr;
    HANDLE child_stdout = nullptr;
    if (!CreatePipe(&child_stdin, &process->to_child, &inherit, 0))
        return make_error(ErrorCode::internal, "could not create worker pipes");
    if (!CreatePipe(&process->from_child, &child_stdout, &inherit, 0)) {
        CloseHandle(child_stdin); // the parent's ends close with `process`
        return make_error(ErrorCode::internal, "could not create worker pipes");
    }
    SetHandleInformation(process->to_child, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(process->from_child, HANDLE_FLAG_INHERIT, 0);
    // Inherit only the two pipe ends; no unrelated handles (section 28.2).
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<std::uint8_t> attribute_storage(attribute_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size);
    HANDLE inherited[2] = {child_stdin, child_stdout};
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited, nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_stdin;
    startup.StartupInfo.hStdOutput = child_stdout;
    startup.StartupInfo.hStdError = nullptr; // diagnostics go through frames, not an inherited console
    startup.lpAttributeList = attributes;
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    PROCESS_INFORMATION information{};
    const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                        nullptr, &startup.StartupInfo, &information);
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(child_stdin);
    CloseHandle(child_stdout);
    if (!created)
        return make_error(ErrorCode::internal, "could not start mwb-worker (Windows error " +
                                                   std::to_string(GetLastError()) + ")");
    process->process = information.hProcess;
    process->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
    job_limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    job_limits.BasicLimitInformation.ActiveProcessLimit = 1;
    job_limits.ProcessMemoryLimit = static_cast<SIZE_T>(limits.private_memory_bytes);
    if (!process->job ||
        !SetInformationJobObject(process->job, JobObjectExtendedLimitInformation, &job_limits, sizeof job_limits) ||
        !AssignProcessToJobObject(process->job, information.hProcess)) {
        TerminateProcess(information.hProcess, 1);
        CloseHandle(information.hThread);
        return make_error(ErrorCode::internal, "could not apply the worker Job Object limits");
    }
    ResumeThread(information.hThread);
    CloseHandle(information.hThread);
#else
    int input[2];
    int output[2];
    if (::pipe(input) != 0 || ::pipe(output) != 0)
        return make_error(ErrorCode::internal, "could not create worker pipes");
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input[0], 0);
    posix_spawn_file_actions_adddup2(&actions, output[1], 1);
    posix_spawn_file_actions_addclose(&actions, input[1]);
    posix_spawn_file_actions_addclose(&actions, output[0]);
    const std::string path = executable.string();
    char* arguments[] = {const_cast<char*>(path.c_str()), nullptr};
    const int spawned = posix_spawn(&process->pid, path.c_str(), &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(input[0]);
    ::close(output[1]);
    process->to_child = input[1];
    process->from_child = output[0];
    if (spawned != 0)
        return make_error(ErrorCode::internal, "could not start mwb-worker");
#endif
    process->start_reader();
    auto client = std::unique_ptr<WorkerClient>(new WorkerClient(std::move(process), limits));
    // Handshake with a random session nonce.
    const std::string nonce = Uuid::generate().to_string();
    auto hello = encode_frame(FrameKind::hello, 0, {{"nonce", nonce}});
    if (!hello || !client->process_->write(*hello))
        return make_error(ErrorCode::internal, "could not talk to mwb-worker");
    auto reply = client->process_->next_frame(std::chrono::steady_clock::now() + limits.heartbeat_timeout, {});
    if (!reply)
        return std::unexpected(reply.error());
    if (reply->kind != FrameKind::hello || reply->payload.value("nonce", std::string()) != nonce)
        return make_error(ErrorCode::invalid_data, "mwb-worker answered the handshake incorrectly");
    return client;
}

Result<ParsedFile> WorkerClient::parse_midi(std::span<const std::uint8_t> bytes, const std::string& name,
                                            const midi::ParseLimits& limits, std::stop_token stop) {
    if (!is_alive())
        return make_error(ErrorCode::internal, "the worker is not running");
    if (bytes.size() > limits.max_bytes)
        return make_error(ErrorCode::limit_exceeded, "file is larger than the import size limit");
    const std::uint64_t request = next_request_++;
    for (const auto& chunk : chunk_payloads(request, bytes)) {
        auto frame = encode_frame(FrameKind::data_chunk, request, chunk);
        if (!frame || !process_->write(*frame))
            return make_error(ErrorCode::internal, "could not send data to the worker");
    }
    auto frame = encode_frame(FrameKind::parse_midi, request,
                              {{"name", name}, {"max_bytes", limits.max_bytes}, {"max_events", limits.max_events}});
    if (!frame || !process_->write(*frame))
        return make_error(ErrorCode::internal, "could not send the parse request to the worker");
    const auto deadline = std::chrono::steady_clock::now() + limits_.parse_timeout;
    ChunkAssembler output;
    std::optional<std::vector<std::uint8_t>> native;
    while (true) {
        auto reply = process_->next_frame(deadline, stop);
        if (!reply) {
            // Timeout, cancel, crash or malformed stream: fail this item and retire the worker.
            process_->terminate();
            return std::unexpected(reply.error());
        }
        if (reply->request_id != request)
            continue; // fenced: a late frame of an earlier request is ignored
        switch (reply->kind) {
        case FrameKind::heartbeat: break;
        case FrameKind::data_chunk: {
            auto added = output.add(reply->payload);
            if (!added) {
                process_->terminate();
                return std::unexpected(added.error());
            }
            if (*added)
                native = std::move(**added);
            break;
        }
        case FrameKind::failure:
            return make_error(static_cast<ErrorCode>(std::clamp(reply->payload.value("code", 12), 1, 12)),
                              reply->payload.value("message", std::string("worker failure")));
        case FrameKind::parse_result: {
            if (!native)
                return make_error(ErrorCode::invalid_data, "worker result arrived without its document");
            auto document = document::native_from_bytes(*native);
            if (!document)
                return std::unexpected(document.error().wrapped(ErrorCode::invalid_data, "worker returned an invalid document"));
            return ParsedFile{std::move(*document), reply->payload.value("report", std::string("{}"))};
        }
        case FrameKind::hello:
        case FrameKind::parse_midi:
        case FrameKind::cancel:
        case FrameKind::shutdown: break;
        }
    }
}

bool WorkerClient::is_alive() const { return process_ && process_->alive(); }

void WorkerClient::stop() {
    if (!process_)
        return;
    if (process_->alive())
        if (auto frame = encode_frame(FrameKind::shutdown, 0, nlohmann::json::object()))
            std::ignore = process_->write(*frame); // the process is terminated below regardless
    process_->terminate();
}

std::string WorkerClient::isolation_description() const {
#ifdef _WIN32
    return "separate process in a Windows Job Object (process memory limit " +
           std::to_string(limits_.private_memory_bytes / (1024 * 1024)) +
           " MiB, kill on close, single process); only its two pipe handles are inherited; it runs with the "
           "user's token (no restricted token) and is not a complete security boundary";
#else
    return "separate process connected by pipes; no memory limit applied on this platform";
#endif
}

} // namespace mwb::worker
