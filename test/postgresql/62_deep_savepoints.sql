-- Exercise caller-owned buffers across internal subtransactions at deep nesting.
\set testid '62-deep-savepoints'
\ir helper_test_init.sql
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_62_source;
DROP DATABASE IF EXISTS cloudsync_test_62_target;
CREATE DATABASE cloudsync_test_62_source;
CREATE DATABASE cloudsync_test_62_target;
\connect cloudsync_test_62_source
\ir helper_psql_conn_setup.sql
CREATE EXTENSION cloudsync;
CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, value TEXT);
SELECT cloudsync_init('t');
INSERT INTO t SELECT i::text, repeat('value', 50) FROM generate_series(1, 20) i;
SELECT encode(cloudsync_payload_encode(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq),'hex') AS payload FROM cloudsync_changes \gset
\connect cloudsync_test_62_target
\ir helper_psql_conn_setup.sql
CREATE EXTENSION cloudsync;
CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, value TEXT);
SELECT cloudsync_init('t');
CREATE TABLE transport(payload BYTEA);
INSERT INTO transport SELECT decode(:'payload','hex') FROM generate_series(1, 10);

BEGIN;
SELECT 'SAVEPOINT user_sp' FROM generate_series(1, 1) \gexec
-- A heap scan owns the input buffer; an aggregate consumes every apply result.
SELECT sum(cloudsync_payload_apply(payload)) FROM transport;
SELECT count(*) = 20 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) apply at depth 1
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) apply at depth 1
\endif
ROLLBACK TO user_sp;
SELECT count(*) = 0 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) rollback at depth 1
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) rollback at depth 1
\endif
SELECT sum(cloudsync_payload_apply(payload)) FROM transport;
COMMIT;
SELECT count(*) = 20 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) reuse and commit at depth 1
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) reuse and commit at depth 1
\endif
TRUNCATE t, t_cloudsync;

BEGIN;
SELECT 'SAVEPOINT user_sp' FROM generate_series(1, 125) \gexec
-- A heap scan owns the input buffer; an aggregate consumes every apply result.
SELECT sum(cloudsync_payload_apply(payload)) FROM transport;
SELECT count(*) = 20 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) apply at depth 125
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) apply at depth 125
\endif
ROLLBACK TO user_sp;
SELECT count(*) = 0 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) rollback at depth 125
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) rollback at depth 125
\endif
SELECT sum(cloudsync_payload_apply(payload)) FROM transport;
COMMIT;
SELECT count(*) = 20 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) reuse and commit at depth 125
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) reuse and commit at depth 125
\endif
TRUNCATE t, t_cloudsync;

BEGIN;
SELECT 'SAVEPOINT user_sp' FROM generate_series(1, 126) \gexec
-- A heap scan owns the input buffer; an aggregate consumes every apply result.
SELECT sum(cloudsync_payload_apply(payload)) FROM transport;
SELECT count(*) = 20 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) apply at depth 126
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) apply at depth 126
\endif
ROLLBACK TO user_sp;
SELECT count(*) = 0 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) rollback at depth 126
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) rollback at depth 126
\endif
SELECT sum(cloudsync_payload_apply(payload)) FROM transport;
COMMIT;
SELECT count(*) = 20 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) reuse and commit at depth 126
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) reuse and commit at depth 126
\endif
TRUNCATE t, t_cloudsync;

BEGIN;
SELECT 'SAVEPOINT user_sp' FROM generate_series(1, 127) \gexec
-- A heap scan owns the input buffer; an aggregate consumes every apply result.
SELECT sum(cloudsync_payload_apply(payload)) FROM transport;
SELECT count(*) = 20 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) apply at depth 127
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) apply at depth 127
\endif
ROLLBACK TO user_sp;
SELECT count(*) = 0 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) rollback at depth 127
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) rollback at depth 127
\endif
SELECT sum(cloudsync_payload_apply(payload)) FROM transport;
COMMIT;
SELECT count(*) = 20 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) reuse and commit at depth 127
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) reuse and commit at depth 127
\endif
TRUNCATE t, t_cloudsync;

