-- CloudSync Extension for PostgreSQL
-- Version 1.0

-- Complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION cloudsync" to load this file. \quit

-- ============================================================================
-- Public Functions
-- ============================================================================

-- Get extension version
CREATE FUNCTION cloudsync_version()
RETURNS text
AS 'MODULE_PATHNAME', 'cloudsync_version'
LANGUAGE C IMMUTABLE STRICT;

-- Get site identifier (UUID)
CREATE FUNCTION cloudsync_siteid()
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_cloudsync_siteid'
LANGUAGE C STABLE;

-- Generate a new UUID
CREATE FUNCTION cloudsync_uuid()
RETURNS bytea
AS 'MODULE_PATHNAME', 'cloudsync_uuid'
LANGUAGE C VOLATILE;

-- Get current database version
CREATE FUNCTION cloudsync_db_version()
RETURNS bigint
AS 'MODULE_PATHNAME', 'cloudsync_db_version'
LANGUAGE C STABLE;

-- Get next database version (with optional merging version)
CREATE FUNCTION cloudsync_db_version_next()
RETURNS bigint
AS 'MODULE_PATHNAME', 'cloudsync_db_version_next'
LANGUAGE C VOLATILE;

CREATE FUNCTION cloudsync_db_version_next(merging_version bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'cloudsync_db_version_next'
LANGUAGE C VOLATILE;

-- Initialize CloudSync for a table (3 variants for 1-3 arguments)
-- Returns site_id as bytea
CREATE FUNCTION cloudsync_init(table_name text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'cloudsync_init'
LANGUAGE C VOLATILE;

CREATE FUNCTION cloudsync_init(table_name text, algo text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'cloudsync_init'
LANGUAGE C VOLATILE;

CREATE FUNCTION cloudsync_init(table_name text, algo text, skip_int_pk_check boolean)
RETURNS bytea
AS 'MODULE_PATHNAME', 'cloudsync_init'
LANGUAGE C VOLATILE;

-- Enable sync for a table
CREATE FUNCTION cloudsync_enable(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_enable'
LANGUAGE C VOLATILE;

-- Disable sync for a table
CREATE FUNCTION cloudsync_disable(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_disable'
LANGUAGE C VOLATILE;

-- Check if table is sync-enabled
CREATE FUNCTION cloudsync_is_enabled(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_is_enabled'
LANGUAGE C STABLE;

-- Cleanup orphaned metadata for a table
CREATE FUNCTION cloudsync_cleanup(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_cleanup'
LANGUAGE C VOLATILE;

-- Terminate CloudSync
CREATE FUNCTION cloudsync_terminate()
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_terminate'
LANGUAGE C VOLATILE;

-- Set global configuration
CREATE FUNCTION cloudsync_set(key text, value text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_set'
LANGUAGE C VOLATILE;

-- Set table-level configuration
CREATE FUNCTION cloudsync_set_table(table_name text, key text, value text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_set_table'
LANGUAGE C VOLATILE;

-- Set column-level configuration
CREATE FUNCTION cloudsync_set_column(table_name text, column_name text, key text, value text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_set_column'
LANGUAGE C VOLATILE;

-- Begin schema alteration
CREATE FUNCTION cloudsync_begin_alter(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_begin_alter'
LANGUAGE C VOLATILE;

-- Commit schema alteration
CREATE FUNCTION cloudsync_commit_alter(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_commit_alter'
LANGUAGE C VOLATILE;

-- Payload encoding (aggregate function)
CREATE FUNCTION cloudsync_payload_encode_transfn(state internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'cloudsync_payload_encode_transfn'
LANGUAGE C;

CREATE FUNCTION cloudsync_payload_encode_finalfn(state internal)
RETURNS bytea
AS 'MODULE_PATHNAME', 'cloudsync_payload_encode_finalfn'
LANGUAGE C;

CREATE AGGREGATE cloudsync_payload_encode(*) (
    SFUNC = cloudsync_payload_encode_transfn,
    STYPE = internal,
    FINALFUNC = cloudsync_payload_encode_finalfn
);

-- Payload decoding and application
CREATE FUNCTION cloudsync_payload_decode(payload bytea)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_payload_decode'
LANGUAGE C VOLATILE;

-- Alias for payload_decode
CREATE FUNCTION cloudsync_payload_apply(payload bytea)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_payload_apply'
LANGUAGE C VOLATILE;

-- ============================================================================
-- Private/Internal Functions
-- ============================================================================

-- Check if table has sync metadata
CREATE FUNCTION cloudsync_is_sync(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_is_sync'
LANGUAGE C STABLE;

-- Internal insert handler (variadic for multiple PK columns)
CREATE FUNCTION cloudsync_insert(table_name text, VARIADIC pk_values anyarray)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_insert'
LANGUAGE C VOLATILE;

-- Internal delete handler (variadic for multiple PK columns)
CREATE FUNCTION cloudsync_delete(table_name text, VARIADIC pk_values anyarray)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_delete'
LANGUAGE C VOLATILE;

-- Internal update tracking (aggregate function)
CREATE FUNCTION cloudsync_update_transfn(state internal, table_name text, pk text, new_value anyelement)
RETURNS internal
AS 'MODULE_PATHNAME', 'cloudsync_update_transfn'
LANGUAGE C;

CREATE FUNCTION cloudsync_update_finalfn(state internal)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_update_finalfn'
LANGUAGE C;

CREATE AGGREGATE cloudsync_update(text, text, anyelement) (
    SFUNC = cloudsync_update_transfn,
    STYPE = internal,
    FINALFUNC = cloudsync_update_finalfn
);

-- Get sequence number
CREATE FUNCTION cloudsync_seq()
RETURNS integer
AS 'MODULE_PATHNAME', 'cloudsync_seq'
LANGUAGE C VOLATILE;

-- Encode primary key (variadic for multiple columns)
CREATE FUNCTION cloudsync_pk_encode(VARIADIC pk_values anyarray)
RETURNS bytea
AS 'MODULE_PATHNAME', 'cloudsync_pk_encode'
LANGUAGE C IMMUTABLE STRICT;

-- Decode primary key component
CREATE OR REPLACE FUNCTION cloudsync_pk_decode(encoded_pk bytea, index integer)
RETURNS text
AS 'MODULE_PATHNAME', 'cloudsync_pk_decode'
LANGUAGE C IMMUTABLE STRICT;
