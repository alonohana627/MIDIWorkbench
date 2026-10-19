#include "mwb/midi/codec.hpp"

#include "mwb/core/checked.hpp"
#include "mwb/core/hashing.hpp"
#include "mwb/core/notes.hpp"
#include "mwb/core/tempo_map.hpp"
#include "mwb/core/text.hpp"

#include <libremidi/reader.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace mwb::midi {

namespace {

bool starts_with(std::span<const std::uint8_t> bytes, std::size_t offset, const char* tag) {
    const std::size_t length = std::strlen(tag);
    if (bytes.size() < offset + length)
        return false;
    return std::memcmp(bytes.data() + offset, tag, length) == 0;
}

std::uint32_t read_le32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

constexpr std::size_t max_diagnostics = 10'000;

void add_diagnostic(std::vector<Diagnostic>& diagnostics, Severity severity, std::string code, std::string message,
                    int sequence = -1, int track = -1, std::int64_t ordinal = -1) {
    if (diagnostics.size() > max_diagnostics)
        return;
    if (diagnostics.size() == max_diagnostics) {
        diagnostics.push_back(Diagnostic{Severity::warning, "smf.diagnostics_truncated",
                                         "more problems were found; only the first 10,000 are listed", -1, -1, -1});
        return;
    }
    diagnostics.push_back(Diagnostic{severity, std::move(code), std::move(message), sequence, track, ordinal});
}

bool read_vlq(std::span<const std::uint8_t> data, std::size_t& index, std::uint64_t& value) {
    value = 0;
    for (int count = 0; count < 4; ++count) {
        if (index >= data.size())
            return false;
        const std::uint8_t byte = data[index++];
        value = (value << 7) | (byte & 0x7F);
        if (!(byte & 0x80))
            return true;
    }
    return false;
}

/// Counts track events without materializing them, so the event limit is enforced before the
/// reader allocates (a running-status track packs an event into two bytes). Stops at malformed
/// data; the reader then reports it.
std::size_t count_smf_events(std::span<const std::uint8_t> smf, std::size_t stop_after) {
    std::size_t total = 0;
    std::size_t offset = 0;
    while (offset + 8 <= smf.size()) {
        const std::uint32_t length = (std::uint32_t{smf[offset + 4]} << 24) | (std::uint32_t{smf[offset + 5]} << 16) |
                                     (std::uint32_t{smf[offset + 6]} << 8) | smf[offset + 7];
        const bool is_track = smf[offset] == 'M' && smf[offset + 1] == 'T' && smf[offset + 2] == 'r' && smf[offset + 3] == 'k';
        const std::size_t begin = offset + 8;
        const std::size_t end = std::min<std::size_t>(smf.size(), begin + length);
        if (is_track) {
            const auto data = smf.subspan(begin, end - begin);
            std::size_t index = 0;
            std::uint8_t running = 0;
            while (index < data.size()) {
                std::uint64_t value = 0;
                if (!read_vlq(data, index, value) || index >= data.size())
                    break;
                std::uint8_t status = data[index];
                if (status >= 0x80)
                    ++index;
                else if (running != 0)
                    status = running;
                else
                    break;
                if (status == 0xFF) {
                    if (index >= data.size() || !read_vlq(data, ++index, value))
                        break;
                    index += value;
                } else if (status == 0xF0 || status == 0xF7) {
                    if (!read_vlq(data, index, value))
                        break;
                    index += value;
                    running = 0;
                } else if (status >= 0x80 && status < 0xF0) {
                    running = status;
                    const std::uint8_t kind = status & 0xF0;
                    index += (kind == 0xC0 || kind == 0xD0) ? 1 : 2;
                } else {
                    break;
                }
                if (++total > stop_after)
                    return total;
            }
        }
        if (end == smf.size())
            break;
        offset = end;
    }
    return total;
}

/// Decodes the VLQ length that libremidi keeps inside meta message bytes ([FF, type, VLQ, data]).
bool split_meta(const libremidi::midi_bytes& bytes, std::uint8_t& type, std::size_t& data_offset) {
    if (bytes.size() < 3 || bytes[0] != 0xFF)
        return false;
    type = bytes[1];
    std::size_t index = 2;
    std::uint32_t length = 0;
    for (int count = 0; count < 4 && index < bytes.size(); ++count) {
        const std::uint8_t byte = bytes[index++];
        length = (length << 7) | (byte & 0x7Fu);
        if ((byte & 0x80) == 0) {
            data_offset = index;
            return bytes.size() - index == length;
        }
    }
    return false;
}

/// Validates known meta payloads (section 16.2). Malformed ones are preserved with a flag.
bool meta_payload_valid(std::uint8_t type, std::span<const std::uint8_t> data, std::string& reason) {
    switch (type) {
    case meta_type::tempo:
        if (data.size() != 3) {
            reason = "tempo meta event does not have three bytes";
            return false;
        }
        if (data[0] == 0 && data[1] == 0 && data[2] == 0) {
            reason = "tempo meta event is zero microseconds per quarter";
            return false;
        }
        return true;
    case meta_type::time_signature:
        if (data.size() != 4) {
            reason = "time signature meta event does not have four bytes";
            return false;
        }
        if (data[0] == 0) {
            reason = "time signature numerator is zero";
            return false;
        }
        if (data[1] > 6) {
            reason = "time signature denominator exponent is outside the supported range (2^0..2^6)";
            return false;
        }
        return true;
    case meta_type::key_signature: {
        if (data.size() != 2) {
            reason = "key signature meta event does not have two bytes";
            return false;
        }
        const auto sharps = static_cast<std::int8_t>(data[0]);
        if (sharps < -7 || sharps > 7 || data[1] > 1) {
            reason = "key signature values are out of range";
            return false;
        }
        return true;
    }
    case meta_type::channel_prefix:
    case meta_type::port_prefix:
        if (data.size() != 1) {
            reason = "prefix meta event does not have one byte";
            return false;
        }
        return true;
    case meta_type::smpte_offset:
        if (data.size() != 5) {
            reason = "SMPTE offset meta event does not have five bytes";
            return false;
        }
        return true;
    case meta_type::sequence_number:
        if (!data.empty() && data.size() != 2) {
            reason = "sequence number meta event must have zero or two bytes";
            return false;
        }
        return true;
    default: return true;
    }
}

const char* reader_result_name(libremidi::reader::parse_result result) {
    switch (result) {
    case libremidi::reader::invalid: return "invalid";
    case libremidi::reader::incomplete: return "incomplete";
    case libremidi::reader::complete: return "complete";
    case libremidi::reader::validated: return "validated";
    }
    return "invalid";
}

std::string title_from_name(std::string_view original_name) {
    // Plain text handling, not std::filesystem::path: a name that is not valid UTF-8 would throw
    // there and fail the whole import (code-core-23). The bytes after the last separator, with
    // the extension removed, decoded like MIDI text (UTF-8, else Windows-1252).
    const auto separator = original_name.find_last_of("/\\");
    auto name = separator == std::string_view::npos ? original_name : original_name.substr(separator + 1);
    if (const auto dot = name.find_last_of('.'); dot != std::string_view::npos && dot > 0)
        name = name.substr(0, dot);
    const auto title = decode_midi_text(std::span(reinterpret_cast<const std::uint8_t*>(name.data()), name.size()));
    return title.empty() ? std::string("Imported MIDI") : title;
}

} // namespace

