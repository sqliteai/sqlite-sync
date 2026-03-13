-- Test: NULL Primary Key Insert Rejection
-- Verifies that inserting a NULL primary key into a cloudsync-enabled table fails
-- and that the metatable only contains rows for valid inserts.

\set testid '30'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

-- Cleanup and create test database
DROP DATABASE IF EXISTS cloudsync_test_30;
CREATE DATABASE cloudsync_test_30;

\connect cloudsync_test_30
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- Create table with primary key and init cloudsync
CREATE TABLE t_null_pk (
    id TEXT NOT NULL PRIMARY KEY,
    value TEXT
);

SELECT cloudsync_init('t_null_pk', 'CLS', true) AS _init \gset

-- Test 1: INSERT with NULL primary key should fail
DO $$
BEGIN
    INSERT INTO t_null_pk (id, value) VALUES (NULL, 'test');
    RAISE EXCEPTION 'INSERT with NULL PK should have failed';
EXCEPTION WHEN not_null_violation THEN
    -- Expected
END $$;

SELECT (COUNT(*) = 0) AS null_pk_rejected FROM t_null_pk \gset
\if :null_pk_rejected
\echo [PASS] (:testid) NULL PK insert rejected
\else
\echo [FAIL] (:testid) NULL PK insert was not rejected
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 2: INSERT with valid (non-NULL) primary key should succeed
INSERT INTO t_null_pk (id, value) VALUES ('valid_id', 'test');

SELECT (COUNT(*) = 1) AS valid_insert_ok FROM t_null_pk WHERE id = 'valid_id' \gset
\if :valid_insert_ok
\echo [PASS] (:testid) Valid PK insert succeeded
\else
\echo [FAIL] (:testid) Valid PK insert failed
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 3: Metatable should have exactly 1 row (from the valid insert only)
SELECT (COUNT(*) = 1) AS meta_row_ok FROM t_null_pk_cloudsync \gset
\if :meta_row_ok
\echo [PASS] (:testid) Metatable has exactly 1 row
\else
\echo [FAIL] (:testid) Metatable row count mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_30;
\endif
