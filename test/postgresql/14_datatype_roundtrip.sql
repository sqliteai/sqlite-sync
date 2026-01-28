-- DBTYPE Roundtrip Test
-- Tests encoding/decoding of all DBTYPEs (INTEGER, FLOAT, TEXT, BLOB, NULL)
-- in the internal value representation during database synchronization

\set testid '14'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

-- Cleanup and create test databases
DROP DATABASE IF EXISTS cloudsync_test_14a;
DROP DATABASE IF EXISTS cloudsync_test_14b;
CREATE DATABASE cloudsync_test_14a;
CREATE DATABASE cloudsync_test_14b;

-- ============================================================================
-- Setup Database A with comprehensive table
-- ============================================================================

\connect cloudsync_test_14a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- Create table with composite primary key and all data types
CREATE TABLE all_types (
    -- Composite primary key (TEXT columns as required by CloudSync)
    id1 TEXT NOT NULL,
    id2 TEXT NOT NULL,
    PRIMARY KEY (id1, id2),

    -- INTEGER columns
    col_int_notnull INTEGER NOT NULL DEFAULT 0,
    col_int_nullable INTEGER,

    -- FLOAT columns (using DOUBLE PRECISION in PostgreSQL)
    col_float_notnull DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    col_float_nullable DOUBLE PRECISION,

    -- TEXT columns
    col_text_notnull TEXT NOT NULL DEFAULT '',
    col_text_nullable TEXT,

    -- BLOB columns (BYTEA in PostgreSQL)
    col_blob_notnull BYTEA NOT NULL DEFAULT E'\\x00',
    col_blob_nullable BYTEA
);

-- Initialize CloudSync
SELECT cloudsync_init('all_types', 'CLS', true) AS _init_a \gset

-- ============================================================================
-- Insert test data with various values for each type
-- ============================================================================

-- Row 1: All non-null values
INSERT INTO all_types VALUES (
    'pk1', 'pk2',
    -- INTEGER
    42, 100,
    -- FLOAT
    3.14159, 2.71828,
    -- TEXT
    'hello world', 'test string',
    -- BLOB
    E'\\xDEADBEEF', E'\\xCAFEBABE'
);

-- Row 2: Mix of null and non-null
INSERT INTO all_types (id1, id2, col_int_notnull, col_float_notnull, col_text_notnull, col_blob_notnull)
VALUES (
    'pk3', 'pk4',
    -999,
    -123.456,
    'only required fields',
    E'\\x0102030405'
);

-- Row 3: Edge cases - zeros, empty strings, single byte blob
INSERT INTO all_types VALUES (
    'pk5', 'pk6',
    0, 0,
    0.0, 0.0,
    '', '',
    E'\\x00', E'\\x00'
);

-- Row 4: Large values
INSERT INTO all_types VALUES (
    'pk7', 'pk8',
    2147483647, -2147483648, -- INT max and min
    1.7976931348623157e+308, -1.7976931348623157e+308, -- Large floats
    'Lorem ipsum dolor sit amet, consectetur adipiscing elit. ' ||
    'Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.',
    'Another long text with special chars: café, naïve, 日本語, emoji: 🚀',
    E'\\xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF', -- Large blob (16 bytes of 0xFF)
    E'\\x0102030405060708090A0B0C0D0E0F10' -- Sequential bytes
);

-- Row 5: Special characters in text
INSERT INTO all_types VALUES (
    'pk9', 'pk10',
    1, 2,
    1.5, 2.5,
    E'Special\nchars:\t\r\nand\\backslash', -- Escaped characters
    E'Quote''s and "double" quotes',
    E'\\x5C00', -- Backslash byte and null byte
    E'\\x0D0A' -- CR LF
);

-- ============================================================================
-- Compute hash of Database A data
-- ============================================================================

SELECT md5(
    COALESCE(
        string_agg(
            id1 || ':' || id2 || ':' ||
            COALESCE(col_int_notnull::text, 'NULL') || ':' ||
            COALESCE(col_int_nullable::text, 'NULL') || ':' ||
            COALESCE(col_float_notnull::text, 'NULL') || ':' ||
            COALESCE(col_float_nullable::text, 'NULL') || ':' ||
            COALESCE(col_text_notnull, 'NULL') || ':' ||
            COALESCE(col_text_nullable, 'NULL') || ':' ||
            COALESCE(encode(col_blob_notnull, 'hex'), 'NULL') || ':' ||
            COALESCE(encode(col_blob_nullable, 'hex'), 'NULL'),
            '|' ORDER BY id1, id2
        ),
        ''
    )
) AS hash_a FROM all_types \gset

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

