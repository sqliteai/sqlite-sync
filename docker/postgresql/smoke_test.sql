-- usage:
-- - normal: `psql postgresql://postgres:postgres@localhost:5432/cloudsync_test -f docker/postgresql/smoke_test.sql`
-- - debug: `psql -v DEBUG=1 postgresql://postgres:postgres@localhost:5432/cloudsync_test -f docker/postgresql/smoke_test.sql`

\set ON_ERROR_STOP on
\set fail 0
\if :{?DEBUG}
SET client_min_messages = debug1; SET log_min_messages = debug1;
\set QUIET 0
\pset tuples_only off
\pset format aligned
\echo '[DEBUG] verbose output enabled'
\else
\set QUIET 1
\pset tuples_only on
\pset format unaligned
SET client_min_messages = warning; SET log_min_messages = warning;
\endif

-- Reset extension and install
DROP EXTENSION IF EXISTS cloudsync CASCADE;
CREATE EXTENSION cloudsync;

-- 'Test version visibility'
SELECT cloudsync_version() AS version \gset
\echo [PASS] Test cloudsync_version: :version

-- 'Test uuid generation'
SELECT (length(cloudsync_uuid()) > 0) AS uuid_ok \gset
\if :uuid_ok
\echo '[PASS] Test uuid generation'
\else
\echo '[FAIL] Test uuid generation'
SELECT (:fail::int + 1) AS fail \gset
\endif

-- SELECT (cloudsync_db_version() >= 0) AS dbv_ok \gset
-- \if :dbv_ok
-- \else
--   \quit 1
-- \endif

-- 'Test init on a simple table'
SELECT cloudsync_cleanup('smoke_tbl') AS _cleanup_ok \gset
SELECT (cloudsync_is_sync('smoke_tbl') = false) AS init_cleanup_ok \gset
\if :init_cleanup_ok
\echo '[PASS] Test init cleanup'
\else
\echo '[FAIL] Test init cleanup'
SELECT (:fail::int + 1) AS fail \gset
\endif
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id \gset
SELECT (to_regclass('public.smoke_tbl_cloudsync') IS NOT NULL) AS init_create_ok \gset
\if :init_create_ok
\echo '[PASS] Test init create'
\else
\echo '[FAIL] Test init create'
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test insert metadata row creation'
SELECT cloudsync_uuid() AS smoke_id \gset
INSERT INTO smoke_tbl (id, val) VALUES (:'smoke_id', 'hello');
SELECT (COUNT(*) = 1) AS insert_meta_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id']::text[])
  AND col_name = 'val' \gset
\if :insert_meta_ok
\echo '[PASS] Test insert metadata row creation'
\else
\echo '[FAIL] Test insert metadata row creation'
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test insert metadata fields'
SELECT (db_version > 0 AND seq >= 0) AS insert_meta_fields_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id']::text[])
  AND col_name = 'val' \gset
\if :insert_meta_fields_ok
\echo '[PASS] Test insert metadata fields'
\else
\echo '[FAIL] Test insert metadata fields'
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test delete metadata tombstone'
DELETE FROM smoke_tbl WHERE id = :'smoke_id';
SELECT (COUNT(*) = 1) AS delete_meta_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id']::text[])
  AND col_name = '__[RIP]__' \gset
\if :delete_meta_ok
\echo '[PASS] Test delete metadata tombstone'
\else
\echo '[FAIL] Test delete metadata tombstone'
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test delete metadata fields'
SELECT (db_version > 0 AND seq >= 0) AS delete_meta_fields_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id']::text[])
  AND col_name = '__[RIP]__' \gset
\if :delete_meta_fields_ok
\echo '[PASS] Test delete metadata fields'
\else
\echo '[FAIL] Test delete metadata fields'
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test delete removes non-tombstone metadata'
SELECT (COUNT(*) = 0) AS delete_meta_only_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id']::text[])
  AND col_name != '__[RIP]__' \gset
\if :delete_meta_only_ok
\echo '[PASS] Test delete removes non-tombstone metadata'
\else
\echo '[FAIL] Test delete removes non-tombstone metadata'
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test site id visibility'
SELECT cloudsync_siteid() AS site_id \gset
\echo [PASS] Test site id visibility :site_id

-- 'Test site id encoding'
SELECT (length(encode(cloudsync_siteid()::bytea, 'hex')) > 0) AS sid_ok \gset
\if :sid_ok
\echo '[PASS] Test site id encoding'
\else
\echo '[FAIL] Test site id encoding'
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test double init no-op'
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id2 \gset
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id3 \gset
\echo '[PASS] double init no-op'

-- 'Test summary'
\echo '\nTest summary:'
\echo - Failures: :fail
SELECT (:fail::int > 0) AS fail_any \gset
\if :fail_any
\echo smoke test failed: :fail test(s) failed
DO $$ BEGIN
  RAISE EXCEPTION 'smoke test failed';
END $$;
\else
\echo - Status: OK
\endif
