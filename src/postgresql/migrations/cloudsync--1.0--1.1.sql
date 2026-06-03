-- CloudSync PostgreSQL extension upgrade: 1.0 -> 1.1
--
-- Adds the chunked-payload SQL surface introduced in 1.1:
--   * cloudsync_payload_chunks() set-returning function (with the
--     exclude_filter_site_id flag used by the /check download path)
--   * cloudsync_uuid_text() / cloudsync_uuid_blob() UUID conversion helpers
--
-- Run automatically by: ALTER EXTENSION cloudsync UPDATE;

CREATE OR REPLACE FUNCTION cloudsync_payload_chunks(
  since_db_version bigint DEFAULT NULL,
  filter_site_id bytea DEFAULT NULL,
  until_db_version bigint DEFAULT NULL,
  exclude_filter_site_id boolean DEFAULT false
)
RETURNS TABLE (
  payload bytea,
  chunk_index bigint,
  payload_size bigint,
  rows bigint,
  db_version_min bigint,
  db_version_max bigint,
  watermark_db_version bigint
)
AS 'MODULE_PATHNAME', 'cloudsync_payload_chunks'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_uuid_text(uuid bytea, dash_format boolean DEFAULT true)
RETURNS text
AS 'MODULE_PATHNAME', 'cloudsync_uuid_text'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION cloudsync_uuid_blob(uuid text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'cloudsync_uuid_blob'
LANGUAGE C IMMUTABLE;

-- Download spool: the /check path fills a window's chunk stream once and pages it
-- out one chunk per call so the network driver never re-materializes the whole
-- stream. See cloudsync.sql.in for the rationale.
CREATE TABLE IF NOT EXISTS cloudsync_payload_spool (
  stream_id text NOT NULL,
  chunk_index bigint NOT NULL,
  payload bytea NOT NULL,
  payload_size bigint NOT NULL,
  db_version_min bigint NOT NULL,
  db_version_max bigint NOT NULL,
  watermark bigint NOT NULL,
  is_final boolean NOT NULL DEFAULT false,
  created_at bigint NOT NULL DEFAULT extract(epoch FROM now())::bigint,
  PRIMARY KEY (stream_id, chunk_index)
);

CREATE OR REPLACE FUNCTION cloudsync_payload_spool_fill(
  p_stream_id text,
  p_since_db_version bigint,
  p_filter_site_id bytea DEFAULT NULL,
  p_exclude_filter_site_id boolean DEFAULT false
) RETURNS bigint AS $$
DECLARE
  existing bigint;
  cnt bigint := 0;
  rec record;
BEGIN
  DELETE FROM cloudsync_payload_spool
  WHERE stream_id IN (
    SELECT s.stream_id FROM cloudsync_payload_spool s
    GROUP BY s.stream_id
    HAVING max(s.created_at) < extract(epoch FROM now())::bigint - 86400);

  SELECT count(*) INTO existing FROM cloudsync_payload_spool WHERE stream_id = p_stream_id;
  IF existing > 0 THEN
    RETURN existing;
  END IF;

  FOR rec IN
    SELECT c.payload, c.chunk_index, c.payload_size, c.db_version_min, c.db_version_max, c.watermark_db_version
    FROM cloudsync_payload_chunks(p_since_db_version, p_filter_site_id, NULL, p_exclude_filter_site_id) c
  LOOP
    INSERT INTO cloudsync_payload_spool
      (stream_id, chunk_index, payload, payload_size, db_version_min, db_version_max, watermark, is_final)
    VALUES (p_stream_id, rec.chunk_index, rec.payload, rec.payload_size,
            rec.db_version_min, rec.db_version_max, rec.watermark_db_version, false);
    cnt := cnt + 1;
  END LOOP;

  IF cnt > 0 THEN
    UPDATE cloudsync_payload_spool SET is_final = true
    WHERE stream_id = p_stream_id
      AND chunk_index = (SELECT max(x.chunk_index) FROM cloudsync_payload_spool x
                         WHERE x.stream_id = p_stream_id);
  END IF;

  RETURN cnt;
END;
$$ LANGUAGE plpgsql VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_payload_spool_drop(p_stream_id text)
RETURNS bigint AS $$
DECLARE
  deleted bigint := 0;
BEGIN
  DELETE FROM cloudsync_payload_spool WHERE stream_id = p_stream_id;
  GET DIAGNOSTICS deleted = ROW_COUNT;
  RETURN deleted;
END;
$$ LANGUAGE plpgsql VOLATILE;
