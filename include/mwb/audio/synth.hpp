#pragma once

#include "mwb/core/error.hpp"
#include "mwb/playback/realtime_event.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace mwb::audio {

/// Identity of a synth instance for render provenance and preview cache keys (section 15).
struct SynthIdentity {
    std::string engine;       ///< "builtin" or "sf2".
    std::string build;        ///< Engine build/version string.
    std::string bank_sha256;  ///< SoundFont SHA-256 (empty for the built-in synth).
    std::string preset;       ///< Timbre set or bank description.
};

/// Synthesizer interface (specification section 19.2). prepare() and every loading or
/// configuration method run off the real-time thread and return Result/Status. dispatch(),
/// render() and panic() are the real-time operations on prepared state: they never allocate,
/// lock, log or throw.
class ISynth {
public:
    virtual ~ISynth() = default;

    /// Prepares for rendering at `sample_rate` in blocks of at most `max_block_frames` and resets
    /// every voice and channel to power-on state. Off the real-time thread.
    [[nodiscard]] virtual Status prepare(std::uint32_t sample_rate, std::uint32_t max_block_frames) = 0;
    /// Applies one MIDI short message immediately (frame offsets are handled by the caller, which
    /// renders sub-segments between events). Non-MIDI kinds are ignored.
    virtual void dispatch(const playback::RealtimeMidiEvent& event) noexcept = 0;
    /// Renders `frame_count` frames and ADDS them into `interleaved_stereo` (L, R, L, R, ...).
    virtual void render(float* interleaved_stereo, std::uint32_t frame_count) noexcept = 0;
    /// Silences every voice quickly and resets controllers that could hold notes (sustain).
    virtual void panic() noexcept = 0;
    /// Identity for provenance. Off the real-time thread.
    [[nodiscard]] virtual SynthIdentity identity() const = 0;
    /// Number of currently sounding voices (diagnostics; real-time safe).
    [[nodiscard]] virtual std::size_t active_voices() const noexcept = 0;
};

} // namespace mwb::audio
