#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace mwb::audio {

/// Peak values read (and reset) by the user interface.
struct PeakReading {
    float left = 0.0f;
    float right = 0.0f;
    std::uint64_t clipped_samples = 0; ///< Samples at or beyond full scale since the last reading.
};

/// Peak and clip meter published from the render thread through atomics (section 13.2). The
/// render thread calls update(); any thread may call take() at display rate.
class PeakMeter {
public:
    /// Folds one block of interleaved stereo into the held peaks (real-time safe).
    void update(const float* interleaved_stereo, std::uint32_t frame_count) noexcept {
        float left = 0.0f;
        float right = 0.0f;
        std::uint64_t clipped = 0;
        for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
            const float l = std::fabs(interleaved_stereo[2 * frame]);
            const float r = std::fabs(interleaved_stereo[2 * frame + 1]);
            left = std::max(left, l);
            right = std::max(right, r);
            clipped += (l >= 1.0f ? 1u : 0u) + (r >= 1.0f ? 1u : 0u);
        }
        raise(left_, left);
        raise(right_, right);
        if (clipped > 0)
            clipped_.fetch_add(clipped, std::memory_order_relaxed);
    }

    /// Returns the held peaks and resets them.
    [[nodiscard]] PeakReading take() noexcept {
        return PeakReading{left_.exchange(0.0f, std::memory_order_relaxed),
                           right_.exchange(0.0f, std::memory_order_relaxed),
                           clipped_.exchange(0, std::memory_order_relaxed)};
    }

private:
    static void raise(std::atomic<float>& target, float value) noexcept {
        float current = target.load(std::memory_order_relaxed);
        while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
        }
    }

    std::atomic<float> left_{0.0f};
    std::atomic<float> right_{0.0f};
    std::atomic<std::uint64_t> clipped_{0};
};

} // namespace mwb::audio
