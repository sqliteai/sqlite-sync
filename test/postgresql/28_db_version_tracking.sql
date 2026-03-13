-- Test db_version/seq tracking in cloudsync_changes after payload apply
-- PostgreSQL equivalent of SQLite unit tests:
--   "Merge Test db_version 1" (do_test_merge_check_db_version)
--   "Merge Test db_version 2" (do_test_merge_check_db_version_2)

\set testid '28'
\ir helper_test_init.sql

-- ============================================================
-- Setup: create databases A and B with the todo table
-- ============================================================
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_28_a;
DROP DATABASE IF EXISTS cloudsync_test_28_b;
CREATE DATABASE cloudsync_test_28_a;
CREATE DATABASE cloudsync_test_28_b;

\connect cloudsync_test_28_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE todo (id TEXT PRIMARY KEY NOT NULL, title TEXT, status TEXT);
SELECT cloudsync_init('todo', 'CLS', true) AS _init_a \gset

\connect cloudsync_test_28_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE todo (id TEXT PRIMARY KEY NOT NULL, title TEXT, status TEXT);
SELECT cloudsync_init('todo', 'CLS', true) AS _init_b \gset

-- ============================================================
-- Test 1: One-way merge (A -> B), mixed insert patterns
-- Mirrors do_test_merge_check_db_version from test/unit.c
-- ============================================================

\connect cloudsync_test_28_a
\ir helper_psql_conn_setup.sql

-- Autocommit insert (db_version 1)
INSERT INTO todo VALUES ('ID1', 'Buy groceries', 'in_progress1');

-- Multi-row insert (db_version 2 — single statement)
INSERT INTO todo VALUES ('ID2', 'Buy bananas', 'in_progress2'), ('ID3', 'Buy vegetables', 'in_progress3');

-- Autocommit insert (db_version 3)
INSERT INTO todo VALUES ('ID4', 'Buy apples', 'in_progress4');

-- Transaction with 3 inserts (db_version 4 — one transaction)
BEGIN;
INSERT INTO todo VALUES ('ID5', 'Buy oranges', 'in_progress5');
INSERT INTO todo VALUES ('ID6', 'Buy lemons', 'in_progress6');
INSERT INTO todo VALUES ('ID7', 'Buy pizza', 'in_progress7');
COMMIT;

-- Encode payload
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_a_t1,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_a_t1_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

-- Apply to B
\connect cloudsync_test_28_b
\ir helper_psql_conn_setup.sql
\if :payload_a_t1_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_t1', 3), 'hex')) AS _apply_t1 \gset
\endif

-- Verify data matches
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(title, '') || ':' || COALESCE(status, ''), ',' ORDER BY id), '')) AS hash_b_t1
FROM todo \gset

\connect cloudsync_test_28_a
\ir helper_psql_conn_setup.sql
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(title, '') || ':' || COALESCE(status, ''), ',' ORDER BY id), '')) AS hash_a_t1
FROM todo \gset

SELECT (:'hash_a_t1' = :'hash_b_t1') AS t1_data_ok \gset
\if :t1_data_ok
\echo [PASS] (:testid) db_version test 1: data roundtrip matches
\else
\echo [FAIL] (:testid) db_version test 1: data roundtrip mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify no repeated (db_version, seq) tuples on B
\connect cloudsync_test_28_b
\ir helper_psql_conn_setup.sql
SELECT COUNT(*) AS dup_count_b_t1
FROM (
  SELECT db_version, seq, COUNT(*) AS cnt
  FROM cloudsync_changes
  GROUP BY db_version, seq
  HAVING COUNT(*) > 1
) AS dups \gset

SELECT (:dup_count_b_t1::int = 0) AS t1_no_dups_b \gset
\if :t1_no_dups_b
\echo [PASS] (:testid) db_version test 1: no duplicate (db_version, seq) on B
\else
\echo [FAIL] (:testid) db_version test 1: duplicate (db_version, seq) on B
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify row count
SELECT COUNT(*) AS row_count_b_t1 FROM todo \gset
SELECT (:row_count_b_t1::int = 7) AS t1_count_ok \gset
\if :t1_count_ok
\echo [PASS] (:testid) db_version test 1: row count correct (7)
\else
\echo [FAIL] (:testid) db_version test 1: expected 7 rows, got :row_count_b_t1
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 2: Bidirectional merge (A -> B, B -> A), mixed patterns
-- Mirrors do_test_merge_check_db_version_2 from test/unit.c
-- ============================================================

-- Reset: drop and recreate databases
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_28_a;
DROP DATABASE IF EXISTS cloudsync_test_28_b;
CREATE DATABASE cloudsync_test_28_a;
CREATE DATABASE cloudsync_test_28_b;

