-- Bulk Insert Performance Roundtrip Test
-- Tests roundtrip with 1000 rows and measures time for each operation.

\set testid '18'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

-- Cleanup and create test databases
DROP DATABASE IF EXISTS cloudsync_test_18a;
DROP DATABASE IF EXISTS cloudsync_test_18b;
CREATE DATABASE cloudsync_test_18a;
CREATE DATABASE cloudsync_test_18b;

-- ============================================================================
-- Setup Database A
-- ============================================================================

\connect cloudsync_test_18a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE items (
    id UUID NOT NULL PRIMARY KEY DEFAULT cloudsync_uuid(),
    name TEXT NOT NULL DEFAULT '',
    value DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    quantity INTEGER NOT NULL DEFAULT 0,
    description TEXT
);

-- Initialize CloudSync
SELECT cloudsync_init('items', 'CLS', false) AS _init_a \gset

-- ============================================================================
-- Record start time
-- ============================================================================

SELECT clock_timestamp() AS test_start_time \gset
\echo [INFO] (:testid) Test started at :test_start_time

-- ============================================================================
-- Insert 1000 rows and measure time
-- ============================================================================

SELECT clock_timestamp() AS insert_start_time \gset

INSERT INTO items (name, value, quantity, description)
SELECT
    'Item ' || i,
    (random() * 1000)::DOUBLE PRECISION,
    (random() * 100)::INTEGER,
    'Description for item ' || i || ' with some additional text to simulate real data'
FROM generate_series(1, 1000) AS i;

SELECT clock_timestamp() AS insert_end_time \gset

SELECT EXTRACT(EPOCH FROM (:'insert_end_time'::timestamp - :'insert_start_time'::timestamp)) * 1000 AS insert_time_ms \gset
\echo [INFO] (:testid) Insert 1000 rows: :insert_time_ms ms

-- ============================================================================
-- Verify row count in Database A
-- ============================================================================

SELECT COUNT(*) AS count_a FROM items \gset
SELECT (:count_a = 1000) AS insert_count_ok \gset
\if :insert_count_ok
\echo [PASS] (:testid) Inserted 1000 rows successfully
\else
\echo [FAIL] (:testid) Expected 1000 rows, got :count_a
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
            COALESCE(value::text, 'NULL') || ':' ||
            COALESCE(quantity::text, 'NULL') || ':' ||
            COALESCE(description, 'NULL'),
            '|' ORDER BY id
        ),
        ''
    )
) AS hash_a FROM items \gset

\echo [INFO] (:testid) Database A hash: :hash_a

-- ============================================================================
-- Encode payload from Database A and measure time
-- ============================================================================

SELECT clock_timestamp() AS encode_start_time \gset

SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_a_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

SELECT clock_timestamp() AS encode_end_time \gset

SELECT EXTRACT(EPOCH FROM (:'encode_end_time'::timestamp - :'encode_start_time'::timestamp)) * 1000 AS encode_time_ms \gset
\echo [INFO] (:testid) Encode payload: :encode_time_ms ms

-- Verify payload was created
SELECT (length(:'payload_a_hex') > 0) AS payload_created \gset
\if :payload_created
\echo [PASS] (:testid) Payload encoded from Database A
\else
\echo [FAIL] (:testid) Payload encoded from Database A - Empty payload
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Report payload size
SELECT length(:'payload_a_hex') / 2 AS payload_size_bytes \gset
\echo [INFO] (:testid) Payload size: :payload_size_bytes bytes

-- ============================================================================
-- Setup Database B with same schema
-- ============================================================================

\connect cloudsync_test_18b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE items (
    id UUID NOT NULL PRIMARY KEY DEFAULT cloudsync_uuid(),
    name TEXT NOT NULL DEFAULT '',
    value DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    quantity INTEGER NOT NULL DEFAULT 0,
    description TEXT
);

-- Initialize CloudSync
SELECT cloudsync_init('items', 'CLS', false) AS _init_b \gset

-- ============================================================================
-- Apply payload to Database B and measure time
-- ============================================================================

SELECT clock_timestamp() AS apply_start_time \gset

SELECT cloudsync_payload_apply(decode(:'payload_a_hex', 'hex')) AS apply_result \gset

SELECT clock_timestamp() AS apply_end_time \gset

SELECT EXTRACT(EPOCH FROM (:'apply_end_time'::timestamp - :'apply_start_time'::timestamp)) * 1000 AS apply_time_ms \gset
\echo [INFO] (:testid) Apply payload: :apply_time_ms ms

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
            COALESCE(value::text, 'NULL') || ':' ||
            COALESCE(quantity::text, 'NULL') || ':' ||
            COALESCE(description, 'NULL'),
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

SELECT (:count_b = :count_a) AS row_counts_match \gset
\if :row_counts_match
\echo [PASS] (:testid) Row counts match (:count_b rows)
\else
\echo [FAIL] (:testid) Row counts mismatch - Database A: :count_a, Database B: :count_b
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Verify sample data integrity
-- ============================================================================

SELECT COUNT(*) = 1 AS sample_check_ok
FROM items
WHERE name = 'Item 500' \gset
\if :sample_check_ok
\echo [PASS] (:testid) Sample row verified (name='Item 500')
\else
\echo [FAIL] (:testid) Sample row verification failed
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Calculate and report total elapsed time
-- ============================================================================

SELECT clock_timestamp() AS test_end_time \gset

SELECT EXTRACT(EPOCH FROM (:'test_end_time'::timestamp - :'test_start_time'::timestamp)) * 1000 AS total_time_ms \gset

\echo [INFO] (:testid) Performance summary:
\echo [INFO] (:testid)   - Insert 1000 rows: :insert_time_ms ms
\echo [INFO] (:testid)   - Encode payload: :encode_time_ms ms
\echo [INFO] (:testid)   - Apply payload: :apply_time_ms ms
\echo [INFO] (:testid)   - Total elapsed time: :total_time_ms ms

-- ============================================================================
-- Cleanup: Drop test databases if not in DEBUG mode and no failures
-- ============================================================================

\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_18a;
DROP DATABASE IF EXISTS cloudsync_test_18b;
\endif
