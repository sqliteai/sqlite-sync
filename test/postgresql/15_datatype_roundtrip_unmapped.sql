-- DBTYPE Roundtrip Test (Unmapped PostgreSQL Types)
-- Tests encoding/decoding for types that are not explicitly mapped to
-- DBTYPE_INTEGER/FLOAT/TEXT/BLOB/NULL in the common layer.

\set testid '15'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

-- Cleanup and create test databases
DROP DATABASE IF EXISTS cloudsync_test_15a;
DROP DATABASE IF EXISTS cloudsync_test_15b;
CREATE DATABASE cloudsync_test_15a;
CREATE DATABASE cloudsync_test_15b;

-- ============================================================================
-- Setup Database A with unmapped types table
-- ============================================================================

\connect cloudsync_test_15a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- Create table with composite primary key and unmapped types
CREATE TABLE unmapped_types (
    -- Composite primary key (TEXT columns as required by CloudSync)
    id1 TEXT NOT NULL,
    id2 TEXT NOT NULL,
    PRIMARY KEY (id1, id2),

    -- JSONB columns
    col_jsonb_notnull JSONB NOT NULL DEFAULT '{}'::jsonb,
    col_jsonb_nullable JSONB,

    -- UUID columns
    col_uuid_notnull UUID NOT NULL DEFAULT '00000000-0000-0000-0000-000000000000',
    col_uuid_nullable UUID,

    -- INET columns
    col_inet_notnull INET NOT NULL DEFAULT '0.0.0.0',
    col_inet_nullable INET,

    -- CIDR columns
    col_cidr_notnull CIDR NOT NULL DEFAULT '0.0.0.0/0',
    col_cidr_nullable CIDR,

    -- RANGE columns
    col_int4range_notnull INT4RANGE NOT NULL DEFAULT 'empty'::int4range,
    col_int4range_nullable INT4RANGE
);

-- Initialize CloudSync
SELECT cloudsync_init('unmapped_types', 'CLS', true) AS _init_a \gset

-- ============================================================================
-- Insert test data with various values for each type
-- ============================================================================

-- Row 1: All non-null values
INSERT INTO unmapped_types VALUES (
    'pk1', 'pk2',
    '{"a":1,"b":[1,2]}'::jsonb, '{"k":"v"}'::jsonb,
    '11111111-1111-1111-1111-111111111111', '22222222-2222-2222-2222-222222222222',
    '192.168.1.10', '10.1.2.3',
    '10.0.0.0/24', '192.168.0.0/16',
    '[1,10)', '[20,30)'
);

-- Row 2: Mix of null and non-null
INSERT INTO unmapped_types (
    id1, id2,
    col_jsonb_notnull,
    col_uuid_notnull,
    col_inet_notnull,
    col_cidr_notnull,
    col_int4range_notnull
) VALUES (
    'pk3', 'pk4',
    '{"only":"required"}'::jsonb,
    '33333333-3333-3333-3333-333333333333',
    '127.0.0.1',
    '127.0.0.0/8',
    '[0,1)'
);

-- Row 3: Edge cases - empty JSON, empty range
INSERT INTO unmapped_types VALUES (
    'pk5', 'pk6',
    '{}'::jsonb, '[]'::jsonb,
    '44444444-4444-4444-4444-444444444444', '55555555-5555-5555-5555-555555555555',
    '0.0.0.0', '255.255.255.255',
    '0.0.0.0/0', '255.255.255.0/24',
    'empty'::int4range, 'empty'::int4range
);

-- Row 4: IPv6 + negative range
INSERT INTO unmapped_types VALUES (
    'pk7', 'pk8',
    '{"ipv6":true}'::jsonb, '{"note":"range"}'::jsonb,
    '66666666-6666-6666-6666-666666666666', '77777777-7777-7777-7777-777777777777',
    '2001:db8::1', '2001:db8::2',
    '2001:db8::/32', '2001:db8:abcd::/48',
    '[-5,5]', '[-10,10)'
);

-- Row 5: Nested JSON
INSERT INTO unmapped_types VALUES (
    'pk9', 'pk10',
    '{"obj":{"x":1,"y":[2,3]}}'::jsonb, '{"arr":[{"a":1},{"b":2}]}'::jsonb,
    '88888888-8888-8888-8888-888888888888', '99999999-9999-9999-9999-999999999999',
    '172.16.0.1', '172.16.0.2',
    '172.16.0.0/12', '172.16.1.0/24',
    '[100,200)', '[200,300)'
);

