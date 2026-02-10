-- Test: Nullable Types Roundtrip
-- This test verifies that syncing rows with various nullable column types works correctly.
-- Tests the type mapping for NULL values: INT2/4/8 → INT8, FLOAT4/8/NUMERIC → FLOAT8, BYTEA → BYTEA, others → TEXT
--
-- IMPORTANT: This test inserts NULL values FIRST to trigger SPI plan caching with decoded types,
-- then inserts non-NULL values to verify the type mapping is consistent.

\set testid '24'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

-- Cleanup and create test databases
DROP DATABASE IF EXISTS cloudsync_test_24a;
DROP DATABASE IF EXISTS cloudsync_test_24b;
CREATE DATABASE cloudsync_test_24a;
CREATE DATABASE cloudsync_test_24b;

-- ============================================================================
-- Setup Database A - Source database
-- ============================================================================

\connect cloudsync_test_24a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- Create table with various nullable column types
-- NOTE: BOOLEAN is excluded because it encodes as INTEGER but PostgreSQL can't cast INT8 to BOOLEAN.
-- This is a known limitation that requires changes to the encoding layer.
CREATE TABLE types_sync_test (
    id TEXT PRIMARY KEY NOT NULL,
    -- Integer types (all map to INT8OID in decoding)
    col_int2 SMALLINT,
    col_int4 INTEGER,
    col_int8 BIGINT,
    -- Float types (all map to FLOAT8OID in decoding)
    col_float4 REAL,
    col_float8 DOUBLE PRECISION,
    col_numeric NUMERIC(10,2),
    -- Binary type (maps to BYTEAOID in decoding)
    col_bytea BYTEA,
    -- Text types (all map to TEXTOID in decoding)
    col_text TEXT,
    col_varchar VARCHAR(100),
    col_char CHAR(10),
    -- Other types that map to TEXTOID
    col_uuid UUID,
    col_json JSON,
    col_jsonb JSONB,
    col_date DATE,
    col_timestamp TIMESTAMP
);

-- Initialize CloudSync
SELECT cloudsync_init('types_sync_test', 'CLS', true) AS _init_a \gset

-- ============================================================================
-- Setup Database B with same schema (before any inserts)
-- ============================================================================

\connect cloudsync_test_24b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE types_sync_test (
    id TEXT PRIMARY KEY NOT NULL,
    col_int2 SMALLINT,
    col_int4 INTEGER,
    col_int8 BIGINT,
    col_float4 REAL,
    col_float8 DOUBLE PRECISION,
    col_numeric NUMERIC(10,2),
    col_bytea BYTEA,
    col_text TEXT,
    col_varchar VARCHAR(100),
    col_char CHAR(10),
    col_uuid UUID,
    col_json JSON,
    col_jsonb JSONB,
    col_date DATE,
    col_timestamp TIMESTAMP
);

SELECT cloudsync_init('types_sync_test', 'CLS', true) AS _init_b \gset

-- ============================================================================
-- STEP 1: Insert row with ALL NULL values first (triggers SPI plan caching)
-- ============================================================================

\echo [INFO] (:testid) === STEP 1: Insert row with ALL NULL values ===

\connect cloudsync_test_24a

INSERT INTO types_sync_test (
    id, col_int2, col_int4, col_int8, col_float4, col_float8, col_numeric,
    col_bytea, col_text, col_varchar, col_char, col_uuid, col_json, col_jsonb,
    col_date, col_timestamp
) VALUES (
    'null_row', NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL
);

-- Encode payload
SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_step1_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

SELECT max(db_version) AS db_version FROM types_sync_test_cloudsync \gset

-- Apply to Database B
\connect cloudsync_test_24b
SELECT cloudsync_payload_apply(decode(:'payload_step1_hex', 'hex')) AS apply_step1 \gset

-- Verify step 1: all values should be NULL
SELECT (SELECT
    col_int2 IS NULL AND col_int4 IS NULL AND col_int8 IS NULL AND
    col_float4 IS NULL AND col_float8 IS NULL AND col_numeric IS NULL AND
    col_bytea IS NULL AND col_text IS NULL AND col_varchar IS NULL AND
    col_char IS NULL AND col_uuid IS NULL AND col_json IS NULL AND
    col_jsonb IS NULL AND col_date IS NULL AND col_timestamp IS NULL
FROM types_sync_test WHERE id = 'null_row') AS step1_ok \gset

