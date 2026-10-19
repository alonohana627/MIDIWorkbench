#include "mwb/audio/offline_renderer.hpp"

#include "mwb/audio/block_renderer.hpp"
#include "mwb/audio/builtin_synth.hpp"
#include "mwb/audio/metronome.hpp"
#include "mwb/core/hashing.hpp"
#include "mwb/core/random.hpp"
#include "mwb/playback/scheduler.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <system_error>
#include <cstring>
#include <tuple>
#include <vector>

namespace mwb::audio {

namespace {

/// Bytes per sample of each format.
std::uint32_t bytes_per_sample(WavSampleFormat format) noexcept {
    switch (format) {
    case WavSampleFormat::pcm16:
        return 2;
    case WavSampleFormat::pcm24:
        return 3;
    case WavSampleFormat::float32:
        return 4;
    }
    return 4;
}

/// miniaudio sample format of a WAV format.
ma_format miniaudio_format(WavSampleFormat format) noexcept {
    switch (format) {
    case WavSampleFormat::pcm16:
        return ma_format_s16;
    case WavSampleFormat::pcm24:
        return ma_format_s24;
    case WavSampleFormat::float32:
        return ma_format_f32;
    }
    return ma_format_f32;
}

/// Output stream used by the miniaudio encoder callbacks.
struct EncoderStream {
    std::ofstream file;
};

/// miniaudio encoder write callback: appends to the temporary file.
ma_result write_callback(ma_encoder* encoder, const void* buffer, size_t bytes, size_t* written) {
    auto* stream = static_cast<EncoderStream*>(encoder->pUserData);
    stream->file.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(bytes));
    if (!stream->file) {
        *written = 0;
        return MA_IO_ERROR;
    }
    *written = bytes;
    return MA_SUCCESS;
}

/// miniaudio encoder seek callback (header patching at finalization).
ma_result seek_callback(ma_encoder* encoder, ma_int64 offset, ma_seek_origin origin) {
    auto* stream = static_cast<EncoderStream*>(encoder->pUserData);
    std::ios_base::seekdir direction = std::ios_base::beg;
    switch (origin) {
    case ma_seek_origin_start:
        direction = std::ios_base::beg;
        break;
    case ma_seek_origin_current:
        direction = std::ios_base::cur;
        break;
    case ma_seek_origin_end:
        direction = std::ios_base::end;
        break;
    }
    stream->file.seekp(static_cast<std::streamoff>(offset), direction);
    return stream->file ? MA_SUCCESS : MA_IO_ERROR;
}

/// Removes the temporary file unless the render committed it.
class TemporaryFileGuard {
public:
    explicit TemporaryFileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryFileGuard() {
        if (!committed_) {
            std::error_code error;
            std::filesystem::remove(path_, error);
        }
    }
    TemporaryFileGuard(const TemporaryFileGuard&) = delete;
    TemporaryFileGuard& operator=(const TemporaryFileGuard&) = delete;
    void commit() noexcept { committed_ = true; }

private:
    std::filesystem::path path_;
    bool committed_ = false;
};

/// Unique sibling temporary path for `target`.
std::filesystem::path temporary_path_for(const std::filesystem::path& target) {
    static std::atomic<std::uint64_t> counter{0};
    const auto stamp = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    // Built in native form: path::string() throws for names outside the ANSI code page when the
    // process does not run with the UTF-8 code page (tests, tools embedding the renderer).
    std::filesystem::path name(".");
    name += target.filename();
    name += std::format(".partial-{:x}-{}.tmp", stamp, counter.fetch_add(1, std::memory_order_relaxed));
    return target.parent_path() / name;
}

/// Everything one render pass needs.
struct PassInput {
    const playback::PlaybackSnapshot* snapshot = nullptr;
    ISynth* synth = nullptr;
    ISynth* soundfont = nullptr;
    const RenderSettings* settings = nullptr;
    Tick start_tick = 0;
    std::uint64_t musical_frames = 0;
    std::uint64_t tail_frames = 0;
    std::stop_token stop;
    double progress_base = 0.0;   ///< Progress offset of this pass.
    double progress_span = 1.0;   ///< Progress share of this pass.
};

/// Renders one complete pass and hands every block to `consume`.
template <class Consumer>
Status render_pass(const PassInput& input, Consumer&& consume) {
    const RenderSettings& settings = *input.settings;
    MWB_TRY(input.synth->prepare(settings.sample_rate, settings.block_frames));
    if (input.soundfont != nullptr)
        MWB_TRY(input.soundfont->prepare(settings.sample_rate, settings.block_frames));
    MetronomeSynth metronome;
    MWB_TRY(metronome.prepare(settings.sample_rate));

    playback::SchedulerSettings scheduler_settings;
    scheduler_settings.loop_enabled = false;
    scheduler_settings.metronome = settings.include_metronome;
    scheduler_settings.route_capacity = std::max<std::size_t>(input.snapshot->routes.size(), 1);
    playback::Scheduler scheduler(scheduler_settings);
    BlockRenderer renderer(scheduler_settings.route_capacity);
    renderer.set_instruments(input.synth, input.soundfont, settings.include_metronome ? &metronome : nullptr);
    renderer.set_builtin_timbre_target(dynamic_cast<BuiltinSynth*>(input.synth));
    if (!scheduler.set_snapshot(input.snapshot, renderer) || !renderer.bind(input.snapshot))
        return make_error(ErrorCode::internal, "the render scheduler could not accept the snapshot");

    std::vector<float> buffer(static_cast<std::size_t>(settings.block_frames) * 2);
    renderer.begin(buffer.data(), 0);
    scheduler.start(input.start_tick, renderer);

    const std::uint64_t total = input.musical_frames + input.tail_frames;
    std::uint64_t done = 0;
    std::uint64_t block_index = 0;
    bool released = false;
    while (done < total || !released) {
        if (input.stop.stop_requested())
            return make_error(ErrorCode::cancelled, "the render was cancelled");
        std::uint64_t limit = total - done;
        if (done < input.musical_frames)
            limit = input.musical_frames - done; // Never cross the musical end inside one block.
        const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(settings.block_frames, limit));
        renderer.begin(buffer.data(), count);
        if (done >= input.musical_frames && !released) {
            // Musical end: release owned notes and sustain before the tail.
            scheduler.stop(renderer, 0);
            released = true;
        } else {
            std::ignore = scheduler.process_block(count, renderer); // End is driven by the frame count.
        }
        renderer.finish();
        if (count > 0)
            MWB_TRY(consume(buffer.data(), count));
        done += count;
        if (settings.progress && (++block_index & 63u) == 0 && total > 0)
            settings.progress(input.progress_base +
                              input.progress_span * static_cast<double>(done) / static_cast<double>(total));
    }
    return ok();
}

} // namespace

