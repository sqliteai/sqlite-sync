-- Low findings of the security review: long table names sharing a 63-byte prefix must not
-- share trigger functions, and column names made of quotes must not truncate the SQL that
-- applies a payload.

\set testid '71-low-review'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_71_src;
DROP DATABASE IF EXISTS cloudsync_test_71_qsrc;
DROP DATABASE IF EXISTS cloudsync_test_71_dst;
CREATE DATABASE cloudsync_test_71_src;
CREATE DATABASE cloudsync_test_71_dst;

\connect cloudsync_test_71_src
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- 1. Two long names with the same first 63 bytes of trigger name.
CREATE TABLE abcdefghijklmnopqrstuvwxyzabcdefghijklmn_one (id text PRIMARY KEY NOT NULL, v text);
CREATE TABLE abcdefghijklmnopqrstuvwxyzabcdefghijklmn_two (id text PRIMARY KEY NOT NULL, v text);
SELECT cloudsync_init('abcdefghijklmnopqrstuvwxyzabcdefghijklmn_one') AS _a \gset
SELECT cloudsync_init('abcdefghijklmnopqrstuvwxyzabcdefghijklmn_two') AS _b \gset
INSERT INTO abcdefghijklmnopqrstuvwxyzabcdefghijklmn_one VALUES ('1', 'x');
UPDATE abcdefghijklmnopqrstuvwxyzabcdefghijklmn_one SET v = 'y';
INSERT INTO abcdefghijklmnopqrstuvwxyzabcdefghijklmn_two VALUES ('2', 'x');
SELECT count(*) AS one_rows FROM abcdefghijklmnopqrstuvwxyzabcdefghijklmn_one_cloudsync \gset
SELECT count(*) AS two_rows FROM abcdefghijklmnopqrstuvwxyzabcdefghijklmn_two_cloudsync \gset
SELECT count(DISTINCT tgfoid) AS functions FROM pg_trigger
WHERE tgrelid IN ('abcdefghijklmnopqrstuvwxyzabcdefghijklmn_one'::regclass, 'abcdefghijklmnopqrstuvwxyzabcdefghijklmn_two'::regclass)
  AND tgname LIKE 'cloudsync_after_insert_%' \gset
SELECT (:one_rows > 0 AND :two_rows > 0 AND :functions = 2) AS collide_ok \gset
\if :collide_ok
\echo [PASS] (:testid) long table names get their own trigger functions
\else
\echo [FAIL] (:testid) long names collided (:one_rows / :two_rows tracked rows, :functions distinct insert functions)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- dropping one table's tracking leaves the other's alone
SELECT cloudsync_cleanup('abcdefghijklmnopqrstuvwxyzabcdefghijklmn_one') AS _c \gset
SELECT count(*) AS left_triggers FROM pg_trigger
WHERE tgrelid = 'abcdefghijklmnopqrstuvwxyzabcdefghijklmn_two'::regclass AND tgname LIKE 'cloudsync_%' \gset
SELECT count(*) AS gone_triggers FROM pg_trigger
WHERE tgrelid = 'abcdefghijklmnopqrstuvwxyzabcdefghijklmn_one'::regclass AND tgname LIKE 'cloudsync_%' \gset
INSERT INTO abcdefghijklmnopqrstuvwxyzabcdefghijklmn_two VALUES ('3', 'x');
SELECT count(*) AS two_after FROM abcdefghijklmnopqrstuvwxyzabcdefghijklmn_two_cloudsync \gset
SELECT (:left_triggers >= 3 AND :gone_triggers = 0 AND :two_after > :two_rows) AS cleanup_ok \gset
\if :cleanup_ok
\echo [PASS] (:testid) cleanup of a long-named table leaves the other tracked
\else
\echo [FAIL] (:testid) cleanup affected the wrong table (:left_triggers kept, :gone_triggers left on the cleaned one, rows :two_rows -> :two_after)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- A resume offset that points outside the value is refused.
DO $$
DECLARE
    v bigint;
    q bigint;
BEGIN
    SELECT db_version, seq INTO v, q FROM cloudsync_changes ORDER BY db_version, seq LIMIT 1;
    IF v IS NULL THEN
        PERFORM set_config('cloudsync.t71', 'no rows to resume from', false);
        RETURN;
    END IF;
    BEGIN
        PERFORM * FROM cloudsync_payload_chunks(resume_db_version => v, resume_seq => q, resume_frag_offset => 9223372036854775807);
        PERFORM set_config('cloudsync.t71', 'accepted', false);
    EXCEPTION WHEN OTHERS THEN
        PERFORM set_config('cloudsync.t71', 'refused: ' || SQLERRM, false);
    END;
END $$;
SELECT current_setting('cloudsync.t71') AS resume_result \gset
SELECT (:'resume_result' LIKE 'refused%') AS resume_ok \gset
\if :resume_ok
\echo [PASS] (:testid) an out-of-range resume_frag_offset is refused
\else
\echo [FAIL] (:testid) an out-of-range resume_frag_offset: :resume_result
SELECT (:fail::int + 1) AS fail \gset
\endif

-- An install made before the names were hashed already has the 63-byte form: init keeps it
-- instead of adding a second trigger, and cleanup removes it.
CREATE TABLE a_long_table_for_legacy_names_kept_as_before (id text PRIMARY KEY NOT NULL, v text);
CREATE FUNCTION legacy_noop() RETURNS trigger LANGUAGE plpgsql AS $f$ BEGIN RETURN NEW; END $f$;
SELECT left('cloudsync_after_insert_a_long_table_for_legacy_names_kept_as_before', 63) AS legacy_name \gset
CREATE TRIGGER "legacy_placeholder" AFTER INSERT ON a_long_table_for_legacy_names_kept_as_before
    FOR EACH ROW EXECUTE FUNCTION legacy_noop();
