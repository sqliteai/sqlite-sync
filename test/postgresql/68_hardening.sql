-- Hardening of PUBLIC-callable entry points: none of these may crash the backend or run
-- caller-controlled SQL; each either works or fails with an ordinary error.

\set testid '68-hardening'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_68;
CREATE DATABASE cloudsync_test_68;

\connect cloudsync_test_68
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- 1. The cloudsync_changes trigger refuses a table that does not have the view's layout.
CREATE TABLE foreign_layout (c1 bigint);
CREATE TRIGGER foreign_layout_trg BEFORE INSERT ON foreign_layout
    FOR EACH ROW EXECUTE FUNCTION cloudsync_changes_insert_trigger();
DO $$
BEGIN
    INSERT INTO foreign_layout VALUES (5);
    PERFORM set_config('cloudsync.t68', 'no-error', false);
EXCEPTION WHEN feature_not_supported THEN
    PERFORM set_config('cloudsync.t68', 'refused', false);
END $$;
SELECT current_setting('cloudsync.t68') = 'refused' AS trigger_ok \gset
\if :trigger_ok
\echo [PASS] (:testid) the changes trigger refuses a foreign table layout
\else
\echo [FAIL] (:testid) the changes trigger accepted a foreign table layout
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 2. A name value is encoded as the text it spells, not read as a varlena.
SELECT (cloudsync_encode_value('@A'::name) = cloudsync_encode_value('@A'::text)) AS name_ok \gset
\if :name_ok
\echo [PASS] (:testid) a name value encodes like the same text
\else
\echo [FAIL] (:testid) a name value did not encode like the same text
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 3. Names with quotes and dollar tags never leave their literal / function body.
CREATE TABLE "q'uote" (id text PRIMARY KEY NOT NULL, "v'al" text);
SELECT cloudsync_init('q''uote') AS _q \gset
INSERT INTO "q'uote" VALUES ('a', 'x');
UPDATE "q'uote" SET "v'al" = 'y' WHERE id = 'a';
DELETE FROM "q'uote" WHERE id = 'a';
SELECT (count(*) > 0) AS q_meta FROM "q'uote_cloudsync" \gset
\if :q_meta
\echo [PASS] (:testid) a table name with a single quote syncs
\else
\echo [FAIL] (:testid) a table name with a single quote left no metadata
SELECT (:fail::int + 1) AS fail \gset
\endif

CREATE TABLE "ev$cloudsync_body$il" (id text PRIMARY KEY NOT NULL, v text);
CREATE TABLE sentinel (x int);
DO $$
BEGIN
    PERFORM cloudsync_init('ev$cloudsync_body$il');
EXCEPTION WHEN OTHERS THEN
    NULL;
END $$;
SELECT to_regclass('sentinel') IS NOT NULL AS dollar_ok \gset
\if :dollar_ok
\echo [PASS] (:testid) a dollar-quote tag in a table name is refused or contained
\else
\echo [FAIL] (:testid) a dollar-quote tag in a table name ran as SQL
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 4. cloudsync_update keeps pass-by-reference values past the row that produced them.
CREATE TABLE refvals (id text PRIMARY KEY NOT NULL, u uuid, n numeric, b box);
SELECT cloudsync_init('refvals') AS _r \gset
INSERT INTO refvals SELECT g::text, gen_random_uuid(), g * 1.5, box(point(g, g), point(0, 0)) FROM generate_series(1, 200) g;
UPDATE refvals SET u = gen_random_uuid(), n = n + 1, b = box(point(1, 1), point(2, 2));
SELECT count(*) AS ref_rows FROM refvals_cloudsync WHERE col_name IN ('u', 'n', 'b') \gset
SELECT (:ref_rows::int >= 600) AS ref_ok \gset
\if :ref_ok
\echo [PASS] (:testid) a bulk update of by-reference columns is tracked
\else
\echo [FAIL] (:testid) a bulk update of by-reference columns tracked only :ref_rows rows
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 4b. The values the aggregate keeps must not be the caller's per-row memory: they are
-- produced row by row in expressions whose memory is reset between rows (numeric and uuid
-- are not varlenas the wrapper knew how to copy). The primary key kept for row one must
-- still be 10 when the aggregate finishes.
CREATE TABLE refpk (id numeric PRIMARY KEY NOT NULL, c1 numeric, u uuid);
SELECT cloudsync_init('refpk') AS _rp \gset
INSERT INTO refpk VALUES (10, 1, gen_random_uuid());
DO $$
BEGIN
    PERFORM cloudsync_update('refpk', s.x, s.y) FROM (
        SELECT CASE g WHEN 1 THEN (9 + g)::numeric ELSE (g * 1000 + 1)::numeric END AS x,
               CASE g WHEN 1 THEN (9 + g)::numeric ELSE (g - 1)::numeric END AS y
        FROM generate_series(1, 2) g) s;
    PERFORM set_config('cloudsync.t68', 'ok', false);
EXCEPTION WHEN OTHERS THEN
    PERFORM set_config('cloudsync.t68', 'error: ' || SQLERRM, false);
