-- Init With Existing Data Test
-- Tests cloudsync_init on a table that already contains data.
-- This verifies that cloudsync_refill_metatable correctly creates
-- metadata entries for pre-existing rows.

\set testid '20'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

-- Cleanup and create test databases
DROP DATABASE IF EXISTS cloudsync_test_20a;
DROP DATABASE IF EXISTS cloudsync_test_20b;
CREATE DATABASE cloudsync_test_20a;
CREATE DATABASE cloudsync_test_20b;

-- ============================================================================
-- Setup Database A - INSERT DATA BEFORE cloudsync_init
-- ============================================================================

\connect cloudsync_test_20a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- Create table with UUID primary key (required for CRDT replication)
CREATE TABLE items (
    id UUID PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    price DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    quantity INTEGER NOT NULL DEFAULT 0,
    metadata JSONB
);

-- ============================================================================
-- INSERT DATA BEFORE CALLING cloudsync_init
-- This is the key difference from other tests - data exists before sync setup
-- ============================================================================

INSERT INTO items VALUES ('11111111-1111-1111-1111-111111111111', 'Pre-existing Item 1', 10.99, 100, '{"pre": true}');
INSERT INTO items VALUES ('22222222-2222-2222-2222-222222222222', 'Pre-existing Item 2', 20.50, 200, '{"pre": true, "id": 2}');
INSERT INTO items VALUES ('33333333-3333-3333-3333-333333333333', 'Pre-existing Item 3', 30.00, 300, NULL);
INSERT INTO items VALUES ('44444444-4444-4444-4444-444444444444', 'Pre-existing Item 4', 0.0, 0, '[]');
INSERT INTO items VALUES ('55555555-5555-5555-5555-555555555555', 'Pre-existing Item 5', -5.50, -10, '{"nested": {"key": "value"}}');

-- Verify data exists before init
SELECT COUNT(*) AS pre_init_count FROM items \gset
\echo [INFO] (:testid) Rows before cloudsync_init: :pre_init_count

-- ============================================================================
-- NOW call cloudsync_init - this should trigger cloudsync_refill_metatable
-- ============================================================================

SELECT cloudsync_init('items', 'CLS', false) AS _init_a \gset

-- ============================================================================
-- Verify metadata was created for existing rows
-- ============================================================================

-- Check that metadata table exists and has entries
SELECT COUNT(*) AS metadata_count FROM items_cloudsync \gset

SELECT (:metadata_count > 0) AS metadata_created \gset
\if :metadata_created
\echo [PASS] (:testid) Metadata table populated after init (:metadata_count entries)
\else
\echo [FAIL] (:testid) Metadata table empty after init - cloudsync_refill_metatable may have failed
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Compute hash of Database A data
-- ============================================================================

SELECT md5(
    COALESCE(
        string_agg(
            id::text || ':' ||
            COALESCE(name, 'NULL') || ':' ||
            COALESCE(price::text, 'NULL') || ':' ||
            COALESCE(quantity::text, 'NULL') || ':' ||
            COALESCE(metadata::text, 'NULL'),
            '|' ORDER BY id
        ),
        ''
    )
) AS hash_a FROM items \gset

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
\echo [PASS] (:testid) Payload encoded from Database A (pre-existing data)
\else
\echo [FAIL] (:testid) Payload encoded from Database A - Empty payload
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Setup Database B with same schema (empty)
-- ============================================================================

\connect cloudsync_test_20b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE items (
    id UUID PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    price DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    quantity INTEGER NOT NULL DEFAULT 0,
    metadata JSONB
);

-- Initialize CloudSync on empty table
SELECT cloudsync_init('items', 'CLS', false) AS _init_b \gset

-- ============================================================================
-- Apply payload to Database B
-- ============================================================================

SELECT cloudsync_payload_apply(decode(:'payload_a_hex', 'hex')) AS apply_result \gset

-- Verify application succeeded
SELECT (:apply_result >= 0) AS payload_applied \gset
\if :payload_applied
\echo [PASS] (:testid) Payload applied to Database B
\else
\echo [FAIL] (:testid) Payload applied to Database B - Apply returned :apply_result
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Verify data integrity after roundtrip
-- ============================================================================

