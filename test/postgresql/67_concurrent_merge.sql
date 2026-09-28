-- Issue #70: two merge decisions must not use the same stale row clocks.
-- Run with psql -v ON_ERROR_STOP=1 -f test/postgresql/67_concurrent_merge.sql.
-- dblink observes the waiter before releasing x; no timing-dependent overlap.
\set ON_ERROR_STOP on
\set testid '67-concurrent-merge'
\ir helper_test_init.sql
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_67;
CREATE DATABASE cloudsync_test_67;
\connect cloudsync_test_67
\ir helper_psql_conn_setup.sql
CREATE EXTENSION cloudsync;
CREATE EXTENSION dblink;
CREATE TABLE t(id TEXT PRIMARY KEY, v TEXT);
SELECT cloudsync_init('t') AS _init \gset
CREATE TABLE transport(name TEXT PRIMARY KEY, payload BYTEA);
INSERT INTO t VALUES ('row', 'higher');
INSERT INTO transport SELECT 'higher', cloudsync_payload_encode(tbl, pk, col_name,
    col_value, 3, 3, decode(repeat('01',16),'hex'), 1, 0) FROM cloudsync_changes;
UPDATE t SET v='lower';
INSERT INTO transport SELECT 'lower', cloudsync_payload_encode(tbl, pk, col_name,
    col_value, 2, 2, decode(repeat('02',16),'hex'), 1, 0) FROM cloudsync_changes;
TRUNCATE t, t_cloudsync;
CREATE FUNCTION apply_value(n TEXT) RETURNS INT LANGUAGE plpgsql AS $$
DECLARE data BYTEA; BEGIN
    SELECT payload INTO STRICT data FROM transport WHERE name=n;
    RETURN cloudsync_payload_apply(data);
END $$;
CREATE FUNCTION assert_winner(label TEXT) RETURNS VOID LANGUAGE plpgsql AS $$
DECLARE val TEXT; ver BIGINT; BEGIN
    SELECT v INTO val FROM t WHERE id='row';
    SELECT col_version INTO ver FROM t_cloudsync WHERE pk=cloudsync_pk_encode('row'::text) AND col_name='v';
    IF val IS DISTINCT FROM 'higher' OR ver IS DISTINCT FROM 3::bigint THEN
        RAISE EXCEPTION '%: expected higher/3, got %/%', label, val, ver;
    END IF;
END $$;
SELECT apply_value('lower');
SELECT apply_value('higher');
SELECT assert_winner('serial lower then higher');
TRUNCATE t, t_cloudsync;
SELECT apply_value('higher');
SELECT apply_value('lower');
SELECT assert_winner('serial higher then lower');
SELECT dblink_connect('x', format('dbname=%s user=%s application_name=cloudsync_67_x',current_database(),current_user));
SELECT dblink_connect('y', format('dbname=%s user=%s application_name=cloudsync_67_y',current_database(),current_user));
-- Initialize both worker contexts outside the contending transactions.
SELECT * FROM dblink('x', 'SELECT apply_value(''higher'')') AS r(n INT);
SELECT * FROM dblink('y', 'SELECT apply_value(''higher'')') AS r(n INT);
SELECT dblink_exec('x', 'SET statement_timeout=''15s''');
SELECT dblink_exec('y', 'SET statement_timeout=''15s''');
CREATE FUNCTION wait_for_y() RETURNS VOID LANGUAGE plpgsql AS $$
BEGIN
    FOR i IN 1..500 LOOP
        PERFORM pg_stat_clear_snapshot();
        IF EXISTS (SELECT FROM pg_stat_activity WHERE application_name='cloudsync_67_y' AND wait_event_type='Lock') THEN RETURN; END IF;
        IF dblink_is_busy('y')=0 THEN RAISE EXCEPTION 'y finished without waiting'; END IF;
        PERFORM pg_sleep(0.01);
    END LOOP;
    RAISE EXCEPTION 'timeout waiting for y';
END $$;
-- Fresh and existing rows; both arrival orders; rollback must release the lock too.
-- seeded=False, first=higher, rollback=False
TRUNCATE t, t_cloudsync;

