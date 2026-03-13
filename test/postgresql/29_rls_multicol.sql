-- 'RLS multi-column batch merge test'
-- Extends test 27 with more column types (INTEGER, BOOLEAN) and additional
-- test cases: update-denied, mixed payloads (per-PK savepoint isolation),
-- and NULL handling.
--
-- Tests 1-2: superuser (service-role pattern)
-- Tests 3-8: authenticated-role pattern

\set testid '29'
\ir helper_test_init.sql

\set USER1 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa'
\set USER2 'bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb'

-- ============================================================
-- DB A: source database (no RLS)
-- ============================================================
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_29_a;
CREATE DATABASE cloudsync_test_29_a;

\connect cloudsync_test_29_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE tasks (
    id          TEXT PRIMARY KEY NOT NULL,
    user_id     UUID,
    title       TEXT,
    description TEXT,
    priority    INTEGER,
    is_complete BOOLEAN
);
SELECT cloudsync_init('tasks') AS _init_site_id_a \gset

-- ============================================================
-- DB B: target database (with RLS)
-- ============================================================
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_29_b;
CREATE DATABASE cloudsync_test_29_b;

-- Create non-superuser role (ignore error if it already exists)
DO $$ BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'test_rls_user') THEN
        CREATE ROLE test_rls_user LOGIN;
    END IF;
END $$;

\connect cloudsync_test_29_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE tasks (
    id          TEXT PRIMARY KEY NOT NULL,
    user_id     UUID,
    title       TEXT,
    description TEXT,
    priority    INTEGER,
    is_complete BOOLEAN
);
SELECT cloudsync_init('tasks') AS _init_site_id_b \gset

-- Auth mock: auth.uid() reads from session variable app.current_user_id
CREATE SCHEMA IF NOT EXISTS auth;
CREATE OR REPLACE FUNCTION auth.uid() RETURNS UUID
    LANGUAGE sql STABLE
AS $$ SELECT NULLIF(current_setting('app.current_user_id', true), '')::UUID; $$;

-- Enable RLS
ALTER TABLE tasks ENABLE ROW LEVEL SECURITY;

CREATE POLICY "select_own" ON tasks FOR SELECT
    USING (auth.uid() = user_id);
CREATE POLICY "insert_own" ON tasks FOR INSERT
    WITH CHECK (auth.uid() = user_id);
CREATE POLICY "update_own" ON tasks FOR UPDATE
    USING (auth.uid() = user_id)
    WITH CHECK (auth.uid() = user_id);
CREATE POLICY "delete_own" ON tasks FOR DELETE
    USING (auth.uid() = user_id);

-- Grant permissions to test_rls_user
GRANT USAGE ON SCHEMA public TO test_rls_user;
GRANT ALL ON ALL TABLES IN SCHEMA public TO test_rls_user;
GRANT ALL ON ALL SEQUENCES IN SCHEMA public TO test_rls_user;
GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA public TO test_rls_user;
GRANT USAGE ON SCHEMA auth TO test_rls_user;
GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA auth TO test_rls_user;

-- ============================================================
-- Test 1: Superuser multi-row insert with varied types
-- ============================================================
\connect cloudsync_test_29_a
\ir helper_psql_conn_setup.sql
INSERT INTO tasks VALUES ('t1', :'USER1'::UUID, 'Task 1', 'Desc 1', 3, false);
INSERT INTO tasks VALUES ('t2', :'USER1'::UUID, 'Task 2', 'Desc 2', 1, true);
INSERT INTO tasks VALUES ('t3', :'USER2'::UUID, 'Task 3', 'Desc 3', 5, false);

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex_1
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

SELECT COALESCE(max(db_version), 0) AS max_dbv_1 FROM cloudsync_changes \gset

-- Apply as superuser
\connect cloudsync_test_29_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_hex_1', 'hex')) AS apply_1 \gset

