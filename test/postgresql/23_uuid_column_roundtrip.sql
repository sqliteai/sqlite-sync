-- Test: UUID Column Roundtrip
-- This test verifies that syncing rows with UUID columns (not as PK) works correctly.
-- Tests various combinations of NULL and non-NULL UUID values alongside other nullable columns.
--
-- IMPORTANT: This test is structured to isolate whether NULL UUID values trigger encoding bugs:
-- Step 1: Sync a single row with non-NULL UUID only (baseline)
-- Step 2: Sync a row with NULL UUID, then a row with non-NULL UUID (test NULL trigger)
-- Step 3: Sync remaining rows with mixed NULL/non-NULL UUIDs

\set testid '23'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

-- Cleanup and create test databases
DROP DATABASE IF EXISTS cloudsync_test_23a;
DROP DATABASE IF EXISTS cloudsync_test_23b;
CREATE DATABASE cloudsync_test_23a;
CREATE DATABASE cloudsync_test_23b;

-- ============================================================================
-- Setup Database A - Source database
-- ============================================================================

\connect cloudsync_test_23a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- Create table with UUID column and other nullable columns
CREATE TABLE uuid_sync_test (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT,
    value INTEGER,
    id2 UUID
);

-- Initialize CloudSync
SELECT cloudsync_init('uuid_sync_test', 'CLS', true) AS _init_a \gset

-- ============================================================================
-- Setup Database B with same schema (before any inserts)
-- ============================================================================

\connect cloudsync_test_23b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE uuid_sync_test (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT,
    value INTEGER,
    id2 UUID
);

SELECT cloudsync_init('uuid_sync_test', 'CLS', true) AS _init_b \gset

-- ============================================================================
-- STEP 1: Sync a single row with non-NULL UUID only (baseline test)
-- ============================================================================

\echo [INFO] (:testid) === STEP 1: Single row with non-NULL UUID ===

\connect cloudsync_test_23a

