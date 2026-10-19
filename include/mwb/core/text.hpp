#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace mwb {

/// True when the bytes are well-formed UTF-8.
[[nodiscard]] bool is_valid_utf8(std::span<const std::uint8_t> bytes);

/// Decodes MIDI text bytes for display: valid UTF-8 is used as is; otherwise each byte is read as
/// Windows-1252 and `used_fallback` is set (section 16.3). Stored bytes never change by decoding.
[[nodiscard]] std::string decode_midi_text(std::span<const std::uint8_t> bytes, bool* used_fallback = nullptr);

/// NFKC case folding through utf8proc for search keys, tags and sort keys (section 4.4.1).
/// Invalid UTF-8 is decoded with the display fallback first.
[[nodiscard]] std::string fold_for_search(std::string_view utf8);

/// Trims ASCII whitespace from both ends.
[[nodiscard]] std::string trim(std::string_view text);

/// Lowercase ASCII only (for keywords, never for user text identity).
[[nodiscard]] std::string ascii_lower(std::string_view text);

} // namespace mwb
