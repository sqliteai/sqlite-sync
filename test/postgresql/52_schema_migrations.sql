-- Schema migration payload tests
-- Covers local V1/V2 migration application for PostgreSQL.

\set testid '52'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

DROP DATABASE IF EXISTS cloudsync_test_52;
CREATE DATABASE cloudsync_test_52;

\connect cloudsync_test_52
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- ============================================================================
-- V1: create table + augment + block-level LWW
-- ============================================================================

SELECT cloudsync_migration_apply($json$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 1,
  "migrationId": "mig-pg-v1-create",
  "requiredCapabilities": ["schema:write"],
  "ops": [
    {
      "op": "createTable",
      "table": "notes",
      "columns": [
        {"name": "id", "type": "uuid", "primaryKey": true, "nullable": false},
        {"name": "body", "type": "text", "nullable": false, "default": {"type": "text", "value": ""}}
      ]
    },
    {"op": "augmentTable", "table": "notes", "algorithm": "CLS", "initFlags": 0},
    {"op": "setBlockLww", "table": "notes", "column": "body", "delimiter": "\n"}
  ]
}
$json$) AS migration_v1_create \gset

INSERT INTO notes (id, body)
VALUES ('11111111-1111-1111-1111-111111111111', E'a\nb\nc');

SELECT count(*) = 3 AS v1_blocks_ok
FROM notes_cloudsync_blocks
WHERE pk = cloudsync_pk_encode('11111111-1111-1111-1111-111111111111'::uuid) \gset

SELECT is_nullable = 'NO' AS v1_pk_notnull_ok
FROM information_schema.columns
WHERE table_name = 'notes' AND column_name = 'id' \gset

\if :v1_blocks_ok
\if :v1_pk_notnull_ok
\echo [PASS] (:testid) V1 create/augment/block migration created block metadata
\else
\echo [FAIL] (:testid) V1 create migration did not mark the primary key NOT NULL
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V1 create/augment/block migration did not create expected block metadata
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- V1: add column + idempotency
-- ============================================================================

SELECT cloudsync_migration_apply($json$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 1,
  "migrationId": "mig-pg-v1-add-column",
  "requiredCapabilities": ["schema:write"],
  "ops": [
    {
      "op": "addColumn",
      "table": "notes",
      "column": {
        "name": "subtitle",
        "type": "text",
        "nullable": false,
        "default": {"type": "text", "value": ""}
      }
    }
  ]
}
$json$) AS migration_v1_add \gset

SELECT cloudsync_migration_apply($json$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 1,
  "migrationId": "mig-pg-v1-add-column",
  "requiredCapabilities": ["schema:write"],
  "ops": [
    {
      "op": "addColumn",
      "table": "notes",
      "column": {
        "name": "subtitle",
        "type": "text",
        "nullable": false,
        "default": {"type": "text", "value": ""}
      }
    }
  ]
}
$json$) AS migration_v1_add_again \gset

SELECT count(*) = 1 AS v1_add_column_ok
FROM information_schema.columns
WHERE table_name = 'notes' AND column_name = 'subtitle' \gset

SELECT count(*) = 1 AS v1_idempotent_ok
FROM cloudsync_migrations
WHERE migration_id = 'mig-pg-v1-add-column' \gset

\if :v1_add_column_ok
\if :v1_idempotent_ok
\echo [PASS] (:testid) V1 addColumn migration is idempotent
\else
\echo [FAIL] (:testid) V1 addColumn migration idempotency record mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V1 addColumn migration did not add subtitle
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- V1: large payload requires dynamically sized JSON token parsing
-- ============================================================================

WITH cols AS (
  SELECT 0 AS ord, jsonb_build_object(
    'name', 'id',
    'type', 'uuid',
    'primaryKey', true,
    'nullable', false
  ) AS col
  UNION ALL
  SELECT gs + 1 AS ord, jsonb_build_object(
    'name', format('extra_%s', lpad(gs::text, 3, '0')),
    'type', 'text',
    'nullable', true
  ) AS col
  FROM generate_series(0, 699) AS gs
), payload AS (
  SELECT jsonb_build_object(
    'type', 'cloudsync.schema.migration',
    'formatVersion', 1,
    'migrationId', '0197097c-8b35-7c11-8ed4-4e59ddfdb929',
    'requiredCapabilities', jsonb_build_array('schema:write'),
    'ops', jsonb_build_array(jsonb_build_object(
      'op', 'createTable',
      'table', 'pg_wide_dynamic_notes',
      'columns', (SELECT jsonb_agg(col ORDER BY ord) FROM cols)
    ))
  ) AS doc
)
SELECT cloudsync_migration_apply(doc::text) AS migration_large_json
FROM payload \gset

SELECT count(*) = 701 AS large_json_columns_ok
FROM information_schema.columns
WHERE table_name = 'pg_wide_dynamic_notes' \gset

SELECT count(*) = 1 AS large_json_record_ok
FROM cloudsync_migrations
WHERE migration_id = '0197097c-8b35-7c11-8ed4-4e59ddfdb929' \gset

\if :large_json_columns_ok
\if :large_json_record_ok
\echo [PASS] (:testid) Large migration JSON payload parsed dynamically
\else
\echo [FAIL] (:testid) Large migration JSON payload was not recorded
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Large migration JSON payload did not create all columns
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- V2: metadata-preserving renameColumn
-- ============================================================================

SELECT cloudsync_migration_apply($json$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 2,
  "migrationId": "mig-pg-v2-rename-column",
  "requiredCapabilities": ["schema:destructive"],
  "ops": [
    {"op": "renameColumn", "table": "notes", "from": "subtitle", "to": "summary"}
  ]
}
$json$) AS migration_v2_rename \gset

UPDATE notes
SET summary = 'renamed column works'
WHERE id = '11111111-1111-1111-1111-111111111111';

SELECT count(*) = 1 AS v2_rename_col_ok
FROM information_schema.columns
WHERE table_name = 'notes' AND column_name = 'summary' \gset

SELECT count(*) = 0 AS v2_rename_old_meta_ok
FROM notes_cloudsync
WHERE col_name = 'subtitle' \gset

\if :v2_rename_col_ok
\if :v2_rename_old_meta_ok
\echo [PASS] (:testid) V2 renameColumn migration updated schema and metadata
\else
\echo [FAIL] (:testid) V2 renameColumn migration left old metadata
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 renameColumn migration did not rename column
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- V2: rebuildTableSync with destructive DDL
-- ============================================================================