const char* wav_sample_format_name(WavSampleFormat format) noexcept {
    switch (format) {
    case WavSampleFormat::pcm16:
        return "pcm16";
    case WavSampleFormat::pcm24:
        return "pcm24";
    case WavSampleFormat::float32:
        return "float32";
    }
    return "float32";
}

std::string RenderProvenance::to_text() const {
    std::string text;
    text += std::format("source_semantic_hash={}\n", source_semantic_hash);
    text += std::format("snapshot_generation={}\n", snapshot_generation);
    text += std::format("synth_engine={}\n", synth_engine);
    text += std::format("synth_build={}\n", synth_build);
    text += std::format("bank_sha256={}\n", bank_sha256);
    text += std::format("preset={}\n", preset);
    text += std::format("sample_rate={}\n", sample_rate);
    text += std::format("sample_format={}\n", sample_format);
    text += std::format("dither={}\n", dither);
    text += std::format("dither_seed={}\n", dither_seed);
    text += std::format("tail_seconds={}\n", tail_seconds);
    text += std::format("normalize={}\n", normalize);
    text += std::format("normalize_peak_dbfs={}\n", normalize_peak_dbfs);
    text += std::format("tempo_multiplier={}\n", tempo_multiplier);
    text += std::format("preview_transpose={}\n", preview_transpose);
    text += std::format("range={}..{}\n", range_begin, range_end);
    text += std::format("routes={}\n", routes);
    text += std::format("metronome={}\n", metronome);
    return text;
}

