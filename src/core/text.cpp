#include "mwb/core/text.hpp"

#include <utf8proc.h>

#include <cstdlib>
#include <memory>

namespace mwb {

namespace {

/// Windows-1252 code points for bytes 0x80..0x9F (0 means undefined; shown as U+FFFD).
constexpr char32_t windows1252_high[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

void append_utf8(std::string& text, char32_t code_point) {
    if (code_point < 0x80) {
        text.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        text.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        text.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        text.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        text.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        text.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        text.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

struct FreeDeleter {
    void operator()(utf8proc_uint8_t* pointer) const { std::free(pointer); }
};

} // namespace

bool is_valid_utf8(std::span<const std::uint8_t> bytes) {
    std::size_t index = 0;
    while (index < bytes.size()) {
        utf8proc_int32_t code_point = 0;
        const auto length = utf8proc_iterate(bytes.data() + index, static_cast<utf8proc_ssize_t>(bytes.size() - index),
                                             &code_point);
        if (length <= 0)
            return false;
        index += static_cast<std::size_t>(length);
    }
    return true;
}

std::string decode_midi_text(std::span<const std::uint8_t> bytes, bool* used_fallback) {
    if (is_valid_utf8(bytes)) {
        if (used_fallback)
            *used_fallback = false;
        return std::string(bytes.begin(), bytes.end());
    }
    if (used_fallback)
        *used_fallback = true;
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        if (byte < 0x80) {
            text.push_back(static_cast<char>(byte));
        } else if (byte < 0xA0) {
            const char32_t mapped = windows1252_high[byte - 0x80];
            append_utf8(text, mapped == 0 ? char32_t{0xFFFD} : mapped);
        } else {
            append_utf8(text, byte);
        }
    }
    return text;
}

std::string fold_for_search(std::string_view utf8) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(utf8.data());
    std::string valid = decode_midi_text(std::span<const std::uint8_t>(data, utf8.size()));
    utf8proc_uint8_t* folded = nullptr;
    const auto options = static_cast<utf8proc_option_t>(UTF8PROC_COMPOSE | UTF8PROC_COMPAT | UTF8PROC_CASEFOLD |
                                                         UTF8PROC_STABLE | UTF8PROC_IGNORE);
    const auto length = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(valid.data()),
                                     static_cast<utf8proc_ssize_t>(valid.size()), &folded, options);
    if (length < 0 || folded == nullptr)
        return valid;
    std::unique_ptr<utf8proc_uint8_t, FreeDeleter> owner(folded);
    return std::string(reinterpret_cast<const char*>(folded), static_cast<std::size_t>(length));
}

std::string trim(std::string_view text) {
    const auto is_space = [](char character) {
        return character == ' ' || character == '\t' || character == '\n' || character == '\r';
    };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_space(text[begin]))
        ++begin;
    while (end > begin && is_space(text[end - 1]))
        --end;
    return std::string(text.substr(begin, end - begin));
}

std::string ascii_lower(std::string_view text) {
    std::string lowered(text);
    for (auto& character : lowered)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    return lowered;
}

} // namespace mwb
