-- Payload chunks positional-cursor resume
--
-- Proves the positional cursor on cloudsync_payload_chunks tiles a window exactly:
-- resuming at any chunk's (next_db_version, next_seq, next_frag_offset) reproduces
-- the following chunk byte-for-byte, including boundaries that fall inside a single
-- committed db_version and inside a value larger than the chunk budget. No spool
-- table, no idempotent overlap.
--
-- Part 2 is end-to-end: drain the whole window the way the /check job will (one
-- chunk per call via the positional cursor), apply that stream to a fresh database,
-- and assert the receiver's table content hashes identically to the source.

\set testid '55-positional'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_55_positional;
DROP DATABASE IF EXISTS cloudsync_test_55_positional_dst;
CREATE DATABASE cloudsync_test_55_positional;
CREATE DATABASE cloudsync_test_55_positional_dst;

\connect cloudsync_test_55_positional
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE split_test (id TEXT PRIMARY KEY, body BYTEA DEFAULT '\x'::bytea);
SELECT cloudsync_init('split_test', 'CLS', 1) AS _init \gset
SELECT cloudsync_set('payload_max_chunk_size', '262144');

-- tx1: many medium incompressible rows in one statement -> a single db_version
-- split across several chunks (row-boundary resumes, incl. resumes landing INSIDE
-- one committed version that the legacy since>db_version cursor could not express).
INSERT INTO split_test(id, body)
SELECT format('row-%s', lpad(i::text, 4, '0')),
       decode((SELECT string_agg(md5((i * 1000 + j)::text), '') FROM generate_series(1, 88) AS s(j)), 'hex')
FROM generate_series(1, 500) AS g(i);

-- tx2: one value larger than the chunk budget -> v3 fragments (mid-fragment resumes).
INSERT INTO split_test(id, body)
VALUES ('big', decode((SELECT string_agg(md5(j::text), '') FROM generate_series(1, 30000) AS s(j)), 'hex'));

-- For each non-final chunk of the full-window scan, resume at its reported cursor
-- and fetch the first chunk; it must equal the next chunk of the full scan. The
-- correlated SRF subquery uses ORDER BY ... LIMIT 1 so each resume call drains
-- fully (no early-terminated value-per-call SRF).
WITH base AS (
  SELECT chunk_index, payload, next_db_version, next_seq, next_frag_offset, is_final, watermark_db_version
  FROM cloudsync_payload_chunks(0, cloudsync_siteid(), NULL, false)
),
resumed AS (
  SELECT b.chunk_index,
         (b.next_frag_offset > 0) AS is_frag_boundary,
         (SELECT r.payload
            FROM cloudsync_payload_chunks(NULL, cloudsync_siteid(),
                                          (SELECT max(watermark_db_version) FROM base), false,
                                          b.next_db_version, b.next_seq, b.next_frag_offset) r
            ORDER BY r.chunk_index LIMIT 1) AS next_payload
  FROM base b
  WHERE NOT b.is_final
)
SELECT
  (SELECT count(*) FROM base) AS base_count,
  coalesce((SELECT bool_and(r.next_payload = b2.payload)
              FROM resumed r JOIN base b2 ON b2.chunk_index = r.chunk_index + 1), false) AS chunks_identical,
  coalesce((SELECT bool_or(is_frag_boundary) FROM resumed), false) AS saw_frag
\gset

SELECT (:base_count::int >= 4) AS enough_chunks \gset
\if :enough_chunks
\echo [PASS] (:testid) window produced multiple chunks (:base_count)
\else
\echo [FAIL] (:testid) expected a multi-chunk window, got :base_count
SELECT (:fail::int + 1) AS fail \gset
\endif

\if :chunks_identical
\echo [PASS] (:testid) positional resume reproduced every following chunk byte-for-byte
\else
\echo [FAIL] (:testid) a positional resume did not reproduce the next chunk
SELECT (:fail::int + 1) AS fail \gset
\endif

