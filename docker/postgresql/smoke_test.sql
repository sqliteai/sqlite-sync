-- Enable debug logs
-- SET client_min_messages = debug1; SET log_min_messages = debug1;
SET client_min_messages = warning; SET log_min_messages = warning;

\set ON_ERROR_STOP on

-- Reset extension and install
DROP EXTENSION IF EXISTS cloudsync CASCADE;
CREATE EXTENSION cloudsync;

\echo 'Test version visibility'
SELECT cloudsync_version() AS version;

\echo 'Test uuid generation'
SELECT (length(cloudsync_uuid()) > 0) AS uuid_ok \gset
\if :uuid_ok
\else
  DO $$ BEGIN
    RAISE EXCEPTION 'smoke test failed: uuid_ok';
  END $$;
\endif

-- SELECT (cloudsync_db_version() >= 0) AS dbv_ok \gset
-- \if :dbv_ok
-- \else
--   \quit 1
-- \endif

\echo 'Test init on a simple table'
SELECT cloudsync_cleanup('smoke_tbl');
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true);

\echo 'Test insert metadata row creation'
SELECT cloudsync_uuid() AS smoke_id \gset
INSERT INTO smoke_tbl (id, val) VALUES (:'smoke_id', 'hello');
SELECT (COUNT(*) = 1) AS insert_meta_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id']::text[])
  AND col_name = 'val' \gset
\if :insert_meta_ok
\else
  DO $$ BEGIN
    RAISE EXCEPTION 'smoke test failed: insert_meta_ok';
  END $$;
\endif
\echo 'Test insert metadata fields'
SELECT (db_version > 0 AND seq >= 0) AS insert_meta_fields_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id']::text[])
  AND col_name = 'val' \gset
\if :insert_meta_fields_ok
\else
  DO $$ BEGIN
    RAISE EXCEPTION 'smoke test failed: insert_meta_fields_ok';
  END $$;
\endif

\echo 'Test delete metadata tombstone'
DELETE FROM smoke_tbl WHERE id = :'smoke_id';
SELECT (COUNT(*) = 1) AS delete_meta_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id']::text[])
  AND col_name = '__[RIP]__' \gset
\if :delete_meta_ok
\else
  DO $$ BEGIN
    RAISE EXCEPTION 'smoke test failed: delete_meta_ok';
  END $$;
\endif
\echo 'Test delete metadata fields'
SELECT (db_version > 0 AND seq >= 0) AS delete_meta_fields_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id']::text[])
  AND col_name = '__[RIP]__' \gset
\if :delete_meta_fields_ok
\else
  DO $$ BEGIN
    RAISE EXCEPTION 'smoke test failed: delete_meta_fields_ok';
  END $$;
\endif

\echo 'Test delete removes non-tombstone metadata'
SELECT (COUNT(*) = 0) AS delete_meta_only_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id']::text[])
  AND col_name != '__[RIP]__' \gset
\if :delete_meta_only_ok
\else
  DO $$ BEGIN
    RAISE EXCEPTION 'smoke test failed: delete_meta_only_ok';
  END $$;
\endif

\echo 'Test site id visibility'
SELECT cloudsync_siteid();

\echo 'Test site id encoding'
SELECT (length(encode(cloudsync_siteid()::bytea, 'hex')) > 0) AS sid_ok \gset
\if :sid_ok
\else
  \quit 1
\endif

\echo 'Test double init no-op'
SELECT cloudsync_init('smoke_tbl', 'CLS', true);
SELECT cloudsync_init('smoke_tbl', 'CLS', true);

SELECT cloudsync_cleanup('smoke_tbl');
