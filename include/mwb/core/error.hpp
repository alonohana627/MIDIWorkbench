#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace mwb {

/// Stable error taxonomy (specification section 19.3). Values are part of the CLI and
/// diagnostics contract; never renumber them.
enum class ErrorCode : std::uint16_t {
    invalid_argument = 1,   ///< Caller supplied an invalid parameter or option.
    invalid_data = 2,       ///< Input bytes or a document violate the format/domain rules.
    unsupported = 3,        ///< Recognized input that this version deliberately does not handle.
    limit_exceeded = 4,     ///< A configured resource limit (section 28.3) was reached.
    io = 5,                 ///< Filesystem failure.
    database = 6,           ///< SQLite failure.
    device = 7,             ///< Audio or MIDI device failure.
    cancelled = 8,          ///< Cooperative cancellation was requested.
    conflict = 9,           ///< Optimistic concurrency check failed (expected head changed).
    not_found = 10,         ///< A referenced entity does not exist.
    busy = 11,              ///< Another owner holds the resource (library writer lock).
    internal = 12,          ///< A broken invariant inside the application.
};

/// Short machine-readable name of an error code, used in JSON output.
[[nodiscard]] std::string_view error_code_name(ErrorCode code) noexcept;

/// Error value carried by mwb::Result. The message is user-readable; context is optional
/// detail such as an item name. Raw paths are not embedded in shared diagnostics by default.
struct Error {
    ErrorCode code = ErrorCode::internal;
    std::string message;
    std::string context;
    std::shared_ptr<const Error> cause;

    /// Returns a copy of this error with an added context description.
    [[nodiscard]] Error with_context(std::string text) const;
    /// Returns a new error of the given code whose cause is this error.
    [[nodiscard]] Error wrapped(ErrorCode outer_code, std::string outer_message) const;
    /// Full description including context and the cause chain.
    [[nodiscard]] std::string describe() const;
};

/// Result type used for every fallible operation (section 19.3).
template <class T>
using Result = std::expected<T, Error>;

/// Result of an operation that produces no value.
using Status = std::expected<void, Error>;

/// Builds an unexpected error value.
[[nodiscard]] inline std::unexpected<Error> make_error(ErrorCode code, std::string message,
                                                       std::string context = {}) {
    return std::unexpected<Error>(Error{code, std::move(message), std::move(context), nullptr});
}

/// Successful empty status.
[[nodiscard]] inline Status ok() { return {}; }

} // namespace mwb

/// Propagates an error from an expression returning mwb::Result/Status.
#define MWB_TRY(expression)                                                                        \
    do {                                                                                           \
        if (auto mwb_try_result_ = (expression); !mwb_try_result_)                                 \
            return std::unexpected(std::move(mwb_try_result_).error());                            \
    } while (false)
