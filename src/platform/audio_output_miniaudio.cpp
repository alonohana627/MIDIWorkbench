#include "mwb/platform/audio_output.hpp"

#include <miniaudio.h>

#include <atomic>
#include <cstring>
#include <format>
#include <string>

namespace mwb::platform {

namespace {

/// Hex form of the meaningful bytes of a device identity.
std::string encode_device_id(const ma_device_id& id) {
    std::string text;
#ifdef _WIN32
    // WASAPI identities are NUL-terminated wide strings.
    for (std::size_t index = 0; index < sizeof(id.wasapi) / sizeof(id.wasapi[0]) && id.wasapi[index] != 0; ++index)
        text += std::format("{:04x}", static_cast<unsigned>(id.wasapi[index]));
#else
    const auto* bytes = reinterpret_cast<const unsigned char*>(&id);
    for (std::size_t index = 0; index < sizeof(id); ++index)
        text += std::format("{:02x}", bytes[index]);
#endif
    return text;
}

/// Inverse of encode_device_id; false for malformed text.
bool decode_device_id(const std::string& text, ma_device_id& id) {
    std::memset(&id, 0, sizeof(id));
#ifdef _WIN32
    constexpr std::size_t width = 4;
    const std::size_t capacity = sizeof(id.wasapi) / sizeof(id.wasapi[0]) - 1;
#else
    constexpr std::size_t width = 2;
    const std::size_t capacity = sizeof(id);
#endif
    if (text.size() % width != 0 || text.size() / width > capacity)
        return false;
    for (std::size_t index = 0; index < text.size() / width; ++index) {
        unsigned value = 0;
        for (std::size_t digit = 0; digit < width; ++digit) {
            const char character = text[index * width + digit];
            value <<= 4;
            if (character >= '0' && character <= '9')
                value |= static_cast<unsigned>(character - '0');
            else if (character >= 'a' && character <= 'f')
                value |= static_cast<unsigned>(character - 'a' + 10);
            else
                return false;
        }
#ifdef _WIN32
        id.wasapi[index] = static_cast<wchar_t>(value);
#else
        reinterpret_cast<unsigned char*>(&id)[index] = static_cast<unsigned char>(value);
#endif
    }
    return true;
}

/// miniaudio output: WASAPI shared mode on Windows (default backend list elsewhere), float
/// stereo, low-latency performance profile (IAudioClient3 period negotiation when available) and
/// MMCSS "Pro Audio" thread characteristics.
class MiniaudioOutput final : public IAudioOutput {
public:
    ~MiniaudioOutput() override {
        close();
        if (context_ready_)
            ma_context_uninit(&context_);
    }

    /// Initializes the backend context (WASAPI only on Windows).
    [[nodiscard]] Status initialize() {
#ifdef _WIN32
        const ma_backend backends[] = {ma_backend_wasapi};
        const ma_result result = ma_context_init(backends, 1, nullptr, &context_);
#else
        const ma_result result = ma_context_init(nullptr, 0, nullptr, &context_);
#endif
        if (result != MA_SUCCESS)
            return make_error(ErrorCode::device, "cannot initialize the audio backend", ma_result_description(result));
        context_ready_ = true;
        return ok();
    }

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return context_ready_ ? ma_get_backend_name(context_.backend) : "miniaudio";
    }

    [[nodiscard]] Result<std::vector<AudioDeviceInfo>> enumerate_devices() override {
        ma_device_info* playback = nullptr;
        ma_uint32 playback_count = 0;
        const ma_result result = ma_context_get_devices(&context_, &playback, &playback_count, nullptr, nullptr);
        if (result != MA_SUCCESS)
            return make_error(ErrorCode::device, "cannot enumerate audio devices", ma_result_description(result));
        std::vector<AudioDeviceInfo> devices;
        devices.reserve(playback_count);
        for (ma_uint32 index = 0; index < playback_count; ++index)
            devices.push_back(AudioDeviceInfo{encode_device_id(playback[index].id), playback[index].name,
                                              playback[index].isDefault != MA_FALSE});
        return devices;
    }