SELECT cloudsync_cleanup('notes') AS _cleanup_notes \gset

CREATE TABLE rebuild_docs (
  id UUID PRIMARY KEY,
  title TEXT NOT NULL DEFAULT '',
  legacy TEXT NOT NULL DEFAULT ''
);
SELECT cloudsync_init('rebuild_docs', 'CLS', 0) AS _init_rebuild \gset
INSERT INTO rebuild_docs VALUES ('22222222-2222-2222-2222-222222222222', 'title', 'legacy');

SELECT encode(site_id, 'hex') AS rebuild_site_before
FROM cloudsync_site_id
WHERE id = 0 \gset

SELECT cloudsync_migration_apply($json$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 2,
  "migrationId": "mig-pg-v2-rebuild",
  "requiredCapabilities": ["schema:destructive"],
  "ops": [
    {
      "op": "rebuildTableSync",
      "table": "rebuild_docs",
      "algorithm": "CLS",
      "initFlags": 0,
      "ddl": [
        {"op": "rawSql", "sql": {"postgresql": ["ALTER TABLE rebuild_docs DROP COLUMN legacy"]}}
      ]
    }
  ]
}
$json$) AS migration_v2_rebuild \gset

INSERT INTO rebuild_docs (id, title)
VALUES ('33333333-3333-3333-3333-333333333333', 'new');

SELECT count(*) = 0 AS v2_rebuild_drop_ok
FROM information_schema.columns
WHERE table_name = 'rebuild_docs' AND column_name = 'legacy' \gset

SELECT count(*) = 1 AS v2_rebuild_meta_ok
FROM information_schema.tables
WHERE table_name = 'rebuild_docs_cloudsync' \gset

SELECT encode(site_id, 'hex') = :'rebuild_site_before' AS v2_rebuild_site_ok
FROM cloudsync_site_id
WHERE id = 0 \gset

\if :v2_rebuild_drop_ok
\if :v2_rebuild_meta_ok
\if :v2_rebuild_site_ok
\echo [PASS] (:testid) V2 rebuildTableSync migration dropped column and recreated metadata
\else
\echo [FAIL] (:testid) V2 rebuildTableSync migration changed site identity
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 rebuildTableSync migration did not recreate metadata
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 rebuildTableSync migration did not drop legacy column
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- V2: reject malformed rebuildTableSync DDL payloads
-- ============================================================================

CREATE TABLE bad_rebuild_docs (
  id UUID PRIMARY KEY,
  title TEXT NOT NULL DEFAULT '',
  legacy TEXT NOT NULL DEFAULT ''
);
SELECT cloudsync_init('bad_rebuild_docs', 'CLS', 0) AS _init_bad_rebuild \gset
INSERT INTO bad_rebuild_docs VALUES ('44444444-4444-4444-4444-444444444444', 'title', 'legacy');

CREATE TEMP TABLE schema_migration_error_flags (
  key TEXT PRIMARY KEY,
  ok BOOLEAN NOT NULL
) ON COMMIT PRESERVE ROWS;

DO $$
DECLARE
  rejected BOOLEAN := false;
BEGIN
  BEGIN
    PERFORM cloudsync_migration_apply($payload$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 2,
  "migrationId": "mig-pg-v2-rebuild-bad-ddl",
  "requiredCapabilities": ["schema:destructive"],
  "ops": [
    {
      "op": "rebuildTableSync",
      "table": "bad_rebuild_docs",
      "algorithm": "CLS",
      "initFlags": 0,
      "ddl": {"op": "rawSql", "sql": {"postgresql": ["ALTER TABLE bad_rebuild_docs DROP COLUMN legacy"]}}
    }
  ]
}
$payload$);
  EXCEPTION WHEN OTHERS THEN
    rejected := true;
  END;

  INSERT INTO schema_migration_error_flags VALUES ('bad_rebuild_ddl_rejected', rejected);
END
$$;

SELECT ok AS v2_rebuild_bad_ddl_rejected_ok
FROM schema_migration_error_flags
WHERE key = 'bad_rebuild_ddl_rejected' \gset

SELECT count(*) = 0 AS v2_rebuild_bad_ddl_unrecorded_ok
FROM cloudsync_migrations
WHERE migration_id = 'mig-pg-v2-rebuild-bad-ddl' \gset

SELECT count(*) = 1 AS v2_rebuild_bad_ddl_legacy_ok
FROM information_schema.columns
WHERE table_name = 'bad_rebuild_docs' AND column_name = 'legacy' \gset

\if :v2_rebuild_bad_ddl_rejected_ok
\if :v2_rebuild_bad_ddl_unrecorded_ok
\if :v2_rebuild_bad_ddl_legacy_ok
\echo [PASS] (:testid) V2 rebuildTableSync rejected malformed DDL payload
\else
\echo [FAIL] (:testid) V2 malformed rebuildTableSync DDL altered the table
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 malformed rebuildTableSync DDL was recorded
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 malformed rebuildTableSync DDL was accepted
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- V2: reject malformed rebuildTableSync blockLww payloads
-- ============================================================================

CREATE TABLE bad_block_docs (
  id UUID PRIMARY KEY,
  body TEXT NOT NULL DEFAULT ''
);
SELECT cloudsync_init('bad_block_docs', 'CLS', 0) AS _init_bad_block \gset
INSERT INTO bad_block_docs VALUES ('55555555-5555-5555-5555-555555555555', E'line1\nline2');

DO $$
DECLARE
  rejected BOOLEAN := false;
BEGIN
  BEGIN
    PERFORM cloudsync_migration_apply($payload$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 2,
  "migrationId": "mig-pg-v2-rebuild-bad-block",
  "requiredCapabilities": ["schema:destructive"],
  "ops": [
    {
      "op": "rebuildTableSync",
      "table": "bad_block_docs",
      "algorithm": "CLS",
      "initFlags": 0,
      "blockLww": {"column": "body", "delimiter": "\n"}
    }
  ]
}
$payload$);
  EXCEPTION WHEN OTHERS THEN
    rejected := true;
  END;

  INSERT INTO schema_migration_error_flags VALUES ('bad_rebuild_block_rejected', rejected);
END
$$;

SELECT ok AS v2_rebuild_bad_block_rejected_ok
FROM schema_migration_error_flags
WHERE key = 'bad_rebuild_block_rejected' \gset