\connect cloudsync_test_28_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE todo (id TEXT PRIMARY KEY NOT NULL, title TEXT, status TEXT);
SELECT cloudsync_init('todo', 'CLS', true) AS _init_a2 \gset

\connect cloudsync_test_28_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE todo (id TEXT PRIMARY KEY NOT NULL, title TEXT, status TEXT);
SELECT cloudsync_init('todo', 'CLS', true) AS _init_b2 \gset

-- DB A: two autocommit inserts (db_version 1, 2)
\connect cloudsync_test_28_a
\ir helper_psql_conn_setup.sql
INSERT INTO todo VALUES ('ID1', 'Buy groceries', 'in_progress');
INSERT INTO todo VALUES ('ID2', 'Foo', 'Bar');

-- DB B: two autocommit inserts + one transaction with 2 inserts
\connect cloudsync_test_28_b
\ir helper_psql_conn_setup.sql
INSERT INTO todo VALUES ('ID3', 'Foo3', 'Bar3');
INSERT INTO todo VALUES ('ID4', 'Foo4', 'Bar4');
BEGIN;
INSERT INTO todo VALUES ('ID5', 'Foo5', 'Bar5');
INSERT INTO todo VALUES ('ID6', 'Foo6', 'Bar6');
COMMIT;

-- Encode A's payload
\connect cloudsync_test_28_a
\ir helper_psql_conn_setup.sql
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_a_t2,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_a_t2_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

-- Encode B's payload
\connect cloudsync_test_28_b
\ir helper_psql_conn_setup.sql
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_b_t2,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_b_t2_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

-- Apply A -> B
\connect cloudsync_test_28_b
\ir helper_psql_conn_setup.sql
\if :payload_a_t2_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_t2', 3), 'hex')) AS _apply_a_to_b \gset
\endif

-- Apply B -> A
\connect cloudsync_test_28_a
\ir helper_psql_conn_setup.sql
\if :payload_b_t2_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_t2', 3), 'hex')) AS _apply_b_to_a \gset
\endif

-- Verify data matches between A and B
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(title, '') || ':' || COALESCE(status, ''), ',' ORDER BY id), '')) AS hash_a_t2
FROM todo \gset

\connect cloudsync_test_28_b
\ir helper_psql_conn_setup.sql
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(title, '') || ':' || COALESCE(status, ''), ',' ORDER BY id), '')) AS hash_b_t2
FROM todo \gset

SELECT (:'hash_a_t2' = :'hash_b_t2') AS t2_data_ok \gset
\if :t2_data_ok
\echo [PASS] (:testid) db_version test 2: bidirectional data matches
\else
\echo [FAIL] (:testid) db_version test 2: bidirectional data mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify row count (6 rows: ID1-ID6)
SELECT COUNT(*) AS row_count_t2 FROM todo \gset
SELECT (:row_count_t2::int = 6) AS t2_count_ok \gset
\if :t2_count_ok
\echo [PASS] (:testid) db_version test 2: row count correct (6)
\else
\echo [FAIL] (:testid) db_version test 2: expected 6 rows, got :row_count_t2
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify no repeated (db_version, seq) tuples on A
\connect cloudsync_test_28_a
\ir helper_psql_conn_setup.sql
SELECT COUNT(*) AS dup_count_a_t2
FROM (
  SELECT db_version, seq, COUNT(*) AS cnt
  FROM cloudsync_changes
  GROUP BY db_version, seq
  HAVING COUNT(*) > 1
) AS dups \gset

SELECT (:dup_count_a_t2::int = 0) AS t2_no_dups_a \gset
\if :t2_no_dups_a
\echo [PASS] (:testid) db_version test 2: no duplicate (db_version, seq) on A
\else
\echo [FAIL] (:testid) db_version test 2: duplicate (db_version, seq) on A
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify no repeated (db_version, seq) tuples on B
\connect cloudsync_test_28_b
\ir helper_psql_conn_setup.sql
SELECT COUNT(*) AS dup_count_b_t2
FROM (
  SELECT db_version, seq, COUNT(*) AS cnt
  FROM cloudsync_changes
  GROUP BY db_version, seq
  HAVING COUNT(*) > 1
) AS dups \gset

SELECT (:dup_count_b_t2::int = 0) AS t2_no_dups_b \gset
\if :t2_no_dups_b
\echo [PASS] (:testid) db_version test 2: no duplicate (db_version, seq) on B
\else
\echo [FAIL] (:testid) db_version test 2: duplicate (db_version, seq) on B
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Cleanup
-- ============================================================
\ir helper_test_cleanup.sql
\if :should_cleanup
-- DROP DATABASE IF EXISTS cloudsync_test_28_a;
-- DROP DATABASE IF EXISTS cloudsync_test_28_b;
\endif