-- 3 rows × 5 non-PK columns = 15 column-change entries
SELECT (:apply_1::int = 15) AS apply_1_ok \gset
\if :apply_1_ok
\echo [PASS] (:testid) RLS multicol: superuser multi-row apply returned :apply_1
\else
\echo [FAIL] (:testid) RLS multicol: superuser multi-row apply returned :apply_1 (expected 15)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify all 3 rows with correct column values
SELECT COUNT(*) AS t1_ok FROM tasks WHERE id = 't1' AND user_id = :'USER1'::UUID AND title = 'Task 1' AND description = 'Desc 1' AND priority = 3 AND is_complete = false \gset
SELECT COUNT(*) AS t2_ok FROM tasks WHERE id = 't2' AND user_id = :'USER1'::UUID AND title = 'Task 2' AND description = 'Desc 2' AND priority = 1 AND is_complete = true \gset
SELECT COUNT(*) AS t3_ok FROM tasks WHERE id = 't3' AND user_id = :'USER2'::UUID AND title = 'Task 3' AND description = 'Desc 3' AND priority = 5 AND is_complete = false \gset
SELECT (:t1_ok::int = 1 AND :t2_ok::int = 1 AND :t3_ok::int = 1) AS test1_ok \gset
\if :test1_ok
\echo [PASS] (:testid) RLS multicol: superuser multi-row insert with varied types
\else
\echo [FAIL] (:testid) RLS multicol: superuser multi-row insert with varied types — t1=:t1_ok t2=:t2_ok t3=:t3_ok
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 2: Superuser multi-column partial update
-- ============================================================
\connect cloudsync_test_29_a
\ir helper_psql_conn_setup.sql
UPDATE tasks SET title = 'Task 1 Updated', priority = 10, is_complete = true WHERE id = 't1';

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex_2
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid()
  AND db_version > :max_dbv_1 \gset

SELECT COALESCE(max(db_version), 0) AS max_dbv_2 FROM cloudsync_changes \gset

-- Apply as superuser
\connect cloudsync_test_29_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_hex_2', 'hex')) AS apply_2 \gset

-- 1 row × 3 changed columns (title, priority, is_complete) = 3 entries
SELECT (:apply_2::int = 3) AS apply_2_ok \gset
\if :apply_2_ok
\echo [PASS] (:testid) RLS multicol: superuser partial update apply returned :apply_2
\else
\echo [FAIL] (:testid) RLS multicol: superuser partial update apply returned :apply_2 (expected 3)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify updated columns changed and description preserved
SELECT COUNT(*) AS t1_updated FROM tasks WHERE id = 't1' AND title = 'Task 1 Updated' AND description = 'Desc 1' AND priority = 10 AND is_complete = true \gset
SELECT (:t1_updated::int = 1) AS test2_ok \gset
\if :test2_ok
\echo [PASS] (:testid) RLS multicol: superuser partial update preserves unchanged columns
\else
\echo [FAIL] (:testid) RLS multicol: superuser partial update preserves unchanged columns — got :t1_updated
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 3: Authenticated insert own row (all columns)
-- ============================================================
\connect cloudsync_test_29_a
\ir helper_psql_conn_setup.sql
INSERT INTO tasks VALUES ('t4', :'USER1'::UUID, 'Task 4', 'Desc 4', 2, false);

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex_3
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid()
  AND db_version > :max_dbv_2 \gset

SELECT COALESCE(max(db_version), 0) AS max_dbv_3 FROM cloudsync_changes \gset

\connect cloudsync_test_29_b
\ir helper_psql_conn_setup.sql
SET app.current_user_id = :'USER1';
SET ROLE test_rls_user;
SELECT cloudsync_payload_apply(decode(:'payload_hex_3', 'hex')) AS apply_3 \gset
RESET ROLE;

