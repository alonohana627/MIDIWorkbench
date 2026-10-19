#include "mwb/core/error.hpp"

namespace mwb {

std::string_view error_code_name(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::invalid_data: return "invalid_data";
    case ErrorCode::unsupported: return "unsupported";
    case ErrorCode::limit_exceeded: return "limit_exceeded";
    case ErrorCode::io: return "io";
    case ErrorCode::database: return "database";
    case ErrorCode::device: return "device";
    case ErrorCode::cancelled: return "cancelled";
    case ErrorCode::conflict: return "conflict";
    case ErrorCode::not_found: return "not_found";
    case ErrorCode::busy: return "busy";
    case ErrorCode::internal: return "internal";
    }
    return "internal";
}

Error Error::with_context(std::string text) const {
    Error copy = *this;
    copy.context = context.empty() ? std::move(text) : std::move(text) + ": " + context;
    return copy;
}

Error Error::wrapped(ErrorCode outer_code, std::string outer_message) const {
    return Error{outer_code, std::move(outer_message), {}, std::make_shared<const Error>(*this)};
}

std::string Error::describe() const {
    std::string text = message;
    if (!context.empty())
        text += " (" + context + ")";
    if (cause)
        text += ": " + cause->describe();
    return text;
}

} // namespace mwb