SELECT dblink_exec('x','BEGIN');
SELECT * FROM dblink('x','SELECT apply_value(''higher'')') AS r(n INT);
SELECT dblink_send_query('y','SELECT apply_value(''lower'')');
SELECT wait_for_y();
SELECT dblink_exec('x','COMMIT');
SELECT * FROM dblink_get_result('y') AS r(n INT);
SELECT * FROM dblink_get_result('y') AS r(n INT);

SELECT assert_winner('seeded=False first=higher rollback=False');

-- seeded=False, first=higher, rollback=True
TRUNCATE t, t_cloudsync;

SELECT dblink_exec('x','BEGIN');
SELECT * FROM dblink('x','SELECT apply_value(''higher'')') AS r(n INT);
SELECT dblink_send_query('y','SELECT apply_value(''lower'')');
SELECT wait_for_y();
SELECT dblink_exec('x','ROLLBACK');
SELECT * FROM dblink_get_result('y') AS r(n INT);
SELECT * FROM dblink_get_result('y') AS r(n INT);
SELECT apply_value('higher');
SELECT assert_winner('seeded=False first=higher rollback=True');

-- seeded=False, first=lower, rollback=False
TRUNCATE t, t_cloudsync;

SELECT dblink_exec('x','BEGIN');
SELECT * FROM dblink('x','SELECT apply_value(''lower'')') AS r(n INT);
SELECT dblink_send_query('y','SELECT apply_value(''higher'')');
SELECT wait_for_y();
SELECT dblink_exec('x','COMMIT');
SELECT * FROM dblink_get_result('y') AS r(n INT);
SELECT * FROM dblink_get_result('y') AS r(n INT);

SELECT assert_winner('seeded=False first=lower rollback=False');

-- seeded=False, first=lower, rollback=True
TRUNCATE t, t_cloudsync;

SELECT dblink_exec('x','BEGIN');
SELECT * FROM dblink('x','SELECT apply_value(''lower'')') AS r(n INT);
SELECT dblink_send_query('y','SELECT apply_value(''higher'')');
SELECT wait_for_y();
SELECT dblink_exec('x','ROLLBACK');
SELECT * FROM dblink_get_result('y') AS r(n INT);
SELECT * FROM dblink_get_result('y') AS r(n INT);
SELECT apply_value('lower');
SELECT assert_winner('seeded=False first=lower rollback=True');

-- seeded=True, first=higher, rollback=False
TRUNCATE t, t_cloudsync;
SELECT apply_value('lower');
SELECT dblink_exec('x','BEGIN');
SELECT * FROM dblink('x','SELECT apply_value(''higher'')') AS r(n INT);
SELECT dblink_send_query('y','SELECT apply_value(''lower'')');
SELECT wait_for_y();
SELECT dblink_exec('x','COMMIT');
SELECT * FROM dblink_get_result('y') AS r(n INT);
SELECT * FROM dblink_get_result('y') AS r(n INT);

SELECT assert_winner('seeded=True first=higher rollback=False');

-- seeded=True, first=higher, rollback=True
TRUNCATE t, t_cloudsync;
SELECT apply_value('lower');
SELECT dblink_exec('x','BEGIN');
SELECT * FROM dblink('x','SELECT apply_value(''higher'')') AS r(n INT);
SELECT dblink_send_query('y','SELECT apply_value(''lower'')');
SELECT wait_for_y();
SELECT dblink_exec('x','ROLLBACK');
SELECT * FROM dblink_get_result('y') AS r(n INT);
SELECT * FROM dblink_get_result('y') AS r(n INT);
SELECT apply_value('higher');
SELECT assert_winner('seeded=True first=higher rollback=True');

-- seeded=True, first=lower, rollback=False
TRUNCATE t, t_cloudsync;
SELECT apply_value('lower');
SELECT dblink_exec('x','BEGIN');
SELECT * FROM dblink('x','SELECT apply_value(''lower'')') AS r(n INT);
SELECT dblink_send_query('y','SELECT apply_value(''higher'')');
SELECT wait_for_y();
SELECT dblink_exec('x','COMMIT');
SELECT * FROM dblink_get_result('y') AS r(n INT);
SELECT * FROM dblink_get_result('y') AS r(n INT);

