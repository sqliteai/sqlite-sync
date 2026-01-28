-- Composite PK Roundtrip Test (TEXT + INTEGER)
-- Tests roundtrip with a composite primary key that mixes TEXT and INTEGER.

\set testid '16'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

-- Cleanup and create test databases
DROP DATABASE IF EXISTS cloudsync_test_16a;
DROP DATABASE IF EXISTS cloudsync_test_16b;
CREATE DATABASE cloudsync_test_16a;
CREATE DATABASE cloudsync_test_16b;

-- ============================================================================
-- Setup Database A with composite PK (TEXT + INTEGER)
-- ============================================================================

\connect cloudsync_test_16a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE mixed_pk (
    id_text TEXT NOT NULL,
    id_int INTEGER NOT NULL,
    PRIMARY KEY (id_text, id_int),

    col_text TEXT NOT NULL DEFAULT '',
    col_int INTEGER NOT NULL DEFAULT 0,
    col_float DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    col_blob BYTEA
);

-- Initialize CloudSync (skip int pk check for this test)
SELECT cloudsync_init('mixed_pk', 'CLS', true) AS _init_a \gset

-- ============================================================================
-- Insert test data
-- ============================================================================

INSERT INTO mixed_pk VALUES ('pkA', 1, 'hello', 42, 3.14, E'\\xDEADBEEF');
INSERT INTO mixed_pk VALUES ('pkA', 2, 'world', 7, 2.71, NULL);
INSERT INTO mixed_pk VALUES ('pkB', 1, '', 0, 0.0, E'\\x00');
INSERT INTO mixed_pk VALUES ('pkC', 10, 'edge', -1, -1.5, E'\\xCAFEBABE');
INSERT INTO mixed_pk VALUES ('pkD', 20, 'more', 999, 123.456, E'\\x010203');

-- ============================================================================
-- Compute hash of Database A data
-- ============================================================================

SELECT md5(
    COALESCE(
        string_agg(
            id_text || ':' || id_int::text || ':' ||
            COALESCE(col_text, 'NULL') || ':' ||
            COALESCE(col_int::text, 'NULL') || ':' ||
            COALESCE(col_float::text, 'NULL') || ':' ||
            COALESCE(encode(col_blob, 'hex'), 'NULL'),
            '|' ORDER BY id_text, id_int
        ),
        ''
    )
) AS hash_a FROM mixed_pk \gset

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

\connect cloudsync_test_16b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE mixed_pk (
    id_text TEXT NOT NULL,
    id_int INTEGER NOT NULL,
    PRIMARY KEY (id_text, id_int),
    col_text TEXT NOT NULL DEFAULT '',
    col_int INTEGER NOT NULL DEFAULT 0,
    col_float DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    col_blob BYTEA
);

-- Initialize CloudSync (skip int pk check for this test)
SELECT cloudsync_init('mixed_pk', 'CLS', true) AS _init_b \gset

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
            id_text || ':' || id_int::text || ':' ||
            COALESCE(col_text, 'NULL') || ':' ||
            COALESCE(col_int::text, 'NULL') || ':' ||
            COALESCE(col_float::text, 'NULL') || ':' ||
            COALESCE(encode(col_blob, 'hex'), 'NULL'),
            '|' ORDER BY id_text, id_int
        ),
        ''
    )
) AS hash_b FROM mixed_pk \gset

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

SELECT COUNT(*) AS count_b FROM mixed_pk \gset
\connect cloudsync_test_16a
SELECT COUNT(*) AS count_a_orig FROM mixed_pk \gset

\connect cloudsync_test_16b
SELECT (:count_b = :count_a_orig) AS row_counts_match \gset
\if :row_counts_match
\echo [PASS] (:testid) Row counts match (:count_b rows)
\else
\echo [FAIL] (:testid) Row counts mismatch - Database A: :count_a_orig, Database B: :count_b
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test composite primary key encoding
-- ============================================================================

SELECT COUNT(DISTINCT (id_text, id_int)) = 5 AS pk_count_ok FROM mixed_pk \gset
\if :pk_count_ok
\echo [PASS] (:testid) Composite primary keys preserved
\else
\echo [FAIL] (:testid) Composite primary keys not all preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test bidirectional sync (B -> A)
-- ============================================================================

\connect cloudsync_test_16b

INSERT INTO mixed_pk VALUES ('pkB', 99, 'from B', 123, 9.99, E'\\xBEEF');

SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_b_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_test_16a
SELECT cloudsync_payload_apply(decode(:'payload_b_hex', 'hex')) AS apply_b_to_a \gset

SELECT COUNT(*) = 1 AS bidirectional_ok
FROM mixed_pk
WHERE id_text = 'pkB' AND id_int = 99 AND col_text = 'from B' \gset
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
DROP DATABASE IF EXISTS cloudsync_test_16a;
DROP DATABASE IF EXISTS cloudsync_test_16b;
\endif