-- 1 row × 5 non-PK columns = 5 entries
SELECT (:apply_3::int = 5) AS apply_3_ok \gset
\if :apply_3_ok
\echo [PASS] (:testid) RLS multicol auth: insert own row apply returned :apply_3
\else
\echo [FAIL] (:testid) RLS multicol auth: insert own row apply returned :apply_3 (expected 5)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify row exists with all columns correct
SELECT COUNT(*) AS t4_count FROM tasks WHERE id = 't4' AND user_id = :'USER1'::UUID AND title = 'Task 4' AND description = 'Desc 4' AND priority = 2 AND is_complete = false \gset
SELECT (:t4_count::int = 1) AS test3_ok \gset
\if :test3_ok
\echo [PASS] (:testid) RLS multicol auth: insert own row allowed
\else
\echo [FAIL] (:testid) RLS multicol auth: insert own row allowed — got :t4_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 4: Authenticated insert denied (other user's row)
-- ============================================================
\connect cloudsync_test_29_a
\ir helper_psql_conn_setup.sql
INSERT INTO tasks VALUES ('t5', :'USER2'::UUID, 'Task 5', 'Desc 5', 7, true);

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex_4
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid()
  AND db_version > :max_dbv_3 \gset

SELECT COALESCE(max(db_version), 0) AS max_dbv_4 FROM cloudsync_changes \gset

-- Apply as test_rls_user with USER1 identity — should be denied (t5 owned by USER2)
\connect cloudsync_test_29_b
\ir helper_psql_conn_setup.sql
SET app.current_user_id = :'USER1';
SET ROLE test_rls_user;
SELECT cloudsync_payload_apply(decode(:'payload_hex_4', 'hex')) AS apply_4 \gset

-- Reconnect for clean state after expected RLS denial
\connect cloudsync_test_29_b
\ir helper_psql_conn_setup.sql

-- 1 row × 5 columns = 5 entries in payload (returned even if denied)
SELECT (:apply_4::int = 5) AS apply_4_ok \gset
\if :apply_4_ok
\echo [PASS] (:testid) RLS multicol auth: denied insert apply returned :apply_4
\else
\echo [FAIL] (:testid) RLS multicol auth: denied insert apply returned :apply_4 (expected 5)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify t5 does NOT exist (superuser check)
SELECT COUNT(*) AS t5_count FROM tasks WHERE id = 't5' \gset
SELECT (:t5_count::int = 0) AS test4_ok \gset
\if :test4_ok
\echo [PASS] (:testid) RLS multicol auth: insert other user row denied
\else
\echo [FAIL] (:testid) RLS multicol auth: insert other user row denied — got :t5_count rows (expected 0)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 5: Authenticated update own row (multiple columns)
-- ============================================================
\connect cloudsync_test_29_a
\ir helper_psql_conn_setup.sql
UPDATE tasks SET title = 'Task 4 Updated', priority = 9 WHERE id = 't4';

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex_5
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid()
  AND db_version > :max_dbv_4 \gset

SELECT COALESCE(max(db_version), 0) AS max_dbv_5 FROM cloudsync_changes \gset

\connect cloudsync_test_29_b
\ir helper_psql_conn_setup.sql
SET app.current_user_id = :'USER1';
SET ROLE test_rls_user;
SELECT cloudsync_payload_apply(decode(:'payload_hex_5', 'hex')) AS apply_5 \gset
RESET ROLE;

-- 1 row × 2 changed columns (title, priority) = 2 entries
SELECT (:apply_5::int = 2) AS apply_5_ok \gset
\if :apply_5_ok
\echo [PASS] (:testid) RLS multicol auth: update own row apply returned :apply_5
\else
\echo [FAIL] (:testid) RLS multicol auth: update own row apply returned :apply_5 (expected 2)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify both columns changed, others preserved
SELECT COUNT(*) AS t4_updated FROM tasks WHERE id = 't4' AND title = 'Task 4 Updated' AND description = 'Desc 4' AND priority = 9 AND is_complete = false \gset
SELECT (:t4_updated::int = 1) AS test5_ok \gset
\if :test5_ok
\echo [PASS] (:testid) RLS multicol auth: update own row allowed
\else
\echo [FAIL] (:testid) RLS multicol auth: update own row allowed — got :t4_updated
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 6: Authenticated update denied (other user's row)
-- ============================================================
\connect cloudsync_test_29_a
\ir helper_psql_conn_setup.sql
-- t3 is owned by USER2, update it on A
UPDATE tasks SET title = 'Task 3 Hacked', priority = 99 WHERE id = 't3';

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex_6
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid()
  AND db_version > :max_dbv_5 \gset

SELECT COALESCE(max(db_version), 0) AS max_dbv_6 FROM cloudsync_changes \gset

-- Apply as test_rls_user with USER1 identity — should be denied (t3 owned by USER2)
\connect cloudsync_test_29_b
\ir helper_psql_conn_setup.sql
SET app.current_user_id = :'USER1';
SET ROLE test_rls_user;
SELECT cloudsync_payload_apply(decode(:'payload_hex_6', 'hex')) AS apply_6 \gset

-- Reconnect for clean state after expected RLS denial
\connect cloudsync_test_29_b
\ir helper_psql_conn_setup.sql

-- 1 row × 2 changed columns (title, priority) = 2 entries in payload
SELECT (:apply_6::int = 2) AS apply_6_ok \gset
\if :apply_6_ok
\echo [PASS] (:testid) RLS multicol auth: denied update apply returned :apply_6
\else
\echo [FAIL] (:testid) RLS multicol auth: denied update apply returned :apply_6 (expected 2)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify t3 still has original values (superuser check)
SELECT COUNT(*) AS t3_unchanged FROM tasks WHERE id = 't3' AND title = 'Task 3' AND priority = 5 \gset
SELECT (:t3_unchanged::int = 1) AS test6_ok \gset
\if :test6_ok
\echo [PASS] (:testid) RLS multicol auth: update other user row denied
\else
\echo [FAIL] (:testid) RLS multicol auth: update other user row denied — got :t3_unchanged (expected 1 unchanged)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 7: Mixed payload — own + other user's rows (per-PK savepoint)
-- ============================================================
\connect cloudsync_test_29_a
\ir helper_psql_conn_setup.sql
INSERT INTO tasks VALUES ('t6', :'USER1'::UUID, 'Task 6', 'Desc 6', 4, false);
INSERT INTO tasks VALUES ('t7', :'USER2'::UUID, 'Task 7', 'Desc 7', 8, true);

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex_7
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid()
  AND db_version > :max_dbv_6 \gset

SELECT COALESCE(max(db_version), 0) AS max_dbv_7 FROM cloudsync_changes \gset

-- Apply as test_rls_user with USER1 identity
-- Per-PK savepoint: t6 (USER1) should succeed, t7 (USER2) should be denied
\connect cloudsync_test_29_b
\ir helper_psql_conn_setup.sql
SET app.current_user_id = :'USER1';
SET ROLE test_rls_user;
SELECT cloudsync_payload_apply(decode(:'payload_hex_7', 'hex')) AS apply_7 \gset

-- Reconnect for clean verification as superuser
\connect cloudsync_test_29_b
\ir helper_psql_conn_setup.sql

-- 2 rows × 5 columns = 10 entries in payload
SELECT (:apply_7::int = 10) AS apply_7_ok \gset
\if :apply_7_ok
\echo [PASS] (:testid) RLS multicol auth: mixed payload apply returned :apply_7
\else
\echo [FAIL] (:testid) RLS multicol auth: mixed payload apply returned :apply_7 (expected 10)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- t6 (own row) should exist, t7 (other's row) should NOT
SELECT COUNT(*) AS t6_exists FROM tasks WHERE id = 't6' AND user_id = :'USER1'::UUID AND title = 'Task 6' \gset
SELECT COUNT(*) AS t7_exists FROM tasks WHERE id = 't7' \gset
SELECT (:t6_exists::int = 1 AND :t7_exists::int = 0) AS test7_ok \gset
\if :test7_ok
\echo [PASS] (:testid) RLS multicol auth: mixed payload — per-PK savepoint isolation
\else
\echo [FAIL] (:testid) RLS multicol auth: mixed payload — t6=:t6_exists (expect 1) t7=:t7_exists (expect 0)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 8: NULL in non-ownership columns
-- ============================================================
\connect cloudsync_test_29_a
\ir helper_psql_conn_setup.sql
INSERT INTO tasks VALUES ('t8', :'USER1'::UUID, 'Task 8', NULL, NULL, false);

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex_8
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid()
  AND db_version > :max_dbv_7 \gset

\connect cloudsync_test_29_b
\ir helper_psql_conn_setup.sql
SET app.current_user_id = :'USER1';
SET ROLE test_rls_user;
SELECT cloudsync_payload_apply(decode(:'payload_hex_8', 'hex')) AS apply_8 \gset
RESET ROLE;

-- 1 row × 5 non-PK columns = 5 entries
SELECT (:apply_8::int = 5) AS apply_8_ok \gset
\if :apply_8_ok
\echo [PASS] (:testid) RLS multicol auth: NULL columns apply returned :apply_8
\else
\echo [FAIL] (:testid) RLS multicol auth: NULL columns apply returned :apply_8 (expected 5)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify NULLs preserved
SELECT COUNT(*) AS t8_count FROM tasks WHERE id = 't8' AND user_id = :'USER1'::UUID AND title = 'Task 8' AND description IS NULL AND priority IS NULL AND is_complete = false \gset
SELECT (:t8_count::int = 1) AS test8_ok \gset
\if :test8_ok
\echo [PASS] (:testid) RLS multicol auth: NULL in non-ownership columns preserved
\else
\echo [FAIL] (:testid) RLS multicol auth: NULL in non-ownership columns preserved — got :t8_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Cleanup
-- ============================================================
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_29_a;
DROP DATABASE IF EXISTS cloudsync_test_29_b;
\else
\echo [INFO] !!!!!
\endif
