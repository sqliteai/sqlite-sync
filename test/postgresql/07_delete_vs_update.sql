-- Concurrent delete vs update
-- Steps:
-- 1) Seed id1 on A, sync to B/C
-- 2) B deletes id1 while C updates id1, then sync
-- 3) A updates id1 after merge, then sync

\set testid '07'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_07_a;
DROP DATABASE IF EXISTS cloudsync_test_07_b;
DROP DATABASE IF EXISTS cloudsync_test_07_c;
CREATE DATABASE cloudsync_test_07_a;
CREATE DATABASE cloudsync_test_07_b;
CREATE DATABASE cloudsync_test_07_c;

\connect cloudsync_test_07_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_a \gset

\connect cloudsync_test_07_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_b \gset

\connect cloudsync_test_07_c
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_c \gset

-- Round 1: seed id1 on A, sync to B/C
\connect cloudsync_test_07_a
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

\connect cloudsync_test_07_b
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

\connect cloudsync_test_07_c
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

\connect cloudsync_test_07_a
\if :payload_b_r1_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r1', 3), 'hex')) AS _apply_a_r1_b \gset
\else
SELECT 0 AS _apply_a_r1_b \gset
\endif
\if :payload_c_r1_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r1', 3), 'hex')) AS _apply_a_r1_c \gset
\else
SELECT 0 AS _apply_a_r1_c \gset
\endif

\connect cloudsync_test_07_b
\if :payload_a_r1_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r1', 3), 'hex')) AS _apply_b_r1_a \gset
\else
SELECT 0 AS _apply_b_r1_a \gset
\endif
\if :payload_c_r1_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r1', 3), 'hex')) AS _apply_b_r1_c \gset
\else
SELECT 0 AS _apply_b_r1_c \gset
\endif

\connect cloudsync_test_07_c
\if :payload_a_r1_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r1', 3), 'hex')) AS _apply_c_r1_a \gset
\else
SELECT 0 AS _apply_c_r1_a \gset
\endif
\if :payload_b_r1_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r1', 3), 'hex')) AS _apply_c_r1_b \gset
\else
SELECT 0 AS _apply_c_r1_b \gset
\endif

-- Round 2: B deletes id1, C updates id1, then sync
\connect cloudsync_test_07_b
DELETE FROM smoke_tbl WHERE id = 'id1';
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

\connect cloudsync_test_07_c
UPDATE smoke_tbl SET val = 'c1_update' WHERE id = 'id1';
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

\connect cloudsync_test_07_a
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

\connect cloudsync_test_07_a
\if :payload_b_r2_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r2', 3), 'hex')) AS _apply_a_r2_b \gset
\else
SELECT 0 AS _apply_a_r2_b \gset
\endif
\if :payload_c_r2_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r2', 3), 'hex')) AS _apply_a_r2_c \gset
\else
SELECT 0 AS _apply_a_r2_c \gset
\endif

\connect cloudsync_test_07_b
\if :payload_a_r2_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r2', 3), 'hex')) AS _apply_b_r2_a \gset
\else
SELECT 0 AS _apply_b_r2_a \gset
\endif
\if :payload_c_r2_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r2', 3), 'hex')) AS _apply_b_r2_c \gset
\else
SELECT 0 AS _apply_b_r2_c \gset
\endif

\connect cloudsync_test_07_c
\if :payload_a_r2_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r2', 3), 'hex')) AS _apply_c_r2_a \gset
\else
SELECT 0 AS _apply_c_r2_a \gset
\endif
\if :payload_b_r2_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r2', 3), 'hex')) AS _apply_c_r2_b \gset
\else
SELECT 0 AS _apply_c_r2_b \gset
\endif

-- Round 3: A updates id1 after merge, then sync
\connect cloudsync_test_07_a
UPDATE smoke_tbl SET val = 'a1_post_merge' WHERE id = 'id1';
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

\connect cloudsync_test_07_b
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

\connect cloudsync_test_07_c
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

\connect cloudsync_test_07_a
\if :payload_b_r3_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r3', 3), 'hex')) AS _apply_a_r3_b \gset
\else
SELECT 0 AS _apply_a_r3_b \gset
\endif
\if :payload_c_r3_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r3', 3), 'hex')) AS _apply_a_r3_c \gset
\else
SELECT 0 AS _apply_a_r3_c \gset
\endif

\connect cloudsync_test_07_b
\if :payload_a_r3_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r3', 3), 'hex')) AS _apply_b_r3_a \gset
\else
SELECT 0 AS _apply_b_r3_a \gset
\endif
\if :payload_c_r3_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r3', 3), 'hex')) AS _apply_b_r3_c \gset
\else
SELECT 0 AS _apply_b_r3_c \gset
\endif

\connect cloudsync_test_07_c
\if :payload_a_r3_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r3', 3), 'hex')) AS _apply_c_r3_a \gset
\else
SELECT 0 AS _apply_c_r3_a \gset
\endif
\if :payload_b_r3_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r3', 3), 'hex')) AS _apply_c_r3_b \gset
\else
SELECT 0 AS _apply_c_r3_b \gset
\endif

-- Final consistency check across all three databases
\connect cloudsync_test_07_a
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_a
FROM smoke_tbl \gset

\connect cloudsync_test_07_b
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_b
FROM smoke_tbl \gset

\connect cloudsync_test_07_c
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_c
FROM smoke_tbl \gset

SELECT (:'smoke_hash_a' = :'smoke_hash_b' AND :'smoke_hash_a' = :'smoke_hash_c') AS multi_db_roundtrip_ok \gset
\if :multi_db_roundtrip_ok
\echo [PASS] (:testid) Concurrent delete vs update
\else
\echo [FAIL] (:testid) Concurrent delete vs update
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup: Drop test databases if not in DEBUG mode and no failures
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_07_a;
DROP DATABASE IF EXISTS cloudsync_test_07_b;
DROP DATABASE IF EXISTS cloudsync_test_07_c;
\endif