-- Medium findings of the security review: a same-named trigger on another table must not
-- stop cloudsync_init from tracking, lookalike *_cloudsync tables must not poison
-- cloudsync_db_version()/cloudsync_changes, and another role's DDL must keep working once
-- the schema-version event trigger exists.

\set testid '70-medium-review'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_70;
CREATE DATABASE cloudsync_test_70;

\connect cloudsync_test_70
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- 1. A trigger of the same name on somebody else's table.
CREATE SCHEMA other;
CREATE TABLE other.victim (id text PRIMARY KEY NOT NULL, v text);
CREATE FUNCTION other.noop() RETURNS trigger LANGUAGE plpgsql AS $f$ BEGIN RETURN NEW; END $f$;
CREATE TRIGGER cloudsync_after_insert_victim AFTER INSERT ON other.victim FOR EACH ROW EXECUTE FUNCTION other.noop();
CREATE TRIGGER cloudsync_after_update_victim AFTER UPDATE ON other.victim FOR EACH ROW EXECUTE FUNCTION other.noop();
CREATE TRIGGER cloudsync_after_delete_victim AFTER DELETE ON other.victim FOR EACH ROW EXECUTE FUNCTION other.noop();

CREATE TABLE public.victim (id text PRIMARY KEY NOT NULL, v text);
SELECT cloudsync_init('victim') AS _v \gset
INSERT INTO public.victim VALUES ('1', 'a');
UPDATE public.victim SET v = 'b';
SELECT count(*) AS own_triggers FROM pg_trigger
WHERE tgrelid = 'public.victim'::regclass AND tgname LIKE 'cloudsync_after_%' \gset
SELECT count(*) AS tracked FROM public.victim_cloudsync \gset
SELECT (:own_triggers >= 3 AND :tracked > 0) AS trigger_ok \gset
\if :trigger_ok
\echo [PASS] (:testid) a same-named trigger on another table does not stop tracking
\else
\echo [FAIL] (:testid) tracking was lost (:own_triggers triggers on the table, :tracked tracked rows)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 2. Lookalike metadata tables: wrong shape, and right shape without a base table.
CREATE SCHEMA lookalike;
CREATE TABLE lookalike.junk_cloudsync (anything text);
CREATE TABLE lookalike.orphan_cloudsync (
    pk bytea NOT NULL, col_name text NOT NULL, col_version bigint, db_version bigint NOT NULL DEFAULT 999999,
    seq integer NOT NULL DEFAULT 0, site_id bigint NOT NULL DEFAULT 0, PRIMARY KEY (pk, col_name));
INSERT INTO lookalike.orphan_cloudsync (pk, col_name, col_version) VALUES ('\x01', 'c', 1);

DO $$
DECLARE
    v bigint;
    n bigint;
BEGIN
    SELECT cloudsync_db_version() INTO v;
    SELECT count(*) INTO n FROM cloudsync_changes;
    PERFORM set_config('cloudsync.t70', v::text || ',' || n::text, false);
EXCEPTION WHEN OTHERS THEN
    PERFORM set_config('cloudsync.t70', 'error: ' || SQLERRM, false);
END $$;
SELECT current_setting('cloudsync.t70') AS lookalike_result \gset
SELECT (:'lookalike_result' ~ '^[0-9]+,[0-9]+$' AND split_part(:'lookalike_result', ',', 1)::bigint < 999999) AS lookalike_ok \gset
\if :lookalike_ok
\echo [PASS] (:testid) lookalike *_cloudsync tables are ignored
\else
\echo [FAIL] (:testid) a lookalike table was used: :lookalike_result
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 3. Another role's DDL after the event trigger exists, and its effect on the version.
CREATE ROLE cs70_user NOLOGIN;
CREATE SCHEMA s70 AUTHORIZATION cs70_user;
SELECT COALESCE(max(version), 0) AS v_before FROM public.app_schema_version \gset
SET ROLE cs70_user;
DO $$
BEGIN
    CREATE TABLE s70.t (x int);
    PERFORM set_config('cloudsync.t70', 'ok', false);
EXCEPTION WHEN OTHERS THEN
    PERFORM set_config('cloudsync.t70', 'error: ' || SQLERRM, false);
END $$;
RESET ROLE;
SELECT current_setting('cloudsync.t70') AS ddl_result \gset
SELECT COALESCE(max(version), 0) AS v_after FROM public.app_schema_version \gset
SELECT (:'ddl_result' = 'ok' AND :v_after > :v_before) AS ddl_ok \gset
\if :ddl_ok
\echo [PASS] (:testid) DDL by another role works and bumps the schema version
\else
\echo [FAIL] (:testid) DDL by another role: :ddl_result (version :v_before -> :v_after)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 4. cloudsync_init is idempotent for a role that does not own the event trigger.
GRANT USAGE ON SCHEMA public TO cs70_user;
GRANT CREATE ON SCHEMA public TO cs70_user;
GRANT ALL ON ALL TABLES IN SCHEMA public TO cs70_user;
GRANT ALL ON ALL SEQUENCES IN SCHEMA public TO cs70_user;
CREATE TABLE public.mine (id text PRIMARY KEY NOT NULL, v text);
ALTER TABLE public.mine OWNER TO cs70_user;
SET ROLE cs70_user;
DO $$
BEGIN
    PERFORM cloudsync_init('mine');
    PERFORM set_config('cloudsync.t70', 'ok', false);
EXCEPTION WHEN OTHERS THEN
    PERFORM set_config('cloudsync.t70', 'error: ' || SQLERRM, false);
END $$;
RESET ROLE;
SELECT current_setting('cloudsync.t70') AS init_result \gset
SELECT (:'init_result' = 'ok') AS init_ok \gset
\if :init_ok
\echo [PASS] (:testid) a non-owner role can run cloudsync_init
\else
\echo [FAIL] (:testid) cloudsync_init by another role: :init_result
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_70;
DROP ROLE IF EXISTS cs70_user;
