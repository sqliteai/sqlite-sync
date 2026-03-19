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
\ir 12_repeated_table_multi_schemas.sql
\ir 13_per_table_schema_tracking.sql
\ir 14_datatype_roundtrip.sql
\ir 15_datatype_roundtrip_unmapped.sql
\ir 16_composite_pk_text_int_roundtrip.sql
\ir 17_uuid_pk_roundtrip.sql
\ir 18_bulk_insert_performance.sql
\ir 19_uuid_pk_with_unmapped_cols.sql
\ir 20_init_with_existing_data.sql
\ir 21_null_value_sync.sql
\ir 22_null_column_roundtrip.sql
\ir 23_uuid_column_roundtrip.sql
\ir 24_nullable_types_roundtrip.sql
\ir 25_boolean_type_issue.sql
\ir 26_row_filter.sql
\ir 27_rls_batch_merge.sql
\ir 28_db_version_tracking.sql
\ir 29_rls_multicol.sql
\ir 30_null_prikey_insert.sql
\ir 31_alter_table_sync.sql
\ir 32_block_lww.sql
\ir 33_block_lww_extended.sql
\ir 34_block_lww_advanced.sql
\ir 35_block_lww_edge_cases.sql
\ir 36_block_lww_round3.sql
\ir 37_block_lww_round4.sql
\ir 38_block_lww_round5.sql
\ir 39_concurrent_write_apply.sql
\ir 40_unsupported_algorithms.sql
\ir 41_corrupted_payload.sql
\ir 42_payload_idempotency.sql
\ir 43_delete_resurrect_ordering.sql
\ir 44_large_composite_pk.sql
\ir 45_pg_specific_types.sql
\ir 46_schema_hash_mismatch.sql

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