SELECT count(*) = 0 AS v2_rebuild_bad_block_unrecorded_ok
FROM cloudsync_migrations
WHERE migration_id = 'mig-pg-v2-rebuild-bad-block' \gset

SELECT count(*) = 1 AS v2_rebuild_bad_block_meta_ok
FROM information_schema.tables
WHERE table_name = 'bad_block_docs_cloudsync' \gset

\if :v2_rebuild_bad_block_rejected_ok
\if :v2_rebuild_bad_block_unrecorded_ok
\if :v2_rebuild_bad_block_meta_ok
\echo [PASS] (:testid) V2 rebuildTableSync rejected malformed blockLww payload
\else
\echo [FAIL] (:testid) V2 malformed rebuildTableSync blockLww dropped metadata
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 malformed rebuildTableSync blockLww was recorded
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 malformed rebuildTableSync blockLww was accepted
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Reject migration payloads with trailing JSON data
-- ============================================================================

DO $$
DECLARE
  rejected BOOLEAN := false;
BEGIN
  BEGIN
    PERFORM cloudsync_migration_apply($payload$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 1,
  "migrationId": "mig-pg-invalid-json",
  "ops": [
    {
      "op": "createTable",
      "table": "invalid_json_notes",
      "columns": [
        {"name": "id", "type": "uuid", "primaryKey": true, "nullable": false}
      ]
    }
  ]
}
true
$payload$);
  EXCEPTION WHEN OTHERS THEN
    rejected := true;
  END;

  INSERT INTO schema_migration_error_flags VALUES ('invalid_json_rejected', rejected);
END
$$;

SELECT ok AS invalid_json_rejected_ok
FROM schema_migration_error_flags
WHERE key = 'invalid_json_rejected' \gset

SELECT count(*) = 0 AS invalid_json_table_absent_ok
FROM information_schema.tables
WHERE table_name = 'invalid_json_notes' \gset

SELECT count(*) = 0 AS invalid_json_unrecorded_ok
FROM cloudsync_migrations
WHERE migration_id = 'mig-pg-invalid-json' \gset

\if :invalid_json_rejected_ok
\if :invalid_json_table_absent_ok
\if :invalid_json_unrecorded_ok
\echo [PASS] (:testid) Invalid migration JSON payload was rejected before applying
\else
\echo [FAIL] (:testid) Invalid migration JSON payload was recorded
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Invalid migration JSON payload altered the schema
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Invalid migration JSON payload was accepted
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Reject malformed column boolean flags
-- ============================================================================

DO $$
DECLARE
  rejected BOOLEAN := false;
BEGIN
  BEGIN
    PERFORM cloudsync_migration_apply($payload$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 1,
  "migrationId": "mig-pg-bad-bool-pk",
  "ops": [
    {
      "op": "createTable",
      "table": "bad_bool_pk_notes",
      "columns": [
        {"name": "id", "type": "uuid", "primaryKey": "true", "nullable": false}
      ]
    }
  ]
}
$payload$);
  EXCEPTION WHEN OTHERS THEN
    rejected := true;
  END;

  INSERT INTO schema_migration_error_flags VALUES ('bad_bool_pk_rejected', rejected);
END
$$;

CREATE TABLE bad_bool_add_notes (
  id UUID PRIMARY KEY,
  title TEXT NOT NULL DEFAULT ''
);
SELECT cloudsync_init('bad_bool_add_notes', 'CLS', 0) AS _bad_bool_init \gset

DO $$
DECLARE
  rejected BOOLEAN := false;
BEGIN
  BEGIN
    PERFORM cloudsync_migration_apply($payload$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 1,
  "migrationId": "mig-pg-bad-bool-nullable",
  "ops": [
    {
      "op": "addColumn",
      "table": "bad_bool_add_notes",
      "column": {
        "name": "summary",
        "type": "text",
        "nullable": "false",
        "default": {"type": "text", "value": ""}
      }
    }
  ]
}
$payload$);
  EXCEPTION WHEN OTHERS THEN
    rejected := true;
  END;

  INSERT INTO schema_migration_error_flags VALUES ('bad_bool_nullable_rejected', rejected);
END
$$;

SELECT ok AS bad_bool_pk_rejected_ok
FROM schema_migration_error_flags
WHERE key = 'bad_bool_pk_rejected' \gset

SELECT ok AS bad_bool_nullable_rejected_ok
FROM schema_migration_error_flags
WHERE key = 'bad_bool_nullable_rejected' \gset

SELECT count(*) = 0 AS bad_bool_pk_table_absent_ok
FROM information_schema.tables
WHERE table_name = 'bad_bool_pk_notes' \gset

SELECT count(*) = 0 AS bad_bool_add_column_absent_ok
FROM information_schema.columns
WHERE table_name = 'bad_bool_add_notes' AND column_name = 'summary' \gset

SELECT count(*) = 0 AS bad_bool_unrecorded_ok
FROM cloudsync_migrations
WHERE migration_id IN ('mig-pg-bad-bool-pk', 'mig-pg-bad-bool-nullable') \gset

\if :bad_bool_pk_rejected_ok
\if :bad_bool_nullable_rejected_ok
\if :bad_bool_pk_table_absent_ok
\if :bad_bool_add_column_absent_ok
\if :bad_bool_unrecorded_ok
\echo [PASS] (:testid) Malformed column boolean flags were rejected
\else
\echo [FAIL] (:testid) Malformed column boolean migration was recorded
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Malformed nullable flag added a column
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Malformed primaryKey flag created a table
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Malformed nullable flag was accepted
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Malformed primaryKey flag was accepted
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- V2: rebuildTableSync with valid blockLww array
-- ============================================================================

CREATE TABLE rebuild_block_docs (
  id UUID PRIMARY KEY,
  body TEXT NOT NULL DEFAULT ''
);
SELECT cloudsync_init('rebuild_block_docs', 'CLS', 0) AS _init_rebuild_block \gset
INSERT INTO rebuild_block_docs VALUES ('66666666-6666-6666-6666-666666666666', E'one\ntwo');

SELECT cloudsync_migration_apply($json$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 2,
  "migrationId": "mig-pg-v2-rebuild-good-block",
  "requiredCapabilities": ["schema:destructive"],
  "ops": [
    {
      "op": "rebuildTableSync",
      "table": "rebuild_block_docs",
      "algorithm": "CLS",
      "initFlags": 0,
      "blockLww": [{"column": "body", "delimiter": "\n"}]
    }
  ]
}
$json$) AS migration_v2_rebuild_good_block \gset

