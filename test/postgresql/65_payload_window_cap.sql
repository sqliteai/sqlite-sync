-- max_window_bytes bounds what one cloudsync_payload_chunks() call prepares. A capped
-- window is an ordinary complete stream over a smaller window, so repeated calls must
-- tile the stream exactly: same chunks, same bytes, no gap and no overlap.
--
-- Two shapes are needed in the data. Single-row transactions give chunk boundaries that
-- coincide with db_version boundaries, which is where a window may end. The large
-- transactions are bigger than one chunk, so a chunk boundary also falls inside a
-- db_version -- that is what exercises the rule that a window must not end there,
-- because the next window resumes with db_version > since and would skip the remainder.

\set testid '65-window-cap'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_65;
CREATE DATABASE cloudsync_test_65;

\connect cloudsync_test_65
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE items (id TEXT PRIMARY KEY NOT NULL, v BYTEA);
SELECT cloudsync_init('items', 'CLS', 1) AS _init \gset
-- The smallest chunk size the setting allows, so the rows below span several chunks.
SELECT cloudsync_set('payload_max_chunk_size', '262144') AS _chunk \gset

-- 40 single-row transactions, then two of 15 rows (~300 KB each, larger than a chunk).
-- \gexec runs each generated INSERT as its own command, so each is its own transaction
-- and therefore its own db_version -- a DO block would make all of them one. The values
-- are random because the payload is LZ4-compressed: a repeating pattern would collapse
-- to a few bytes and never fill a chunk.
SELECT format($f$INSERT INTO items (id, v) SELECT 'r%s', (SELECT decode(string_agg(md5(random()::text), ''), 'hex') FROM generate_series(1, 1250))$f$, i)
  FROM generate_series(1, 40) i \gexec

BEGIN;
INSERT INTO items (id, v)
  SELECT 'b1_' || i, (SELECT decode(string_agg(md5(random()::text || g::text), ''), 'hex') FROM generate_series(1, 1250) g)
    FROM generate_series(1, 15) i;
COMMIT;
BEGIN;
INSERT INTO items (id, v)
  SELECT 'b2_' || i, (SELECT decode(string_agg(md5(random()::text || g::text), ''), 'hex') FROM generate_series(1, 1250) g)
    FROM generate_series(1, 15) i;
COMMIT;

-- Baseline: the whole window, uncapped. Nothing is capped without a budget.
SELECT count(*) AS base_chunks, sum(payload_size) AS base_bytes,
       max(watermark_db_version) AS base_wm,
       (count(*) > 1 AND sum(payload_size) > 0 AND bool_or(window_capped) IS FALSE) AS base_ok
  FROM cloudsync_payload_chunks(0, NULL, NULL, false) \gset
\if :base_ok
\echo [PASS] (:testid) uncapped baseline spans several chunks and reports no cap
\else
\echo [FAIL] (:testid) baseline chunks=:base_chunks bytes=:base_bytes
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Drain the same stream in capped windows, following watermark_db_version each time,
-- and add up what the windows covered. A budget below one chunk also proves the drain
-- still advances: a db_version larger than the whole budget must be emitted in full.
CREATE FUNCTION pg_temp.drain_capped(cap bigint)
RETURNS TABLE (windows int, chunks bigint, bytes bigint, last_wm bigint, contiguous boolean) AS $$
DECLARE
  since bigint := 0;
  w int := 0;
  c bigint := 0;
  b bigint := 0;
  ok boolean := true;
  r record;
BEGIN
  LOOP
    SELECT count(*) AS n, coalesce(sum(payload_size), 0) AS sz,
           max(watermark_db_version) AS wm, bool_or(window_capped) AS capped,
           min(db_version_min) AS first_dbv
      INTO r
      FROM cloudsync_payload_chunks(since, NULL, NULL, false, NULL, NULL, NULL, cap);
    EXIT WHEN r.n = 0;
    -- Windows must abut exactly, and each must advance or the drain never ends.
    IF r.first_dbv <> since + 1 OR r.wm <= since THEN ok := false; END IF;
    w := w + 1; c := c + r.n; b := b + r.sz; since := r.wm;
    EXIT WHEN NOT r.capped OR w > 100;
  END LOOP;
  RETURN QUERY SELECT w, c, b, since, ok;
END $$ LANGUAGE plpgsql;

SELECT d.windows AS w1, d.chunks AS c1, d.bytes AS b1, d.last_wm AS wm1,
       (d.windows > 1 AND d.contiguous AND d.chunks = :base_chunks::bigint
        AND d.bytes = :base_bytes::bigint AND d.last_wm = :base_wm::bigint) AS cap1_ok
  FROM pg_temp.drain_capped(200000) d \gset
\if :cap1_ok
\echo [PASS] (:testid) a 200 KB budget splits the stream into :w1 windows that tile it exactly
\else
\echo [FAIL] (:testid) windows=:w1 chunks=:c1/:base_chunks bytes=:b1/:base_bytes wm=:wm1/:base_wm
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT d.windows AS w2, d.chunks AS c2, d.bytes AS b2, d.last_wm AS wm2,
       (d.windows > 1 AND d.contiguous AND d.chunks = :base_chunks::bigint
        AND d.bytes = :base_bytes::bigint AND d.last_wm = :base_wm::bigint) AS cap2_ok
  FROM pg_temp.drain_capped(1) d \gset
\if :cap2_ok
\echo [PASS] (:testid) a 1-byte budget still advances and tiles the stream exactly
\else
\echo [FAIL] (:testid) windows=:w2 chunks=:c2/:base_chunks bytes=:b2/:base_bytes wm=:wm2/:base_wm
SELECT (:fail::int + 1) AS fail \gset
\endif

-- The positional resume still works alongside the new argument.
SELECT count(*) AS res_chunks, min(db_version_min) AS res_first,
       (count(*) > 0 AND min(db_version_min) = 5) AS res_ok
  FROM cloudsync_payload_chunks(NULL, NULL, :base_wm, false, 5, 0, 0) \gset
\if :res_ok
\echo [PASS] (:testid) positional resume is unaffected by the new argument
\else
\echo [FAIL] (:testid) positional resume returned :res_chunks chunk(s) starting at :res_first
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_65;
