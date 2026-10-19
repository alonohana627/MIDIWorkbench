#pragma once

#include <cstdint>
#include <string_view>

/// Catalog database schema (specification section 23.2) and its connection policy (section 22.5).
namespace mwb::library {

/// SQLite application_id of library catalogs.
inline constexpr std::int32_t catalog_application_id = 1297564204;
/// Catalog schema version (PRAGMA user_version).
inline constexpr std::int32_t catalog_schema_version = 1;

/// Executable baseline DDL, verbatim from section 23.2.
inline constexpr std::string_view catalog_ddl_v1 = R"sql(-- schema: catalog
PRAGMA foreign_keys = ON;
PRAGMA application_id = 1297564204;
PRAGMA user_version = 1;

CREATE TABLE catalog (
    singleton INTEGER PRIMARY KEY CHECK (singleton=1),
    uuid BLOB NOT NULL UNIQUE CHECK (length(uuid)=16),
    created_ms INTEGER NOT NULL,
    generation INTEGER NOT NULL DEFAULT 0 CHECK (generation>=0)
) STRICT;

CREATE TABLE object_file (
    sha256 BLOB PRIMARY KEY CHECK (length(sha256)=32),
    media_type TEXT NOT NULL CHECK (media_type IN ('smf','rmid','native')),
    extension TEXT NOT NULL CHECK (extension IN ('mid','rmi','mwb')),
    byte_count INTEGER NOT NULL CHECK (byte_count>=0),
    created_ms INTEGER NOT NULL,
    verified_ms INTEGER
) STRICT;

CREATE TABLE asset (
    id INTEGER PRIMARY KEY,
    uuid BLOB NOT NULL UNIQUE CHECK (length(uuid)=16),
    title TEXT NOT NULL,
    sort_title TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    kind TEXT NOT NULL CHECK (kind IN ('song','phrase','template')),
    role TEXT NOT NULL DEFAULT 'unspecified',
    rating INTEGER NOT NULL DEFAULT 0 CHECK (rating BETWEEN 0 AND 5),
    favorite INTEGER NOT NULL DEFAULT 0 CHECK (favorite IN (0,1)),
    created_ms INTEGER NOT NULL,
    modified_ms INTEGER NOT NULL,
    deleted_ms INTEGER,
    current_revision_id INTEGER,
    metadata_generation INTEGER NOT NULL DEFAULT 0,
    FOREIGN KEY(id, current_revision_id)
      REFERENCES revision(asset_id,id) DEFERRABLE INITIALLY DEFERRED
) STRICT;

CREATE TABLE revision (
    id INTEGER PRIMARY KEY,
    uuid BLOB NOT NULL UNIQUE CHECK (length(uuid)=16),
    asset_id INTEGER NOT NULL REFERENCES asset(id) ON DELETE CASCADE,
    ordinal INTEGER NOT NULL CHECK (ordinal>=1),
    parent_id INTEGER REFERENCES revision(id) ON DELETE SET NULL,
    object_sha256 BLOB NOT NULL REFERENCES object_file(sha256),
    created_ms INTEGER NOT NULL,
    origin TEXT NOT NULL CHECK (origin IN ('import','edit','extract','restore')),
    label TEXT NOT NULL DEFAULT '',
    pinned INTEGER NOT NULL DEFAULT 0 CHECK (pinned IN (0,1)),
    UNIQUE(asset_id,ordinal),
    UNIQUE(asset_id,id)
) STRICT;
CREATE INDEX asset_browse ON asset(deleted_ms,sort_title,id);
CREATE INDEX revision_object ON revision(object_sha256);

CREATE TABLE asset_override (
    asset_id INTEGER NOT NULL REFERENCES asset(id) ON DELETE CASCADE,
    sequence_no INTEGER NOT NULL CHECK (sequence_no>=0),
    field TEXT NOT NULL CHECK (field IN ('key','tempo_reference','drum_tracks')),
    schema_version INTEGER NOT NULL CHECK (schema_version>0),
    value_json TEXT NOT NULL CHECK (json_valid(value_json)),
    modified_ms INTEGER NOT NULL,
    PRIMARY KEY(asset_id,sequence_no,field)
) STRICT, WITHOUT ROWID;

CREATE TABLE catalog_change (
    id INTEGER PRIMARY KEY,
    operation_uuid BLOB NOT NULL UNIQUE CHECK (length(operation_uuid)=16),
    created_ms INTEGER NOT NULL,
    operation TEXT NOT NULL,
    schema_version INTEGER NOT NULL CHECK (schema_version>0),
    before_json TEXT NOT NULL CHECK (json_valid(before_json)),
    after_json TEXT NOT NULL CHECK (json_valid(after_json)),
    undone INTEGER NOT NULL DEFAULT 0 CHECK (undone IN (0,1))
) STRICT;

CREATE TABLE tag (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    normalized_name TEXT NOT NULL UNIQUE,
    color_rgba INTEGER
) STRICT;

CREATE TABLE asset_tag (
    asset_id INTEGER NOT NULL REFERENCES asset(id) ON DELETE CASCADE,
    tag_id INTEGER NOT NULL REFERENCES tag(id) ON DELETE CASCADE,
    PRIMARY KEY(asset_id,tag_id)
) STRICT, WITHOUT ROWID;
CREATE INDEX tag_assets ON asset_tag(tag_id,asset_id);

CREATE TABLE collection (
    id INTEGER PRIMARY KEY,
    uuid BLOB NOT NULL UNIQUE CHECK (length(uuid)=16),
    parent_id INTEGER REFERENCES collection(id) ON DELETE RESTRICT,
    name TEXT NOT NULL,
    kind TEXT NOT NULL CHECK (kind IN ('manual','smart')),
    query_json TEXT,
    CHECK ((kind='manual' AND query_json IS NULL)
       OR (kind='smart' AND query_json IS NOT NULL AND json_valid(query_json)))
) STRICT;

CREATE TABLE collection_asset (
    collection_id INTEGER NOT NULL REFERENCES collection(id) ON DELETE CASCADE,
    asset_id INTEGER NOT NULL REFERENCES asset(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    PRIMARY KEY(collection_id,asset_id)
) STRICT, WITHOUT ROWID;

CREATE TABLE source_root (
    id INTEGER PRIMARY KEY,
    uuid BLOB NOT NULL UNIQUE CHECK (length(uuid)=16),
    display_path TEXT NOT NULL,
    watch_enabled INTEGER NOT NULL CHECK (watch_enabled IN (0,1)),
    policy_json TEXT NOT NULL CHECK (json_valid(policy_json))
) STRICT;

CREATE TABLE source_location (
    id INTEGER PRIMARY KEY,
    root_id INTEGER NOT NULL REFERENCES source_root(id) ON DELETE CASCADE,
    asset_id INTEGER NOT NULL REFERENCES asset(id) ON DELETE CASCADE,
    relative_path TEXT NOT NULL,
    comparison_key TEXT NOT NULL,
    file_identity BLOB,
    byte_count INTEGER,
    modified_ms INTEGER,
    last_seen_ms INTEGER NOT NULL,
    last_object_sha256 BLOB REFERENCES object_file(sha256),
    state TEXT NOT NULL CHECK
      (state IN ('present','missing','offline','changed','unstable','error')),
    UNIQUE(root_id,comparison_key)
) STRICT;

CREATE TABLE analysis_run (
    id INTEGER PRIMARY KEY,
    revision_id INTEGER NOT NULL REFERENCES revision(id) ON DELETE CASCADE,
    analyzer TEXT NOT NULL,
    version INTEGER NOT NULL CHECK (version>0),
    settings_sha256 BLOB NOT NULL CHECK (length(settings_sha256)=32),
    status TEXT NOT NULL CHECK
      (status IN ('queued','running','complete','failed','cancelled')),
    completed_ms INTEGER,
    result_json TEXT,
    UNIQUE(revision_id,analyzer,version,settings_sha256),
    CHECK (result_json IS NULL OR json_valid(result_json))
) STRICT;

CREATE TABLE sequence_summary (
    revision_id INTEGER NOT NULL REFERENCES revision(id) ON DELETE CASCADE,
    sequence_no INTEGER NOT NULL CHECK (sequence_no>=0),
    analysis_version INTEGER NOT NULL CHECK (analysis_version>0),
    division TEXT NOT NULL CHECK (division IN ('ppq','smpte')),
    duration_seconds REAL NOT NULL CHECK (duration_seconds>=0),
    quarter_length REAL,
    tempo_min REAL,
    tempo_max REAL,
    tempo_representative REAL,
    tempo_origin TEXT NOT NULL CHECK
      (tempo_origin IN ('explicit','default','reference','unknown')),
    key_root INTEGER CHECK (key_root BETWEEN 0 AND 11),
    key_mode TEXT,
    key_origin TEXT NOT NULL CHECK
      (key_origin IN ('manual','signature','estimated','unknown')),
    key_confidence REAL CHECK (key_confidence BETWEEN 0 AND 1),
    track_count INTEGER NOT NULL CHECK (track_count>=0),
    event_count INTEGER NOT NULL CHECK (event_count>=0),
    note_count INTEGER NOT NULL CHECK (note_count>=0),
    min_pitch INTEGER CHECK (min_pitch BETWEEN 0 AND 127),
    max_pitch INTEGER CHECK (max_pitch BETWEEN 0 AND 127),
    max_polyphony INTEGER NOT NULL CHECK (max_polyphony>=0),
    has_drums INTEGER NOT NULL CHECK (has_drums IN (0,1)),
    diagnostics_json TEXT NOT NULL CHECK (json_valid(diagnostics_json)),
    PRIMARY KEY(revision_id,sequence_no)
) STRICT, WITHOUT ROWID;
CREATE INDEX summary_tempo ON sequence_summary(tempo_representative,revision_id);
CREATE INDEX summary_key ON sequence_summary(key_root,key_mode,revision_id);
CREATE INDEX summary_length ON sequence_summary(quarter_length,revision_id);

CREATE TABLE track_summary (
    revision_id INTEGER NOT NULL,
    sequence_no INTEGER NOT NULL,
    track_no INTEGER NOT NULL CHECK (track_no>=0),
    name TEXT NOT NULL,
    channel_mask INTEGER NOT NULL CHECK (channel_mask BETWEEN 0 AND 65535),
    program_json TEXT NOT NULL CHECK (json_valid(program_json)),
    note_count INTEGER NOT NULL CHECK (note_count>=0),
    feature_json TEXT NOT NULL CHECK (json_valid(feature_json)),
    PRIMARY KEY(revision_id,sequence_no,track_no),
    FOREIGN KEY(revision_id,sequence_no)
      REFERENCES sequence_summary(revision_id,sequence_no) ON DELETE CASCADE
) STRICT, WITHOUT ROWID;

CREATE TABLE motif_stream (
    id INTEGER PRIMARY KEY,
    revision_id INTEGER NOT NULL REFERENCES revision(id) ON DELETE CASCADE,
    sequence_no INTEGER NOT NULL,
    track_no INTEGER NOT NULL,
    channel INTEGER NOT NULL CHECK (channel BETWEEN 0 AND 15),
    voice_kind TEXT NOT NULL,
    profile_version INTEGER NOT NULL CHECK (profile_version>0),
    note_count INTEGER NOT NULL CHECK (note_count>=0),
    stream_sha256 BLOB NOT NULL CHECK (length(stream_sha256)=32),
    codec_version INTEGER NOT NULL CHECK (codec_version>0),
    payload BLOB NOT NULL
) STRICT;
CREATE INDEX stream_revision ON motif_stream(revision_id,profile_version);

CREATE TABLE motif_gram (
    stream_id INTEGER NOT NULL REFERENCES motif_stream(id) ON DELETE CASCADE,
    family TEXT NOT NULL CHECK
      (family IN ('pitch','interval','rhythm','contour','chord')),
    gram_length INTEGER NOT NULL CHECK (gram_length BETWEEN 2 AND 16),
    gram_hash BLOB NOT NULL CHECK (length(gram_hash)=8),
    start_ordinal INTEGER NOT NULL CHECK (start_ordinal>=0),
    start_tick INTEGER NOT NULL CHECK (start_tick>=0),
    PRIMARY KEY(stream_id,family,gram_length,start_ordinal)
) STRICT, WITHOUT ROWID;
CREATE INDEX gram_lookup
  ON motif_gram(family,gram_length,gram_hash,stream_id,start_ordinal);

CREATE TABLE feature_vector (
    revision_id INTEGER NOT NULL REFERENCES revision(id) ON DELETE CASCADE,
    sequence_no INTEGER NOT NULL,
    profile_version INTEGER NOT NULL CHECK (profile_version>0),
    dimension INTEGER NOT NULL CHECK (dimension>0),
    payload BLOB NOT NULL,
    PRIMARY KEY(revision_id,sequence_no,profile_version),
    CHECK (length(payload)=dimension*4)
) STRICT, WITHOUT ROWID;

CREATE TABLE fingerprint (
    revision_id INTEGER NOT NULL REFERENCES revision(id) ON DELETE CASCADE,
    profile TEXT NOT NULL,
    profile_version INTEGER NOT NULL CHECK (profile_version>0),
    sha256 BLOB NOT NULL CHECK (length(sha256)=32),
    PRIMARY KEY(revision_id,profile,profile_version)
) STRICT, WITHOUT ROWID;
CREATE INDEX fingerprint_lookup ON fingerprint(profile,profile_version,sha256);

CREATE VIRTUAL TABLE asset_fts USING fts5(
    title, source_names, tags, description, lyrics,
    tokenize='unicode61 remove_diacritics 2', prefix='2 3'
);

CREATE TABLE job (
    id INTEGER PRIMARY KEY,
    kind TEXT NOT NULL,
    idempotency_key TEXT NOT NULL UNIQUE,
    arguments_json TEXT NOT NULL CHECK (json_valid(arguments_json)),
    state TEXT NOT NULL CHECK
      (state IN ('queued','running','paused','complete','failed','cancelled','review')),
    completed_units INTEGER NOT NULL DEFAULT 0 CHECK (completed_units>=0),
    total_units INTEGER CHECK (total_units>=0),
    attempts INTEGER NOT NULL DEFAULT 0 CHECK (attempts>=0),
    created_ms INTEGER NOT NULL,
    updated_ms INTEGER NOT NULL,
    error_json TEXT CHECK (error_json IS NULL OR json_valid(error_json))
) STRICT;

CREATE TABLE catalog_setting (
    key TEXT PRIMARY KEY,
    schema_version INTEGER NOT NULL CHECK (schema_version>0),
    value_json TEXT NOT NULL CHECK (json_valid(value_json))
) STRICT;
)sql";

/// Index tuning added within schema version 1 (section 23: "index tuning may add indexes without
/// changing their meaning"). Each serves a hot query that EXPLAIN QUERY PLAN tests guard: keyset
/// sorts by modification/creation time, job leasing, manual-collection order, foreign-key lookups
/// on cascades and object garbage collection.
inline constexpr std::string_view catalog_tuning_indexes_v1 = R"sql(
CREATE INDEX asset_modified ON asset(deleted_ms,modified_ms,id);
CREATE INDEX asset_created ON asset(deleted_ms,created_ms,id);
CREATE INDEX job_queue ON job(state,id);
CREATE INDEX collection_order ON collection_asset(collection_id,position,asset_id);
CREATE INDEX collection_membership ON collection_asset(asset_id,collection_id);
CREATE INDEX collection_children ON collection(parent_id);
CREATE INDEX source_asset ON source_location(asset_id);
CREATE INDEX source_object ON source_location(last_object_sha256);
CREATE INDEX revision_parent ON revision(parent_id);
CREATE INDEX asset_head ON asset(current_revision_id);
)sql";

} // namespace mwb::library
