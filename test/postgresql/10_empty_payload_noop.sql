-- Empty payload + no-op merge
-- Steps:
-- 1) Setup three DBs and table
-- 2) Attempt to encode/apply empty payloads
-- 3) Verify data unchanged and hashes match

\set testid '10'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_10_a;
DROP DATABASE IF EXISTS cloudsync_test_10_b;
DROP DATABASE IF EXISTS cloudsync_test_10_c;
CREATE DATABASE cloudsync_test_10_a;
CREATE DATABASE cloudsync_test_10_b;
CREATE DATABASE cloudsync_test_10_c;

\connect cloudsync_test_10_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_a \gset

\connect cloudsync_test_10_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_b \gset

\connect cloudsync_test_10_c
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_c \gset

-- Seed a stable row so hashes are meaningful
\connect cloudsync_test_10_a
INSERT INTO smoke_tbl VALUES ('id1', 'seed_v1');
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_a_seed,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_a_seed_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_10_b
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_b_seed,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_b_seed_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_10_c
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_c_seed,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_c_seed_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

-- Apply seed payloads so all DBs start in sync
\connect cloudsync_test_10_a
\if :payload_b_seed_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_seed', 3), 'hex')) AS _apply_a_seed_b \gset
\else
SELECT 0 AS _apply_a_seed_b \gset
\endif
\if :payload_c_seed_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_seed', 3), 'hex')) AS _apply_a_seed_c \gset
\else
SELECT 0 AS _apply_a_seed_c \gset
\endif

\connect cloudsync_test_10_b
\if :payload_a_seed_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_seed', 3), 'hex')) AS _apply_b_seed_a \gset
\else
SELECT 0 AS _apply_b_seed_a \gset
\endif
\if :payload_c_seed_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_seed', 3), 'hex')) AS _apply_b_seed_c \gset
\else
SELECT 0 AS _apply_b_seed_c \gset
\endif

\connect cloudsync_test_10_c
\if :payload_a_seed_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_seed', 3), 'hex')) AS _apply_c_seed_a \gset
\else
SELECT 0 AS _apply_c_seed_a \gset
\endif
\if :payload_b_seed_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_seed', 3), 'hex')) AS _apply_c_seed_b \gset
\else
SELECT 0 AS _apply_c_seed_b \gset
\endif

-- Encode payloads with no changes (expected empty)
\connect cloudsync_test_10_a
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_a_empty,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_a_empty_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_10_b
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_b_empty,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_b_empty_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_10_c
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_c_empty,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_c_empty_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

-- Apply empty payloads (should be no-ops)
\connect cloudsync_test_10_a
\if :payload_b_empty_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_empty', 3), 'hex')) AS _apply_a_empty_b \gset
\else
SELECT 0 AS _apply_a_empty_b \gset
\endif
\if :payload_c_empty_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_empty', 3), 'hex')) AS _apply_a_empty_c \gset
\else
SELECT 0 AS _apply_a_empty_c \gset
\endif

\connect cloudsync_test_10_b
\if :payload_a_empty_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_empty', 3), 'hex')) AS _apply_b_empty_a \gset
\else
SELECT 0 AS _apply_b_empty_a \gset
\endif
\if :payload_c_empty_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_empty', 3), 'hex')) AS _apply_b_empty_c \gset
\else
SELECT 0 AS _apply_b_empty_c \gset
\endif

\connect cloudsync_test_10_c
\if :payload_a_empty_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_empty', 3), 'hex')) AS _apply_c_empty_a \gset
\else
SELECT 0 AS _apply_c_empty_a \gset
\endif
\if :payload_b_empty_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_empty', 3), 'hex')) AS _apply_c_empty_b \gset
\else
SELECT 0 AS _apply_c_empty_b \gset
\endif

-- Final consistency check across all three databases
\connect cloudsync_test_10_a
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_a
FROM smoke_tbl \gset

\connect cloudsync_test_10_b
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_b
FROM smoke_tbl \gset

\connect cloudsync_test_10_c
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_c
FROM smoke_tbl \gset

SELECT (:'smoke_hash_a' = :'smoke_hash_b' AND :'smoke_hash_a' = :'smoke_hash_c') AS multi_db_roundtrip_ok \gset
\if :multi_db_roundtrip_ok
\echo [PASS] (:testid) Empty payload + no-op merge
\else
\echo [FAIL] (:testid) Empty payload + no-op merge
SELECT (:fail::int + 1) AS fail \gset
\endif


-- Cleanup: Drop test databases if not in DEBUG mode and no failures
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_10_a;
DROP DATABASE IF EXISTS cloudsync_test_10_b;
DROP DATABASE IF EXISTS cloudsync_test_10_c;
\endif