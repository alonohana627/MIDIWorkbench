#pragma once

#include "mwb/core/error.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace mwb::audio {

/// Metronome click generator (section 13.7). Synthesized separately from the instrument synths so
/// clicks never enter MIDI export and are excluded from WAV renders unless explicitly requested.
/// Accent levels: 2 = bar start, 1 = group start in compound meters, 0 = other beats.
class MetronomeSynth {
public:
    /// Sets the sample rate and silences pending clicks (off the real-time thread).
    [[nodiscard]] Status prepare(std::uint32_t sample_rate);
    /// Starts a click (real-time safe).
    void trigger(std::uint32_t accent) noexcept;
    /// Adds clicks into interleaved stereo (real-time safe).
    void render(float* interleaved_stereo, std::uint32_t frame_count) noexcept;
    /// Linear click level, 0..1 (real-time safe to call from any thread).
    void set_level(float level) noexcept { level_.store(level, std::memory_order_relaxed); }
    [[nodiscard]] float level() const noexcept { return level_.load(std::memory_order_relaxed); }

private:
    struct Click {
        bool active = false;
        float phase = 0.0f;
        float step = 0.0f;
        float amplitude = 0.0f;
        float decay = 0.0f;
    };
    std::array<Click, 4> clicks_{};
    std::uint32_t sample_rate_ = 48000;
    std::atomic<float> level_{0.5f};
};

} // namespace mwb::audio
