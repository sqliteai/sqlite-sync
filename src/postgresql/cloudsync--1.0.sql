-- CloudSync Extension for PostgreSQL
-- Version 1.0

-- Complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION cloudsync" to load this file. \quit

-- ============================================================================
-- Public Functions
-- ============================================================================

-- Get extension version
CREATE OR REPLACE FUNCTION cloudsync_version()
RETURNS text
AS 'MODULE_PATHNAME', 'cloudsync_version'
LANGUAGE C IMMUTABLE STRICT;

-- Get site identifier (UUID)
CREATE OR REPLACE FUNCTION cloudsync_siteid()
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_cloudsync_siteid'
LANGUAGE C STABLE;

-- Generate a new UUID
CREATE OR REPLACE FUNCTION cloudsync_uuid()
RETURNS uuid
AS 'MODULE_PATHNAME', 'cloudsync_uuid'
LANGUAGE C VOLATILE;

-- Get current database version
CREATE OR REPLACE FUNCTION cloudsync_db_version()
RETURNS bigint
AS 'MODULE_PATHNAME', 'cloudsync_db_version'
LANGUAGE C STABLE;

-- Get next database version (with optional merging version)
CREATE OR REPLACE FUNCTION cloudsync_db_version_next()
RETURNS bigint
AS 'MODULE_PATHNAME', 'cloudsync_db_version_next'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_db_version_next(merging_version bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'cloudsync_db_version_next'
LANGUAGE C VOLATILE;

-- Initialize CloudSync for a table (3 variants for 1-3 arguments)
-- Returns site_id as bytea
CREATE OR REPLACE FUNCTION cloudsync_init(table_name text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'cloudsync_init'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_init(table_name text, algo text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'cloudsync_init'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_init(table_name text, algo text, init_flags integer)
RETURNS bytea
AS 'MODULE_PATHNAME', 'cloudsync_init'
LANGUAGE C VOLATILE;

-- Enable sync for a table
CREATE OR REPLACE FUNCTION cloudsync_enable(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_enable'
LANGUAGE C VOLATILE;

-- Disable sync for a table
CREATE OR REPLACE FUNCTION cloudsync_disable(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_disable'
LANGUAGE C VOLATILE;

-- Check if table is sync-enabled
CREATE OR REPLACE FUNCTION cloudsync_is_enabled(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_is_enabled'
LANGUAGE C STABLE;

-- Cleanup orphaned metadata for a table
CREATE OR REPLACE FUNCTION cloudsync_cleanup(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_cleanup'
LANGUAGE C VOLATILE;

-- Terminate CloudSync
CREATE OR REPLACE FUNCTION cloudsync_terminate()
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_terminate'
LANGUAGE C VOLATILE;

-- Set global configuration
CREATE OR REPLACE FUNCTION cloudsync_set(key text, value text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_set'
LANGUAGE C VOLATILE;

-- Set table-level configuration
CREATE OR REPLACE FUNCTION cloudsync_set_table(table_name text, key text, value text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_set_table'
LANGUAGE C VOLATILE;

-- Set row-level filter for conditional sync
CREATE OR REPLACE FUNCTION cloudsync_set_filter(table_name text, filter_expr text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_set_filter'
LANGUAGE C VOLATILE;

-- Clear row-level filter
CREATE OR REPLACE FUNCTION cloudsync_clear_filter(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_clear_filter'
LANGUAGE C VOLATILE;

-- Set column-level configuration
CREATE OR REPLACE FUNCTION cloudsync_set_column(table_name text, column_name text, key text, value text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_set_column'
LANGUAGE C VOLATILE;

-- Begin schema alteration
CREATE OR REPLACE FUNCTION cloudsync_begin_alter(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_begin_alter'
LANGUAGE C VOLATILE;

-- Commit schema alteration
CREATE OR REPLACE FUNCTION cloudsync_commit_alter(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_commit_alter'
LANGUAGE C VOLATILE;

-- Payload encoding (aggregate function)
CREATE OR REPLACE FUNCTION cloudsync_payload_encode_transfn(state internal, tbl text, pk bytea, col_name text, col_value bytea, col_version bigint, db_version bigint, site_id bytea, cl bigint, seq bigint)
RETURNS internal
AS 'MODULE_PATHNAME', 'cloudsync_payload_encode_transfn'
LANGUAGE C;

CREATE OR REPLACE FUNCTION cloudsync_payload_encode_finalfn(state internal)
RETURNS bytea
AS 'MODULE_PATHNAME', 'cloudsync_payload_encode_finalfn'
LANGUAGE C;

CREATE OR REPLACE AGGREGATE cloudsync_payload_encode(text, bytea, text, bytea, bigint, bigint, bytea, bigint, bigint) (
    SFUNC = cloudsync_payload_encode_transfn,
    STYPE = internal,
    FINALFUNC = cloudsync_payload_encode_finalfn
);

-- Payload decoding and application
CREATE OR REPLACE FUNCTION cloudsync_payload_decode(payload bytea)
RETURNS integer
AS 'MODULE_PATHNAME', 'cloudsync_payload_decode'
LANGUAGE C VOLATILE;

-- Alias for payload_decode
CREATE OR REPLACE FUNCTION cloudsync_payload_apply(payload bytea)
RETURNS integer
AS 'MODULE_PATHNAME', 'pg_cloudsync_payload_apply'
LANGUAGE C VOLATILE;

-- ============================================================================
-- Private/Internal Functions
-- ============================================================================

-- Check if table has sync metadata
CREATE OR REPLACE FUNCTION cloudsync_is_sync(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_is_sync'
LANGUAGE C STABLE;

-- Internal insert handler (variadic for multiple PK columns)
CREATE OR REPLACE FUNCTION cloudsync_insert(table_name text, VARIADIC pk_values "any")
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_insert'
LANGUAGE C VOLATILE;

-- Internal delete handler (variadic for multiple PK columns)
CREATE OR REPLACE FUNCTION cloudsync_delete(table_name text, VARIADIC pk_values "any")
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_delete'
LANGUAGE C VOLATILE;

-- Internal update tracking (aggregate function)
CREATE OR REPLACE FUNCTION cloudsync_update_transfn(state internal, table_name text, new_value anyelement, old_value anyelement)
RETURNS internal
AS 'MODULE_PATHNAME', 'cloudsync_update_transfn'
LANGUAGE C;

CREATE OR REPLACE FUNCTION cloudsync_update_finalfn(state internal)
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_update_finalfn'
LANGUAGE C;

CREATE AGGREGATE cloudsync_update(text, anyelement, anyelement) (
    SFUNC = cloudsync_update_transfn,
    STYPE = internal,
    FINALFUNC = cloudsync_update_finalfn
);

-- Get sequence number
CREATE OR REPLACE FUNCTION cloudsync_seq()
RETURNS integer
AS 'MODULE_PATHNAME', 'cloudsync_seq'
LANGUAGE C VOLATILE;

-- Encode primary key (variadic for multiple columns)
CREATE OR REPLACE FUNCTION cloudsync_pk_encode(VARIADIC pk_values "any")
RETURNS bytea
AS 'MODULE_PATHNAME', 'cloudsync_pk_encode'
LANGUAGE C IMMUTABLE STRICT;

-- Decode primary key component
CREATE OR REPLACE FUNCTION cloudsync_pk_decode(encoded_pk bytea, index integer)
RETURNS text
AS 'MODULE_PATHNAME', 'cloudsync_pk_decode'
LANGUAGE C IMMUTABLE STRICT;

-- ============================================================================
-- Changes Functions
-- ============================================================================

CREATE OR REPLACE FUNCTION cloudsync_encode_value(anyelement)
RETURNS bytea
AS 'MODULE_PATHNAME', 'cloudsync_encode_value'
LANGUAGE C IMMUTABLE;

-- Encoded column value helper (PG): returns cloudsync-encoded bytea
CREATE OR REPLACE FUNCTION cloudsync_col_value(
  table_name text,
  col_name text,
  pk bytea
)
RETURNS bytea
AS 'MODULE_PATHNAME', 'cloudsync_col_value'
LANGUAGE C STABLE;

-- SetReturningFunction: To implement SELECT FROM cloudsync_changes
CREATE FUNCTION cloudsync_changes_select(
  min_db_version bigint DEFAULT 0,
  filter_site_id bytea DEFAULT NULL
)
RETURNS TABLE (
    tbl text,
    pk bytea,
    col_name text,
    col_value bytea,     -- pk_encoded value bytes
    col_version bigint,
    db_version bigint,
    site_id bytea,
    cl bigint,
    seq bigint
)
AS 'MODULE_PATHNAME', 'cloudsync_changes_select'
LANGUAGE C STABLE;

-- View con lo stesso nome della vtab SQLite
CREATE OR REPLACE VIEW cloudsync_changes AS
SELECT * FROM cloudsync_changes_select(0, NULL);

-- Trigger function to implement INSERT on the cloudsync_changes view
CREATE FUNCTION cloudsync_changes_insert_trigger()
RETURNS trigger
AS 'MODULE_PATHNAME', 'cloudsync_changes_insert_trigger'
LANGUAGE C;

CREATE OR REPLACE TRIGGER cloudsync_changes_insert
INSTEAD OF INSERT ON cloudsync_changes
FOR EACH ROW
EXECUTE FUNCTION cloudsync_changes_insert_trigger();

-- Set current schema name
CREATE OR REPLACE FUNCTION cloudsync_set_schema(schema text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_set_schema'
LANGUAGE C VOLATILE;

-- Get current schema name (if any)
CREATE OR REPLACE FUNCTION cloudsync_schema()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_cloudsync_schema'
LANGUAGE C VOLATILE;

-- Get current schema name (if any)
CREATE OR REPLACE FUNCTION cloudsync_table_schema(table_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'pg_cloudsync_table_schema'
LANGUAGE C VOLATILE;

-- ============================================================================
-- Block-level LWW Functions
-- ============================================================================

-- Materialize block-level column into base table
CREATE OR REPLACE FUNCTION cloudsync_text_materialize(table_name text, col_name text, VARIADIC pk_values "any")
RETURNS boolean
AS 'MODULE_PATHNAME', 'cloudsync_text_materialize'
LANGUAGE C VOLATILE;

-- ============================================================================
-- Type Casts
-- ============================================================================

-- Cast function: converts bigint to boolean (0 = false, non-zero = true)
-- Required because BOOLEAN values are encoded as INT8 in sync payloads,
-- but PostgreSQL has no built-in cast from bigint to boolean.
CREATE FUNCTION cloudsync_int8_to_bool(bigint) RETURNS boolean AS $$
    SELECT $1 <> 0
$$ LANGUAGE SQL IMMUTABLE STRICT;

-- ASSIGNMENT cast: auto-applies in INSERT/UPDATE context only
-- This enables BOOLEAN column sync where values are encoded as INT8.
-- Using ASSIGNMENT (not IMPLICIT) to avoid unintended conversions in WHERE clauses.
CREATE CAST (bigint AS boolean)
    WITH FUNCTION cloudsync_int8_to_bool(bigint)
    AS ASSIGNMENT;
