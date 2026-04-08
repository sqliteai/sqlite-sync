-- 'Row-level filter advanced tests (clear, complex expressions, row transitions, filter change)'

\set testid '47'
\ir helper_test_init.sql

-- Create database
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_47_a;
CREATE DATABASE cloudsync_test_47_a;

\connect cloudsync_test_47_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- ============================================================
-- Test 1: cloudsync_clear_filter lifecycle
-- ============================================================
CREATE TABLE tasks (id TEXT PRIMARY KEY NOT NULL, title TEXT, user_id INTEGER);
SELECT cloudsync_init('tasks') AS _init \gset
SELECT cloudsync_set_filter('tasks', 'user_id = 1') AS _sf \gset

INSERT INTO tasks VALUES ('a', 'Task A', 1);
INSERT INTO tasks VALUES ('b', 'Task B', 2);
INSERT INTO tasks VALUES ('c', 'Task C', 1);

-- Only matching rows tracked
SELECT COUNT(DISTINCT pk) AS meta_pk_count FROM tasks_cloudsync \gset
SELECT (:meta_pk_count = 2) AS clear_t1a_ok \gset
\if :clear_t1a_ok
\echo [PASS] (:testid) clear_filter: 2 PKs tracked before clear
\else
\echo [FAIL] (:testid) clear_filter: expected 2 tracked PKs before clear, got :meta_pk_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Clear filter
SELECT cloudsync_clear_filter('tasks') AS _cf \gset

-- Insert non-matching row — should now be tracked (no filter)
-- clear_filter refilled metatable with all 3 existing rows (a, b, c) + insert d = 4
INSERT INTO tasks VALUES ('d', 'Task D', 2);
SELECT COUNT(DISTINCT pk) AS meta_pk_count FROM tasks_cloudsync \gset
SELECT (:meta_pk_count = 4) AS clear_t1b_ok \gset
\if :clear_t1b_ok
\echo [PASS] (:testid) clear_filter: non-matching row tracked after clear (4 PKs)
\else
\echo [FAIL] (:testid) clear_filter: expected 4 PKs after clear+insert, got :meta_pk_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Update row 'b' — already tracked by clear_filter refill, meta count unchanged
UPDATE tasks SET title = 'Task B Updated' WHERE id = 'b';
SELECT COUNT(DISTINCT pk) AS meta_pk_count FROM tasks_cloudsync \gset
SELECT (:meta_pk_count = 4) AS clear_t1c_ok \gset
\if :clear_t1c_ok
\echo [PASS] (:testid) clear_filter: update on 'b' still 4 PKs
\else
\echo [FAIL] (:testid) clear_filter: expected 4 PKs after update on 'b', got :meta_pk_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 2: Complex filter — AND + comparison operators
-- ============================================================
DROP TABLE IF EXISTS items;
CREATE TABLE items (id TEXT PRIMARY KEY NOT NULL, status TEXT, priority INTEGER, category TEXT, user_id INTEGER);
SELECT cloudsync_init('items') AS _init \gset
SELECT cloudsync_set_filter('items', 'user_id = 1 AND priority > 3') AS _sf \gset

INSERT INTO items VALUES ('a', 'active', 5, 'work', 1);   -- matches
INSERT INTO items VALUES ('b', 'active', 2, 'work', 1);   -- fails priority
INSERT INTO items VALUES ('c', 'active', 5, 'work', 2);   -- fails user_id

SELECT COUNT(DISTINCT pk) AS meta_pk_count FROM items_cloudsync \gset
SELECT (:meta_pk_count = 1) AS complex_t2_ok \gset
\if :complex_t2_ok
\echo [PASS] (:testid) complex_filter: AND+comparison tracked 1 of 3 rows
\else
\echo [FAIL] (:testid) complex_filter: expected 1 tracked PK, got :meta_pk_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 3: IS NULL filter
-- ============================================================
SELECT cloudsync_clear_filter('items') AS _cf \gset
SELECT cloudsync_set_filter('items', 'category IS NULL') AS _sf \gset

