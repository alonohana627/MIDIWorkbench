#include "mwb/platform/midi_backend.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#endif

#include <blockingconcurrentqueue.h>
#include <concurrentqueue.h>
#include <libremidi/libremidi.hpp>
#include <readerwriterqueue.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <format>
#include <map>
#include <mutex>
#include <thread>
#include <variant>

namespace mwb::platform {

namespace {

std::int64_t steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Number of data bytes after a status byte for messages this layer carries (0 = not a short
/// message the queues accept).
std::size_t short_message_length(std::uint8_t status) noexcept {
    if (status < 0x80)
        return 0;
    if (status < 0xF0)
        return (status & 0xF0u) == 0xC0 || (status & 0xF0u) == 0xD0 ? 2 : 3;
    switch (status) {
    case 0xF1:
    case 0xF3:
        return 2;
    case 0xF2:
        return 3;
    case 0xF6:
    case 0xF8:
    case 0xFA:
    case 0xFB:
    case 0xFC:
    case 0xFE:
    case 0xFF:
        return 1;
    default:
        return 0; // F0/F7 (SysEx) and undefined statuses.
    }
}

/// Waits until a steady-clock time with sub-millisecond precision. std::this_thread::sleep_until
/// maps to Sleep() in milliseconds on Windows and, at the default 15.6 ms timer resolution, wakes
/// up to a full tick late (section 14.1 asks for high-resolution waits). One high-resolution
/// waitable timer per thread; where the system lacks it, the thread raises the timer resolution
/// to 1 ms for its lifetime instead.
class PreciseWaiter {
public:
    PreciseWaiter() {
#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        high_resolution_ = timer_ != nullptr;
        if (!high_resolution_)
            raised_period_ = timeBeginPeriod(1) == TIMERR_NOERROR;
#endif
    }
    ~PreciseWaiter() {
#ifdef _WIN32
        if (timer_ != nullptr)
            CloseHandle(timer_);
        if (raised_period_)
            timeEndPeriod(1);
#endif
    }
    PreciseWaiter(const PreciseWaiter&) = delete;
    PreciseWaiter& operator=(const PreciseWaiter&) = delete;

    [[nodiscard]] bool high_resolution() const noexcept { return high_resolution_ || raised_period_; }

    void wait_until(std::int64_t due_ns) noexcept {
        const std::int64_t remaining = due_ns - steady_now_ns();
        if (remaining <= 0)
            return;
#ifdef _WIN32
        if (timer_ != nullptr) {
            LARGE_INTEGER relative;
            relative.QuadPart = -std::max<std::int64_t>(remaining / 100, 1); // negative = relative, 100 ns units
            if (SetWaitableTimer(timer_, &relative, 0, nullptr, nullptr, FALSE) != 0 &&
                WaitForSingleObject(timer_, INFINITE) == WAIT_OBJECT_0)
                return;
        }
#endif
        std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(due_ns)));
    }

private:
#ifdef _WIN32
    HANDLE timer_ = nullptr;
#endif
    bool high_resolution_ = false;
    bool raised_period_ = false;
};

// --- Input side -------------------------------------------------------------------------------

/// Preallocated single-producer queue fed by a backend callback.
class InputQueue {
public:
    explicit InputQueue(std::size_t capacity) : queue_(capacity), capacity_(capacity) {}

