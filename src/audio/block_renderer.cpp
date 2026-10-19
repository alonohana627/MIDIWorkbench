#include "mwb/audio/block_renderer.hpp"

#include <algorithm>

namespace mwb::audio {

BlockRenderer::BlockRenderer(std::size_t route_capacity)
    : destinations_(std::max<std::size_t>(route_capacity, 1), Destination::builtin_synth) {}

void BlockRenderer::set_instruments(ISynth* builtin, ISynth* soundfont, MetronomeSynth* metronome) noexcept {
    builtin_ = builtin;
    soundfont_ = soundfont;
    metronome_ = metronome;
}

bool BlockRenderer::bind(const playback::PlaybackSnapshot* snapshot) noexcept {
    if (snapshot == nullptr) {
        route_count_ = 0;
        return true;
    }
    if (snapshot->routes.size() > destinations_.size())
        return false;
    route_count_ = snapshot->routes.size();
    for (std::size_t index = 0; index < route_count_; ++index) {
        const playback::Route& route = snapshot->routes[index];
        switch (route.output) {
        case OutputKind::internal_synth:
            destinations_[index] = Destination::builtin_synth;
            if (timbre_target_ != nullptr && !route.drum)
                timbre_target_->set_channel_timbre(route.channel, parse_timbre(route.instrument.synth_preset));
            break;
        case OutputKind::soundfont:
            destinations_[index] = Destination::soundfont_synth;
            break;
        case OutputKind::external_midi:
            destinations_[index] = Destination::external_midi;
            break;
        }
    }
    return true;
}

ISynth* BlockRenderer::synth_for(Destination destination) const noexcept {
    switch (destination) {
    case Destination::builtin_synth:
        return builtin_;
    case Destination::soundfont_synth:
        return soundfont_ != nullptr ? soundfont_ : builtin_;
    case Destination::external_midi:
        return nullptr;
    }
    return nullptr;
}

void BlockRenderer::begin(float* interleaved_stereo, std::uint32_t frame_count) noexcept {
    output_ = interleaved_stereo;
    frames_ = frame_count;
    rendered_ = 0;
    block_events_ = 0;
    if (output_ != nullptr)
        std::fill(output_, output_ + static_cast<std::size_t>(frame_count) * 2, 0.0f);
}

void BlockRenderer::render_until(std::uint32_t offset) noexcept {
    offset = std::min(offset, frames_);
    if (offset <= rendered_ || output_ == nullptr)
        return;
    float* segment = output_ + static_cast<std::size_t>(rendered_) * 2;
    const std::uint32_t count = offset - rendered_;
    if (builtin_ != nullptr)
        builtin_->render(segment, count);
    if (soundfont_ != nullptr)
        soundfont_->render(segment, count);
    if (metronome_ != nullptr)
        metronome_->render(segment, count);
    rendered_ = offset;
}

void BlockRenderer::on_event(const playback::RealtimeMidiEvent& event) noexcept {
    render_until(event.frame_offset);
    switch (event.kind) {
    case playback::RealtimeEventKind::tempo:
        return;
    case playback::RealtimeEventKind::metronome_click:
        if (metronome_ != nullptr)
            metronome_->trigger(event.value);
        return;
    case playback::RealtimeEventKind::midi:
        break;
    }
    ++block_events_;
    const Destination destination =
        event.route < route_count_ ? destinations_[event.route] : Destination::builtin_synth;
    if (destination == Destination::external_midi) {
        if (external_ != nullptr)
            external_->on_event(event);
        return;
    }
    if (ISynth* synth = synth_for(destination))
        synth->dispatch(event);
}

void BlockRenderer::dispatch_live(const playback::RealtimeMidiEvent& event, bool to_soundfont) noexcept {
    render_until(event.frame_offset);
    ++block_events_;
    if (ISynth* synth = synth_for(to_soundfont ? Destination::soundfont_synth : Destination::builtin_synth))
        synth->dispatch_live(event);
}

void BlockRenderer::finish() noexcept { render_until(frames_); }

} // namespace mwb::audio
