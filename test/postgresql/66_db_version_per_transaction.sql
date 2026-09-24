-- Every transaction, implicit or explicit, takes one db_version; its changes share it
-- with seq restarting at 0, and a rolled back transaction takes none. Same scenario and
-- expectations as test_db_version_per_transaction in test/review_regressions.c, so
-- SQLite and PostgreSQL number changes the same way. PostgreSQL also has to hold while
-- another session keeps an old transaction open.

\set testid '66-db-version'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_66;
CREATE DATABASE cloudsync_test_66;
\connect cloudsync_test_66
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE EXTENSION IF NOT EXISTS dblink;
CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, v TEXT);
SELECT cloudsync_init('t') AS _init \gset
CREATE VIEW changes_t AS
SELECT string_agg(cloudsync_pk_decode(pk,1) || ':' || col_name || '@' || db_version || '/' || seq, '  ' ORDER BY db_version, seq) AS list
FROM cloudsync_changes WHERE tbl = 't';

INSERT INTO t VALUES ('a1','x');
INSERT INTO t VALUES ('a2','x');
INSERT INTO t VALUES ('b1','x'),('b2','x');
SELECT list = 'a1:v@1/0  a2:v@2/0  b1:v@3/0  b2:v@3/1' AS autocommit_ok, list AS autocommit_list FROM changes_t \gset
\if :autocommit_ok
\echo [PASS] (:testid) autocommit statements take one db_version each, seq restarts at 0
\else
\echo [FAIL] (:testid) autocommit statements: :autocommit_list
SELECT (:fail::int + 1) AS fail \gset
\endif

BEGIN;
INSERT INTO t VALUES ('c1','x');
SELECT cloudsync_db_version() AS mid_dbv \gset
INSERT INTO t VALUES ('c2','x');
UPDATE t SET v='y' WHERE id='a1';
COMMIT;
UPDATE t SET v='z' WHERE id='a2';
BEGIN;
UPDATE t SET v='y' WHERE id='b1';
UPDATE t SET v='y' WHERE id='b2';
COMMIT;
BEGIN;
INSERT INTO t VALUES ('r1','x');
ROLLBACK;
INSERT INTO t VALUES ('d1','x');
BEGIN;
UPDATE t SET v='w' WHERE id='a1';
UPDATE t SET v='q' WHERE id='a1';
DELETE FROM t WHERE id='b2';
COMMIT;
SELECT list = 'c1:v@4/0  c2:v@4/1  a2:v@5/0  b1:v@6/0  d1:v@7/0  a1:v@8/1  b2:__[RIP]__@8/2'
   AND :mid_dbv = 3 AND cloudsync_db_version() = 8 AS tx_ok, list AS tx_list FROM changes_t \gset
\if :tx_ok
\echo [PASS] (:testid) explicit transactions share one db_version, a rollback takes none
\else
\echo [FAIL] (:testid) explicit transactions: :tx_list (db_version inside tx1: :mid_dbv)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Another session holds an old transaction open, pinning the snapshot xmin that the
-- cached db_version is checked against: later transactions must still take new ones.
SELECT dblink_connect('old', format('dbname=%s user=%s', current_database(), current_user)) AS _c \gset
SELECT dblink_exec('old', 'BEGIN') AS _b \gset
SELECT x AS _xid FROM dblink('old', 'SELECT txid_current()') AS r(x BIGINT) \gset
DELETE FROM t;
INSERT INTO t VALUES ('p1','x');
INSERT INTO t VALUES ('p2','x');
BEGIN;
INSERT INTO t VALUES ('p3','x');
INSERT INTO t VALUES ('p4','x');
COMMIT;
INSERT INTO t VALUES ('p5','x');
SELECT dblink_exec('old', 'COMMIT') AS _e \gset
SELECT dblink_disconnect('old') AS _d \gset
SELECT string_agg(cloudsync_pk_decode(pk,1) || '@' || db_version || '/' || seq, '  ' ORDER BY db_version, seq) AS pinned_list
FROM cloudsync_changes WHERE tbl = 't' AND cloudsync_pk_decode(pk,1) LIKE 'p%' \gset
SELECT :'pinned_list' = 'p1@10/0  p2@11/0  p3@12/0  p4@12/1  p5@13/0' AS pinned_ok \gset
\if :pinned_ok
\echo [PASS] (:testid) an old open transaction elsewhere does not make transactions share a db_version
\else
\echo [FAIL] (:testid) with an old transaction open elsewhere: :pinned_list
SELECT (:fail::int + 1) AS fail \gset
\endif

-- A payload applied inside a transaction takes its own db_versions, apart from the local
-- writes before and after it, and a rollback gives them all back. Same expectations as
-- test_db_version_apply_in_transaction in test/review_regressions.c.
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_66_src;
DROP DATABASE IF EXISTS cloudsync_test_66_dst;
DROP DATABASE IF EXISTS cloudsync_test_66_rb;
CREATE DATABASE cloudsync_test_66_src;
CREATE DATABASE cloudsync_test_66_dst;
CREATE DATABASE cloudsync_test_66_rb;

\connect cloudsync_test_66_src
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, v TEXT);
SELECT cloudsync_init('t') AS _init \gset
INSERT INTO t VALUES ('r1','x'),('r2','x');
INSERT INTO t VALUES ('r3','x');
SELECT '\x' || encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload
FROM cloudsync_changes \gset

\connect cloudsync_test_66_dst
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, v TEXT);
SELECT cloudsync_init('t') AS _init \gset
BEGIN;
INSERT INTO t VALUES ('l1','x');
SELECT cloudsync_payload_apply(decode(substr(:'payload', 3), 'hex')) AS _apply \gset
INSERT INTO t VALUES ('l2','x');
COMMIT;
SELECT string_agg(cloudsync_pk_decode(pk,1) || '@' || db_version || '/' || seq, '  ' ORDER BY db_version, seq) AS apply_list
FROM cloudsync_changes \gset
SELECT :'apply_list' = 'l1@1/0  r1@2/0  r2@2/1  r3@3/0  l2@4/0' AS apply_ok \gset
\if :apply_ok
\echo [PASS] (:testid) a payload applied inside a transaction keeps its db_versions apart from local writes
\else
\echo [FAIL] (:testid) payload applied inside a transaction: :apply_list
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect cloudsync_test_66_rb
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, v TEXT);
SELECT cloudsync_init('t') AS _init \gset
BEGIN;
SELECT cloudsync_payload_apply(decode(substr(:'payload', 3), 'hex')) AS _apply \gset
ROLLBACK;
SELECT cloudsync_db_version() AS rb_dbv \gset
INSERT INTO t VALUES ('l1','x');
SELECT string_agg(cloudsync_pk_decode(pk,1) || '@' || db_version || '/' || seq, '  ' ORDER BY db_version, seq) AS rb_list
FROM cloudsync_changes \gset
SELECT :rb_dbv = 0 AND :'rb_list' = 'l1@1/0' AS rb_ok \gset
\if :rb_ok
\echo [PASS] (:testid) a rolled back payload apply takes no db_version
\else
\echo [FAIL] (:testid) after a rolled back apply: db_version :rb_dbv, :rb_list
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_66;
DROP DATABASE IF EXISTS cloudsync_test_66_src;
DROP DATABASE IF EXISTS cloudsync_test_66_dst;
DROP DATABASE IF EXISTS cloudsync_test_66_rb;