SELECT COUNT(DISTINCT pk) AS meta_before FROM items_cloudsync \gset
INSERT INTO items VALUES ('f', 'x', 1, NULL, 1);       -- matches
INSERT INTO items VALUES ('g', 'x', 1, 'work', 1);     -- fails
SELECT COUNT(DISTINCT pk) AS meta_after FROM items_cloudsync \gset
SELECT ((:meta_after::int - :meta_before::int) = 1) AS null_t3_ok \gset
\if :null_t3_ok
\echo [PASS] (:testid) IS NULL filter: only NULL-category row tracked
\else
\echo [FAIL] (:testid) IS NULL filter: expected 1 new PK, got (:meta_after - :meta_before)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 4: Row exits filter (matching -> non-matching via UPDATE)
-- ============================================================
DROP TABLE IF EXISTS trans;
CREATE TABLE trans (id TEXT PRIMARY KEY NOT NULL, title TEXT, user_id INTEGER);
SELECT cloudsync_init('trans') AS _init \gset
SELECT cloudsync_set_filter('trans', 'user_id = 1') AS _sf \gset

INSERT INTO trans VALUES ('a', 'Task A', 1);

SELECT COUNT(*) AS meta_before FROM trans_cloudsync \gset
UPDATE trans SET user_id = 2 WHERE id = 'a';
SELECT COUNT(*) AS meta_after FROM trans_cloudsync \gset
SELECT (:meta_before = :meta_after) AS exit_t4_ok \gset
\if :exit_t4_ok
\echo [PASS] (:testid) row_exit: UPDATE out of filter did not change metadata
\else
\echo [FAIL] (:testid) row_exit: UPDATE out of filter changed metadata (:meta_before -> :meta_after)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 5: Row enters filter (non-matching -> matching via UPDATE)
-- ============================================================
INSERT INTO trans VALUES ('b', 'Task B', 2);

SELECT COUNT(DISTINCT pk) AS meta_before FROM trans_cloudsync \gset
UPDATE trans SET user_id = 1 WHERE id = 'b';
SELECT COUNT(DISTINCT pk) AS meta_after FROM trans_cloudsync \gset
SELECT (:meta_after::int > :meta_before::int) AS enter_t5_ok \gset
\if :enter_t5_ok
\echo [PASS] (:testid) row_enter: UPDATE into filter created metadata
\else
\echo [FAIL] (:testid) row_enter: UPDATE into filter did not create metadata (:meta_before -> :meta_after)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 6: Filter change after data
-- ============================================================
DROP TABLE IF EXISTS fchange;
CREATE TABLE fchange (id TEXT PRIMARY KEY NOT NULL, title TEXT, user_id INTEGER);
SELECT cloudsync_init('fchange') AS _init \gset
SELECT cloudsync_set_filter('fchange', 'user_id = 1') AS _sf \gset

INSERT INTO fchange VALUES ('a', 'A', 1);  -- matches
INSERT INTO fchange VALUES ('b', 'B', 2);  -- non-matching
INSERT INTO fchange VALUES ('c', 'C', 1);  -- matches

SELECT COUNT(DISTINCT pk) AS meta_count FROM fchange_cloudsync \gset
SELECT (:meta_count = 2) AS change_t6a_ok \gset
\if :change_t6a_ok
\echo [PASS] (:testid) filter_change: 2 PKs under initial filter
\else
\echo [FAIL] (:testid) filter_change: expected 2 PKs under initial filter, got :meta_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Change filter — resets metatable to only rows matching new filter (user_id = 2)
-- Only 'b' (user_id=2) matches new filter → 1 PK from refill, then insert d → 2
SELECT cloudsync_set_filter('fchange', 'user_id = 2') AS _sf2 \gset

INSERT INTO fchange VALUES ('d', 'D', 2);  -- matches new filter
INSERT INTO fchange VALUES ('e', 'E', 1);  -- non-matching under new filter

SELECT COUNT(DISTINCT pk) AS meta_count FROM fchange_cloudsync \gset
SELECT (:meta_count = 2) AS change_t6b_ok \gset
\if :change_t6b_ok
\echo [PASS] (:testid) filter_change: 2 PKs after filter change (metatable reset)
\else
\echo [FAIL] (:testid) filter_change: expected 2 PKs after filter change, got :meta_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Update 'a' (user_id=1) should NOT generate new metadata under new filter (user_id=2)
SELECT COUNT(*) AS meta_before FROM fchange_cloudsync \gset
UPDATE fchange SET title = 'A Updated' WHERE id = 'a';
SELECT COUNT(*) AS meta_after FROM fchange_cloudsync \gset
SELECT (:meta_before = :meta_after) AS change_t6c_ok \gset
\if :change_t6c_ok
\echo [PASS] (:testid) filter_change: update on 'a' not tracked under new filter
\else
\echo [FAIL] (:testid) filter_change: update on 'a' changed metadata (:meta_before -> :meta_after)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_47_a;
\else
\echo [INFO] !!!!!
\endif
