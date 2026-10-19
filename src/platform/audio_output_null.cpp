#include "mwb/platform/audio_output.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string_view>

namespace mwb::platform {

bool null_audio_backend_requested() {
    const char* value = std::getenv("MWB_AUDIO_BACKEND");
    return value != nullptr && std::string_view(value) == "null";
}

Result<std::unique_ptr<IAudioOutput>> create_audio_output() {
    if (null_audio_backend_requested())
        return std::unique_ptr<IAudioOutput>(std::make_unique<NullAudioOutput>());
    return create_miniaudio_output();
}

NullAudioOutput::NullAudioOutput(NullAudioSettings settings) : settings_(settings) {}

NullAudioOutput::~NullAudioOutput() { close(); }

Result<std::vector<AudioDeviceInfo>> NullAudioOutput::enumerate_devices() {
    return std::vector<AudioDeviceInfo>{AudioDeviceInfo{"null", "Null output (synthetic clock)", true}};
}

Result<NegotiatedFormat> NullAudioOutput::open(const AudioOutputConfig& config, IAudioRenderCallback& callback) {
    if (open_)
        return make_error(ErrorCode::busy, "the null output is already open");
    if (config.device_id && *config.device_id != "null")
        return make_error(ErrorCode::not_found, "the requested audio device does not exist");
    if (config.sample_rate != 0)
        settings_.sample_rate = config.sample_rate;
    if (config.period_frames != 0)
        settings_.period_frames = config.period_frames;
    if (settings_.sample_rate < 8000 || settings_.sample_rate > 384000 || settings_.period_frames == 0 ||
        settings_.period_frames > 65536)
        return make_error(ErrorCode::invalid_argument, "invalid null output format");
    callback_ = &callback;
    buffer_.assign(static_cast<std::size_t>(settings_.period_frames) * 2, 0.0f);
    rendered_.store(0, std::memory_order_release);
    lost_.store(false, std::memory_order_release);
    open_ = true;
    NegotiatedFormat format;
    format.backend = "null";
    format.device_name = "Null output (synthetic clock)";
    format.sample_rate = settings_.sample_rate;
    format.device_sample_rate = settings_.sample_rate;
    format.period_frames = settings_.period_frames;
    format.periods = 1;
    format.channels = 2;
    return format;
}

Status NullAudioOutput::start() {
    if (!open_)
        return make_error(ErrorCode::invalid_argument, "the null output is not open");
    if (lost_.load(std::memory_order_acquire))
        return make_error(ErrorCode::device, "the null output was lost");
    if (worker_.joinable())
        return ok();
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
    return ok();
}

Status NullAudioOutput::stop() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    return ok();
}

void NullAudioOutput::close() noexcept {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    open_ = false;
    callback_ = nullptr;
}

Status NullAudioOutput::pump(std::uint64_t frames) {
    if (!open_ || callback_ == nullptr)
        return make_error(ErrorCode::invalid_argument, "the null output is not open");
    if (worker_.joinable())
        return make_error(ErrorCode::busy, "the null output clock is running");
    while (frames > 0 && !lost_.load(std::memory_order_acquire)) {
        const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, settings_.period_frames));
        callback_->render(buffer_.data(), count);
        rendered_.fetch_add(count, std::memory_order_acq_rel);
        frames -= count;
    }
    return ok();
}

void NullAudioOutput::simulate_device_loss() noexcept { lost_.store(true, std::memory_order_release); }

void NullAudioOutput::run(std::stop_token stop) noexcept {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(static_cast<std::int64_t>(settings_.period_frames) * 1'000'000'000 /
                                                 settings_.sample_rate);
    auto deadline = clock::now();
    while (!stop.stop_requested() && !lost_.load(std::memory_order_acquire)) {
        callback_->render(buffer_.data(), settings_.period_frames);
        rendered_.fetch_add(settings_.period_frames, std::memory_order_acq_rel);
        if (settings_.paced) {
            deadline += period;
            std::this_thread::sleep_until(deadline); // The synthetic device clock, not a test delay.
        } else {
            std::this_thread::yield();
        }
    }
}

} // namespace mwb::platform
