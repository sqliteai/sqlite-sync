-- Test unsupported CRDT algorithms (DWS, AWS)
-- Verifies that cloudsync_init rejects DWS and AWS with clear errors
-- and that no metadata tables are created.

\set testid '40'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_40;
CREATE DATABASE cloudsync_test_40;

\connect cloudsync_test_40
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE test_dws (id TEXT PRIMARY KEY, val TEXT);
CREATE TABLE test_aws (id TEXT PRIMARY KEY, val TEXT);

-- Test DWS rejection
DO $$
BEGIN
  PERFORM cloudsync_init('test_dws', 'dws', true);
  RAISE EXCEPTION 'cloudsync_init with dws should have failed';
EXCEPTION WHEN OTHERS THEN
  IF SQLERRM NOT LIKE '%not yet supported%' THEN
    RAISE EXCEPTION 'Unexpected error for dws: %', SQLERRM;
  END IF;
END $$;

-- Verify no companion table was created for DWS
SELECT COUNT(*) = 0 AS no_dws_meta
FROM information_schema.tables
WHERE table_name = 'test_dws_cloudsync' \gset
\if :no_dws_meta
\echo [PASS] (:testid) DWS rejected - no metadata table created
\else
\echo [FAIL] (:testid) DWS metadata table should not exist
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test AWS rejection
DO $$
BEGIN
  PERFORM cloudsync_init('test_aws', 'aws', true);
  RAISE EXCEPTION 'cloudsync_init with aws should have failed';
EXCEPTION WHEN OTHERS THEN
  IF SQLERRM NOT LIKE '%not yet supported%' THEN
    RAISE EXCEPTION 'Unexpected error for aws: %', SQLERRM;
  END IF;
END $$;

-- Verify no companion table was created for AWS
SELECT COUNT(*) = 0 AS no_aws_meta
FROM information_schema.tables
WHERE table_name = 'test_aws_cloudsync' \gset
\if :no_aws_meta
\echo [PASS] (:testid) AWS rejected - no metadata table created
\else
\echo [FAIL] (:testid) AWS metadata table should not exist
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify CLS still works (sanity check)
SELECT cloudsync_init('test_dws', 'cls', true) AS _init_cls \gset
SELECT COUNT(*) = 1 AS cls_meta_ok
FROM information_schema.tables
WHERE table_name = 'test_dws_cloudsync' \gset
\if :cls_meta_ok
\echo [PASS] (:testid) CLS init works after DWS/AWS rejection
\else
\echo [FAIL] (:testid) CLS init should work
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_40;
\endif
