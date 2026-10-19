#include "mwb/audio/soundfont_synth.hpp"

#include "mwb/core/hashing.hpp"

#define TSF_IMPLEMENTATION
#include <tsf.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <limits>
#include <system_error>
#include <tuple>

namespace mwb::audio {

/// Owner of the TinySoundFont handle.
struct SoundFontSynth::Engine {
    tsf* handle = nullptr;
    ~Engine() {
        if (handle != nullptr)
            tsf_close(handle);
    }
};

namespace {

Status validate_options(const SoundFontOptions& options) {
    if (options.max_voices < 1 || options.max_voices > 1024)
        return make_error(ErrorCode::invalid_argument, "SoundFont voice count must be between 1 and 1024");
    if (options.max_file_bytes == 0)
        return make_error(ErrorCode::invalid_argument, "SoundFont size limit must be positive");
    return ok();
}

} // namespace

SoundFontSynth::SoundFontSynth(std::unique_ptr<Engine> engine, SoundFontOptions options, std::string sha256)
    : engine_(std::move(engine)), options_(options), bank_sha256_(std::move(sha256)) {
    const int count = tsf_get_presetcount(engine_->handle);
    presets_.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int index = 0; index < count; ++index) {
        const tsf_preset& preset = engine_->handle->presets[index];
        const char* name = tsf_get_presetname(engine_->handle, index);
        presets_.push_back(SoundFontPreset{index, preset.bank, preset.preset, name != nullptr ? std::string(name) : std::string{}});
    }
    std::ranges::sort(presets_, [](const SoundFontPreset& left, const SoundFontPreset& right) {
        return left.bank != right.bank ? left.bank < right.bank : left.program < right.program;
    });
}

SoundFontSynth::~SoundFontSynth() = default;

Result<std::unique_ptr<SoundFontSynth>> SoundFontSynth::load_memory(std::span<const std::uint8_t> bytes,
                                                                    const SoundFontOptions& options) {
    MWB_TRY(validate_options(options));
    if (bytes.size() > options.max_file_bytes)
        return make_error(ErrorCode::limit_exceeded,
                          std::format("the SoundFont is larger than the {} MiB limit", options.max_file_bytes >> 20));
    if (bytes.size() < 12 || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return make_error(ErrorCode::invalid_data, "the file is not a SoundFont 2 bank");
    const auto riff = std::string_view(reinterpret_cast<const char*>(bytes.data()), 4);
    const auto form = std::string_view(reinterpret_cast<const char*>(bytes.data() + 8), 4);
    if (riff != "RIFF" || form != "sfbk")
        return make_error(ErrorCode::invalid_data, "the file is not a SoundFont 2 bank (missing RIFF sfbk header)");
    auto engine = std::make_unique<Engine>();
    engine->handle = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
    if (engine->handle == nullptr)
        return make_error(ErrorCode::invalid_data, "TinySoundFont could not parse the SoundFont bank");
    if (tsf_get_presetcount(engine->handle) <= 0)
        return make_error(ErrorCode::invalid_data, "the SoundFont bank contains no presets");
    return std::unique_ptr<SoundFontSynth>(new SoundFontSynth(std::move(engine), options, to_hex(sha256(bytes))));
}

Result<std::unique_ptr<SoundFontSynth>> SoundFontSynth::load_file(const std::filesystem::path& path,
                                                                  const SoundFontOptions& options) {
    MWB_TRY(validate_options(options));
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return make_error(ErrorCode::io, "cannot read the SoundFont file size", error.message());
    if (size > options.max_file_bytes)
        return make_error(ErrorCode::limit_exceeded,
                          std::format("the SoundFont is larger than the {} MiB limit", options.max_file_bytes >> 20));
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return make_error(ErrorCode::io, "cannot open the SoundFont file");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size()))
        return make_error(ErrorCode::io, "the SoundFont file could not be read completely");
    return load_memory(bytes, options);
}