UPDATE rebuild_block_docs
SET body = E'one\ntwo\nthree'
WHERE id = '66666666-6666-6666-6666-666666666666';

SELECT count(*) = 3 AS v2_rebuild_good_block_blocks_ok
FROM rebuild_block_docs_cloudsync_blocks \gset

SELECT count(*) = 1 AS v2_rebuild_good_block_setting_ok
FROM cloudsync_table_settings
WHERE tbl_name = 'rebuild_block_docs' AND col_name = 'body' AND key = 'algo' AND value = 'block' \gset

\if :v2_rebuild_good_block_blocks_ok
\if :v2_rebuild_good_block_setting_ok
\echo [PASS] (:testid) V2 rebuildTableSync applied valid blockLww array
\else
\echo [FAIL] (:testid) V2 rebuildTableSync blockLww did not persist setting
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 rebuildTableSync blockLww did not create expected blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- V2: rename block-level LWW column metadata
-- ============================================================================

CREATE TABLE rename_block_docs (
  id UUID PRIMARY KEY,
  body TEXT NOT NULL DEFAULT ''
);
SELECT cloudsync_init('rename_block_docs', 'CLS', 0) AS _init_rename_block \gset
SELECT cloudsync_set_column('rename_block_docs', 'body', 'algo', 'block') AS _set_rename_block \gset
INSERT INTO rename_block_docs VALUES ('77777777-7777-7777-7777-777777777777', E'a\nb');

SELECT cloudsync_migration_apply($json$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 2,
  "migrationId": "mig-pg-v2-rename-block",
  "requiredCapabilities": ["schema:destructive"],
  "ops": [
    {"op": "renameColumn", "table": "rename_block_docs", "from": "body", "to": "content"}
  ]
}
$json$) AS migration_v2_rename_block \gset

UPDATE rename_block_docs
SET content = E'a\nb\nc'
WHERE id = '77777777-7777-7777-7777-777777777777';

SELECT count(*) = 1 AS v2_rename_block_col_ok
FROM information_schema.columns
WHERE table_name = 'rename_block_docs' AND column_name = 'content' \gset

SELECT count(*) = 0 AS v2_rename_block_old_meta_ok
FROM rename_block_docs_cloudsync
WHERE col_name = 'body' OR col_name LIKE 'body' || chr(31) || '%' \gset

SELECT count(*) > 0 AS v2_rename_block_new_meta_ok
FROM rename_block_docs_cloudsync
WHERE col_name = 'content' OR col_name LIKE 'content' || chr(31) || '%' \gset

SELECT count(*) = 0 AS v2_rename_block_old_blocks_ok
FROM rename_block_docs_cloudsync_blocks
WHERE col_name = 'body' OR col_name LIKE 'body' || chr(31) || '%' \gset

SELECT count(*) > 0 AS v2_rename_block_new_blocks_ok
FROM rename_block_docs_cloudsync_blocks
WHERE col_name = 'content' OR col_name LIKE 'content' || chr(31) || '%' \gset

SELECT count(*) = 1 AS v2_rename_block_setting_ok
FROM cloudsync_table_settings
WHERE tbl_name = 'rename_block_docs' AND col_name = 'content' AND key = 'algo' AND value = 'block' \gset

\if :v2_rename_block_col_ok
\if :v2_rename_block_old_meta_ok
\if :v2_rename_block_new_meta_ok
\if :v2_rename_block_old_blocks_ok
\if :v2_rename_block_new_blocks_ok
\if :v2_rename_block_setting_ok
\echo [PASS] (:testid) V2 renameColumn migration updated block metadata and settings
\else
\echo [FAIL] (:testid) V2 renameColumn block setting was not renamed
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 renameColumn block metadata missing new block entries
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 renameColumn block metadata left old block entries
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 renameColumn block metadata missing new meta entries
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 renameColumn block metadata left old meta entries
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 renameColumn block column was not renamed
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- V2: direct dropColumn operation
-- ============================================================================

CREATE TABLE direct_drop_docs (
  id UUID PRIMARY KEY,
  title TEXT NOT NULL DEFAULT '',
  legacy TEXT NOT NULL DEFAULT ''
);
SELECT cloudsync_init('direct_drop_docs', 'CLS', 0) AS _init_direct_drop \gset
INSERT INTO direct_drop_docs VALUES ('88888888-8888-8888-8888-888888888888', 'title', 'legacy');

SELECT cloudsync_migration_apply($json$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 2,
  "migrationId": "mig-pg-v2-drop-column",
  "requiredCapabilities": ["schema:destructive"],
  "ops": [
    {"op": "dropColumn", "table": "direct_drop_docs", "column": "legacy"}
  ]
}
$json$) AS migration_v2_drop_column \gset

INSERT INTO direct_drop_docs (id, title)
VALUES ('99999999-9999-9999-9999-999999999999', 'new');

SELECT count(*) = 0 AS v2_drop_column_absent_ok
FROM information_schema.columns
WHERE table_name = 'direct_drop_docs' AND column_name = 'legacy' \gset

SELECT count(*) = 0 AS v2_drop_column_meta_ok
FROM direct_drop_docs_cloudsync
WHERE col_name = 'legacy' \gset

SELECT count(*) = 2 AS v2_drop_column_rows_ok
FROM direct_drop_docs \gset

\if :v2_drop_column_absent_ok
\if :v2_drop_column_meta_ok
\if :v2_drop_column_rows_ok
\echo [PASS] (:testid) V2 dropColumn migration dropped schema and metadata
\else
\echo [FAIL] (:testid) V2 dropColumn migration left wrong row count
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 dropColumn migration left old metadata
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 dropColumn migration did not drop column
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- V2: drop block-level LWW column cleans table-local metadata
-- ============================================================================

CREATE TABLE drop_block_docs (
  id UUID PRIMARY KEY,
  title TEXT NOT NULL DEFAULT '',
  body TEXT NOT NULL DEFAULT ''
);
SELECT cloudsync_init('drop_block_docs', 'CLS', 0) AS _init_drop_block \gset
SELECT cloudsync_set_column('drop_block_docs', 'body', 'algo', 'block') AS _set_drop_block \gset
INSERT INTO drop_block_docs VALUES ('abababab-abab-abab-abab-abababababab', 'title', E'a\nb');

