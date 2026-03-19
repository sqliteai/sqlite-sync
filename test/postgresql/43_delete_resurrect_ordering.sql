-- Test delete/resurrect with out-of-order payload delivery
-- Verifies CRDT causal length parity handles resurrection correctly
-- even when payloads arrive in non-causal order.

\set testid '43'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_43_a;
DROP DATABASE IF EXISTS cloudsync_test_43_b;
DROP DATABASE IF EXISTS cloudsync_test_43_c;
CREATE DATABASE cloudsync_test_43_a;
CREATE DATABASE cloudsync_test_43_b;
CREATE DATABASE cloudsync_test_43_c;

-- Setup all three databases
\connect cloudsync_test_43_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE test_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('test_tbl', 'CLS', true) AS _init_a \gset

\connect cloudsync_test_43_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE test_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('test_tbl', 'CLS', true) AS _init_b \gset

\connect cloudsync_test_43_c
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE test_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('test_tbl', 'CLS', true) AS _init_c \gset

-- Round 1: A inserts row, sync to all
\connect cloudsync_test_43_a
INSERT INTO test_tbl VALUES ('row1', 'original');
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_a_r1,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_a_r1_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_43_b
\if :payload_a_r1_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r1', 3), 'hex')) AS _apply \gset
\endif

\connect cloudsync_test_43_c
\if :payload_a_r1_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r1', 3), 'hex')) AS _apply \gset
\endif

-- Round 2: A deletes row (CL goes 1->2)
\connect cloudsync_test_43_a
DELETE FROM test_tbl WHERE id = 'row1';
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_a_r2,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_a_r2_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

-- Sync delete to B
\connect cloudsync_test_43_b
\if :payload_a_r2_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r2', 3), 'hex')) AS _apply \gset
\endif

-- Round 3: B re-inserts (CL goes 2->3, resurrection)
\connect cloudsync_test_43_b
INSERT INTO test_tbl VALUES ('row1', 'resurrected_by_b');
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_b_r3,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_b_r3_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

-- C receives payloads in REVERSE order: B's resurrection FIRST, then A's delete
\connect cloudsync_test_43_c
\if :payload_b_r3_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r3', 3), 'hex')) AS _apply_b \gset
\endif
\if :payload_a_r2_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r2', 3), 'hex')) AS _apply_a \gset
\endif

-- A receives B's resurrection
\connect cloudsync_test_43_a
\if :payload_b_r3_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r3', 3), 'hex')) AS _apply_b \gset
\endif

-- Final convergence check: all three should have row1 with 'resurrected_by_b'
\connect cloudsync_test_43_a
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS hash_a
FROM test_tbl \gset

\connect cloudsync_test_43_b
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS hash_b
FROM test_tbl \gset

\connect cloudsync_test_43_c
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS hash_c
FROM test_tbl \gset

SELECT (:'hash_a' = :'hash_b' AND :'hash_b' = :'hash_c') AS all_converge \gset
\if :all_converge
\echo [PASS] (:testid) All 3 databases converge after out-of-order delete/resurrect
\else
\echo [FAIL] (:testid) Databases diverged - A: :hash_a, B: :hash_b, C: :hash_c
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify the row exists (resurrection won)
SELECT COUNT(*) = 1 AS row_exists
FROM test_tbl
WHERE id = 'row1' \gset
\if :row_exists
\echo [PASS] (:testid) Resurrected row exists on C (received out of order)
\else
\echo [FAIL] (:testid) Resurrected row missing on C
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_43_a;
DROP DATABASE IF EXISTS cloudsync_test_43_b;
DROP DATABASE IF EXISTS cloudsync_test_43_c;
\endif
