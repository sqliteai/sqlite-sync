-- '2 db roundtrip test'

\echo '\nRunning two-db roundtrip test ...'
\connect cloudsync_test_1
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

DROP DATABASE IF EXISTS cloudsync_test_2;
CREATE DATABASE cloudsync_test_2;
\connect cloudsync_test_2
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id_b \gset
SELECT cloudsync_payload_apply(decode(:'payload_hex', 'hex')) AS _apply_ok \gset
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash_b
FROM smoke_tbl \gset
SELECT (:'smoke_hash' = :'smoke_hash_b') AS payload_roundtrip_ok \gset
\if :payload_roundtrip_ok
\echo '[PASS] Test payload roundtrip to another database'
\else
\echo '[FAIL] Test payload roundtrip to another database'
SELECT (:fail::int + 1) AS fail \gset
\endif