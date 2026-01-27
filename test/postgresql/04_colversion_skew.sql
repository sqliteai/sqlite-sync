-- 'Test multi-db roundtrip with skewed col_version updates'
--   - concurrent update pattern where A/B/C perform 2/1/3 updates respectively on id1 before syncing. 
--   - It follows the same apply order as the existing 3‑DB test and verifies final convergence across all three databases

\set testid '04'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_04_a;
DROP DATABASE IF EXISTS cloudsync_test_04_b;
DROP DATABASE IF EXISTS cloudsync_test_04_c;
CREATE DATABASE cloudsync_test_04_a;
CREATE DATABASE cloudsync_test_04_b;
CREATE DATABASE cloudsync_test_04_c;

\connect cloudsync_test_04_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_a \gset

\connect cloudsync_test_04_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_b \gset

\connect cloudsync_test_04_c
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_c \gset

-- Round 1: seed id1 on a single database, then sync
\connect cloudsync_test_04_a
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_04_a INSERT id1=seed_a1'
\endif
INSERT INTO smoke_tbl VALUES ('id1', 'seed_a1');
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

\connect cloudsync_test_04_b
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

\connect cloudsync_test_04_c
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
\connect cloudsync_test_04_a
\if :{?DEBUG_MERGE}
\echo '[INFO] round1 before merge cloudsync_test_04_a smoke_tbl'
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
\echo '[INFO] round1 after merge cloudsync_test_04_a smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif

\connect cloudsync_test_04_b
\if :{?DEBUG_MERGE}
\echo '[INFO] round1 before merge cloudsync_test_04_b smoke_tbl'
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
\echo '[INFO] round1 after merge cloudsync_test_04_b smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif

\connect cloudsync_test_04_c
\if :{?DEBUG_MERGE}
\echo '[INFO] round1 before merge cloudsync_test_04_c smoke_tbl'
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
\echo '[INFO] round1 after merge cloudsync_test_04_c smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif

-- Round 2: skewed concurrent updates on id1
\connect cloudsync_test_04_a
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_04_a UPDATE id1=a1_u1'
\endif
UPDATE smoke_tbl SET val = 'a1_u1' WHERE id = 'id1';
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_04_a UPDATE id1=a1_u2'
\endif
UPDATE smoke_tbl SET val = 'a1_u2' WHERE id = 'id1';
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

\connect cloudsync_test_04_b
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_04_b UPDATE id1=b1_u1'
\endif
UPDATE smoke_tbl SET val = 'b1_u1' WHERE id = 'id1';
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

\connect cloudsync_test_04_c
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_04_c UPDATE id1=c1_u1'
\endif
UPDATE smoke_tbl SET val = 'c1_u1' WHERE id = 'id1';
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_04_c UPDATE id1=c1_u2'
\endif
UPDATE smoke_tbl SET val = 'c1_u2' WHERE id = 'id1';
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_04_c UPDATE id1=c1_u3'
\endif
UPDATE smoke_tbl SET val = 'c1_u3' WHERE id = 'id1';
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
\connect cloudsync_test_04_a
\if :{?DEBUG_MERGE}
\echo '[INFO] round2 before merge cloudsync_test_04_a smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif
\if :payload_b_r2_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round2 apply b -> a'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r2', 3), 'hex')) AS _apply_a_r2_b \gset
\else
SELECT 0 AS _apply_a_r2_b \gset
\endif
\if :payload_c_r2_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round2 apply c -> a'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r2', 3), 'hex')) AS _apply_a_r2_c \gset
\else
SELECT 0 AS _apply_a_r2_c \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round2 after merge cloudsync_test_04_a smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif

\connect cloudsync_test_04_b
\if :{?DEBUG_MERGE}
\echo '[INFO] round2 before merge cloudsync_test_04_b smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif
\if :payload_a_r2_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round2 apply a -> b'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r2', 3), 'hex')) AS _apply_b_r2_a \gset
\else
SELECT 0 AS _apply_b_r2_a \gset
\endif
\if :payload_c_r2_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round2 apply c -> b'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r2', 3), 'hex')) AS _apply_b_r2_c \gset
\else
SELECT 0 AS _apply_b_r2_c \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round2 after merge cloudsync_test_04_b smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif

\connect cloudsync_test_04_c
\if :{?DEBUG_MERGE}
\echo '[INFO] round2 before merge cloudsync_test_04_c smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif
\if :payload_a_r2_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round2 apply a -> c'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r2', 3), 'hex')) AS _apply_c_r2_a \gset
\else
SELECT 0 AS _apply_c_r2_a \gset
\endif
\if :payload_b_r2_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round2 apply b -> c'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r2', 3), 'hex')) AS _apply_c_r2_b \gset
\else
SELECT 0 AS _apply_c_r2_b \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round2 after merge cloudsync_test_04_c smoke_tbl'
SELECT * FROM smoke_tbl ORDER BY id;
\endif

-- Final consistency check across all three databases
\connect cloudsync_test_04_a
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_a
FROM smoke_tbl \gset

\connect cloudsync_test_04_b
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_b
FROM smoke_tbl \gset

\connect cloudsync_test_04_c
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_c
FROM smoke_tbl \gset

SELECT (:'smoke_hash_a' = :'smoke_hash_b' AND :'smoke_hash_a' = :'smoke_hash_c') AS multi_db_roundtrip_ok \gset
\if :multi_db_roundtrip_ok
\echo [PASS] (:testid) Test multi-db roundtrip with skewed col_version updates
\else
\echo [FAIL] (:testid) Test multi-db roundtrip with skewed col_version updates
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup: Drop test databases if not in DEBUG mode and no failures
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_04_a;
DROP DATABASE IF EXISTS cloudsync_test_04_b;
DROP DATABASE IF EXISTS cloudsync_test_04_c;
\endif