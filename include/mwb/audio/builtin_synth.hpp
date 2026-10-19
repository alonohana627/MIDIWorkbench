#pragma once

#include "mwb/audio/synth.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace mwb::audio {

/// Deterministic oscillator timbres of the built-in audition synth (section 13.1).
enum class Timbre : std::uint8_t { classic, saw_pad, square_lead, electric_piano, bass };

/// Stable identifier ("classic", "saw-pad", ...).
[[nodiscard]] std::string_view timbre_name(Timbre timbre) noexcept;
/// Parses a timbre identifier as stored in InstrumentSettings::synth_preset.
[[nodiscard]] std::optional<Timbre> parse_timbre(std::string_view name) noexcept;
/// General MIDI program family -> timbre mapping used when a channel has no explicit timbre.
[[nodiscard]] Timbre timbre_for_program(std::uint8_t program) noexcept;

/// Construction settings of the built-in synth.
struct BuiltinSynthSettings {
    std::size_t voice_capacity = 256;   ///< Preallocated voices, 1..512 (section 13.2).
    float master_gain = 0.25f;          ///< Linear output gain applied to every voice.
};

/// Always-available audition synthesizer: preallocated voice pool, five oscillator timbres and a
/// small synthesized percussion kit used for channel 10 or bank MSB 120 (GM2 rhythm bank). Voice
/// stealing takes released voices first, then the quietest (oldest on ties) voice, which is moved
/// to a separate fade slot and ramped out over a few milliseconds so the steal does not click.
///
/// Understood messages: note on/off (release velocity ignored), CC1 vibrato, CC7 volume, CC10 pan,
/// CC11 expression, CC64 sustain, CC0/32 bank, RPN 0 pitch-bend range, CC120/121/123, program
/// change, pitch bend and channel pressure (adds vibrato depth). Poly pressure is ignored. This is
/// an audition instrument with selected timbres, not a General MIDI sound set.
class BuiltinSynth final : public ISynth {
public:
    /// Validates the settings and allocates the voice pool (off the real-time thread).
    [[nodiscard]] static Result<std::unique_ptr<BuiltinSynth>> create(const BuiltinSynthSettings& settings = {});
    ~BuiltinSynth() override;

    [[nodiscard]] Status prepare(std::uint32_t sample_rate, std::uint32_t max_block_frames) override;
    void dispatch(const playback::RealtimeMidiEvent& event) noexcept override;
    /// Plays every live message on the private audition channel (index 16, after the sixteen MIDI
    /// channels), whatever channel its status byte names.
    void dispatch_live(const playback::RealtimeMidiEvent& event) noexcept override;
    void render(float* interleaved_stereo, std::uint32_t frame_count) noexcept override;
    void panic() noexcept override;
    [[nodiscard]] SynthIdentity identity() const override;
    [[nodiscard]] std::size_t active_voices() const noexcept override;

    /// Forces a timbre on a channel (nullopt returns to the program mapping). Real-time safe.
    void set_channel_timbre(std::uint8_t channel, std::optional<Timbre> timbre) noexcept;
    /// Timbre of the private audition channel (any thread).
    void set_audition_timbre(std::optional<Timbre> timbre) noexcept;
    /// Index of the private audition channel.
    static constexpr std::uint8_t audition_channel = 16;
    /// Voices taken from sounding notes since prepare().
    [[nodiscard]] std::uint64_t steal_count() const noexcept { return steals_.load(std::memory_order_relaxed); }
    /// Note-ons that could not get any voice (only when a stolen voice could not be reused).
    [[nodiscard]] std::uint64_t dropped_notes() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t voice_capacity() const noexcept;

    struct Voice;
    struct Channel;

private:
    explicit BuiltinSynth(const BuiltinSynthSettings& settings);
    void note_on(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity) noexcept;
    void note_off(std::uint8_t channel, std::uint8_t note) noexcept;
    void control_change(std::uint8_t channel, std::uint8_t controller, std::uint8_t value) noexcept;
    void dispatch_on(std::uint8_t channel, const playback::RealtimeMidiEvent& event) noexcept;
    void start_voice(Voice& voice, std::uint8_t channel, std::uint8_t note, std::uint8_t velocity) noexcept;
    Voice* allocate_voice() noexcept;

    BuiltinSynthSettings settings_;
    std::vector<Voice> voices_;       ///< Main pool.
    std::vector<Voice> fade_voices_;  ///< Ramp-out slots for stolen voices.
    /// Sixteen MIDI channels plus the private audition channel.
    std::unique_ptr<std::array<Channel, 17>> channels_;
    std::array<std::atomic<int>, 17> timbre_override_{};
    std::uint32_t sample_rate_ = 48000;
    std::uint64_t voice_serial_ = 0;
    std::atomic<std::uint64_t> steals_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::size_t> active_{0};
};

} // namespace mwb::audio