ALTER TRIGGER "legacy_placeholder" ON a_long_table_for_legacy_names_kept_as_before RENAME TO :"legacy_name";
SELECT cloudsync_init('a_long_table_for_legacy_names_kept_as_before') AS _l \gset
SELECT count(*) AS legacy_inserts FROM pg_trigger
WHERE tgrelid = 'a_long_table_for_legacy_names_kept_as_before'::regclass AND tgname LIKE 'cloudsync_after_insert_%' \gset
SELECT cloudsync_cleanup('a_long_table_for_legacy_names_kept_as_before') AS _lc \gset
SELECT count(*) AS legacy_left FROM pg_trigger
WHERE tgrelid = 'a_long_table_for_legacy_names_kept_as_before'::regclass AND tgname LIKE 'cloudsync_%' \gset
SELECT (:legacy_inserts = 1 AND :legacy_left = 0) AS legacy_ok \gset
\if :legacy_ok
\echo [PASS] (:testid) a trigger under the old 63-byte name is kept by init and removed by cleanup
\else
\echo [FAIL] (:testid) legacy trigger name: :legacy_inserts insert triggers after init, :legacy_left left after cleanup
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 2. Fourteen columns whose names are 60 single quotes plus a letter. A payload only applies
-- where the schema is the same, so the source is a database of its own.
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_71_qsrc;
CREATE DATABASE cloudsync_test_71_qsrc;
\connect cloudsync_test_71_qsrc
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DO $$
DECLARE
    cols text := '';
    i int;
BEGIN
    FOR i IN 0..13 LOOP
        cols := cols || format(', %I text', repeat('''', 60) || chr(97 + i));
    END LOOP;
    EXECUTE 'CREATE TABLE quoty (id text PRIMARY KEY NOT NULL' || cols || ')';
END $$;
SELECT cloudsync_init('quoty') AS _q \gset
DO $$
DECLARE
    sets text := '';
    i int;
BEGIN
    INSERT INTO quoty (id) VALUES ('1');
    FOR i IN 0..13 LOOP
        sets := sets || format('%s%I = %L', CASE WHEN i > 0 THEN ', ' ELSE '' END, repeat('''', 60) || chr(97 + i), 'v' || i);
    END LOOP;
    EXECUTE 'UPDATE quoty SET ' || sets;
END $$;
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex
FROM cloudsync_changes \gset

\connect cloudsync_test_71_dst
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DO $$
DECLARE
    cols text := '';
    i int;
BEGIN
    FOR i IN 0..13 LOOP
        cols := cols || format(', %I text', repeat('''', 60) || chr(97 + i));
    END LOOP;
    EXECUTE 'CREATE TABLE quoty (id text PRIMARY KEY NOT NULL' || cols || ')';
END $$;
SELECT cloudsync_init('quoty') AS _q \gset
SELECT set_config('cloudsync.payload71', :'payload_hex', false) AS _p \gset
DO $$
BEGIN
    PERFORM cloudsync_payload_apply(decode(current_setting('cloudsync.payload71'), 'hex'));
    PERFORM set_config('cloudsync.t71', 'ok', false);
EXCEPTION WHEN OTHERS THEN
    PERFORM set_config('cloudsync.t71', 'error: ' || SQLERRM, false);
END $$;
SELECT current_setting('cloudsync.t71') AS apply_result \gset
SELECT count(*) AS applied FROM quoty WHERE id = '1' AND (quoty::text) LIKE '%v13%' \gset
SELECT (:'apply_result' = 'ok' AND :applied = 1) AS quote_ok \gset
\if :quote_ok
\echo [PASS] (:testid) a payload applies to a table with quote-heavy column names
\else
\echo [FAIL] (:testid) quote-heavy columns: :apply_result (:applied rows)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 3. A text value whose bytes hold a NUL cannot be a PostgreSQL text: it is cut at the NUL,
-- and the stored copy matches its length.
\connect cloudsync_test_71_dst
\ir helper_psql_conn_setup.sql
CREATE TABLE nultext (id text PRIMARY KEY NOT NULL, v text);
SELECT cloudsync_init('nultext') AS _n \gset
SET client_min_messages = error;
DO $$
DECLARE
    enc bytea := cloudsync_encode_value('abcde'::text);
BEGIN
    enc := set_byte(enc, length(enc) - 4, 0);
    BEGIN
        INSERT INTO cloudsync_changes(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq)
        VALUES ('nultext', cloudsync_pk_encode('1'), 'v', enc, 1, 1, '\x0102030405060708090a0b0c0d0e0f10', 1, 0);
        PERFORM set_config('cloudsync.t71', 'accepted', false);
    EXCEPTION WHEN OTHERS THEN
        PERFORM set_config('cloudsync.t71', 'refused', false);
    END;
END $$;
SET client_min_messages = warning;
SELECT (current_setting('cloudsync.t71') = 'accepted' AND (SELECT count(*) FROM nultext WHERE v = 'a') = 1) AS nul_ok \gset
\if :nul_ok
\echo [PASS] (:testid) a text value with an embedded NUL is cut at the NUL
\else
\echo [FAIL] (:testid) a text value with an embedded NUL was not cut at the NUL
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_71_src;
DROP DATABASE IF EXISTS cloudsync_test_71_qsrc;
DROP DATABASE IF EXISTS cloudsync_test_71_dst;
