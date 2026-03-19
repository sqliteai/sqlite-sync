-- Test PostgreSQL-specific type roundtrips
-- Covers JSONB, TIMESTAMPTZ, NUMERIC with precision, BYTEA

\set testid '45'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_45_a;
DROP DATABASE IF EXISTS cloudsync_test_45_b;
CREATE DATABASE cloudsync_test_45_a;
CREATE DATABASE cloudsync_test_45_b;

-- Setup Database A
\connect cloudsync_test_45_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE typed_tbl (
    id TEXT PRIMARY KEY,
    json_col JSONB,
    ts_col TIMESTAMPTZ,
    num_col NUMERIC(18, 6),
    bin_col BYTEA
);

SELECT cloudsync_init('typed_tbl', 'CLS', true) AS _init_a \gset

INSERT INTO typed_tbl VALUES (
    'row1',
    '{"key": "value", "nested": {"arr": [1, 2, 3]}}',
    '2025-01-15 10:30:00+00',
    123456.789012,
    '\x48656c6c6f'
);

INSERT INTO typed_tbl VALUES (
    'row2',
    '[1, "two", null, true, false]',
    '2024-06-30 23:59:59.999999+05:30',
    -999999.123456,
    '\xdeadbeef'
);

INSERT INTO typed_tbl VALUES (
    'row3',
    'null',
    '1970-01-01 00:00:00+00',
    0.000000,
    '\x'
);

-- Setup Database B
\connect cloudsync_test_45_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE typed_tbl (
    id TEXT PRIMARY KEY,
    json_col JSONB,
    ts_col TIMESTAMPTZ,
    num_col NUMERIC(18, 6),
    bin_col BYTEA
);

SELECT cloudsync_init('typed_tbl', 'CLS', true) AS _init_b \gset

-- Encode and apply A -> B
\connect cloudsync_test_45_a
\ir helper_psql_conn_setup.sql
SELECT cloudsync_init('typed_tbl', 'CLS', true) AS _reinit \gset
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_a
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_test_45_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_init('typed_tbl', 'CLS', true) AS _reinit \gset
SELECT cloudsync_payload_apply(decode(:'payload_a', 'hex')) AS apply_a_to_b \gset

-- Verify row count
SELECT COUNT(*) AS count_b FROM typed_tbl \gset
SELECT (:count_b::int = 3) AS count_ok \gset
\if :count_ok
\echo [PASS] (:testid) All 3 rows synced to B
\else
\echo [FAIL] (:testid) Expected 3 rows, got :count_b
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify JSONB roundtrip
SELECT COUNT(*) = 1 AS jsonb_ok
FROM typed_tbl
WHERE id = 'row1'
  AND json_col @> '{"key": "value"}'
  AND json_col -> 'nested' -> 'arr' = '[1, 2, 3]'::jsonb \gset
\if :jsonb_ok
\echo [PASS] (:testid) JSONB roundtrip correct
\else
\echo [FAIL] (:testid) JSONB data mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify TIMESTAMPTZ roundtrip
SELECT COUNT(*) = 1 AS ts_ok
FROM typed_tbl
WHERE id = 'row1'
  AND ts_col = '2025-01-15 10:30:00+00'::timestamptz \gset
\if :ts_ok
\echo [PASS] (:testid) TIMESTAMPTZ roundtrip correct
\else
\echo [FAIL] (:testid) TIMESTAMPTZ data mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify NUMERIC roundtrip
SELECT COUNT(*) = 1 AS num_ok
FROM typed_tbl
WHERE id = 'row1'
  AND num_col = 123456.789012 \gset
\if :num_ok
\echo [PASS] (:testid) NUMERIC(18,6) roundtrip correct
\else
\echo [FAIL] (:testid) NUMERIC data mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify BYTEA roundtrip
SELECT COUNT(*) = 1 AS bytea_ok
FROM typed_tbl
WHERE id = 'row1'
  AND bin_col = '\x48656c6c6f'::bytea \gset
\if :bytea_ok
\echo [PASS] (:testid) BYTEA roundtrip correct
\else
\echo [FAIL] (:testid) BYTEA data mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify full hash match
\connect cloudsync_test_45_a
\ir helper_psql_conn_setup.sql
SELECT md5(COALESCE(string_agg(
    id || ':' ||
    COALESCE(json_col::text, 'NULL') || ':' ||
    COALESCE(ts_col::text, 'NULL') || ':' ||
    COALESCE(num_col::text, 'NULL') || ':' ||
    COALESCE(encode(bin_col, 'hex'), 'NULL'),
    '|' ORDER BY id
), '')) AS hash_a FROM typed_tbl \gset

\connect cloudsync_test_45_b
\ir helper_psql_conn_setup.sql
SELECT md5(COALESCE(string_agg(
    id || ':' ||
    COALESCE(json_col::text, 'NULL') || ':' ||
    COALESCE(ts_col::text, 'NULL') || ':' ||
    COALESCE(num_col::text, 'NULL') || ':' ||
    COALESCE(encode(bin_col, 'hex'), 'NULL'),
    '|' ORDER BY id
), '')) AS hash_b FROM typed_tbl \gset

SELECT (:'hash_a' = :'hash_b') AS hash_match \gset
\if :hash_match
\echo [PASS] (:testid) Full data hash matches for PG-specific types
\else
\echo [FAIL] (:testid) Hash mismatch - A: :hash_a, B: :hash_b
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_45_a;
DROP DATABASE IF EXISTS cloudsync_test_45_b;
\endif