Container detect_container(std::span<const std::uint8_t> bytes) {
    if (starts_with(bytes, 0, "MThd"))
        return Container::smf;
    if (starts_with(bytes, 0, "RIFF") && starts_with(bytes, 8, "RMID"))
        return Container::rmid;
    return Container::unknown;
}

const char* container_name(Container container) {
    switch (container) {
    case Container::smf: return "smf";
    case Container::rmid: return "rmid";
    case Container::unknown: return "unknown";
    }
    return "unknown";
}

Result<std::span<const std::uint8_t>> unwrap_rmid(std::span<const std::uint8_t> bytes) {
    if (detect_container(bytes) != Container::rmid || bytes.size() < 12)
        return make_error(ErrorCode::invalid_data, "not a RIFF RMID file");
    const std::uint64_t riff_size = read_le32(bytes, 4);
    const std::uint64_t riff_end = std::min<std::uint64_t>(bytes.size(), riff_size + 8);
    std::size_t offset = 12;
    while (offset + 8 <= riff_end) {
        const std::uint64_t chunk_size = read_le32(bytes, offset + 4);
        const std::size_t payload = offset + 8;
        if (payload + chunk_size > bytes.size())
            return make_error(ErrorCode::invalid_data, "RMID chunk extends beyond the end of the file");
        if (starts_with(bytes, offset, "data")) {
            auto embedded = bytes.subspan(payload, static_cast<std::size_t>(chunk_size));
            if (detect_container(embedded) != Container::smf)
                return make_error(ErrorCode::invalid_data, "RMID data chunk does not contain a Standard MIDI File");
            return embedded;
        }
        // RIFF chunks are word aligned: odd sizes carry one pad byte.
        offset = payload + static_cast<std::size_t>(chunk_size) + (chunk_size & 1u);
    }
    return make_error(ErrorCode::invalid_data, "RMID file has no data chunk");
}

