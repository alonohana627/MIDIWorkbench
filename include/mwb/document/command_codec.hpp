#pragma once

#include "mwb/core/change_set.hpp"
#include "mwb/core/error.hpp"
#include "mwb/core/hashing.hpp"
#include "mwb/core/ids.hpp"
#include "mwb/core/time.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/// Recovery journal command codec, version 1 (section 23.3): a versioned CBOR envelope
/// (nlohmann::json::to_cbor) holding the command identity, the semantic hash of the state the
/// command applies to, the generations it moves between, the affected entity identities and the
/// complete ChangeSet with every before/after value.
namespace mwb::document {

/// Version of the command envelope written by this build.
inline constexpr std::int32_t command_codec_version = 1;

/// Format tag stored in every envelope.
inline constexpr std::string_view command_codec_format = "mwb.command";

/// What produced a journaled transition. Undo and redo are recorded as new forward transitions
/// (their applied change set is the inverse or the original change set).
enum class CommandKind : std::uint8_t { edit, undo, redo };

/// One journaled command.
struct CommandEnvelope {
    Uuid command_id;
    CommandKind kind = CommandKind::edit;
    Sha256Digest base_semantic_hash{};   ///< semantic_hash of the document before the change.
    Generation base_generation = 0;
    Generation result_generation = 0;    ///< Must exceed base_generation.
    ChangeSet changes;                   ///< The change set that was applied.
};

/// Stable lowercase name of a command kind ("edit", "undo", "redo"), as stored in envelopes.
[[nodiscard]] const char* command_kind_name(CommandKind kind);

/// Sorted, de-duplicated identities of every sequence, track and event a change set touches.
[[nodiscard]] std::vector<Uuid> affected_entities(const ChangeSet& changes);

/// Encodes an envelope as CBOR.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_command(const CommandEnvelope& envelope);

/// Decodes and validates an envelope: format tag, codec version, field types and ranges, enum
/// values, generations and the affected-entity list.
[[nodiscard]] Result<CommandEnvelope> decode_command(std::span<const std::uint8_t> bytes);

} // namespace mwb::document