\if :step1_ok
\echo [PASS] (:testid) Step 1: All NULL values preserved correctly
\else
\echo [FAIL] (:testid) Step 1: NULL values NOT preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- STEP 2: Insert row with ALL non-NULL values (tests type consistency)
-- ============================================================================

\echo [INFO] (:testid) === STEP 2: Insert row with ALL non-NULL values ===

\connect cloudsync_test_24a

INSERT INTO types_sync_test (
    id, col_int2, col_int4, col_int8, col_float4, col_float8, col_numeric,
    col_bytea, col_text, col_varchar, col_char, col_uuid, col_json, col_jsonb,
    col_date, col_timestamp
) VALUES (
    'full_row',
    32767,                                          -- INT2 max
    2147483647,                                     -- INT4 max
    9223372036854775807,                            -- INT8 max
    3.14159,                                        -- FLOAT4
    3.141592653589793,                              -- FLOAT8
    12345.67,                                       -- NUMERIC
    '\xDEADBEEF',                                   -- BYTEA
    'Hello, World!',                                -- TEXT
    'varchar_val',                                  -- VARCHAR
    'char_val',                                     -- CHAR (will be padded)
    'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11',        -- UUID
    '{"key": "value"}',                             -- JSON
    '{"nested": {"array": [1, 2, 3]}}',            -- JSONB
    '2024-01-15',                                   -- DATE
    '2024-01-15 10:30:00'                           -- TIMESTAMP
);

-- Encode payload (only new rows)
SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_step2_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() AND db_version > :db_version \gset

SELECT max(db_version) AS db_version FROM types_sync_test_cloudsync \gset

-- Apply to Database B
\connect cloudsync_test_24b
SELECT cloudsync_payload_apply(decode(:'payload_step2_hex', 'hex')) AS apply_step2 \gset

-- Verify step 2: Integer types
SELECT (SELECT col_int2 = 32767 FROM types_sync_test WHERE id = 'full_row') AS int2_ok \gset
\if :int2_ok
\echo [PASS] (:testid) INT2 (SMALLINT) value preserved: 32767
\else
\echo [FAIL] (:testid) INT2 (SMALLINT) value NOT preserved
SELECT (:fail::int + 1) AS fail \gset
SELECT col_int2 FROM types_sync_test WHERE id = 'full_row';
\endif

SELECT (SELECT col_int4 = 2147483647 FROM types_sync_test WHERE id = 'full_row') AS int4_ok \gset
\if :int4_ok
\echo [PASS] (:testid) INT4 (INTEGER) value preserved: 2147483647
\else
\echo [FAIL] (:testid) INT4 (INTEGER) value NOT preserved
SELECT (:fail::int + 1) AS fail \gset
SELECT col_int4 FROM types_sync_test WHERE id = 'full_row';
\endif

SELECT (SELECT col_int8 = 9223372036854775807 FROM types_sync_test WHERE id = 'full_row') AS int8_ok \gset
\if :int8_ok
\echo [PASS] (:testid) INT8 (BIGINT) value preserved: 9223372036854775807
\else
\echo [FAIL] (:testid) INT8 (BIGINT) value NOT preserved
SELECT (:fail::int + 1) AS fail \gset
SELECT col_int8 FROM types_sync_test WHERE id = 'full_row';
\endif

-- Verify step 2: Float types (use approximate comparison for floats)
SELECT (SELECT abs(col_float4 - 3.14159) < 0.0001 FROM types_sync_test WHERE id = 'full_row') AS float4_ok \gset
\if :float4_ok
\echo [PASS] (:testid) FLOAT4 (REAL) value preserved: ~3.14159
\else
\echo [FAIL] (:testid) FLOAT4 (REAL) value NOT preserved
SELECT (:fail::int + 1) AS fail \gset
SELECT col_float4 FROM types_sync_test WHERE id = 'full_row';
\endif