\if :saw_frag
\echo [PASS] (:testid) mid-fragment resume exercised
\else
\echo [FAIL] (:testid) mid-fragment resume not exercised
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Part 2: end-to-end drain + apply round-trip.
--
-- Drain the window exactly as the /check job will: start with the legacy
-- exclusive cursor (since=0), then step the positional cursor one chunk per call,
-- collecting payloads in drain order. ORDER BY chunk_index LIMIT 1 forces each
-- value-per-call SRF to run to completion (no early-terminated cursor). The drained
-- chunks are returned hex-joined so they can cross \connect into the receiver DB.
CREATE OR REPLACE FUNCTION _positional_drain_hex() RETURNS text LANGUAGE plpgsql AS $$
DECLARE
  rdbv bigint; rseq bigint; rfrag bigint; wm bigint := 0;
  rec record; parts text[] := '{}'; guard int := 0;
BEGIN
  LOOP
    guard := guard + 1;
    IF guard > 100000 THEN RAISE EXCEPTION 'positional drain did not terminate'; END IF;
    IF wm = 0 THEN
      SELECT * INTO rec FROM cloudsync_payload_chunks(0, cloudsync_siteid(), NULL, false)
      ORDER BY chunk_index LIMIT 1;
      IF NOT FOUND THEN EXIT; END IF;
      wm := rec.watermark_db_version;
    ELSE
      SELECT * INTO rec FROM cloudsync_payload_chunks(NULL, cloudsync_siteid(), wm, false, rdbv, rseq, rfrag)
      ORDER BY chunk_index LIMIT 1;
      IF NOT FOUND THEN EXIT; END IF;
    END IF;
    parts := array_append(parts, encode(rec.payload, 'hex'));
    rdbv := rec.next_db_version; rseq := rec.next_seq; rfrag := rec.next_frag_offset;
    EXIT WHEN rec.is_final;
  END LOOP;
  RETURN array_to_string(parts, ',');
END $$;

SELECT _positional_drain_hex() AS chunks_hex \gset
SELECT
  md5(string_agg(id || ':' || encode(body, 'hex'), '|' ORDER BY id)) AS src_hash,
  count(*) AS src_count
FROM split_test \gset

\connect cloudsync_test_55_positional_dst
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE split_test (id TEXT PRIMARY KEY, body BYTEA DEFAULT '\x'::bytea);
SELECT cloudsync_init('split_test', 'CLS', 1) AS _init_dst \gset
SELECT cloudsync_set('payload_max_chunk_size', '262144');

-- Reconstitute the drained chunks and apply them (reverse order on purpose: apply
-- must be order-independent and reassemble fragments regardless).
CREATE TEMP TABLE chunk_transport(ord int, payload bytea);
INSERT INTO chunk_transport(ord, payload)
SELECT ord::int, decode(chunk_hex, 'hex')
FROM unnest(string_to_array(:'chunks_hex', ',')) WITH ORDINALITY AS t(chunk_hex, ord);

SELECT coalesce(sum(cloudsync_payload_apply(payload)), 0) AS applied_rows
FROM (SELECT payload FROM chunk_transport ORDER BY ord DESC) AS ordered \gset

SELECT
  md5(string_agg(id || ':' || encode(body, 'hex'), '|' ORDER BY id)) AS dst_hash,
  count(*) AS dst_count
FROM split_test \gset

SELECT (:'dst_hash' = :'src_hash' AND :dst_count::int = :src_count::int
        AND :dst_count::int > 0) AS roundtrip_ok \gset
\if :roundtrip_ok
\echo [PASS] (:testid) positional drain applied to a fresh database reproduces the source (:dst_count rows)
\else
\echo [FAIL] (:testid) drain/apply mismatch (src_count=:src_count dst_count=:dst_count hashes :'src_hash' vs :'dst_hash')
SELECT (:fail::int + 1) AS fail \gset
\endif

\ir helper_test_cleanup.sql
\if :should_cleanup
\connect postgres
DROP DATABASE IF EXISTS cloudsync_test_55_positional;
DROP DATABASE IF EXISTS cloudsync_test_55_positional_dst;
\endif
