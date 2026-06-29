-- Reproduction probe for finding #2: cloudsync_payload_chunks() stages
-- fragment-spanning state (tbl/pk/col_name/col_value/site_id) that is palloc'd
-- by payload_chunks_fetch_current in the SRF's per-call memory context, then
-- read again on later SRF_PERCALL invocations when emitting fragments 1..N of a
-- single oversized value. Per the SRF protocol only multi_call_memory_ctx is
-- guaranteed to survive between calls, so reading that state on a later call is
-- a use-after-free in theory.
--
-- This test forces a single column value large enough to span MANY fragments
-- (each ~256KB, the technical minimum), gives each fragment region a distinct
-- byte pattern so any cross-fragment corruption is visible, round-trips the
-- chunks through apply on a fresh database, and compares the recovered value to
-- the source byte-for-byte. If fragments 1..N read freed/clobbered memory the
-- reassembly checksum fails ("inconsistent v3 fragments") or the recovered value
-- differs -> [FAIL]. If the per-call state survives (current FROM-clause path)
-- it round-trips cleanly -> [PASS].

\set testid '54-frag-state'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_54_frag_src;
DROP DATABASE IF EXISTS cloudsync_test_54_frag_dst;
CREATE DATABASE cloudsync_test_54_frag_src;
CREATE DATABASE cloudsync_test_54_frag_dst;

\connect cloudsync_test_54_frag_src
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE frag_test (
  id TEXT PRIMARY KEY,
  note TEXT DEFAULT ''
);
SELECT cloudsync_init('frag_test', 'CLS', 1) AS _init_src \gset
SELECT cloudsync_set('payload_max_chunk_size', '1');  -- clamps to 256KB minimum

-- One oversized value (~1.18 MB) whose fragments straddle distinct regions:
-- A...A | B...B | C...C | D...D | E...E. Cross-fragment corruption (reading the
-- wrong region after a per-call context reset) would change the recovered text.
INSERT INTO frag_test(id, note)
VALUES ('big', repeat('A', 262144) || repeat('B', 262144) || repeat('C', 262144)
              || repeat('D', 262144) || repeat('E', 131072));

-- Confirm this single value actually fragments into several v3 chunks.
SELECT
  count(*) FILTER (WHERE get_byte(payload, 4) = 3) AS v3_chunks,
  count(*) AS total_chunks
FROM cloudsync_payload_chunks() \gset
\if :{?v3_chunks}
\endif
SELECT (:v3_chunks::int >= 3) AS multi_fragment_ok \gset
\if :multi_fragment_ok
\echo [PASS] (:testid) Oversized value fragmented into :v3_chunks v3 fragments (:total_chunks chunks total)
\else
\echo [FAIL] (:testid) Expected >=3 v3 fragments, got :v3_chunks (test cannot exercise multi-fragment state)
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT
  md5(note) AS src_md5,
  length(note) AS src_len
FROM frag_test WHERE id = 'big' \gset

-- Capture every chunk for transport, in generation order.
SELECT string_agg(encode(payload, 'hex'), ',' ORDER BY chunk_index) AS chunks_hex
FROM cloudsync_payload_chunks() \gset

\connect cloudsync_test_54_frag_dst
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE frag_test (
  id TEXT PRIMARY KEY,
  note TEXT DEFAULT ''
);
SELECT cloudsync_init('frag_test', 'CLS', 1) AS _init_dst \gset
SELECT cloudsync_set('payload_max_chunk_size', '262144');

CREATE TEMP TABLE chunk_transport(ord INT, payload BYTEA);
INSERT INTO chunk_transport(ord, payload)
SELECT ord::int, decode(chunk_hex, 'hex')
FROM unnest(string_to_array(:'chunks_hex', ',')) WITH ORDINALITY AS t(chunk_hex, ord);

SELECT coalesce(sum(cloudsync_payload_apply(payload)), 0) AS applied_rows
FROM (SELECT payload FROM chunk_transport ORDER BY ord) AS ordered \gset

SELECT
  md5(note) AS dst_md5,
  length(note) AS dst_len
FROM frag_test WHERE id = 'big' \gset

SELECT (:'dst_md5' = :'src_md5' AND :dst_len::bigint = :src_len::bigint) AS frag_roundtrip_ok \gset
\if :frag_roundtrip_ok
\echo [PASS] (:testid) Multi-fragment oversized value round-trips byte-exact (:src_len bytes)
\else
\echo [FAIL] (:testid) Multi-fragment value corrupted across SRF calls (src_len=:src_len dst_len=:dst_len src_md5=:src_md5 dst_md5=:dst_md5)
SELECT (:fail::int + 1) AS fail \gset
\endif

\ir helper_test_cleanup.sql
\if :should_cleanup
\connect postgres
DROP DATABASE IF EXISTS cloudsync_test_54_frag_src;
DROP DATABASE IF EXISTS cloudsync_test_54_frag_dst;
\endif