END $$;
SELECT current_setting('cloudsync.t68') AS refpk_call \gset
SELECT (:'refpk_call' = 'ok'
        AND count(DISTINCT pk) = 2
        AND bool_and(cloudsync_pk_decode(pk, 1)::numeric = 10)) AS refpk_ok
FROM refpk_cloudsync WHERE col_name = 'c1' \gset
\if :refpk_ok
\echo [PASS] (:testid) values kept by cloudsync_update survive the per-row memory reset
\else
\echo [FAIL] (:testid) cloudsync_update kept a value from per-row memory (:refpk_call)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 5. The aggregates consume their state: a window use is refused with an ordinary error,
-- a plain or grouped use still works.
DO $$
BEGIN
    PERFORM cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) OVER ()
    FROM cloudsync_changes;
    PERFORM set_config('cloudsync.t68', 'no-error', false);
EXCEPTION WHEN feature_not_supported THEN
    PERFORM set_config('cloudsync.t68', 'refused', false);
END $$;
SELECT current_setting('cloudsync.t68') = 'refused' AS win_enc_ok \gset
\if :win_enc_ok
\echo [PASS] (:testid) the payload aggregate refuses a window context
\else
\echo [FAIL] (:testid) the payload aggregate did not refuse a window context
SELECT (:fail::int + 1) AS fail \gset
\endif

DO $$
BEGIN
    PERFORM cloudsync_update(t.tn, t.nv, t.ov) OVER ()
    FROM (VALUES ('refvals', 'a', 'a')) AS t(tn, nv, ov);
    PERFORM set_config('cloudsync.t68', 'no-error', false);
EXCEPTION WHEN feature_not_supported THEN
    PERFORM set_config('cloudsync.t68', 'refused', false);
END $$;
SELECT current_setting('cloudsync.t68') = 'refused' AS win_upd_ok \gset
\if :win_upd_ok
\echo [PASS] (:testid) the update aggregate refuses a window context
\else
\echo [FAIL] (:testid) the update aggregate did not refuse a window context
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (length(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq)) > 0) AS plain_agg_ok
FROM cloudsync_changes \gset
SELECT (count(*) = 2) AS grouped_agg_ok FROM (
    SELECT tbl, cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq)
    FROM cloudsync_changes WHERE tbl = 'refvals' GROUP BY GROUPING SETS ((tbl), ())) g \gset
\if :plain_agg_ok
\if :grouped_agg_ok
\echo [PASS] (:testid) plain and grouped uses of the payload aggregate still work
\else
\echo [FAIL] (:testid) grouped use of the payload aggregate broke
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) plain use of the payload aggregate broke
SELECT (:fail::int + 1) AS fail \gset
\endif

-- A hashed aggregate on the inner side of a nested loop is rescanned without being rebuilt:
-- the executor finalizes the same group states again, and every pass must return the same payload.
CREATE TEMP TABLE t68_changes AS SELECT * FROM cloudsync_changes WHERE tbl = 'refvals';
SELECT md5(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq)) AS enc_md5
FROM t68_changes \gset
BEGIN;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_material = off;
SET LOCAL enable_memoize = off;
SET LOCAL enable_sort = off;
SELECT (count(*) = 3 AND count(DISTINCT m) = 1 AND min(m) = :'enc_md5') AS rescan_agg_ok FROM (
    SELECT md5(s.p) AS m FROM generate_series(1, 3) g LEFT JOIN (
        SELECT tbl, cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS p
        FROM t68_changes GROUP BY tbl) s ON true) r \gset
COMMIT;
\if :rescan_agg_ok
\echo [PASS] (:testid) a rescanned payload aggregate returns the same payload on every pass
\else
\echo [FAIL] (:testid) a rescanned payload aggregate returned a different payload
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 6. A column that is not bytea is never read as a blob. The block migration scan reads the
-- pk column of the metadata table; with that column and the blocks table's pk both retyped
-- to bigint the scan query still prepares, so the read really reaches database_column_blob,
-- which used to dereference the bigint as a bytea pointer.
CREATE TABLE retyped (id text PRIMARY KEY NOT NULL, body text);
SELECT cloudsync_init('retyped', 'CLS', 1) AS _rt \gset
INSERT INTO retyped VALUES ('r1', E'a\nb\nc');
ALTER TABLE retyped_cloudsync DROP CONSTRAINT retyped_cloudsync_pkey;
ALTER TABLE retyped_cloudsync ALTER COLUMN pk TYPE bigint USING 5;
CREATE TABLE retyped_cloudsync_blocks (
    pk bigint NOT NULL, col_name text COLLATE "C" NOT NULL, col_value text, PRIMARY KEY (pk, col_name));
DO $$
BEGIN
    PERFORM cloudsync_set_column('retyped', 'body', 'algo', 'block');
EXCEPTION WHEN OTHERS THEN
    NULL;
END $$;
SELECT count(*) AS retyped_blocks FROM retyped_cloudsync_blocks \gset
SELECT (:retyped_blocks::int = 0) AS retyped_ok \gset
\if :retyped_ok
\echo [PASS] (:testid) a non-bytea column is not read as a blob
\else
\echo [FAIL] (:testid) a non-bytea column was read as a blob (:retyped_blocks blocks)
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_68;
