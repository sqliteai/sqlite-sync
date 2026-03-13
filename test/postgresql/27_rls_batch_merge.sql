-- 'RLS batch merge test'
-- Verifies that the deferred column-batch merge produces complete rows
-- that work correctly with PostgreSQL Row Level Security policies.
--
-- Tests 1-3: cloudsync_payload_apply runs as superuser (service-role pattern).
-- RLS is enforced at the query layer when users access data.
--
-- Tests 4-6: cloudsync_payload_apply runs as non-superuser (authenticated-role
-- pattern). RLS is enforced during the write itself.

\set testid '27'
\ir helper_test_init.sql

\set USER1 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa'
\set USER2 'bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb'

-- ============================================================
-- DB A: source database (no RLS)
-- ============================================================
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_27_a;
CREATE DATABASE cloudsync_test_27_a;

\connect cloudsync_test_27_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE documents (
    id      TEXT PRIMARY KEY NOT NULL,
    user_id UUID,
    title   TEXT,
    content TEXT
);
SELECT cloudsync_init('documents') AS _init_site_id_a \gset

-- ============================================================
-- DB B: target database (with RLS)
-- ============================================================
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_27_b;
CREATE DATABASE cloudsync_test_27_b;

-- Create non-superuser role (ignore error if it already exists)
DO $$ BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'test_rls_user') THEN
        CREATE ROLE test_rls_user LOGIN;
    END IF;
END $$;

\connect cloudsync_test_27_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE documents (
    id      TEXT PRIMARY KEY NOT NULL,
    user_id UUID,
    title   TEXT,
    content TEXT
);
SELECT cloudsync_init('documents') AS _init_site_id_b \gset

-- Auth mock: auth.uid() reads from session variable app.current_user_id
CREATE SCHEMA IF NOT EXISTS auth;
CREATE OR REPLACE FUNCTION auth.uid() RETURNS UUID
    LANGUAGE sql STABLE
AS $$ SELECT NULLIF(current_setting('app.current_user_id', true), '')::UUID; $$;

-- Enable RLS
ALTER TABLE documents ENABLE ROW LEVEL SECURITY;

CREATE POLICY "select_own" ON documents FOR SELECT
    USING (auth.uid() = user_id);
CREATE POLICY "insert_own" ON documents FOR INSERT
    WITH CHECK (auth.uid() = user_id);
CREATE POLICY "update_own" ON documents FOR UPDATE
    USING (auth.uid() = user_id)
    WITH CHECK (auth.uid() = user_id);
CREATE POLICY "delete_own" ON documents FOR DELETE
    USING (auth.uid() = user_id);

-- Grant permissions to test_rls_user
GRANT USAGE ON SCHEMA public TO test_rls_user;
GRANT ALL ON ALL TABLES IN SCHEMA public TO test_rls_user;
GRANT ALL ON ALL SEQUENCES IN SCHEMA public TO test_rls_user;
GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA public TO test_rls_user;
GRANT USAGE ON SCHEMA auth TO test_rls_user;
GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA auth TO test_rls_user;

-- ============================================================
-- Test 1: Batch merge produces complete row — user1 doc synced
-- ============================================================
\connect cloudsync_test_27_a
\ir helper_psql_conn_setup.sql
INSERT INTO documents VALUES ('doc1', :'USER1'::UUID, 'Title 1', 'Content 1');

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex_1
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

-- Save high-water mark so subsequent encodes only pick up new changes
SELECT COALESCE(max(db_version), 0) AS max_dbv_1 FROM cloudsync_changes \gset

-- Apply as superuser (service-role pattern)
\connect cloudsync_test_27_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_hex_1', 'hex')) AS apply_1 \gset

-- 1 row × 3 non-PK columns = 3 column-change entries
SELECT (:apply_1::int = 3) AS apply_1_ok \gset
\if :apply_1_ok
\echo [PASS] (:testid) RLS: apply returned :apply_1
\else
\echo [FAIL] (:testid) RLS: apply returned :apply_1 (expected 3)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify complete row written (all columns present)
SELECT COUNT(*) AS doc1_count FROM documents WHERE id = 'doc1' AND title = 'Title 1' AND content = 'Content 1' AND user_id = :'USER1'::UUID \gset
SELECT (:doc1_count::int = 1) AS test1_ok \gset
\if :test1_ok
\echo [PASS] (:testid) RLS: batch merge writes complete row
\else
\echo [FAIL] (:testid) RLS: batch merge writes complete row — got :doc1_count matching rows
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 2: Sync user2 doc, then verify RLS hides it from user1
-- ============================================================
\connect cloudsync_test_27_a
\ir helper_psql_conn_setup.sql
INSERT INTO documents VALUES ('doc2', :'USER2'::UUID, 'Title 2', 'Content 2');