Status SoundFontSynth::prepare(std::uint32_t sample_rate, std::uint32_t max_block_frames) {
    if (sample_rate < 8000 || sample_rate > 384000)
        return make_error(ErrorCode::invalid_argument, "unsupported sample rate for the SoundFont synth");
    if (max_block_frames == 0)
        return make_error(ErrorCode::invalid_argument, "block size must be positive");
    tsf* handle = engine_->handle;
    tsf_reset(handle);
    tsf_set_output(handle, TSF_STEREO_INTERLEAVED, static_cast<int>(sample_rate), options_.gain_db);
    if (tsf_set_max_voices(handle, options_.max_voices) == 0)
        return make_error(ErrorCode::internal, "cannot preallocate SoundFont voices");
    // Initializes all 16 channels now (TinySoundFont allocates channel state on first use).
    if (tsf_channel_set_pitchwheel(handle, 15, 8192) == 0)
        return make_error(ErrorCode::internal, "cannot allocate SoundFont channel state");
    bank_msb_.fill(0);
    program_.fill(0);
    for (int channel = 0; channel < 16; ++channel) {
        std::ignore = tsf_channel_midi_control(handle, channel, 121, 0); // Always succeeds once allocated.
        std::ignore = tsf_channel_set_pitchwheel(handle, channel, 8192); // Same.
        select_program(channel);
    }
    prepared_ = true;
    active_.store(0, std::memory_order_relaxed);
    return ok();
}

void SoundFontSynth::select_program(int channel) noexcept {
    tsf* handle = engine_->handle;
    const auto index = static_cast<std::size_t>(channel);
    const int program = program_[index];
    if (channel == 9 || bank_msb_[index] == 120) {
        std::ignore = tsf_channel_set_bank(handle, channel, 0); // Channels are preallocated.
        std::ignore = tsf_channel_set_presetnumber(handle, channel, program, 1); // Falls back internally.
        return;
    }
    if (tsf_channel_set_bank_preset(handle, channel, bank_msb_[index], program) != 0)
        return;
    if (tsf_channel_set_bank_preset(handle, channel, 0, program) != 0)
        return;
    std::ignore = tsf_channel_set_presetindex(handle, channel, 0); // Keep a playable preset selected.
}

void SoundFontSynth::dispatch(const playback::RealtimeMidiEvent& event) noexcept {
    if (!prepared_ || event.kind != playback::RealtimeEventKind::midi || event.size == 0)
        return;
    tsf* handle = engine_->handle;
    const int channel = event.channel();
    switch (event.message()) {
    case 0x90:
        if (event.data2() > 0) {
            std::ignore = tsf_channel_note_on(handle, channel, event.data1(), static_cast<float>(event.data2()) / 127.0f);
            return; // A zero result means no voice was free; nothing else to do in real time.
        }
        tsf_channel_note_off(handle, channel, event.data1());
        return;
    case 0x80:
        tsf_channel_note_off(handle, channel, event.data1());
        return;
    case 0xB0:
        if (event.data1() == 0) {
            bank_msb_[static_cast<std::size_t>(channel)] = event.data2();
            return;
        }
        if (event.data1() == 32)
            return; // Bank LSB is not used for SF2 bank numbers.
        std::ignore = tsf_channel_midi_control(handle, channel, event.data1(), event.data2()); // Channels exist.
        return;
    case 0xC0:
        program_[static_cast<std::size_t>(channel)] = event.data1();
        select_program(channel);
        return;
    case 0xE0:
        std::ignore = tsf_channel_set_pitchwheel(handle, channel, event.data1() | (event.data2() << 7)); // Same.
        return;
    default:
        return; // Pressure and poly pressure are not supported by TinySoundFont.
    }
}

void SoundFontSynth::render(float* interleaved_stereo, std::uint32_t frame_count) noexcept {
    if (!prepared_ || interleaved_stereo == nullptr || frame_count == 0)
        return;
    tsf_render_float(engine_->handle, interleaved_stereo, static_cast<int>(frame_count), 1);
    active_.store(static_cast<std::size_t>(std::max(tsf_active_voice_count(engine_->handle), 0)),
                  std::memory_order_relaxed);
}

void SoundFontSynth::panic() noexcept {
    if (!prepared_)
        return;
    for (int channel = 0; channel < 16; ++channel) {
        tsf_channel_sounds_off_all(engine_->handle, channel);
        std::ignore = tsf_channel_set_sustain(engine_->handle, channel, 0);       // Channels exist.
        std::ignore = tsf_channel_set_pitchwheel(engine_->handle, channel, 8192); // Same.
    }
}

SynthIdentity SoundFontSynth::identity() const {
    return SynthIdentity{"sf2", "TinySoundFont git-853a0a1 (vendored)", bank_sha256_,
                         std::format("{} presets, {} voices", presets_.size(), options_.max_voices)};
}

std::size_t SoundFontSynth::active_voices() const noexcept { return active_.load(std::memory_order_relaxed); }

} // namespace mwb::audio
