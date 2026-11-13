-- 'Schema hash with column settings and filters'
-- Tables with more than one settings row (a block column, a filter) keep the same schema hash
-- when it is computed again after a library version change, so peers still accept the payloads.

\set testid '78'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_78_a;
DROP DATABASE IF EXISTS cloudsync_test_78_b;
DROP DATABASE IF EXISTS cloudsync_test_78_peer;
DROP DATABASE IF EXISTS cloudsync_test_78_x;
CREATE DATABASE cloudsync_test_78_a;
CREATE DATABASE cloudsync_test_78_b;
CREATE DATABASE cloudsync_test_78_peer;
CREATE DATABASE cloudsync_test_78_x;

-- a: a block column and a filter; b: no column settings
\connect cloudsync_test_78_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE items (id TEXT PRIMARY KEY NOT NULL, name TEXT, qty INTEGER);
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('items') AS _init \gset
SELECT cloudsync_init('docs') AS _init \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol \gset
SELECT cloudsync_set_filter('items', 'qty > 0') AS _filter \gset
SELECT hash AS a_hash_before FROM cloudsync_schema_versions ORDER BY seq DESC LIMIT 1 \gset
UPDATE cloudsync_settings SET value = '0.0.1' WHERE key = 'version';

\connect cloudsync_test_78_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE items (id TEXT PRIMARY KEY NOT NULL, name TEXT, qty INTEGER);
SELECT cloudsync_init('items') AS _init \gset
SELECT hash AS b_hash_before FROM cloudsync_schema_versions ORDER BY seq DESC LIMIT 1 \gset
UPDATE cloudsync_settings SET value = '0.0.1' WHERE key = 'version';

-- New sessions: context load sees the old version and computes the hash again
\connect cloudsync_test_78_a
\ir helper_psql_conn_setup.sql
INSERT INTO items VALUES ('i1', 'one', 1);
INSERT INTO docs VALUES ('d1', E'a\nb');
SELECT hash AS a_hash_after FROM cloudsync_schema_versions ORDER BY seq DESC LIMIT 1 \gset
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS a_payload
FROM cloudsync_changes \gset

\connect cloudsync_test_78_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_db_version() AS _dbv \gset
SELECT hash AS b_hash_after FROM cloudsync_schema_versions ORDER BY seq DESC LIMIT 1 \gset

SELECT (:'a_hash_before' = :'a_hash_after' AND :'b_hash_before' = :'b_hash_after') AS hash_kept \gset
\if :hash_kept
\echo [PASS] (:testid) schema hash unchanged after a library version change
\else
\echo [FAIL] (:testid) schema hash unchanged after a library version change
SELECT (:fail::int + 1) AS fail \gset
\endif

-- A peer configured the same way accepts the payload
\connect cloudsync_test_78_peer
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE items (id TEXT PRIMARY KEY NOT NULL, name TEXT, qty INTEGER);
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('items') AS _init \gset
SELECT cloudsync_init('docs') AS _init \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol \gset
SELECT cloudsync_set_filter('items', 'qty > 0') AS _filter \gset
SELECT cloudsync_payload_apply(decode(:'a_payload', 'hex')) AS _applied \gset
SELECT (SELECT count(*) FROM items WHERE id = 'i1') = 1
       AND (SELECT body FROM docs WHERE id = 'd1') = E'a\nb' AS peer_applied \gset
\if :peer_applied
\echo [PASS] (:testid) a peer applies the payload after the hash is computed again
\else
\echo [FAIL] (:testid) a peer applies the payload after the hash is computed again
SELECT (:fail::int + 1) AS fail \gset
\endif

-- The same schema hashes to the same value on SQLite. The constant is the latest hash (signed,
-- as stored) in cloudsync_schema_versions after running, on the SQLite build:
--   CREATE TABLE hash_types (id TEXT PRIMARY KEY NOT NULL, n INTEGER, r REAL, b BLOB, d NUMERIC);
--   CREATE TABLE hash_docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
--   SELECT cloudsync_init('hash_types'); SELECT cloudsync_init('hash_docs');
--   SELECT cloudsync_set_column('hash_docs', 'body', 'algo', 'block');
\set sqlite_hash 2317239499380814180
\connect cloudsync_test_78_x
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE hash_types (id TEXT PRIMARY KEY NOT NULL, n INTEGER, r DOUBLE PRECISION, b BYTEA, d NUMERIC);
CREATE TABLE hash_docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('hash_types') AS _init \gset
SELECT cloudsync_init('hash_docs') AS _init \gset
SELECT cloudsync_set_column('hash_docs', 'body', 'algo', 'block') AS _setcol \gset
UPDATE cloudsync_settings SET value = '0.0.1' WHERE key = 'version';
\connect cloudsync_test_78_x
\ir helper_psql_conn_setup.sql
SELECT cloudsync_db_version() AS _dbv \gset
SELECT hash::text AS x_hash
FROM cloudsync_schema_versions ORDER BY seq DESC LIMIT 1 \gset
SELECT (:'x_hash' = :'sqlite_hash') AS cross_engine_ok \gset
\if :cross_engine_ok
\echo [PASS] (:testid) schema hash matches the SQLite value for the same schema
\else
\echo [FAIL] (:testid) schema hash matches the SQLite value for the same schema (got :x_hash, expected :sqlite_hash)
SELECT (:fail::int + 1) AS fail \gset
\endif

\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_78_a;
DROP DATABASE IF EXISTS cloudsync_test_78_b;
DROP DATABASE IF EXISTS cloudsync_test_78_peer;
DROP DATABASE IF EXISTS cloudsync_test_78_x;
\endif
