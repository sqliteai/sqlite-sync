-- 'Test concurrent write lock during payload apply'
-- NOTE: The lock-contention portion requires dblink with table access.
-- On environments where dblink cannot lock the table (e.g. Supabase),
-- the lock test is skipped and only apply + consistency are verified.

\set testid '39'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_39_a;
DROP DATABASE IF EXISTS cloudsync_test_39_b;
CREATE DATABASE cloudsync_test_39_a;
CREATE DATABASE cloudsync_test_39_b;

-- Setup db_a
\connect cloudsync_test_39_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE concurrent_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('concurrent_tbl', 'CLS', true) AS _init_a \gset

-- Setup db_b
\connect cloudsync_test_39_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE concurrent_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('concurrent_tbl', 'CLS', true) AS _init_b \gset

-- Insert row1 on db_a and sync to db_b
\connect cloudsync_test_39_a
INSERT INTO concurrent_tbl VALUES ('row1', 'val_a');

SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_init,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_init_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version,
         db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_39_b
\if :payload_init_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_init', 3), 'hex')) AS _apply_init \gset
\endif

-- Update row1 on db_a
\connect cloudsync_test_39_a
UPDATE concurrent_tbl SET val = 'val_a_updated' WHERE id = 'row1';

SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_upd,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_upd_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version,
         db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes WHERE site_id = cloudsync_siteid()
) AS p \gset

-- Try to set up dblink and acquire a table lock
\connect cloudsync_test_39_b
CREATE EXTENSION IF NOT EXISTS dblink;

SELECT dblink_connect('locker', 'dbname=cloudsync_test_39_b') AS _conn \gset
SELECT dblink_exec('locker', 'BEGIN') AS _begin \gset

-- Try to acquire EXCLUSIVE lock — if this fails (e.g. permission denied on
-- Supabase), _lock won't be set and we skip the lock-contention test
\unset _lock
SELECT dblink_exec('locker', 'LOCK TABLE concurrent_tbl IN EXCLUSIVE MODE') AS _lock \gset

\if :{?_lock}
-- ===== Lock acquired — run lock-contention test =====

BEGIN;
\set ON_ERROR_ROLLBACK on
SET LOCAL lock_timeout = '500ms';

\if :payload_upd_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_upd', 3), 'hex')) AS _blocked_apply \gset
\endif

COMMIT;
\set ON_ERROR_ROLLBACK off

-- row1 should still have the OLD value because the apply was blocked
SELECT val AS row1_val_check FROM concurrent_tbl WHERE id = 'row1' \gset
SELECT (:'row1_val_check' = 'val_a') AS blocked_ok \gset
\if :blocked_ok
\echo [PASS] (:testid) Apply correctly blocked by concurrent table lock
\else
\echo [FAIL] (:testid) Expected val_a (blocked), got :'row1_val_check'
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Release the table lock
SELECT dblink_exec('locker', 'COMMIT') AS _release \gset
SELECT dblink_disconnect('locker') AS _disconn \gset

-- Retry apply — should succeed now
\if :payload_upd_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_upd', 3), 'hex')) AS _apply_retry \gset
\endif

SELECT val AS row1_val FROM concurrent_tbl WHERE id = 'row1' \gset
SELECT (:'row1_val' = 'val_a_updated') AS retry_ok \gset
\if :retry_ok
\echo [PASS] (:testid) Apply succeeded after lock released
\else
\echo [FAIL] (:testid) Apply after unlock - expected val_a_updated, got :'row1_val'
SELECT (:fail::int + 1) AS fail \gset
\endif

\else
-- ===== Lock failed — skip contention test, apply directly =====
\echo [SKIP] (:testid) Lock-contention test skipped (dblink cannot lock table)

-- Clean up the dblink connection (transaction is aborted)
SELECT dblink_exec('locker', 'ROLLBACK') AS _rollback \gset
SELECT dblink_disconnect('locker') AS _disconn \gset

\if :payload_upd_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_upd', 3), 'hex')) AS _apply_direct \gset
\endif

SELECT val AS row1_val FROM concurrent_tbl WHERE id = 'row1' \gset
SELECT (:'row1_val' = 'val_a_updated') AS direct_ok \gset
\if :direct_ok
\echo [PASS] (:testid) Apply succeeded (no lock contention)
\else
\echo [FAIL] (:testid) Apply failed - expected val_a_updated, got :'row1_val'
SELECT (:fail::int + 1) AS fail \gset
\endif

\endif

-- Full cross-sync for consistency
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_b_final,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_b_final_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version,
         db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_39_a
\if :payload_b_final_ok
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_final', 3), 'hex')) AS _apply_final \gset
\endif

SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS hash_a
FROM concurrent_tbl \gset

\connect cloudsync_test_39_b
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS hash_b
FROM concurrent_tbl \gset

SELECT (:'hash_a' = :'hash_b') AS consistency_ok \gset
\if :consistency_ok
\echo [PASS] (:testid) Cross-database consistency verified
\else
\echo [FAIL] (:testid) Consistency failed (hash_a=:'hash_a' hash_b=:'hash_b')
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_39_a;
DROP DATABASE IF EXISTS cloudsync_test_39_b;
\endif
