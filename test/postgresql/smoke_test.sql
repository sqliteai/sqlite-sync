-- usage:
-- - normal: `psql postgresql://postgres:postgres@localhost:5432/cloudsync_test -f test/postgresql/smoke_test.sql`
-- - debug: `psql -v DEBUG=1 postgresql://postgres:postgres@localhost:5432/cloudsync_test -f test/postgresql/smoke_test.sql`

\echo 'Running smoke_test...'

\ir helper_psql_conn_setup.sql
\set ON_ERROR_STOP on
\set fail 0

\echo '\nRunning 01_unittest.sql...' 
\ir 01_unittest.sql

\echo '\nRunning 02_2db_roundtrip.sql...'
\ir 02_2db_roundtrip.sql

\echo '\nRunning 03_3db_multiple_roundtrip.sql...'
\ir 03_3db_multiple_roundtrip.sql

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
