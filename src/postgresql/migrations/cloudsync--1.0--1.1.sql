-- CloudSync PostgreSQL extension upgrade: 1.0 -> 1.1
--
-- Adds the chunked-payload SQL surface introduced in 1.1:
--   * cloudsync_payload_chunks() set-returning function (with the
--     exclude_filter_site_id flag used by the /check download path)
--   * cloudsync_payload_blob_checked() scalar helper for guarded legacy
--     monolithic payload materialization
--   * cloudsync_uuid_text() / cloudsync_uuid_blob() UUID conversion helpers
--   * cloudsync_payload_fragments spool table for oversized v3 fragment
--     values
--
-- Run automatically by: ALTER EXTENSION cloudsync UPDATE;

-- Spool table for oversized v3 fragment values. Created here (as the
-- extension owner) because on an already-configured database the first role
-- to touch it may be a sync-only role without CREATE privileges; fresh
-- setups get it from dbutils_settings_init like the other cloudsync tables.
-- Detached from the extension right after creation so DROP EXTENSION and
-- pg_dump treat it exactly like those runtime-created tables. The DO block
-- skips both steps when the table already exists (detach would fail on a
-- non-member table).
DO $$
BEGIN
    IF to_regclass('cloudsync_payload_fragments') IS NULL THEN
        CREATE TABLE cloudsync_payload_fragments (
            value_id TEXT NOT NULL, part_index BIGINT NOT NULL, part_count BIGINT NOT NULL, total_size BIGINT NOT NULL,
            checksum TEXT NOT NULL, created_at BIGINT NOT NULL DEFAULT (EXTRACT(EPOCH FROM now())::bigint),
            tbl TEXT NOT NULL, pk BYTEA NOT NULL, col_name TEXT NOT NULL, col_version BIGINT NOT NULL, db_version BIGINT NOT NULL,
            site_id BYTEA NOT NULL, cl BIGINT NOT NULL, seq BIGINT NOT NULL, fragment BYTEA NOT NULL,
            PRIMARY KEY(value_id, part_index)
        );
        ALTER EXTENSION cloudsync DROP TABLE cloudsync_payload_fragments;
    END IF;
END $$;

CREATE OR REPLACE FUNCTION cloudsync_payload_chunks(
  since_db_version bigint DEFAULT NULL,
  filter_site_id bytea DEFAULT NULL,
  until_db_version bigint DEFAULT NULL,
  exclude_filter_site_id boolean DEFAULT false,
  resume_db_version bigint DEFAULT NULL,
  resume_seq bigint DEFAULT NULL,
  resume_frag_offset bigint DEFAULT NULL
)
RETURNS TABLE (
  payload bytea,
  chunk_index bigint,
  payload_size bigint,
  rows bigint,
  db_version_min bigint,
  db_version_max bigint,
  watermark_db_version bigint,
  next_db_version bigint,
  next_seq bigint,
  next_frag_offset bigint,
  is_final boolean
)
AS 'MODULE_PATHNAME', 'cloudsync_payload_chunks'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_payload_blob_checked(
  since_db_version bigint,
  since_seq bigint,
  filter_site_id bytea,
  exclude_filter_site_id boolean,
  max_estimated_payload_size bigint
)
RETURNS bytea
AS 'MODULE_PATHNAME', 'cloudsync_payload_blob_checked'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_uuid_text(uuid bytea, dash_format boolean DEFAULT true)
RETURNS text
AS 'MODULE_PATHNAME', 'cloudsync_uuid_text'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION cloudsync_uuid_blob(uuid text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'cloudsync_uuid_blob'
LANGUAGE C IMMUTABLE;

