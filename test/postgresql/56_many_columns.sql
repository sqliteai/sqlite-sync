-- Wide-table payload apply.
--
-- Exercises the old fixed 32-parameter boundary (32 data columns + one PK)
-- and multiple dynamic parameter-array growths (127 data columns + one PK).
-- Both INSERT/upsert and UPDATE paths must reproduce every column.

\set testid '56-many-columns'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_56_many_columns_src;
DROP DATABASE IF EXISTS cloudsync_test_56_many_columns_dst;
CREATE DATABASE cloudsync_test_56_many_columns_src;
CREATE DATABASE cloudsync_test_56_many_columns_dst;

\connect cloudsync_test_56_many_columns_src
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

SELECT 'CREATE TABLE wide_33 (id TEXT PRIMARY KEY' ||
       string_agg(format(', col%s TEXT', i), '' ORDER BY i) || ');'
FROM generate_series(1, 32) AS g(i) \gexec
SELECT 'CREATE TABLE wide_128 (id TEXT PRIMARY KEY' ||
       string_agg(format(', col%s TEXT', i), '' ORDER BY i) || ');'
FROM generate_series(1, 127) AS g(i) \gexec

SELECT cloudsync_init('wide_33', 'CLS', 1) AS _init_33 \gset
SELECT cloudsync_init('wide_128', 'CLS', 1) AS _init_128 \gset

SELECT 'INSERT INTO wide_33 VALUES (' || quote_literal('row-33') || ', ' ||
       string_agg(quote_literal('v' || i), ', ' ORDER BY i) || ');'
FROM generate_series(1, 32) AS g(i) \gexec
SELECT 'INSERT INTO wide_128 VALUES (' || quote_literal('row-128') || ', ' ||
       string_agg(quote_literal('v' || i), ', ' ORDER BY i) || ');'
FROM generate_series(1, 127) AS g(i) \gexec

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version,
                                       db_version, site_id, cl, seq), 'hex') AS insert_payload
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_test_56_many_columns_dst
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

SELECT 'CREATE TABLE wide_33 (id TEXT PRIMARY KEY' ||
       string_agg(format(', col%s TEXT', i), '' ORDER BY i) || ');'
FROM generate_series(1, 32) AS g(i) \gexec
SELECT 'CREATE TABLE wide_128 (id TEXT PRIMARY KEY' ||
       string_agg(format(', col%s TEXT', i), '' ORDER BY i) || ');'
FROM generate_series(1, 127) AS g(i) \gexec

SELECT cloudsync_init('wide_33', 'CLS', 1) AS _init_dst_33 \gset
SELECT cloudsync_init('wide_128', 'CLS', 1) AS _init_dst_128 \gset
SELECT cloudsync_payload_apply(decode(:'insert_payload', 'hex')) AS insert_apply_count \gset

SELECT coalesce(count(*) = 32 AND bool_and(value = 'v' || substring(key FROM 4)), false) AS insert_33_ok
FROM wide_33 AS w
CROSS JOIN LATERAL jsonb_each_text(to_jsonb(w) - 'id')
WHERE id = 'row-33' \gset
SELECT coalesce(count(*) = 127 AND bool_and(value = 'v' || substring(key FROM 4)), false) AS insert_128_ok
FROM wide_128 AS w
CROSS JOIN LATERAL jsonb_each_text(to_jsonb(w) - 'id')
WHERE id = 'row-128' \gset

SELECT (:insert_apply_count::int = 159 AND :'insert_33_ok'::boolean AND :'insert_128_ok'::boolean) AS insert_ok \gset
\if :insert_ok
\echo [PASS] (:testid) wide INSERT apply crossed the old 32-parameter limit (:insert_apply_count changes)
\else
\echo [FAIL] (:testid) wide INSERT apply failed (count=:insert_apply_count, 33=:insert_33_ok, 128=:insert_128_ok)
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect cloudsync_test_56_many_columns_src
SELECT 'UPDATE wide_33 SET ' ||
       string_agg(format('col%s = %L', i, 'u' || i), ', ' ORDER BY i) ||
       ' WHERE id = ''row-33'';'
FROM generate_series(1, 32) AS g(i) \gexec
SELECT 'UPDATE wide_128 SET ' ||
       string_agg(format('col%s = %L', i, 'u' || i), ', ' ORDER BY i) ||
       ' WHERE id = ''row-128'';'
FROM generate_series(1, 127) AS g(i) \gexec

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version,
                                       db_version, site_id, cl, seq), 'hex') AS update_payload
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_test_56_many_columns_dst
SELECT cloudsync_payload_apply(decode(:'update_payload', 'hex')) AS update_apply_count \gset

SELECT coalesce(count(*) = 32 AND bool_and(value = 'u' || substring(key FROM 4)), false) AS update_33_ok
FROM wide_33 AS w
CROSS JOIN LATERAL jsonb_each_text(to_jsonb(w) - 'id')
WHERE id = 'row-33' \gset
SELECT coalesce(count(*) = 127 AND bool_and(value = 'u' || substring(key FROM 4)), false) AS update_128_ok
FROM wide_128 AS w
CROSS JOIN LATERAL jsonb_each_text(to_jsonb(w) - 'id')
WHERE id = 'row-128' \gset

SELECT (:update_apply_count::int = 159 AND :'update_33_ok'::boolean AND :'update_128_ok'::boolean) AS update_ok \gset
\if :update_ok
\echo [PASS] (:testid) wide UPDATE apply preserved every column (:update_apply_count changes)
\else
\echo [FAIL] (:testid) wide UPDATE apply failed (count=:update_apply_count, 33=:update_33_ok, 128=:update_128_ok)
SELECT (:fail::int + 1) AS fail \gset
\endif

\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_56_many_columns_src;
DROP DATABASE IF EXISTS cloudsync_test_56_many_columns_dst;
\endif