-- Encode only changes newer than test 1 (doc2 only)
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex_2
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid()
  AND db_version > :max_dbv_1 \gset

SELECT COALESCE(max(db_version), 0) AS max_dbv_2 FROM cloudsync_changes \gset

-- Apply as superuser
\connect cloudsync_test_27_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_hex_2', 'hex')) AS apply_2 \gset

-- 1 row × 3 non-PK columns = 3 entries
SELECT (:apply_2::int = 3) AS apply_2_ok \gset
\if :apply_2_ok
\echo [PASS] (:testid) RLS: apply returned :apply_2
\else
\echo [FAIL] (:testid) RLS: apply returned :apply_2 (expected 3)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify doc2 exists (superuser sees all)
SELECT COUNT(*) AS doc2_exists FROM documents WHERE id = 'doc2' \gset

-- Now check as user1: RLS should hide doc2 (owned by user2)
SET app.current_user_id = :'USER1';
SET ROLE test_rls_user;
SELECT COUNT(*) AS doc2_visible FROM documents WHERE id = 'doc2' \gset
RESET ROLE;

SELECT (:doc2_exists::int = 1 AND :doc2_visible::int = 0) AS test2_ok \gset
\if :test2_ok
\echo [PASS] (:testid) RLS: user2 doc synced but hidden from user1
\else
\echo [FAIL] (:testid) RLS: user2 doc synced but hidden from user1 — exists=:doc2_exists visible=:doc2_visible
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 3: Update doc1, verify user1 sees update via RLS
-- ============================================================
\connect cloudsync_test_27_a
\ir helper_psql_conn_setup.sql
UPDATE documents SET title = 'Title 1 Updated' WHERE id = 'doc1';

-- Encode only changes newer than test 2 (doc1 update only)
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex_3
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid()
  AND db_version > :max_dbv_2 \gset

SELECT COALESCE(max(db_version), 0) AS max_dbv_3 FROM cloudsync_changes \gset

-- Apply as superuser
\connect cloudsync_test_27_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_hex_3', 'hex')) AS apply_3 \gset

-- 1 row × 1 changed column (title) = 1 entry
SELECT (:apply_3::int = 1) AS apply_3_ok \gset
\if :apply_3_ok
\echo [PASS] (:testid) RLS: apply returned :apply_3
\else
\echo [FAIL] (:testid) RLS: apply returned :apply_3 (expected 1)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify update applied (superuser check)
SELECT COUNT(*) AS doc1_updated FROM documents WHERE id = 'doc1' AND title = 'Title 1 Updated' \gset

-- Verify user1 can see the updated row via RLS
SET app.current_user_id = :'USER1';
SET ROLE test_rls_user;
SELECT COUNT(*) AS doc1_visible FROM documents WHERE id = 'doc1' AND title = 'Title 1 Updated' \gset
RESET ROLE;

SELECT (:doc1_updated::int = 1 AND :doc1_visible::int = 1) AS test3_ok \gset
\if :test3_ok
\echo [PASS] (:testid) RLS: update synced and visible to owner
\else
\echo [FAIL] (:testid) RLS: update synced and visible to owner — updated=:doc1_updated visible=:doc1_visible
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 4: Authenticated insert allowed (own row)
-- cloudsync_payload_apply as non-superuser with matching user_id
-- ============================================================
\connect cloudsync_test_27_a
\ir helper_psql_conn_setup.sql
INSERT INTO documents VALUES ('doc3', :'USER1'::UUID, 'Title 3', 'Content 3');

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex_4
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid()
  AND db_version > :max_dbv_3 \gset

SELECT COALESCE(max(db_version), 0) AS max_dbv_4 FROM cloudsync_changes \gset

\connect cloudsync_test_27_b
\ir helper_psql_conn_setup.sql
SET app.current_user_id = :'USER1';
SET ROLE test_rls_user;
SELECT cloudsync_payload_apply(decode(:'payload_hex_4', 'hex')) AS apply_4 \gset
RESET ROLE;