-- ============================================================================
-- Compute hash of Database A data
-- ============================================================================

SELECT md5(
    COALESCE(
        string_agg(
            id1 || ':' || id2 || ':' ||
            COALESCE(col_jsonb_notnull::text, 'NULL') || ':' ||
            COALESCE(col_jsonb_nullable::text, 'NULL') || ':' ||
            COALESCE(col_uuid_notnull::text, 'NULL') || ':' ||
            COALESCE(col_uuid_nullable::text, 'NULL') || ':' ||
            COALESCE(col_inet_notnull::text, 'NULL') || ':' ||
            COALESCE(col_inet_nullable::text, 'NULL') || ':' ||
            COALESCE(col_cidr_notnull::text, 'NULL') || ':' ||
            COALESCE(col_cidr_nullable::text, 'NULL') || ':' ||
            COALESCE(col_int4range_notnull::text, 'NULL') || ':' ||
            COALESCE(col_int4range_nullable::text, 'NULL'),
            '|' ORDER BY id1, id2
        ),
        ''
    )
) AS hash_a FROM unmapped_types \gset

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

\connect cloudsync_test_15b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- Create identical table schema
CREATE TABLE unmapped_types (
    id1 TEXT NOT NULL,
    id2 TEXT NOT NULL,
    PRIMARY KEY (id1, id2),
    col_jsonb_notnull JSONB NOT NULL DEFAULT '{}'::jsonb,
    col_jsonb_nullable JSONB,
    col_uuid_notnull UUID NOT NULL DEFAULT '00000000-0000-0000-0000-000000000000',
    col_uuid_nullable UUID,
    col_inet_notnull INET NOT NULL DEFAULT '0.0.0.0',
    col_inet_nullable INET,
    col_cidr_notnull CIDR NOT NULL DEFAULT '0.0.0.0/0',
    col_cidr_nullable CIDR,
    col_int4range_notnull INT4RANGE NOT NULL DEFAULT 'empty'::int4range,
    col_int4range_nullable INT4RANGE
);

-- Initialize CloudSync
SELECT cloudsync_init('unmapped_types', 'CLS', true) AS _init_b \gset

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
            COALESCE(col_jsonb_notnull::text, 'NULL') || ':' ||
            COALESCE(col_jsonb_nullable::text, 'NULL') || ':' ||
            COALESCE(col_uuid_notnull::text, 'NULL') || ':' ||
            COALESCE(col_uuid_nullable::text, 'NULL') || ':' ||
            COALESCE(col_inet_notnull::text, 'NULL') || ':' ||
            COALESCE(col_inet_nullable::text, 'NULL') || ':' ||
            COALESCE(col_cidr_notnull::text, 'NULL') || ':' ||
            COALESCE(col_cidr_nullable::text, 'NULL') || ':' ||
            COALESCE(col_int4range_notnull::text, 'NULL') || ':' ||
            COALESCE(col_int4range_nullable::text, 'NULL'),
            '|' ORDER BY id1, id2
        ),
        ''
    )
) AS hash_b FROM unmapped_types \gset

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

SELECT COUNT(*) AS count_b FROM unmapped_types \gset
\connect cloudsync_test_15a
SELECT COUNT(*) AS count_a_orig FROM unmapped_types \gset

\connect cloudsync_test_15b
SELECT (:count_b = :count_a_orig) AS row_counts_match \gset
\if :row_counts_match
\echo [PASS] (:testid) Row counts match (:count_b rows)
\else
\echo [FAIL] (:testid) Row counts mismatch - Database A: :count_a_orig, Database B: :count_b
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test specific data type preservation
-- ============================================================================

-- JSONB values
SELECT
    (SELECT col_jsonb_notnull FROM unmapped_types WHERE id1 = 'pk1' AND id2 = 'pk2') = '{"a":1,"b":[1,2]}'::jsonb AND
    (SELECT col_jsonb_nullable FROM unmapped_types WHERE id1 = 'pk1' AND id2 = 'pk2') = '{"k":"v"}'::jsonb AND
    (SELECT col_jsonb_nullable FROM unmapped_types WHERE id1 = 'pk3' AND id2 = 'pk4') IS NULL
AS jsonb_ok \gset
\if :jsonb_ok
\echo [PASS] (:testid) JSONB type preservation
\else
\echo [FAIL] (:testid) JSONB type preservation
SELECT (:fail::int + 1) AS fail \gset
\endif