std::string ImportReport::to_json() const {
    nlohmann::json value = {
        {"version", 1},
        {"container", container_name(container)},
        {"format", format},
        {"declared_tracks", declared_tracks},
        {"decoded_tracks", decoded_tracks},
        {"division",
         division.is_ppq()
             ? nlohmann::json{{"kind", "ppq"}, {"ppq", division.ppq}}
             : nlohmann::json{{"kind", "smpte"},
                              {"fps_code", static_cast<int>(division.smpte_format)},
                              {"ticks_per_frame", division.ticks_per_frame}}},
        {"reader_result", reader_result},
        {"semantic_model_complete", semantic_model_complete},
        {"event_count", event_count},
        {"note_count", note_count},
        {"skipped_chunks", skipped_chunks},
        {"header_extension", header_extension},
    };
    auto list = nlohmann::json::array();
    for (const auto& diagnostic : diagnostics)
        list.push_back({{"severity", severity_name(diagnostic.severity)},
                        {"code", diagnostic.code},
                        {"message", diagnostic.message},
                        {"sequence", diagnostic.sequence},
                        {"track", diagnostic.track},
                        {"event_ordinal", diagnostic.event_ordinal}});
    value["diagnostics"] = std::move(list);
    return value.dump();
}

namespace {
Result<ImportResult> import_midi_unguarded(std::span<const std::uint8_t> bytes, std::string_view original_name,
                                           const ParseLimits& limits, std::stop_token stop);
} // namespace

Result<ImportReport> ImportReport::from_json(std::string_view text) {
    const auto value = nlohmann::json::parse(text, nullptr, false);
    const auto invalid = [] { return make_error(ErrorCode::invalid_data, "import report is not valid"); };
    if (value.is_discarded() || !value.is_object() || !value.contains("version"))
        return invalid();
    try {
        ImportReport report;
        const std::string container = value.value("container", "unknown");
        for (const auto candidate : {Container::smf, Container::rmid, Container::unknown})
            if (container == container_name(candidate))
                report.container = candidate;
        report.format = value.value("format", 0);
        report.declared_tracks = value.value("declared_tracks", 0);
        report.decoded_tracks = value.value("decoded_tracks", 0);
        if (const auto division = value.find("division"); division != value.end() && division->is_object()) {
            if (division->value("kind", "") == "smpte")
                report.division = Division::make_smpte(static_cast<SmpteFormat>(division->value("fps_code", -25)),
                                                       division->value("ticks_per_frame", 40));
            else
                report.division = Division::make_ppq(division->value("ppq", 480));
        }
        report.reader_result = value.value("reader_result", "invalid");
        report.semantic_model_complete = value.value("semantic_model_complete", false);
        report.event_count = value.value("event_count", std::size_t{0});
        report.note_count = value.value("note_count", std::size_t{0});
        report.skipped_chunks = value.value("skipped_chunks", 0);
        report.header_extension = value.value("header_extension", false);
        if (const auto list = value.find("diagnostics"); list != value.end() && list->is_array())
            for (const auto& item : *list) {
                if (!item.is_object())
                    continue;
                Diagnostic diagnostic;
                const std::string severity = item.value("severity", "warning");
                for (const auto candidate : {Severity::info, Severity::warning, Severity::error})
                    if (severity == severity_name(candidate))
                        diagnostic.severity = candidate;
                diagnostic.code = item.value("code", "");
                diagnostic.message = item.value("message", "");
                diagnostic.sequence = item.value("sequence", -1);
                diagnostic.track = item.value("track", -1);
                diagnostic.event_ordinal = item.value("event_ordinal", std::int64_t{-1});
                report.diagnostics.push_back(std::move(diagnostic));
            }
        return report;
    } catch (const nlohmann::json::exception&) {
        return invalid();
    }
}

Result<ImportResult> import_midi(std::span<const std::uint8_t> bytes, std::string_view original_name,
                                 const ParseLimits& limits, std::stop_token stop) {
    // Adapter boundary: no exception from the reader or from allocation escapes the Result API.
    try {
        return import_midi_unguarded(bytes, original_name, limits, std::move(stop));
    } catch (const std::bad_alloc&) {
        return make_error(ErrorCode::limit_exceeded, "the file needs more memory than is available to import");
    } catch (const std::exception& error) {
        return make_error(ErrorCode::invalid_data, std::string("the MIDI reader failed: ") + error.what());
    }
}

