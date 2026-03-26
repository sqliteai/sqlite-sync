-- 'Unittest'

\set testid '01'

\connect postgres
\ir helper_psql_conn_setup.sql

DROP DATABASE IF EXISTS cloudsync_test_1;
CREATE DATABASE cloudsync_test_1;

\connect cloudsync_test_1
\ir helper_psql_conn_setup.sql

-- Reset extension and install
-- DROP EXTENSION IF EXISTS cloudsync CASCADE;
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- 'Test version visibility'
SELECT cloudsync_version() AS version \gset
SELECT current_setting('server_version') AS pg_version \gset
\echo [PASS] (:testid) Test cloudsync_version: :version (PostgreSQL :pg_version)

-- Test uuid generation
SELECT cloudsync_uuid() AS uuid1 \gset
SELECT pg_sleep(0.1) \gset
SELECT cloudsync_uuid() AS uuid2 \gset

-- Test 1: Format check (UUID v7 has standard format: xxxxxxxx-xxxx-7xxx-xxxx-xxxxxxxxxxxx)
SELECT (:'uuid1' ~ '^[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$') AS uuid_format_ok \gset
\if :uuid_format_ok
\echo [PASS] (:testid) UUID format valid (UUIDv7 pattern)
\else
\echo [FAIL] (:testid) UUID format invalid - Got: :uuid1
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 2: Uniqueness check
SELECT (:'uuid1' != :'uuid2') AS uuid_unique_ok \gset
\if :uuid_unique_ok
\echo [PASS] (:testid) UUID uniqueness (two calls generated different UUIDs)
\else
\echo [FAIL] (:testid) UUID uniqueness - Both calls returned: :uuid1
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 3: Monotonicity check (UUIDv7 should be sortable by timestamp)
SELECT (:'uuid1' < :'uuid2') AS uuid_monotonic_ok \gset
\if :uuid_monotonic_ok
\echo [PASS] (:testid) UUID monotonicity (UUIDs are time-ordered)
\else
\echo [FAIL] (:testid) UUID monotonicity - uuid1: :uuid1, uuid2: :uuid2
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 4: Type check (ensure it's actually UUID type, not text)
SELECT (pg_typeof(cloudsync_uuid())::text = 'uuid') AS uuid_type_ok \gset
\if :uuid_type_ok
\echo [PASS] (:testid) UUID type is correct (uuid, not text or bytea)
\else
\echo [FAIL] (:testid) UUID type incorrect - Got: (pg_typeof(cloudsync_uuid())::text)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test init on a simple table'
SELECT cloudsync_cleanup('smoke_tbl') AS _cleanup_ok \gset
SELECT (cloudsync_is_sync('smoke_tbl') = false) AS init_cleanup_ok \gset
\if :init_cleanup_ok
\echo [PASS] (:testid) Test init cleanup
\else
\echo [FAIL] (:testid) Test init cleanup
SELECT (:fail::int + 1) AS fail \gset
\endif
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id \gset
SELECT (to_regclass('public.smoke_tbl_cloudsync') IS NOT NULL) AS init_create_ok \gset
\if :init_create_ok
\echo [PASS] (:testid) Test init create
\else
\echo [FAIL] (:testid) Test init create
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
\echo [PASS] (:testid) Test insert metadata row creation
\else
\echo [FAIL] (:testid) Test insert metadata row creation
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test insert metadata fields'
SELECT (db_version > 0 AND seq >= 0) AS insert_meta_fields_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id']::text[])
  AND col_name = 'val' \gset
\if :insert_meta_fields_ok
\echo [PASS] (:testid) Test insert metadata fields
\else
\echo [FAIL] (:testid) Test insert metadata fields
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test update val only'
SELECT col_version AS val_ver_before
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id']::text[])
  AND col_name = 'val' \gset
UPDATE smoke_tbl SET val = 'hello2' WHERE id = :'smoke_id';
SELECT col_version AS val_ver_after
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id']::text[])
  AND col_name = 'val' \gset
SELECT (:val_ver_after::bigint > :val_ver_before::bigint) AS update_val_ok \gset
\if :update_val_ok
\echo [PASS] (:testid) Test update val only
\else
\echo [FAIL] (:testid) Test update val only
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test update id only'
SELECT cloudsync_uuid() AS smoke_id2 \gset
UPDATE smoke_tbl SET id = :'smoke_id2' WHERE id = :'smoke_id';
SELECT (COUNT(*) = 1) AS update_id_old_tombstone_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id']::text[])
  AND col_name = '__[RIP]__' \gset
\if :update_id_old_tombstone_ok
\echo [PASS] (:testid) Test update id only (old tombstone)
\else
\echo [FAIL] (:testid) Test update id only (old tombstone)
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT (COUNT(*) = 0) AS update_id_old_val_gone_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id']::text[])
  AND col_name = 'val' \gset
\if :update_id_old_val_gone_ok
\echo [PASS] (:testid) Test update id only (old val gone)
\else
\echo [FAIL] (:testid) Test update id only (old val gone)
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT (COUNT(*) = 1) AS update_id_new_val_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id2']::text[])
  AND col_name = 'val' \gset
\if :update_id_new_val_ok
\echo [PASS] (:testid) Test update id only (new val)
\else
\echo [FAIL] (:testid) Test update id only (new val)
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT (COUNT(*) = 1) AS update_id_new_tombstone_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id2']::text[])
  AND col_name = '__[RIP]__' \gset
\if :update_id_new_tombstone_ok
\echo [PASS] (:testid) Test update id only (new tombstone)
\else
\echo [FAIL] (:testid) Test update id only (new tombstone)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test update id and val'
SELECT cloudsync_uuid() AS smoke_id3 \gset
UPDATE smoke_tbl SET id = :'smoke_id3', val = 'hello3' WHERE id = :'smoke_id2';
SELECT (COUNT(*) = 1) AS update_both_old_tombstone_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id2']::text[])
  AND col_name = '__[RIP]__' \gset
\if :update_both_old_tombstone_ok
\echo [PASS] (:testid) Test update id and val (old tombstone)
\else
\echo [FAIL] (:testid) Test update id and val (old tombstone)
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT (COUNT(*) = 0) AS update_both_old_val_gone_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id2']::text[])
  AND col_name = 'val' \gset
\if :update_both_old_val_gone_ok
\echo [PASS] (:testid) Test update id and val (old val gone)
\else
\echo [FAIL] (:testid) Test update id and val (old val gone)
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT (COUNT(*) = 1) AS update_both_new_val_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id3']::text[])
  AND col_name = 'val' \gset
\if :update_both_new_val_ok
\echo [PASS] (:testid) Test update id and val (new val)
\else
\echo [FAIL] (:testid) Test update id and val (new val)
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT (COUNT(*) = 1) AS update_both_new_tombstone_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id3']::text[])
  AND col_name = '__[RIP]__' \gset
\if :update_both_new_tombstone_ok
\echo [PASS] (:testid) Test update id and val (new tombstone)
\else
\echo [FAIL] (:testid) Test update id and val (new tombstone)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test delete metadata tombstone'
DELETE FROM smoke_tbl WHERE id = :'smoke_id3';
SELECT (COUNT(*) = 1) AS delete_meta_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id3']::text[])
  AND col_name = '__[RIP]__' \gset
\if :delete_meta_ok
\echo [PASS] (:testid) Test delete metadata tombstone
\else
\echo [FAIL] (:testid) Test delete metadata tombstone
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test delete metadata fields'
SELECT (db_version > 0 AND seq >= 0) AS delete_meta_fields_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id3']::text[])
  AND col_name = '__[RIP]__' \gset
\if :delete_meta_fields_ok
\echo [PASS] (:testid) Test delete metadata fields
\else
\echo [FAIL] (:testid) Test delete metadata fields
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test delete removes non-tombstone metadata'
SELECT (COUNT(*) = 0) AS delete_meta_only_ok
FROM smoke_tbl_cloudsync
WHERE pk = cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id3']::text[])
  AND col_name != '__[RIP]__' \gset
\if :delete_meta_only_ok
\echo [PASS] (:testid) Test delete removes non-tombstone metadata
\else
\echo [FAIL] (:testid) Test delete removes non-tombstone metadata
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test cloudsync_changes view write'
SELECT cloudsync_uuid() AS smoke_id4 \gset
INSERT INTO cloudsync_changes (tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq)
VALUES (
  'smoke_tbl',
  cloudsync_pk_encode(VARIADIC ARRAY[:'smoke_id4']::text[]),
  'val',
  -- "change_write" encoded as cloudsync text value (type 0x61 + len 0x0c)
  decode('0b0c6368616e67655f7772697465', 'hex'),
  1,
  cloudsync_db_version_next(),
  cloudsync_siteid(),
  1,
  0
);
SELECT (COUNT(*) = 1) AS changes_write_row_ok
FROM smoke_tbl
WHERE id = :'smoke_id4' AND val = 'change_write' \gset
\if :changes_write_row_ok
\echo [PASS] (:testid) Test cloudsync_changes view write
\else
\echo [FAIL] (:testid) Test cloudsync_changes view write
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test cloudsync_changes view read'
SELECT COUNT(*) AS changes_view_count
FROM cloudsync_changes
WHERE tbl = 'smoke_tbl' \gset
SELECT COUNT(*) AS changes_meta_count
FROM smoke_tbl_cloudsync \gset
SELECT (:changes_view_count::int = :changes_meta_count::int) AS changes_read_ok \gset
\if :changes_read_ok
\echo [PASS] (:testid) Test cloudsync_changes view read
\else
\echo [FAIL] (:testid) Test cloudsync_changes view read
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test site id visibility'
SELECT cloudsync_siteid() AS site_id \gset
\echo [PASS] (:testid) Test site id visibility :site_id

-- 'Test site id encoding'
SELECT (length(encode(cloudsync_siteid()::bytea, 'hex')) > 0) AS sid_ok \gset
\if :sid_ok
\echo [PASS] (:testid) Test site id encoding
\else
\echo [FAIL] (:testid) Test site id encoding
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 'Test double init no-op'
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id2 \gset
SELECT cloudsync_init('smoke_tbl', 'CLS', true) AS _init_site_id3 \gset
\echo [PASS] (:testid) Test double init no-op

-- 'Test payload encode signature'
SELECT md5(COALESCE(string_agg(id || ':' || COALESCE(val, ''), ',' ORDER BY id), '')) AS smoke_hash
FROM smoke_tbl \gset
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset
SELECT (length(:'payload_hex') > 0 AND substring(:'payload_hex' from 1 for 8) = '434c5359') AS payload_sig_ok \gset
\if :payload_sig_ok
\echo [PASS] (:testid) Test payload encode signature
\else
\echo [FAIL] (:testid) Test payload encode signature
SELECT (:fail::int + 1) AS fail \gset
\endif