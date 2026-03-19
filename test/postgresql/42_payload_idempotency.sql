-- Test payload apply idempotency
-- Applying the same payload multiple times must produce identical results.

\set testid '42'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_42_src;
DROP DATABASE IF EXISTS cloudsync_test_42_dst;
CREATE DATABASE cloudsync_test_42_src;
CREATE DATABASE cloudsync_test_42_dst;

-- Setup source with data
\connect cloudsync_test_42_src
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE test_tbl (id TEXT PRIMARY KEY, val TEXT, num INTEGER);
SELECT cloudsync_init('test_tbl', 'CLS', true) AS _init_src \gset
INSERT INTO test_tbl VALUES ('id1', 'hello', 10);
INSERT INTO test_tbl VALUES ('id2', 'world', 20);
UPDATE test_tbl SET val = 'hello_updated' WHERE id = 'id1';

-- Encode payload
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

-- Setup destination
\connect cloudsync_test_42_dst
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE test_tbl (id TEXT PRIMARY KEY, val TEXT, num INTEGER);
SELECT cloudsync_init('test_tbl', 'CLS', true) AS _init_dst \gset

-- Apply #1
SELECT cloudsync_payload_apply(decode(:'payload_hex', 'hex')) AS apply_1 \gset
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, '') || ':' || COALESCE(num::text, ''), ',' ORDER BY id), '')) AS hash_1
FROM test_tbl \gset
SELECT COUNT(*) AS count_1 FROM test_tbl \gset

-- Apply #2
SELECT cloudsync_payload_apply(decode(:'payload_hex', 'hex')) AS apply_2 \gset
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, '') || ':' || COALESCE(num::text, ''), ',' ORDER BY id), '')) AS hash_2
FROM test_tbl \gset
SELECT COUNT(*) AS count_2 FROM test_tbl \gset

-- Apply #3
SELECT cloudsync_payload_apply(decode(:'payload_hex', 'hex')) AS apply_3 \gset
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, '') || ':' || COALESCE(num::text, ''), ',' ORDER BY id), '')) AS hash_3
FROM test_tbl \gset
SELECT COUNT(*) AS count_3 FROM test_tbl \gset

-- Verify row count stays constant
SELECT (:count_1::int = :count_2::int AND :count_2::int = :count_3::int) AS count_stable \gset
\if :count_stable
\echo [PASS] (:testid) Row count stable across 3 applies (:count_1 rows)
\else
\echo [FAIL] (:testid) Row count changed: :count_1 -> :count_2 -> :count_3
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify data hash is identical after each apply
SELECT (:'hash_1' = :'hash_2' AND :'hash_2' = :'hash_3') AS hash_stable \gset
\if :hash_stable
\echo [PASS] (:testid) Data hash identical across 3 applies
\else
\echo [FAIL] (:testid) Data hash changed: :hash_1 -> :hash_2 -> :hash_3
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify data values are correct
SELECT COUNT(*) = 1 AS data_ok
FROM test_tbl
WHERE id = 'id1' AND val = 'hello_updated' AND num = 10 \gset
\if :data_ok
\echo [PASS] (:testid) Data values correct after idempotent applies
\else
\echo [FAIL] (:testid) Data values incorrect
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_42_src;
DROP DATABASE IF EXISTS cloudsync_test_42_dst;
\endif
