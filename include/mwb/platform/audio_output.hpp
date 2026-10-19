#pragma once

#include "mwb/core/error.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace mwb::platform {

/// One playback endpoint offered by the audio backend.
struct AudioDeviceInfo {
    std::string id;          ///< Opaque stable identity for reopening (backend specific).
    std::string name;        ///< Friendly name (UTF-8).
    bool is_default = false;
};

/// Requested output configuration. Zero values ask for the backend's choice; the negotiated
/// result is reported separately and never assumed to equal the request (section 13.2).
struct AudioOutputConfig {
    std::optional<std::string> device_id;  ///< Device to open; the default endpoint when absent.
    std::uint32_t sample_rate = 0;         ///< 0 = the device's mix rate.
    std::uint32_t period_frames = 0;       ///< 0 = the low-latency profile's default.
    bool low_latency = true;               ///< Request the low-latency performance profile.
};

/// Format and timing actually obtained from the device.
struct NegotiatedFormat {
    std::string backend;                   ///< "wasapi", "null", ...
    std::string device_name;
    std::uint32_t sample_rate = 0;         ///< Rate the render callback runs at.
    std::uint32_t device_sample_rate = 0;  ///< Endpoint (internal) rate.
    std::uint32_t period_frames = 0;       ///< Endpoint period actually negotiated.
    std::uint32_t periods = 0;
    std::uint32_t channels = 2;

    /// Buffering latency implied by the negotiated period(s), in milliseconds.
    [[nodiscard]] double buffer_latency_ms() const noexcept {
        return device_sample_rate == 0 ? 0.0
                                       : 1000.0 * static_cast<double>(period_frames) * static_cast<double>(periods) /
                                             static_cast<double>(device_sample_rate);
    }
};

/// Render callback invoked on the audio thread with float interleaved stereo. Implementations
/// must be real-time safe and must fill every frame.
class IAudioRenderCallback {
public:
    virtual ~IAudioRenderCallback() = default;
    virtual void render(float* interleaved_stereo, std::uint32_t frame_count) noexcept = 0;
};

/// Audio output device abstraction (section 13.2). All methods except the callback run on a
/// non-real-time owner thread.
class IAudioOutput {
public:
    virtual ~IAudioOutput() = default;
    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;
    /// Lists playback endpoints.
    [[nodiscard]] virtual Result<std::vector<AudioDeviceInfo>> enumerate_devices() = 0;
    /// Opens the device for float stereo; reports the actual negotiated format.
    [[nodiscard]] virtual Result<NegotiatedFormat> open(const AudioOutputConfig& config, IAudioRenderCallback& callback) = 0;
    [[nodiscard]] virtual Status start() = 0;
    [[nodiscard]] virtual Status stop() = 0;
    /// Stops and releases the device (safe to call repeatedly).
    virtual void close() noexcept = 0;
    /// True after the device disappeared or failed while running (the owner enters DeviceLost).
    [[nodiscard]] virtual bool device_lost() const noexcept = 0;
};

/// Synthetic-clock output for tests and headless runs (MWB_AUDIO_BACKEND=null): drives the same
/// render callback either from a worker thread (paced to real time, or as fast as possible) or
/// synchronously through pump().
struct NullAudioSettings {
    std::uint32_t sample_rate = 48000;
    std::uint32_t period_frames = 480;
    bool paced = true;  ///< Wait for each period's deadline in the worker thread.
};

class NullAudioOutput final : public IAudioOutput {
public:
    explicit NullAudioOutput(NullAudioSettings settings = {});
    ~NullAudioOutput() override;

    [[nodiscard]] std::string_view backend_name() const noexcept override { return "null"; }
    [[nodiscard]] Result<std::vector<AudioDeviceInfo>> enumerate_devices() override;
    [[nodiscard]] Result<NegotiatedFormat> open(const AudioOutputConfig& config, IAudioRenderCallback& callback) override;
    [[nodiscard]] Status start() override;
    [[nodiscard]] Status stop() override;
    void close() noexcept override;
    [[nodiscard]] bool device_lost() const noexcept override { return lost_.load(std::memory_order_acquire); }

    /// Renders `frames` synchronously in period-sized blocks (only while not started).
    [[nodiscard]] Status pump(std::uint64_t frames);
    /// Frames rendered since open().
    [[nodiscard]] std::uint64_t rendered_frames() const noexcept { return rendered_.load(std::memory_order_acquire); }
    /// Test hook: behaves like an unplugged device (stops the clock, reports device loss).
    void simulate_device_loss() noexcept;

private:
    void run(std::stop_token stop) noexcept;

    NullAudioSettings settings_;
    IAudioRenderCallback* callback_ = nullptr;
    std::vector<float> buffer_;
    std::jthread worker_;
    std::atomic<std::uint64_t> rendered_{0};
    std::atomic<bool> lost_{false};
    bool open_ = false;
};

/// Creates the miniaudio output (WASAPI shared mode on Windows with the low-latency profile).
[[nodiscard]] Result<std::unique_ptr<IAudioOutput>> create_miniaudio_output();
/// Chooses the null backend when MWB_AUDIO_BACKEND=null, otherwise miniaudio.
[[nodiscard]] Result<std::unique_ptr<IAudioOutput>> create_audio_output();
/// True when the environment selects the null audio backend.
[[nodiscard]] bool null_audio_backend_requested();

} // namespace mwb::platform
