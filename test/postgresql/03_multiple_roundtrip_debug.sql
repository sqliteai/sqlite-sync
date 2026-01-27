-- usage:
-- - normal: `psql postgresql://postgres:postgres@localhost:5432/cloudsync_test -f test/postgresql/smoke_test_02_id1.sql`
-- - debug: `psql -v DEBUG=1 postgresql://postgres:postgres@localhost:5432/cloudsync_test -f test/postgresql/smoke_test_02_id1.sql`

\echo 'Running smoke_test_02_id1...'

\set ON_ERROR_STOP on
\set fail 0

-- 'Test multi-db roundtrip with concurrent updates (id1 only)'
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_a;
DROP DATABASE IF EXISTS cloudsync_test_b;
DROP DATABASE IF EXISTS cloudsync_test_c;
CREATE DATABASE cloudsync_test_a;
CREATE DATABASE cloudsync_test_b;
CREATE DATABASE cloudsync_test_c;

\connect cloudsync_test_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_a \gset

\connect cloudsync_test_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_b \gset

\connect cloudsync_test_c
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_c \gset

-- Round 1: independent inserts on each database (id1 only)
\connect cloudsync_test_a
INSERT INTO smoke_tbl VALUES ('id1', 'a1');
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

\connect cloudsync_test_b
INSERT INTO smoke_tbl VALUES ('id1', 'b1');
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_b_r1,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_b_r1_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_c
INSERT INTO smoke_tbl VALUES ('id1', 'c1');
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_c_r1,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_c_r1_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

-- Round 1 apply: fan-out changes
\connect cloudsync_test_a
\if :payload_b_r1_ok
\echo '[DEBUG] apply b -> a (round1)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r1', 3), 'hex')) AS _apply_a_r1_b \gset
\else
SELECT 0 AS _apply_a_r1_b \gset
\endif
\if :payload_c_r1_ok
\echo '[DEBUG] apply c -> a (round1)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r1', 3), 'hex')) AS _apply_a_r1_c \gset
\else
SELECT 0 AS _apply_a_r1_c \gset
\endif

\connect cloudsync_test_b
\if :payload_a_r1_ok
\echo '[DEBUG] apply a -> b (round1)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r1', 3), 'hex')) AS _apply_b_r1_a \gset
\else
SELECT 0 AS _apply_b_r1_a \gset
\endif
\if :payload_c_r1_ok
\echo '[DEBUG] apply c -> b (round1)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r1', 3), 'hex')) AS _apply_b_r1_c \gset
\else
SELECT 0 AS _apply_b_r1_c \gset
\endif

\connect cloudsync_test_c
\if :payload_a_r1_ok
\echo '[DEBUG] apply a -> c (round1)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r1', 3), 'hex')) AS _apply_c_r1_a \gset
\else
SELECT 0 AS _apply_c_r1_a \gset
\endif
\if :payload_b_r1_ok
\echo '[DEBUG] apply b -> c (round1)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r1', 3), 'hex')) AS _apply_c_r1_b \gset
\else
SELECT 0 AS _apply_c_r1_b \gset
\endif

-- Debug after round 1
\connect cloudsync_test_a
\echo '[DEBUG] round1 state cloudsync_test_a smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\echo '[DEBUG] round1 state cloudsync_test_a smoke_tbl_cloudsync'
SELECT * FROM smoke_tbl_cloudsync ORDER BY pk, col_name;

\connect cloudsync_test_b
\echo '[DEBUG] round1 state cloudsync_test_b smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\echo '[DEBUG] round1 state cloudsync_test_b smoke_tbl_cloudsync'
SELECT * FROM smoke_tbl_cloudsync ORDER BY pk, col_name;

\connect cloudsync_test_c
\echo '[DEBUG] round1 state cloudsync_test_c smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\echo '[DEBUG] round1 state cloudsync_test_c smoke_tbl_cloudsync'
SELECT * FROM smoke_tbl_cloudsync ORDER BY pk, col_name;

-- Round 2: concurrent updates on the same row (id1 only)
\connect cloudsync_test_a
UPDATE smoke_tbl SET val = 'a1_a' WHERE id = 'id1';
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

\connect cloudsync_test_b
UPDATE smoke_tbl SET val = 'a1_b' WHERE id = 'id1';
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_b_r2,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_b_r2_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_c
UPDATE smoke_tbl SET val = 'a1_c' WHERE id = 'id1';
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_c_r2,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_c_r2_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

-- Round 2 apply: fan-out changes
\connect cloudsync_test_a
\if :payload_b_r2_ok
\echo '[DEBUG] apply b -> a (round2)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r2', 3), 'hex')) AS _apply_a_r2_b \gset
\else
SELECT 0 AS _apply_a_r2_b \gset
\endif
\if :payload_c_r2_ok
\echo '[DEBUG] apply c -> a (round2)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r2', 3), 'hex')) AS _apply_a_r2_c \gset
\else
SELECT 0 AS _apply_a_r2_c \gset
\endif

\connect cloudsync_test_b
\if :payload_a_r2_ok
\echo '[DEBUG] apply a -> b (round2)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r2', 3), 'hex')) AS _apply_b_r2_a \gset
\else
SELECT 0 AS _apply_b_r2_a \gset
\endif
\if :payload_c_r2_ok
\echo '[DEBUG] apply c -> b (round2)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r2', 3), 'hex')) AS _apply_b_r2_c \gset
\else
SELECT 0 AS _apply_b_r2_c \gset
\endif