    /// Copies a message (callback thread). Never allocates.
    void push(std::span<const std::uint8_t> bytes, std::int64_t timestamp_ns, TimestampQuality quality) noexcept {
        received_.fetch_add(1, std::memory_order_relaxed);
        if (bytes.empty() || bytes.size() > 3 || short_message_length(bytes[0]) != bytes.size()) {
            long_dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        MidiInputMessage message;
        message.arrival_ns = steady_now_ns();
        message.timestamp_ns = timestamp_ns;
        message.quality = quality;
        message.size = static_cast<std::uint8_t>(bytes.size());
        std::copy(bytes.begin(), bytes.end(), message.bytes.begin());
        if (!queue_.try_enqueue(message))
            overflowed_.fetch_add(1, std::memory_order_relaxed);
    }

    std::size_t drain(std::span<MidiInputMessage> output) noexcept {
        std::size_t count = 0;
        while (count < output.size() && queue_.try_dequeue(output[count]))
            ++count;
        return count;
    }

    std::size_t wait_drain(std::span<MidiInputMessage> output, std::chrono::milliseconds timeout) noexcept {
        if (output.empty() || !queue_.wait_dequeue_timed(output[0], timeout))
            return 0;
        return 1 + drain(output.subspan(1));
    }

    MidiInputStatistics statistics(std::uint64_t wraps) const noexcept {
        MidiInputStatistics statistics;
        statistics.received = received_.load(std::memory_order_relaxed);
        statistics.overflowed = overflowed_.load(std::memory_order_relaxed);
        statistics.long_messages_dropped = long_dropped_.load(std::memory_order_relaxed);
        statistics.timestamp_wraps = wraps;
        statistics.queue_capacity = capacity_;
        return statistics;
    }

private:
    moodycamel::BlockingReaderWriterQueue<MidiInputMessage> queue_;
    std::size_t capacity_;
    std::atomic<std::uint64_t> received_{0};
    std::atomic<std::uint64_t> overflowed_{0};
    std::atomic<std::uint64_t> long_dropped_{0};
};

// --- Output side ------------------------------------------------------------------------------

/// One queued short message.
struct OutgoingMessage {
    std::int64_t due_ns = 0;
    std::uint8_t size = 0;
    std::array<std::uint8_t, 3> bytes{};
};

/// Dedicated MIDI-output thread with its own queues. Subclasses implement transmit().
class OutputWorker : public IMidiOutput {
public:
    OutputWorker(MidiPortInfo info, const MidiOutputOptions& options)
        : info_(std::move(info)), options_(options), queue_(options.queue_capacity) {}

    ~OutputWorker() override = default;