SELECT (SELECT abs(col_float8 - 3.141592653589793) < 0.000000000001 FROM types_sync_test WHERE id = 'full_row') AS float8_ok \gset
\if :float8_ok
\echo [PASS] (:testid) FLOAT8 (DOUBLE PRECISION) value preserved: ~3.141592653589793
\else
\echo [FAIL] (:testid) FLOAT8 (DOUBLE PRECISION) value NOT preserved
SELECT (:fail::int + 1) AS fail \gset
SELECT col_float8 FROM types_sync_test WHERE id = 'full_row';
\endif

SELECT (SELECT col_numeric = 12345.67 FROM types_sync_test WHERE id = 'full_row') AS numeric_ok \gset
\if :numeric_ok
\echo [PASS] (:testid) NUMERIC value preserved: 12345.67
\else
\echo [FAIL] (:testid) NUMERIC value NOT preserved
SELECT (:fail::int + 1) AS fail \gset
SELECT col_numeric FROM types_sync_test WHERE id = 'full_row';
\endif

-- Verify step 2: BYTEA type
SELECT (SELECT col_bytea = '\xDEADBEEF' FROM types_sync_test WHERE id = 'full_row') AS bytea_ok \gset
\if :bytea_ok
\echo [PASS] (:testid) BYTEA value preserved: DEADBEEF
\else
\echo [FAIL] (:testid) BYTEA value NOT preserved
SELECT (:fail::int + 1) AS fail \gset
SELECT encode(col_bytea, 'hex') FROM types_sync_test WHERE id = 'full_row';
\endif

-- Verify step 2: Text types
SELECT (SELECT col_text = 'Hello, World!' FROM types_sync_test WHERE id = 'full_row') AS text_ok \gset
\if :text_ok
\echo [PASS] (:testid) TEXT value preserved: Hello, World!
\else
\echo [FAIL] (:testid) TEXT value NOT preserved
SELECT (:fail::int + 1) AS fail \gset
SELECT col_text FROM types_sync_test WHERE id = 'full_row';
\endif

SELECT (SELECT col_varchar = 'varchar_val' FROM types_sync_test WHERE id = 'full_row') AS varchar_ok \gset
\if :varchar_ok
\echo [PASS] (:testid) VARCHAR value preserved: varchar_val
\else
\echo [FAIL] (:testid) VARCHAR value NOT preserved
SELECT (:fail::int + 1) AS fail \gset
SELECT col_varchar FROM types_sync_test WHERE id = 'full_row';
\endif

SELECT (SELECT trim(col_char) = 'char_val' FROM types_sync_test WHERE id = 'full_row') AS char_ok \gset
\if :char_ok
\echo [PASS] (:testid) CHAR value preserved: char_val
\else
\echo [FAIL] (:testid) CHAR value NOT preserved
SELECT (:fail::int + 1) AS fail \gset
SELECT col_char FROM types_sync_test WHERE id = 'full_row';
\endif

