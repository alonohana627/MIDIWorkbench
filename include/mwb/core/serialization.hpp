#pragma once

#include "mwb/core/error.hpp"
#include "mwb/core/hashing.hpp"
#include "mwb/core/model.hpp"

#include <string>
#include <vector>

namespace mwb {

/// Versioned JSON forms of the settings columns (section 23.1 JSON contracts).
[[nodiscard]] std::string sequence_settings_to_json(const SequenceSettings& settings);
[[nodiscard]] Result<SequenceSettings> sequence_settings_from_json(const std::string& json);
[[nodiscard]] std::string routing_to_json(const RoutingSettings& routing);
[[nodiscard]] Result<RoutingSettings> routing_from_json(const std::string& json);
[[nodiscard]] std::string instrument_to_json(const InstrumentSettings& instrument);
[[nodiscard]] Result<InstrumentSettings> instrument_from_json(const std::string& json);
[[nodiscard]] std::string region_tracks_to_json(const std::vector<TrackId>& tracks);
[[nodiscard]] Result<std::vector<TrackId>> region_tracks_from_json(const std::string& json);

/// SHA-256 over a canonical, length-prefixed, little-endian serialization of the document's
/// semantic content: identities, events, tracks, sequences, regions, scales, recipes, takes and
/// title. Generation and timestamps are excluded. Proves that undo restores the exact state.
[[nodiscard]] Sha256Digest semantic_hash(const DocumentData& document);
/// Semantic hash of one sequence (recipe provenance).
[[nodiscard]] Sha256Digest semantic_hash(const Sequence& sequence);

} // namespace mwb