-- Insert only one row with a non-NULL UUID
INSERT INTO uuid_sync_test (id, name, value, id2) VALUES ('step1', 'baseline', 1, 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa');

-- Encode payload
SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_step1_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

SELECT max(db_version) AS db_version FROM uuid_sync_test_cloudsync \gset

-- Apply to Database B
\connect cloudsync_test_23b
SELECT cloudsync_payload_apply(decode(:'payload_step1_hex', 'hex')) AS apply_step1 \gset

-- Verify step 1
SELECT (SELECT id2 = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa' FROM uuid_sync_test WHERE id = 'step1') AS step1_ok \gset
\if :step1_ok
\echo [PASS] (:testid) Step 1: Single non-NULL UUID preserved correctly
\else
\echo [FAIL] (:testid) Step 1: Single non-NULL UUID NOT preserved
SELECT (:fail::int + 1) AS fail \gset
SELECT id, name, value, id2::text FROM uuid_sync_test WHERE id = 'step1';
\endif

-- ============================================================================
-- STEP 2: Sync NULL UUID row, then non-NULL UUID row (test NULL trigger)
-- ============================================================================

\echo [INFO] (:testid) === STEP 2: NULL UUID followed by non-NULL UUID ===

\connect cloudsync_test_23a

-- Insert a row with NULL UUID first
INSERT INTO uuid_sync_test (id, name, value, id2) VALUES ('step2a', 'null_uuid', 2, NULL);

-- Then insert a row with non-NULL UUID
INSERT INTO uuid_sync_test (id, name, value, id2) VALUES ('step2b', 'after_null', 3, 'bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb');

-- Encode payload (should contain both rows)
SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_step2_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() AND db_version > :db_version \gset

SELECT max(db_version) AS db_version FROM uuid_sync_test_cloudsync \gset

-- Apply to Database B
\connect cloudsync_test_23b
SELECT cloudsync_payload_apply(decode(:'payload_step2_hex', 'hex')) AS apply_step2 \gset

-- Verify step 2a (NULL UUID)
SELECT (SELECT id2 IS NULL FROM uuid_sync_test WHERE id = 'step2a') AS step2a_ok \gset
\if :step2a_ok
\echo [PASS] (:testid) Step 2a: NULL UUID preserved correctly
\else
\echo [FAIL] (:testid) Step 2a: NULL UUID NOT preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify step 2b (non-NULL UUID after NULL)
SELECT (SELECT id2 = 'bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb' FROM uuid_sync_test WHERE id = 'step2b') AS step2b_ok \gset
\if :step2b_ok
\echo [PASS] (:testid) Step 2b: Non-NULL UUID after NULL preserved correctly
\else
\echo [FAIL] (:testid) Step 2b: Non-NULL UUID after NULL NOT preserved (NULL may have triggered bug)
SELECT (:fail::int + 1) AS fail \gset
SELECT id, name, value, id2::text FROM uuid_sync_test WHERE id = 'step2b';
\endif

-- ============================================================================
-- STEP 3: Sync remaining rows with mixed NULL/non-NULL UUIDs
-- ============================================================================

\echo [INFO] (:testid) === STEP 3: Mixed NULL/non-NULL UUIDs ===

\connect cloudsync_test_23a

-- Row with NULL in value and id2
INSERT INTO uuid_sync_test (id, name, value, id2) VALUES ('pg1', 'name1', NULL, NULL);

-- Row with NULL in name, has UUID
INSERT INTO uuid_sync_test (id, name, value, id2) VALUES ('pg2', NULL, 42, 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11');

-- Row with all nullable columns NULL
INSERT INTO uuid_sync_test (id, name, value, id2) VALUES ('pg3', NULL, NULL, NULL);

-- Row with no NULLs - all columns have values
INSERT INTO uuid_sync_test (id, name, value, id2) VALUES ('pg4', 'name4', 100, 'b0eebc99-9c0b-4ef8-bb6d-6bb9bd380a22');

-- Row with only id2 NULL
INSERT INTO uuid_sync_test (id, name, value, id2) VALUES ('pg5', 'name5', 55, NULL);

-- Row with only name NULL, has different UUID
INSERT INTO uuid_sync_test (id, name, value, id2) VALUES ('pg6', NULL, 66, 'c0eebc99-9c0b-4ef8-bb6d-6bb9bd380a33');

-- ============================================================================
-- Verify source data
-- ============================================================================

SELECT COUNT(*) = 9 AS source_row_count_ok FROM uuid_sync_test \gset
\if :source_row_count_ok
\echo [PASS] (:testid) Source database has 9 rows
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
            COALESCE(value::text, 'NULL') || ':' ||
            COALESCE(id2::text, 'NULL'),
            '|' ORDER BY id
        ),
        ''
    )
) AS hash_a FROM uuid_sync_test \gset

\echo [INFO] (:testid) Database A hash: :hash_a

-- ============================================================================
-- Encode payload from Database A (step 3 rows only)
-- ============================================================================

SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_step3_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() AND db_version > :db_version \gset

SELECT max(db_version) AS db_version FROM uuid_sync_test_cloudsync \gset

-- Verify payload was created
SELECT (length(:'payload_step3_hex') > 0) AS payload_created \gset
\if :payload_created
\echo [PASS] (:testid) Payload encoded from Database A
\else
\echo [FAIL] (:testid) Payload encoded from Database A - Empty payload
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Apply payload to Database B
-- ============================================================================

\connect cloudsync_test_23b

SELECT cloudsync_payload_apply(decode(:'payload_step3_hex', 'hex')) AS apply_result \gset

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
            COALESCE(value::text, 'NULL') || ':' ||
            COALESCE(id2::text, 'NULL'),
            '|' ORDER BY id
        ),
        ''
    )
) AS hash_b FROM uuid_sync_test \gset

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

