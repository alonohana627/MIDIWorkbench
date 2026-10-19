#include "mwb/audio/builtin_synth.hpp"

#include <signalsmith-dsp/filters.h>
#include <pcg_random.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mwb::audio {

namespace {

constexpr std::size_t maximum_voices = 512;
constexpr std::size_t fade_slot_count = 16;
constexpr float silence_threshold = 1.0e-4f;
constexpr float steal_ramp_seconds = 0.003f;
constexpr float two_pi = 2.0f * std::numbers::pi_v<float>;
constexpr int no_override = -1;

/// Percussion instruments of the built-in kit.
enum class DrumKind : std::uint8_t { none, kick, snare, clap, closed_hat, open_hat, tom, crash, ride, rim };

/// Envelope stages.
enum class Stage : std::uint8_t { attack, decay, sustain, release };

/// Static parameters of one timbre.
struct TimbreParameters {
    float attack_seconds;
    float decay_seconds;   ///< Exponential time constant toward the sustain level.
    float sustain_level;
    float release_seconds; ///< Exponential time constant.
    float cutoff_hz;
    float resonance_q;
};

constexpr TimbreParameters timbre_parameters(Timbre timbre) noexcept {
    switch (timbre) {
    case Timbre::classic:
        return {0.004f, 0.35f, 0.55f, 0.12f, 9000.0f, 0.707f};
    case Timbre::saw_pad:
        return {0.30f, 1.20f, 0.80f, 0.45f, 2400.0f, 0.707f};
    case Timbre::square_lead:
        return {0.004f, 0.25f, 0.70f, 0.08f, 3200.0f, 0.9f};
    case Timbre::electric_piano:
        return {0.002f, 1.10f, 0.0f, 0.18f, 7000.0f, 0.707f};
    case Timbre::bass:
        return {0.003f, 0.40f, 0.50f, 0.06f, 900.0f, 1.2f};
    }
    return {0.004f, 0.35f, 0.55f, 0.12f, 9000.0f, 0.707f};
}

/// Percussion parameters for one note of the kit.
struct DrumParameters {
    DrumKind kind = DrumKind::none;
    float tone_hz = 0.0f;
    float sweep_hz = 0.0f;       ///< Initial extra pitch that decays away.
    float sweep_seconds = 0.03f; ///< Time constant of the pitch sweep.
    float decay_seconds = 0.2f;
    float tone_mix = 0.0f;
    float noise_mix = 0.0f;
};

DrumParameters drum_for_note(std::uint8_t note) noexcept {
    switch (note) {
    case 35:
    case 36:
        return {DrumKind::kick, 48.0f, 110.0f, 0.035f, 0.30f, 1.0f, 0.02f};
    case 37:
        return {DrumKind::rim, 900.0f, 0.0f, 0.03f, 0.03f, 0.4f, 0.6f};
    case 38:
    case 40:
        return {DrumKind::snare, 185.0f, 40.0f, 0.02f, 0.16f, 0.35f, 0.75f};
    case 39:
        return {DrumKind::clap, 0.0f, 0.0f, 0.03f, 0.14f, 0.0f, 1.0f};
    case 42:
    case 44:
        return {DrumKind::closed_hat, 0.0f, 0.0f, 0.03f, 0.045f, 0.0f, 0.8f};
    case 46:
        return {DrumKind::open_hat, 0.0f, 0.0f, 0.03f, 0.32f, 0.0f, 0.75f};
    case 41:
    case 43:
    case 45:
    case 47:
    case 48:
    case 50: {
        const float base = 80.0f + static_cast<float>(note - 41) * 14.0f;
        return {DrumKind::tom, base, base * 0.8f, 0.05f, 0.35f, 1.0f, 0.05f};
    }
    case 49:
    case 52:
    case 55:
    case 57:
        return {DrumKind::crash, 0.0f, 0.0f, 0.03f, 1.10f, 0.0f, 0.7f};
    case 51:
    case 53:
    case 59:
        return {DrumKind::ride, 3200.0f, 0.0f, 0.03f, 0.60f, 0.08f, 0.45f};
    default:
        return {DrumKind::rim, 600.0f, 0.0f, 0.03f, 0.05f, 0.4f, 0.5f};
    }
}

float coefficient(float seconds, float sample_rate) noexcept {
    return std::exp(-1.0f / std::max(seconds * sample_rate, 1.0f));
}

/// PolyBLEP residual for band-limited saw and square waves.
float poly_blep(float phase, float step) noexcept {
    if (phase < step) {
        const float t = phase / step;
        return t + t - t * t - 1.0f;
    }
    if (phase > 1.0f - step) {
        const float t = (phase - 1.0f) / step;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

float saw(float phase, float step) noexcept { return 2.0f * phase - 1.0f - poly_blep(phase, step); }

float square(float phase, float step) noexcept {
    float value = phase < 0.5f ? 1.0f : -1.0f;
    value += poly_blep(phase, step);
    float shifted = phase + 0.5f;
    if (shifted >= 1.0f)
        shifted -= 1.0f;
    value -= poly_blep(shifted, step);
    return value;
}

float triangle(float phase) noexcept { return phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase; }

float wrap(float phase) noexcept { return phase >= 1.0f ? phase - std::floor(phase) : phase; }

} // namespace

/// One synthesis voice; trivially copyable so a stolen voice can be moved to a fade slot.
struct BuiltinSynth::Voice {
    bool active = false;
    bool released = false;
    bool sustained = false;
    bool fading = false;
    std::uint8_t channel = 0;
    std::uint8_t note = 0;
    Timbre timbre = Timbre::classic;
    DrumKind drum = DrumKind::none;
    Stage stage = Stage::attack;
    std::uint64_t serial = 0;
    float velocity_gain = 0.0f;
    float base_frequency = 440.0f;
    float phase_primary = 0.0f;
    float phase_secondary = 0.0f;
    float phase_sub = 0.0f;
    float level = 0.0f;
    float attack_step = 1.0f;
    float decay_coefficient = 0.0f;
    float sustain_level = 0.0f;
    float release_coefficient = 0.0f;
    float modulation_index = 0.0f;
    float modulation_decay = 1.0f;
    float sweep_hz = 0.0f;
    float sweep_coefficient = 0.0f;
    float tone_mix = 0.0f;
    float noise_mix = 0.0f;
    float fade_gain = 1.0f;
    float fade_step = 0.0f;
    signalsmith::filters::BiquadStatic<float> filter;
    pcg32 noise;
};

/// Per-channel controller state.
struct BuiltinSynth::Channel {
    std::uint8_t program = 0;
    std::uint8_t bank_msb = 0;
    std::uint8_t bank_lsb = 0;
    std::uint8_t parameter_msb = 127;
    std::uint8_t parameter_lsb = 127;
    bool sustain = false;
    float volume = 100.0f / 127.0f;
    float expression = 1.0f;
    float pan = 0.5f;
    float bend = 0.0f;           ///< -1..1
    float bend_range = 2.0f;     ///< Semitones.
    float modulation = 0.0f;     ///< 0..1
    float pressure = 0.0f;       ///< 0..1
    float vibrato_phase = 0.0f;  ///< 0..1
};

std::string_view timbre_name(Timbre timbre) noexcept {
    switch (timbre) {
    case Timbre::classic:
        return "classic";
    case Timbre::saw_pad:
        return "saw-pad";
    case Timbre::square_lead:
        return "square-lead";
    case Timbre::electric_piano:
        return "electric-piano";
    case Timbre::bass:
        return "bass";
    }
    return "classic";
}

std::optional<Timbre> parse_timbre(std::string_view name) noexcept {
    for (Timbre timbre : {Timbre::classic, Timbre::saw_pad, Timbre::square_lead, Timbre::electric_piano, Timbre::bass})
        if (timbre_name(timbre) == name)
            return timbre;
    return std::nullopt;
}

Timbre timbre_for_program(std::uint8_t program) noexcept {
    if (program == 4 || program == 5 || (program >= 8 && program <= 15))
        return Timbre::electric_piano;
    if (program >= 32 && program <= 39)
        return Timbre::bass;
    if ((program >= 40 && program <= 55) || (program >= 88 && program <= 95))
        return Timbre::saw_pad;
    if (program >= 56 && program <= 87)
        return Timbre::square_lead;
    return Timbre::classic;
}

BuiltinSynth::BuiltinSynth(const BuiltinSynthSettings& settings)
    : settings_(settings),
      voices_(settings.voice_capacity),
      fade_voices_(fade_slot_count),
      channels_(std::make_unique<std::array<Channel, 17>>()) {
    for (auto& value : timbre_override_)
        value.store(no_override, std::memory_order_relaxed);
}

BuiltinSynth::~BuiltinSynth() = default;

Result<std::unique_ptr<BuiltinSynth>> BuiltinSynth::create(const BuiltinSynthSettings& settings) {
    if (settings.voice_capacity == 0 || settings.voice_capacity > maximum_voices)
        return make_error(ErrorCode::invalid_argument, "the built-in synth supports 1 to 512 voices");
    if (!(settings.master_gain >= 0.0f) || settings.master_gain > 4.0f)
        return make_error(ErrorCode::invalid_argument, "master gain must be between 0 and 4");
    return std::unique_ptr<BuiltinSynth>(new BuiltinSynth(settings));
}

std::size_t BuiltinSynth::voice_capacity() const noexcept { return voices_.size(); }

Status BuiltinSynth::prepare(std::uint32_t sample_rate, std::uint32_t max_block_frames) {
    if (sample_rate < 8000 || sample_rate > 384000)
        return make_error(ErrorCode::invalid_argument, "unsupported sample rate for the built-in synth");
    if (max_block_frames == 0)
        return make_error(ErrorCode::invalid_argument, "block size must be positive");
    sample_rate_ = sample_rate;
    for (Voice& voice : voices_)
        voice = Voice{};
    for (Voice& voice : fade_voices_)
        voice = Voice{};
    *channels_ = std::array<Channel, 17>{};
    voice_serial_ = 0;
    steals_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    active_.store(0, std::memory_order_relaxed);
    return ok();
}

SynthIdentity BuiltinSynth::identity() const {
    return SynthIdentity{"builtin", "mwb-builtin-synth 1 (classic, saw-pad, square-lead, electric-piano, bass, kit 1)",
                         "", "built-in timbres v1"};
}

std::size_t BuiltinSynth::active_voices() const noexcept { return active_.load(std::memory_order_relaxed); }

void BuiltinSynth::set_channel_timbre(std::uint8_t channel, std::optional<Timbre> timbre) noexcept {
    timbre_override_[channel & 0x0Fu].store(timbre ? static_cast<int>(*timbre) : no_override, std::memory_order_relaxed);
}

void BuiltinSynth::set_audition_timbre(std::optional<Timbre> timbre) noexcept {
    timbre_override_[audition_channel].store(timbre ? static_cast<int>(*timbre) : no_override, std::memory_order_relaxed);
}

void BuiltinSynth::dispatch(const playback::RealtimeMidiEvent& event) noexcept {
    if (event.kind != playback::RealtimeEventKind::midi || event.size == 0)
        return;
    dispatch_on(event.channel(), event);
}

void BuiltinSynth::dispatch_live(const playback::RealtimeMidiEvent& event) noexcept {
    if (event.kind != playback::RealtimeEventKind::midi || event.size == 0)
        return;
    dispatch_on(audition_channel, event);
}

void BuiltinSynth::dispatch_on(std::uint8_t channel, const playback::RealtimeMidiEvent& event) noexcept {
    Channel& state = (*channels_)[channel];
    switch (event.message()) {
    case 0x90:
        if (event.data2() == 0)
            note_off(channel, event.data1());
        else
            note_on(channel, event.data1(), event.data2());
        return;
    case 0x80:
        note_off(channel, event.data1());
        return;
    case 0xB0:
        control_change(channel, event.data1(), event.data2());
        return;
    case 0xC0:
        state.program = event.data1();
        return;
    case 0xD0:
        state.pressure = static_cast<float>(event.data1()) / 127.0f;
        return;
    case 0xE0: {
        const int value = static_cast<int>(event.data1()) | (static_cast<int>(event.data2()) << 7);
        state.bend = static_cast<float>(value - 8192) / 8192.0f;
        return;
    }
    default:
        return;
    }
}

void BuiltinSynth::control_change(std::uint8_t channel, std::uint8_t controller, std::uint8_t value) noexcept {
    Channel& state = (*channels_)[channel];
    const float normalized = static_cast<float>(value) / 127.0f;
    switch (controller) {
    case 0:
        state.bank_msb = value;
        return;
    case 32:
        state.bank_lsb = value;
        return;
    case 1:
        state.modulation = normalized;
        return;
    case 7:
        state.volume = normalized;
        return;
    case 10:
        state.pan = normalized;
        return;
    case 11:
        state.expression = normalized;
        return;
    case 101:
        state.parameter_msb = value;
        return;
    case 100:
        state.parameter_lsb = value;
        return;
    case 98:
    case 99:
        state.parameter_msb = 127;
        state.parameter_lsb = 127;
        return;
    case 6:
        if (state.parameter_msb == 0 && state.parameter_lsb == 0)
            state.bend_range = static_cast<float>(value);
        return;
    case 64: {
        const bool down = value >= 64;
        if (state.sustain && !down) {
            for (Voice& voice : voices_) {
                if (voice.active && voice.channel == channel && voice.sustained) {
                    voice.sustained = false;
                    voice.released = true;
                    voice.stage = Stage::release;
                }
            }
        }
        state.sustain = down;
        return;
    }
    case 120: // All sound off: fast ramp.
        for (Voice& voice : voices_) {
            if (voice.active && voice.channel == channel && !voice.fading) {
                voice.fading = true;
                voice.fade_step = 1.0f / (steal_ramp_seconds * static_cast<float>(sample_rate_));
            }
        }
        return;
    case 121:
        state.modulation = 0.0f;
        state.expression = 1.0f;
        state.pressure = 0.0f;
        state.bend = 0.0f;
        state.parameter_msb = 127;
        state.parameter_lsb = 127;
        control_change(channel, 64, 0);
        return;
    case 123: // All notes off: release (sustain still holds, as in MIDI).
        for (Voice& voice : voices_)
            if (voice.active && voice.channel == channel && !voice.released)
                note_off(channel, voice.note);
        return;
    default:
        return;
    }
}

BuiltinSynth::Voice* BuiltinSynth::allocate_voice() noexcept {
    for (Voice& voice : voices_)
        if (!voice.active)
            return &voice;
    // Steal: released voices first, then the quietest, oldest on ties.
    Voice* victim = nullptr;
    for (Voice& voice : voices_) {
        if (victim == nullptr) {
            victim = &voice;
            continue;
        }
        const bool candidate_released = voice.released && !voice.sustained;
        const bool victim_released = victim->released && !victim->sustained;
        if (candidate_released != victim_released) {
            if (candidate_released)
                victim = &voice;
            continue;
        }
        if (voice.level < victim->level || (voice.level == victim->level && voice.serial < victim->serial))
            victim = &voice;
    }
    if (victim == nullptr)
        return nullptr;
    steals_.fetch_add(1, std::memory_order_relaxed);
    for (Voice& slot : fade_voices_) {
        if (!slot.active) {
            slot = *victim;
            slot.fading = true;
            slot.fade_step = 1.0f / (steal_ramp_seconds * static_cast<float>(sample_rate_));
            break;
        }
    }
    return victim;
}

void BuiltinSynth::start_voice(Voice& voice, std::uint8_t channel, std::uint8_t note, std::uint8_t velocity) noexcept {
    const Channel& state = (*channels_)[channel];
    const float rate = static_cast<float>(sample_rate_);
    voice = Voice{};
    voice.active = true;
    voice.channel = channel;
    voice.note = note;
    voice.serial = ++voice_serial_;
    const float normalized_velocity = static_cast<float>(velocity) / 127.0f;
    voice.velocity_gain = normalized_velocity * normalized_velocity;
    voice.noise = pcg32(voice.serial * 0x9E3779B97F4A7C15ull + note);
    voice.stage = Stage::attack;

    const bool percussion = channel == 9 || state.bank_msb == 120;
    if (percussion) {
        const DrumParameters drum = drum_for_note(note);
        voice.drum = drum.kind;
        voice.base_frequency = drum.tone_hz;
        voice.sweep_hz = drum.sweep_hz;
        voice.sweep_coefficient = coefficient(drum.sweep_seconds, rate);
        voice.tone_mix = drum.tone_mix;
        voice.noise_mix = drum.noise_mix;
        voice.attack_step = 1.0f / std::max(0.0005f * rate, 1.0f);
        voice.decay_coefficient = coefficient(drum.decay_seconds, rate);
        voice.sustain_level = 0.0f;
        voice.release_coefficient = voice.decay_coefficient;
        const float nyquist_scale = 1.0f / rate;
        switch (drum.kind) {
        case DrumKind::snare:
            voice.filter.bandpassQ(1800.0f * nyquist_scale, 0.9);
            break;
        case DrumKind::clap:
            voice.filter.bandpassQ(1200.0f * nyquist_scale, 1.4);
            break;
        case DrumKind::closed_hat:
        case DrumKind::open_hat:
            voice.filter.highpassQ(7000.0f * nyquist_scale, 0.707);
            break;
        case DrumKind::crash:
        case DrumKind::ride:
            voice.filter.highpassQ(5000.0f * nyquist_scale, 0.707);
            break;
        case DrumKind::rim:
            voice.filter.highpassQ(1500.0f * nyquist_scale, 0.707);
            break;
        case DrumKind::kick:
            voice.filter.lowpassQ(1800.0f * nyquist_scale, 0.707);
            break;
        case DrumKind::tom:
            voice.filter.lowpassQ(3000.0f * nyquist_scale, 0.707);
            break;
        case DrumKind::none:
            voice.filter.lowpassQ(8000.0f * nyquist_scale, 0.707);
            break;
        }
        return;
    }

    const int forced = timbre_override_[channel].load(std::memory_order_relaxed);
    voice.timbre = forced >= 0 ? static_cast<Timbre>(forced) : timbre_for_program(state.program);
    const TimbreParameters parameters = timbre_parameters(voice.timbre);
    voice.base_frequency = 440.0f * std::exp2((static_cast<float>(note) - 69.0f) / 12.0f);
    voice.attack_step = 1.0f / std::max(parameters.attack_seconds * rate, 1.0f);
    voice.decay_coefficient = coefficient(parameters.decay_seconds, rate);
    voice.sustain_level = parameters.sustain_level;
    voice.release_coefficient = coefficient(parameters.release_seconds, rate);
    if (voice.timbre == Timbre::electric_piano) {
        voice.modulation_index = 1.2f + 2.0f * normalized_velocity;
        voice.modulation_decay = coefficient(0.35f, rate);
    }
    // Brighter with velocity for the filtered timbres, never above 0.45 of the sample rate.
    const float cutoff = std::min(parameters.cutoff_hz * (0.6f + 0.8f * normalized_velocity), 0.45f * rate);
    voice.filter.lowpassQ(cutoff / rate, parameters.resonance_q);
}

void BuiltinSynth::note_on(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity) noexcept {
    Voice* voice = allocate_voice();
    if (voice == nullptr) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    start_voice(*voice, channel, note, velocity);
}

void BuiltinSynth::note_off(std::uint8_t channel, std::uint8_t note) noexcept {
    const bool sustain = (*channels_)[channel].sustain;
    for (Voice& voice : voices_) {
        if (!voice.active || voice.released || voice.sustained || voice.channel != channel || voice.note != note)
            continue;
        if (voice.drum != DrumKind::none)
            continue; // One-shot percussion ignores note-off.
        if (sustain) {
            voice.sustained = true;
        } else {
            voice.released = true;
            voice.stage = Stage::release;
        }
    }
}

void BuiltinSynth::panic() noexcept {
    for (Voice& voice : voices_)
        if (voice.active) {
            voice.fading = true;
            voice.fade_step = 1.0f / (steal_ramp_seconds * static_cast<float>(sample_rate_));
        }
    for (Channel& channel : *channels_) {
        channel.sustain = false;
        channel.bend = 0.0f;
    }
}

namespace {

/// Advances the amplitude envelope one sample; returns false when the voice has finished.
bool advance_envelope(BuiltinSynth::Voice& voice) noexcept;

} // namespace

void BuiltinSynth::render(float* interleaved_stereo, std::uint32_t frame_count) noexcept {
    if (interleaved_stereo == nullptr || frame_count == 0)
        return;
    const float rate = static_cast<float>(sample_rate_);
    constexpr std::size_t channel_count = 17;
    std::array<float, channel_count> pitch_offset{};
    std::array<float, channel_count> left_gain{};
    std::array<float, channel_count> right_gain{};
    const float block_seconds = static_cast<float>(frame_count) / rate;
    for (std::size_t index = 0; index < channel_count; ++index) {
        Channel& channel = (*channels_)[index];
        const float vibrato_depth = std::min(1.0f, channel.modulation + 0.5f * channel.pressure) * 0.5f; // semitones
        const float vibrato = vibrato_depth * std::sin(two_pi * channel.vibrato_phase);
        channel.vibrato_phase = wrap(channel.vibrato_phase + 5.5f * block_seconds);
        pitch_offset[index] = channel.bend * channel.bend_range + vibrato;
        const float gain = channel.volume * channel.volume * channel.expression * channel.expression *
                           settings_.master_gain;
        left_gain[index] = gain * std::cos(channel.pan * std::numbers::pi_v<float> * 0.5f);
        right_gain[index] = gain * std::sin(channel.pan * std::numbers::pi_v<float> * 0.5f);
    }

    auto render_voice = [&](Voice& voice) {
        const std::size_t channel = voice.channel;
        const float frequency = voice.drum == DrumKind::none
                                    ? voice.base_frequency * std::exp2(pitch_offset[channel] / 12.0f)
                                    : voice.base_frequency;
        const float step = std::min(frequency / rate, 0.49f);
        const float left = left_gain[channel] * voice.velocity_gain;
        const float right = right_gain[channel] * voice.velocity_gain;
        for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
            float sample = 0.0f;
            if (voice.drum != DrumKind::none) {
                const float tone_step = std::min((voice.base_frequency + voice.sweep_hz) / rate, 0.49f);
                voice.sweep_hz *= voice.sweep_coefficient;
                voice.phase_primary = wrap(voice.phase_primary + tone_step);
                const float tone = voice.tone_mix > 0.0f ? std::sin(two_pi * voice.phase_primary) : 0.0f;
                const float noise = static_cast<float>(voice.noise()) * (2.0f / 4294967296.0f) - 1.0f;
                sample = voice.filter(tone * voice.tone_mix + noise * voice.noise_mix);
            } else {
                switch (voice.timbre) {
                case Timbre::classic:
                    sample = 0.7f * std::sin(two_pi * voice.phase_primary) + 0.3f * triangle(voice.phase_primary);
                    break;
                case Timbre::saw_pad: {
                    const float detuned = step * 1.0058f;
                    sample = 0.5f * (saw(voice.phase_primary, step) + saw(voice.phase_secondary, detuned));
                    voice.phase_secondary = wrap(voice.phase_secondary + detuned);
                    break;
                }
                case Timbre::square_lead:
                    sample = 0.6f * square(voice.phase_primary, step);
                    break;
                case Timbre::electric_piano: {
                    const float modulator = std::sin(two_pi * voice.phase_secondary);
                    sample = std::sin(two_pi * voice.phase_primary + voice.modulation_index * modulator);
                    voice.modulation_index *= voice.modulation_decay;
                    voice.phase_secondary = wrap(voice.phase_secondary + step);
                    break;
                }
                case Timbre::bass:
                    sample = 0.55f * saw(voice.phase_primary, step) + 0.6f * std::sin(two_pi * voice.phase_sub);
                    voice.phase_sub = wrap(voice.phase_sub + step * 0.5f);
                    break;
                }
                voice.phase_primary = wrap(voice.phase_primary + step);
                sample = voice.filter(sample);
            }
            if (!advance_envelope(voice)) {
                voice.active = false;
                return;
            }
            float gain = voice.level;
            if (voice.fading) {
                voice.fade_gain -= voice.fade_step;
                if (voice.fade_gain <= 0.0f) {
                    voice.active = false;
                    return;
                }
                gain *= voice.fade_gain;
            }
            interleaved_stereo[2 * frame] += sample * gain * left;
            interleaved_stereo[2 * frame + 1] += sample * gain * right;
        }
    };

    std::size_t active = 0;
    for (Voice& voice : voices_) {
        if (!voice.active)
            continue;
        render_voice(voice);
        active += voice.active ? 1u : 0u;
    }
    for (Voice& voice : fade_voices_)
        if (voice.active)
            render_voice(voice);
    active_.store(active, std::memory_order_relaxed);
}

namespace {

bool advance_envelope(BuiltinSynth::Voice& voice) noexcept {
    switch (voice.stage) {
    case Stage::attack:
        voice.level += voice.attack_step;
        if (voice.level >= 1.0f) {
            voice.level = 1.0f;
            voice.stage = Stage::decay;
        }
        return true;
    case Stage::decay:
        voice.level = voice.sustain_level + (voice.level - voice.sustain_level) * voice.decay_coefficient;
        if (voice.level - voice.sustain_level < silence_threshold) {
            if (voice.sustain_level <= 0.0f)
                return false;
            voice.level = voice.sustain_level;
            voice.stage = Stage::sustain;
        }
        return true;
    case Stage::sustain:
        return true;
    case Stage::release:
        voice.level *= voice.release_coefficient;
        return voice.level >= silence_threshold;
    }
    return false;
}

} // namespace

} // namespace mwb::audio
