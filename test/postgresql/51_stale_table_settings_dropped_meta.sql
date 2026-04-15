-- 'Stale cloudsync_table_settings with dropped meta-table'
-- Mirrors the SQLite unit test: Stale Table Settings Dropped Meta
--
-- When a user drops a tracked table and its <table>_cloudsync meta-table
-- manually (without calling cloudsync_cleanup), cloudsync_table_settings is
-- left with stale rows while pg_tables no longer has any matching
-- *_cloudsync table. Before the fix in cloudsync_dbversion_rebuild, opening a
-- new backend and calling any cloudsync function caused
-- cloudsync_dbversion_build_query to produce a NULL SQL string (string_agg
-- over zero rows), which was misreported as DBRES_NOMEM, making
-- cloudsync_context_init fail and every cloudsync_* call ereport ERROR.

\set testid '51'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

DROP DATABASE IF EXISTS cloudsync_test_51;
CREATE DATABASE cloudsync_test_51;

\connect cloudsync_test_51
\ir helper_psql_conn_setup.sql

CREATE EXTENSION IF NOT EXISTS cloudsync;

-- Phase 1: create a tracked table and initialize cloudsync on it.
DROP TABLE IF EXISTS stale_doc CASCADE;
CREATE TABLE stale_doc (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('stale_doc', 'CLS', 1) AS _init \gset

-- Sanity: the meta-table exists and cloudsync_table_settings has a row for it.
SELECT count(*) AS meta_exists FROM pg_tables WHERE tablename = 'stale_doc_cloudsync' \gset
SELECT (:meta_exists::int = 1) AS meta_exists_ok \gset
\if :meta_exists_ok
\echo [PASS] (:testid) stale_doc_cloudsync meta-table created
\else
\echo [FAIL] (:testid) expected stale_doc_cloudsync to exist
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT count(*) AS settings_rows FROM cloudsync_table_settings WHERE tbl_name = 'stale_doc' \gset
SELECT (:settings_rows::int > 0) AS settings_rows_ok \gset
\if :settings_rows_ok
\echo [PASS] (:testid) cloudsync_table_settings has row(s) for stale_doc
\else
\echo [FAIL] (:testid) expected cloudsync_table_settings row for stale_doc
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Phase 2: drop BOTH the base table and the meta-table without calling
-- cloudsync_cleanup. cloudsync_table_settings still references stale_doc,
-- but pg_tables has no *_cloudsync tables at all now.
DROP TABLE stale_doc;
DROP TABLE stale_doc_cloudsync;

SELECT count(*) AS cloudsync_meta_tables FROM pg_tables WHERE tablename LIKE '%_cloudsync' \gset
SELECT (:cloudsync_meta_tables::int = 0) AS no_meta_ok \gset
\if :no_meta_ok
\echo [PASS] (:testid) no *_cloudsync meta-tables remain in pg_tables
\else
\echo [FAIL] (:testid) expected zero *_cloudsync tables, got :cloudsync_meta_tables
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Phase 3: reconnect to force a fresh backend. pg_cloudsync_context is a
-- static per-process pointer, so a new backend means
-- cloudsync_pg_context_init runs again on the next cloudsync call — which
-- is exactly what used to fail under this bug.
\connect cloudsync_test_51
\ir helper_psql_conn_setup.sql

-- cloudsync_version is a pure function that does not touch the context, so
-- this call cannot fail even with the bug present. It's here only as a
-- trivial smoke check that the extension is still loadable.
SELECT cloudsync_version() IS NOT NULL AS version_ok \gset
\if :version_ok
\echo [PASS] (:testid) cloudsync_version() reachable after reopen
\else
\echo [FAIL] (:testid) cloudsync_version() failed after reopen
SELECT (:fail::int + 1) AS fail \gset
\endif

-- The real check: calling a function that goes through get_cloudsync_context()
-- must succeed. Before the fix, cloudsync_dbversion_rebuild returned
-- DBRES_NOMEM here because SQL_DBVERSION_BUILD_QUERY's string_agg over zero
-- rows produced a NULL SQL string, and the whole init path would ereport
-- ERROR — any cloudsync_* call below would abort the script.
CREATE TABLE stale_doc2 (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('stale_doc2', 'CLS', 1) AS _init2 \gset

-- The new table's meta-table exists. If cloudsync_init failed (pre-fix
-- behavior) this count will be 0, covering both the init-rc check and the
-- meta-table creation in a single assertion.
SELECT count(*) AS meta2_exists FROM pg_tables WHERE tablename = 'stale_doc2_cloudsync' \gset
SELECT (:meta2_exists::int = 1) AS meta2_exists_ok \gset
\if :meta2_exists_ok
\echo [PASS] (:testid) cloudsync_init succeeded and stale_doc2_cloudsync was created
\else
\echo [FAIL] (:testid) expected stale_doc2_cloudsync to exist after cloudsync_init
SELECT (:fail::int + 1) AS fail \gset
\endif

-- An insert via the new cloudsync_init'd table should produce a cloudsync
-- metadata entry — confirming the context is fully functional.
INSERT INTO stale_doc2 (id, body) VALUES ('a', 'hello');

SELECT count(*) AS meta_rows FROM stale_doc2_cloudsync \gset
SELECT (:meta_rows::int > 0) AS meta_rows_ok \gset
\if :meta_rows_ok
\echo [PASS] (:testid) stale_doc2_cloudsync has metadata after insert
\else
\echo [FAIL] (:testid) expected metadata rows in stale_doc2_cloudsync
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_51;
\else
\echo [INFO] !!!!!
\endif