\connect cloudsync_test_14b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- Create identical table schema
CREATE TABLE all_types (
    id1 TEXT NOT NULL,
    id2 TEXT NOT NULL,
    PRIMARY KEY (id1, id2),
    col_int_notnull INTEGER NOT NULL DEFAULT 0,
    col_int_nullable INTEGER,
    col_float_notnull DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    col_float_nullable DOUBLE PRECISION,
    col_text_notnull TEXT NOT NULL DEFAULT '',
    col_text_nullable TEXT,
    col_blob_notnull BYTEA NOT NULL DEFAULT E'\\x00',
    col_blob_nullable BYTEA
);

-- Initialize CloudSync
SELECT cloudsync_init('all_types', 'CLS', true) AS _init_b \gset

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

-- Compute hash of Database B data (should match Database A)
SELECT md5(
    COALESCE(
        string_agg(
            id1 || ':' || id2 || ':' ||
            COALESCE(col_int_notnull::text, 'NULL') || ':' ||
            COALESCE(col_int_nullable::text, 'NULL') || ':' ||
            COALESCE(col_float_notnull::text, 'NULL') || ':' ||
            COALESCE(col_float_nullable::text, 'NULL') || ':' ||
            COALESCE(col_text_notnull, 'NULL') || ':' ||
            COALESCE(col_text_nullable, 'NULL') || ':' ||
            COALESCE(encode(col_blob_notnull, 'hex'), 'NULL') || ':' ||
            COALESCE(encode(col_blob_nullable, 'hex'), 'NULL'),
            '|' ORDER BY id1, id2
        ),
        ''
    )
) AS hash_b FROM all_types \gset

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

SELECT COUNT(*) AS count_a FROM all_types \gset
\connect cloudsync_test_14a
SELECT COUNT(*) AS count_a_orig FROM all_types \gset

\connect cloudsync_test_14b
SELECT (:count_a = :count_a_orig) AS row_counts_match \gset
\if :row_counts_match
\echo [PASS] (:testid) Row counts match (:count_a rows)
\else
\echo [FAIL] (:testid) Row counts mismatch - Database A: :count_a_orig, Database B: :count_a
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test specific data type preservation
-- ============================================================================

-- Verify INTEGER values
SELECT
    (SELECT col_int_notnull FROM all_types WHERE id1 = 'pk1' AND id2 = 'pk2') = 42 AND
    (SELECT col_int_nullable FROM all_types WHERE id1 = 'pk1' AND id2 = 'pk2') = 100 AND
    (SELECT col_int_nullable FROM all_types WHERE id1 = 'pk3' AND id2 = 'pk4') IS NULL
AS integers_ok \gset
\if :integers_ok
\echo [PASS] (:testid) INTEGER type preservation
\else
\echo [FAIL] (:testid) INTEGER type preservation
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify FLOAT values (with tolerance for floating point)
SELECT
    ABS((SELECT col_float_notnull FROM all_types WHERE id1 = 'pk1' AND id2 = 'pk2') - 3.14159) < 0.00001 AND
    ABS((SELECT col_float_nullable FROM all_types WHERE id1 = 'pk1' AND id2 = 'pk2') - 2.71828) < 0.00001 AND
    (SELECT col_float_nullable FROM all_types WHERE id1 = 'pk3' AND id2 = 'pk4') IS NULL
AS floats_ok \gset
\if :floats_ok
\echo [PASS] (:testid) FLOAT type preservation
\else
\echo [FAIL] (:testid) FLOAT type preservation
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify TEXT values
SELECT
    (SELECT col_text_notnull FROM all_types WHERE id1 = 'pk1' AND id2 = 'pk2') = 'hello world' AND
    (SELECT col_text_nullable FROM all_types WHERE id1 = 'pk1' AND id2 = 'pk2') = 'test string' AND
    (SELECT col_text_nullable FROM all_types WHERE id1 = 'pk3' AND id2 = 'pk4') IS NULL AND
    (SELECT col_text_notnull FROM all_types WHERE id1 = 'pk5' AND id2 = 'pk6') = ''