-- Verify step 2: Other types mapped to TEXT
SELECT (SELECT col_uuid = 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11' FROM types_sync_test WHERE id = 'full_row') AS uuid_ok \gset
\if :uuid_ok
\echo [PASS] (:testid) UUID value preserved: a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11
\else
\echo [FAIL] (:testid) UUID value NOT preserved
SELECT (:fail::int + 1) AS fail \gset
SELECT col_uuid FROM types_sync_test WHERE id = 'full_row';
\endif

SELECT (SELECT col_json::text = '{"key": "value"}' FROM types_sync_test WHERE id = 'full_row') AS json_ok \gset
\if :json_ok
\echo [PASS] (:testid) JSON value preserved: {"key": "value"}
\else
\echo [FAIL] (:testid) JSON value NOT preserved
SELECT (:fail::int + 1) AS fail \gset
SELECT col_json FROM types_sync_test WHERE id = 'full_row';
\endif

SELECT (SELECT col_jsonb @> '{"nested": {"array": [1, 2, 3]}}' FROM types_sync_test WHERE id = 'full_row') AS jsonb_ok \gset
\if :jsonb_ok
\echo [PASS] (:testid) JSONB value preserved: {"nested": {"array": [1, 2, 3]}}
\else
\echo [FAIL] (:testid) JSONB value NOT preserved
SELECT (:fail::int + 1) AS fail \gset
SELECT col_jsonb FROM types_sync_test WHERE id = 'full_row';
\endif

SELECT (SELECT col_date = '2024-01-15' FROM types_sync_test WHERE id = 'full_row') AS date_ok \gset
\if :date_ok
\echo [PASS] (:testid) DATE value preserved: 2024-01-15
\else
\echo [FAIL] (:testid) DATE value NOT preserved
SELECT (:fail::int + 1) AS fail \gset
SELECT col_date FROM types_sync_test WHERE id = 'full_row';
\endif

SELECT (SELECT col_timestamp = '2024-01-15 10:30:00' FROM types_sync_test WHERE id = 'full_row') AS timestamp_ok \gset
\if :timestamp_ok
\echo [PASS] (:testid) TIMESTAMP value preserved: 2024-01-15 10:30:00
\else
\echo [FAIL] (:testid) TIMESTAMP value NOT preserved
SELECT (:fail::int + 1) AS fail \gset
SELECT col_timestamp FROM types_sync_test WHERE id = 'full_row';
\endif

-- ============================================================================
-- STEP 3: Insert row with mixed NULL/non-NULL values
-- ============================================================================

\echo [INFO] (:testid) === STEP 3: Insert row with mixed NULL/non-NULL values ===

\connect cloudsync_test_24a

INSERT INTO types_sync_test (
    id, col_int2, col_int4, col_int8, col_float4, col_float8, col_numeric,
    col_bytea, col_text, col_varchar, col_char, col_uuid, col_json, col_jsonb,
    col_date, col_timestamp
) VALUES (
    'mixed_row',
    NULL,                                           -- INT2 NULL
    42,                                             -- INT4 non-NULL
    NULL,                                           -- INT8 NULL
    NULL,                                           -- FLOAT4 NULL
    2.718281828,                                    -- FLOAT8 non-NULL (e)
    NULL,                                           -- NUMERIC NULL
    '\xCAFEBABE',                                   -- BYTEA non-NULL
    NULL,                                           -- TEXT NULL
    'mixed',                                        -- VARCHAR non-NULL
    NULL,                                           -- CHAR NULL
    'b0eebc99-9c0b-4ef8-bb6d-6bb9bd380a22',        -- UUID non-NULL
    NULL,                                           -- JSON NULL
    '{"mixed": true}',                              -- JSONB non-NULL
    NULL,                                           -- DATE NULL
    '2024-06-15 14:00:00'                           -- TIMESTAMP non-NULL
);

-- Encode payload (only new rows)
SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_step3_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() AND db_version > :db_version \gset

SELECT max(db_version) AS db_version FROM types_sync_test_cloudsync \gset

-- Apply to Database B
\connect cloudsync_test_24b
SELECT cloudsync_payload_apply(decode(:'payload_step3_hex', 'hex')) AS apply_step3 \gset

-- Verify mixed row
SELECT (SELECT
    col_int2 IS NULL AND
    col_int4 = 42 AND
    col_int8 IS NULL AND
    col_float4 IS NULL AND
    abs(col_float8 - 2.718281828) < 0.000001 AND
    col_numeric IS NULL AND
    col_bytea = '\xCAFEBABE' AND
    col_text IS NULL AND
    col_varchar = 'mixed' AND
    col_char IS NULL AND
    col_uuid = 'b0eebc99-9c0b-4ef8-bb6d-6bb9bd380a22' AND
    col_json IS NULL AND
    col_jsonb @> '{"mixed": true}' AND
    col_date IS NULL AND
    col_timestamp = '2024-06-15 14:00:00'
FROM types_sync_test WHERE id = 'mixed_row') AS mixed_ok \gset

\if :mixed_ok
\echo [PASS] (:testid) Mixed NULL/non-NULL row preserved correctly
\else
\echo [FAIL] (:testid) Mixed NULL/non-NULL row NOT preserved correctly
SELECT (:fail::int + 1) AS fail \gset
SELECT * FROM types_sync_test WHERE id = 'mixed_row';
\endif

-- ============================================================================
-- STEP 4: Verify data integrity with hash comparison
-- ============================================================================

\echo [INFO] (:testid) === STEP 4: Verify data integrity ===

\connect cloudsync_test_24a

SELECT md5(
    COALESCE(
        string_agg(
            id || ':' ||
            COALESCE(col_int2::text, 'NULL') || ':' ||
            COALESCE(col_int4::text, 'NULL') || ':' ||
            COALESCE(col_int8::text, 'NULL') || ':' ||
            COALESCE(col_float8::text, 'NULL') || ':' ||
            COALESCE(col_numeric::text, 'NULL') || ':' ||
            COALESCE(encode(col_bytea, 'hex'), 'NULL') || ':' ||
            COALESCE(col_text, 'NULL') || ':' ||
            COALESCE(col_varchar, 'NULL') || ':' ||
            COALESCE(col_uuid::text, 'NULL') || ':' ||
            COALESCE(col_jsonb::text, 'NULL') || ':' ||
            COALESCE(col_date::text, 'NULL') || ':' ||
            COALESCE(col_timestamp::text, 'NULL'),
            '|' ORDER BY id
        ),
        ''
    )
) AS hash_a FROM types_sync_test \gset

\echo [INFO] (:testid) Database A hash: :hash_a

\connect cloudsync_test_24b

SELECT md5(
    COALESCE(
        string_agg(
            id || ':' ||
            COALESCE(col_int2::text, 'NULL') || ':' ||
            COALESCE(col_int4::text, 'NULL') || ':' ||
            COALESCE(col_int8::text, 'NULL') || ':' ||
            COALESCE(col_float8::text, 'NULL') || ':' ||
            COALESCE(col_numeric::text, 'NULL') || ':' ||
            COALESCE(encode(col_bytea, 'hex'), 'NULL') || ':' ||
            COALESCE(col_text, 'NULL') || ':' ||
            COALESCE(col_varchar, 'NULL') || ':' ||
            COALESCE(col_uuid::text, 'NULL') || ':' ||
            COALESCE(col_jsonb::text, 'NULL') || ':' ||
            COALESCE(col_date::text, 'NULL') || ':' ||
            COALESCE(col_timestamp::text, 'NULL'),
            '|' ORDER BY id
        ),
        ''
    )
) AS hash_b FROM types_sync_test \gset

\echo [INFO] (:testid) Database B hash: :hash_b

SELECT (:'hash_a' = :'hash_b') AS hashes_match \gset
\if :hashes_match
\echo [PASS] (:testid) Data integrity verified - hashes match
\else
\echo [FAIL] (:testid) Data integrity check failed - hashes do not match
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify row count
SELECT COUNT(*) AS count_b FROM types_sync_test \gset
SELECT (:count_b = 3) AS row_counts_match \gset
\if :row_counts_match
\echo [PASS] (:testid) Row counts match (3 rows)
\else
\echo [FAIL] (:testid) Row counts mismatch - Expected 3, got :count_b
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Show actual data for debugging if there are failures
-- ============================================================================

\if :{?DEBUG}
\echo [INFO] (:testid) Database A data:
\connect cloudsync_test_24a
SELECT id, col_int2, col_int4, col_int8, col_float4, col_float8, col_numeric FROM types_sync_test ORDER BY id;
SELECT id, encode(col_bytea, 'hex') as col_bytea, col_text, col_varchar, trim(col_char) as col_char FROM types_sync_test ORDER BY id;
SELECT id, col_uuid, col_json, col_jsonb, col_date, col_timestamp FROM types_sync_test ORDER BY id;

\echo [INFO] (:testid) Database B data:
\connect cloudsync_test_24b
SELECT id, col_int2, col_int4, col_int8, col_float4, col_float8, col_numeric FROM types_sync_test ORDER BY id;
SELECT id, encode(col_bytea, 'hex') as col_bytea, col_text, col_varchar, trim(col_char) as col_char FROM types_sync_test ORDER BY id;
SELECT id, col_uuid, col_json, col_jsonb, col_date, col_timestamp FROM types_sync_test ORDER BY id;
\endif

-- ============================================================================
-- Cleanup
-- ============================================================================

\ir helper_test_cleanup.sql
\if :should_cleanup
\connect postgres
DROP DATABASE IF EXISTS cloudsync_test_24a;
DROP DATABASE IF EXISTS cloudsync_test_24b;
\endif
