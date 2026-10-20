#include "mwb/music/general_midi.hpp"

#include <algorithm>
#include <array>
#include <set>

namespace mwb::music {

namespace {

// General MIDI Level 1 sound set, programs 1-128 (stored 0-based).
constexpr std::array<std::string_view, gm_program_count> program_names = {
    // Piano
    "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano",
    "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet",
    // Chromatic Percussion
    "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    // Organ
    "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica",
    "Tango Accordion",
    // Guitar
    "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)", "Electric Guitar (jazz)", "Electric Guitar (clean)",
    "Electric Guitar (muted)", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
    // Bass
    "Acoustic Bass", "Electric Bass (finger)", "Electric Bass (pick)", "Fretless Bass", "Slap Bass 1",
    "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
    // Strings
    "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp",
    "Timpani",
    // Ensemble
    "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs",
    "Synth Voice", "Orchestra Hit",
    // Brass
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1",
    "Synth Brass 2",
    // Reed
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
    // Pipe
    "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    // Synth Lead
    "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)", "Lead 4 (chiff)", "Lead 5 (charang)",
    "Lead 6 (voice)", "Lead 7 (fifths)", "Lead 8 (bass + lead)",
    // Synth Pad
    "Pad 1 (new age)", "Pad 2 (warm)", "Pad 3 (polysynth)", "Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)",
    "Pad 7 (halo)", "Pad 8 (sweep)",
    // Synth Effects
    "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)", "FX 4 (atmosphere)", "FX 5 (brightness)",
    "FX 6 (goblins)", "FX 7 (echoes)", "FX 8 (sci-fi)",
    // Ethnic
    "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    // Percussive
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum",
    "Reverse Cymbal",
    // Sound Effects
    "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause",
    "Gunshot",
};

constexpr std::array<std::string_view, gm_family_count> family_names = {
    "Piano", "Chromatic Percussion", "Organ", "Guitar", "Bass", "Strings", "Ensemble", "Brass",
    "Reed", "Pipe", "Synth Lead", "Synth Pad", "Synth Effects", "Ethnic", "Percussive", "Sound Effects",
};

/// First Program Change before the first note start, on `channel` only or (nullopt) any channel.
const Event* first_program_before_notes(const Track& track, std::optional<std::uint8_t> channel) {
    for (const auto& event : track.events) {
        if (!event.is_channel() || (channel && event.channel() != *channel))
            continue;
        if (event.is_note_start())
            return nullptr;
        if (event.message() == ChannelMessage::program_change)
            return &event;
    }
    return nullptr;
}

} // namespace

std::string_view gm_program_name(std::uint8_t program) noexcept { return program_names[program & 0x7F]; }

std::string_view gm_family_name(int family) noexcept {
    return family >= 0 && family < gm_family_count ? family_names[static_cast<std::size_t>(family)] : std::string_view();
}

std::optional<std::uint8_t> gm_program_named(std::string_view name) noexcept {
    for (std::size_t index = 0; index < program_names.size(); ++index)
        if (program_names[index] == name)
            return static_cast<std::uint8_t>(index);
    return std::nullopt;
}

TrackInstrument effective_instrument(const Track& track) {
    if (track.is_drum)
        return TrackInstrument{true, 0, false};
    if (const Event* program = first_program_before_notes(track, std::nullopt))
        return TrackInstrument{false, static_cast<std::uint8_t>(program->data1() & 0x7F), true};
    return TrackInstrument{false, static_cast<std::uint8_t>(track.instrument.program & 0x7F), false};
}

std::string instrument_label(const Track& track) {
    const auto instrument = effective_instrument(track);
    return instrument.drums ? std::string("Drums") : std::string(gm_program_name(instrument.program));
}

namespace {

/// The lowest channel no other track of the sequence plays on (events or a channel override),
/// never the drum channel. When all 15 are taken, the lowest channel this track does not use
/// elsewhere, else channel 1: the move off channel 10 matters more than sharing a channel.
std::uint8_t free_channel(const Sequence& sequence, const Track& track) {
    std::array<bool, 16> taken{};
    taken[gm_drum_channel] = true;
    for (const auto& other : sequence.tracks) {
        if (other.id == track.id)
            continue;
        if (other.routing.channel_override)
            taken[*other.routing.channel_override & 0x0F] = true;
        for (const auto& event : other.events)
            if (event.is_channel())
                taken[event.channel()] = true;
    }
    for (std::uint8_t channel = 0; channel < 16; ++channel)
        if (!taken[channel])
            return channel;
    std::array<bool, 16> own{};
    for (const auto& event : track.events)
        if (event.is_channel())
            own[event.channel()] = true;
    for (std::uint8_t channel = 0; channel < 16; ++channel)
        if (channel != gm_drum_channel && !own[channel])
            return channel;
    return 0;
}

} // namespace

Result<InstrumentEdit> set_track_instrument(const DocumentData& document, TrackId track_id,
                                            std::optional<std::uint8_t> program, std::string synth_preset) {
    const auto location = find_track(document, track_id);
    if (!location)
        return make_error(ErrorCode::not_found, "the track does not exist");
    if (program && *program >= gm_program_count)
        return make_error(ErrorCode::invalid_argument, "a program is 0 to 127");
    const Sequence& sequence = document.sequences[location->sequence];
    const Track& track = sequence.tracks[location->track];
    Track updated = track_header(track); // properties only, no events
    InstrumentEdit result;
    ChangeBuilder builder(document, "Set instrument");
    if (!program) {
        updated.is_drum = true;
        updated.routing.channel_override = gm_drum_channel;
    } else {
        updated.is_drum = false;
        if (updated.routing.channel_override == gm_drum_channel)
            updated.routing.channel_override.reset(); // channel 10 would keep it on the drum kit
        updated.instrument.program = *program;
        updated.instrument.synth_preset = std::move(synth_preset);
        // Events on channel 10 would still play the drum kit whatever the program: they move to
        // a free channel, all of them (notes, controllers, bends), so the part stays together.
        // A Program Change already on channel 10 moves too and is then updated below.
        Track working = track; // the events as they are after the move
        if (!updated.routing.channel_override) {
            const bool on_drum_channel = std::ranges::any_of(track.events, [](const Event& event) {
                return event.is_channel() && event.channel() == gm_drum_channel;
            });
            if (on_drum_channel) {
                const std::uint8_t target = free_channel(sequence, track);
                for (auto& event : working.events)
                    if (event.is_channel() && event.channel() == gm_drum_channel) {
                        event.code = static_cast<std::uint8_t>((event.code & 0xF0) | target);
                        builder.replace(track.id, event.id, event, OrderPlacement::after_existing);
                        ++result.moved_events;
                    }
                result.moved_to = target;
            }
        }
        // Every channel the notes play on gets the program, so a multi-channel track (common in
        // imported type-0 files) does not keep its old sound on some of them. A track without
        // notes gets channel 1's Program Change, ready for the notes added later.
        std::set<std::uint8_t> channels;
        if (updated.routing.channel_override)
            channels.insert(*updated.routing.channel_override);
        else
            for (const auto& event : working.events)
                if (event.is_note_start())
                    channels.insert(event.channel());
        if (channels.empty())
            channels.insert(0);
        channels.erase(gm_drum_channel); // never a Program Change on the percussion channel
        for (const std::uint8_t channel : channels) {
            if (const Event* existing = first_program_before_notes(working, channel)) {
                if (existing->data1() != *program) {
                    Event replacement = *existing;
                    replacement.payload = events::program_change(existing->tick, channel, *program).payload;
                    builder.replace(track.id, existing->id, std::move(replacement));
                }
            } else {
                builder.insert(track.id, events::program_change(0, channel, *program), OrderPlacement::before_note_starts);
            }
        }
    }
    if (updated != track_header(track))
        builder.set_track_properties(track.id, updated);
    result.changes = builder.finish();
    return result;
}

} // namespace mwb::music
