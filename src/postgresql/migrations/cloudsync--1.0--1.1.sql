-- CloudSync PostgreSQL extension upgrade: 1.0 -> 1.1
--
-- Adds the chunked-payload SQL surface introduced in 1.1:
--   * cloudsync_payload_chunks() set-returning function (with the
--     exclude_filter_site_id flag used by the /check download path)
--   * cloudsync_payload_blob_checked() scalar helper for guarded legacy
--     monolithic payload materialization
--   * cloudsync_uuid_text() / cloudsync_uuid_blob() UUID conversion helpers
--
-- Run automatically by: ALTER EXTENSION cloudsync UPDATE;

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

