#pragma once

#include "mwb/audio/synth.hpp"
#include "mwb/core/error.hpp"
#include "mwb/core/time.hpp"
#include "mwb/playback/snapshot.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>

namespace mwb::audio {

/// WAV sample formats (section 15).
enum class WavSampleFormat : std::uint8_t { pcm16, pcm24, float32 };

[[nodiscard]] const char* wav_sample_format_name(WavSampleFormat format) noexcept;

/// Offline render request. The snapshot passed alongside decides tracks, mute/solo inclusion,
/// preview transpose and tempo override; it must have been compiled at `sample_rate`.
struct RenderSettings {
    std::filesystem::path output;          ///< Target .wav path (replaced only on success).
    WavSampleFormat format = WavSampleFormat::pcm24;
    std::uint32_t sample_rate = 48000;     ///< 44100, 48000 or 96000.
    std::optional<TimeRange> range;        ///< Tick range; whole sequence [0, end) when absent.
    double tail_seconds = 2.0;             ///< 0..30 s rendered after sustain release at the end.
    bool normalize = false;                ///< Explicit peak-normalization gain pass.
    double normalize_peak_dbfs = -1.0;     ///< Target peak of the normalization pass.
    bool dither = true;                    ///< Seeded TPDF dither for integer formats.
    std::uint64_t dither_seed = 0x4D5742;  ///< PCG32 seed (section 10.2).
    bool include_metronome = false;        ///< Clicks are excluded unless requested.
    std::uint32_t block_frames = 512;      ///< Render block size (does not change the result).
    /// Optional progress callback (fraction 0..1), called from the rendering thread.
    std::function<void(double)> progress;
};

/// Everything that identifies how a render was produced (section 15 provenance).
struct RenderProvenance {
    std::string source_semantic_hash;  ///< Hex SHA-256 of the rendered sequence.
    std::uint64_t snapshot_generation = 0;
    std::string synth_engine;
    std::string synth_build;
    std::string bank_sha256;
    std::string preset;
    std::uint32_t sample_rate = 0;
    std::string sample_format;
    bool dither = false;
    std::uint64_t dither_seed = 0;
    double tail_seconds = 0.0;
    bool normalize = false;
    double normalize_peak_dbfs = 0.0;
    std::string tempo_multiplier;
    std::int32_t preview_transpose = 0;
    Tick range_begin = 0;
    Tick range_end = 0;
    std::size_t routes = 0;
    bool metronome = false;

    /// Stable "key=value" lines (one per field) for reports and cache keys.
    [[nodiscard]] std::string to_text() const;
};

/// Result of a successful render.
struct RenderReport {
    std::filesystem::path output;
    std::uint64_t total_frames = 0;     ///< Musical range plus tail.
    std::uint64_t musical_frames = 0;
    std::uint64_t tail_frames = 0;
    double peak_before = 0.0;           ///< Linear peak before the gain pass.
    double peak_after = 0.0;            ///< Linear peak after gain (before integer conversion).
    double gain = 1.0;                  ///< Linear normalization gain (1 when not normalizing).
    std::uint64_t clipped_samples = 0;  ///< Samples beyond full scale after gain.
    RenderProvenance provenance;
};

/// Renders a snapshot to WAV with the same scheduler, event order and synth implementation as live
/// playback (section 15): the scheduler is started at the range start with controller chase, the
/// musical range is rendered block by block, then owned notes and sustain are released and the
/// tail is rendered. The WAV is written by miniaudio's encoder (dr_wav) to a sibling temporary
/// file that replaces the target only after a complete, flushed write; cancellation or failure
/// removes the temporary file and leaves an existing target untouched. Normalization renders twice
/// (the synth is re-prepared, so both passes are identical). `soundfont` serves SoundFont routes
/// when given. Output above the 4 GiB RIFF limit is refused (see the module notes on RF64).
[[nodiscard]] Result<RenderReport> render_to_wav(const playback::PlaybackSnapshot& snapshot, ISynth& synth,
                                                 const RenderSettings& settings, std::stop_token stop = {},
                                                 ISynth* soundfont = nullptr);

} // namespace mwb::audio
