-- CloudSync 1.0 -> 1.1
-- Adds declarative schema migration SQL functions.

DROP FUNCTION IF EXISTS cloudsync_begin_alter(text);
DROP FUNCTION IF EXISTS cloudsync_commit_alter(text);

CREATE OR REPLACE FUNCTION cloudsync_alter_create_table(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_create_table'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_add_column(table_name text, column_name text, logical_type text, nullable boolean)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_add_column'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_add_column(table_name text, column_name text, logical_type text, nullable boolean, default_value text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_add_column'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_add_column_sqlite(table_name text, column_name text, type_sql text, nullable boolean)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_add_column_sqlite'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_add_column_sqlite(table_name text, column_name text, type_sql text, nullable boolean, default_sql text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_add_column_sqlite'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_add_column_postgresql(table_name text, column_name text, type_sql text, nullable boolean)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_add_column_postgresql'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_add_column_postgresql(table_name text, column_name text, type_sql text, nullable boolean, default_sql text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_add_column_postgresql'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_sql(sql text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_sql'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_sqlite(sql text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_sqlite'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_postgresql(sql text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_postgresql'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_add_primary_key(table_name text, column_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_add_primary_key'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_augment_table(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_augment_table'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_augment_table(table_name text, algo text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_augment_table'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_augment_table(table_name text, algo text, init_flags integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_augment_table'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_set_block_lww(table_name text, column_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_set_block_lww'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_set_block_lww(table_name text, column_name text, delimiter text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_set_block_lww'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_set_column(table_name text, column_name text, key text, value text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_set_column'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_set_filter(table_name text, filter_expr text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_set_filter'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_set_filter_sqlite(table_name text, filter_expr text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_set_filter_sqlite'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_set_filter_postgresql(table_name text, filter_expr text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_set_filter_postgresql'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_drop_column(table_name text, column_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_drop_column'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_rename_column(table_name text, from_name text, to_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_rename_column'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_clear()
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_clear'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_clear(table_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_clear'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_preview()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_preview'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_alter_apply()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_cloudsync_alter_apply'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION cloudsync_migration_apply(payload text)
RETURNS text
AS 'MODULE_PATHNAME', 'pg_cloudsync_migration_apply'
LANGUAGE C VOLATILE;
