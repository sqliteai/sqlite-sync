-- Payload chunks positional-cursor resume
--
-- Proves the positional cursor on cloudsync_payload_chunks tiles a window exactly:
-- resuming at any chunk's (next_db_version, next_seq, next_frag_offset) reproduces
-- the following chunk byte-for-byte, including boundaries that fall inside a single
-- committed db_version and inside a value larger than the chunk budget. No spool
-- table, no idempotent overlap.

\set testid '55-positional'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_55_positional;
CREATE DATABASE cloudsync_test_55_positional;

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

\ir helper_test_cleanup.sql
\if :should_cleanup
\connect postgres
DROP DATABASE IF EXISTS cloudsync_test_55_positional;
\endif
