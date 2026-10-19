#include "mwb/ui/clipboard.hpp"

#include "mwb/core/checked.hpp"
#include "mwb/core/note_edit.hpp"
#include "mwb/core/pitch.hpp"
#include "mwb/ui/document_controller.hpp"

#include <QMimeData>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>

namespace mwb::ui {

namespace {
constexpr std::size_t max_clipboard_notes = 500'000;
constexpr std::size_t max_clipboard_tracks = 256;
} // namespace

std::string clipboard_to_json(const ClipboardPayload& payload) {
    nlohmann::json notes = nlohmann::json::array();
    for (const auto& note : payload.notes)
        notes.push_back({note.start, note.end, note.pitch, note.velocity, note.release_velocity, note.channel, note.track});
    nlohmann::json programs = nlohmann::json::array();
    for (const auto& [channel, program] : payload.programs)
        programs.push_back({channel, program});
    return nlohmann::json{{"format", "mwb.selection"},
                          {"schema_version", 1},
                          {"ppq", payload.ppq},
                          {"selection_length", payload.selection_length},
                          {"tracks", payload.tracks},
                          {"notes", std::move(notes)},
                          {"programs", std::move(programs)},
                          {"scale", {{"root", payload.source_root}, {"mask", payload.source_scale_mask}}}}
        .dump();
}

namespace {
Result<ClipboardPayload> decode_clipboard(std::string_view text);
} // namespace

Result<ClipboardPayload> clipboard_from_json(std::string_view text) {
    // Clipboard content comes from any program: type mismatches inside the JSON must be a
    // validation error, never an exception escaping into the event loop.
    try {
        return decode_clipboard(text);
    } catch (const nlohmann::json::exception&) {
        return make_error(ErrorCode::invalid_data, "clipboard content is not a valid selection");
    }
}

namespace {

Result<ClipboardPayload> decode_clipboard(std::string_view text) {
    if (text.size() > clipboard_max_bytes)
        return make_error(ErrorCode::limit_exceeded, "clipboard content is too large");
    const auto value = nlohmann::json::parse(text, nullptr, false);
    const auto invalid = [] { return make_error(ErrorCode::invalid_data, "clipboard content is not a valid selection"); };
    if (value.is_discarded() || !value.is_object() || value.value("format", "") != "mwb.selection")
        return invalid();
    if (value.value("schema_version", 0) != 1)
        return make_error(ErrorCode::unsupported, "clipboard selection was written by a newer version");
    ClipboardPayload payload;
    payload.ppq = value.value("ppq", 0);
    payload.selection_length = value.value("selection_length", Tick{0});
    // Ticks from other programs are bounded so conversion arithmetic cannot overflow.
    constexpr Tick max_clipboard_tick = Tick{1} << 40;
    if (payload.ppq < 1 || payload.ppq > 32767 || payload.selection_length < 0 || payload.selection_length > max_clipboard_tick)
        return invalid();
    const auto tracks_found = value.find("tracks");
    const auto notes_found = value.find("notes");
    if (tracks_found == value.end() || notes_found == value.end())
        return invalid();
    const auto& tracks = *tracks_found;
    const auto& notes = *notes_found;
    if (!tracks.is_array() || !notes.is_array() || tracks.size() > max_clipboard_tracks || notes.size() > max_clipboard_notes)
        return invalid();
    for (const auto& track : tracks) {
        if (!track.is_string())
            return invalid();
        payload.tracks.push_back(track.get<std::string>());
    }
    for (const auto& item : notes) {
        if (!item.is_array() || item.size() != 7)
            return invalid();
        for (const auto& field : item)
            if (!field.is_number_integer())
                return invalid();
        ClipboardNote note{item[0].get<Tick>(), item[1].get<Tick>(), item[2].get<int>(), item[3].get<int>(),
                           item[4].get<int>(), item[5].get<int>(), item[6].get<int>()};
        if (note.start < 0 || note.end < note.start || note.end > max_clipboard_tick || note.pitch < 0 || note.pitch > 127 || note.velocity < 1 ||
            note.velocity > 127 || note.release_velocity < 0 || note.release_velocity > 127 || note.channel < 0 ||
            note.channel > 15 || note.track < 0 || static_cast<std::size_t>(note.track) >= payload.tracks.size())
            return invalid();
        payload.notes.push_back(note);
    }
    if (const auto programs = value.find("programs"); programs != value.end() && programs->is_array()) {
        for (const auto& item : *programs)
            if (item.is_array() && item.size() == 2 && item[0].is_number_integer() && item[1].is_number_integer())
                payload.programs.emplace_back(std::clamp(item[0].get<int>(), 0, 15), std::clamp(item[1].get<int>(), 0, 127));
    }
    if (const auto scale = value.find("scale"); scale != value.end() && scale->is_object()) {
        payload.source_root = std::clamp(scale->value("root", 0), 0, 11);
        payload.source_scale_mask = static_cast<std::uint16_t>(std::clamp(scale->value("mask", 0x0FFF), 1, 0x0FFF));
    }
    return payload;
}

} // namespace

std::string clipboard_summary(const ClipboardPayload& payload) {
    std::string text = std::to_string(payload.notes.size()) + " note(s) at PPQ " + std::to_string(payload.ppq) + ":";
    std::size_t shown = 0;
    for (const auto& note : payload.notes) {
        if (++shown > 64) {
            text += " ...";
            break;
        }
        text += " " + pitch_name(static_cast<std::uint8_t>(note.pitch)) + "@" + std::to_string(note.start) + "+" +
                std::to_string(note.end - note.start);
    }
    return text;
}

QMimeData* make_clipboard_mime(const DocumentController& controller) {
    const auto notes = controller.selected_notes();
    if (notes.empty())
        return nullptr;
    const auto& sequence = controller.active_sequence();
    ClipboardPayload payload;
    payload.ppq = sequence.division.is_ppq() ? sequence.division.ppq : default_ppq;
    payload.source_root = sequence.root_pc;
    payload.source_scale_mask = sequence.scale_mask;
    Tick start = notes.front().start;
    Tick end = notes.front().end;
    for (const auto& note : notes) {
        start = std::min(start, note.start);
        end = std::max(end, note.end);
    }
    payload.selection_length = end - start;
    std::map<std::uint32_t, int> track_slots;
    for (const auto& note : notes) {
        auto [slot, inserted] = track_slots.emplace(note.on_track, static_cast<int>(payload.tracks.size()));
        if (inserted)
            payload.tracks.push_back(sequence.tracks[note.on_track].name);
        payload.notes.push_back(ClipboardNote{note.start - start, note.end - start, note.pitch, note.velocity,
                                              note.release_velocity, note.channel, slot->second});
    }
    // Program in effect at the selection start for each used channel (initial controller state).
    std::map<int, int> programs;
    for (const auto& [index, slot] : track_slots)
        for (const auto& event : sequence.tracks[index].events) {
            if (event.tick > start)
                break;
            if (event.is_channel() && event.message() == ChannelMessage::program_change)
                programs[event.channel()] = event.data1();
        }
    for (const auto& entry : programs)
        payload.programs.push_back(entry);
    auto* mime = new QMimeData;
    mime->setData(clipboard_mime_type, QByteArray::fromStdString(clipboard_to_json(payload)));
    mime->setText(QString::fromStdString(clipboard_summary(payload)));
    return mime;
}

std::vector<EventId> paste_clipboard(DocumentController& controller, const QMimeData* data, Tick anchor, QString* error) {
    if (!data || !data->hasFormat(clipboard_mime_type)) {
        if (error)
            *error = QObject::tr("The clipboard holds no MIDI Workbench selection (other programs exchange MIDI as .mid files)");
        return {};
    }
    auto payload = clipboard_from_json(data->data(clipboard_mime_type).toStdString());
    if (!payload) {
        if (error)
            *error = QString::fromStdString(payload.error().message);
        return {};
    }
    const auto& sequence = controller.active_sequence();
    const std::int32_t destination_ppq = sequence.division.is_ppq() ? sequence.division.ppq : default_ppq;
    std::vector<NewNote> notes;
    Tick max_error_numerator = 0;
    for (const auto& note : payload->notes) {
        // Each event converted independently from its exact original position (section 17.5).
        const Tick start = mul_div_round(note.start, destination_ppq, payload->ppq).value_or(0);
        Tick end = mul_div_round(note.end, destination_ppq, payload->ppq).value_or(start + 1);
        if (end <= start)
            end = start + 1; // authored duration floor
        const Tick exact_error = std::abs(start * payload->ppq - note.start * destination_ppq);
        max_error_numerator = std::max(max_error_numerator, exact_error);
        notes.push_back(NewNote{controller.active_track(), anchor + start, anchor + end,
                                static_cast<std::uint8_t>(note.pitch), static_cast<std::uint8_t>(note.velocity),
                                static_cast<std::uint8_t>(note.channel), static_cast<std::uint8_t>(note.release_velocity)});
    }
    auto changes = insert_notes(controller.document(), notes, "Paste");
    if (!changes) {
        if (error)
            *error = QString::fromStdString(changes.error().message);
        return {};
    }
    const auto before = controller.notes().notes;
    if (!controller.apply(*changes))
        return {};
    std::set<EventId> previous;
    for (const auto& note : before)
        previous.insert(note.on_id);
    std::vector<EventId> created;
    for (const auto& note : controller.notes().notes)
        if (!previous.contains(note.on_id))
            created.push_back(note.on_id);
    if (error && payload->ppq != destination_ppq)
        *error = QObject::tr("Pasted from PPQ %1 into PPQ %2; largest timing error %3 tick(s)")
                     .arg(payload->ppq)
                     .arg(destination_ppq)
                     .arg(static_cast<double>(max_error_numerator) / payload->ppq, 0, 'f', 3);
    if (error && payload->tracks.size() > 1 && error->isEmpty())
        *error = QObject::tr("Notes from %1 tracks were pasted onto the active track").arg(payload->tracks.size());
    return created;
}

} // namespace mwb::ui