SELECT md5(
    COALESCE(
        string_agg(
            id::text || ':' ||
            COALESCE(name, 'NULL') || ':' ||
            COALESCE(price::text, 'NULL') || ':' ||
            COALESCE(quantity::text, 'NULL') || ':' ||
            COALESCE(metadata::text, 'NULL'),
            '|' ORDER BY id
        ),
        ''
    )
) AS hash_b FROM items \gset

\echo [INFO] (:testid) Database B hash: :hash_b

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

SELECT COUNT(*) AS count_b FROM items \gset
\connect cloudsync_test_20a
SELECT COUNT(*) AS count_a_orig FROM items \gset

\connect cloudsync_test_20b
SELECT (:count_b = :count_a_orig) AS row_counts_match \gset
\if :row_counts_match
\echo [PASS] (:testid) Row counts match (:count_b rows)
\else
\echo [FAIL] (:testid) Row counts mismatch - Database A: :count_a_orig, Database B: :count_b
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Verify specific pre-existing data was synced correctly
-- ============================================================================

SELECT COUNT(*) = 1 AS item1_ok
FROM items
WHERE id = '11111111-1111-1111-1111-111111111111'
  AND name = 'Pre-existing Item 1'
  AND price = 10.99
  AND quantity = 100 \gset
\if :item1_ok
\echo [PASS] (:testid) Pre-existing item 1 synced correctly
\else
\echo [FAIL] (:testid) Pre-existing item 1 not found or incorrect
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify JSONB data
SELECT COUNT(*) = 1 AS jsonb_ok
FROM items
WHERE id = '55555555-5555-5555-5555-555555555555' AND metadata = '{"nested": {"key": "value"}}'::jsonb \gset
\if :jsonb_ok
\echo [PASS] (:testid) JSONB data synced correctly
\else
\echo [FAIL] (:testid) JSONB data not synced correctly
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test: Add new data AFTER init, verify it also syncs
-- ============================================================================

\connect cloudsync_test_20a

-- Add new row after init
INSERT INTO items VALUES ('66666666-6666-6666-6666-666666666666', 'Post-init Item', 66.66, 666, '{"post": true}');

-- Encode new changes
SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_a2_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_test_20b
SELECT cloudsync_payload_apply(decode(:'payload_a2_hex', 'hex')) AS apply_result2 \gset

SELECT COUNT(*) = 1 AS post_init_ok
FROM items
WHERE id = '66666666-6666-6666-6666-666666666666' AND name = 'Post-init Item' \gset
\if :post_init_ok
\echo [PASS] (:testid) Post-init data syncs correctly
\else
\echo [FAIL] (:testid) Post-init data failed to sync
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test bidirectional sync (B -> A)
-- ============================================================================

INSERT INTO items VALUES ('77777777-7777-7777-7777-777777777777', 'From B', 77.77, 777, '{"from": "B"}');

SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_b_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_test_20a
SELECT cloudsync_payload_apply(decode(:'payload_b_hex', 'hex')) AS apply_b_to_a \gset

SELECT COUNT(*) = 1 AS bidirectional_ok
FROM items
WHERE id = '77777777-7777-7777-7777-777777777777' AND name = 'From B' \gset
\if :bidirectional_ok
\echo [PASS] (:testid) Bidirectional sync works (B to A)
\else
\echo [FAIL] (:testid) Bidirectional sync failed
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Final verification: total row count should be 7 in both databases
-- ============================================================================

SELECT COUNT(*) AS final_count_a FROM items \gset
\connect cloudsync_test_20b
SELECT COUNT(*) AS final_count_b FROM items \gset

SELECT (:final_count_a = 7 AND :final_count_b = 7) AS final_counts_ok \gset
\if :final_counts_ok
\echo [PASS] (:testid) Final row counts correct (7 rows each)
\else
\echo [FAIL] (:testid) Final row counts incorrect - A: :final_count_a, B: :final_count_b
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Cleanup: Drop test databases if not in DEBUG mode and no failures
-- ============================================================================

\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_20a;
DROP DATABASE IF EXISTS cloudsync_test_20b;
\endif