SELECT count(*) > 0 AS v2_drop_block_before_blocks_ok
FROM drop_block_docs_cloudsync_blocks
WHERE col_name LIKE 'body' || chr(31) || '%' \gset

SELECT count(*) > 0 AS v2_drop_block_before_settings_ok
FROM cloudsync_table_settings
WHERE tbl_name = 'drop_block_docs' AND col_name = 'body' \gset

SELECT cloudsync_migration_apply($json$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 2,
  "migrationId": "mig-pg-v2-drop-block-column",
  "requiredCapabilities": ["schema:destructive"],
  "ops": [
    {"op": "dropColumn", "table": "drop_block_docs", "column": "body"}
  ]
}
$json$) AS migration_v2_drop_block_column \gset

UPDATE drop_block_docs
SET title = 'changed'
WHERE id = 'abababab-abab-abab-abab-abababababab';

SELECT count(*) = 0 AS v2_drop_block_column_absent_ok
FROM information_schema.columns
WHERE table_name = 'drop_block_docs' AND column_name = 'body' \gset

SELECT count(*) = 0 AS v2_drop_block_meta_ok
FROM drop_block_docs_cloudsync
WHERE col_name = 'body' OR col_name LIKE 'body' || chr(31) || '%' \gset

SELECT count(*) = 0 AS v2_drop_block_rows_ok
FROM drop_block_docs_cloudsync_blocks
WHERE col_name LIKE 'body' || chr(31) || '%' \gset

SELECT count(*) = 0 AS v2_drop_block_settings_ok
FROM cloudsync_table_settings
WHERE tbl_name = 'drop_block_docs' AND col_name = 'body' \gset

\if :v2_drop_block_before_blocks_ok
\if :v2_drop_block_before_settings_ok
\if :v2_drop_block_column_absent_ok
\if :v2_drop_block_meta_ok
\if :v2_drop_block_rows_ok
\if :v2_drop_block_settings_ok
\echo [PASS] (:testid) V2 dropColumn cleaned block metadata and settings
\else
\echo [FAIL] (:testid) V2 dropColumn left block column settings
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 dropColumn left block table rows
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 dropColumn left block metadata rows
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V2 dropColumn did not drop block column
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Test setup did not persist block column settings
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Test setup did not create block rows
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- V1: setFilter and setColumn operations
-- ============================================================================

CREATE TABLE filtered_tasks (
  id UUID PRIMARY KEY,
  owner TEXT NOT NULL DEFAULT '',
  title TEXT NOT NULL DEFAULT ''
);
SELECT cloudsync_init('filtered_tasks', 'CLS', 0) AS _init_filtered \gset

SELECT cloudsync_migration_apply($json$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 1,
  "migrationId": "mig-pg-v1-filter-column",
  "requiredCapabilities": ["schema:write"],
  "ops": [
    {"op": "setColumn", "table": "filtered_tasks", "column": "title", "key": "label", "value": "sync-title"},
    {"op": "setFilter", "table": "filtered_tasks", "filter": "owner = 'alice'"}
  ]
}
$json$) AS migration_v1_filter_column \gset

INSERT INTO filtered_tasks VALUES ('aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa', 'alice', 'visible');
INSERT INTO filtered_tasks VALUES ('bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb', 'bob', 'hidden');

SELECT count(*) = 1 AS v1_set_column_ok
FROM cloudsync_table_settings
WHERE tbl_name = 'filtered_tasks' AND col_name = 'title' AND key = 'label' AND value = 'sync-title' \gset

SELECT count(*) = 1 AS v1_set_filter_ok
FROM cloudsync_table_settings
WHERE tbl_name = 'filtered_tasks' AND col_name = '*' AND key = 'filter' AND value = 'owner = ''alice''' \gset

SELECT count(*) > 0 AS v1_filter_alice_ok
FROM filtered_tasks_cloudsync
WHERE pk = cloudsync_pk_encode('aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa'::uuid) \gset

SELECT count(*) = 0 AS v1_filter_bob_ok
FROM filtered_tasks_cloudsync
WHERE pk = cloudsync_pk_encode('bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb'::uuid) \gset

\if :v1_set_column_ok
\if :v1_set_filter_ok
\if :v1_filter_alice_ok
\if :v1_filter_bob_ok
\echo [PASS] (:testid) V1 setFilter and setColumn migration applied settings and triggers
\else
\echo [FAIL] (:testid) V1 setFilter tracked non-matching row
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V1 setFilter did not track matching row
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V1 setFilter setting missing
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) V1 setColumn setting missing
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Declarative alter rejects explicit NULL defaults for NOT NULL columns
-- ============================================================================

DO $$
DECLARE
  rejected BOOLEAN := false;
BEGIN
  BEGIN
    PERFORM cloudsync_alter_add_column('filtered_tasks', 'required_null', 'text', false, NULL);
    PERFORM cloudsync_alter_apply();
  EXCEPTION WHEN OTHERS THEN
    rejected := true;
    PERFORM cloudsync_alter_clear('filtered_tasks');
  END;

  INSERT INTO schema_migration_error_flags VALUES ('alter_required_null_rejected', rejected);
END
$$;

DO $$
DECLARE
  rejected BOOLEAN := false;
BEGIN
  BEGIN
    PERFORM cloudsync_alter_add_column('filtered_tasks', 'required_null_override', 'text', false, '');
    PERFORM cloudsync_alter_add_column_postgresql('filtered_tasks', 'required_null_override', 'TEXT', false, NULL);
    PERFORM cloudsync_alter_apply();
  EXCEPTION WHEN OTHERS THEN
    rejected := true;
    PERFORM cloudsync_alter_clear('filtered_tasks');
  END;

  INSERT INTO schema_migration_error_flags VALUES ('alter_required_null_override_rejected', rejected);
END
$$;

SELECT ok AS alter_required_null_rejected_ok
FROM schema_migration_error_flags
WHERE key = 'alter_required_null_rejected' \gset

SELECT ok AS alter_required_null_override_rejected_ok
FROM schema_migration_error_flags
WHERE key = 'alter_required_null_override_rejected' \gset

SELECT count(*) = 0 AS alter_required_null_absent_ok
FROM information_schema.columns
WHERE table_name = 'filtered_tasks' AND column_name IN ('required_null', 'required_null_override') \gset

