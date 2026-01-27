-- 'Test multi-db roundtrip with concurrent updates'

\set testid '03' 
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_03_a;
DROP DATABASE IF EXISTS cloudsync_test_03_b;
DROP DATABASE IF EXISTS cloudsync_test_03_c;
CREATE DATABASE cloudsync_test_03_a;
CREATE DATABASE cloudsync_test_03_b;
CREATE DATABASE cloudsync_test_03_c;

\connect cloudsync_test_03_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_a \gset

\connect cloudsync_test_03_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_b \gset

\connect cloudsync_test_03_c
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_c \gset

-- Round 1: independent inserts on each database
\connect cloudsync_test_03_a
INSERT INTO smoke_tbl VALUES ('id1', 'a1');
INSERT INTO smoke_tbl VALUES ('id2', 'a2');
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

\connect cloudsync_test_03_b
INSERT INTO smoke_tbl VALUES ('id3', 'b3');
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

\connect cloudsync_test_03_c
INSERT INTO smoke_tbl VALUES ('id4', 'c4');
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
\connect cloudsync_test_03_a
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

\connect cloudsync_test_03_b
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

\connect cloudsync_test_03_c
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

-- Round 2: concurrent updates on the same row + mixed operations
\connect cloudsync_test_03_a
UPDATE smoke_tbl SET val = 'a1_a' WHERE id = 'id1';
DELETE FROM smoke_tbl WHERE id = 'id2';
INSERT INTO smoke_tbl VALUES ('id5', 'a5');
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

\connect cloudsync_test_03_b
UPDATE smoke_tbl SET val = 'a1_b' WHERE id = 'id1';
UPDATE smoke_tbl SET val = 'b3_b' WHERE id = 'id3';
INSERT INTO smoke_tbl VALUES ('id6', 'b6');
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

\connect cloudsync_test_03_c
UPDATE smoke_tbl SET val = 'a1_c' WHERE id = 'id1';
DELETE FROM smoke_tbl WHERE id = 'id4';
INSERT INTO smoke_tbl VALUES ('id7', 'c7');
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
\connect cloudsync_test_03_a
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

\connect cloudsync_test_03_b
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

\connect cloudsync_test_03_c
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

-- Round 3: additional operations to force another sync cycle
\connect cloudsync_test_03_a
UPDATE smoke_tbl SET val = 'b3_a' WHERE id = 'id3';
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

\connect cloudsync_test_03_b
DELETE FROM smoke_tbl WHERE id = 'id5';
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

\connect cloudsync_test_03_c
UPDATE smoke_tbl SET val = 'b6_c' WHERE id = 'id6';
INSERT INTO smoke_tbl VALUES ('id8', 'c8');
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
\connect cloudsync_test_03_a
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

\connect cloudsync_test_03_b
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

\connect cloudsync_test_03_c
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
\connect cloudsync_test_03_a
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_a
FROM smoke_tbl \gset

\connect cloudsync_test_03_b
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_b
FROM smoke_tbl \gset

\connect cloudsync_test_03_c
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_c
FROM smoke_tbl \gset

SELECT (:'smoke_hash_a' = :'smoke_hash_b' AND :'smoke_hash_a' = :'smoke_hash_c') AS multi_db_roundtrip_ok \gset
\if :multi_db_roundtrip_ok
\echo [PASS] (:testid) Test multi-db roundtrip with concurrent updates
\else
\echo [FAIL] (:testid) Test multi-db roundtrip with concurrent updates
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup: Drop test databases if not in DEBUG mode and no failures
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_03_a;
DROP DATABASE IF EXISTS cloudsync_test_03_b;
DROP DATABASE IF EXISTS cloudsync_test_03_c;
\endif