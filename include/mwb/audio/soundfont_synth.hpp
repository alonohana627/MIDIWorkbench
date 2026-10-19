#pragma once

#include "mwb/audio/synth.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace mwb::audio {

/// Limits and output settings for SoundFont loading.
struct SoundFontOptions {
    std::uint64_t max_file_bytes = 512ull * 1024 * 1024; ///< Section 28.3 default (512 MiB).
    int max_voices = 256;                                 ///< Preallocated voices (1..1024).
    float gain_db = 0.0f;                                 ///< Global output gain.
};

/// One preset of a loaded bank.
struct SoundFontPreset {
    int index = 0;        ///< TinySoundFont preset index.
    int bank = 0;         ///< SF2 bank number (128 = percussion).
    int program = 0;      ///< Preset (program) number.
    std::string name;
};

/// SoundFont 2 instrument backed by TinySoundFont (section 13.1, 13.2) behind ISynth.
///
/// Loading parses the bank off the real-time thread (rejecting files above the size limit before
/// reading them); prepare() sets the output format, preallocates the voice pool with
/// tsf_set_max_voices and initializes all 16 channels, after which note-on, controller changes and
/// rendering do not allocate (TinySoundFont only reallocates voices when no maximum is set, and
/// channels only on first use).
///
/// Behaviour on top of TinySoundFont: bank select MSB chooses the bank; channel 10 and bank MSB
/// 120 select percussion (bank 128); unknown presets fall back to the same program in bank 0.
///
/// Unsupported or limited, documented honestly:
/// - SF2 modulators (pmod/imod) are parsed by TinySoundFont but not applied; only its built-in
///   velocity, volume/expression, pan, pitch-wheel and sustain handling is used.
/// - No reverb or chorus sends (CC91/CC93 and generators 15/16 are ignored).
/// - Channel and polyphonic pressure, modulation wheel vibrato (CC1) and NRPNs are ignored.
/// - Exclusive classes are honoured; when every preallocated voice is busy, a new note can only
///   take a voice that is in its release phase, otherwise the note is not started.
/// - `.sf3` (compressed samples) is not supported.
class SoundFontSynth final : public ISynth {
public:
    /// Loads a bank from a file (size checked before reading).
    [[nodiscard]] static Result<std::unique_ptr<SoundFontSynth>> load_file(const std::filesystem::path& path,
                                                                           const SoundFontOptions& options = {});
    /// Reads a bank file's bytes (size checked against `options.max_file_bytes` before reading)
    /// without parsing them, so they can be validated in the worker first (section 28.2).
    [[nodiscard]] static Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path,
                                                                     const SoundFontOptions& options = {});
    /// Loads a bank from memory.
    [[nodiscard]] static Result<std::unique_ptr<SoundFontSynth>> load_memory(std::span<const std::uint8_t> bytes,
                                                                             const SoundFontOptions& options = {});
    ~SoundFontSynth() override;
    SoundFontSynth(const SoundFontSynth&) = delete;
    SoundFontSynth& operator=(const SoundFontSynth&) = delete;

    [[nodiscard]] Status prepare(std::uint32_t sample_rate, std::uint32_t max_block_frames) override;
    void dispatch(const playback::RealtimeMidiEvent& event) noexcept override;
    void render(float* interleaved_stereo, std::uint32_t frame_count) noexcept override;
    void panic() noexcept override;
    [[nodiscard]] SynthIdentity identity() const override;
    [[nodiscard]] std::size_t active_voices() const noexcept override;

    /// Presets of the bank ordered by (bank, program).
    [[nodiscard]] const std::vector<SoundFontPreset>& presets() const noexcept { return presets_; }
    /// Lowercase hex SHA-256 of the bank bytes.
    [[nodiscard]] const std::string& bank_sha256() const noexcept { return bank_sha256_; }

private:
    struct Engine;
    SoundFontSynth(std::unique_ptr<Engine> engine, SoundFontOptions options, std::string sha256);
    void select_program(int channel) noexcept;

    std::unique_ptr<Engine> engine_;
    SoundFontOptions options_;
    std::string bank_sha256_;
    std::vector<SoundFontPreset> presets_;
    std::array<std::uint8_t, 16> bank_msb_{};
    std::array<std::uint8_t, 16> program_{};
    bool prepared_ = false;
    std::atomic<std::size_t> active_{0};
};

} // namespace mwb::audio
