\set testid '12'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

DROP DATABASE IF EXISTS cloudsync_test_12;
CREATE DATABASE cloudsync_test_12;

\connect cloudsync_test_12
\ir helper_psql_conn_setup.sql

-- Reset extension and install
DROP EXTENSION IF EXISTS cloudsync CASCADE;
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- 'Test multi-schema table init (setup)'
CREATE SCHEMA IF NOT EXISTS test_schema;
DROP TABLE IF EXISTS public.repeated_table;
DROP TABLE IF EXISTS test_schema.repeated_table;
CREATE TABLE public.repeated_table (id TEXT PRIMARY KEY, data TEXT);
CREATE TABLE test_schema.repeated_table (id TEXT PRIMARY KEY, data TEXT);

-- Reset the connection to test if we load the configuration correctly
\connect cloudsync_test_12
\ir helper_psql_conn_setup.sql

-- 'Test init on table that exists in multiple schemas (default: public)'
SELECT cloudsync_cleanup('repeated_table') AS _cleanup_repeated \gset
SELECT cloudsync_init('repeated_table', 'CLS', true) AS _init_repeated_public \gset
SELECT cloudsync_table_schema('repeated_table') AS repeated_schema_public \gset
SELECT (:'repeated_schema_public' = 'public') AS repeated_schema_public_ok \gset
\if :repeated_schema_public_ok
\echo [PASS] (:testid) Test cloudsync_table_schema returns public for repeated_table
\else
\echo [FAIL] (:testid) Test cloudsync_table_schema returns public for repeated_table
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT (to_regclass('public.repeated_table_cloudsync') IS NOT NULL) AS init_repeated_public_ok \gset
\if :init_repeated_public_ok
\echo [PASS] (:testid) Test init on repeated_table in public schema
\else
\echo [FAIL] (:testid) Test init on repeated_table in public schema
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test insert on repeated_table in public schema'
SELECT cloudsync_uuid() AS repeated_id1 \gset
INSERT INTO public.repeated_table (id, data) VALUES (:'repeated_id1', 'public_data');
SELECT (COUNT(*) = 1) AS insert_repeated_public_ok
FROM public.repeated_table_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'repeated_id1']::text[])
  AND col_name = 'data' \gset
\if :insert_repeated_public_ok
\echo [PASS] (:testid) Test insert metadata on repeated_table in public
\else
\echo [FAIL] (:testid) Test insert metadata on repeated_table in public
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test cloudsync_changes view read for public.repeated_table'
SELECT COUNT(*) AS changes_view_repeated_count
FROM cloudsync_changes
WHERE tbl = 'repeated_table' \gset
SELECT COUNT(*) AS changes_meta_repeated_count
FROM public.repeated_table_cloudsync \gset
SELECT (:changes_view_repeated_count::int = :changes_meta_repeated_count::int) AS changes_read_repeated_ok \gset
\if :changes_read_repeated_ok
\echo [PASS] (:testid) Test cloudsync_changes view read for public.repeated_table
\else
\echo [FAIL] (:testid) Test cloudsync_changes view read for public.repeated_table
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test cloudsync_changes view write for public.repeated_table'
SELECT cloudsync_uuid() AS repeated_id2 \gset
INSERT INTO cloudsync_changes (tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq)
VALUES (
  'repeated_table',
  cloudsync_pk_encode(VARIADIC ARRAY[:'repeated_id2']::text[]),
  'data',
  -- "public_write" encoded as cloudsync text value (type 0x0b + len 0x0c)
  decode('0b0c7075626c69635f7772697465', 'hex'),
  1,
  cloudsync_db_version_next(),
  cloudsync_siteid(),
  1,
  0
);
SELECT (COUNT(*) = 1) AS changes_write_repeated_ok
FROM public.repeated_table
WHERE id = :'repeated_id2' AND data = 'public_write' \gset
\if :changes_write_repeated_ok
\echo [PASS] (:testid) Test cloudsync_changes view write for public.repeated_table
\else
\echo [FAIL] (:testid) Test cloudsync_changes view write for public.repeated_table
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test cleanup on table with ambiguous name'
SELECT cloudsync_cleanup('repeated_table') AS _cleanup_repeated2 \gset
SELECT (to_regclass('public.repeated_table_cloudsync') IS NULL) AS cleanup_repeated_ok \gset
\if :cleanup_repeated_ok
\echo [PASS] (:testid) Test cleanup on repeated_table
\else
\echo [FAIL] (:testid) Test cleanup on repeated_table
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test cloudsync_set_schema and init on test_schema'
SELECT cloudsync_set_schema('test_schema') AS _set_schema \gset
SELECT cloudsync_init('repeated_table', 'CLS', true) AS _init_repeated_test_schema \gset
SELECT cloudsync_table_schema('repeated_table') AS repeated_schema_test_schema \gset
SELECT (:'repeated_schema_test_schema' = 'test_schema') AS repeated_schema_test_schema_ok \gset
\if :repeated_schema_test_schema_ok
\echo [PASS] (:testid) Test cloudsync_table_schema returns test_schema for repeated_table
\else
\echo [FAIL] (:testid) Test cloudsync_table_schema returns test_schema for repeated_table
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT (to_regclass('test_schema.repeated_table_cloudsync') IS NOT NULL) AS init_repeated_test_schema_ok \gset
\if :init_repeated_test_schema_ok
\echo [PASS] (:testid) Test init on repeated_table in test_schema
\else
\echo [FAIL] (:testid) Test init on repeated_table in test_schema
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test that public.repeated_table_cloudsync was not recreated'
SELECT (to_regclass('public.repeated_table_cloudsync') IS NULL) AS public_still_clean_ok \gset
\if :public_still_clean_ok
\echo [PASS] (:testid) Test public.repeated_table_cloudsync still cleaned up
\else
\echo [FAIL] (:testid) Test public.repeated_table_cloudsync should not exist
SELECT (:fail::int + 1) AS fail \gset
\endif