SELECT COUNT(*) AS count_b FROM uuid_sync_test \gset
SELECT (:count_b = 9) AS row_counts_match \gset
\if :row_counts_match
\echo [PASS] (:testid) Row counts match (9 rows)
\else
\echo [FAIL] (:testid) Row counts mismatch - Expected 9, got :count_b
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Verify specific UUID and NULL patterns
-- ============================================================================

-- pg1: name='name1', value=NULL, id2=NULL
SELECT (SELECT name = 'name1' AND value IS NULL AND id2 IS NULL FROM uuid_sync_test WHERE id = 'pg1') AS pg1_ok \gset
\if :pg1_ok
\echo [PASS] (:testid) pg1: name='name1', value=NULL, id2=NULL preserved
\else
\echo [FAIL] (:testid) pg1: NULL values not preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- pg2: name=NULL, value=42, id2='a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'
SELECT (SELECT name IS NULL AND value = 42 AND id2 = 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11' FROM uuid_sync_test WHERE id = 'pg2') AS pg2_ok \gset
\if :pg2_ok
\echo [PASS] (:testid) pg2: name=NULL, value=42, UUID preserved
\else
\echo [FAIL] (:testid) pg2: UUID or NULL not preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- pg3: all nullable columns NULL
SELECT (SELECT name IS NULL AND value IS NULL AND id2 IS NULL FROM uuid_sync_test WHERE id = 'pg3') AS pg3_ok \gset
\if :pg3_ok
\echo [PASS] (:testid) pg3: all NULLs preserved
\else
\echo [FAIL] (:testid) pg3: all NULLs not preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- pg4: no NULLs, id2='b0eebc99-9c0b-4ef8-bb6d-6bb9bd380a22'
SELECT (SELECT name = 'name4' AND value = 100 AND id2 = 'b0eebc99-9c0b-4ef8-bb6d-6bb9bd380a22' FROM uuid_sync_test WHERE id = 'pg4') AS pg4_ok \gset
\if :pg4_ok
\echo [PASS] (:testid) pg4: all values including UUID preserved
\else
\echo [FAIL] (:testid) pg4: values not preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- pg5: name='name5', value=55, id2=NULL
SELECT (SELECT name = 'name5' AND value = 55 AND id2 IS NULL FROM uuid_sync_test WHERE id = 'pg5') AS pg5_ok \gset
\if :pg5_ok
\echo [PASS] (:testid) pg5: values with NULL UUID preserved
\else
\echo [FAIL] (:testid) pg5: NULL UUID not preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- pg6: name=NULL, value=66, id2='c0eebc99-9c0b-4ef8-bb6d-6bb9bd380a33'
SELECT (SELECT name IS NULL AND value = 66 AND id2 = 'c0eebc99-9c0b-4ef8-bb6d-6bb9bd380a33' FROM uuid_sync_test WHERE id = 'pg6') AS pg6_ok \gset
\if :pg6_ok
\echo [PASS] (:testid) pg6: NULL name with UUID preserved
\else
\echo [FAIL] (:testid) pg6: UUID c0eebc99-9c0b-4ef8-bb6d-6bb9bd380a33 not preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Show actual data for debugging if there are failures
-- ============================================================================

\if :{?DEBUG}
\echo [INFO] (:testid) Database A data:
\connect cloudsync_test_23a
SELECT id, name, value, id2::text FROM uuid_sync_test ORDER BY id;

\echo [INFO] (:testid) Database B data:
\connect cloudsync_test_23b
SELECT id, name, value, id2::text FROM uuid_sync_test ORDER BY id;
\endif

-- ============================================================================
-- Cleanup
-- ============================================================================

\ir helper_test_cleanup.sql
\if :should_cleanup
\connect postgres
DROP DATABASE IF EXISTS cloudsync_test_23a;
DROP DATABASE IF EXISTS cloudsync_test_23b;
\endif