\if :alter_required_null_rejected_ok
\if :alter_required_null_override_rejected_ok
\if :alter_required_null_absent_ok
\echo [PASS] (:testid) Declarative alter rejected NOT NULL column with NULL default
\else
\echo [FAIL] (:testid) Declarative alter NULL default changed schema
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Declarative alter accepted dialect override with NULL default
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Declarative alter accepted NOT NULL column with NULL default
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Declarative alter accepts non-text default arguments
-- ============================================================================

CREATE TABLE pg_default_tasks (
  id UUID PRIMARY KEY
);
SELECT cloudsync_init('pg_default_tasks', 'CLS', 0) AS _init_pg_default_tasks \gset

SELECT cloudsync_alter_add_column('pg_default_tasks', 'rank', 'integer', false, 0) AS alter_default_rank_ok \gset
SELECT cloudsync_alter_add_column('pg_default_tasks', 'done', 'boolean', false, false) AS alter_default_done_ok \gset
SELECT cloudsync_alter_add_column('pg_default_tasks', 'metadata', 'json', false, '{}'::jsonb) AS alter_default_metadata_ok \gset
SELECT cloudsync_alter_apply() IS NOT NULL AS alter_non_text_defaults_apply_ok \gset

INSERT INTO pg_default_tasks (id)
VALUES ('cccccccc-cccc-cccc-cccc-cccccccccccc');

SELECT count(*) = 3 AS alter_non_text_defaults_cols_ok
FROM information_schema.columns
WHERE table_name = 'pg_default_tasks'
  AND column_name IN ('rank', 'done', 'metadata')
  AND is_nullable = 'NO' \gset

SELECT rank = 0 AND done = false AND metadata = '{}'::jsonb AS alter_non_text_defaults_values_ok
FROM pg_default_tasks
WHERE id = 'cccccccc-cccc-cccc-cccc-cccccccccccc' \gset

\if :alter_default_rank_ok
\if :alter_default_done_ok
\if :alter_default_metadata_ok
\if :alter_non_text_defaults_apply_ok
\if :alter_non_text_defaults_cols_ok
\if :alter_non_text_defaults_values_ok
\echo [PASS] (:testid) Declarative alter accepted non-text default arguments
\else
\echo [FAIL] (:testid) Declarative alter non-text defaults did not populate inserted row
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Declarative alter non-text defaults did not create NOT NULL columns
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Declarative alter non-text defaults apply failed
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Declarative alter JSON default argument failed
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Declarative alter boolean default argument failed
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Declarative alter integer default argument failed
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Hash guards reject and roll back migrations
-- ============================================================================

DO $$
DECLARE
  rejected BOOLEAN := false;
BEGIN
  BEGIN
    PERFORM cloudsync_migration_apply($payload$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 1,
  "migrationId": "mig-pg-bad-base",
  "baseSchemaHash": "1",
  "ops": [
    {"op": "addColumn", "table": "filtered_tasks", "column": {"name": "base_fail", "type": "text", "nullable": true}}
  ]
}
$payload$);
  EXCEPTION WHEN OTHERS THEN
    rejected := true;
  END;
  INSERT INTO schema_migration_error_flags VALUES ('bad_base_rejected', rejected);
END
$$;

DO $$
DECLARE
  rejected BOOLEAN := false;
BEGIN
  BEGIN
    PERFORM cloudsync_migration_apply($payload$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 1,
  "migrationId": "mig-pg-bad-target",
  "targetSchemaHash": "1",
  "ops": [
    {"op": "addColumn", "table": "filtered_tasks", "column": {"name": "target_fail", "type": "text", "nullable": true}}
  ]
}
$payload$);
  EXCEPTION WHEN OTHERS THEN
    rejected := true;
  END;
  INSERT INTO schema_migration_error_flags VALUES ('bad_target_rejected', rejected);
END
$$;

SELECT ok AS hash_bad_base_rejected_ok
FROM schema_migration_error_flags
WHERE key = 'bad_base_rejected' \gset

SELECT ok AS hash_bad_target_rejected_ok
FROM schema_migration_error_flags
WHERE key = 'bad_target_rejected' \gset

SELECT count(*) = 0 AS hash_guard_columns_absent_ok
FROM information_schema.columns
WHERE table_name = 'filtered_tasks' AND column_name IN ('base_fail', 'target_fail') \gset

SELECT count(*) = 0 AS hash_guard_unrecorded_ok
FROM cloudsync_migrations
WHERE migration_id IN ('mig-pg-bad-base', 'mig-pg-bad-target') \gset

\if :hash_bad_base_rejected_ok
\if :hash_bad_target_rejected_ok
\if :hash_guard_columns_absent_ok
\if :hash_guard_unrecorded_ok
\echo [PASS] (:testid) Migration hash guards rejected and rolled back invalid payloads
\else
\echo [FAIL] (:testid) Hash guard failure was recorded
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Hash guard failure altered schema
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) targetSchemaHash mismatch was accepted
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) baseSchemaHash mismatch was accepted
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- rawSql dialect validation
-- ============================================================================

SELECT cloudsync_migration_apply($json$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 1,
  "migrationId": "mig-pg-raw-success",
  "requiredCapabilities": ["schema:write"],
  "ops": [
    {"op": "rawSql", "sql": {"postgresql": ["CREATE TABLE raw_ok (id UUID PRIMARY KEY)"]}}
  ]
}
$json$) AS migration_raw_success \gset

DO $$
DECLARE
  rejected BOOLEAN := false;
BEGIN
  BEGIN
    PERFORM cloudsync_migration_apply($payload$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 1,
  "migrationId": "mig-pg-raw-missing",
  "ops": [
    {"op": "rawSql", "sql": {"sqlite": ["CREATE TABLE raw_missing (id TEXT PRIMARY KEY NOT NULL)"]}}
  ]
}
$payload$);
  EXCEPTION WHEN OTHERS THEN
    rejected := true;
  END;
  INSERT INTO schema_migration_error_flags VALUES ('raw_missing_rejected', rejected);
END
$$;

DO $$
DECLARE
  rejected BOOLEAN := false;
BEGIN
  BEGIN
    PERFORM cloudsync_migration_apply($payload$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 1,
  "migrationId": "mig-pg-raw-bad-item",
  "ops": [
    {"op": "rawSql", "sql": {"postgresql": [123]}}
  ]
}
$payload$);
  EXCEPTION WHEN OTHERS THEN
    rejected := true;
  END;
  INSERT INTO schema_migration_error_flags VALUES ('raw_bad_item_rejected', rejected);