-- reset the current schema to check if the next connection load the correct configuration
SELECT cloudsync_set_schema('public') AS _reset_schema \gset

-- Reset the connection to test if if loads the correct configuration for the table on the correct schema
\connect cloudsync_test_12
\ir helper_psql_conn_setup.sql

-- 'Test insert on repeated_table in test_schema'
SELECT cloudsync_uuid() AS repeated_id3 \gset
INSERT INTO test_schema.repeated_table (id, data) VALUES (:'repeated_id3', 'test_schema_data');
SELECT (COUNT(*) = 1) AS insert_repeated_test_schema_ok
FROM test_schema.repeated_table_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'repeated_id3']::text[])
  AND col_name = 'data' \gset
\if :insert_repeated_test_schema_ok
\echo [PASS] (:testid) Test insert metadata on repeated_table in test_schema
\else
\echo [FAIL] (:testid) Test insert metadata on repeated_table in test_schema
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test cloudsync_changes view read for test_schema.repeated_table'
SELECT COUNT(*) AS changes_view_test_schema_count
FROM cloudsync_changes
WHERE tbl = 'repeated_table' \gset
SELECT COUNT(*) AS changes_meta_test_schema_count
FROM test_schema.repeated_table_cloudsync \gset
SELECT (:changes_view_test_schema_count::int = :changes_meta_test_schema_count::int) AS changes_read_test_schema_ok \gset
\if :changes_read_test_schema_ok
\echo [PASS] (:testid) Test cloudsync_changes view read for test_schema.repeated_table
\else
\echo [FAIL] (:testid) Test cloudsync_changes view read for test_schema.repeated_table
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test cloudsync_changes view write for test_schema.repeated_table'
SELECT cloudsync_uuid() AS repeated_id4 \gset
INSERT INTO cloudsync_changes (tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq)
VALUES (
  'repeated_table',
  cloudsync_pk_encode(VARIADIC ARRAY[:'repeated_id4']::text[]),
  'data',
  -- "testschema_write" encoded as cloudsync text value (type 0x0b + len 0x10)
  decode('0b1074657374736368656d615f7772697465', 'hex'),
  1,
  cloudsync_db_version_next(),
  cloudsync_siteid(),
  1,
  0
);
SELECT (COUNT(*) = 1) AS changes_write_test_schema_ok
FROM test_schema.repeated_table
WHERE id = :'repeated_id4' AND data = 'testschema_write' \gset
\if :changes_write_test_schema_ok
\echo [PASS] (:testid) Test cloudsync_changes view write for test_schema.repeated_table
\else
\echo [FAIL] (:testid) Test cloudsync_changes view write for test_schema.repeated_table
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test cleanup on repeated_table on test_schema'
SELECT cloudsync_cleanup('repeated_table') AS _cleanup_repeated3 \gset
SELECT (to_regclass('test_schema.repeated_table_cloudsync') IS NULL) AS cleanup_repeated3_ok \gset
\if :cleanup_repeated3_ok
\echo [PASS] (:testid) Test cleanup on repeated_table on test_schema
\else
\echo [FAIL] (:testid) Test cleanup on repeated_table on test_schema
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Reset schema to public for subsequent tests'
SELECT cloudsync_set_schema('public') AS _reset_schema \gset
SELECT current_schema() AS current_schema_after_reset \gset
SELECT (:'current_schema_after_reset' = 'public') AS schema_reset_ok \gset
\if :schema_reset_ok
\echo [PASS] (:testid) Test schema reset to public
\else
\echo [FAIL] (:testid) Test schema reset to public
SELECT (:fail::int + 1) AS fail \gset
\endif

\if :{?DEBUG_MERGE}
\connect postgres
DROP DATABASE IF EXISTS cloudsync_test_12;
\endif

-- Cleanup: Drop test databases if not in DEBUG mode and no failures
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_12;
\endif