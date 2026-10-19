#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/model.hpp"

#include <QString>

#include <string>
#include <vector>

class QMimeData;

namespace mwb::ui {

class DocumentController;

/// Internal clipboard MIME type (section 7.6).
inline constexpr const char* clipboard_mime_type = "application/x-midiworkbench-selection+json;version=1";
/// Maximum accepted clipboard payload; larger payloads are refused like an oversized file.
inline constexpr std::size_t clipboard_max_bytes = 8u * 1024u * 1024u;

/// One copied note relative to the selection start.
struct ClipboardNote {
    Tick start = 0;
    Tick end = 0;
    int pitch = 60;
    int velocity = 100;
    int release_velocity = 0;
    int channel = 0;
    int track = 0; ///< Index into ClipboardPayload::tracks.
};

/// Decoded clipboard payload (schema version 1).
struct ClipboardPayload {
    std::int32_t ppq = default_ppq;
    Tick selection_length = 0;
    std::vector<std::string> tracks;               ///< Source track names (grouping preserved).
    std::vector<ClipboardNote> notes;
    std::vector<std::pair<int, int>> programs;     ///< (channel, program) in effect at the start.
    int source_root = 0;
    std::uint16_t source_scale_mask = 0x0FFF;
};

/// Serializes the payload as the versioned JSON form.
[[nodiscard]] std::string clipboard_to_json(const ClipboardPayload& payload);
/// Parses and validates a payload (bounded sizes, value ranges); never trusts the input.
[[nodiscard]] Result<ClipboardPayload> clipboard_from_json(std::string_view json);
/// Plain-text note summary also placed on the clipboard.
[[nodiscard]] std::string clipboard_summary(const ClipboardPayload& payload);

/// Builds clipboard data (internal JSON and a text summary) for the controller's selection.
[[nodiscard]] QMimeData* make_clipboard_mime(const DocumentController& controller);

/// Pastes onto the active track with the source selection start anchored at `anchor`, converting
/// resolution with round(tick * destination_ppq / source_ppq) per event (section 17.5). Returns the
/// identities of the created notes; reports problems through `error`.
[[nodiscard]] std::vector<EventId> paste_clipboard(DocumentController& controller, const QMimeData* data, Tick anchor,
                                                   QString* error);

} // namespace mwb::ui
