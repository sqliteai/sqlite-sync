-- Pieces of one fragmented value applied by concurrent transactions (the server runs
-- one apply job per uploaded chunk). Each transaction must not see only its own piece
-- and succeed, leaving a complete value unapplied; and the stale cleanup must not
-- remove the pieces of a value another transaction is reconstructing.

\set testid '60-fragment-concurrency'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_60_src;
DROP DATABASE IF EXISTS cloudsync_test_60_dst;
CREATE DATABASE cloudsync_test_60_src;
CREATE DATABASE cloudsync_test_60_dst;

-- Source: 'two' splits into two pieces, 'three' into three.
\connect cloudsync_test_60_src
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, value TEXT);
SELECT cloudsync_init('t') AS _init \gset
-- a size of 1 clamps to the minimum chunk size
SELECT cloudsync_set('payload_max_chunk_size', '1') AS _size \gset
INSERT INTO t VALUES ('two', repeat('A', 300000));
SELECT string_agg(encode(payload, 'hex'), ',' ORDER BY chunk_index) AS two_hex FROM cloudsync_payload_chunks() \gset
DELETE FROM t;
SELECT max(db_version) AS dbv FROM cloudsync_changes \gset
INSERT INTO t VALUES ('three', repeat('B', 600000));
SELECT string_agg(encode(payload, 'hex'), ',' ORDER BY chunk_index) AS three_hex
FROM cloudsync_payload_chunks(:dbv) WHERE get_byte(payload, 4) = 3 \gset

\connect cloudsync_test_60_dst
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE EXTENSION IF NOT EXISTS dblink;
CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, value TEXT);
SELECT cloudsync_init('t') AS _init \gset
CREATE TABLE transport(name TEXT, part INT, payload BYTEA, PRIMARY KEY (name, part));
INSERT INTO transport SELECT 'two', ord, decode(h, 'hex') FROM unnest(string_to_array(:'two_hex', ',')) WITH ORDINALITY AS x(h, ord);
INSERT INTO transport SELECT 'three', ord, decode(h, 'hex') FROM unnest(string_to_array(:'three_hex', ',')) WITH ORDINALITY AS x(h, ord);
CREATE FUNCTION apply_part(n TEXT, p INT) RETURNS INT LANGUAGE plpgsql AS $$
DECLARE data BYTEA; BEGIN
    SELECT payload INTO STRICT data FROM transport WHERE name = n AND part = p;
    RETURN cloudsync_payload_apply(data);
END $$;
-- Returns once session y has finished or is waiting on a lock, so the test never
-- relies on a sleep to make the two transactions overlap.
CREATE FUNCTION wait_for_y() RETURNS TEXT LANGUAGE plpgsql AS $$
DECLARE state TEXT; BEGIN
    FOR i IN 1..500 LOOP
        IF dblink_is_busy('y') = 0 THEN RETURN 'finished'; END IF;
        PERFORM pg_stat_clear_snapshot();
        SELECT wait_event_type INTO state FROM pg_stat_activity WHERE application_name = 'cloudsync_60_y';
        IF state = 'Lock' THEN RETURN 'waiting'; END IF;
        PERFORM pg_sleep(0.01);
    END LOOP;
    RETURN 'timeout';
END $$;

SELECT (SELECT count(*) FROM transport WHERE name = 'two') = 2
   AND (SELECT count(*) FROM transport WHERE name = 'three') = 3 AS parts_ok \gset
\if :parts_ok
\echo [PASS] (:testid) values split into 2 and 3 fragments
\else
\echo [FAIL] (:testid) unexpected fragment counts
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT dblink_connect('x', format('dbname=%s user=%s application_name=cloudsync_60_x', current_database(), current_user)) AS _cx \gset
SELECT dblink_connect('y', format('dbname=%s user=%s application_name=cloudsync_60_y', current_database(), current_user)) AS _cy \gset
-- Warm both sessions as a long-running worker would be: their first cloudsync call
-- initializes the context, and an open transaction holding those writes would make
-- the other session wait for reasons unrelated to the fragments.
SELECT n AS _n FROM dblink('x', 'SELECT apply_part(''two'', 1)') AS r(n INT) \gset
SELECT n AS _n FROM dblink('y', 'SELECT apply_part(''two'', 1)') AS r(n INT) \gset
DELETE FROM cloudsync_payload_fragments;

-- 1. READ COMMITTED: x stages piece 1 and stays open while y applies piece 2.
SELECT dblink_exec('x', 'BEGIN') AS _b \gset
SELECT n AS _n FROM dblink('x', 'SELECT apply_part(''two'', 1)') AS r(n INT) \gset
SELECT dblink_send_query('y', 'SELECT apply_part(''two'', 2)') AS _s \gset
SELECT wait_for_y() AS y_state \gset
SELECT dblink_exec('x', 'COMMIT') AS _c \gset
SELECT count(*) AS _n FROM dblink_get_result('y', false) AS r(n INT) \gset
SELECT count(*) AS _drain FROM dblink_get_result('y', false) AS r(n INT) \gset
SELECT (SELECT length(value) FROM t WHERE id = 'two') IS NOT DISTINCT FROM 300000
   AND NOT EXISTS (SELECT FROM cloudsync_payload_fragments) AS rc_ok \gset
