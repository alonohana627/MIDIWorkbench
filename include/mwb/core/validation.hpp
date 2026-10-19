#pragma once

#include "mwb/core/model.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mwb {

/// Severity of a diagnostic.
enum class Severity : std::uint8_t { info, warning, error };

/// Name used in JSON output ("info", "warning", "error").
[[nodiscard]] const char* severity_name(Severity severity);

/// One structured finding (section 16.2 report shape).
struct Diagnostic {
    Severity severity = Severity::warning;
    std::string code;              ///< Stable short code such as "event.payload_length".
    std::string message;           ///< Human explanation.
    std::int32_t sequence = -1;
    std::int32_t track = -1;
    std::int64_t event_ordinal = -1;
};

/// Resource limits (section 28.3 defaults).
struct DocumentLimits {
    std::size_t max_events_per_document = 1'000'000;
    std::size_t max_tracks_per_sequence = 256;
    std::size_t max_sequences = 128;
    std::size_t max_payload_bytes = 8u * 1024u * 1024u;
};

/// Checks the invariants of section 18.2 and the payload rules of section 23.1. Errors mean the
/// document must not be accepted as editable.
[[nodiscard]] std::vector<Diagnostic> validate_document(const DocumentData& document,
                                                        const DocumentLimits& limits = {});

/// True when any diagnostic is an error.
[[nodiscard]] bool has_errors(const std::vector<Diagnostic>& diagnostics);

/// Validates one event's category, code and payload length rules.
[[nodiscard]] bool event_is_well_formed(const Event& event, std::string* reason = nullptr);

} // namespace mwb