    [[nodiscard]] Result<NegotiatedFormat> open(const AudioOutputConfig& config, IAudioRenderCallback& callback) override {
        if (device_ready_)
            return make_error(ErrorCode::busy, "the audio output is already open");
        ma_device_id device_id;
        if (config.device_id && !decode_device_id(*config.device_id, device_id))
            return make_error(ErrorCode::invalid_argument, "malformed audio device identity");
        callback_ = &callback;
        ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
        device_config.playback.format = ma_format_f32;
        device_config.playback.channels = 2;
        device_config.playback.pDeviceID = config.device_id ? &device_id : nullptr;
        device_config.sampleRate = config.sample_rate;
        device_config.periodSizeInFrames = config.period_frames;
        device_config.performanceProfile =
            config.low_latency ? ma_performance_profile_low_latency : ma_performance_profile_conservative;
        device_config.wasapi.usage = ma_wasapi_usage_pro_audio;
        device_config.dataCallback = &MiniaudioOutput::data_callback;
        device_config.notificationCallback = &MiniaudioOutput::notification_callback;
        device_config.pUserData = this;
        const ma_result result = ma_device_init(&context_, &device_config, &device_);
        if (result != MA_SUCCESS)
            return make_error(ErrorCode::device, "cannot open the audio device", ma_result_description(result));
        device_ready_ = true;
        lost_.store(false, std::memory_order_release);
        NegotiatedFormat format;
        format.backend = std::string(ma_get_backend_name(context_.backend));
        format.device_name = device_.playback.name;
        format.sample_rate = device_.sampleRate;
        format.device_sample_rate = device_.playback.internalSampleRate;
        format.period_frames = device_.playback.internalPeriodSizeInFrames;
        format.periods = device_.playback.internalPeriods;
        format.channels = device_.playback.channels;
        return format;
    }

    [[nodiscard]] Status start() override {
        if (!device_ready_)
            return make_error(ErrorCode::invalid_argument, "the audio output is not open");
        running_requested_.store(true, std::memory_order_release);
        const ma_result result = ma_device_start(&device_);
        if (result != MA_SUCCESS) {
            running_requested_.store(false, std::memory_order_release);
            return make_error(ErrorCode::device, "cannot start the audio device", ma_result_description(result));
        }
        return ok();
    }

    [[nodiscard]] Status stop() override {
        if (!device_ready_)
            return ok();
        running_requested_.store(false, std::memory_order_release);
        const ma_result result = ma_device_stop(&device_);
        if (result != MA_SUCCESS && result != MA_INVALID_OPERATION)
            return make_error(ErrorCode::device, "cannot stop the audio device", ma_result_description(result));
        return ok();
    }

    void close() noexcept override {
        if (!device_ready_)
            return;
        running_requested_.store(false, std::memory_order_release);
        ma_device_uninit(&device_);
        device_ready_ = false;
        callback_ = nullptr;
    }

    [[nodiscard]] bool device_lost() const noexcept override { return lost_.load(std::memory_order_acquire); }

private:
    /// Device data callback (audio thread): forwards to the render callback.
    static void data_callback(ma_device* device, void* output, const void*, ma_uint32 frame_count) {
        auto* self = static_cast<MiniaudioOutput*>(device->pUserData);
        if (self->callback_ != nullptr)
            self->callback_->render(static_cast<float*>(output), frame_count);
    }

    /// Device notifications: an unrequested stop means the device was lost.
    static void notification_callback(const ma_device_notification* notification) {
        auto* self = static_cast<MiniaudioOutput*>(notification->pDevice->pUserData);
        if (notification->type == ma_device_notification_type_stopped &&
            self->running_requested_.load(std::memory_order_acquire))
            self->lost_.store(true, std::memory_order_release); // Stopped without being asked: device lost.
    }

    ma_context context_{};
    ma_device device_{};
    bool context_ready_ = false;
    bool device_ready_ = false;
    IAudioRenderCallback* callback_ = nullptr;
    std::atomic<bool> running_requested_{false};
    std::atomic<bool> lost_{false};
};

} // namespace

Result<std::unique_ptr<IAudioOutput>> create_miniaudio_output() {
    auto output = std::make_unique<MiniaudioOutput>();
    MWB_TRY(output->initialize());
    return std::unique_ptr<IAudioOutput>(std::move(output));
}

} // namespace mwb::platform
