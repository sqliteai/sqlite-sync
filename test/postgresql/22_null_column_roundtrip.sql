-- Test: NULL Column Roundtrip
-- This test verifies that syncing rows with various NULL column combinations works correctly.
-- Tests all permutations: NULL in first column, second column, both, and neither.

\set testid '22'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

-- Cleanup and create test databases
DROP DATABASE IF EXISTS cloudsync_test_22a;
DROP DATABASE IF EXISTS cloudsync_test_22b;
CREATE DATABASE cloudsync_test_22a;
CREATE DATABASE cloudsync_test_22b;

-- ============================================================================
-- Setup Database A - Source database
-- ============================================================================

\connect cloudsync_test_22a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- Create table with nullable columns (no DEFAULT values)
CREATE TABLE null_sync_test (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT,
    value INTEGER
);

-- Initialize CloudSync
SELECT cloudsync_init('null_sync_test', 'CLS', true) AS _init_a \gset

-- ============================================================================
-- Insert test data with various NULL combinations
-- ============================================================================

-- Row 1: NULL in value column only
INSERT INTO null_sync_test (id, name, value) VALUES ('pg1', 'name1', NULL);

-- Row 2: NULL in name column only
INSERT INTO null_sync_test (id, name, value) VALUES ('pg2', NULL, 42);

-- Row 3: NULL in both columns
INSERT INTO null_sync_test (id, name, value) VALUES ('pg3', NULL, NULL);

-- Row 4: No NULLs (both columns have values)
INSERT INTO null_sync_test (id, name, value) VALUES ('pg4', 'name4', 100);

-- Row 5: Empty string (not NULL) and zero
INSERT INTO null_sync_test (id, name, value) VALUES ('pg5', '', 0);

-- Row 6: Another NULL in value
INSERT INTO null_sync_test (id, name, value) VALUES ('pg6', 'name6', NULL);

-- ============================================================================
-- Verify source data
-- ============================================================================

SELECT COUNT(*) = 6 AS source_row_count_ok FROM null_sync_test \gset
\if :source_row_count_ok
\echo [PASS] (:testid) Source database has 6 rows
\else
\echo [FAIL] (:testid) Source database row count incorrect
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Compute hash of Database A data
-- ============================================================================

SELECT md5(
    COALESCE(
        string_agg(
            id || ':' ||
            COALESCE(name, 'NULL') || ':' ||
            COALESCE(value::text, 'NULL'),
            '|' ORDER BY id
        ),
        ''
    )
) AS hash_a FROM null_sync_test \gset

\echo [INFO] (:testid) Database A hash: :hash_a

-- ============================================================================
-- Encode payload from Database A
-- ============================================================================

SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_a_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

-- Verify payload was created
SELECT (length(:'payload_a_hex') > 0) AS payload_created \gset
\if :payload_created
\echo [PASS] (:testid) Payload encoded from Database A
\else
\echo [FAIL] (:testid) Payload encoded from Database A - Empty payload
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Setup Database B with same schema
-- ============================================================================

\connect cloudsync_test_22b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- Create identical table schema
CREATE TABLE null_sync_test (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT,
    value INTEGER
);

-- Initialize CloudSync
SELECT cloudsync_init('null_sync_test', 'CLS', true) AS _init_b \gset

-- ============================================================================
-- Apply payload to Database B
-- ============================================================================

SELECT cloudsync_payload_apply(decode(:'payload_a_hex', 'hex')) AS apply_result \gset

-- Verify application succeeded
SELECT (:apply_result >= 0) AS payload_applied \gset
\if :payload_applied
\echo [PASS] (:testid) Payload applied to Database B (result: :apply_result)
\else
\echo [FAIL] (:testid) Payload applied to Database B - Apply returned :apply_result
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Verify data integrity after roundtrip
-- ============================================================================

-- Compute hash of Database B data (should match Database A)
SELECT md5(
    COALESCE(
        string_agg(
            id || ':' ||
            COALESCE(name, 'NULL') || ':' ||
            COALESCE(value::text, 'NULL'),
            '|' ORDER BY id
        ),
        ''
    )
) AS hash_b FROM null_sync_test \gset

\echo [INFO] (:testid) Database B hash: :hash_b

-- Compare hashes
SELECT (:'hash_a' = :'hash_b') AS hashes_match \gset
\if :hashes_match
\echo [PASS] (:testid) Data integrity verified - hashes match
\else
\echo [FAIL] (:testid) Data integrity check failed - Database A hash: :hash_a, Database B hash: :hash_b
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Verify row count
-- ============================================================================

SELECT COUNT(*) AS count_b FROM null_sync_test \gset
SELECT (:count_b = 6) AS row_counts_match \gset
\if :row_counts_match
\echo [PASS] (:testid) Row counts match (6 rows)
\else
\echo [FAIL] (:testid) Row counts mismatch - Expected 6, got :count_b
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Verify specific NULL patterns
-- ============================================================================