-- 1 row × 3 non-PK columns = 3 entries
SELECT (:apply_4::int = 3) AS apply_4_ok \gset
\if :apply_4_ok
\echo [PASS] (:testid) RLS auth: apply returned :apply_4
\else
\echo [FAIL] (:testid) RLS auth: apply returned :apply_4 (expected 3)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify doc3 exists with all columns correct
SELECT COUNT(*) AS doc3_count FROM documents WHERE id = 'doc3' AND title = 'Title 3' AND content = 'Content 3' AND user_id = :'USER1'::UUID \gset
SELECT (:doc3_count::int = 1) AS test4_ok \gset
\if :test4_ok
\echo [PASS] (:testid) RLS auth: insert own row allowed
\else
\echo [FAIL] (:testid) RLS auth: insert own row allowed — got :doc3_count matching rows
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 5: Authenticated insert denied (other user's row)
-- cloudsync_payload_apply as non-superuser with mismatched user_id
-- ============================================================
\connect cloudsync_test_27_a
\ir helper_psql_conn_setup.sql
INSERT INTO documents VALUES ('doc4', :'USER2'::UUID, 'Title 4', 'Content 4');

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex_5
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid()
  AND db_version > :max_dbv_4 \gset

SELECT COALESCE(max(db_version), 0) AS max_dbv_5 FROM cloudsync_changes \gset

-- Apply as test_rls_user with USER1 identity — should be denied (doc4 owned by USER2)
\connect cloudsync_test_27_b
\ir helper_psql_conn_setup.sql
SET app.current_user_id = :'USER1';
SET ROLE test_rls_user;
SELECT cloudsync_payload_apply(decode(:'payload_hex_5', 'hex')) AS apply_5 \gset

-- Reconnect for clean state after expected RLS denial
\connect cloudsync_test_27_b
\ir helper_psql_conn_setup.sql

-- 1 row × 3 non-PK columns = 3 entries (returned even if denied)
SELECT (:apply_5::int = 3) AS apply_5_ok \gset
\if :apply_5_ok
\echo [PASS] (:testid) RLS auth: denied apply returned :apply_5
\else
\echo [FAIL] (:testid) RLS auth: denied apply returned :apply_5 (expected 3)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify doc4 does NOT exist (superuser check)
SELECT COUNT(*) AS doc4_count FROM documents WHERE id = 'doc4' \gset
SELECT (:doc4_count::int = 0) AS test5_ok \gset
\if :test5_ok
\echo [PASS] (:testid) RLS auth: insert other user row denied
\else
\echo [FAIL] (:testid) RLS auth: insert other user row denied — got :doc4_count rows (expected 0)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 6: Authenticated update allowed (own row)
-- cloudsync_payload_apply as non-superuser updating own row
-- ============================================================
\connect cloudsync_test_27_a
\ir helper_psql_conn_setup.sql
UPDATE documents SET title = 'Title 3 Updated' WHERE id = 'doc3';

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex_6
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid()
  AND db_version > :max_dbv_5 \gset

\connect cloudsync_test_27_b
\ir helper_psql_conn_setup.sql
SET app.current_user_id = :'USER1';
SET ROLE test_rls_user;
SELECT cloudsync_payload_apply(decode(:'payload_hex_6', 'hex')) AS apply_6 \gset
RESET ROLE;

-- 1 row × 1 changed column (title) = 1 entry
SELECT (:apply_6::int = 1) AS apply_6_ok \gset
\if :apply_6_ok
\echo [PASS] (:testid) RLS auth: apply returned :apply_6
\else
\echo [FAIL] (:testid) RLS auth: apply returned :apply_6 (expected 1)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify doc3 title was updated
SELECT COUNT(*) AS doc3_updated FROM documents WHERE id = 'doc3' AND title = 'Title 3 Updated' \gset
SELECT (:doc3_updated::int = 1) AS test6_ok \gset
\if :test6_ok
\echo [PASS] (:testid) RLS auth: update own row allowed
\else
\echo [FAIL] (:testid) RLS auth: update own row allowed — got :doc3_updated matching rows
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Cleanup
-- ============================================================
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_27_a;
DROP DATABASE IF EXISTS cloudsync_test_27_b;
DROP ROLE IF EXISTS test_rls_user;
\else
\echo [INFO] !!!!!
\endif
