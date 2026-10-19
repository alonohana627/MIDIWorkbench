#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/model.hpp"
#include "mwb/core/validation.hpp"

#include <cstdint>
#include <optional>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

/// Standard MIDI File adapter around libremidi's reader and writer (specification section 16).
/// The adapter converts between libremidi tracks and the domain model, enforces limits and
/// validation on the decoded result, and produces structured reports. No project SMF parser.
namespace mwb::midi {

/// Import limits (section 28.3 defaults).
struct ParseLimits {
    std::size_t max_bytes = 256u * 1024u * 1024u;
    std::size_t max_events = 2'000'000;
    std::size_t max_tracks = 256;
    std::size_t max_sequences = 128;
    std::size_t max_payload_bytes = 8u * 1024u * 1024u;
};

/// Container recognized by content, not by extension (section 16.1).
enum class Container : std::uint8_t { smf, rmid, unknown };

/// Detects the container of the bytes: "MThd" header or RIFF/RMID wrapper.
[[nodiscard]] Container detect_container(std::span<const std::uint8_t> bytes);
[[nodiscard]] const char* container_name(Container container);

/// Extracts the embedded SMF from a RIFF RMID file (the wrapper bytes stay in the original
/// object for Original export). Returns the byte range of the "data" chunk payload.
[[nodiscard]] Result<std::span<const std::uint8_t>> unwrap_rmid(std::span<const std::uint8_t> bytes);

/// Structured import report (section 16.2).
struct ImportReport {
    Container container = Container::unknown;
    int format = 0;                    ///< SMF type 0, 1 or 2.
    int declared_tracks = 0;           ///< Track count in MThd.
    int decoded_tracks = 0;            ///< Tracks the reader returned.
    Division division;
    std::string reader_result;         ///< "validated", "complete", "incomplete" or "invalid".
    bool semantic_model_complete = false;
    std::size_t event_count = 0;
    std::size_t note_count = 0;
    int skipped_chunks = 0;
    bool header_extension = false;
    std::vector<Diagnostic> diagnostics;

    /// Versioned JSON object stored as source_blob.import_report_json.
    [[nodiscard]] std::string to_json() const;
    /// Reads to_json() output (for example from the parser worker); never throws.
    [[nodiscard]] static Result<ImportReport> from_json(std::string_view text);
};

/// Import result: the editable document plus its report. The original bytes are attached to the
/// document as its first SourceBlob.
struct ImportResult {
    DocumentData document;
    ImportReport report;
};

/// Parses SMF/RMID bytes: import_midi in this process, or the isolated parser worker (section
/// 28.2). Same contract as import_midi.
using MidiParser = std::function<Result<ImportResult>(std::span<const std::uint8_t> bytes, std::string_view name,
                                                      const ParseLimits& limits, std::stop_token stop)>;

/// Parses SMF/RMID bytes into a new document. Rejects inputs above the size limit before calling
/// the reader. A truncated or invalid file is reported (reader_result "incomplete"/"invalid");
/// a recovered partial interpretation is returned only for "incomplete" and labeled as such.
[[nodiscard]] Result<ImportResult> import_midi(std::span<const std::uint8_t> bytes, std::string_view original_name,
                                               const ParseLimits& limits = {}, std::stop_token stop = {});

/// The three export modes of section 16.5.
enum class ExportMode : std::uint8_t {
    original,      ///< Exact stored imported bytes.
    preserving,    ///< Current events, retaining supported and opaque payloads and ordering.
    compatibility, ///< Named profile with an explicit report of changes.
};

/// Compatibility profiles for ExportMode::compatibility.
enum class CompatibilityProfile : std::uint8_t {
    canonical_type1, ///< Conductor track with tempo/meter; malformed events dropped; notes closed.
    type0,           ///< Single merged track.
};

/// What to export.
struct ExportOptions {
    ExportMode mode = ExportMode::preserving;
    CompatibilityProfile profile = CompatibilityProfile::canonical_type1;
    int smf_format = -1;                    ///< -1: type 1 (or 2 when several sequences are exported).
    std::optional<std::size_t> sequence;    ///< Sequence to export; default first (or all for type 2).
    bool audible_tracks_only = false;       ///< Skip muted tracks / honor solo (section 11.1).
    std::optional<TimeRange> range;         ///< Export-selected-range (section 16.6).
    std::vector<TrackId> tracks;            ///< Optional track subset; empty means all.
};

/// Export report: what was omitted, converted or repaired.
struct ExportReport {
    std::vector<Diagnostic> diagnostics;
    std::size_t events_written = 0;
    std::size_t events_omitted = 0;
    std::size_t notes_trimmed = 0;
    int format = 1;
    int track_count = 0;

    [[nodiscard]] std::string to_json() const;
};

/// Serializes the document to SMF bytes. Deterministic for equal semantic input and options.
[[nodiscard]] Result<std::vector<std::uint8_t>> export_midi(const DocumentData& document, const ExportOptions& options,
                                                            ExportReport* report = nullptr);

} // namespace mwb::midi
