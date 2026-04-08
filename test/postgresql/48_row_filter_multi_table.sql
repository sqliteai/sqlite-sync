-- 'Row-level filter: multi-table with different filters and composite primary keys'

\set testid '48'
\ir helper_test_init.sql

-- Create databases
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_48_a;
DROP DATABASE IF EXISTS cloudsync_test_48_b;
CREATE DATABASE cloudsync_test_48_a;
CREATE DATABASE cloudsync_test_48_b;

-- ============================================================
-- Setup Database A
-- ============================================================
\connect cloudsync_test_48_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- Table 1: composite PK with simple filter
CREATE TABLE projects (org_id INTEGER NOT NULL, proj_id INTEGER NOT NULL, name TEXT, PRIMARY KEY(org_id, proj_id));
SELECT cloudsync_init('projects') AS _init \gset
SELECT cloudsync_set_filter('projects', 'org_id = 1') AS _sf \gset

-- Table 2: composite PK with multi-column filter including string literal
CREATE TABLE members (org_id INTEGER NOT NULL, user_id INTEGER NOT NULL, role TEXT, PRIMARY KEY(org_id, user_id));
SELECT cloudsync_init('members') AS _init \gset
SELECT cloudsync_set_filter('members', 'org_id = 1 AND role = ''admin''') AS _sf \gset

-- ============================================================
-- Test 1: Composite PK — only matching rows tracked
-- ============================================================
INSERT INTO projects VALUES (1, 1, 'Proj A');   -- matches
INSERT INTO projects VALUES (2, 1, 'Proj B');   -- fails org_id
INSERT INTO projects VALUES (1, 2, 'Proj C');   -- matches

SELECT COUNT(DISTINCT pk) AS proj_meta FROM projects_cloudsync \gset
SELECT (:proj_meta = 2) AS t1_proj_ok \gset
\if :t1_proj_ok
\echo [PASS] (:testid) composite_pk: 2 of 3 projects tracked
\else
\echo [FAIL] (:testid) composite_pk: expected 2 project PKs, got :proj_meta
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 2: Multi-column filter with string literal — different table
-- ============================================================
INSERT INTO members VALUES (1, 10, 'admin');    -- matches both conditions
INSERT INTO members VALUES (1, 20, 'viewer');   -- fails role
INSERT INTO members VALUES (2, 10, 'admin');    -- fails org_id

SELECT COUNT(DISTINCT pk) AS mem_meta FROM members_cloudsync \gset
SELECT (:mem_meta = 1) AS t2_mem_ok \gset
\if :t2_mem_ok
\echo [PASS] (:testid) multi_filter: 1 of 3 members tracked
\else
\echo [FAIL] (:testid) multi_filter: expected 1 member PK, got :mem_meta
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 3: Roundtrip sync — only matching rows per table transfer
-- ============================================================
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_test_48_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE projects (org_id INTEGER NOT NULL, proj_id INTEGER NOT NULL, name TEXT, PRIMARY KEY(org_id, proj_id));
SELECT cloudsync_init('projects') AS _init \gset
SELECT cloudsync_set_filter('projects', 'org_id = 1') AS _sf \gset

CREATE TABLE members (org_id INTEGER NOT NULL, user_id INTEGER NOT NULL, role TEXT, PRIMARY KEY(org_id, user_id));
SELECT cloudsync_init('members') AS _init \gset
SELECT cloudsync_set_filter('members', 'org_id = 1 AND role = ''admin''') AS _sf \gset

SELECT cloudsync_payload_apply(decode(:'payload_hex', 'hex')) AS _apply \gset

-- Verify projects
SELECT COUNT(*) AS proj_count FROM projects \gset
SELECT (:proj_count = 2) AS t3_proj_ok \gset
\if :t3_proj_ok
\echo [PASS] (:testid) roundtrip: 2 projects synced to db_b
\else
\echo [FAIL] (:testid) roundtrip: expected 2 projects in db_b, got :proj_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify members
SELECT COUNT(*) AS mem_count FROM members \gset
SELECT (:mem_count = 1) AS t3_mem_ok \gset
\if :t3_mem_ok
\echo [PASS] (:testid) roundtrip: 1 member synced to db_b
\else
\echo [FAIL] (:testid) roundtrip: expected 1 member in db_b, got :mem_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify correct member identity
SELECT COUNT(*) AS admin_exists FROM members WHERE org_id = 1 AND user_id = 10 AND role = 'admin' \gset
SELECT (:admin_exists = 1) AS t3_admin_ok \gset
\if :t3_admin_ok
\echo [PASS] (:testid) roundtrip: correct admin member (1,10) present
\else
\echo [FAIL] (:testid) roundtrip: admin member (1,10) not found
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Non-matching rows should NOT exist
SELECT COUNT(*) AS bad_proj FROM projects WHERE org_id = 2 \gset
SELECT (:bad_proj = 0) AS t3_no_bad_proj \gset
\if :t3_no_bad_proj
\echo [PASS] (:testid) roundtrip: no org_id=2 projects in db_b
\else
\echo [FAIL] (:testid) roundtrip: unexpected org_id=2 projects found (:bad_proj)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 4: Update and delete on composite PK, then re-sync
-- ============================================================
\connect cloudsync_test_48_a
\ir helper_psql_conn_setup.sql

UPDATE projects SET name = 'Proj A Updated' WHERE org_id = 1 AND proj_id = 1;
DELETE FROM projects WHERE org_id = 1 AND proj_id = 2;

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload2_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_test_48_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload2_hex', 'hex')) AS _apply2 \gset

SELECT COUNT(*) AS proj_count FROM projects \gset
SELECT (:proj_count = 1) AS t4_count_ok \gset
\if :t4_count_ok
\echo [PASS] (:testid) update_delete: 1 project remaining after sync
\else
\echo [FAIL] (:testid) update_delete: expected 1 project, got :proj_count
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT COUNT(*) AS updated_exists FROM projects WHERE org_id = 1 AND proj_id = 1 AND name = 'Proj A Updated' \gset
SELECT (:updated_exists = 1) AS t4_updated_ok \gset
\if :t4_updated_ok
\echo [PASS] (:testid) update_delete: 'Proj A Updated' present in db_b
\else
\echo [FAIL] (:testid) update_delete: 'Proj A Updated' not found in db_b
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_48_a;
DROP DATABASE IF EXISTS cloudsync_test_48_b;
\else
\echo [INFO] !!!!!
\endif
