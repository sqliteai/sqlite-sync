-- CloudSync PostgreSQL extension upgrade: 1.1 -> 1.2
--
-- Bounds what one call to cloudsync_payload_chunks() prepares:
--   * new max_window_bytes input, declared last so the existing positional
--     arguments 1..7 keep their meaning
--   * new window_capped output, true when the scan stopped on the budget
--     rather than because the window was drained
--
-- Both are optional: without max_window_bytes the function behaves exactly as
-- it did in 1.1.
--
-- Run automatically by: ALTER EXTENSION cloudsync UPDATE;

-- The old function has to go before the new one is created. CREATE OR REPLACE
-- cannot change a return type ("cannot change return type of existing
-- function"), and because the new parameter has a default, keeping both would
-- leave a 7-argument call matching two candidates -- an ambiguous function
-- call error at every existing call site.
DROP FUNCTION IF EXISTS cloudsync_payload_chunks(bigint, bytea, bigint, boolean, bigint, bigint, bigint);

CREATE OR REPLACE FUNCTION cloudsync_payload_chunks(
  since_db_version bigint DEFAULT NULL,
  filter_site_id bytea DEFAULT NULL,
  until_db_version bigint DEFAULT NULL,
  exclude_filter_site_id boolean DEFAULT false,
  resume_db_version bigint DEFAULT NULL,
  resume_seq bigint DEFAULT NULL,
  resume_frag_offset bigint DEFAULT NULL,
  max_window_bytes bigint DEFAULT NULL
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
  is_final boolean,
  window_capped boolean
)
AS 'MODULE_PATHNAME', 'cloudsync_payload_chunks'
LANGUAGE C VOLATILE;