namespace {

Result<ImportResult> import_midi_unguarded(std::span<const std::uint8_t> bytes, std::string_view original_name,
                                           const ParseLimits& limits, std::stop_token stop) {
    if (bytes.size() > limits.max_bytes)
        return make_error(ErrorCode::limit_exceeded, "file is larger than the import size limit");
    ImportReport report;
    report.container = detect_container(bytes);
    std::span<const std::uint8_t> smf = bytes;
    if (report.container == Container::unknown)
        return make_error(ErrorCode::unsupported, "not a Standard MIDI File or RMID file (identified by content)");
    if (report.container == Container::rmid) {
        auto embedded = unwrap_rmid(bytes);
        if (!embedded)
            return std::unexpected(embedded.error());
        smf = *embedded;
    }

    if (count_smf_events(smf, limits.max_events) > limits.max_events)
        return make_error(ErrorCode::limit_exceeded, "file has more events than the import limit");
    libremidi::reader reader(false); // delta ticks; accumulated here with checked arithmetic
    const auto result = reader.parse(smf.data(), smf.size());
    report.reader_result = reader_result_name(result);
    if (result == libremidi::reader::invalid)
        return make_error(ErrorCode::invalid_data, "the MIDI reader could not parse this file");
    if (stop.stop_requested())
        return make_error(ErrorCode::cancelled, "import cancelled");

    report.format = reader.format;
    report.declared_tracks = reader.declaredTrackCount;
    report.decoded_tracks = static_cast<int>(reader.tracks.size());
    report.skipped_chunks = reader.skippedChunks;
    report.header_extension = reader.headerLength > 6;
    if (reader.smpteDivision) {
        report.division = Division::make_smpte(static_cast<SmpteFormat>(reader.smpteFormat), reader.ticksPerFrame);
    } else {
        if (reader.rawDivision == 0)
            return make_error(ErrorCode::invalid_data, "PPQ division is zero");
        report.division = Division::make_ppq(reader.rawDivision);
    }
    if (reader.tracks.size() > limits.max_tracks && report.format != 2)
        return make_error(ErrorCode::limit_exceeded, "file has more tracks than the per-sequence limit");
    if (report.format == 2 && reader.tracks.size() > limits.max_sequences)
        return make_error(ErrorCode::limit_exceeded, "file has more independent sequences than the limit");

    auto& diagnostics = report.diagnostics;
    if (result == libremidi::reader::incomplete) {
        add_diagnostic(diagnostics, Severity::error, "smf.incomplete",
                       "the file is truncated or malformed; this is a recovered partial interpretation");
    } else if (result == libremidi::reader::complete) {
        add_diagnostic(diagnostics, Severity::warning, "smf.not_validated",
                       "the file parsed but does not follow every SMF rule (missing or misplaced end-of-track, "
                       "or trailing bytes)");
    }
    if (report.declared_tracks != report.decoded_tracks)
        add_diagnostic(diagnostics, Severity::warning, "smf.track_count",
                       "header declares " + std::to_string(report.declared_tracks) + " tracks but " +
                           std::to_string(report.decoded_tracks) + " were read");
    if (report.skipped_chunks > 0)
        add_diagnostic(diagnostics, Severity::info, "smf.unknown_chunks",
                       std::to_string(report.skipped_chunks) +
                           " non-track chunk(s) ignored; they survive only through Original export");
    if (report.header_extension)
        add_diagnostic(diagnostics, Severity::info, "smf.header_extension",
                       "header is longer than six bytes; the extension was skipped");
    if (report.decoded_tracks == 0)
        return make_error(ErrorCode::invalid_data, "file contains no tracks");
    if (report.format == 0 && report.decoded_tracks > 1)
        add_diagnostic(diagnostics, Severity::warning, "smf.type0_tracks",
                       "a type-0 file holds " + std::to_string(report.decoded_tracks) +
                           " tracks; they were kept as separate tracks");

    DocumentData document;
    document.id = DocumentId::generate();
    document.title = title_from_name(original_name);
    document.created_ms = now_utc_ms();
    document.modified_ms = document.created_ms;

    std::vector<Track> tracks;
    tracks.reserve(reader.tracks.size());
    std::size_t total_events = 0;
    for (std::size_t track_index = 0; track_index < reader.tracks.size(); ++track_index) {
        if (stop.stop_requested())
            return make_error(ErrorCode::cancelled, "import cancelled");
        const auto& source = reader.tracks[track_index];
        if (total_events + source.size() > limits.max_events)
            return make_error(ErrorCode::limit_exceeded, "file has more events than the import limit");
        const int sequence_number = report.format == 2 ? static_cast<int>(track_index) : 0;
        const int track_number = report.format == 2 ? 0 : static_cast<int>(track_index);
        Track track;
        track.id = TrackId::generate();
        track.name = "Track " + std::to_string(track_index + 1);
        track.events.reserve(source.size());
        Tick tick = 0;
        Tick end_of_track = -1;
        bool name_set = false;
        bool events_after_end = false;
        std::size_t ordinal = 0;
        for (const auto& item : source) {
            auto next = checked_add(tick, item.tick);
            if (!next || item.tick < 0)
                return make_error(ErrorCode::invalid_data, "tick position overflows");
            tick = *next;
            const auto& message = item.m.bytes;
            if (message.empty())
                continue;
            if (end_of_track >= 0)
                events_after_end = true;
            Event event;
            event.id = EventId::generate();
            event.tick = tick;
            const auto event_ordinal = static_cast<std::int64_t>(ordinal);
            if (message[0] == 0xFF) {
                std::uint8_t type = 0;
                std::size_t data_offset = 0;
                if (!split_meta(message, type, data_offset)) {
                    add_diagnostic(diagnostics, Severity::warning, "meta.length",
                                   "meta event length does not match its data; the event was skipped (Original export keeps the source bytes)",
                                   sequence_number, track_number, event_ordinal);
                    continue;
                }
                if (type == meta_type::end_of_track) {
                    if (end_of_track < 0)
                        end_of_track = tick;
                    else
                        add_diagnostic(diagnostics, Severity::warning, "smf.extra_end_of_track",
                                       "track has more than one end-of-track event", sequence_number, track_number);
                    continue;
                }
                event.category = EventCategory::meta;
                event.code = type;
                const std::span<const std::uint8_t> data(message.data() + data_offset, message.size() - data_offset);
                if (data.size() > limits.max_payload_bytes)
                    return make_error(ErrorCode::limit_exceeded, "meta payload exceeds the size limit");
                event.payload = Payload(data);
                std::string reason;
                if (type > 0x7F || !meta_payload_valid(type, data, reason)) {
                    event.flags |= event_flag::malformed;
                    add_diagnostic(diagnostics, Severity::warning, "meta.malformed",
                                   (reason.empty() ? std::string("meta type outside 0..127") : reason) +
                                       "; preserved as raw evidence and ignored for timing",
                                   sequence_number, track_number, event_ordinal);
                }
                if (type == meta_type::track_name && !name_set) {
                    bool fallback = false;
                    const std::string decoded = decode_midi_text(data, &fallback);
                    if (!decoded.empty()) {
                        track.name = decoded;
                        name_set = true;
                    }
                    if (fallback)
                        add_diagnostic(diagnostics, Severity::info, "text.encoding",
                                       "track name is not UTF-8; shown with the Windows-1252 fallback",
                                       sequence_number, track_number, event_ordinal);
                }
            } else if (message[0] == 0xF0 || message[0] == 0xF7) {
                event.category = message[0] == 0xF0 ? EventCategory::sysex_f0 : EventCategory::sysex_f7;
                event.code = message[0];
                if (message.size() - 1 > limits.max_payload_bytes)
                    return make_error(ErrorCode::limit_exceeded, "SysEx payload exceeds the size limit");
                event.payload = Payload(std::span<const std::uint8_t>(message.data() + 1, message.size() - 1));
            } else if (message[0] >= 0x80 && message[0] <= 0xEF) {
                event.category = EventCategory::channel;
                event.code = message[0];
                event.payload = Payload(std::span<const std::uint8_t>(message.data() + 1, message.size() - 1));
                if (event.payload.size() != channel_data_length(event.code)) {
                    event.flags |= event_flag::malformed;
                    add_diagnostic(diagnostics, Severity::warning, "channel.length",
                                   "channel message has the wrong number of data bytes", sequence_number,
                                   track_number, event_ordinal);
                }
            } else {
                add_diagnostic(diagnostics, Severity::warning, "smf.unsupported_status",
                               "system message inside a track was skipped", sequence_number, track_number,
                               event_ordinal);
                continue;
            }
            event.order_key = (ordinal + 1) * order_key_spacing;
            ++ordinal;
            track.events.push_back(std::move(event));
        }
        if (end_of_track < 0)
            add_diagnostic(diagnostics, Severity::warning, "smf.missing_end_of_track",
                           "track has no end-of-track event; its end is its last event", sequence_number, track_number);
        if (events_after_end)
            add_diagnostic(diagnostics, Severity::warning, "smf.events_after_end",
                           "events follow the end-of-track event", sequence_number, track_number);
        track.end_tick = std::max(end_of_track, track.last_event_tick());
        total_events += track.events.size();
        if (total_events > limits.max_events)
            return make_error(ErrorCode::limit_exceeded, "file has more events than the import limit");
        // Drum presentation hint only: notes exclusively on MIDI channel 10 (index 9).
        bool any_note = false;
        bool only_channel_ten = true;
        for (const auto& event : track.events) {
            if (event.is_note_start()) {
                any_note = true;
                if (event.channel() != 9)
                    only_channel_ten = false;
            }
        }
        track.is_drum = any_note && only_channel_ten;
        tracks.push_back(std::move(track));
    }
    report.event_count = total_events;

    const auto make_sequence = [&](std::string name) {
        Sequence sequence;
        sequence.id = SequenceId::generate();
        sequence.name = std::move(name);
        sequence.division = report.division;
        return sequence;
    };
    if (report.format == 2) {
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            Sequence sequence = make_sequence("Sequence " + std::to_string(index + 1));
            sequence.end_tick = tracks[index].end_tick;
            sequence.tracks.push_back(std::move(tracks[index]));
            document.sequences.push_back(std::move(sequence));
        }
    } else {
        Sequence sequence = make_sequence("Sequence 1");
        for (auto& track : tracks) {
            sequence.end_tick = std::max(sequence.end_tick, track.end_tick);
            sequence.tracks.push_back(std::move(track));
        }
        document.sequences.push_back(std::move(sequence));
    }

