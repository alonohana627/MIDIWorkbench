#include "mwb/audio/metronome.hpp"

#include <cmath>
#include <numbers>

namespace mwb::audio {

Status MetronomeSynth::prepare(std::uint32_t sample_rate) {
    if (sample_rate < 8000 || sample_rate > 384000)
        return make_error(ErrorCode::invalid_argument, "unsupported sample rate for the metronome");
    sample_rate_ = sample_rate;
    clicks_ = {};
    return ok();
}

void MetronomeSynth::trigger(std::uint32_t accent) noexcept {
    Click* slot = &clicks_[0];
    for (Click& click : clicks_) {
        if (!click.active) {
            slot = &click;
            break;
        }
        if (click.amplitude < slot->amplitude)
            slot = &click;
    }
    const float frequency = accent >= 2 ? 1760.0f : (accent == 1 ? 1320.0f : 1000.0f);
    const float rate = static_cast<float>(sample_rate_);
    slot->active = true;
    slot->phase = 0.0f;
    slot->step = frequency / rate;
    slot->amplitude = accent >= 2 ? 1.0f : (accent == 1 ? 0.8f : 0.6f);
    slot->decay = std::exp(-1.0f / (0.012f * rate));
}

void MetronomeSynth::render(float* interleaved_stereo, std::uint32_t frame_count) noexcept {
    const float level = level_.load(std::memory_order_relaxed);
    for (Click& click : clicks_) {
        if (!click.active)
            continue;
        for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
            const float sample = std::sin(2.0f * std::numbers::pi_v<float> * click.phase) * click.amplitude * level;
            interleaved_stereo[2 * frame] += sample;
            interleaved_stereo[2 * frame + 1] += sample;
            click.phase += click.step;
            if (click.phase >= 1.0f)
                click.phase -= 1.0f;
            click.amplitude *= click.decay;
            if (click.amplitude < 1.0e-4f) {
                click.active = false;
                break;
            }
        }
    }
}

} // namespace mwb::audio
