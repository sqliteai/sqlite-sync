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

-- The shape that matters most: ~13 rows fill a 256 KB chunk, so transactions of 12 then
-- 13 rows keep every chunk boundary one row inside a db_version. Waiting for a chunk to
-- end on a boundary never succeeds here, so a cap that only checks after a chunk is
-- built never fires and the whole history comes out in one window.
SELECT format($f$INSERT INTO items (id, v) SELECT 'd%s_' || i, (SELECT decode(string_agg(md5(random()::text || g::text), ''), 'hex') FROM generate_series(1, 1250) g) FROM generate_series(1, %s) i$f$, t, CASE WHEN t = 1 THEN 12 ELSE 13 END)
  FROM generate_series(1, 20) t \gexec

-- Baseline: the whole window, uncapped. Nothing is capped without a budget.
SELECT sum(rows) AS base_rows, sum(payload_size) AS base_bytes,
       max(watermark_db_version) AS base_wm,
       (count(*) > 1 AND sum(rows) > 0 AND bool_or(window_capped) IS FALSE) AS base_ok
  FROM cloudsync_payload_chunks(0, NULL, NULL, false) \gset
\if :base_ok
\echo [PASS] (:testid) uncapped baseline spans several chunks and reports no cap
\else
\echo [FAIL] (:testid) baseline rows=:base_rows bytes=:base_bytes
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Drain the same stream in capped windows, following watermark_db_version each time,
-- and add up what the windows covered. A budget below one chunk also proves the drain
-- still advances: a db_version larger than the whole budget must be emitted in full.
CREATE FUNCTION pg_temp.drain_capped_from(start_since bigint, cap bigint)
RETURNS TABLE (windows int, nrows bigint, maxbytes bigint, last_wm bigint, contiguous boolean) AS $$
DECLARE
  since bigint := start_since;
  w int := 0;
  c bigint := 0;
  b bigint := 0;
  ok boolean := true;
  r record;
BEGIN
  LOOP
    SELECT count(*) AS n, coalesce(sum(rows), 0) AS nr, coalesce(sum(payload_size), 0) AS sz,
           max(watermark_db_version) FILTER (WHERE is_final) AS wm,
           bool_or(window_capped) AS capped, min(db_version_min) AS first_dbv
      INTO r
      FROM cloudsync_payload_chunks(since, NULL, NULL, false, NULL, NULL, NULL, cap);
    EXIT WHEN r.n = 0;
    -- Windows must abut exactly, and each must advance or the drain never ends.
    IF r.first_dbv <> since + 1 OR r.wm <= since THEN ok := false; END IF;
    -- A window ends at the first db_version boundary at or after the budget, so it can
    -- overshoot by at most the chunk that crossed it plus the version in progress.
    w := w + 1; c := c + r.nr; b := greatest(b, r.sz); since := r.wm;
    EXIT WHEN NOT r.capped OR w > 100;
  END LOOP;
  RETURN QUERY SELECT w, c, b, since, ok;
END $$ LANGUAGE plpgsql;

SELECT d.windows AS w1, d.nrows AS c1, d.maxbytes AS b1, d.last_wm AS wm1,
       (d.windows > 1 AND d.contiguous AND d.nrows = :base_rows::bigint
        AND d.maxbytes <= 200000 + 3 * 262144 AND d.last_wm = :base_wm::bigint) AS cap1_ok
  FROM pg_temp.drain_capped_from(0, 200000) d \gset
\if :cap1_ok
\echo [PASS] (:testid) a 200 KB budget splits the stream into :w1 windows that tile it exactly
\else
\echo [FAIL] (:testid) windows=:w1 rows=:c1/:base_rows maxwindow=:b1 wm=:wm1/:base_wm
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT d.windows AS w2, d.nrows AS c2, d.maxbytes AS b2, d.last_wm AS wm2,
       (d.windows > 1 AND d.contiguous AND d.nrows = :base_rows::bigint
        AND d.maxbytes <= 1 + 3 * 262144 AND d.last_wm = :base_wm::bigint) AS cap2_ok
  FROM pg_temp.drain_capped_from(0, 1) d \gset
\if :cap2_ok
\echo [PASS] (:testid) a 1-byte budget still advances and tiles the stream exactly
\else
\echo [FAIL] (:testid) windows=:w2 rows=:c2/:base_rows maxwindow=:b2 wm=:wm2/:base_wm
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

-- A history of oversized values is emitted entirely as fragment chunks, which the
-- ordinary chunk builder never produces. Those bytes still have to spend the budget, or
-- such a history never reaches the cap at all.
INSERT INTO items (id, v) SELECT 'f1', (SELECT decode(string_agg(md5(random()::text || g::text), ''), 'hex') FROM generate_series(1, 18750) g);
INSERT INTO items (id, v) SELECT 'f2', (SELECT decode(string_agg(md5(random()::text || g::text), ''), 'hex') FROM generate_series(1, 18750) g);
INSERT INTO items (id, v) SELECT 'f3', (SELECT decode(string_agg(md5(random()::text || g::text), ''), 'hex') FROM generate_series(1, 18750) g);
INSERT INTO items (id, v) SELECT 'f4', (SELECT decode(string_agg(md5(random()::text || g::text), ''), 'hex') FROM generate_series(1, 18750) g);
INSERT INTO items (id, v) SELECT 'f5', (SELECT decode(string_agg(md5(random()::text || g::text), ''), 'hex') FROM generate_series(1, 18750) g);
INSERT INTO items (id, v) SELECT 'f6', (SELECT decode(string_agg(md5(random()::text || g::text), ''), 'hex') FROM generate_series(1, 18750) g);
INSERT INTO items (id, v) SELECT 'f7', (SELECT decode(string_agg(md5(random()::text || g::text), ''), 'hex') FROM generate_series(1, 18750) g);
INSERT INTO items (id, v) SELECT 'f8', (SELECT decode(string_agg(md5(random()::text || g::text), ''), 'hex') FROM generate_series(1, 18750) g);

SELECT sum(rows) AS frag_rows, max(watermark_db_version) AS frag_wm
  FROM cloudsync_payload_chunks(:base_wm, NULL, NULL, false) \gset

SELECT d.windows AS w3, d.nrows AS c3, d.last_wm AS wm3,
       (d.windows > 1 AND d.contiguous AND d.nrows = :frag_rows::bigint
        AND d.last_wm = :frag_wm::bigint) AS cap3_ok
  FROM pg_temp.drain_capped_from(:base_wm, 200000) d \gset
\if :cap3_ok
\echo [PASS] (:testid) a purely fragmented history is capped into :w3 windows
\else
\echo [FAIL] (:testid) windows=:w3 rows=:c3/:frag_rows wm=:wm3/:frag_wm
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_65;