    for (std::size_t sequence_index = 0; sequence_index < document.sequences.size(); ++sequence_index) {
        const auto& sequence = document.sequences[sequence_index];
        const auto pairing = pair_notes(sequence);
        report.note_count += pairing.notes.size();
        const int s = static_cast<int>(sequence_index);
        if (!pairing.orphan_ends.empty())
            add_diagnostic(diagnostics, Severity::info, "notes.orphan_end",
                           std::to_string(pairing.orphan_ends.size()) +
                               " note end(s) without a note start; kept as events",
                           s);
        std::size_t unterminated = 0;
        std::size_t zero_length = 0;
        for (const auto& note : pairing.notes) {
            if (note.flags & note_flag::unterminated)
                ++unterminated;
            if (note.flags & note_flag::zero_length)
                ++zero_length;
        }
        if (unterminated > 0)
            add_diagnostic(diagnostics, Severity::warning, "notes.unterminated",
                           std::to_string(unterminated) +
                               " note(s) never end; preview closes them at the sequence end",
                           s);
        if (zero_length > 0)
            add_diagnostic(diagnostics, Severity::info, "notes.zero_length",
                           std::to_string(zero_length) + " zero-length note pair(s) retained", s);
        if (pairing.ambiguous_count > 0)
            add_diagnostic(diagnostics, Severity::info, "notes.ambiguous",
                           std::to_string(pairing.ambiguous_count) +
                               " overlapping same-key notes; paired first-in-first-out",
                           s);
        if (sequence.division.is_ppq()) {
            const auto tempo = TempoMap::build(sequence);
            if (tempo.has_conflicts())
                add_diagnostic(diagnostics, Severity::warning, "tempo.conflict",
                               "different tempo events at the same tick; the last in track order is used", s);
            if (!tempo.has_explicit_initial_tempo())
                add_diagnostic(diagnostics, Severity::info, "tempo.default",
                               "no tempo at the start; 120 BPM is assumed", s);
        }
    }
    report.semantic_model_complete = result != libremidi::reader::incomplete;

    SourceBlob source;
    source.id = Uuid::generate();
    source.sha256 = sha256(bytes);
    source.original_name = std::string(original_name);
    source.media_type = report.container == Container::rmid ? "rmid" : "smf";
    source.bytes = std::make_shared<const std::vector<std::uint8_t>>(bytes.begin(), bytes.end());
    source.import_report_json = report.to_json();
    document.sources.push_back(std::move(source));

    return ImportResult{std::move(document), std::move(report)};
}

} // namespace

} // namespace mwb::midi