-- pg1: name='name1', value=NULL
SELECT (SELECT name = 'name1' AND value IS NULL FROM null_sync_test WHERE id = 'pg1') AS pg1_ok \gset
\if :pg1_ok
\echo [PASS] (:testid) pg1: name='name1', value=NULL preserved
\else
\echo [FAIL] (:testid) pg1: NULL in value column not preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- pg2: name=NULL, value=42
SELECT (SELECT name IS NULL AND value = 42 FROM null_sync_test WHERE id = 'pg2') AS pg2_ok \gset
\if :pg2_ok
\echo [PASS] (:testid) pg2: name=NULL, value=42 preserved
\else
\echo [FAIL] (:testid) pg2: NULL in name column not preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- pg3: name=NULL, value=NULL
SELECT (SELECT name IS NULL AND value IS NULL FROM null_sync_test WHERE id = 'pg3') AS pg3_ok \gset
\if :pg3_ok
\echo [PASS] (:testid) pg3: name=NULL, value=NULL preserved
\else
\echo [FAIL] (:testid) pg3: Both NULLs not preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- pg4: name='name4', value=100 (no NULLs)
SELECT (SELECT name = 'name4' AND value = 100 FROM null_sync_test WHERE id = 'pg4') AS pg4_ok \gset
\if :pg4_ok
\echo [PASS] (:testid) pg4: name='name4', value=100 preserved
\else
\echo [FAIL] (:testid) pg4: Non-NULL values not preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- pg5: name='', value=0 (empty string and zero, not NULL)
SELECT (SELECT name = '' AND value = 0 FROM null_sync_test WHERE id = 'pg5') AS pg5_ok \gset
\if :pg5_ok
\echo [PASS] (:testid) pg5: empty string and zero preserved (not NULL)
\else
\echo [FAIL] (:testid) pg5: Empty string or zero incorrectly converted
SELECT (:fail::int + 1) AS fail \gset
\endif

-- pg6: name='name6', value=NULL
SELECT (SELECT name = 'name6' AND value IS NULL FROM null_sync_test WHERE id = 'pg6') AS pg6_ok \gset
\if :pg6_ok
\echo [PASS] (:testid) pg6: name='name6', value=NULL preserved
\else
\echo [FAIL] (:testid) pg6: NULL in value column not preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test bidirectional sync (B -> A)
-- ============================================================================

-- Add a new row in Database B with NULLs
INSERT INTO null_sync_test (id, name, value) VALUES ('pgB1', NULL, 999);

-- Encode payload from Database B
SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_b_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

-- Apply to Database A
\connect cloudsync_test_22a
SELECT cloudsync_payload_apply(decode(:'payload_b_hex', 'hex')) AS apply_b_to_a \gset

-- Verify the new row exists in Database A with correct NULL
SELECT (SELECT name IS NULL AND value = 999 FROM null_sync_test WHERE id = 'pgB1') AS bidirectional_ok \gset
\if :bidirectional_ok
\echo [PASS] (:testid) Bidirectional sync works (B to A with NULL)
\else
\echo [FAIL] (:testid) Bidirectional sync failed
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test UPDATE to NULL
-- ============================================================================

-- Update pg4 to set name to NULL
UPDATE null_sync_test SET name = NULL WHERE id = 'pg4';

-- Encode and sync to B
SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_update_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_test_22b
SELECT cloudsync_payload_apply(decode(:'payload_update_hex', 'hex')) AS apply_update \gset

-- Verify pg4 now has NULL name
SELECT (SELECT name IS NULL AND value = 100 FROM null_sync_test WHERE id = 'pg4') AS update_to_null_ok \gset
\if :update_to_null_ok
\echo [PASS] (:testid) UPDATE to NULL synced correctly
\else
\echo [FAIL] (:testid) UPDATE to NULL failed
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test UPDATE from NULL to value
-- ============================================================================

-- Update pg3 to set both columns to non-NULL values
\connect cloudsync_test_22a
UPDATE null_sync_test SET name = 'updated', value = 123 WHERE id = 'pg3';

-- Encode and sync to B
SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_update2_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_test_22b
SELECT cloudsync_payload_apply(decode(:'payload_update2_hex', 'hex')) AS apply_update2 \gset

-- Verify pg3 now has non-NULL values
SELECT (SELECT name = 'updated' AND value = 123 FROM null_sync_test WHERE id = 'pg3') AS update_from_null_ok \gset
\if :update_from_null_ok
\echo [PASS] (:testid) UPDATE from NULL to value synced correctly
\else
\echo [FAIL] (:testid) UPDATE from NULL to value failed
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Final verification - both databases should have 7 rows with matching content
-- ============================================================================

SELECT COUNT(*) AS final_count_b FROM null_sync_test \gset
\connect cloudsync_test_22a
SELECT COUNT(*) AS final_count_a FROM null_sync_test \gset

\connect cloudsync_test_22b
SELECT (:final_count_a = 7 AND :final_count_b = 7) AS final_counts_ok \gset
\if :final_counts_ok
\echo [PASS] (:testid) Final row counts correct (7 rows each)
\else
\echo [FAIL] (:testid) Final row counts incorrect - A: :final_count_a, B: :final_count_b
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Cleanup
-- ============================================================================

\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_22a;
DROP DATABASE IF EXISTS cloudsync_test_22b;
\endif
