#pragma once

#include "mwb/audio/builtin_synth.hpp"
#include "mwb/audio/metronome.hpp"
#include "mwb/audio/synth.hpp"
#include "mwb/playback/realtime_event.hpp"
#include "mwb/playback/snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mwb::audio {

/// Where events of one route are delivered.
enum class Destination : std::uint8_t { builtin_synth, soundfont_synth, external_midi };

/// Shared render path of live playback and offline rendering (sections 13.4 and 15): an event sink
/// that renders the synths in sub-segments between events, so every event takes effect at its
/// exact sample offset. Routes are bound to destinations from the snapshot: internal-synth routes
/// go to the built-in synth, SoundFont routes to the SoundFont synth when one is loaded (else the
/// built-in synth), external routes to an external sink; tempo markers are ignored and metronome
/// clicks go to the metronome. Everything after construction is real-time safe.
class BlockRenderer final : public playback::IEventSink {
public:
    /// Preallocates the route destination table.
    explicit BlockRenderer(std::size_t route_capacity = 256);

    /// Sets the instruments (any may be null). Call while no block is in progress.
    void set_instruments(ISynth* builtin, ISynth* soundfont, MetronomeSynth* metronome) noexcept;
    /// Applies route timbres (InstrumentSettings::synth_preset) to this built-in synth on bind().
    void set_builtin_timbre_target(BuiltinSynth* synth) noexcept { timbre_target_ = synth; }
    /// Receives events of external-MIDI routes (frame offsets preserved).
    void set_external_sink(playback::IEventSink* sink) noexcept { external_ = sink; }
    /// Binds route destinations of `snapshot`; false when it has more routes than the capacity.
    [[nodiscard]] bool bind(const playback::PlaybackSnapshot* snapshot) noexcept;

    /// Starts a block: zeroes `frame_count` stereo frames of `interleaved_stereo`.
    void begin(float* interleaved_stereo, std::uint32_t frame_count) noexcept;
    /// Renders up to the event offset, then delivers the event.
    void on_event(const playback::RealtimeMidiEvent& event) noexcept override;
    /// Delivers a live-input event to the built-in synth's private audition channel (or the SoundFont synth when
    /// `to_soundfont`), at its offset inside the current block.
    void dispatch_live(const playback::RealtimeMidiEvent& event, bool to_soundfont = false) noexcept;
    /// Renders the rest of the block.
    void finish() noexcept;

    /// Events delivered to synths and the external sink in the current block.
    [[nodiscard]] std::size_t block_events() const noexcept { return block_events_; }

private:
    void render_until(std::uint32_t offset) noexcept;
    ISynth* synth_for(Destination destination) const noexcept;

    std::vector<Destination> destinations_;
    std::size_t route_count_ = 0;
    ISynth* builtin_ = nullptr;
    ISynth* soundfont_ = nullptr;
    MetronomeSynth* metronome_ = nullptr;
    BuiltinSynth* timbre_target_ = nullptr;
    playback::IEventSink* external_ = nullptr;
    float* output_ = nullptr;
    std::uint32_t frames_ = 0;
    std::uint32_t rendered_ = 0;
    std::size_t block_events_ = 0;
};

} // namespace mwb::audio
