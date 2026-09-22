-- cloudsync_payload_apply never owns the transaction on PostgreSQL, so a failure at
-- commit time belongs to the caller and leaves nothing behind.
--
-- 1. A deferred foreign key violated by the payload lets the apply succeed and fails the
--    caller's COMMIT with 23503: rows, metadata, the receive checkpoint and the caller's
--    own work roll back together, and the connection stays usable.
-- 2. The same payload in autocommit mode fails its statement with 23503 and keeps nothing.
-- 3. A write failing inside a caller's savepoint does not end the caller's transaction:
--    rolling back to the savepoint keeps earlier work, and redelivery in the same
--    transaction applies the payload.

\set testid '62-deferred-fk'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_62_src;
DROP DATABASE IF EXISTS cloudsync_test_62_dst;
CREATE DATABASE cloudsync_test_62_src;
CREATE DATABASE cloudsync_test_62_dst;

-- ------------------------------------------------------------------ source
\connect cloudsync_test_62_src
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE parent (id TEXT PRIMARY KEY);
CREATE TABLE t (id TEXT PRIMARY KEY NOT NULL, value TEXT REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED);
SELECT cloudsync_init('t') AS _init_src \gset
INSERT INTO parent VALUES ('valid'), ('missing');
INSERT INTO t VALUES ('a', 'valid');
INSERT INTO t VALUES ('b', 'missing');
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

-- ------------------------------------------------------------------ target
\connect cloudsync_test_62_dst
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE parent (id TEXT PRIMARY KEY);
CREATE TABLE t (id TEXT PRIMARY KEY NOT NULL, value TEXT REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED);
CREATE TABLE caller_work (value TEXT);
SELECT cloudsync_init('t') AS _init_dst \gset
INSERT INTO parent VALUES ('valid');

-- 1. Caller-owned transaction: the violation surfaces at the caller's COMMIT.
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_before \gset
BEGIN;
INSERT INTO caller_work VALUES ('kept');
\set ON_ERROR_STOP off
SELECT cloudsync_payload_apply(decode(:'payload', 'hex')) AS _applied \gset
\set apply_state :SQLSTATE
\set ON_ERROR_STOP on
SELECT (SELECT count(*) FROM t) = 2
   AND (SELECT count(*) FROM caller_work) = 1
   AND coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) > :ckpt_before::bigint
   AS in_txn_ok \gset
\set ON_ERROR_STOP off
COMMIT;
\set commit_state :SQLSTATE
\set ON_ERROR_STOP on
SELECT (:'apply_state' = '00000' AND :'in_txn_ok' = 't') AS apply_ok \gset
\if :apply_ok
\echo [PASS] (:testid) caller transaction: apply succeeded and left the transaction to the caller
\else
\echo [FAIL] (:testid) caller transaction: apply SQLSTATE :apply_state, in-transaction state :in_txn_ok
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT (:'commit_state' = '23503') AS commit_failed_ok \gset
\if :commit_failed_ok
\echo [PASS] (:testid) caller transaction: deferred foreign key failed the caller COMMIT with 23503
\else
\echo [FAIL] (:testid) caller transaction: COMMIT SQLSTATE :commit_state, expected 23503
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT NOT EXISTS (SELECT FROM t)
   AND NOT EXISTS (SELECT FROM t_cloudsync)
   AND NOT EXISTS (SELECT FROM caller_work)
   AND coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) = :ckpt_before::bigint
   AS rolled_back_ok \gset
\if :rolled_back_ok
\echo [PASS] (:testid) caller transaction: rows, metadata, checkpoint and caller work rolled back together
\else
\echo [FAIL] (:testid) caller transaction: state left behind after the failed COMMIT
SELECT (:fail::int + 1) AS fail \gset
\endif
BEGIN;
INSERT INTO caller_work VALUES ('after');
COMMIT;
SELECT (SELECT count(*) FROM caller_work) = 1 AS reusable_ok \gset
\if :reusable_ok
\echo [PASS] (:testid) caller transaction: connection usable after the failed COMMIT
\else
\echo [FAIL] (:testid) caller transaction: connection not usable after the failed COMMIT
SELECT (:fail::int + 1) AS fail \gset
\endif
DELETE FROM caller_work;

-- 2. Autocommit: the statement's own commit fails and keeps nothing.
\set ON_ERROR_STOP off
SELECT cloudsync_payload_apply(decode(:'payload', 'hex')) AS _applied \gset
\set auto_state :SQLSTATE
\set ON_ERROR_STOP on
SELECT (:'auto_state' = '23503')
   AND NOT EXISTS (SELECT FROM t)
   AND NOT EXISTS (SELECT FROM t_cloudsync)
   AND coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) = :ckpt_before::bigint
   AS auto_ok \gset
\if :auto_ok
\echo [PASS] (:testid) autocommit: 23503 and nothing kept
\else
\echo [FAIL] (:testid) autocommit: SQLSTATE :auto_state, or state left behind
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 3. A write failing inside a caller savepoint, then redelivery in the same transaction.
CREATE FUNCTION deny_t() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'denied'; END $$;
CREATE TRIGGER deny BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION deny_t();
BEGIN;
INSERT INTO caller_work VALUES ('kept');
SAVEPOINT caller_sp;
\set ON_ERROR_STOP off
SELECT cloudsync_payload_apply(decode(:'payload', 'hex')) AS _applied \gset
\set denied_state :SQLSTATE
\set ON_ERROR_STOP on
ROLLBACK TO SAVEPOINT caller_sp;
RELEASE SAVEPOINT caller_sp;
SELECT (:'denied_state' = 'P0001')
   AND (SELECT count(*) FROM caller_work) = 1
   AND NOT EXISTS (SELECT FROM t)
   AND NOT EXISTS (SELECT FROM t_cloudsync)
   AS savepoint_ok \gset
DROP TRIGGER deny ON t;
INSERT INTO parent VALUES ('missing');
\set ON_ERROR_STOP off
SELECT cloudsync_payload_apply(decode(:'payload', 'hex')) AS _applied \gset
\set retry_state :SQLSTATE
\set ON_ERROR_STOP on
COMMIT;
\if :savepoint_ok
\echo [PASS] (:testid) caller savepoint: failed write raised P0001 and the caller kept its earlier work
\else
\echo [FAIL] (:testid) caller savepoint: SQLSTATE :denied_state, or caller work or payload state wrong
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT (:'retry_state' = '00000')
   AND (SELECT count(*) FROM t) = 2
   AND (SELECT count(*) FROM caller_work) = 1
   AND coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) > :ckpt_before::bigint
   AS retry_ok \gset
\if :retry_ok
\echo [PASS] (:testid) redelivery: applied, committed with the caller work, checkpoint advanced
\else
\echo [FAIL] (:testid) redelivery: SQLSTATE :retry_state, or rows or checkpoint wrong
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_62_src;
DROP DATABASE IF EXISTS cloudsync_test_62_dst;