AS text_ok \gset
\if :text_ok
\echo [PASS] (:testid) TEXT type preservation
\else
\echo [FAIL] (:testid) TEXT type preservation
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify BLOB values
SELECT
    encode((SELECT col_blob_notnull FROM all_types WHERE id1 = 'pk1' AND id2 = 'pk2'), 'hex') = 'deadbeef' AND
    encode((SELECT col_blob_nullable FROM all_types WHERE id1 = 'pk1' AND id2 = 'pk2'), 'hex') = 'cafebabe' AND
    (SELECT col_blob_nullable FROM all_types WHERE id1 = 'pk3' AND id2 = 'pk4') IS NULL
AS blobs_ok \gset
\if :blobs_ok
\echo [PASS] (:testid) BLOB type preservation
\else
\echo [FAIL] (:testid) BLOB type preservation
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify NULL handling
SELECT
    (SELECT col_int_nullable FROM all_types WHERE id1 = 'pk3' AND id2 = 'pk4') IS NULL AND
    (SELECT col_float_nullable FROM all_types WHERE id1 = 'pk3' AND id2 = 'pk4') IS NULL AND
    (SELECT col_text_nullable FROM all_types WHERE id1 = 'pk3' AND id2 = 'pk4') IS NULL AND
    (SELECT col_blob_nullable FROM all_types WHERE id1 = 'pk3' AND id2 = 'pk4') IS NULL
AS nulls_ok \gset
\if :nulls_ok
\echo [PASS] (:testid) NULL type preservation
\else
\echo [FAIL] (:testid) NULL type preservation
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test special characters and edge cases
-- ============================================================================

-- Verify special characters in TEXT
SELECT
    (SELECT col_text_notnull FROM all_types WHERE id1 = 'pk9' AND id2 = 'pk10') = E'Special\nchars:\t\r\nand\\backslash'
AS special_chars_ok \gset
\if :special_chars_ok
\echo [PASS] (:testid) Special characters in TEXT preserved
\else
\echo [FAIL] (:testid) Special characters in TEXT not preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify zero values
SELECT
    (SELECT col_int_notnull FROM all_types WHERE id1 = 'pk5' AND id2 = 'pk6') = 0 AND
    (SELECT col_float_notnull FROM all_types WHERE id1 = 'pk5' AND id2 = 'pk6') = 0.0
AS zero_values_ok \gset
\if :zero_values_ok
\echo [PASS] (:testid) Zero values preserved
\else
\echo [FAIL] (:testid) Zero values not preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test composite primary key encoding
-- ============================================================================

-- Verify all primary key combinations are present
SELECT COUNT(DISTINCT (id1, id2)) = 5 AS pk_count_ok FROM all_types \gset
\if :pk_count_ok
\echo [PASS] (:testid) Composite primary keys preserved
\else
\echo [FAIL] (:testid) Composite primary keys not all preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test bidirectional sync (B -> A)
-- ============================================================================

\connect cloudsync_test_14b

-- Add a new row in Database B
INSERT INTO all_types VALUES (
    'pkB1', 'pkB2',
    999, 888,
    9.99, 8.88,
    'from database B', 'bidirectional test',
    E'\\xBBBBBBBB', E'\\xAAAAAAAA'
);

-- Encode payload from Database B
SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_b_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

-- Apply to Database A
\connect cloudsync_test_14a
SELECT cloudsync_payload_apply(decode(:'payload_b_hex', 'hex')) AS apply_b_to_a \gset

-- Verify the new row exists in Database A
SELECT COUNT(*) = 1 AS bidirectional_ok
FROM all_types
WHERE id1 = 'pkB1' AND id2 = 'pkB2' AND col_text_notnull = 'from database B' \gset
\if :bidirectional_ok
\echo [PASS] (:testid) Bidirectional sync works (B to A)
\else
\echo [FAIL] (:testid) Bidirectional sync failed
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Cleanup: Drop test databases if not in DEBUG mode and no failures
-- ============================================================================

\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_14a;
DROP DATABASE IF EXISTS cloudsync_test_14b;
\endif
