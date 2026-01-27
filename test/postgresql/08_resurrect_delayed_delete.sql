-- Resurrect after delete with delayed payload
-- Steps:
-- 1) Seed id1 on A, sync to B/C
-- 2) A deletes id1 and generates delete payload (do not apply yet on B)
-- 3) B recreates id1 with new value, sync to A/C
-- 4) Apply delayed delete payload from A to B/C
-- 5) Verify convergence

\set testid '08'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_08_a;
DROP DATABASE IF EXISTS cloudsync_test_08_b;
DROP DATABASE IF EXISTS cloudsync_test_08_c;
CREATE DATABASE cloudsync_test_08_a;
CREATE DATABASE cloudsync_test_08_b;
CREATE DATABASE cloudsync_test_08_c;

\connect cloudsync_test_08_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_a \gset

\connect cloudsync_test_08_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_b \gset

\connect cloudsync_test_08_c
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_c \gset

-- Round 1: seed id1 on A, sync to B/C
\connect cloudsync_test_08_a
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_08_a INSERT id1=seed_v1'
\endif
INSERT INTO smoke_tbl VALUES ('id1', 'seed_v1');
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

\connect cloudsync_test_08_b
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

\connect cloudsync_test_08_c
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

\connect cloudsync_test_08_a
\if :{?DEBUG_MERGE}
\echo '[INFO] round1 before merge cloudsync_test_08_a smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif
\if :payload_b_r1_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round1 apply b -> a'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r1', 3), 'hex')) AS _apply_a_r1_b \gset
\else
SELECT 0 AS _apply_a_r1_b \gset
\endif
\if :payload_c_r1_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round1 apply c -> a'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r1', 3), 'hex')) AS _apply_a_r1_c \gset
\else
SELECT 0 AS _apply_a_r1_c \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round1 after merge cloudsync_test_08_a smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif

\connect cloudsync_test_08_b
\if :{?DEBUG_MERGE}
\echo '[INFO] round1 before merge cloudsync_test_08_b smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif
\if :payload_a_r1_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round1 apply a -> b'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r1', 3), 'hex')) AS _apply_b_r1_a \gset
\else
SELECT 0 AS _apply_b_r1_a \gset
\endif
\if :payload_c_r1_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round1 apply c -> b'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r1', 3), 'hex')) AS _apply_b_r1_c \gset
\else
SELECT 0 AS _apply_b_r1_c \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round1 after merge cloudsync_test_08_b smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif

\connect cloudsync_test_08_c
\if :{?DEBUG_MERGE}
\echo '[INFO] round1 before merge cloudsync_test_08_c smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif
\if :payload_a_r1_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round1 apply a -> c'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r1', 3), 'hex')) AS _apply_c_r1_a \gset
\else
SELECT 0 AS _apply_c_r1_a \gset
\endif
\if :payload_b_r1_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round1 apply b -> c'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r1', 3), 'hex')) AS _apply_c_r1_b \gset
\else
SELECT 0 AS _apply_c_r1_b \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round1 after merge cloudsync_test_08_c smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif

-- Round 2: A deletes id1 (payload delayed for B/C)
\connect cloudsync_test_08_a
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_08_a DELETE id1'
\endif
DELETE FROM smoke_tbl WHERE id = 'id1';
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

-- Round 3: B recreates id1, sync to A/C (but A's delete still not applied on B/C)
\connect cloudsync_test_08_b
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_08_b UPSERT id1=recreate_v2'
\endif
INSERT INTO smoke_tbl (id, val)
VALUES ('id1', 'recreate_v2')
ON CONFLICT (id) DO UPDATE SET val = EXCLUDED.val;
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

\connect cloudsync_test_08_c
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

\connect cloudsync_test_08_a
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

\connect cloudsync_test_08_a
\if :{?DEBUG_MERGE}
\echo '[INFO] round3 before merge cloudsync_test_08_a smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif
\if :payload_b_r3_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round3 apply b -> a'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r3', 3), 'hex')) AS _apply_a_r3_b \gset
\else
SELECT 0 AS _apply_a_r3_b \gset
\endif
\if :payload_c_r3_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round3 apply c -> a'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r3', 3), 'hex')) AS _apply_a_r3_c \gset
\else
SELECT 0 AS _apply_a_r3_c \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round3 after merge cloudsync_test_08_a smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif

\connect cloudsync_test_08_b
\if :{?DEBUG_MERGE}
\echo '[INFO] round3 before merge cloudsync_test_08_b smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif
\if :payload_a_r3_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round3 apply a -> b'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r3', 3), 'hex')) AS _apply_b_r3_a \gset
\else
SELECT 0 AS _apply_b_r3_a \gset
\endif
\if :payload_c_r3_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round3 apply c -> b'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r3', 3), 'hex')) AS _apply_b_r3_c \gset
\else
SELECT 0 AS _apply_b_r3_c \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round3 after merge cloudsync_test_08_b smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif

\connect cloudsync_test_08_c
\if :{?DEBUG_MERGE}
\echo '[INFO] round3 before merge cloudsync_test_08_c smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif
\if :payload_a_r3_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round3 apply a -> c'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r3', 3), 'hex')) AS _apply_c_r3_a \gset
\else
SELECT 0 AS _apply_c_r3_a \gset
\endif
\if :payload_b_r3_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round3 apply b -> c'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r3', 3), 'hex')) AS _apply_c_r3_b \gset
\else
SELECT 0 AS _apply_c_r3_b \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round3 after merge cloudsync_test_08_c smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif

-- Round 4: apply delayed delete payload from A to B/C
\connect cloudsync_test_08_b
\if :{?DEBUG_MERGE}
\echo '[INFO] round4 before merge cloudsync_test_08_b smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif
\if :payload_a_r2_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round4 apply delayed a -> b'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r2', 3), 'hex')) AS _apply_b_r4_a_delayed \gset
\else
SELECT 0 AS _apply_b_r4_a_delayed \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round4 after merge cloudsync_test_08_b smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif

\connect cloudsync_test_08_c
\if :{?DEBUG_MERGE}
\echo '[INFO] round4 before merge cloudsync_test_08_c smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif
\if :payload_a_r2_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round4 apply delayed a -> c'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r2', 3), 'hex')) AS _apply_c_r4_a_delayed \gset
\else
SELECT 0 AS _apply_c_r4_a_delayed \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round4 after merge cloudsync_test_08_c smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif

-- Final consistency check across all three databases
\connect cloudsync_test_08_a
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_a
FROM smoke_tbl \gset

\connect cloudsync_test_08_b
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_b
FROM smoke_tbl \gset

\connect cloudsync_test_08_c
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_c
FROM smoke_tbl \gset

SELECT (:'smoke_hash_a' = :'smoke_hash_b' AND :'smoke_hash_a' = :'smoke_hash_c') AS multi_db_roundtrip_ok \gset
\if :multi_db_roundtrip_ok
\echo [PASS] (:testid) Resurrect after delete with delayed payload
\else
\echo [FAIL] (:testid) Resurrect after delete with delayed payload
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup: Drop test databases if not in DEBUG mode and no failures
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_08_a;
DROP DATABASE IF EXISTS cloudsync_test_08_b;
DROP DATABASE IF EXISTS cloudsync_test_08_c;
\endif