-- UUID values
SELECT
    (SELECT col_uuid_notnull FROM unmapped_types WHERE id1 = 'pk1' AND id2 = 'pk2') = '11111111-1111-1111-1111-111111111111'::uuid AND
    (SELECT col_uuid_nullable FROM unmapped_types WHERE id1 = 'pk1' AND id2 = 'pk2') = '22222222-2222-2222-2222-222222222222'::uuid AND
    (SELECT col_uuid_nullable FROM unmapped_types WHERE id1 = 'pk3' AND id2 = 'pk4') IS NULL
AS uuid_ok \gset
\if :uuid_ok
\echo [PASS] (:testid) UUID type preservation
\else
\echo [FAIL] (:testid) UUID type preservation
SELECT (:fail::int + 1) AS fail \gset
\endif

-- INET values
SELECT
    (SELECT col_inet_notnull FROM unmapped_types WHERE id1 = 'pk1' AND id2 = 'pk2') = '192.168.1.10'::inet AND
    (SELECT col_inet_nullable FROM unmapped_types WHERE id1 = 'pk1' AND id2 = 'pk2') = '10.1.2.3'::inet AND
    (SELECT col_inet_nullable FROM unmapped_types WHERE id1 = 'pk3' AND id2 = 'pk4') IS NULL
AS inet_ok \gset
\if :inet_ok
\echo [PASS] (:testid) INET type preservation
\else
\echo [FAIL] (:testid) INET type preservation
SELECT (:fail::int + 1) AS fail \gset
\endif

-- CIDR values
SELECT
    (SELECT col_cidr_notnull FROM unmapped_types WHERE id1 = 'pk1' AND id2 = 'pk2') = '10.0.0.0/24'::cidr AND
    (SELECT col_cidr_nullable FROM unmapped_types WHERE id1 = 'pk1' AND id2 = 'pk2') = '192.168.0.0/16'::cidr AND
    (SELECT col_cidr_nullable FROM unmapped_types WHERE id1 = 'pk3' AND id2 = 'pk4') IS NULL
AS cidr_ok \gset
\if :cidr_ok
\echo [PASS] (:testid) CIDR type preservation
\else
\echo [FAIL] (:testid) CIDR type preservation
SELECT (:fail::int + 1) AS fail \gset
\endif

-- RANGE values
SELECT
    (SELECT col_int4range_notnull FROM unmapped_types WHERE id1 = 'pk1' AND id2 = 'pk2') = '[1,10)'::int4range AND
    (SELECT col_int4range_nullable FROM unmapped_types WHERE id1 = 'pk1' AND id2 = 'pk2') = '[20,30)'::int4range AND
    (SELECT col_int4range_nullable FROM unmapped_types WHERE id1 = 'pk3' AND id2 = 'pk4') IS NULL
AS ranges_ok \gset
\if :ranges_ok
\echo [PASS] (:testid) RANGE type preservation
\else
\echo [FAIL] (:testid) RANGE type preservation
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test composite primary key encoding
-- ============================================================================

-- Verify all primary key combinations are present
SELECT COUNT(DISTINCT (id1, id2)) = 5 AS pk_count_ok FROM unmapped_types \gset
\if :pk_count_ok
\echo [PASS] (:testid) Composite primary keys preserved
\else
\echo [FAIL] (:testid) Composite primary keys not all preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test bidirectional sync (B -> A)
-- ============================================================================

\connect cloudsync_test_15b

-- Add a new row in Database B
INSERT INTO unmapped_types VALUES (
    'pkB1', 'pkB2',
    '{"from":"database B"}'::jsonb, '{"bidirectional":true}'::jsonb,
    'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa', 'bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb',
    '10.10.10.10', '10.10.10.11',
    '10.10.0.0/16', '10.10.10.0/24',
    '[50,60)', '[60,70)'
);

-- Encode payload from Database B
SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_b_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

-- Apply to Database A
\connect cloudsync_test_15a
SELECT cloudsync_payload_apply(decode(:'payload_b_hex', 'hex')) AS apply_b_to_a \gset

-- Verify the new row exists in Database A
SELECT COUNT(*) = 1 AS bidirectional_ok
FROM unmapped_types
WHERE id1 = 'pkB1' AND id2 = 'pkB2' AND col_jsonb_notnull = '{"from":"database B"}'::jsonb \gset
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
DROP DATABASE IF EXISTS cloudsync_test_15a;
DROP DATABASE IF EXISTS cloudsync_test_15b;
\endif
