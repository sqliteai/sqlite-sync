-- Findings of the security review: a NULL tbl in a cloudsync_changes insert, primary key blobs
-- of the wrong shape, and trigger bodies that must not resolve cloudsync_* through the
-- search_path of whoever fires them.

\set testid '69-security-review'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_69;
CREATE DATABASE cloudsync_test_69;

\connect cloudsync_test_69
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- 1. NULL tbl / pk: an ordinary error, the backend survives.
DO $$
BEGIN
    INSERT INTO cloudsync_changes(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq)
    VALUES (NULL, '\x01', 'c', '\x05', 1, 1, '\x00112233445566778899aabbccddeeff', 1, 1);
    PERFORM set_config('cloudsync.t69', 'no-error', false);
EXCEPTION WHEN OTHERS THEN
    PERFORM set_config('cloudsync.t69', 'error', false);
END $$;
SELECT current_setting('cloudsync.t69') = 'error' AS null_tbl_ok \gset
\if :null_tbl_ok
\echo [PASS] (:testid) a NULL tbl is refused with an error
\else
\echo [FAIL] (:testid) a NULL tbl was accepted
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 2. A primary key blob must declare as many components as the table has key columns.
CREATE TABLE pkshape (a text NOT NULL, b text NOT NULL, c text, PRIMARY KEY (a, b));
SELECT cloudsync_init('pkshape') AS _ps \gset
SELECT 0 AS bad_accepted \gset
DO $$
DECLARE
    v text;
    n int := 0;
BEGIN
    FOREACH v IN ARRAY ARRAY['\x00', '\x01', '\x0101', '\x03'] LOOP
        BEGIN
            INSERT INTO cloudsync_changes(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq)
            VALUES ('pkshape', v::bytea, 'c', cloudsync_encode_value('hello'::text), 1, 1, '\x0102030405060708090a0b0c0d0e0f10', 1, 0);
            n := n + 1;
        EXCEPTION WHEN OTHERS THEN
            NULL;
        END;
    END LOOP;
    PERFORM set_config('cloudsync.t69', n::text, false);
END $$;
SELECT current_setting('cloudsync.t69')::int AS bad_accepted \gset
SELECT count(*) AS shaped_rows FROM pkshape \gset
SELECT (:bad_accepted = 0 AND :shaped_rows = 0) AS shape_ok \gset
\if :shape_ok
\echo [PASS] (:testid) primary key blobs of the wrong shape are refused
\else
\echo [FAIL] (:testid) a primary key blob of the wrong shape was accepted (:bad_accepted accepted, :shaped_rows rows)
SELECT (:fail::int + 1) AS fail \gset
\endif

INSERT INTO cloudsync_changes(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq)
SELECT 'pkshape', cloudsync_pk_encode(VARIADIC ARRAY['x','y']::text[]), 'c', cloudsync_encode_value('hello'::text), 1, 1, '\x0102030405060708090a0b0c0d0e0f10'::bytea, 1, 0;
SELECT count(*) AS good_rows FROM pkshape WHERE a = 'x' AND b = 'y' AND c = 'hello' \gset
\if :good_rows
\echo [PASS] (:testid) a primary key blob of the right shape is applied
\else
\echo [FAIL] (:testid) a primary key blob of the right shape was not applied
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 3. The generated trigger bodies call the extension's own functions, even when the session
-- that fires them has a schema of someone else first in its search_path (the functions are
-- created by the test role itself: the point is which function the body resolves).
CREATE SCHEMA evil;
CREATE TABLE evil.hijack_log (what text);
CREATE FUNCTION evil.cloudsync_is_sync(text) RETURNS boolean LANGUAGE plpgsql AS
    $f$ BEGIN INSERT INTO evil.hijack_log VALUES ('is_sync'); RETURN true; END $f$;
CREATE FUNCTION evil.cloudsync_is_enabled(text) RETURNS boolean LANGUAGE plpgsql AS
    $f$ BEGIN INSERT INTO evil.hijack_log VALUES ('is_enabled'); RETURN false; END $f$;
CREATE FUNCTION evil.cloudsync_insert(text, text) RETURNS boolean LANGUAGE plpgsql AS
    $f$ BEGIN INSERT INTO evil.hijack_log VALUES ('insert'); RETURN true; END $f$;
CREATE FUNCTION evil.cloudsync_delete(text, text) RETURNS boolean LANGUAGE plpgsql AS
    $f$ BEGIN INSERT INTO evil.hijack_log VALUES ('delete'); RETURN true; END $f$;

CREATE TABLE victim (id text PRIMARY KEY NOT NULL, v text);
SELECT cloudsync_init('victim') AS _v \gset
SET search_path = evil, public;
INSERT INTO victim VALUES ('1', 'a');
UPDATE victim SET v = 'b';
DELETE FROM victim;
RESET search_path;

SELECT count(*) AS hijacked FROM evil.hijack_log \gset
SELECT count(*) AS tracked FROM victim_cloudsync \gset
SELECT (:hijacked = 0 AND :tracked > 0) AS hijack_ok \gset
\if :hijack_ok
\echo [PASS] (:testid) trigger bodies do not resolve cloudsync_* through the firing search_path
\else
\echo [FAIL] (:testid) a function from another schema ran inside the trigger (:hijacked calls, :tracked tracked rows)
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_69;
