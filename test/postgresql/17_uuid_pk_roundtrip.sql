-- UUID Primary Key Roundtrip Test
-- Tests roundtrip with a UUID primary key (single column).

\set testid '17'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

-- Cleanup and create test databases
DROP DATABASE IF EXISTS cloudsync_test_17a;
DROP DATABASE IF EXISTS cloudsync_test_17b;
CREATE DATABASE cloudsync_test_17a;
CREATE DATABASE cloudsync_test_17b;

-- ============================================================================
-- Setup Database A with UUID primary key
-- ============================================================================

\connect cloudsync_test_17a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE products (
    id UUID PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    price DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    stock INTEGER NOT NULL DEFAULT 0,
    metadata BYTEA
);

-- Initialize CloudSync
SELECT cloudsync_init('products', 'CLS', false) AS _init_a \gset

-- ============================================================================
-- Insert test data with UUIDs
-- ============================================================================

INSERT INTO products VALUES ('550e8400-e29b-41d4-a716-446655440000', 'Product A', 99.99, 100, E'\\xDEADBEEF');
INSERT INTO products VALUES ('6ba7b810-9dad-11d1-80b4-00c04fd430c8', 'Product B', 49.50, 50, NULL);
INSERT INTO products VALUES ('6ba7b811-9dad-11d1-80b4-00c04fd430c8', 'Product C', 0.0, 0, E'\\x00');
INSERT INTO products VALUES ('6ba7b812-9dad-11d1-80b4-00c04fd430c8', 'Product D', 123.45, 999, E'\\xCAFEBABE');
INSERT INTO products VALUES ('6ba7b813-9dad-11d1-80b4-00c04fd430c8', '', -1.0, -1, E'\\x010203');

-- ============================================================================
-- Compute hash of Database A data
-- ============================================================================

SELECT md5(
    COALESCE(
        string_agg(
            id::text || ':' ||
            COALESCE(name, 'NULL') || ':' ||
            COALESCE(price::text, 'NULL') || ':' ||
            COALESCE(stock::text, 'NULL') || ':' ||
            COALESCE(encode(metadata, 'hex'), 'NULL'),
            '|' ORDER BY id
        ),
        ''
    )
) AS hash_a FROM products \gset

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

\connect cloudsync_test_17b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE products (
    id UUID PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    price DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    stock INTEGER NOT NULL DEFAULT 0,
    metadata BYTEA
);

-- Initialize CloudSync
SELECT cloudsync_init('products', 'CLS', false) AS _init_b \gset

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
            COALESCE(stock::text, 'NULL') || ':' ||
            COALESCE(encode(metadata, 'hex'), 'NULL'),
            '|' ORDER BY id
        ),
        ''
    )
) AS hash_b FROM products \gset

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

SELECT COUNT(*) AS count_b FROM products \gset
\connect cloudsync_test_17a
SELECT COUNT(*) AS count_a_orig FROM products \gset

\connect cloudsync_test_17b
SELECT (:count_b = :count_a_orig) AS row_counts_match \gset
\if :row_counts_match
\echo [PASS] (:testid) Row counts match (:count_b rows)
\else
\echo [FAIL] (:testid) Row counts mismatch - Database A: :count_a_orig, Database B: :count_b
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Verify UUID primary keys preserved
-- ============================================================================

SELECT COUNT(DISTINCT id) = 5 AS uuid_count_ok FROM products \gset
\if :uuid_count_ok
\echo [PASS] (:testid) UUID primary keys preserved
\else
\echo [FAIL] (:testid) UUID primary keys not all preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test specific UUID values
-- ============================================================================

SELECT COUNT(*) = 1 AS uuid_test_ok
FROM products
WHERE id = '550e8400-e29b-41d4-a716-446655440000'
  AND name = 'Product A'
  AND price = 99.99 \gset
\if :uuid_test_ok
\echo [PASS] (:testid) Specific UUID record verified
\else
\echo [FAIL] (:testid) Specific UUID record not found or incorrect
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test bidirectional sync (B -> A)
-- ============================================================================

\connect cloudsync_test_17b

INSERT INTO products VALUES ('7ba7b814-9dad-11d1-80b4-00c04fd430c8', 'From B', 77.77, 777, E'\\xBEEF');

SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_b_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_test_17a
SELECT cloudsync_payload_apply(decode(:'payload_b_hex', 'hex')) AS apply_b_to_a \gset

SELECT COUNT(*) = 1 AS bidirectional_ok
FROM products
WHERE id = '7ba7b814-9dad-11d1-80b4-00c04fd430c8'
  AND name = 'From B'
  AND price = 77.77 \gset
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
DROP DATABASE IF EXISTS cloudsync_test_17a;
DROP DATABASE IF EXISTS cloudsync_test_17b;
\endif