\if :rc_ok
\echo [PASS] (:testid) READ COMMITTED: the value applies once both pieces commit (y :y_state)
\else
\echo [FAIL] (:testid) READ COMMITTED: both pieces committed but the value was not applied (y :y_state)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 2. SERIALIZABLE: the same overlap either applies the value or fails one transaction
-- with a retryable serialization failure. Never both succeed without the value.
DELETE FROM t; DELETE FROM t_cloudsync; DELETE FROM cloudsync_payload_fragments;
SELECT dblink_exec('x', 'BEGIN ISOLATION LEVEL SERIALIZABLE') AS _b \gset
SELECT n AS _n FROM dblink('x', 'SELECT apply_part(''two'', 1)') AS r(n INT) \gset
SELECT dblink_exec('y', 'BEGIN ISOLATION LEVEL SERIALIZABLE') AS _b \gset
SELECT dblink_send_query('y', 'SELECT apply_part(''two'', 2)') AS _s \gset
SELECT wait_for_y() AS y_state \gset
SELECT dblink_exec('x', 'COMMIT', false) AS x_commit \gset
SELECT count(*) AS _n FROM dblink_get_result('y', false) AS r(n INT) \gset
SELECT count(*) AS _drain FROM dblink_get_result('y', false) AS r(n INT) \gset
SELECT dblink_error_message('y') AS y_apply_error \gset
SELECT dblink_exec('y', 'COMMIT', false) AS y_commit \gset
SELECT dblink_error_message('y') AS y_commit_error \gset
SELECT dblink_exec('y', 'ROLLBACK', false) AS _r \gset
SELECT (SELECT length(value) FROM t WHERE id = 'two') IS NOT DISTINCT FROM 300000 AS ser_applied \gset
SELECT (:'y_apply_error' LIKE '%could not serialize%' OR :'y_commit_error' LIKE '%could not serialize%') AS ser_failed \gset
SELECT (:'ser_applied' OR :'ser_failed') AS ser_ok \gset
\if :ser_ok
\echo [PASS] (:testid) SERIALIZABLE: the overlap applies the value or fails with a serialization error
\else
\echo [FAIL] (:testid) SERIALIZABLE: both transactions succeeded without applying the value (apply: :y_apply_error, commit: :y_commit_error)
SELECT (:fail::int + 1) AS fail \gset
\endif
-- a retry of the failed piece completes the value
SELECT n AS _n FROM dblink('y', 'SELECT apply_part(''two'', 2)') AS r(n INT) \gset
SELECT (SELECT length(value) FROM t WHERE id = 'two') IS NOT DISTINCT FROM 300000
   AND NOT EXISTS (SELECT FROM cloudsync_payload_fragments) AS ser_retry_ok \gset
\if :ser_retry_ok
\echo [PASS] (:testid) SERIALIZABLE: redelivering the piece completes the value
\else
\echo [FAIL] (:testid) SERIALIZABLE: the value is still missing after redelivery
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 3. REPEATABLE READ keeps the snapshot taken before the wait, so a fragment apply
-- there is refused instead of risking a value that is never applied.
DELETE FROM t; DELETE FROM t_cloudsync; DELETE FROM cloudsync_payload_fragments;
SELECT dblink_exec('y', 'BEGIN ISOLATION LEVEL REPEATABLE READ') AS _b \gset
SELECT count(*) AS _n FROM dblink('y', 'SELECT apply_part(''two'', 1)', false) AS r(n INT) \gset
SELECT dblink_error_message('y') AS rr_error \gset
SELECT dblink_exec('y', 'ROLLBACK', false) AS _r \gset
SELECT (:'rr_error' LIKE '%REPEATABLE READ%') AS rr_ok \gset
\if :rr_ok
\echo [PASS] (:testid) REPEATABLE READ: a fragment apply is refused
\else
\echo [FAIL] (:testid) REPEATABLE READ: expected a refusal, got: :rr_error
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 4. The stale cleanup must not remove the old pieces of a value that another
-- transaction is resuming: x stages piece 2 of 'three' (piece 1 is two days old)
-- and stays open while a fresh session z runs its first fragment call, which runs
-- the cleanup. After x commits, piece 3 completes the value.
DELETE FROM t; DELETE FROM t_cloudsync; DELETE FROM cloudsync_payload_fragments;
SELECT n AS _n FROM dblink('y', 'SELECT apply_part(''three'', 1)') AS r(n INT) \gset
UPDATE cloudsync_payload_fragments SET created_at = created_at - 172800;
SELECT dblink_exec('x', 'BEGIN') AS _b \gset
SELECT n AS _n FROM dblink('x', 'SELECT apply_part(''three'', 2)') AS r(n INT) \gset
SELECT dblink_connect('z', format('dbname=%s user=%s application_name=cloudsync_60_z', current_database(), current_user)) AS _cz \gset
SELECT n AS _n FROM dblink('z', 'SELECT apply_part(''two'', 1)') AS r(n INT) \gset
SELECT dblink_exec('x', 'COMMIT') AS _c \gset
SELECT n AS _n FROM dblink('y', 'SELECT apply_part(''three'', 3)') AS r(n INT) \gset
SELECT (SELECT length(value) FROM t WHERE id = 'three') IS NOT DISTINCT FROM 600000 AS cleanup_ok \gset
\if :cleanup_ok
\echo [PASS] (:testid) cleanup keeps the pieces of a value being reconstructed
\else
\echo [FAIL] (:testid) cleanup removed the old piece of a value being resumed
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT dblink_disconnect('x') AS _dx \gset
SELECT dblink_disconnect('y') AS _dy \gset
SELECT dblink_disconnect('z') AS _dz \gset

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_60_src;
DROP DATABASE IF EXISTS cloudsync_test_60_dst;
