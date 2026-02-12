-- 'Row-level filter (conditional sync) test'

\set testid '26'
\ir helper_test_init.sql

-- Create first database
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_26_a;
CREATE DATABASE cloudsync_test_26_a;

\connect cloudsync_test_26_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- Create table, init, set filter
CREATE TABLE tasks (id TEXT PRIMARY KEY NOT NULL, title TEXT, user_id INTEGER);
SELECT cloudsync_init('tasks') AS _init_site_id_a \gset
SELECT cloudsync_set_filter('tasks', 'user_id = 1') AS _set_filter_ok \gset

-- Insert matching rows (user_id = 1) and non-matching rows (user_id = 2)
INSERT INTO tasks VALUES ('a', 'Task A', 1);
INSERT INTO tasks VALUES ('b', 'Task B', 2);
INSERT INTO tasks VALUES ('c', 'Task C', 1);

-- Test 1: Verify only matching rows are tracked in _cloudsync metadata
SELECT COUNT(DISTINCT pk) AS meta_pk_count FROM tasks_cloudsync \gset
SELECT (:meta_pk_count = 2) AS filter_insert_ok \gset
\if :filter_insert_ok
\echo [PASS] (:testid) Only matching rows tracked after INSERT (2 of 3)
\else
\echo [FAIL] (:testid) Expected 2 tracked PKs after INSERT, got :meta_pk_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 2: Update non-matching row → no metadata change
SELECT COUNT(*) AS meta_before FROM tasks_cloudsync \gset
UPDATE tasks SET title = 'Task B Updated' WHERE id = 'b';
SELECT COUNT(*) AS meta_after FROM tasks_cloudsync \gset
SELECT (:meta_before = :meta_after) AS filter_update_nonmatch_ok \gset
\if :filter_update_nonmatch_ok
\echo [PASS] (:testid) Non-matching UPDATE did not change metadata
\else
\echo [FAIL] (:testid) Non-matching UPDATE changed metadata (:meta_before -> :meta_after)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 3: Delete non-matching row → no metadata change
SELECT COUNT(*) AS meta_before FROM tasks_cloudsync \gset
DELETE FROM tasks WHERE id = 'b';
SELECT COUNT(*) AS meta_after FROM tasks_cloudsync \gset
SELECT (:meta_before = :meta_after) AS filter_delete_nonmatch_ok \gset
\if :filter_delete_nonmatch_ok
\echo [PASS] (:testid) Non-matching DELETE did not change metadata
\else
\echo [FAIL] (:testid) Non-matching DELETE changed metadata (:meta_before -> :meta_after)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 4: Roundtrip - sync to second database, verify only filtered rows transfer
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_26_b;
CREATE DATABASE cloudsync_test_26_b;

\connect cloudsync_test_26_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE tasks (id TEXT PRIMARY KEY NOT NULL, title TEXT, user_id INTEGER);
SELECT cloudsync_init('tasks') AS _init_site_id_b \gset
SELECT cloudsync_set_filter('tasks', 'user_id = 1') AS _set_filter_b_ok \gset
SELECT cloudsync_payload_apply(decode(:'payload_hex', 'hex')) AS _apply_ok \gset

-- Verify: db2 should have only the matching rows
SELECT COUNT(*) AS task_count FROM tasks \gset
SELECT (:task_count = 2) AS roundtrip_count_ok \gset
\if :roundtrip_count_ok
\echo [PASS] (:testid) Roundtrip: correct number of rows synced (2)
\else
\echo [FAIL] (:testid) Roundtrip: expected 2 rows, got :task_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify row 'c' exists with user_id = 1
SELECT COUNT(*) AS c_exists FROM tasks WHERE id = 'c' AND user_id = 1 \gset
SELECT (:c_exists = 1) AS roundtrip_row_ok \gset
\if :roundtrip_row_ok
\echo [PASS] (:testid) Roundtrip: task 'c' with user_id=1 present
\else
\echo [FAIL] (:testid) Roundtrip: task 'c' with user_id=1 not found
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_26_a;
DROP DATABASE IF EXISTS cloudsync_test_26_b;
\else
\echo [INFO] !!!!!
\endif
