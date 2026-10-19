#pragma once

#include <cstdint>
#include <string_view>

/// Executable DDL of the native document and recovery databases, copied verbatim from
/// specification sections 23.1 and 23.3. Changing either text changes its recorded checksum
/// (migration.hpp) and therefore requires a new schema version and a migration step.
namespace mwb::document::schemas {

/// Section 23.1, schema "native" (application_id 1297564209, user_version 1).
inline constexpr std::string_view native_v1 = R"sql(-- schema: native
PRAGMA foreign_keys = ON;
PRAGMA application_id = 1297564209;
PRAGMA user_version = 1;

CREATE TABLE project (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    uuid BLOB NOT NULL UNIQUE CHECK (length(uuid) = 16),
    title TEXT NOT NULL,
    created_ms INTEGER NOT NULL,
    modified_ms INTEGER NOT NULL,
    generation INTEGER NOT NULL CHECK (generation >= 0),
    writer_version TEXT NOT NULL,
    metadata_json TEXT NOT NULL CHECK (json_valid(metadata_json))
) STRICT;

CREATE TABLE sequence (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    sort_order INTEGER NOT NULL UNIQUE,
    name TEXT NOT NULL,
    division TEXT NOT NULL CHECK (division IN ('ppq','smpte')),
    ppq INTEGER,
    fps_code INTEGER,
    ticks_per_frame INTEGER,
    end_tick INTEGER NOT NULL CHECK (end_tick >= 0),
    root_pc INTEGER NOT NULL CHECK (root_pc BETWEEN 0 AND 11),
    scale_mask INTEGER NOT NULL CHECK
      (scale_mask BETWEEN 1 AND 4095 AND (scale_mask & 1)=1),
    settings_json TEXT NOT NULL CHECK (json_valid(settings_json)),
    CHECK (
      (division='ppq' AND ppq IS NOT NULL AND ppq BETWEEN 1 AND 32767
       AND fps_code IS NULL AND ticks_per_frame IS NULL)
      OR
      (division='smpte' AND ppq IS NULL
       AND fps_code IS NOT NULL AND ticks_per_frame IS NOT NULL
       AND fps_code IN (-24,-25,-29,-30)
       AND ticks_per_frame BETWEEN 1 AND 255)
    )
) STRICT;

CREATE TABLE track (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    sequence_id BLOB NOT NULL REFERENCES sequence(id) ON DELETE CASCADE,
    sort_order INTEGER NOT NULL,
    name TEXT NOT NULL,
    color_rgba INTEGER NOT NULL,
    end_tick INTEGER NOT NULL CHECK (end_tick >= 0),
    muted INTEGER NOT NULL CHECK (muted IN (0,1)),
    solo INTEGER NOT NULL CHECK (solo IN (0,1)),
    is_drum INTEGER NOT NULL CHECK (is_drum IN (0,1)),
    routing_json TEXT NOT NULL CHECK (json_valid(routing_json)),
    instrument_json TEXT NOT NULL CHECK (json_valid(instrument_json)),
    UNIQUE(sequence_id, sort_order)
) STRICT;

CREATE TABLE midi_event (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    track_id BLOB NOT NULL REFERENCES track(id) ON DELETE CASCADE,
    tick INTEGER NOT NULL CHECK (tick >= 0),
    order_key INTEGER NOT NULL CHECK (order_key >= 0),
    category TEXT NOT NULL CHECK
      (category IN ('channel','meta','sysex_f0','sysex_f7')),
    code INTEGER NOT NULL,
    payload BLOB NOT NULL CHECK (length(payload) <= 8388608),
    flags INTEGER NOT NULL DEFAULT 0 CHECK (flags >= 0),
    CHECK (
      (category='channel' AND code BETWEEN 128 AND 239)
      OR (category='meta' AND code BETWEEN 0 AND 127 AND code <> 47)
      OR (category='sysex_f0' AND code=240)
      OR (category='sysex_f7' AND code=247)
    ),
    UNIQUE(track_id, tick, order_key)
) STRICT;
CREATE INDEX event_time ON midi_event(track_id, tick, order_key);

CREATE TABLE note_link (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    on_event_id BLOB NOT NULL UNIQUE
      REFERENCES midi_event(id) ON DELETE CASCADE,
    off_event_id BLOB UNIQUE
      REFERENCES midi_event(id) ON DELETE CASCADE,
    pairing_policy TEXT NOT NULL,
    flags INTEGER NOT NULL DEFAULT 0 CHECK (flags >= 0),
    CHECK (off_event_id IS NULL OR off_event_id <> on_event_id)
) STRICT;

CREATE TABLE region (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    sequence_id BLOB NOT NULL REFERENCES sequence(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    start_tick INTEGER NOT NULL CHECK (start_tick >= 0),
    end_tick INTEGER NOT NULL CHECK (end_tick > start_tick),
    tracks_json TEXT NOT NULL CHECK (json_valid(tracks_json)),
    is_loop INTEGER NOT NULL CHECK (is_loop IN (0,1))
) STRICT;

CREATE TABLE scale_region (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    sequence_id BLOB NOT NULL REFERENCES sequence(id) ON DELETE CASCADE,
    start_tick INTEGER NOT NULL CHECK (start_tick >= 0),
    end_tick INTEGER NOT NULL CHECK (end_tick > start_tick),
    root_pc INTEGER NOT NULL CHECK (root_pc BETWEEN 0 AND 11),
    scale_mask INTEGER NOT NULL CHECK
      (scale_mask BETWEEN 1 AND 4095 AND (scale_mask & 1)=1),
    scale_name TEXT NOT NULL,
    spelling_json TEXT NOT NULL CHECK (json_valid(spelling_json))
) STRICT;
CREATE INDEX scale_region_time
  ON scale_region(sequence_id, start_tick, end_tick);

CREATE TABLE source_blob (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    sha256 BLOB NOT NULL CHECK (length(sha256) = 32),
    original_name TEXT NOT NULL,
    media_type TEXT NOT NULL CHECK (media_type IN ('smf','rmid')),
    bytes BLOB NOT NULL CHECK (length(bytes) <= 268435456),
    import_report_json TEXT NOT NULL CHECK (json_valid(import_report_json))
) STRICT;

CREATE TABLE opaque_record (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    source_id BLOB NOT NULL REFERENCES source_blob(id) ON DELETE CASCADE,
    sequence_id BLOB REFERENCES sequence(id) ON DELETE CASCADE,
    track_id BLOB REFERENCES track(id) ON DELETE CASCADE,
    record_kind TEXT NOT NULL,
    source_offset INTEGER NOT NULL CHECK (source_offset >= 0),
    placement_json TEXT NOT NULL CHECK (json_valid(placement_json)),
    bytes BLOB NOT NULL
) STRICT;

CREATE TABLE recording_take (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    track_id BLOB NOT NULL REFERENCES track(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    created_ms INTEGER NOT NULL,
    start_tick INTEGER NOT NULL CHECK (start_tick >= 0),
    end_tick INTEGER NOT NULL CHECK (end_tick >= start_tick),
    status TEXT NOT NULL CHECK
      (status IN ('captured','accepted','interrupted','discarded')),
    capture_json TEXT NOT NULL CHECK (json_valid(capture_json))
) STRICT;

CREATE TABLE take_chunk (
    take_id BLOB NOT NULL REFERENCES recording_take(id) ON DELETE CASCADE,
    ordinal INTEGER NOT NULL CHECK (ordinal >= 0),
    codec_version INTEGER NOT NULL CHECK (codec_version > 0),
    first_time_ns INTEGER NOT NULL CHECK (first_time_ns >= 0),
    sha256 BLOB NOT NULL CHECK (length(sha256) = 32),
    payload BLOB NOT NULL CHECK (length(payload) <= 1048576),
    PRIMARY KEY(take_id, ordinal)
) STRICT, WITHOUT ROWID;

CREATE TABLE recipe (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    sequence_id BLOB NOT NULL REFERENCES sequence(id) ON DELETE CASCADE,
    tool_id TEXT NOT NULL,
    algorithm_version INTEGER NOT NULL CHECK (algorithm_version > 0),
    source_semantic_hash BLOB NOT NULL CHECK (length(source_semantic_hash)=32),
    parameters_json TEXT NOT NULL CHECK (json_valid(parameters_json)),
    targets_json TEXT NOT NULL CHECK (json_valid(targets_json))
) STRICT;

CREATE TABLE document_setting (
    key TEXT PRIMARY KEY,
    schema_version INTEGER NOT NULL CHECK (schema_version > 0),
    value_json TEXT NOT NULL CHECK (json_valid(value_json))
) STRICT;
)sql";

/// Section 23.3, schema "recovery" (application_id 1297564242, user_version 1).
inline constexpr std::string_view recovery_v1 = R"sql(-- schema: recovery
PRAGMA foreign_keys = ON;
PRAGMA application_id = 1297564242;
PRAGMA user_version = 1;

CREATE TABLE recovery_session (
    id BLOB PRIMARY KEY CHECK (length(id)=16),
    document_uuid BLOB NOT NULL CHECK (length(document_uuid)=16),
    source_reference_json TEXT NOT NULL CHECK (json_valid(source_reference_json)),
    created_ms INTEGER NOT NULL,
    updated_ms INTEGER NOT NULL,
    cleanly_closed INTEGER NOT NULL CHECK (cleanly_closed IN (0,1))
) STRICT;

CREATE TABLE recovery_checkpoint (
    session_id BLOB NOT NULL REFERENCES recovery_session(id) ON DELETE CASCADE,
    command_no INTEGER NOT NULL CHECK (command_no>=0),
    generation INTEGER NOT NULL CHECK (generation>=0),
    sha256 BLOB NOT NULL CHECK (length(sha256)=32),
    native_snapshot BLOB NOT NULL,
    created_ms INTEGER NOT NULL,
    PRIMARY KEY(session_id,command_no)
) STRICT, WITHOUT ROWID;

CREATE TABLE recovery_command (
    session_id BLOB NOT NULL REFERENCES recovery_session(id) ON DELETE CASCADE,
    command_no INTEGER NOT NULL CHECK (command_no>0),
    codec_version INTEGER NOT NULL CHECK (codec_version>0),
    base_generation INTEGER NOT NULL CHECK (base_generation>=0),
    result_generation INTEGER NOT NULL CHECK (result_generation>base_generation),
    sha256 BLOB NOT NULL CHECK (length(sha256)=32),
    payload BLOB NOT NULL,
    created_ms INTEGER NOT NULL,
    PRIMARY KEY(session_id,command_no)
) STRICT, WITHOUT ROWID;
)sql";

} // namespace mwb::document::schemas