SELECT assert_winner('seeded=True first=lower rollback=False');

-- seeded=True, first=lower, rollback=True
TRUNCATE t, t_cloudsync;
SELECT apply_value('lower');
SELECT dblink_exec('x','BEGIN');
SELECT * FROM dblink('x','SELECT apply_value(''lower'')') AS r(n INT);
SELECT dblink_send_query('y','SELECT apply_value(''higher'')');
SELECT wait_for_y();
SELECT dblink_exec('x','ROLLBACK');
SELECT * FROM dblink_get_result('y') AS r(n INT);
SELECT * FROM dblink_get_result('y') AS r(n INT);
SELECT apply_value('lower');
SELECT assert_winner('seeded=True first=lower rollback=True');
-- REPEATABLE READ cannot refresh a snapshot after a lock wait. Preserve SQLSTATE.
CREATE FUNCTION apply_checked(n TEXT) RETURNS TEXT LANGUAGE plpgsql AS $$
BEGIN
    PERFORM apply_value(n);
    RETURN 'ok';
EXCEPTION WHEN OTHERS THEN RETURN SQLSTATE;
END $$;
BEGIN ISOLATION LEVEL REPEATABLE READ;
DO $$ BEGIN
    IF apply_checked('lower') <> '0A000' THEN RAISE EXCEPTION 'expected REPEATABLE READ refusal'; END IF;
END $$;
ROLLBACK;

-- SERIALIZABLE must abort the stale writer. Retrying then converges.
TRUNCATE t, t_cloudsync;
SELECT dblink_exec('x','BEGIN ISOLATION LEVEL SERIALIZABLE');
SELECT * FROM dblink('x','SELECT apply_value(''higher'')') AS r(n INT);
SELECT dblink_exec('y','BEGIN ISOLATION LEVEL SERIALIZABLE');
SELECT dblink_send_query('y','SELECT apply_checked(''lower'')');
SELECT wait_for_y();
SELECT dblink_exec('x','COMMIT');
SELECT state = '40001' AS serialization_ok FROM dblink_get_result('y') AS r(state TEXT) \gset
SELECT * FROM dblink_get_result('y') AS r(state TEXT);
SELECT dblink_exec('y','ROLLBACK');
\if :serialization_ok
\else
DO $$ BEGIN RAISE EXCEPTION 'expected SQLSTATE 40001'; END $$;
\endif
SELECT apply_value('lower');
SELECT assert_winner('SERIALIZABLE retry');

-- The direct cloudsync_changes API uses the same protection. Applying many rows
-- in one transaction must not allocate one advisory lock for each distinct PK.
CREATE TEMP TABLE encoded_value AS SELECT col_value FROM cloudsync_changes WHERE tbl='t' AND col_name='v';
BEGIN;
INSERT INTO cloudsync_changes(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq)
SELECT 't',cloudsync_pk_encode('bulk-' || i), 'v', e.col_value, 3, 3,
       decode(repeat('01',16),'hex'), 1, i FROM generate_series(1,10000) AS g(i) CROSS JOIN encoded_value e;
DO $$ DECLARE n INT; BEGIN
    SELECT count(*) INTO n FROM pg_locks WHERE pid=pg_backend_pid() AND locktype='advisory'
       AND classid=1129530963 AND objsubid=2;
    IF n=0 OR n>256 THEN RAISE EXCEPTION 'unbounded or missing row locks: %',n; END IF;
    IF (SELECT count(*) FROM t WHERE id LIKE 'bulk-%')<>10000 THEN RAISE EXCEPTION 'bulk rows missing'; END IF;
END $$;
ROLLBACK;
DO $$ BEGIN
    IF EXISTS (SELECT FROM pg_locks WHERE pid=pg_backend_pid() AND locktype='advisory' AND classid=1129530963)
    THEN RAISE EXCEPTION 'row locks survived rollback'; END IF;
END $$;
\echo [PASS] (:testid) isolation errors, retry and bounded locks for 10000 rows

SELECT dblink_disconnect('x');
SELECT dblink_disconnect('y');
\echo [PASS] (:testid) serial and concurrent merges converge on higher/3
\connect postgres
DROP DATABASE cloudsync_test_67;
