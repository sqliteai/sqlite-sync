-- usage:
-- - normal: `psql postgresql://postgres:postgres@localhost:5432/cloudsync_test -f test/postgresql/smoke_test.sql`
-- - debug: `psql -v DEBUG=1 postgresql://postgres:postgres@localhost:5432/cloudsync_test -f test/postgresql/smoke_test.sql`

\echo 'Running smoke_test...'

\ir helper_psql_conn_setup.sql
-- \set ON_ERROR_STOP on
\set fail 0

\ir 01_unittest.sql
\ir 02_roundtrip.sql
\ir 03_multiple_roundtrip.sql
\ir 04_colversion_skew.sql
\ir 05_delete_recreate_cycle.sql
\ir 06_out_of_order_delivery.sql
\ir 07_delete_vs_update.sql
\ir 08_resurrect_delayed_delete.sql
\ir 09_multicol_concurrent_edits.sql
\ir 10_empty_payload_noop.sql
\ir 11_multi_table_multi_columns_rounds.sql

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