Result<RenderReport> render_to_wav(const playback::PlaybackSnapshot& snapshot, ISynth& synth,
                                   const RenderSettings& settings, std::stop_token stop, ISynth* soundfont) {
    if (settings.sample_rate != 44100 && settings.sample_rate != 48000 && settings.sample_rate != 96000)
        return make_error(ErrorCode::invalid_argument, "render sample rate must be 44100, 48000 or 96000 Hz");
    if (snapshot.mapping.sample_rate() != settings.sample_rate)
        return make_error(ErrorCode::invalid_argument, "the snapshot was compiled for a different sample rate");
    if (!(settings.tail_seconds >= 0.0) || settings.tail_seconds > 30.0)
        return make_error(ErrorCode::invalid_argument, "tail must be between 0 and 30 seconds");
    if (settings.block_frames == 0 || settings.block_frames > 65536)
        return make_error(ErrorCode::invalid_argument, "render block size must be between 1 and 65536 frames");
    if (settings.output.empty() || !settings.output.has_filename())
        return make_error(ErrorCode::invalid_argument, "an output file name is required");
    const TimeRange range = settings.range.value_or(TimeRange{0, snapshot.end_tick});
    if (range.begin < 0 || range.end <= range.begin)
        return make_error(ErrorCode::invalid_argument, "the render range needs 0 <= begin < end");

    const std::int64_t start_frame = snapshot.mapping.frame_at(range.begin);
    const std::int64_t end_frame = snapshot.mapping.frame_at(range.end);
    RenderReport report;
    report.output = settings.output;
    report.musical_frames = static_cast<std::uint64_t>(std::max<std::int64_t>(end_frame - start_frame, 0));
    report.tail_frames = static_cast<std::uint64_t>(std::llround(settings.tail_seconds * settings.sample_rate));
    report.total_frames = report.musical_frames + report.tail_frames;
    const std::uint32_t sample_bytes = bytes_per_sample(settings.format);
    if (report.total_frames * 2 * sample_bytes > 0xFFFFFFFFull - 1024)
        return make_error(ErrorCode::limit_exceeded,
                          "the render exceeds the 4 GiB RIFF WAV limit; RF64 output is not available through the "
                          "miniaudio encoder");

    PassInput input;
    input.snapshot = &snapshot;
    input.synth = &synth;
    input.soundfont = soundfont;
    input.settings = &settings;
    input.start_tick = range.begin;
    input.musical_frames = report.musical_frames;
    input.tail_frames = report.tail_frames;
    input.stop = stop;

    // Pass 1 (normalization only): measure the peak.
    double gain = 1.0;
    if (settings.normalize) {
        input.progress_span = 0.5;
        double peak = 0.0;
        MWB_TRY(render_pass(input, [&](const float* samples, std::uint32_t frames) -> Status {
            for (std::size_t index = 0; index < static_cast<std::size_t>(frames) * 2; ++index)
                peak = std::max(peak, static_cast<double>(std::fabs(samples[index])));
            return ok();
        }));
        report.peak_before = peak;
        if (peak > 0.0)
            gain = std::pow(10.0, settings.normalize_peak_dbfs / 20.0) / peak;
        input.progress_base = 0.5;
    }
    report.gain = gain;

    // Pass 2: write.
    std::error_code directory_error;
    if (!std::filesystem::is_directory(settings.output.parent_path().empty() ? std::filesystem::path(".")
                                                                            : settings.output.parent_path(),
                                       directory_error))
        return make_error(ErrorCode::io, "the output directory does not exist");
    const std::filesystem::path temporary = temporary_path_for(settings.output);
    TemporaryFileGuard guard(temporary);
    EncoderStream stream;
    stream.file.open(temporary, std::ios::binary | std::ios::trunc);
    if (!stream.file)
        return make_error(ErrorCode::io, "cannot create the temporary render file");
    const ma_encoder_config config =
        ma_encoder_config_init(ma_encoding_format_wav, miniaudio_format(settings.format), 2, settings.sample_rate);
    ma_encoder encoder;
    if (ma_encoder_init(write_callback, seek_callback, &stream, &config, &encoder) != MA_SUCCESS)
        return make_error(ErrorCode::io, "cannot initialize the WAV encoder");
    bool encoder_open = true;
    auto close_encoder = [&]() {
        if (encoder_open) {
            ma_encoder_uninit(&encoder);
            encoder_open = false;
        }
    };

    DeterministicRandom dither_random(settings.dither_seed);
    const bool integer = settings.format != WavSampleFormat::float32;
    const double scale = settings.format == WavSampleFormat::pcm16 ? 32767.0 : 8388607.0;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(settings.block_frames) * 2 * sample_bytes);
    double peak_after = 0.0;
    std::uint64_t clipped = 0;
    const Status written = render_pass(input, [&](const float* samples, std::uint32_t frames) -> Status {
        const std::size_t count = static_cast<std::size_t>(frames) * 2;
        for (std::size_t index = 0; index < count; ++index) {
            const double value = static_cast<double>(samples[index]) * gain;
            const double magnitude = std::fabs(value);
            peak_after = std::max(peak_after, magnitude);
            if (magnitude > 1.0)
                ++clipped;
            if (!integer) {
                const auto sample = static_cast<float>(value);
                std::memcpy(bytes.data() + index * 4, &sample, 4);
                continue;
            }
            double scaled = std::clamp(value, -1.0, 1.0) * scale;
            if (settings.dither)
                scaled += dither_random.unit() - dither_random.unit(); // TPDF, +-1 LSB.
            const auto quantized = static_cast<std::int32_t>(
                std::clamp(std::nearbyint(scaled), -scale - 1.0, scale));
            const auto raw = static_cast<std::uint32_t>(quantized);
            bytes[index * sample_bytes] = static_cast<std::uint8_t>(raw & 0xFFu);
            bytes[index * sample_bytes + 1] = static_cast<std::uint8_t>((raw >> 8) & 0xFFu);
            if (sample_bytes == 3)
                bytes[index * sample_bytes + 2] = static_cast<std::uint8_t>((raw >> 16) & 0xFFu);
        }
        ma_uint64 frames_written = 0;
        if (ma_encoder_write_pcm_frames(&encoder, bytes.data(), frames, &frames_written) != MA_SUCCESS ||
            frames_written != frames)
            return make_error(ErrorCode::io, "writing the WAV data failed");
        return ok();
    });
    close_encoder();
    if (!written)
        return std::unexpected(written.error());
    stream.file.flush();
    const bool stream_ok = static_cast<bool>(stream.file);
    stream.file.close();
    if (!stream_ok || stream.file.fail())
        return make_error(ErrorCode::io, "finalizing the WAV file failed");
    if (stop.stop_requested())
        return make_error(ErrorCode::cancelled, "the render was cancelled");

    std::error_code rename_error;
    std::filesystem::rename(temporary, settings.output, rename_error);
    if (rename_error)
        return make_error(ErrorCode::io, "cannot replace the target WAV file", rename_error.message());
    guard.commit();

    if (!settings.normalize)
        report.peak_before = peak_after;
    report.peak_after = peak_after;
    report.clipped_samples = clipped;
    if (settings.progress)
        settings.progress(1.0);

    const SynthIdentity identity = synth.identity();
    RenderProvenance& provenance = report.provenance;
    provenance.source_semantic_hash = to_hex(snapshot.source_semantic_hash);
    provenance.snapshot_generation = snapshot.snapshot_generation;
    provenance.synth_engine = identity.engine;
    provenance.synth_build = identity.build;
    provenance.bank_sha256 = soundfont != nullptr ? soundfont->identity().bank_sha256 : identity.bank_sha256;
    provenance.preset = identity.preset;
    provenance.sample_rate = settings.sample_rate;
    provenance.sample_format = wav_sample_format_name(settings.format);
    provenance.dither = integer && settings.dither;
    provenance.dither_seed = settings.dither_seed;
    provenance.tail_seconds = settings.tail_seconds;
    provenance.normalize = settings.normalize;
    provenance.normalize_peak_dbfs = settings.normalize_peak_dbfs;
    provenance.tempo_multiplier = snapshot.session.tempo_multiplier.to_string();
    provenance.preview_transpose = snapshot.session.preview_transpose;
    provenance.range_begin = range.begin;
    provenance.range_end = range.end;
    provenance.routes = snapshot.routes.size();
    provenance.metronome = settings.include_metronome;
    return report;
}

} // namespace mwb::audio