END
$$;

DO $$
DECLARE
  rejected BOOLEAN := false;
BEGIN
  BEGIN
    PERFORM cloudsync_migration_apply($payload$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 1,
  "migrationId": "mig-pg-raw-tx",
  "ops": [
    {"op": "rawSql", "sql": {"postgresql": ["COMMIT"]}}
  ]
}
$payload$);
  EXCEPTION WHEN OTHERS THEN
    rejected := true;
  END;
  INSERT INTO schema_migration_error_flags VALUES ('raw_tx_rejected', rejected);
END
$$;

SELECT count(*) = 1 AS raw_success_ok
FROM information_schema.tables
WHERE table_name = 'raw_ok' \gset

SELECT ok AS raw_missing_rejected_ok
FROM schema_migration_error_flags
WHERE key = 'raw_missing_rejected' \gset

SELECT ok AS raw_bad_item_rejected_ok
FROM schema_migration_error_flags
WHERE key = 'raw_bad_item_rejected' \gset

SELECT ok AS raw_tx_rejected_ok
FROM schema_migration_error_flags
WHERE key = 'raw_tx_rejected' \gset

SELECT count(*) = 0 AS raw_missing_absent_ok
FROM information_schema.tables
WHERE table_name = 'raw_missing' \gset

SELECT count(*) = 0 AS raw_bad_unrecorded_ok
FROM cloudsync_migrations
WHERE migration_id IN ('mig-pg-raw-missing', 'mig-pg-raw-bad-item', 'mig-pg-raw-tx') \gset

\if :raw_success_ok
\if :raw_missing_rejected_ok
\if :raw_bad_item_rejected_ok
\if :raw_tx_rejected_ok
\if :raw_missing_absent_ok
\if :raw_bad_unrecorded_ok
\echo [PASS] (:testid) rawSql migration selected dialect and rejected malformed variants
\else
\echo [FAIL] (:testid) rawSql malformed migration was recorded
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) rawSql missing dialect altered schema
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) rawSql transaction control was accepted
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) rawSql non-string array item was accepted
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) rawSql missing dialect was accepted
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) rawSql dialect success branch did not run
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Declarative raw SQL builder API and guardrails.
DO $$
DECLARE
  rejected BOOLEAN := false;
BEGIN
  BEGIN
    PERFORM cloudsync_alter_sql('SAVEPOINT bad_raw_alter');
  EXCEPTION WHEN OTHERS THEN
    rejected := true;
  END;
  INSERT INTO schema_migration_error_flags VALUES ('alter_raw_guard_rejected', rejected);
END
$$;

DO $$
DECLARE
  end_rejected BOOLEAN := false;
  abort_rejected BOOLEAN := false;
BEGIN
  BEGIN
    PERFORM cloudsync_alter_sql('END');
  EXCEPTION WHEN OTHERS THEN
    end_rejected := true;
  END;

  BEGIN
    PERFORM cloudsync_alter_sql('ABORT');
  EXCEPTION WHEN OTHERS THEN
    abort_rejected := true;
  END;

  INSERT INTO schema_migration_error_flags VALUES ('alter_raw_end_guard_rejected', end_rejected);
  INSERT INTO schema_migration_error_flags VALUES ('alter_raw_abort_guard_rejected', abort_rejected);
END
$$;

SELECT cloudsync_alter_create_table('raw_alter_pg') AS alter_raw_create_ok \gset
SELECT cloudsync_alter_add_column('raw_alter_pg', 'id', 'uuid', false) AS alter_raw_id_ok \gset
SELECT cloudsync_alter_add_primary_key('raw_alter_pg', 'id') AS alter_raw_pk_ok \gset
SELECT cloudsync_alter_add_column('raw_alter_pg', 'title', 'text', false, '') AS alter_raw_title_ok \gset
SELECT cloudsync_alter_sql('CREATE INDEX raw_alter_pg_title_common_idx ON raw_alter_pg(title)') AS alter_raw_common_ok \gset
SELECT cloudsync_alter_sqlite('CREATE INDEX raw_alter_pg_title_sqlite_idx ON raw_alter_pg(title)') AS alter_raw_sqlite_ok \gset
SELECT cloudsync_alter_postgresql('CREATE INDEX raw_alter_pg_title_pg_idx ON raw_alter_pg(title)') AS alter_raw_pg_ok \gset
SELECT position('"skipMissingDialect":true' in cloudsync_alter_preview()) > 0 AS alter_raw_preview_skip_ok \gset
SELECT cloudsync_alter_apply() IS NOT NULL AS alter_raw_apply_ok \gset

SELECT ok AS alter_raw_guard_ok
FROM schema_migration_error_flags
WHERE key = 'alter_raw_guard_rejected' \gset

SELECT ok AS alter_raw_end_guard_ok
FROM schema_migration_error_flags
WHERE key = 'alter_raw_end_guard_rejected' \gset

SELECT ok AS alter_raw_abort_guard_ok
FROM schema_migration_error_flags
WHERE key = 'alter_raw_abort_guard_rejected' \gset

SELECT count(*) = 1 AS alter_raw_table_ok
FROM information_schema.tables
WHERE table_name = 'raw_alter_pg' \gset

SELECT count(*) = 1 AS alter_raw_common_idx_ok
FROM pg_indexes
WHERE indexname = 'raw_alter_pg_title_common_idx' \gset

SELECT count(*) = 1 AS alter_raw_pg_idx_ok
FROM pg_indexes
WHERE indexname = 'raw_alter_pg_title_pg_idx' \gset

SELECT count(*) = 0 AS alter_raw_sqlite_idx_skipped_ok
FROM pg_indexes
WHERE indexname = 'raw_alter_pg_title_sqlite_idx' \gset

\if :alter_raw_guard_ok
\if :alter_raw_end_guard_ok
\if :alter_raw_abort_guard_ok
\if :alter_raw_preview_skip_ok
\if :alter_raw_apply_ok
\if :alter_raw_table_ok
\if :alter_raw_common_idx_ok
\if :alter_raw_pg_idx_ok
\if :alter_raw_sqlite_idx_skipped_ok
\echo [PASS] (:testid) Declarative raw SQL builder applied PostgreSQL SQL and skipped SQLite SQL
\else
\echo [FAIL] (:testid) SQLite-only raw SQL ran on PostgreSQL
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) PostgreSQL raw SQL index was not created
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Common raw SQL index was not created
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Declarative raw SQL createTable did not create table
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Declarative raw SQL apply failed
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Declarative raw SQL preview did not mark dialect skip
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Declarative raw SQL accepted ABORT transaction control
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Declarative raw SQL accepted END transaction control
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) Declarative raw SQL accepted transaction control
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- createTable conflict and declarative alter atomicity
-- ============================================================================

