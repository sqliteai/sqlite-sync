-- Test large composite primary key (5 columns)
-- Verifies pk_encode/pk_decode handles complex multi-column PKs correctly.

\set testid '44'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_44_a;
DROP DATABASE IF EXISTS cloudsync_test_44_b;
CREATE DATABASE cloudsync_test_44_a;
CREATE DATABASE cloudsync_test_44_b;

-- Setup Database A
\connect cloudsync_test_44_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE composite_pk_tbl (
    pk_text1 TEXT NOT NULL,
    pk_int1 INTEGER NOT NULL,
    pk_text2 TEXT NOT NULL,
    pk_int2 INTEGER NOT NULL,
    pk_text3 TEXT NOT NULL,
    data_col TEXT,
    num_col INTEGER,
    PRIMARY KEY (pk_text1, pk_int1, pk_text2, pk_int2, pk_text3)
);

SELECT cloudsync_init('composite_pk_tbl', 'CLS', true) AS _init_a \gset

INSERT INTO composite_pk_tbl VALUES ('alpha', 1, 'beta', 100, 'gamma', 'data_a1', 42);
INSERT INTO composite_pk_tbl VALUES ('alpha', 2, 'beta', 200, 'delta', 'data_a2', 84);
INSERT INTO composite_pk_tbl VALUES ('x', 999, 'y', -1, 'z', 'edge_case', 0);

-- Setup Database B
\connect cloudsync_test_44_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE composite_pk_tbl (
    pk_text1 TEXT NOT NULL,
    pk_int1 INTEGER NOT NULL,
    pk_text2 TEXT NOT NULL,
    pk_int2 INTEGER NOT NULL,
    pk_text3 TEXT NOT NULL,
    data_col TEXT,
    num_col INTEGER,
    PRIMARY KEY (pk_text1, pk_int1, pk_text2, pk_int2, pk_text3)
);

SELECT cloudsync_init('composite_pk_tbl', 'CLS', true) AS _init_b \gset

INSERT INTO composite_pk_tbl VALUES ('alpha', 1, 'beta', 100, 'gamma', 'data_b1', 99);
INSERT INTO composite_pk_tbl VALUES ('foo', 3, 'bar', 300, 'baz', 'data_b2', 77);

-- Encode and exchange payloads
\connect cloudsync_test_44_a
\ir helper_psql_conn_setup.sql
SELECT cloudsync_init('composite_pk_tbl', 'CLS', true) AS _reinit \gset
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_a
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_test_44_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_init('composite_pk_tbl', 'CLS', true) AS _reinit \gset
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_b
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

-- Apply A -> B
SELECT cloudsync_payload_apply(decode(:'payload_a', 'hex')) AS apply_a_to_b \gset

-- Apply B -> A
\connect cloudsync_test_44_a
\ir helper_psql_conn_setup.sql
SELECT cloudsync_init('composite_pk_tbl', 'CLS', true) AS _reinit \gset
SELECT cloudsync_payload_apply(decode(:'payload_b', 'hex')) AS apply_b_to_a \gset

-- Update a row on A
UPDATE composite_pk_tbl SET data_col = 'updated_on_a' WHERE pk_text1 = 'foo' AND pk_int1 = 3 AND pk_text2 = 'bar' AND pk_int2 = 300 AND pk_text3 = 'baz';

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_a2
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_test_44_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_init('composite_pk_tbl', 'CLS', true) AS _reinit \gset
SELECT cloudsync_payload_apply(decode(:'payload_a2', 'hex')) AS apply_a2_to_b \gset

-- Final hash comparison
SELECT md5(COALESCE(string_agg(
    pk_text1 || ':' || pk_int1::text || ':' || pk_text2 || ':' || pk_int2::text || ':' || pk_text3 || ':' ||
    COALESCE(data_col, 'NULL') || ':' || COALESCE(num_col::text, 'NULL'),
    '|' ORDER BY pk_text1, pk_int1, pk_text2, pk_int2, pk_text3
), '')) AS hash_b FROM composite_pk_tbl \gset

\connect cloudsync_test_44_a
\ir helper_psql_conn_setup.sql
SELECT md5(COALESCE(string_agg(
    pk_text1 || ':' || pk_int1::text || ':' || pk_text2 || ':' || pk_int2::text || ':' || pk_text3 || ':' ||
    COALESCE(data_col, 'NULL') || ':' || COALESCE(num_col::text, 'NULL'),
    '|' ORDER BY pk_text1, pk_int1, pk_text2, pk_int2, pk_text3
), '')) AS hash_a FROM composite_pk_tbl \gset

SELECT (:'hash_a' = :'hash_b') AS hashes_match \gset
\if :hashes_match
\echo [PASS] (:testid) Large composite PK (5 cols) roundtrip and update
\else
\echo [FAIL] (:testid) Hash mismatch - A: :hash_a, B: :hash_b
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify row count
SELECT COUNT(*) AS row_count FROM composite_pk_tbl \gset
SELECT (:row_count::int = 4) AS count_ok \gset
\if :count_ok
\echo [PASS] (:testid) Row count correct (4 rows with 5-col composite PK)
\else
\echo [FAIL] (:testid) Expected 4 rows, got :row_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify update propagated
SELECT COUNT(*) = 1 AS update_ok
FROM composite_pk_tbl
WHERE pk_text1 = 'foo' AND pk_int1 = 3 AND data_col = 'updated_on_a' \gset
\if :update_ok
\echo [PASS] (:testid) Update propagated correctly for composite PK row
\else
\echo [FAIL] (:testid) Update not propagated
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_44_a;
DROP DATABASE IF EXISTS cloudsync_test_44_b;
\endif