    /// Starts the thread (called by the subclass once the endpoint is open).
    void start_thread() {
        worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    /// Stops the thread (subclasses call this in their destructor before closing the endpoint).
    void stop_thread() {
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
    }

    [[nodiscard]] const MidiPortInfo& info() const noexcept override { return info_; }

    [[nodiscard]] bool send(std::span<const std::uint8_t> message, std::int64_t due_steady_ns) noexcept override {
        if (message.empty() || message.size() > 3 || short_message_length(message[0]) != message.size())
            return false;
        OutgoingMessage outgoing;
        outgoing.due_ns = due_steady_ns;
        outgoing.size = static_cast<std::uint8_t>(message.size());
        std::copy(message.begin(), message.end(), outgoing.bytes.begin());
        if (!queue_.try_enqueue(outgoing)) {
            queue_full_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        enqueued_.fetch_add(1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] Status send_sysex(std::span<const std::uint8_t> message) override {
        if (!options_.allow_sysex)
            return make_error(ErrorCode::unsupported, "SysEx transmission is disabled for this endpoint");
        if (message.size() < 2 || message.front() != 0xF0 || message.back() != 0xF7)
            return make_error(ErrorCode::invalid_argument, "a SysEx message must start with F0 and end with F7");
        if (!sysex_queue_.enqueue(std::vector<std::uint8_t>(message.begin(), message.end())))
            return make_error(ErrorCode::busy, "the SysEx queue is full");
        enqueued_.fetch_add(1, std::memory_order_release);
        return ok();
    }

    [[nodiscard]] MidiOutputStatistics statistics() const noexcept override {
        MidiOutputStatistics statistics;
        statistics.sent = sent_.load(std::memory_order_relaxed);
        statistics.queue_full = queue_full_.load(std::memory_order_relaxed);
        statistics.send_errors = send_errors_.load(std::memory_order_relaxed);
        statistics.sysex_sent = sysex_sent_.load(std::memory_order_relaxed);
        statistics.max_lateness_ns = max_lateness_ns_.load(std::memory_order_relaxed);
        statistics.high_resolution_timer = high_resolution_.load(std::memory_order_relaxed);
        return statistics;
    }

    [[nodiscard]] bool flush(std::chrono::milliseconds timeout) override {
        std::unique_lock lock(flush_mutex_);
        return flushed_.wait_for(lock, timeout, [this] {
            return processed_.load(std::memory_order_acquire) >= enqueued_.load(std::memory_order_acquire);
        });
    }

protected:
    /// Hands one complete message to the driver; false on failure.
    virtual bool transmit(std::span<const std::uint8_t> message) noexcept = 0;

private:
    void finish_one() {
        processed_.fetch_add(1, std::memory_order_acq_rel);
        std::lock_guard lock(flush_mutex_);
        flushed_.notify_all();
    }

    void deliver(const OutgoingMessage& message, PreciseWaiter* waiter) {
        if (message.due_ns > 0 && waiter != nullptr) {
            waiter->wait_until(message.due_ns); // Scheduled delivery time, not a polling delay.
            const std::int64_t lateness = steady_now_ns() - message.due_ns;
            std::int64_t worst = max_lateness_ns_.load(std::memory_order_relaxed);
            while (lateness > worst && !max_lateness_ns_.compare_exchange_weak(worst, lateness)) {
            }
        }
        if (transmit(std::span<const std::uint8_t>(message.bytes.data(), message.size)))
            sent_.fetch_add(1, std::memory_order_relaxed);
        else
            send_errors_.fetch_add(1, std::memory_order_relaxed);
        finish_one();
    }

    void run(std::stop_token stop) {
        PreciseWaiter waiter;
        high_resolution_.store(waiter.high_resolution(), std::memory_order_relaxed);
        OutgoingMessage message;
        std::vector<std::uint8_t> sysex;
        while (!stop.stop_requested()) {
            if (queue_.try_dequeue(message)) {
                deliver(message, &waiter);
                continue;
            }
            // Short messages always go first; SysEx only when no short message is waiting.
            if (sysex_queue_.try_dequeue(sysex)) {
                if (transmit(sysex))
                    sysex_sent_.fetch_add(1, std::memory_order_relaxed);
                else
                    send_errors_.fetch_add(1, std::memory_order_relaxed);
                finish_one();
                continue;
            }
            if (queue_.wait_dequeue_timed(message, std::chrono::milliseconds(20)))
                deliver(message, &waiter);
        }
        // Stopping (endpoint closing, application quitting): whatever is still queued is sent now,
        // ignoring due times, so queued note-offs and panic messages are never dropped (13.6).
        while (queue_.try_dequeue(message))
            deliver(message, nullptr);
    }

    MidiPortInfo info_;
    MidiOutputOptions options_;
    moodycamel::BlockingConcurrentQueue<OutgoingMessage> queue_;
    moodycamel::ConcurrentQueue<std::vector<std::uint8_t>> sysex_queue_;
    std::atomic<std::uint64_t> enqueued_{0};
    std::atomic<std::uint64_t> processed_{0};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> queue_full_{0};
    std::atomic<std::uint64_t> send_errors_{0};
    std::atomic<std::uint64_t> sysex_sent_{0};
    std::atomic<std::int64_t> max_lateness_ns_{0};
    std::atomic<bool> high_resolution_{false};
    std::mutex flush_mutex_;
    std::condition_variable flushed_;
    std::jthread worker_;
};

// --- libremidi backend ------------------------------------------------------------------------

constexpr libremidi::API selected_api() noexcept {
#ifdef _WIN32
    return libremidi::API::WINDOWS_MM;
#else
    return libremidi::midi1::default_api();
#endif
}

/// Backend tag used in stable identities.
std::string api_tag() { return std::string(libremidi::get_api_name(selected_api())); }

/// Stable identity: backend, direction, USB ids when present, raw name and the ordinal among
/// ports sharing that name (not the transient session index).
template <class Port>
std::vector<MidiPortInfo> describe_ports(const std::vector<Port>& ports, bool is_input) {
    std::vector<MidiPortInfo> result;
    std::map<std::string, std::uint32_t> ordinals;
    for (std::size_t index = 0; index < ports.size(); ++index) {
        const Port& port = ports[index];
        MidiPortInfo info;
        info.backend = api_tag();
        info.is_input = is_input;
        info.session_index = static_cast<std::uint32_t>(index);
        info.port_name = port.port_name;
        info.display_name = port.display_name.empty() ? port.port_name : port.display_name;
        if (const auto* usb = std::get_if<libremidi::usb_device_identifier>(&port.device)) {
            info.vendor_id = usb->vendor_id;
            info.product_id = usb->product_id;
        }
        const std::uint32_t ordinal = ordinals[port.port_name]++;
        info.stable_id = std::format("{}:{}:{:04x}:{:04x}:{}#{}", info.backend, is_input ? "in" : "out",
                                     info.vendor_id, info.product_id, port.port_name, ordinal);
        result.push_back(std::move(info));
    }
    return result;
}

libremidi::observer make_observer() {
    libremidi::observer_configuration configuration;
    configuration.on_error = [](std::string_view, const libremidi::source_location&) {};
    configuration.on_warning = [](std::string_view, const libremidi::source_location&) {};
    configuration.track_hardware = true;
    configuration.track_virtual = true;
    configuration.notify_in_constructor = false;
    return libremidi::observer(configuration, libremidi::observer_api_configuration(selected_api()));
}

/// libremidi input endpoint.
class LibremidiInput final : public IMidiInput {
public:
    LibremidiInput(MidiPortInfo info, const MidiInputOptions& options)
        : info_(std::move(info)), queue_(options.queue_capacity), extender_(winmm_timestamp_wrap_ns) {}

    ~LibremidiInput() override {
        if (watcher_.joinable()) {
            watcher_.request_stop();
            watcher_.join();
        }
        if (input_ && input_->is_port_open())
            std::ignore = input_->close_port(); // Closing errors cannot be acted upon here.
    }

    [[nodiscard]] Status open(const libremidi::input_port& port) {
        libremidi::input_configuration configuration;
        configuration.on_raw_data = [this](std::span<const std::uint8_t> bytes, libremidi::timestamp stamp) {
            // Callback thread: bounded copy only.
            const std::int64_t extended = extender_.extend(stamp);
            wraps_.store(extender_.wraps(), std::memory_order_relaxed);
            queue_.push(bytes, extended, TimestampQuality::backend_provided);
        };
        configuration.on_error = [](std::string_view, const libremidi::source_location&) {};
        configuration.on_warning = [](std::string_view, const libremidi::source_location&) {};
        configuration.ignore_sysex = true;
        configuration.ignore_timing = true;
        configuration.ignore_sensing = true;
        configuration.timestamps = libremidi::timestamp_mode::Absolute;
        input_ = std::make_unique<libremidi::midi_in>(configuration, libremidi::input_api_configuration(selected_api()));
        if (input_->open_port(port, "MIDI Workbench input") != stdx::error{})
            return make_error(ErrorCode::device, "the MIDI input is busy or unavailable", info_.display_name);
        // WinMM never tells an open input that its device was unplugged (libremidi ignores
        // MIM_CLOSE and keeps reporting the port connected), so a watcher re-enumerates the inputs
        // twice a second and marks the endpoint removed when its stable identity disappears.
        watcher_ = std::jthread([this](std::stop_token stop) {
            std::mutex mutex;
            std::condition_variable_any wake;
            while (!stop.stop_requested()) {
                {
                    std::unique_lock lock(mutex); // nothing notifies: this is an interruptible sleep
                    std::ignore = wake.wait_for(lock, stop, std::chrono::milliseconds(500), [] { return false; });
                }
                if (stop.stop_requested())
                    return;
                const auto observer = make_observer();
                const auto present = describe_ports(observer.get_input_ports(), true);
                if (std::ranges::none_of(present, [this](const MidiPortInfo& port) { return port.stable_id == info_.stable_id; })) {
                    removed_.store(true, std::memory_order_release);
                    return;
                }
            }
        });
        return ok();
    }

    [[nodiscard]] const MidiPortInfo& info() const noexcept override { return info_; }
    [[nodiscard]] std::size_t drain(std::span<MidiInputMessage> output) noexcept override { return queue_.drain(output); }
    [[nodiscard]] std::size_t wait_drain(std::span<MidiInputMessage> output,
                                         std::chrono::milliseconds timeout) noexcept override {
        return queue_.wait_drain(output, timeout);
    }
    [[nodiscard]] MidiInputStatistics statistics() const noexcept override {
        return queue_.statistics(wraps_.load(std::memory_order_relaxed));
    }
    [[nodiscard]] bool connected() const noexcept override {
        return input_ && input_->is_port_connected() && !removed_.load(std::memory_order_acquire);
    }

private:
    MidiPortInfo info_;
    InputQueue queue_;
    TimestampExtender extender_;  ///< Touched only by the callback thread.
    std::atomic<std::uint64_t> wraps_{0};
    std::atomic<bool> removed_{false};
    std::unique_ptr<libremidi::midi_in> input_;
    std::jthread watcher_; ///< Joined first in the destructor; reads only info_ and removed_.
};

/// libremidi output endpoint driven by its own thread.
class LibremidiOutput final : public OutputWorker {
public:
    LibremidiOutput(MidiPortInfo info, const MidiOutputOptions& options) : OutputWorker(std::move(info), options) {}
    ~LibremidiOutput() override {
        stop_thread();
        if (output_ && output_->is_port_open())
            std::ignore = output_->close_port(); // Closing errors cannot be acted upon here.
    }

    [[nodiscard]] Status open(const libremidi::output_port& port) {
        libremidi::output_configuration configuration;
        configuration.on_error = [](std::string_view, const libremidi::source_location&) {};
        configuration.on_warning = [](std::string_view, const libremidi::source_location&) {};
        output_ = std::make_unique<libremidi::midi_out>(configuration, libremidi::output_api_configuration(selected_api()));
        if (output_->open_port(port, "MIDI Workbench output") != stdx::error{})
            return make_error(ErrorCode::device, "the MIDI output is busy or unavailable", info().display_name);
        start_thread();
        return ok();
    }

protected:
    bool transmit(std::span<const std::uint8_t> message) noexcept override {
        return output_->send_message(message.data(), message.size()) == stdx::error{};
    }

private:
    std::unique_ptr<libremidi::midi_out> output_;
};

class LibremidiBackend final : public IMidiBackend {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "libremidi"; }

    [[nodiscard]] Result<std::vector<MidiPortInfo>> enumerate_inputs() override {
        const auto observer = make_observer();
        return describe_ports(observer.get_input_ports(), true);
    }

    [[nodiscard]] Result<std::vector<MidiPortInfo>> enumerate_outputs() override {
        const auto observer = make_observer();
        return describe_ports(observer.get_output_ports(), false);
    }

    [[nodiscard]] Result<std::unique_ptr<IMidiInput>> open_input(std::string_view stable_id,
                                                                 const MidiInputOptions& options) override {
        if (options.queue_capacity == 0)
            return make_error(ErrorCode::invalid_argument, "the input queue capacity must be positive");
        const auto observer = make_observer();
        const auto ports = observer.get_input_ports();
        const auto infos = describe_ports(ports, true);
        for (std::size_t index = 0; index < infos.size(); ++index) {
            if (infos[index].stable_id != stable_id)
                continue;
            auto input = std::make_unique<LibremidiInput>(infos[index], options);
            MWB_TRY(input->open(ports[index]));
            return std::unique_ptr<IMidiInput>(std::move(input));
        }
        return make_error(ErrorCode::not_found, "the MIDI input is not connected", std::string(stable_id));
    }

    [[nodiscard]] Result<std::unique_ptr<IMidiOutput>> open_output(std::string_view stable_id,
                                                                   const MidiOutputOptions& options) override {
        if (options.queue_capacity == 0)
            return make_error(ErrorCode::invalid_argument, "the output queue capacity must be positive");
        const auto observer = make_observer();
        const auto ports = observer.get_output_ports();
        const auto infos = describe_ports(ports, false);
        for (std::size_t index = 0; index < infos.size(); ++index) {
            if (infos[index].stable_id != stable_id)
                continue;
            auto output = std::make_unique<LibremidiOutput>(infos[index], options);
            MWB_TRY(output->open(ports[index]));
            return std::unique_ptr<IMidiOutput>(std::move(output));
        }
        return make_error(ErrorCode::not_found, "the MIDI output is not connected", std::string(stable_id));
    }
};

// --- Null loopback backend --------------------------------------------------------------------

class NullInput;

/// Connects null outputs to open null inputs.
struct NullHub {
    std::mutex mutex;
    std::vector<NullInput*> inputs;
};

class NullInput final : public IMidiInput {
public:
    NullInput(MidiPortInfo info, const MidiInputOptions& options, std::shared_ptr<NullHub> hub)
        : info_(std::move(info)), queue_(options.queue_capacity), hub_(std::move(hub)) {
        std::lock_guard lock(hub_->mutex);
        hub_->inputs.push_back(this);
    }
    ~NullInput() override {
        std::lock_guard lock(hub_->mutex);
        std::erase(hub_->inputs, this);
    }
    /// Called with the hub mutex held (serializes producers of the SPSC queue).
    void receive(std::span<const std::uint8_t> bytes) noexcept {
        queue_.push(bytes, steady_now_ns(), TimestampQuality::arrival_estimated);
    }
    [[nodiscard]] const MidiPortInfo& info() const noexcept override { return info_; }
    [[nodiscard]] std::size_t drain(std::span<MidiInputMessage> output) noexcept override { return queue_.drain(output); }
    [[nodiscard]] std::size_t wait_drain(std::span<MidiInputMessage> output,
                                         std::chrono::milliseconds timeout) noexcept override {
        return queue_.wait_drain(output, timeout);
    }
    [[nodiscard]] MidiInputStatistics statistics() const noexcept override { return queue_.statistics(0); }
    [[nodiscard]] bool connected() const noexcept override { return true; }

private:
    MidiPortInfo info_;
    InputQueue queue_;
    std::shared_ptr<NullHub> hub_;
};

class NullOutput final : public OutputWorker {
public:
    NullOutput(MidiPortInfo info, const MidiOutputOptions& options, std::shared_ptr<NullHub> hub)
        : OutputWorker(std::move(info), options), hub_(std::move(hub)) {
        start_thread();
    }
    ~NullOutput() override { stop_thread(); }

protected:
    bool transmit(std::span<const std::uint8_t> message) noexcept override {
        std::lock_guard lock(hub_->mutex);
        for (NullInput* input : hub_->inputs)
            input->receive(message);
        return true;
    }

private:
    std::shared_ptr<NullHub> hub_;
};

MidiPortInfo null_port(bool is_input) {
    MidiPortInfo info;
    info.backend = "null";
    info.is_input = is_input;
    info.port_name = is_input ? "Null Loopback In" : "Null Loopback Out";
    info.display_name = info.port_name;
    info.stable_id = is_input ? "null:in:loopback" : "null:out:loopback";
    return info;
}

class NullMidiBackend final : public IMidiBackend {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "null"; }
    [[nodiscard]] Result<std::vector<MidiPortInfo>> enumerate_inputs() override {
        return std::vector<MidiPortInfo>{null_port(true)};
    }
    [[nodiscard]] Result<std::vector<MidiPortInfo>> enumerate_outputs() override {
        return std::vector<MidiPortInfo>{null_port(false)};
    }
    [[nodiscard]] Result<std::unique_ptr<IMidiInput>> open_input(std::string_view stable_id,
                                                                 const MidiInputOptions& options) override {
        if (stable_id != null_port(true).stable_id)
            return make_error(ErrorCode::not_found, "the MIDI input is not connected", std::string(stable_id));
        if (options.queue_capacity == 0)
            return make_error(ErrorCode::invalid_argument, "the input queue capacity must be positive");
        return std::unique_ptr<IMidiInput>(std::make_unique<NullInput>(null_port(true), options, hub_));
    }
    [[nodiscard]] Result<std::unique_ptr<IMidiOutput>> open_output(std::string_view stable_id,
                                                                   const MidiOutputOptions& options) override {
        if (stable_id != null_port(false).stable_id)
            return make_error(ErrorCode::not_found, "the MIDI output is not connected", std::string(stable_id));
        if (options.queue_capacity == 0)
            return make_error(ErrorCode::invalid_argument, "the output queue capacity must be positive");
        return std::unique_ptr<IMidiOutput>(std::make_unique<NullOutput>(null_port(false), options, hub_));
    }

private:
    std::shared_ptr<NullHub> hub_ = std::make_shared<NullHub>();
};

} // namespace

std::string_view timestamp_quality_name(TimestampQuality quality) noexcept {
    switch (quality) {
    case TimestampQuality::backend_provided:
        return "backend-provided";
    case TimestampQuality::arrival_estimated:
        return "arrival-estimated";
    case TimestampQuality::calibrated:
        return "calibrated";
    }
    return "arrival-estimated";
}

std::int64_t TimestampExtender::extend(std::int64_t raw) noexcept {
    if (has_last_ && wrap_period_ > 0 && raw < last_raw_ && last_raw_ - raw > wrap_period_ / 2) {
        offset_ += wrap_period_;
        ++wraps_;
    }
    has_last_ = true;
    last_raw_ = raw;
    return raw + offset_;
}

bool null_midi_backend_requested() {
    const char* value = std::getenv("MWB_MIDI_BACKEND");
    return value != nullptr && std::string_view(value) == "null";
}

Result<std::unique_ptr<IMidiBackend>> create_libremidi_backend() {
    return std::unique_ptr<IMidiBackend>(std::make_unique<LibremidiBackend>());
}

std::unique_ptr<IMidiBackend> create_null_midi_backend() { return std::make_unique<NullMidiBackend>(); }

Result<std::unique_ptr<IMidiBackend>> create_midi_backend() {
    if (null_midi_backend_requested())
        return create_null_midi_backend();
    return create_libremidi_backend();
}

} // namespace mwb::platform
