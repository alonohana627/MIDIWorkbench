#pragma once

#include "mwb/core/error.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mwb::platform {

/// How trustworthy an input timestamp is (section 8.4 rule 1, section 14.1).
enum class TimestampQuality : std::uint8_t {
    backend_provided,   ///< Supplied by the driver/API (WinMM: millisecond resolution).
    arrival_estimated,  ///< Taken when the callback ran.
    calibrated,         ///< Corrected with a measured latency offset.
};

[[nodiscard]] std::string_view timestamp_quality_name(TimestampQuality quality) noexcept;

/// Extends a wrapping timestamp (for example WinMM's 32-bit millisecond counter, delivered as
/// nanoseconds) to a monotonic 64-bit value. A backward jump of more than half the wrap period is
/// treated as one wrap; smaller backward steps are kept as reported (they are jitter).
class TimestampExtender {
public:
    /// `wrap_period` is the counter range in the same unit as the values.
    explicit TimestampExtender(std::int64_t wrap_period) noexcept : wrap_period_(wrap_period) {}
    /// Returns the extended value of `raw` (raw values lie in [0, wrap_period)).
    [[nodiscard]] std::int64_t extend(std::int64_t raw) noexcept;
    [[nodiscard]] std::uint64_t wraps() const noexcept { return wraps_; }

private:
    std::int64_t wrap_period_;
    std::int64_t offset_ = 0;
    std::int64_t last_raw_ = 0;
    bool has_last_ = false;
    std::uint64_t wraps_ = 0;
};

/// WinMM delivers millisecond timestamps that wrap at 2^32 ms; libremidi scales them to ns.
inline constexpr std::int64_t winmm_timestamp_wrap_ns = (std::int64_t{1} << 32) * 1'000'000;

/// One MIDI endpoint with a friendly name and a stable identity (section 8.3, 14.1). Session
/// indices change between sessions; reopen by stable_id.
struct MidiPortInfo {
    std::string backend;          ///< "winmm", "null", ...
    std::string stable_id;        ///< Backend + vendor/product + name + duplicate ordinal.
    std::string display_name;     ///< Friendly unique name.
    std::string port_name;        ///< Raw driver name.
    std::uint32_t session_index = 0;
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
    bool is_input = true;
};

/// One received short message (bounded copy made in the input callback).
struct MidiInputMessage {
    std::int64_t timestamp_ns = 0;   ///< 64-bit extended backend timestamp (or arrival time).
    std::int64_t arrival_ns = 0;     ///< steady_clock time when the callback copied the message.
    TimestampQuality quality = TimestampQuality::arrival_estimated;
    std::uint8_t size = 0;
    std::array<std::uint8_t, 3> bytes{};
};

struct MidiInputOptions {
    std::size_t queue_capacity = 4096;  ///< Preallocated; overflow is counted, never allocated.
};

/// Counters reported in diagnostics and take reports (section 20.2).
struct MidiInputStatistics {
    std::uint64_t received = 0;
    std::uint64_t overflowed = 0;            ///< Messages lost because the queue was full.
    std::uint64_t long_messages_dropped = 0; ///< SysEx and other messages longer than 3 bytes.
    std::uint64_t timestamp_wraps = 0;
    std::size_t queue_capacity = 0;
};

/// An open input endpoint. The backend callback only copies bounded data into a preallocated
/// single-producer queue; drain() runs on the consumer (input router) thread.
class IMidiInput {
public:
    virtual ~IMidiInput() = default;
    [[nodiscard]] virtual const MidiPortInfo& info() const noexcept = 0;
    /// Moves up to output.size() queued messages into `output`; returns the count.
    [[nodiscard]] virtual std::size_t drain(std::span<MidiInputMessage> output) noexcept = 0;
    [[nodiscard]] virtual MidiInputStatistics statistics() const noexcept = 0;
    /// False after the endpoint disappeared.
    [[nodiscard]] virtual bool connected() const noexcept = 0;
};

struct MidiOutputOptions {
    std::size_t queue_capacity = 4096;  ///< Preallocated short-message queue.
    bool allow_sysex = false;           ///< SysEx transmission is disabled by default (section 14.2).
};

struct MidiOutputStatistics {
    std::uint64_t sent = 0;
    std::uint64_t queue_full = 0;
    std::uint64_t send_errors = 0;
    std::uint64_t sysex_sent = 0;
    std::int64_t max_lateness_ns = 0;   ///< Worst measured delivery lateness versus the due time.
};

/// An open output endpoint served by its own MIDI-output thread with its own queue (section 14.1):
/// callers (UI, audio engine router) never block in the driver.
class IMidiOutput {
public:
    virtual ~IMidiOutput() = default;
    [[nodiscard]] virtual const MidiPortInfo& info() const noexcept = 0;
    /// Queues a 1..3 byte channel or system-common message, sent at `due_steady_ns` (steady_clock
    /// nanoseconds; 0 = immediately). False when the message is invalid or the queue is full.
    [[nodiscard]] virtual bool send(std::span<const std::uint8_t> message, std::int64_t due_steady_ns = 0) noexcept = 0;
    /// Queues a SysEx message (F0 ... F7). Fails with unsupported unless allowed in the options.
    /// Short messages are always serviced first so SysEx cannot delay note-offs.
    [[nodiscard]] virtual Status send_sysex(std::span<const std::uint8_t> message) = 0;
    [[nodiscard]] virtual MidiOutputStatistics statistics() const noexcept = 0;
    /// Waits until every queued message was handed to the driver or the timeout passed.
    [[nodiscard]] virtual bool flush(std::chrono::milliseconds timeout) = 0;
};

/// MIDI device backend (section 14.1): enumeration and opening by stable identity. An endpoint
/// that is missing or busy is reported, never silently replaced by another one.
class IMidiBackend {
public:
    virtual ~IMidiBackend() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual Result<std::vector<MidiPortInfo>> enumerate_inputs() = 0;
    [[nodiscard]] virtual Result<std::vector<MidiPortInfo>> enumerate_outputs() = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<IMidiInput>> open_input(std::string_view stable_id,
                                                                         const MidiInputOptions& options = {}) = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<IMidiOutput>> open_output(std::string_view stable_id,
                                                                           const MidiOutputOptions& options = {}) = 0;
};

/// libremidi backend (WinMM API on Windows, the platform default elsewhere).
[[nodiscard]] Result<std::unique_ptr<IMidiBackend>> create_libremidi_backend();
/// In-process loopback backend: one input and one output port; output messages arrive at every
/// open loopback input.
[[nodiscard]] std::unique_ptr<IMidiBackend> create_null_midi_backend();
/// Chooses the null backend when MWB_MIDI_BACKEND=null, otherwise libremidi.
[[nodiscard]] Result<std::unique_ptr<IMidiBackend>> create_midi_backend();
[[nodiscard]] bool null_midi_backend_requested();

} // namespace mwb::platform