BEGIN;
SELECT 'SAVEPOINT user_sp' FROM generate_series(1, 128) \gexec
-- A heap scan owns the input buffer; an aggregate consumes every apply result.
SELECT sum(cloudsync_payload_apply(payload)) FROM transport;
SELECT count(*) = 20 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) apply at depth 128
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) apply at depth 128
\endif
ROLLBACK TO user_sp;
SELECT count(*) = 0 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) rollback at depth 128
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) rollback at depth 128
\endif
SELECT sum(cloudsync_payload_apply(payload)) FROM transport;
COMMIT;
SELECT count(*) = 20 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) reuse and commit at depth 128
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) reuse and commit at depth 128
\endif
TRUNCATE t, t_cloudsync;

BEGIN;
SELECT 'SAVEPOINT user_sp' FROM generate_series(1, 256) \gexec
-- A heap scan owns the input buffer; an aggregate consumes every apply result.
SELECT sum(cloudsync_payload_apply(payload)) FROM transport;
SELECT count(*) = 20 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) apply at depth 256
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) apply at depth 256
\endif
ROLLBACK TO user_sp;
SELECT count(*) = 0 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) rollback at depth 256
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) rollback at depth 256
\endif
SELECT sum(cloudsync_payload_apply(payload)) FROM transport;
COMMIT;
SELECT count(*) = 20 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) reuse and commit at depth 256
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) reuse and commit at depth 256
\endif
TRUNCATE t, t_cloudsync;

BEGIN;
SELECT 'SAVEPOINT user_sp' FROM generate_series(1, 1024) \gexec
-- A heap scan owns the input buffer; an aggregate consumes every apply result.
SELECT sum(cloudsync_payload_apply(payload)) FROM transport;
SELECT count(*) = 20 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) apply at depth 1024
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) apply at depth 1024
\endif
ROLLBACK TO user_sp;
SELECT count(*) = 0 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) rollback at depth 1024
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) rollback at depth 1024
\endif
SELECT sum(cloudsync_payload_apply(payload)) FROM transport;
COMMIT;
SELECT count(*) = 20 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) reuse and commit at depth 1024
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) reuse and commit at depth 1024
\endif
TRUNCATE t, t_cloudsync;

BEGIN;
SELECT 'SAVEPOINT user_sp' FROM generate_series(1, 2048) \gexec
-- A heap scan owns the input buffer; an aggregate consumes every apply result.
SELECT sum(cloudsync_payload_apply(payload)) FROM transport;
SELECT count(*) = 20 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) apply at depth 2048
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) apply at depth 2048
\endif
ROLLBACK TO user_sp;
SELECT count(*) = 0 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) rollback at depth 2048
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) rollback at depth 2048
\endif
SELECT sum(cloudsync_payload_apply(payload)) FROM transport;
COMMIT;
SELECT count(*) = 20 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) reuse and commit at depth 2048
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) reuse and commit at depth 2048
\endif
TRUNCATE t, t_cloudsync;

-- Catch errors outside the internal wrappers, then reuse the same backend.
CREATE FUNCTION reject_row() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN RAISE EXCEPTION 'deep apply rejected'; END $$;
CREATE TRIGGER reject_row BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION reject_row();
BEGIN;
SELECT 'SAVEPOINT user_sp' FROM generate_series(1, 256) \gexec
DO $$
BEGIN
  FOR i IN 1..100 LOOP
    BEGIN
      PERFORM cloudsync_payload_apply(payload) FROM transport;
      RAISE EXCEPTION 'expected trigger rejection';
    EXCEPTION WHEN OTHERS THEN
      IF SQLERRM NOT LIKE '%deep apply rejected%' THEN RAISE; END IF;
    END;
  END LOOP;
END $$;
DROP TRIGGER reject_row ON t;
SELECT sum(cloudsync_payload_apply(payload)) FROM transport;
COMMIT;
SELECT count(*) = 20 AS ok FROM t \gset
\if :ok
\echo [PASS] (:testid) 100 caught errors followed by successful apply
\else
SELECT (:fail::int + 1) AS fail \gset
\echo [FAIL] (:testid) error recovery
\endif
\connect postgres
DROP DATABASE cloudsync_test_62_source;
DROP DATABASE cloudsync_test_62_target;