CREATE TABLE existing_create_conflict (
  id UUID PRIMARY KEY,
  title TEXT NOT NULL DEFAULT ''
);

DO $$
DECLARE
  rejected BOOLEAN := false;
BEGIN
  BEGIN
    PERFORM cloudsync_migration_apply($payload$
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 1,
  "migrationId": "mig-pg-existing-create",
  "ops": [
    {
      "op": "createTable",
      "table": "existing_create_conflict",
      "columns": [
        {"name": "id", "type": "uuid", "primaryKey": true, "nullable": false},
        {"name": "body", "type": "text", "nullable": true}
      ]
    }
  ]
}
$payload$);
  EXCEPTION WHEN OTHERS THEN
    rejected := true;
  END;
  INSERT INTO schema_migration_error_flags VALUES ('existing_create_rejected', rejected);
END
$$;

SELECT ok AS existing_create_rejected_ok
FROM schema_migration_error_flags
WHERE key = 'existing_create_rejected' \gset

SELECT count(*) = 0 AS existing_create_body_absent_ok
FROM information_schema.columns
WHERE table_name = 'existing_create_conflict' AND column_name = 'body' \gset

SELECT count(*) = 0 AS existing_create_unrecorded_ok
FROM cloudsync_migrations
WHERE migration_id = 'mig-pg-existing-create' \gset

-- ============================================================================
-- Initial schema sync generated from declarative API
-- ============================================================================

SELECT cloudsync_alter_create_table('initial_pg_notes');
SELECT cloudsync_alter_add_column('initial_pg_notes', 'id', 'uuid', false);
SELECT cloudsync_alter_add_primary_key('initial_pg_notes', 'id');
SELECT cloudsync_alter_add_column('initial_pg_notes', 'body', 'text', false, '');
SELECT cloudsync_alter_augment_table('initial_pg_notes', 'CLS', 0);
SELECT cloudsync_alter_set_block_lww('initial_pg_notes', 'body', E'\n');
SELECT cloudsync_alter_apply() IS NOT NULL AS initial_pg_apply_ok \gset

SELECT count(*) = 1 AS initial_pg_pending_ok
FROM cloudsync_pending_migration
WHERE uploaded_at IS NULL
  AND payload::jsonb->>'type' = 'cloudsync.schema.migration'
  AND payload::jsonb->'ops' @> '[{"op":"createTable","table":"initial_pg_notes"}]'::jsonb \gset

SELECT count(*) = 1 AS initial_pg_recorded_ok
FROM cloudsync_migrations
WHERE migration_id = (
  SELECT migration_id
  FROM cloudsync_pending_migration
  WHERE uploaded_at IS NULL AND payload::jsonb->'ops' @> '[{"op":"createTable","table":"initial_pg_notes"}]'::jsonb
  LIMIT 1
) \gset

SELECT count(*) = 1 AS initial_pg_block_ok
FROM cloudsync_table_settings
WHERE tbl_name = 'initial_pg_notes' AND col_name = 'body' AND key = 'algo' AND value = 'block' \gset

SELECT count(*) AS migrations_before_atomic
FROM cloudsync_migrations \gset

DROP TABLE IF EXISTS cloudsync_pending_migration;
CREATE TABLE cloudsync_pending_migration (x TEXT);

DO $$
DECLARE
  rejected BOOLEAN := false;
BEGIN
  BEGIN
    PERFORM cloudsync_alter_create_table('atomic_pg_notes');
    PERFORM cloudsync_alter_add_column('atomic_pg_notes', 'id', 'uuid', false);
    PERFORM cloudsync_alter_add_primary_key('atomic_pg_notes', 'id');
    PERFORM cloudsync_alter_apply();
  EXCEPTION WHEN OTHERS THEN
    rejected := true;
  END;
  INSERT INTO schema_migration_error_flags VALUES ('alter_atomic_rejected', rejected);
END
$$;

SELECT ok AS alter_atomic_rejected_ok
FROM schema_migration_error_flags
WHERE key = 'alter_atomic_rejected' \gset

SELECT count(*) = 0 AS alter_atomic_table_absent_ok
FROM information_schema.tables
WHERE table_name = 'atomic_pg_notes' \gset

SELECT count(*) = 1 AS alter_atomic_pending_shape_ok
FROM information_schema.columns
WHERE table_name = 'cloudsync_pending_migration' AND column_name = 'x' \gset

SELECT count(*) = :migrations_before_atomic AS alter_atomic_records_ok
FROM cloudsync_migrations \gset

\if :existing_create_rejected_ok
\if :existing_create_body_absent_ok
\if :existing_create_unrecorded_ok
\echo [PASS] (:testid) createTable rejected an existing conflicting table
\else
\echo [FAIL] (:testid) createTable conflict was recorded
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) createTable conflict changed the existing table
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) createTable accepted an existing conflicting table
SELECT (:fail::int + 1) AS fail \gset
\endif

\if :initial_pg_apply_ok
\if :initial_pg_pending_ok
\if :initial_pg_recorded_ok
\if :initial_pg_block_ok
\echo [PASS] (:testid) declarative initial schema sync generated pending PostgreSQL migration
\else
\echo [FAIL] (:testid) initial PostgreSQL schema sync missed block LWW settings
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) initial PostgreSQL schema sync was not recorded locally
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) initial PostgreSQL schema sync did not create pending upload
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) initial PostgreSQL schema sync apply failed
SELECT (:fail::int + 1) AS fail \gset
\endif

\if :alter_atomic_rejected_ok
\if :alter_atomic_table_absent_ok
\if :alter_atomic_pending_shape_ok
\if :alter_atomic_records_ok
\echo [PASS] (:testid) declarative alter rollback kept local schema and pending save atomic
\else
\echo [FAIL] (:testid) declarative alter atomic rollback recorded a migration
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) declarative alter atomic rollback recreated pending table
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) declarative alter atomic rollback left a local table
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) declarative alter accepted malformed pending migration table
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Cleanup
-- ============================================================================

\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_52;
\endif
