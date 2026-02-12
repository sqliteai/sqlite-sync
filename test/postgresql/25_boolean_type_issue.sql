-- Test: BOOLEAN Type Roundtrip
-- This test verifies that BOOLEAN columns sync correctly.
-- BOOLEAN values are encoded as INT8 in sync payloads. The cloudsync extension
-- provides a custom cast (bigint AS boolean) to enable this.
--
-- See plans/ANALYSIS_BOOLEAN_TYPE_CONVERSION.md for details.

\set testid '25'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

DROP DATABASE IF EXISTS cloudsync_test_25a;
DROP DATABASE IF EXISTS cloudsync_test_25b;
CREATE DATABASE cloudsync_test_25a;
CREATE DATABASE cloudsync_test_25b;

-- Setup Database A
\connect cloudsync_test_25a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE bool_test (
    id TEXT PRIMARY KEY NOT NULL,
    flag BOOLEAN,
    name TEXT
);

SELECT cloudsync_init('bool_test', 'CLS', true) AS _init_a \gset

-- Setup Database B
\connect cloudsync_test_25b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE bool_test (
    id TEXT PRIMARY KEY NOT NULL,
    flag BOOLEAN,
    name TEXT
);

SELECT cloudsync_init('bool_test', 'CLS', true) AS _init_b \gset

-- ============================================================================
-- STEP 1: Insert NULL BOOLEAN first (triggers SPI plan caching)
-- ============================================================================

\echo [INFO] (:testid) === STEP 1: NULL BOOLEAN ===

\connect cloudsync_test_25a
INSERT INTO bool_test (id, flag, name) VALUES ('row1', NULL, 'null_flag');

SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload1_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

SELECT max(db_version) AS db_version FROM bool_test_cloudsync \gset

\connect cloudsync_test_25b
SELECT cloudsync_payload_apply(decode(:'payload1_hex', 'hex')) AS apply1 \gset

SELECT (SELECT flag IS NULL AND name = 'null_flag' FROM bool_test WHERE id = 'row1') AS step1_ok \gset
\if :step1_ok
\echo [PASS] (:testid) Step 1: NULL BOOLEAN preserved
\else
\echo [FAIL] (:testid) Step 1: NULL BOOLEAN not preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- STEP 2: Insert TRUE BOOLEAN (tests INT8 -> BOOLEAN cast after NULL)
-- ============================================================================

\echo [INFO] (:testid) === STEP 2: TRUE BOOLEAN after NULL ===

\connect cloudsync_test_25a
INSERT INTO bool_test (id, flag, name) VALUES ('row2', true, 'true_flag');

SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload2_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() AND db_version > :db_version \gset

SELECT max(db_version) AS db_version FROM bool_test_cloudsync \gset

\connect cloudsync_test_25b
SELECT cloudsync_payload_apply(decode(:'payload2_hex', 'hex')) AS apply2 \gset

SELECT (SELECT flag = true AND name = 'true_flag' FROM bool_test WHERE id = 'row2') AS step2_ok \gset
\if :step2_ok
\echo [PASS] (:testid) Step 2: TRUE BOOLEAN preserved after NULL
\else
\echo [FAIL] (:testid) Step 2: TRUE BOOLEAN not preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- STEP 3: Insert FALSE BOOLEAN
-- ============================================================================

\echo [INFO] (:testid) === STEP 3: FALSE BOOLEAN ===

\connect cloudsync_test_25a
INSERT INTO bool_test (id, flag, name) VALUES ('row3', false, 'false_flag');

SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload3_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() AND db_version > :db_version \gset

SELECT max(db_version) AS db_version FROM bool_test_cloudsync \gset

\connect cloudsync_test_25b
SELECT cloudsync_payload_apply(decode(:'payload3_hex', 'hex')) AS apply3 \gset

SELECT (SELECT flag = false AND name = 'false_flag' FROM bool_test WHERE id = 'row3') AS step3_ok \gset
\if :step3_ok
\echo [PASS] (:testid) Step 3: FALSE BOOLEAN preserved
\else
\echo [FAIL] (:testid) Step 3: FALSE BOOLEAN not preserved
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- STEP 4: Update TRUE to FALSE
-- ============================================================================

\echo [INFO] (:testid) === STEP 4: Update TRUE to FALSE ===

\connect cloudsync_test_25a
UPDATE bool_test SET flag = false WHERE id = 'row2';

SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload4_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() AND db_version > :db_version \gset

SELECT max(db_version) AS db_version FROM bool_test_cloudsync \gset

\connect cloudsync_test_25b
SELECT cloudsync_payload_apply(decode(:'payload4_hex', 'hex')) AS apply4 \gset

SELECT (SELECT flag = false FROM bool_test WHERE id = 'row2') AS step4_ok \gset
\if :step4_ok
\echo [PASS] (:testid) Step 4: Update TRUE to FALSE synced
\else
\echo [FAIL] (:testid) Step 4: Update TRUE to FALSE not synced
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- STEP 5: Update NULL to TRUE
-- ============================================================================

\echo [INFO] (:testid) === STEP 5: Update NULL to TRUE ===

\connect cloudsync_test_25a
UPDATE bool_test SET flag = true WHERE id = 'row1';

SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload5_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() AND db_version > :db_version \gset

SELECT max(db_version) AS db_version FROM bool_test_cloudsync \gset

\connect cloudsync_test_25b
SELECT cloudsync_payload_apply(decode(:'payload5_hex', 'hex')) AS apply5 \gset

SELECT (SELECT flag = true FROM bool_test WHERE id = 'row1') AS step5_ok \gset
\if :step5_ok
\echo [PASS] (:testid) Step 5: Update NULL to TRUE synced
\else
\echo [FAIL] (:testid) Step 5: Update NULL to TRUE not synced
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- STEP 6: Verify final state with hash comparison
-- ============================================================================

\echo [INFO] (:testid) === STEP 6: Verify data integrity ===

\connect cloudsync_test_25a
SELECT md5(
    COALESCE(
        string_agg(
            id || ':' || COALESCE(flag::text, 'NULL') || ':' || COALESCE(name, 'NULL'),
            '|' ORDER BY id
        ),
        ''
    )
) AS hash_a FROM bool_test \gset

\connect cloudsync_test_25b
SELECT md5(
    COALESCE(
        string_agg(
            id || ':' || COALESCE(flag::text, 'NULL') || ':' || COALESCE(name, 'NULL'),
            '|' ORDER BY id
        ),
        ''
    )
) AS hash_b FROM bool_test \gset

SELECT (:'hash_a' = :'hash_b') AS hashes_match \gset
\if :hashes_match
\echo [PASS] (:testid) Data integrity verified - hashes match
\else
\echo [FAIL] (:testid) Data integrity check failed
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT COUNT(*) AS count_b FROM bool_test \gset
SELECT (:count_b = 3) AS count_ok \gset
\if :count_ok
\echo [PASS] (:testid) Row count correct (3 rows)
\else
\echo [FAIL] (:testid) Row count incorrect - expected 3, got :count_b
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
\connect postgres
DROP DATABASE IF EXISTS cloudsync_test_25a;
DROP DATABASE IF EXISTS cloudsync_test_25b;
\endif
