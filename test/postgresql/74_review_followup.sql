-- Follow-up review regressions: which metadata tables count toward db_version and
-- cloudsync_changes, and clock bounds on grow-only tables.

\set testid '74-review-followup'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_74;
DROP ROLE IF EXISTS cs74_owner;
CREATE DATABASE cloudsync_test_74;
CREATE ROLE cs74_owner NOLOGIN;

\connect cloudsync_test_74
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- 1. A synced table owned by another role, initialized by this one.
CREATE TABLE public.owned (id text PRIMARY KEY NOT NULL, v text);
ALTER TABLE public.owned OWNER TO cs74_owner;
SELECT cloudsync_init('owned') AS _init \gset
INSERT INTO public.owned VALUES ('r1', 'a');
INSERT INTO public.owned VALUES ('r2', 'b');
SELECT cloudsync_db_version() AS owned_dbv \gset
SELECT count(*) AS owned_changes FROM cloudsync_changes WHERE tbl = 'owned' \gset
SELECT (:owned_dbv = 2 AND :owned_changes = 2) AS owned_ok \gset
\if :owned_ok
\echo [PASS] (:testid) a table owned by another role counts toward db_version and changes
\else
\echo [FAIL] (:testid) table owned by another role: db_version :owned_dbv, changes :owned_changes
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 2. An unregistered table with the metadata shape beside a real base table is ignored.
CREATE TABLE public.junk (id text PRIMARY KEY NOT NULL);
CREATE TABLE public.junk_cloudsync (
    pk bytea NOT NULL, col_name text NOT NULL, col_version bigint, db_version bigint NOT NULL DEFAULT 999999,
    seq integer NOT NULL DEFAULT 0, site_id bigint NOT NULL DEFAULT 0, PRIMARY KEY (pk, col_name));
INSERT INTO public.junk_cloudsync (pk, col_name, col_version) VALUES ('\x01', 'c', 1);
\connect cloudsync_test_74
\ir helper_psql_conn_setup.sql
SELECT cloudsync_db_version() AS junk_dbv \gset
SELECT count(*) AS junk_changes FROM cloudsync_changes \gset
SELECT (:junk_dbv = 2 AND :junk_changes = 2) AS junk_ok \gset
\if :junk_ok
\echo [PASS] (:testid) an unregistered lookalike metadata table is ignored
\else
\echo [FAIL] (:testid) lookalike used: db_version :junk_dbv, changes :junk_changes
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 3. Dropping a synced table without cleanup does not move db_version back.
CREATE TABLE public.gone (id text PRIMARY KEY NOT NULL, v text);
SELECT cloudsync_init('gone') AS _init_gone \gset
INSERT INTO public.gone VALUES ('g1', 'x');
INSERT INTO public.gone VALUES ('g2', 'x');
SELECT cloudsync_db_version() AS gone_before \gset
DROP TABLE public.gone;
\connect cloudsync_test_74
\ir helper_psql_conn_setup.sql
SELECT cloudsync_db_version() AS gone_after \gset
SELECT count(*) AS gone_changes FROM cloudsync_changes \gset
SELECT (:gone_before = 4 AND :gone_after = 4 AND :gone_changes = 2) AS gone_ok \gset
\if :gone_ok
\echo [PASS] (:testid) a dropped synced table keeps db_version from going back
\else
\echo [FAIL] (:testid) dropped table: db_version :gone_before -> :gone_after, changes :gone_changes
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 4. Out-of-range clocks are refused on a grow-only table too.
CREATE TABLE public.grow (id text PRIMARY KEY NOT NULL, v text);
SELECT cloudsync_init('grow', 'gos') AS _init_grow \gset
DO $$
DECLARE
    clocks bigint[][] := ARRAY[[9223372036854775000, 1, 0], [1, 9223372036854775000, 0],
                               [1, 1, 9223372036854775000], [-1, 1, 0], [1, -1, 0]];
    accepted int := 0;
BEGIN
    FOR i IN 1..array_length(clocks, 1) LOOP
        BEGIN
            INSERT INTO cloudsync_changes(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq)
            VALUES ('grow', cloudsync_pk_encode('r1'), 'v', cloudsync_encode_value('x'::text),
                    clocks[i][1], clocks[i][2], '\x0102030405060708090a0b0c0d0e0f10', 1, clocks[i][3]);
            accepted := accepted + 1;
        EXCEPTION WHEN OTHERS THEN
            NULL;
        END;
    END LOOP;
    PERFORM set_config('cloudsync.t74', accepted::text, false);
END $$;
SELECT current_setting('cloudsync.t74') AS grow_accepted \gset
SELECT count(*) AS grow_rows FROM public.grow \gset
SELECT (:grow_accepted = 0 AND :grow_rows = 0 AND cloudsync_db_version() < 1000) AS grow_ok \gset
\if :grow_ok
\echo [PASS] (:testid) out-of-range clocks are refused on a grow-only table
\else
\echo [FAIL] (:testid) grow-only table accepted :grow_accepted out-of-range changes (:grow_rows rows)
SELECT (:fail::int + 1) AS fail \gset
\endif

\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_74;
DROP ROLE IF EXISTS cs74_owner;
\endif
