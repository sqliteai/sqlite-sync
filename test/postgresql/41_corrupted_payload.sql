-- Test corrupted payload handling
-- Verifies that cloudsync_payload_apply rejects corrupted payloads
-- without crashing or corrupting state.

\set testid '41'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_41_src;
DROP DATABASE IF EXISTS cloudsync_test_41_dst;
CREATE DATABASE cloudsync_test_41_src;
CREATE DATABASE cloudsync_test_41_dst;

-- Setup source database with data
\connect cloudsync_test_41_src
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE test_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('test_tbl', 'CLS', true) AS _init_src \gset
INSERT INTO test_tbl VALUES ('id1', 'value1');
INSERT INTO test_tbl VALUES ('id2', 'value2');

-- Get a valid payload
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS valid_payload_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

-- Setup destination database
\connect cloudsync_test_41_dst
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE test_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('test_tbl', 'CLS', true) AS _init_dst \gset

-- Record initial state
SELECT COUNT(*) AS initial_count FROM test_tbl \gset

-- Test 1: Empty blob (zero bytes)
DO $$
BEGIN
  PERFORM cloudsync_payload_apply(''::bytea);
  -- If it returns without error with 0 rows, that's also acceptable
EXCEPTION WHEN OTHERS THEN
  -- Expected: error on empty payload
  NULL;
END $$;

SELECT COUNT(*) AS count_after_empty FROM test_tbl \gset
SELECT (:count_after_empty::int = :initial_count::int) AS empty_blob_ok \gset
\if :empty_blob_ok
\echo [PASS] (:testid) Empty blob rejected - table unchanged
\else
\echo [FAIL] (:testid) Empty blob corrupted table state
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 2: Random garbage bytes
DO $$
BEGIN
  PERFORM cloudsync_payload_apply(decode('deadbeefcafebabe0102030405060708', 'hex'));
EXCEPTION WHEN OTHERS THEN
  -- Expected: error on garbage payload
  NULL;
END $$;

SELECT COUNT(*) AS count_after_garbage FROM test_tbl \gset
SELECT (:count_after_garbage::int = :initial_count::int) AS garbage_ok \gset
\if :garbage_ok
\echo [PASS] (:testid) Garbage bytes rejected - table unchanged
\else
\echo [FAIL] (:testid) Garbage bytes corrupted table state
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 3: Truncated payload (first 10 bytes of valid payload)
-- Build truncated hex at top level using psql variable interpolation
SELECT substr(:'valid_payload_hex', 1, 20) AS truncated_hex \gset
SELECT cloudsync_payload_apply(decode(:'truncated_hex', 'hex')) AS _apply_truncated \gset
-- If the above errors, psql continues (ON_ERROR_STOP is off)

SELECT COUNT(*) AS count_after_truncated FROM test_tbl \gset
SELECT (:count_after_truncated::int = :initial_count::int) AS truncated_ok \gset
\if :truncated_ok
\echo [PASS] (:testid) Truncated payload rejected - table unchanged
\else
\echo [FAIL] (:testid) Truncated payload corrupted table state
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 4: Valid payload with flipped byte in the middle
-- Compute corrupted payload at top level: flip one byte via XOR with FF
SELECT
  substr(:'valid_payload_hex', 1, length(:'valid_payload_hex') / 2 - 1)
  || lpad(to_hex(get_byte(decode(substr(:'valid_payload_hex', length(:'valid_payload_hex') / 2, 2), 'hex'), 0) # 255), 2, '0')
  || substr(:'valid_payload_hex', length(:'valid_payload_hex') / 2 + 2)
  AS corrupted_hex \gset
SELECT cloudsync_payload_apply(decode(:'corrupted_hex', 'hex')) AS _apply_corrupted \gset
-- If the above errors, psql continues (ON_ERROR_STOP is off)

SELECT COUNT(*) AS count_after_flipped FROM test_tbl \gset
SELECT (:count_after_flipped::int = :initial_count::int) AS flipped_ok \gset
\if :flipped_ok
\echo [PASS] (:testid) Flipped-byte payload rejected - table unchanged
\else
\echo [FAIL] (:testid) Flipped-byte payload corrupted table state
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 5: Now apply the VALID payload to confirm it still works
SELECT cloudsync_payload_apply(decode(:'valid_payload_hex', 'hex')) AS valid_apply \gset
SELECT COUNT(*) AS count_after_valid FROM test_tbl \gset
SELECT (:count_after_valid::int = 2) AS valid_ok \gset
\if :valid_ok
\echo [PASS] (:testid) Valid payload applied successfully after corrupted attempts
\else
\echo [FAIL] (:testid) Valid payload failed after corrupted attempts - count: :count_after_valid
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_41_src;
DROP DATABASE IF EXISTS cloudsync_test_41_dst;
\endif