\connect cloudsync_test_c
\if :payload_a_r2_ok
\echo '[DEBUG] apply a -> c (round2)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r2', 3), 'hex')) AS _apply_c_r2_a \gset
\else
SELECT 0 AS _apply_c_r2_a \gset
\endif
\if :payload_b_r2_ok
\echo '[DEBUG] apply b -> c (round2)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r2', 3), 'hex')) AS _apply_c_r2_b \gset
\else
SELECT 0 AS _apply_c_r2_b \gset
\endif

-- Debug after round 2
\connect cloudsync_test_a
\echo '[DEBUG] round2 state cloudsync_test_a smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\echo '[DEBUG] round2 state cloudsync_test_a smoke_tbl_cloudsync'
SELECT * FROM smoke_tbl_cloudsync ORDER BY pk, col_name;

\connect cloudsync_test_b
\echo '[DEBUG] round2 state cloudsync_test_b smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\echo '[DEBUG] round2 state cloudsync_test_b smoke_tbl_cloudsync'
SELECT * FROM smoke_tbl_cloudsync ORDER BY pk, col_name;

\connect cloudsync_test_c
\echo '[DEBUG] round2 state cloudsync_test_c smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\echo '[DEBUG] round2 state cloudsync_test_c smoke_tbl_cloudsync'
SELECT * FROM smoke_tbl_cloudsync ORDER BY pk, col_name;

-- Round 3: additional operations to force another sync cycle (no id1 changes)
\connect cloudsync_test_a
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_a_r3,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_a_r3_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_b
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

\connect cloudsync_test_c
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_c_r3,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_c_r3_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

-- Round 3 apply: final fan-out
\connect cloudsync_test_a
\if :payload_b_r3_ok
\echo '[DEBUG] apply b -> a (round3)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r3', 3), 'hex')) AS _apply_a_r3_b \gset
\else
SELECT 0 AS _apply_a_r3_b \gset
\endif
\if :payload_c_r3_ok
\echo '[DEBUG] apply c -> a (round3)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r3', 3), 'hex')) AS _apply_a_r3_c \gset
\else
SELECT 0 AS _apply_a_r3_c \gset
\endif

\connect cloudsync_test_b
\if :payload_a_r3_ok
\echo '[DEBUG] apply a -> b (round3)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r3', 3), 'hex')) AS _apply_b_r3_a \gset
\else
SELECT 0 AS _apply_b_r3_a \gset
\endif
\if :payload_c_r3_ok
\echo '[DEBUG] apply c -> b (round3)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r3', 3), 'hex')) AS _apply_b_r3_c \gset
\else
SELECT 0 AS _apply_b_r3_c \gset
\endif

\connect cloudsync_test_c
\if :payload_a_r3_ok
\echo '[DEBUG] apply a -> c (round3)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r3', 3), 'hex')) AS _apply_c_r3_a \gset
\else
SELECT 0 AS _apply_c_r3_a \gset
\endif
\if :payload_b_r3_ok
\echo '[DEBUG] apply b -> c (round3)'
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r3', 3), 'hex')) AS _apply_c_r3_b \gset
\else
SELECT 0 AS _apply_c_r3_b \gset
\endif

-- Debug after round 3
\connect cloudsync_test_a
\echo '[DEBUG] round3 state cloudsync_test_a smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\echo '[DEBUG] round3 state cloudsync_test_a smoke_tbl_cloudsync'
SELECT * FROM smoke_tbl_cloudsync ORDER BY pk, col_name;

\connect cloudsync_test_b
\echo '[DEBUG] round3 state cloudsync_test_b smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\echo '[DEBUG] round3 state cloudsync_test_b smoke_tbl_cloudsync'
SELECT * FROM smoke_tbl_cloudsync ORDER BY pk, col_name;

\connect cloudsync_test_c
\echo '[DEBUG] round3 state cloudsync_test_c smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\echo '[DEBUG] round3 state cloudsync_test_c smoke_tbl_cloudsync'
SELECT * FROM smoke_tbl_cloudsync ORDER BY pk, col_name;

-- Final consistency check across all three databases (id1 only)
\connect cloudsync_test_a
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_a
FROM smoke_tbl WHERE id = 'id1' \gset

\connect cloudsync_test_b
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_b
FROM smoke_tbl WHERE id = 'id1' \gset

\connect cloudsync_test_c
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_c
FROM smoke_tbl WHERE id = 'id1' \gset

SELECT (:'smoke_hash_a' = :'smoke_hash_b' AND :'smoke_hash_a' = :'smoke_hash_c') AS multi_db_roundtrip_ok \gset
\if :multi_db_roundtrip_ok
\echo '[PASS] Test multi-db roundtrip with concurrent updates (id1 only)'
\else
\echo '[FAIL] Test multi-db roundtrip with concurrent updates (id1 only)'
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test summary'
\echo '\nTest summary:'
\echo - Failures: :fail
SELECT (:fail::int > 0) AS fail_any \gset
\if :fail_any
\echo smoke test failed: :fail test(s) failed
DO $$ BEGIN
  RAISE EXCEPTION 'smoke test failed';
END $$;
\else
\echo - Status: OK
\endif
