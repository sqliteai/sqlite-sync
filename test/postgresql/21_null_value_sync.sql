-- Test: NULL Value Sync Parameter Binding
-- This test verifies that syncing NULL values works correctly in all scenarios:
-- 1. Insert with NULL value first, then non-NULL
-- 2. Update existing row to NULL
--
-- ISSUE: When a NULL value is synced first, PostgreSQL SPI prepares a statement with
-- only the PK parameters. Subsequent non-NULL syncs fail with "there is no parameter $3".
--
-- The test uses payload_encode/payload_apply to simulate cross-database sync.

\set testid '21'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

-- Cleanup and create test databases
DROP DATABASE IF EXISTS cloudsync_test_21a;
DROP DATABASE IF EXISTS cloudsync_test_21b;
CREATE DATABASE cloudsync_test_21a;
CREATE DATABASE cloudsync_test_21b;

-- ============================================================================
-- Setup Database A - Source database
-- ============================================================================

\connect cloudsync_test_21a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- Create a simple table with a nullable column
CREATE TABLE test_null_sync (
    id TEXT NOT NULL PRIMARY KEY,
    value TEXT  -- Nullable column
);

-- Initialize CloudSync
SELECT cloudsync_init('test_null_sync', 'CLS', true) AS _init_a \gset

-- ============================================================================
-- Setup Database B - Target database (same schema)
-- ============================================================================

\connect cloudsync_test_21b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE test_null_sync (
    id TEXT NOT NULL PRIMARY KEY,
    value TEXT
);

SELECT cloudsync_init('test_null_sync', 'CLS', true) AS _init_b \gset

-- ============================================================================
-- Test 1: Insert NULL value first, then sync to B
-- ============================================================================

\connect cloudsync_test_21a

-- Insert row with NULL value
INSERT INTO test_null_sync (id, value) VALUES ('row1', NULL);

-- Encode payload from Database A
SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_null_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_test_21b

-- Apply payload with NULL value
SELECT cloudsync_payload_apply(decode(:'payload_null_hex', 'hex')) AS apply_null_result \gset

SELECT (:apply_null_result >= 0) AS null_applied \gset
\if :null_applied
\echo [PASS] (:testid) NULL value payload applied successfully
\else
\echo [FAIL] (:testid) NULL value payload failed to apply
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify the NULL value was synced
SELECT COUNT(*) = 1 AS null_row_exists FROM test_null_sync WHERE id = 'row1' AND value IS NULL \gset
\if :null_row_exists
\echo [PASS] (:testid) NULL value synced correctly
\else
\echo [FAIL] (:testid) NULL value not synced correctly
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test 2: Insert non-NULL value, then sync to B
-- ============================================================================

\connect cloudsync_test_21a

-- Insert row with non-NULL value
INSERT INTO test_null_sync (id, value) VALUES ('row2', 'hello world');

-- Encode payload from Database A (includes new row)
SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_nonnull_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_test_21b

-- Apply payload with non-NULL value
SELECT cloudsync_payload_apply(decode(:'payload_nonnull_hex', 'hex')) AS apply_nonnull_result \gset

SELECT (:apply_nonnull_result >= 0) AS nonnull_applied \gset
\if :nonnull_applied
\echo [PASS] (:testid) Non-NULL value payload applied successfully after NULL
\else
\echo [FAIL] (:testid) Non-NULL value payload failed to apply (parameter binding issue)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify the non-NULL value was synced
SELECT COUNT(*) = 1 AS nonnull_row_exists FROM test_null_sync WHERE id = 'row2' AND value = 'hello world' \gset
\if :nonnull_row_exists
\echo [PASS] (:testid) Non-NULL value synced correctly
\else
\echo [FAIL] (:testid) Non-NULL value not synced correctly
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test 3: Verify both rows exist in Database B
-- ============================================================================

SELECT COUNT(*) AS total_rows FROM test_null_sync \gset
SELECT (:total_rows = 2) AS both_rows_exist \gset
\if :both_rows_exist
\echo [PASS] (:testid) Both rows synced successfully
\else
\echo [FAIL] (:testid) Expected 2 rows, found :total_rows
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test 4: Update existing row to NULL, then sync to B
-- This tests that updating a column from non-NULL to NULL works correctly.
-- ============================================================================

\connect cloudsync_test_21a

-- Update row2 to set value to NULL
UPDATE test_null_sync SET value = NULL WHERE id = 'row2';

-- Encode payload from Database A
SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_update_null_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_test_21b

-- Apply payload with updated NULL value
SELECT cloudsync_payload_apply(decode(:'payload_update_null_hex', 'hex')) AS apply_update_null_result \gset

SELECT (:apply_update_null_result >= 0) AS update_null_applied \gset
\if :update_null_applied
\echo [PASS] (:testid) Update to NULL payload applied successfully
\else
\echo [FAIL] (:testid) Update to NULL payload failed to apply
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify the update to NULL was synced
SELECT COUNT(*) = 1 AS update_null_synced FROM test_null_sync WHERE id = 'row2' AND value IS NULL \gset
\if :update_null_synced
\echo [PASS] (:testid) Update to NULL synced correctly
\else
\echo [FAIL] (:testid) Update to NULL not synced correctly
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Cleanup
-- ============================================================================

\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_21a;
DROP DATABASE IF EXISTS cloudsync_test_21b;
\endif
