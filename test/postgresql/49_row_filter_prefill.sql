-- 'Row-level filter with pre-existing data (prefill tests)'
-- Tests that cloudsync_refill_metatable correctly handles the filter
-- when rows exist before cloudsync_init and cloudsync_set_filter.

\set testid '49'
\ir helper_test_init.sql

-- Create databases
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_49_a;
DROP DATABASE IF EXISTS cloudsync_test_49_b;
CREATE DATABASE cloudsync_test_49_a;
CREATE DATABASE cloudsync_test_49_b;

-- ============================================================
-- Setup Database A — insert data BEFORE cloudsync_init
-- ============================================================

\connect cloudsync_test_49_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE tasks (id TEXT PRIMARY KEY NOT NULL, title TEXT, user_id INTEGER);

-- Pre-existing rows: 3 matching (user_id=1), 2 non-matching
INSERT INTO tasks VALUES ('a', 'Task A', 1);
INSERT INTO tasks VALUES ('b', 'Task B', 2);
INSERT INTO tasks VALUES ('c', 'Task C', 1);
INSERT INTO tasks VALUES ('d', 'Task D', 3);
INSERT INTO tasks VALUES ('e', 'Task E', 1);

-- Init and set filter AFTER data exists
SELECT cloudsync_init('tasks') AS _init_a \gset
SELECT cloudsync_set_filter('tasks', 'user_id = 1') AS _sf_a \gset

-- ============================================================
-- Test 1: Init refill should have created metadata for ALL 5 pre-existing rows
-- (filter was not set in settings when cloudsync_init ran the refill)
-- ============================================================

SELECT COUNT(DISTINCT pk) AS meta_pk_count FROM tasks_cloudsync \gset
SELECT (:meta_pk_count = 5) AS prefill_t1_ok \gset
\if :prefill_t1_ok
\echo [PASS] (:testid) prefill: all 5 pre-existing rows have metadata after init
\else
\echo [FAIL] (:testid) prefill: expected 5 tracked PKs after init, got :meta_pk_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 2: New matching insert IS tracked
-- ============================================================

INSERT INTO tasks VALUES ('f', 'Task F', 1);
SELECT COUNT(DISTINCT pk) AS meta_pk_count FROM tasks_cloudsync \gset
SELECT (:meta_pk_count = 6) AS prefill_t2_ok \gset
\if :prefill_t2_ok
\echo [PASS] (:testid) prefill: new matching insert tracked (6 PKs)
\else
\echo [FAIL] (:testid) prefill: expected 6 PKs after matching insert, got :meta_pk_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 3: New non-matching insert is NOT tracked
-- ============================================================

INSERT INTO tasks VALUES ('g', 'Task G', 2);
SELECT COUNT(DISTINCT pk) AS meta_pk_count FROM tasks_cloudsync \gset
SELECT (:meta_pk_count = 6) AS prefill_t3_ok \gset
\if :prefill_t3_ok
\echo [PASS] (:testid) prefill: new non-matching insert not tracked (still 6 PKs)
\else
\echo [FAIL] (:testid) prefill: expected still 6 PKs after non-matching insert, got :meta_pk_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 4: Sync roundtrip — pre-existing + matching new rows transfer
-- ============================================================

SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_a_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

-- Setup Database B (empty)
\connect cloudsync_test_49_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE tasks (id TEXT PRIMARY KEY NOT NULL, title TEXT, user_id INTEGER);
SELECT cloudsync_init('tasks') AS _init_b \gset
SELECT cloudsync_set_filter('tasks', 'user_id = 1') AS _sf_b \gset

-- Apply payload
SELECT cloudsync_payload_apply(decode(:'payload_a_hex', 'hex')) AS _apply \gset

-- All pre-existing rows (a-e) + matching new (f) should arrive; non-matching new (g) should not
SELECT COUNT(*) AS row_count FROM tasks \gset
SELECT (:row_count = 6) AS prefill_t4a_ok \gset
\if :prefill_t4a_ok
\echo [PASS] (:testid) prefill_sync: 6 rows synced to Database B
\else
\echo [FAIL] (:testid) prefill_sync: expected 6 rows in Database B, got :row_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify non-matching post-filter row 'g' did NOT sync
SELECT COUNT(*) AS g_count FROM tasks WHERE id = 'g' \gset
SELECT (:g_count = 0) AS prefill_t4b_ok \gset
\if :prefill_t4b_ok
\echo [PASS] (:testid) prefill_sync: non-matching post-filter row 'g' not synced
\else
\echo [FAIL] (:testid) prefill_sync: row 'g' should not have synced
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify pre-existing non-matching row 'b' DID sync (metadata from init refill)
SELECT COUNT(*) AS b_count FROM tasks WHERE id = 'b' AND user_id = 2 \gset
SELECT (:b_count = 1) AS prefill_t4c_ok \gset
\if :prefill_t4c_ok
\echo [PASS] (:testid) prefill_sync: pre-existing non-matching row 'b' synced via refill
\else
\echo [FAIL] (:testid) prefill_sync: pre-existing row 'b' should have synced via refill metadata
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 5: Composite PK with pre-existing data
-- ============================================================

\connect cloudsync_test_49_a

CREATE TABLE projects (
    org_id INTEGER NOT NULL,
    proj_id INTEGER NOT NULL,
    name TEXT,
    status TEXT,
    PRIMARY KEY(org_id, proj_id)
);

-- Pre-existing rows: 2 matching (org_id=1), 2 non-matching
INSERT INTO projects VALUES (1, 1, 'Alpha', 'active');
INSERT INTO projects VALUES (1, 2, 'Beta', 'active');
INSERT INTO projects VALUES (2, 1, 'Gamma', 'active');
INSERT INTO projects VALUES (2, 2, 'Delta', 'active');

SELECT cloudsync_init('projects') AS _init_proj \gset
SELECT cloudsync_set_filter('projects', 'org_id = 1') AS _sf_proj \gset

-- All 4 pre-existing rows should have metadata
SELECT COUNT(DISTINCT pk) AS meta_pk_count FROM projects_cloudsync \gset
SELECT (:meta_pk_count = 4) AS prefill_t5_ok \gset
\if :prefill_t5_ok
\echo [PASS] (:testid) prefill_composite: all 4 pre-existing rows have metadata
\else
\echo [FAIL] (:testid) prefill_composite: expected 4 tracked PKs, got :meta_pk_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- New matching insert tracked
INSERT INTO projects VALUES (1, 3, 'Epsilon', 'active');
SELECT COUNT(DISTINCT pk) AS meta_pk_count FROM projects_cloudsync \gset
SELECT (:meta_pk_count = 5) AS prefill_t5b_ok \gset
\if :prefill_t5b_ok
\echo [PASS] (:testid) prefill_composite: new matching row tracked (5 PKs)
\else
\echo [FAIL] (:testid) prefill_composite: expected 5 PKs after matching insert, got :meta_pk_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- New non-matching insert NOT tracked
INSERT INTO projects VALUES (3, 1, 'Zeta', 'active');
SELECT COUNT(DISTINCT pk) AS meta_pk_count FROM projects_cloudsync \gset
SELECT (:meta_pk_count = 5) AS prefill_t5c_ok \gset
\if :prefill_t5c_ok
\echo [PASS] (:testid) prefill_composite: new non-matching row not tracked (still 5 PKs)
\else
\echo [FAIL] (:testid) prefill_composite: expected still 5 PKs, got :meta_pk_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_49_a;
DROP DATABASE IF EXISTS cloudsync_test_49_b;
\endif
