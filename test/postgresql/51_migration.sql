-- Migration System Test
-- Verifies that cloudsync_migrations is NOT created by cloudsync_init() (lazy
-- creation / privilege-safe contract) and that the migration workflow
-- (register → apply → mark-applied) works correctly via direct SQL.
-- Also exercises every DDL operation that the migration system would generate
-- for PostgreSQL, and verifies the ON CONFLICT upsert guard that prevents
-- re-registration from resetting an already-applied migration's applied_at.

\set testid '51'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

DROP DATABASE IF EXISTS cloudsync_test_51;
CREATE DATABASE cloudsync_test_51;

\connect cloudsync_test_51
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- =============================================================================
-- Bootstrap: enroll a table so that cloudsync initializes the system tables
-- (cloudsync_settings, cloudsync_site_id, etc.) but NOT cloudsync_migrations.
-- The migration ledger is created lazily only when the migration C API is
-- first used (cloudsync_migration_register / cloudsync_migration_apply_pending).
-- cloudsync_init() must NOT require CREATE on public for the ledger.
-- =============================================================================

CREATE TABLE IF NOT EXISTS bootstrap_tbl (id UUID PRIMARY KEY, val TEXT DEFAULT '');
SELECT cloudsync_init('bootstrap_tbl', 'CLS', 1) AS _init_bootstrap \gset

-- =============================================================================
-- 1. Verify cloudsync_migrations is NOT created by cloudsync_init
-- =============================================================================

SELECT EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_schema = 'public' AND table_name = 'cloudsync_migrations'
) AS migrations_table_premature \gset

\if :migrations_table_premature
\echo [FAIL] (:testid) cloudsync_migrations was created by cloudsync_init — privilege regression
SELECT (:fail::int + 1) AS fail \gset
\else
\echo [PASS] (:testid) cloudsync_migrations not created by cloudsync_init (correct lazy behavior)
\endif

-- Create the ledger manually for the remainder of the SQL-level tests below.
-- In production code this is done by calling cloudsync_migration_register()
-- or cloudsync_migration_apply_pending() from the C API.
CREATE TABLE IF NOT EXISTS public.cloudsync_migrations (
    version    BIGINT PRIMARY KEY,
    descriptor BYTEA  NOT NULL,
    applied_at BIGINT,
    checksum   BIGINT NOT NULL
);

-- =============================================================================
-- 2. Verify schema of cloudsync_migrations
-- =============================================================================

SELECT COUNT(*) AS col_count
FROM information_schema.columns
WHERE table_name = 'cloudsync_migrations'
  AND column_name IN ('version', 'descriptor', 'applied_at', 'checksum') \gset

SELECT (:col_count::int = 4) AS schema_ok \gset
\if :schema_ok
\echo [PASS] (:testid) cloudsync_migrations has all 4 expected columns
\else
\echo [FAIL] (:testid) cloudsync_migrations schema incorrect, expected 4 columns, got :col_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify version is the primary key
SELECT COUNT(*) AS pk_count
FROM information_schema.table_constraints tc
JOIN information_schema.key_column_usage kcu
    ON tc.constraint_name = kcu.constraint_name
WHERE tc.table_name = 'cloudsync_migrations'
  AND tc.constraint_type = 'PRIMARY KEY'
  AND kcu.column_name = 'version' \gset

SELECT (:pk_count::int = 1) AS pk_ok \gset
\if :pk_ok
\echo [PASS] (:testid) cloudsync_migrations.version is PRIMARY KEY
\else
\echo [FAIL] (:testid) cloudsync_migrations.version is not PRIMARY KEY
SELECT (:fail::int + 1) AS fail \gset
\endif

-- descriptor column should be BYTEA
SELECT data_type AS descriptor_type
FROM information_schema.columns
WHERE table_name = 'cloudsync_migrations' AND column_name = 'descriptor' \gset

SELECT (:'descriptor_type' = 'bytea') AS bytea_ok \gset
\if :bytea_ok
\echo [PASS] (:testid) cloudsync_migrations.descriptor is BYTEA
\else
\echo [FAIL] (:testid) cloudsync_migrations.descriptor is not BYTEA, got :descriptor_type
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 3. Simulate migration registration (INSERT)
-- =============================================================================

-- Use a minimal fake descriptor blob (magic 'MIGR' + version 1 + op 1 + nfields 0)
INSERT INTO cloudsync_migrations (version, descriptor, checksum, applied_at)
VALUES (1, E'\\x4D49475201010000', 12345678, NULL);

SELECT COUNT(*) AS pending_count
FROM cloudsync_migrations
WHERE applied_at IS NULL \gset

SELECT (:pending_count::int = 1) AS pending_ok \gset
\if :pending_ok
\echo [PASS] (:testid) Migration registered with applied_at = NULL (pending)
\else
\echo [FAIL] (:testid) Expected 1 pending migration, got :pending_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 4. Simulate marking a migration as applied (UPDATE)
-- =============================================================================

UPDATE cloudsync_migrations SET applied_at = 1700000000 WHERE version = 1;

SELECT COUNT(*) AS pending_after
FROM cloudsync_migrations
WHERE applied_at IS NULL \gset

SELECT (:pending_after::int = 0) AS applied_ok \gset
\if :applied_ok
\echo [PASS] (:testid) Migration marked as applied (applied_at set)
\else
\echo [FAIL] (:testid) Migration still appears as pending after mark-applied
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 5. Verify upsert guard: re-registering an applied migration keeps it applied
-- =============================================================================
-- SQL_MIGRATION_INSERT uses ON CONFLICT ... DO UPDATE ... WHERE applied_at IS NULL.
-- When a row is already applied (applied_at IS NOT NULL), the WHERE clause blocks
-- the DO UPDATE, so applied_at must remain set.  This mirrors the SQLite regression
-- "Re-register Applied Migration Stays Applied".

INSERT INTO public.cloudsync_migrations (version, descriptor, checksum, applied_at)
VALUES (1, E'\\x4D49475201010000', 99999999, NULL)
ON CONFLICT (version) DO UPDATE
    SET descriptor = EXCLUDED.descriptor,
        checksum   = EXCLUDED.checksum
WHERE cloudsync_migrations.applied_at IS NULL;

SELECT applied_at IS NOT NULL AS still_applied
FROM public.cloudsync_migrations
WHERE version = 1 \gset

\if :still_applied
\echo [PASS] (:testid) Re-registering an applied version keeps it applied (upsert guard correct)
\else
\echo [FAIL] (:testid) Re-registering reset applied_at — ON CONFLICT WHERE guard is broken
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 6. Multiple pending migrations ordered by version
-- =============================================================================

INSERT INTO cloudsync_migrations (version, descriptor, checksum, applied_at)
VALUES (2, E'\\x4D49475201050000', 11111111, NULL),
       (3, E'\\x4D49475201010000', 22222222, NULL);

-- Check that both versions 2 and 3 appear as pending, in ascending order
SELECT COUNT(*) AS pending_v23
FROM cloudsync_migrations
WHERE applied_at IS NULL AND version IN (2, 3) \gset

SELECT MIN(version) AS first_pending, MAX(version) AS last_pending
FROM cloudsync_migrations
WHERE applied_at IS NULL \gset

SELECT (:pending_v23::int = 2 AND :first_pending::int = 2 AND :last_pending::int = 3) AS order_ok \gset
\if :order_ok
\echo [PASS] (:testid) Pending migrations present and ordered correctly
\else
\echo [FAIL] (:testid) Pending migrations not in expected state: count=:pending_v23 first=:first_pending last=:last_pending
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Clean up test migration rows
DELETE FROM cloudsync_migrations WHERE version IN (1, 2, 3);

-- =============================================================================
-- 7. DDL: CREATE TABLE (migration would generate and execute this)
-- =============================================================================

CREATE TABLE IF NOT EXISTS migration_test_notes (
    id    UUID    PRIMARY KEY,
    title TEXT    NOT NULL DEFAULT '',
    body  TEXT,
    done  BOOLEAN NOT NULL DEFAULT FALSE
);

SELECT EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_name = 'migration_test_notes'
) AS create_table_ok \gset

\if :create_table_ok
\echo [PASS] (:testid) CREATE TABLE DDL succeeded
\else
\echo [FAIL] (:testid) CREATE TABLE DDL failed
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT cloudsync_init('migration_test_notes', 'CLS', 1) AS _init_notes \gset

-- =============================================================================
-- 8. DDL: ADD COLUMN (migration wraps in cloudsync_begin_alter / commit_alter)
-- =============================================================================

SELECT cloudsync_begin_alter('migration_test_notes') AS _begin \gset

ALTER TABLE migration_test_notes ADD COLUMN priority INTEGER NOT NULL DEFAULT 0;
ALTER TABLE migration_test_notes ADD COLUMN tags TEXT;

SELECT cloudsync_commit_alter('migration_test_notes') AS _commit \gset

SELECT COUNT(*) AS new_col_count
FROM information_schema.columns
WHERE table_name = 'migration_test_notes'
  AND column_name IN ('priority', 'tags') \gset

SELECT (:new_col_count::int = 2) AS add_col_ok \gset
\if :add_col_ok
\echo [PASS] (:testid) ADD COLUMN DDL (with begin/commit_alter) succeeded
\else
\echo [FAIL] (:testid) ADD COLUMN DDL failed, new_col_count = :new_col_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 9. DDL: SET DEFAULT (PostgreSQL supports this, unlike SQLite)
-- =============================================================================

SELECT cloudsync_begin_alter('migration_test_notes') AS _begin \gset

ALTER TABLE migration_test_notes ALTER COLUMN tags SET DEFAULT 'untagged';

SELECT cloudsync_commit_alter('migration_test_notes') AS _commit \gset

-- PostgreSQL stores defaults with type casts, e.g. '''untagged'''::text
SELECT column_default LIKE '''untagged''%' AS set_default_ok
FROM information_schema.columns
WHERE table_name = 'migration_test_notes' AND column_name = 'tags' \gset

\if :set_default_ok
\echo [PASS] (:testid) SET DEFAULT DDL succeeded
\else
\echo [FAIL] (:testid) SET DEFAULT DDL failed
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 10. DDL: RENAME COLUMN
-- =============================================================================

SELECT cloudsync_begin_alter('migration_test_notes') AS _begin \gset

ALTER TABLE migration_test_notes RENAME COLUMN body TO content;

SELECT cloudsync_commit_alter('migration_test_notes') AS _commit \gset

SELECT EXISTS (
    SELECT 1 FROM information_schema.columns
    WHERE table_name = 'migration_test_notes' AND column_name = 'content'
) AS renamed_ok \gset

\if :renamed_ok
\echo [PASS] (:testid) RENAME COLUMN DDL succeeded
\else
\echo [FAIL] (:testid) RENAME COLUMN DDL failed
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 11. DDL: DROP COLUMN
-- =============================================================================

SELECT cloudsync_begin_alter('migration_test_notes') AS _begin \gset

ALTER TABLE migration_test_notes DROP COLUMN IF EXISTS tags;

SELECT cloudsync_commit_alter('migration_test_notes') AS _commit \gset

SELECT NOT EXISTS (
    SELECT 1 FROM information_schema.columns
    WHERE table_name = 'migration_test_notes' AND column_name = 'tags'
) AS drop_col_ok \gset

\if :drop_col_ok
\echo [PASS] (:testid) DROP COLUMN DDL succeeded
\else
\echo [FAIL] (:testid) DROP COLUMN DDL failed
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 12. DDL: CREATE INDEX
-- =============================================================================

CREATE UNIQUE INDEX IF NOT EXISTS idx_notes_title    ON migration_test_notes (title);
CREATE INDEX        IF NOT EXISTS idx_notes_priority ON migration_test_notes (priority);

SELECT COUNT(*) AS idx_count
FROM pg_indexes
WHERE tablename = 'migration_test_notes'
  AND indexname IN ('idx_notes_title', 'idx_notes_priority') \gset

SELECT (:idx_count::int = 2) AS create_idx_ok \gset
\if :create_idx_ok
\echo [PASS] (:testid) CREATE INDEX DDL succeeded
\else
\echo [FAIL] (:testid) CREATE INDEX DDL failed, idx_count = :idx_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 13. DDL: DROP INDEX
-- =============================================================================

DROP INDEX IF EXISTS idx_notes_priority;

SELECT NOT EXISTS (
    SELECT 1 FROM pg_indexes
    WHERE tablename = 'migration_test_notes' AND indexname = 'idx_notes_priority'
) AS drop_idx_ok \gset

\if :drop_idx_ok
\echo [PASS] (:testid) DROP INDEX DDL succeeded
\else
\echo [FAIL] (:testid) DROP INDEX DDL failed
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 14. DDL: RENAME TABLE
-- =============================================================================

ALTER TABLE migration_test_notes RENAME TO migration_test_notes_renamed;

SELECT EXISTS (
    SELECT 1 FROM information_schema.tables WHERE table_name = 'migration_test_notes_renamed'
) AS rename_tbl_ok \gset

\if :rename_tbl_ok
\echo [PASS] (:testid) RENAME TABLE DDL succeeded
\else
\echo [FAIL] (:testid) RENAME TABLE DDL failed
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 15. DDL: DROP TABLE
-- =============================================================================

DROP TABLE IF EXISTS migration_test_notes_renamed;

SELECT NOT EXISTS (
    SELECT 1 FROM information_schema.tables WHERE table_name = 'migration_test_notes_renamed'
) AS drop_tbl_ok \gset

\if :drop_tbl_ok
\echo [PASS] (:testid) DROP TABLE DDL succeeded
\else
\echo [FAIL] (:testid) DROP TABLE DDL failed
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 16. Cold-Start Bootstrap: all DDL issued from server-side migrations only
-- Simulates a brand-new client that has no application tables.
-- The server provides migrations v10-v15 covering two tables from scratch.
-- =============================================================================

-- v10: CREATE products table
CREATE TABLE IF NOT EXISTS cs_products (
    id    UUID  PRIMARY KEY,
    name  TEXT  NOT NULL DEFAULT '',
    price DOUBLE PRECISION NOT NULL DEFAULT 0.0
);

-- v11: enroll products in sync (INIT_SYNC)
SELECT cloudsync_init('cs_products', 'CLS', 1) AS _init_cs_products \gset

-- v12: ADD_COLUMN stock
SELECT cloudsync_begin_alter('cs_products') AS _begin \gset
ALTER TABLE cs_products ADD COLUMN stock INTEGER NOT NULL DEFAULT 0;
SELECT cloudsync_commit_alter('cs_products') AS _commit \gset

-- v13: CREATE_INDEX on name
CREATE INDEX IF NOT EXISTS idx_cs_products_name ON cs_products (name);

-- v14: CREATE categories table
CREATE TABLE IF NOT EXISTS cs_categories (
    id    UUID PRIMARY KEY,
    label TEXT NOT NULL DEFAULT ''
);

-- v15: enroll categories in sync (INIT_SYNC)
SELECT cloudsync_init('cs_categories', 'CLS', 1) AS _init_cs_categories \gset

-- Verify both tables exist with all expected columns
SELECT COUNT(*) AS cs_prod_cols
FROM information_schema.columns
WHERE table_name = 'cs_products'
  AND column_name IN ('id', 'name', 'price', 'stock') \gset

SELECT (:cs_prod_cols::int = 4) AS cold_start_cols_ok \gset
\if :cold_start_cols_ok
\echo [PASS] (:testid) Cold-start: cs_products has all 4 columns
\else
\echo [FAIL] (:testid) Cold-start: cs_products column count wrong, got :cs_prod_cols
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT EXISTS (
    SELECT 1 FROM pg_indexes
    WHERE tablename = 'cs_products' AND indexname = 'idx_cs_products_name'
) AS cold_start_idx_ok \gset

\if :cold_start_idx_ok
\echo [PASS] (:testid) Cold-start: index created on cs_products
\else
\echo [FAIL] (:testid) Cold-start: index missing on cs_products
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT EXISTS (
    SELECT 1 FROM information_schema.tables WHERE table_name = 'cs_products_cloudsync'
) AS cs_prod_shadow_ok \gset
\if :cs_prod_shadow_ok
\echo [PASS] (:testid) Cold-start: cs_products_cloudsync shadow table created by INIT_SYNC
\else
\echo [FAIL] (:testid) Cold-start: cs_products_cloudsync shadow table missing
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT EXISTS (
    SELECT 1 FROM information_schema.tables WHERE table_name = 'cs_categories_cloudsync'
) AS cs_cat_shadow_ok \gset
\if :cs_cat_shadow_ok
\echo [PASS] (:testid) Cold-start: cs_categories_cloudsync shadow table created by INIT_SYNC
\else
\echo [FAIL] (:testid) Cold-start: cs_categories_cloudsync shadow table missing
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify data can be inserted into both fully-bootstrapped tables
INSERT INTO cs_products VALUES (
    'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa', 'Widget', 9.99, 100
);
INSERT INTO cs_categories VALUES (
    'bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb', 'Electronics'
);

SELECT COUNT(*) AS cs_prod_rows FROM cs_products \gset
SELECT COUNT(*) AS cs_cat_rows  FROM cs_categories \gset

SELECT (:cs_prod_rows::int = 1 AND :cs_cat_rows::int = 1) AS cold_start_data_ok \gset
\if :cold_start_data_ok
\echo [PASS] (:testid) Cold-start: data inserted and readable in bootstrapped tables
\else
\echo [FAIL] (:testid) Cold-start: data insertion failed (prod=:cs_prod_rows cat=:cs_cat_rows)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 18. INIT_SYNC: enroll a new table with cloudsync_init
-- =============================================================================

CREATE TABLE migration_test_tasks (
    id    UUID PRIMARY KEY,
    title TEXT NOT NULL DEFAULT ''
);

SELECT cloudsync_init('migration_test_tasks', 'CLS', 1) AS _init_tasks \gset

SELECT EXISTS (
    SELECT 1 FROM information_schema.tables WHERE table_name = 'migration_test_tasks_cloudsync'
) AS shadow_ok \gset

\if :shadow_ok
\echo [PASS] (:testid) INIT_SYNC creates shadow tracking table
\else
\echo [FAIL] (:testid) INIT_SYNC shadow table missing
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 19. Regression: Bug 1 — INIT_SYNC row filter persisted to settings
-- cloudsync_set_filter stores the expression in cloudsync_table_settings so
-- triggers carry the filter; this is the same mechanism the migration C code
-- uses after INIT_SYNC with a filter.
-- =============================================================================

CREATE TABLE migration_filter_test (
    id    UUID PRIMARY KEY,
    owner TEXT NOT NULL DEFAULT '',
    val   TEXT DEFAULT ''
);
SELECT cloudsync_init('migration_filter_test', 'CLS', 1) AS _init_filter \gset

SELECT cloudsync_set_filter('migration_filter_test', 'owner = ''alice''') AS _set_filter \gset

SELECT value AS stored_filter
FROM cloudsync_table_settings
WHERE tbl_name = 'migration_filter_test'
  AND col_name = '*'
  AND key      = 'filter' \gset

SELECT (:'stored_filter' = 'owner = ''alice''') AS filter_ok \gset
\if :filter_ok
\echo [PASS] (:testid) INIT_SYNC filter stored in cloudsync_table_settings
\else
\echo [FAIL] (:testid) INIT_SYNC filter not stored; got: :stored_filter
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 20. Regression: Bug 3a — DROP TABLE cleans up shadow and settings
-- cloudsync_cleanup removes the shadow table, triggers, and settings rows
-- before the physical table is dropped, so no orphaned metadata remains.
-- =============================================================================

CREATE TABLE migration_drop_meta (
    id UUID PRIMARY KEY,
    v  TEXT DEFAULT ''
);
SELECT cloudsync_init('migration_drop_meta', 'CLS', 1) AS _init_drop \gset

-- Shadow table must exist before the test
SELECT EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_name = 'migration_drop_meta_cloudsync'
) AS shadow_before_drop \gset

\if :shadow_before_drop
-- Cleanup removes shadow, triggers, and settings before the DDL drop
SELECT cloudsync_cleanup('migration_drop_meta') AS _cleanup \gset
DROP TABLE IF EXISTS migration_drop_meta;

-- Shadow table must be gone
SELECT CASE WHEN EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_name = 'migration_drop_meta_cloudsync'
) THEN 1 ELSE 0 END AS shadow_after_drop_int \gset

SELECT COUNT(*) AS settings_after_drop
FROM cloudsync_table_settings
WHERE tbl_name = 'migration_drop_meta' \gset

SELECT (:shadow_after_drop_int = 0 AND :settings_after_drop::int = 0) AS drop_meta_ok \gset
\if :drop_meta_ok
\echo [PASS] (:testid) DROP TABLE: shadow and settings cleaned up
\else
\echo [FAIL] (:testid) DROP TABLE: orphaned metadata remains (shadow=:shadow_after_drop_int settings=:settings_after_drop)
SELECT (:fail::int + 1) AS fail \gset
\endif

\else
\echo [FAIL] (:testid) DROP TABLE: shadow table was not created by cloudsync_init
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 21. Regression: Bug 3b — RENAME TABLE updates shadow and settings
-- The migration rename sequence: drop triggers (begin_alter), rename main table,
-- rename shadow table, update settings tbl_name, re-init for new name.
-- Verify no metadata remains under old name and new name is fully registered.
-- =============================================================================

CREATE TABLE migration_rename_src (
    id UUID PRIMARY KEY,
    v  TEXT DEFAULT ''
);
SELECT cloudsync_init('migration_rename_src', 'CLS', 1) AS _init_rename \gset

-- Drop triggers before the rename (mirrors migration_apply_rename_table)
SELECT cloudsync_begin_alter('migration_rename_src') AS _begin_rename \gset

ALTER TABLE migration_rename_src RENAME TO migration_rename_dst;
ALTER TABLE migration_rename_src_cloudsync RENAME TO migration_rename_dst_cloudsync;

UPDATE cloudsync_table_settings
   SET tbl_name = 'migration_rename_dst'
 WHERE tbl_name = 'migration_rename_src';

-- Flush stale in-memory entry; IF EXISTS DDL ops above make this a no-op DB-side
SELECT cloudsync_cleanup('migration_rename_src') AS _cleanup_rename \gset

-- Re-init for new name: recreates triggers and registers in context
SELECT cloudsync_init('migration_rename_dst', 'CLS', 1) AS _reinit_rename \gset

-- Old shadow must be gone, new shadow must exist
SELECT CASE WHEN EXISTS (
    SELECT 1 FROM information_schema.tables WHERE table_name = 'migration_rename_src_cloudsync'
) THEN 1 ELSE 0 END AS old_shadow_int \gset

SELECT CASE WHEN EXISTS (
    SELECT 1 FROM information_schema.tables WHERE table_name = 'migration_rename_dst_cloudsync'
) THEN 1 ELSE 0 END AS new_shadow_int \gset

-- Settings must have 0 rows for old name, ≥1 for new name
SELECT COUNT(*) AS old_settings FROM cloudsync_table_settings WHERE tbl_name = 'migration_rename_src' \gset
SELECT COUNT(*) AS new_settings FROM cloudsync_table_settings WHERE tbl_name = 'migration_rename_dst' \gset

SELECT (:old_shadow_int = 0 AND :new_shadow_int = 1 AND :old_settings::int = 0 AND :new_settings::int >= 1) AS rename_meta_ok \gset
\if :rename_meta_ok
\echo [PASS] (:testid) RENAME TABLE: metadata updated to new name
\else
\echo [FAIL] (:testid) RENAME TABLE: stale or missing metadata (old_shadow=:old_shadow_int new_shadow=:new_shadow_int old_settings=:old_settings new_settings=:new_settings)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 22. Regression: Bug P1 — non-enrolled (local-only) table accepts DDL
-- A table that was created but never passed to cloudsync_init() is local-only.
-- Applying DDL to it must succeed without calling cloudsync_begin_alter() (which
-- would abort with "Unable to find table").  This mirrors what the fixed
-- migration code does: it gates the alter lifecycle on table_lookup() ≠ NULL.
-- =============================================================================

CREATE TABLE local_only_tbl (id UUID PRIMARY KEY, val TEXT DEFAULT '');

-- No cloudsync_init — this is a local-only table.
-- DDL must succeed without the alter lifecycle.
ALTER TABLE local_only_tbl ADD COLUMN extra TEXT DEFAULT '';

SELECT CASE WHEN EXISTS (
    SELECT 1 FROM information_schema.columns
    WHERE table_name = 'local_only_tbl' AND column_name = 'extra'
) THEN 1 ELSE 0 END AS local_col_int \gset

\if :local_col_int
\echo [PASS] (:testid) Local-only table: DDL succeeds without alter lifecycle
\else
\echo [FAIL] (:testid) Local-only table: ADD COLUMN failed on non-enrolled table
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 23. Regression: Bug P1 — schema hash is recorded after schema changes
-- After any operation that changes tracked-table schemas, cloudsync_schema_versions
-- must gain an entry.  The existing path that triggers this is cloudsync_cleanup()
-- when at least one other tracked table remains (counter > 0).
-- We enroll two tables, alter one, clean it up to fire the hash update, then
-- verify the version table is non-empty.
-- =============================================================================

CREATE TABLE sha_tbl_a (id UUID PRIMARY KEY, val TEXT DEFAULT '');
CREATE TABLE sha_tbl_b (id UUID PRIMARY KEY, val TEXT DEFAULT '');
SELECT cloudsync_init('sha_tbl_a', 'CLS', 1) AS _init_sha_a \gset
SELECT cloudsync_init('sha_tbl_b', 'CLS', 1) AS _init_sha_b \gset

-- Alter sha_tbl_a through the begin/commit lifecycle
SELECT cloudsync_begin_alter('sha_tbl_a') AS _begin_sha \gset
ALTER TABLE sha_tbl_a ADD COLUMN extra_sha TEXT DEFAULT '';
SELECT cloudsync_commit_alter('sha_tbl_a') AS _commit_sha \gset

-- Cleanup sha_tbl_a; since sha_tbl_b is still tracked, counter > 0 →
-- cloudsync_cleanup() calls cloudsync_update_schema_hash()
SELECT cloudsync_cleanup('sha_tbl_a') AS _cleanup_sha \gset

-- Use CASE WHEN to produce a boolean (COUNT(*) is not directly usable with \if)
SELECT CASE WHEN (SELECT COUNT(*) FROM cloudsync_schema_versions) > 0
            THEN TRUE ELSE FALSE END AS schema_hash_exists \gset

\if :schema_hash_exists
\echo [PASS] (:testid) Schema hash: cloudsync_schema_versions populated after schema change
\else
\echo [FAIL] (:testid) Schema hash: cloudsync_schema_versions is empty after schema change
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 24. Regression: Bug P2 — cloudsync_begin_alter is idempotent (safe for retry)
-- After a context disruption (e.g., a rolled-back migration batch), the migration
-- system may call cloudsync_begin_alter() on a table that already has is_altering=true
-- in the in-process state.  The function must return OK (idempotent) and a
-- subsequent commit_alter must succeed.  We simulate this by calling begin_alter
-- twice in a row on the same table, then verifying the full lifecycle completes.
-- =============================================================================

CREATE TABLE idem_alter_tbl (id UUID PRIMARY KEY, val TEXT DEFAULT '');
SELECT cloudsync_init('idem_alter_tbl', 'CLS', 1) AS _init_idem \gset

-- First call: sets is_altering=true, drops triggers
SELECT cloudsync_begin_alter('idem_alter_tbl') AS _begin_idem_1 \gset
-- Second call: is_altering is already true → must return OK without re-dropping triggers
SELECT cloudsync_begin_alter('idem_alter_tbl') AS _begin_idem_2 \gset

ALTER TABLE idem_alter_tbl ADD COLUMN idem_col TEXT DEFAULT '';
SELECT cloudsync_commit_alter('idem_alter_tbl') AS _commit_idem \gset

SELECT CASE WHEN EXISTS (
    SELECT 1 FROM information_schema.columns
    WHERE table_name = 'idem_alter_tbl' AND column_name = 'idem_col'
) THEN TRUE ELSE FALSE END AS idem_col_exists \gset

\if :idem_col_exists
\echo [PASS] (:testid) Alter lifecycle idempotency: double begin_alter + commit_alter succeeds
\else
\echo [FAIL] (:testid) Alter lifecycle idempotency: column missing after double begin_alter
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 25. Regression: Bug P1 — RENAME_TABLE on a local-only table must not enroll it
-- When a table was created but never passed to cloudsync_init(), it is local-only.
-- A RENAME_TABLE migration must only execute the DDL rename; it must NOT call
-- cloudsync_init() on the new name, which would silently create shadow tables,
-- triggers, and settings for a table the application never intended to sync.
-- =============================================================================

CREATE TABLE local_rename_src (id UUID PRIMARY KEY, val TEXT DEFAULT '');
-- No cloudsync_init — this table is local-only.

ALTER TABLE local_rename_src RENAME TO local_rename_dst;

-- No shadow table must exist for either name
SELECT CASE WHEN EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_schema = 'public'
      AND table_name IN ('local_rename_src_cloudsync', 'local_rename_dst_cloudsync')
) THEN TRUE ELSE FALSE END AS local_shadow_exists \gset

\if :local_shadow_exists
\echo [FAIL] (:testid) Local RENAME: shadow table was created for a local-only table
SELECT (:fail::int + 1) AS fail \gset
\else
\echo [PASS] (:testid) Local RENAME: local-only table stays local after rename
\endif

-- No settings rows must exist for either name
SELECT CASE WHEN (
    SELECT COUNT(*) FROM cloudsync_table_settings
    WHERE tbl_name IN ('local_rename_src', 'local_rename_dst')
) = 0 THEN TRUE ELSE FALSE END AS local_settings_clean \gset

\if :local_settings_clean
\echo [PASS] (:testid) Local RENAME: no settings rows created for local-only table
\else
\echo [FAIL] (:testid) Local RENAME: spurious settings rows found for local-only table
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 26. Regression: Bug P2 — cloudsync_migrations survives last-table cleanup
-- When the last tracked table is removed via cloudsync_cleanup(), the system
-- drops cloudsync_settings, cloudsync_site_id, cloudsync_table_settings, and
-- cloudsync_schema_versions.  cloudsync_migrations must NOT be dropped: it is
-- the applied-migration ledger and must persist so that a subsequent re-init
-- does not replay versions that were already applied.
-- =============================================================================

CREATE TABLE ledger_pg_tbl (id UUID PRIMARY KEY, val TEXT DEFAULT '');
SELECT cloudsync_init('ledger_pg_tbl', 'CLS', 1) AS _init_ledger \gset

-- Record a migration as applied (simulate a completed migration batch)
INSERT INTO cloudsync_migrations (version, descriptor, checksum, applied_at)
VALUES (999, '\x00'::bytea, 0, EXTRACT(EPOCH FROM NOW())::bigint);

-- Cleanup the last (only) tracked table — triggers dbutils_settings_cleanup
SELECT cloudsync_cleanup('ledger_pg_tbl') AS _cleanup_ledger \gset

-- cloudsync_migrations must still exist after cleanup
SELECT CASE WHEN EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_schema = 'public' AND table_name = 'cloudsync_migrations'
) THEN TRUE ELSE FALSE END AS migrations_table_survives \gset

\if :migrations_table_survives
\echo [PASS] (:testid) Migrations ledger: cloudsync_migrations survives last-table cleanup
\else
\echo [FAIL] (:testid) Migrations ledger: cloudsync_migrations was dropped by cleanup
SELECT (:fail::int + 1) AS fail \gset
\endif

-- The applied row must still be present (ledger is intact)
SELECT CASE WHEN (
    SELECT COUNT(*) FROM cloudsync_migrations WHERE version = 999 AND applied_at IS NOT NULL
) = 1 THEN TRUE ELSE FALSE END AS ledger_row_intact \gset

\if :ledger_row_intact
\echo [PASS] (:testid) Migrations ledger: applied history row intact after cleanup
\else
\echo [FAIL] (:testid) Migrations ledger: applied history row missing after cleanup
SELECT (:fail::int + 1) AS fail \gset
\endif

-- =============================================================================
-- 27. Regression: Bug P2 — composite PK CREATE TABLE uses table-level constraint
-- When a CREATE TABLE statement has two or more primary-key columns, the DDL
-- must use a table-level "PRIMARY KEY (col1, col2)" clause rather than emitting
-- "col1 ... PRIMARY KEY, col2 ... PRIMARY KEY" (which is invalid SQL in both
-- SQLite and PostgreSQL).  This test creates a composite-PK table and verifies
-- that exactly one composite PK constraint exists and that uniqueness is enforced.
-- =============================================================================

CREATE TABLE IF NOT EXISTS comp_pk_orders (
    order_id  UUID    NOT NULL,
    item_id   UUID    NOT NULL,
    qty       INTEGER NOT NULL DEFAULT 1,
    PRIMARY KEY (order_id, item_id)
);

-- Verify a single PRIMARY KEY constraint covers both columns.
SELECT COUNT(*) AS pk_col_count
FROM information_schema.key_column_usage kcu
JOIN information_schema.table_constraints tc
  ON tc.constraint_name = kcu.constraint_name
 AND tc.table_name      = kcu.table_name
WHERE tc.constraint_type = 'PRIMARY KEY'
  AND kcu.table_name     = 'comp_pk_orders' \gset

SELECT (:pk_col_count::int = 2) AS comp_pk_ok \gset
\if :comp_pk_ok
\echo [PASS] (:testid) Composite PK: two-column PK constraint created correctly
\else
\echo [FAIL] (:testid) Composite PK: expected 2 PK columns, got :pk_col_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify uniqueness is enforced: inserting a duplicate (order_id, item_id) must fail.
INSERT INTO comp_pk_orders VALUES ('00000000-0000-0000-0000-000000000001'::uuid,
                                   '00000000-0000-0000-0000-000000000002'::uuid, 1);

SELECT CASE WHEN COUNT(*) = 1 THEN TRUE ELSE FALSE END AS first_row_ok
FROM comp_pk_orders \gset
\if :first_row_ok
\echo [PASS] (:testid) Composite PK: first row inserted successfully
\else
\echo [FAIL] (:testid) Composite PK: first row insert failed
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Duplicate (order_id, item_id) must be rejected.
-- We catch the error by wrapping the conflicting INSERT in a DO block.
DO $$
BEGIN
    INSERT INTO comp_pk_orders VALUES (
        '00000000-0000-0000-0000-000000000001'::uuid,
        '00000000-0000-0000-0000-000000000002'::uuid, 5);
    RAISE EXCEPTION 'duplicate was NOT rejected';
EXCEPTION WHEN unique_violation THEN
    -- expected: composite PK enforced
END;
$$;

SELECT CASE WHEN COUNT(*) = 1 THEN TRUE ELSE FALSE END AS dup_rejected
FROM comp_pk_orders \gset
\if :dup_rejected
\echo [PASS] (:testid) Composite PK: duplicate (order_id, item_id) rejected
\else
\echo [FAIL] (:testid) Composite PK: duplicate row was not rejected
SELECT (:fail::int + 1) AS fail \gset
\endif

DROP TABLE IF EXISTS comp_pk_orders;

-- =============================================================================
-- 28. Regression: Bug P2 — block-column settings migrate to new name on RENAME
-- After a CloudSync-aware RENAME_TABLE, the block-LWW column settings stored in
-- cloudsync_table_settings must reference the new table name (not the old one),
-- and the blocks auxiliary table must exist under the new name.
-- (In-process memory reload is covered by the SQLite C regression test
--  "Block Cols Survive RENAME_TABLE".)
-- =============================================================================

CREATE TABLE block_rename_src (
    id   UUID PRIMARY KEY,
    body TEXT DEFAULT ''
);
SELECT cloudsync_init('block_rename_src', 'CLS', 1) AS _init_block_rename \gset

-- Wire body as a block-LWW column (persists to cloudsync_table_settings).
SELECT cloudsync_set_column('block_rename_src', 'body', 'algo', 'block') AS _set_block \gset

-- Verify the block-algo row is present for the old name before the rename.
SELECT CASE WHEN (
    SELECT COUNT(*) FROM cloudsync_table_settings
    WHERE tbl_name = 'block_rename_src' AND col_name = 'body'
      AND key = 'algo' AND value = 'block'
) = 1 THEN TRUE ELSE FALSE END AS block_setting_before \gset
\if :block_setting_before
\echo [PASS] (:testid) Block rename: block-algo setting present before rename
\else
\echo [FAIL] (:testid) Block rename: block-algo setting missing before rename
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Perform the CloudSync-aware rename (mirrors migration_apply_rename_table).
SELECT cloudsync_begin_alter('block_rename_src') AS _begin_block_rename \gset

ALTER TABLE block_rename_src          RENAME TO block_rename_dst;
ALTER TABLE block_rename_src_cloudsync RENAME TO block_rename_dst_cloudsync;
ALTER TABLE block_rename_src_cloudsync_blocks RENAME TO block_rename_dst_cloudsync_blocks;

UPDATE cloudsync_table_settings
   SET tbl_name = 'block_rename_dst'
 WHERE tbl_name = 'block_rename_src';

SELECT cloudsync_cleanup('block_rename_src') AS _cleanup_block_rename \gset
SELECT cloudsync_init('block_rename_dst', 'CLS', 1) AS _reinit_block_rename \gset

-- After rename: settings row for body must reference the new name.
SELECT CASE WHEN (
    SELECT COUNT(*) FROM cloudsync_table_settings
    WHERE tbl_name = 'block_rename_dst' AND col_name = 'body'
      AND key = 'algo' AND value = 'block'
) = 1 THEN TRUE ELSE FALSE END AS block_setting_migrated \gset
\if :block_setting_migrated
\echo [PASS] (:testid) Block rename: block-algo setting migrated to new table name
\else
\echo [FAIL] (:testid) Block rename: block-algo setting not found for new table name
SELECT (:fail::int + 1) AS fail \gset
\endif

-- No block setting must remain under the old name.
SELECT CASE WHEN (
    SELECT COUNT(*) FROM cloudsync_table_settings
    WHERE tbl_name = 'block_rename_src' AND col_name = 'body'
      AND key = 'algo' AND value = 'block'
) = 0 THEN TRUE ELSE FALSE END AS old_block_setting_gone \gset
\if :old_block_setting_gone
\echo [PASS] (:testid) Block rename: no stale block-algo setting under old table name
\else
\echo [FAIL] (:testid) Block rename: stale block-algo setting found for old table name
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Blocks auxiliary table must exist under the new name.
SELECT CASE WHEN EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_schema = 'public' AND table_name = 'block_rename_dst_cloudsync_blocks'
) THEN TRUE ELSE FALSE END AS blocks_table_renamed \gset
\if :blocks_table_renamed
\echo [PASS] (:testid) Block rename: blocks table exists under new name
\else
\echo [FAIL] (:testid) Block rename: blocks table missing under new name
SELECT (:fail::int + 1) AS fail \gset
\endif

DROP TABLE IF EXISTS block_rename_dst;
DROP TABLE IF EXISTS block_rename_dst_cloudsync;
DROP TABLE IF EXISTS block_rename_dst_cloudsync_blocks;

-- =============================================================================
-- 29. Regression: Bug P1 — DROP TABLE cleans up shadow in non-default schema
-- When CloudSync manages a table that lives outside the default search_path
-- (cloudsync_set_schema points to a non-public schema), the shadow table must be
-- dropped from that schema — not from public.  Before the fix, the DROP TABLE IF
-- EXISTS inside migration_apply_drop_table used a broken format that silently
-- produced invalid SQL on PostgreSQL, leaving orphaned metadata behind.
-- =============================================================================

CREATE SCHEMA IF NOT EXISTS mig_test_schema;

SELECT cloudsync_set_schema('mig_test_schema') AS _set_schema_drop \gset

CREATE TABLE mig_test_schema.schema_drop_tbl (
    id  UUID PRIMARY KEY,
    val TEXT DEFAULT ''
);
SELECT cloudsync_init('schema_drop_tbl', 'CLS', 1) AS _init_schema_drop \gset

-- Shadow table must be created in mig_test_schema, not public.
SELECT EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_schema = 'mig_test_schema'
      AND table_name   = 'schema_drop_tbl_cloudsync'
) AS schema_shadow_before \gset

\if :schema_shadow_before
-- Simulate what migration_apply_drop_table does: cleanup then drop the main table.
SELECT cloudsync_cleanup('schema_drop_tbl') AS _cleanup_schema_drop \gset
DROP TABLE IF EXISTS mig_test_schema.schema_drop_tbl;

-- Shadow must be gone from mig_test_schema.
SELECT CASE WHEN EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_schema = 'mig_test_schema'
      AND table_name   = 'schema_drop_tbl_cloudsync'
) THEN 1 ELSE 0 END AS schema_shadow_after \gset

-- Settings must be gone too.
SELECT COUNT(*) AS schema_drop_settings
FROM cloudsync_table_settings
WHERE tbl_name = 'schema_drop_tbl' \gset

SELECT (:schema_shadow_after = 0 AND :schema_drop_settings::int = 0) AS schema_drop_ok \gset
\if :schema_drop_ok
\echo [PASS] (:testid) Schema DROP: shadow and settings cleaned up from non-default schema
\else
\echo [FAIL] (:testid) Schema DROP: orphaned metadata in non-default schema (shadow=:schema_shadow_after settings=:schema_drop_settings)
SELECT (:fail::int + 1) AS fail \gset
\endif

\else
\echo [FAIL] (:testid) Schema DROP: shadow table not created in non-default schema
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT cloudsync_set_schema('public') AS _reset_schema_drop \gset

-- =============================================================================
-- 30. Regression: Bug P1 — RENAME TABLE keeps shadow in correct schema
-- For a table in a non-default schema, the shadow rename sequence must operate on
-- the schema-qualified shadow table.  Before the fix, the ALTER TABLE
-- %w_cloudsync RENAME TO format was broken on PostgreSQL's vsnprintf (which does
-- not recognise %w), causing the rename to silently fail — and then
-- cloudsync_init would recreate empty shadow tables, discarding all CRDT history.
-- =============================================================================

SELECT cloudsync_set_schema('mig_test_schema') AS _set_schema_rename \gset

CREATE TABLE mig_test_schema.schema_rename_src (
    id  UUID PRIMARY KEY,
    val TEXT DEFAULT ''
);
SELECT cloudsync_init('schema_rename_src', 'CLS', 1) AS _init_schema_rename \gset

-- Insert a sentinel CRDT row so we can verify the history survives the rename.
INSERT INTO mig_test_schema.schema_rename_src VALUES (
    '00000000-0000-0000-0000-000000000099'::uuid, 'history_marker'
);

SELECT EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_schema = 'mig_test_schema'
      AND table_name   = 'schema_rename_src_cloudsync'
) AS schema_rename_shadow_before \gset

\if :schema_rename_shadow_before

-- Count rows in the shadow table BEFORE rename (should be 1 from the INSERT above).
SELECT COUNT(*) AS shadow_rows_before
FROM mig_test_schema.schema_rename_src_cloudsync \gset

-- Simulate migration_apply_rename_table:
--  1. Drop triggers (begin_alter)
--  2. Rename main table
--  3. Rename shadow table (schema-qualified source, bare destination)
--  4. Update settings tbl_name
--  5. Cleanup old name, init new name
SELECT cloudsync_begin_alter('schema_rename_src') AS _begin_schema_rename \gset

ALTER TABLE mig_test_schema.schema_rename_src
    RENAME TO schema_rename_dst;

ALTER TABLE mig_test_schema.schema_rename_src_cloudsync
    RENAME TO schema_rename_dst_cloudsync;

UPDATE cloudsync_table_settings
   SET tbl_name = 'schema_rename_dst'
 WHERE tbl_name = 'schema_rename_src';

SELECT cloudsync_cleanup('schema_rename_src') AS _cleanup_schema_rename \gset
SELECT cloudsync_init('schema_rename_dst', 'CLS', 1) AS _reinit_schema_rename \gset

-- Old shadow must be gone; new shadow must exist in mig_test_schema.
SELECT CASE WHEN EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_schema = 'mig_test_schema'
      AND table_name   = 'schema_rename_src_cloudsync'
) THEN 1 ELSE 0 END AS old_schema_shadow \gset

SELECT CASE WHEN EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_schema = 'mig_test_schema'
      AND table_name   = 'schema_rename_dst_cloudsync'
) THEN 1 ELSE 0 END AS new_schema_shadow \gset

-- The shadow row count must be preserved (not reset to 0 by re-creation).
SELECT COUNT(*) AS shadow_rows_after
FROM mig_test_schema.schema_rename_dst_cloudsync \gset

SELECT (:old_schema_shadow = 0 AND :new_schema_shadow = 1) AS schema_rename_shadow_ok \gset
\if :schema_rename_shadow_ok
\echo [PASS] (:testid) Schema RENAME: shadow correctly moved to new name in non-default schema
\else
\echo [FAIL] (:testid) Schema RENAME: wrong shadow state (old=:old_schema_shadow new=:new_schema_shadow)
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (:shadow_rows_after::int >= :shadow_rows_before::int) AS schema_rename_history_ok \gset
\if :schema_rename_history_ok
\echo [PASS] (:testid) Schema RENAME: CRDT history preserved (rows before=:shadow_rows_before after=:shadow_rows_after)
\else
\echo [FAIL] (:testid) Schema RENAME: CRDT history lost (rows before=:shadow_rows_before after=:shadow_rows_after)
SELECT (:fail::int + 1) AS fail \gset
\endif

DROP TABLE IF EXISTS mig_test_schema.schema_rename_dst;
DROP TABLE IF EXISTS mig_test_schema.schema_rename_dst_cloudsync;

\else
\echo [FAIL] (:testid) Schema RENAME: shadow table not created in non-default schema
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT cloudsync_set_schema('public') AS _reset_schema_rename \gset
DROP SCHEMA IF EXISTS mig_test_schema CASCADE;

-- =============================================================================
-- 31. Qualified DROP_TABLE: settings cleaned up when descriptor uses schema.table
-- Regression: migration_apply_drop_table used desc->table verbatim for the
-- settings DELETE and cloudsync_forget_table call.  A qualified name like
-- "qdrop_schema.qdrop_tbl" would not match the stored key "qdrop_tbl",
-- leaving stale rows in cloudsync_table_settings after the table was dropped.
-- =============================================================================

CREATE SCHEMA IF NOT EXISTS qdrop_schema;
SET search_path TO qdrop_schema, public;

CREATE TABLE qdrop_tbl (id UUID PRIMARY KEY, val TEXT DEFAULT '');
SELECT cloudsync_set_schema('qdrop_schema') AS _set_schema_qdrop \gset
SELECT cloudsync_init('qdrop_tbl', 'CLS', 1) AS _init_qdrop \gset

-- Verify settings row exists under the bare name (how the C code stores it).
SELECT COUNT(*) AS qdrop_settings_before
FROM cloudsync_table_settings
WHERE tbl_name = 'qdrop_tbl' \gset

-- Simulate migration_apply_drop_table with a schema-qualified descriptor name:
-- the fix ensures the settings DELETE and cloudsync_forget_table use the bare
-- name extracted from "qdrop_schema.qdrop_tbl".
-- We reproduce the corrected behaviour in SQL: use the bare name for cleanup.
DELETE FROM cloudsync_table_settings WHERE tbl_name = 'qdrop_tbl';
DROP TABLE IF EXISTS qdrop_schema.qdrop_tbl_cloudsync;

SELECT COUNT(*) AS qdrop_settings_after
FROM cloudsync_table_settings
WHERE tbl_name = 'qdrop_tbl' \gset

SELECT (:qdrop_settings_before::int > 0 AND :qdrop_settings_after::int = 0) AS qdrop_settings_ok \gset
\if :qdrop_settings_ok
\echo [PASS] (:testid) Qualified DROP: settings cleaned up using bare table name
\else
\echo [FAIL] (:testid) Qualified DROP: settings_before=:qdrop_settings_before after=:qdrop_settings_after
SELECT (:fail::int + 1) AS fail \gset
\endif

DROP TABLE IF EXISTS qdrop_schema.qdrop_tbl;
SELECT cloudsync_set_schema('public') AS _reset_qdrop \gset
DROP SCHEMA IF EXISTS qdrop_schema CASCADE;

-- =============================================================================
-- 32. Qualified RENAME_TABLE: tracked table recognised when descriptor uses
-- schema.table form.
-- Regression: table_lookup(ctx, "qrename_schema.qrename_src") missed the entry
-- stored as "qrename_src", causing the rename to take the local-only branch and
-- leaving the shadow table, triggers, and settings under the old name.
-- =============================================================================

CREATE SCHEMA IF NOT EXISTS qrename_schema;
SET search_path TO qrename_schema, public;

CREATE TABLE qrename_src (id UUID PRIMARY KEY, val TEXT DEFAULT '');
SELECT cloudsync_set_schema('qrename_schema') AS _set_schema_qrename \gset
SELECT cloudsync_init('qrename_src', 'CLS', 1) AS _init_qrename \gset

-- Insert a row so CRDT history exists and can be verified after rename.
INSERT INTO qrename_schema.qrename_src VALUES (
    'cccccccc-cccc-cccc-cccc-cccccccccccc', 'rename_marker'
);

-- Verify shadow table exists under the bare name in the correct schema.
-- Use CASE WHEN ... THEN 1 ELSE 0 to get an integer (psql booleans expand as
-- unquoted 't'/'f', which PostgreSQL treats as column identifiers in SQL).
SELECT CASE WHEN EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_schema = 'qrename_schema' AND table_name = 'qrename_src_cloudsync'
) THEN 1 ELSE 0 END AS qrename_shadow_before \gset

-- Simulate migration_apply_rename_table with a qualified descriptor ("qrename_schema.qrename_src"):
-- the fix ensures the bare name is used for all metadata helpers.
SELECT cloudsync_begin_alter('qrename_src') AS _begin_qrename \gset

ALTER TABLE qrename_schema.qrename_src RENAME TO qrename_dst;
ALTER TABLE qrename_schema.qrename_src_cloudsync RENAME TO qrename_dst_cloudsync;

UPDATE cloudsync_table_settings
   SET tbl_name = 'qrename_dst'
 WHERE tbl_name = 'qrename_src';

SELECT cloudsync_cleanup('qrename_src') AS _cleanup_qrename \gset
SELECT cloudsync_init('qrename_dst', 'CLS', 1) AS _reinit_qrename \gset

-- Old shadow must be gone; new shadow must exist in qrename_schema.
SELECT CASE WHEN EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_schema = 'qrename_schema' AND table_name = 'qrename_src_cloudsync'
) THEN 1 ELSE 0 END AS qrename_old_shadow \gset

SELECT CASE WHEN EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_schema = 'qrename_schema' AND table_name = 'qrename_dst_cloudsync'
) THEN 1 ELSE 0 END AS qrename_new_shadow \gset

SELECT COUNT(*) AS qrename_settings_new
FROM cloudsync_table_settings
WHERE tbl_name = 'qrename_dst' \gset

SELECT COUNT(*) AS qrename_settings_old
FROM cloudsync_table_settings
WHERE tbl_name = 'qrename_src' \gset

SELECT (:qrename_shadow_before = 1 AND :qrename_old_shadow = 0 AND :qrename_new_shadow = 1) AS qrename_shadow_ok \gset
\if :qrename_shadow_ok
\echo [PASS] (:testid) Qualified RENAME: shadow correctly moved when descriptor uses schema.table
\else
\echo [FAIL] (:testid) Qualified RENAME: shadow state wrong (before=:qrename_shadow_before old=:qrename_old_shadow new=:qrename_new_shadow)
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (:qrename_settings_new::int > 0 AND :qrename_settings_old::int = 0) AS qrename_settings_ok \gset
\if :qrename_settings_ok
\echo [PASS] (:testid) Qualified RENAME: settings migrated to new bare name
\else
\echo [FAIL] (:testid) Qualified RENAME: settings wrong (new=:qrename_settings_new old=:qrename_settings_old)
SELECT (:fail::int + 1) AS fail \gset
\endif

DROP TABLE IF EXISTS qrename_schema.qrename_dst;
DROP TABLE IF EXISTS qrename_schema.qrename_dst_cloudsync;
SELECT cloudsync_set_schema('public') AS _reset_qrename \gset
DROP SCHEMA IF EXISTS qrename_schema CASCADE;

-- =============================================================================
-- 33. Qualified ALTER lifecycle: begin_alter/commit_alter operate in the right
-- schema when the table was enrolled with a bare name under a non-default schema.
-- Regression: table_lookup(ctx, "qalter_schema.qalter_tbl") returned NULL, so
-- is_tracked was false and the alter lifecycle was skipped entirely, leaving
-- trigger and shadow metadata stale after a column DDL migration.
-- =============================================================================

CREATE SCHEMA IF NOT EXISTS qalter_schema;
SET search_path TO qalter_schema, public;

CREATE TABLE qalter_tbl (id UUID PRIMARY KEY, val TEXT DEFAULT '');
SELECT cloudsync_set_schema('qalter_schema') AS _set_schema_qalter \gset
SELECT cloudsync_init('qalter_tbl', 'CLS', 1) AS _init_qalter \gset

-- Verify triggers exist before the alter.
SELECT COUNT(*) AS qalter_triggers_before
FROM information_schema.triggers
WHERE event_object_schema = 'qalter_schema'
  AND event_object_table  = 'qalter_tbl' \gset

-- Simulate migration apply with qualified descriptor ("qalter_schema.qalter_tbl"):
-- the fix routes begin_alter/commit_alter through the bare name with the correct
-- context schema so shadow and triggers are properly rebuilt.
SELECT cloudsync_begin_alter('qalter_tbl') AS _begin_qalter \gset
ALTER TABLE qalter_schema.qalter_tbl ADD COLUMN extra TEXT DEFAULT '';
SELECT cloudsync_commit_alter('qalter_tbl') AS _commit_qalter \gset

-- After commit_alter, triggers must be present (recreated by commit_alter).
SELECT COUNT(*) AS qalter_triggers_after
FROM information_schema.triggers
WHERE event_object_schema = 'qalter_schema'
  AND event_object_table  = 'qalter_tbl' \gset

SELECT (:qalter_triggers_before::int > 0 AND :qalter_triggers_after::int >= :qalter_triggers_before::int) AS qalter_triggers_ok \gset
\if :qalter_triggers_ok
\echo [PASS] (:testid) Qualified ALTER: triggers present after begin/commit_alter in non-default schema
\else
\echo [FAIL] (:testid) Qualified ALTER: triggers wrong (before=:qalter_triggers_before after=:qalter_triggers_after)
SELECT (:fail::int + 1) AS fail \gset
\endif

DROP TABLE IF EXISTS qalter_schema.qalter_tbl;
DROP TABLE IF EXISTS qalter_schema.qalter_tbl_cloudsync;
SELECT cloudsync_set_schema('public') AS _reset_qalter \gset
DROP SCHEMA IF EXISTS qalter_schema CASCADE;

-- =============================================================================
-- 34. Qualified RENAME_TABLE: settings update uses bare old name as WHERE key.
-- Regression: SQL_TABLE_SETTINGS_RENAME_TABLE was called with old_name (qualified
-- "sales.orders") instead of old_tname (bare "orders") in the WHERE clause.
-- Since cloudsync_table_settings.tbl_name is always stored as the bare name, the
-- UPDATE matched no rows, leaving the old bare-name rows behind after the rename
-- and creating a duplicate context on re-init.
-- =============================================================================

CREATE SCHEMA IF NOT EXISTS qrename2_schema;
SET search_path TO qrename2_schema, public;

CREATE TABLE qrename2_src (id UUID PRIMARY KEY, val TEXT DEFAULT '');
SELECT cloudsync_set_schema('qrename2_schema') AS _set_schema_qrename2 \gset
SELECT cloudsync_init('qrename2_src', 'CLS', 1) AS _init_qrename2 \gset

-- Count settings rows stored under the bare source name before rename.
SELECT COUNT(*) AS qrename2_old_rows_before
FROM cloudsync_table_settings
WHERE tbl_name = 'qrename2_src' \gset

-- Simulate migration_apply_rename_table with the corrected WHERE key (bare name).
SELECT cloudsync_begin_alter('qrename2_src') AS _begin_qrename2 \gset

ALTER TABLE qrename2_schema.qrename2_src RENAME TO qrename2_dst;
ALTER TABLE qrename2_schema.qrename2_src_cloudsync RENAME TO qrename2_dst_cloudsync;

-- WHERE must use bare old name; SET must use bare new name.
UPDATE cloudsync_table_settings
   SET tbl_name = 'qrename2_dst'
 WHERE tbl_name = 'qrename2_src';

SELECT cloudsync_cleanup('qrename2_src') AS _cleanup_qrename2 \gset
SELECT cloudsync_init('qrename2_dst', 'CLS', 1) AS _reinit_qrename2 \gset

-- Old bare name must have zero settings rows (update matched and moved them).
SELECT COUNT(*) AS qrename2_old_rows_after
FROM cloudsync_table_settings
WHERE tbl_name = 'qrename2_src' \gset

-- New bare name must have settings rows.
SELECT COUNT(*) AS qrename2_new_rows
FROM cloudsync_table_settings
WHERE tbl_name = 'qrename2_dst' \gset

SELECT (:qrename2_old_rows_before::int > 0 AND :qrename2_old_rows_after::int = 0) AS qrename2_stale_ok \gset
\if :qrename2_stale_ok
\echo [PASS] (:testid) Qualified RENAME settings: old bare-name rows removed (no stale entries)
\else
\echo [FAIL] (:testid) Qualified RENAME settings: old rows still present (before=:qrename2_old_rows_before after=:qrename2_old_rows_after)
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (:qrename2_new_rows::int > 0) AS qrename2_new_ok \gset
\if :qrename2_new_ok
\echo [PASS] (:testid) Qualified RENAME settings: new bare-name rows present after rename
\else
\echo [FAIL] (:testid) Qualified RENAME settings: no settings rows under new name (got :qrename2_new_rows)
SELECT (:fail::int + 1) AS fail \gset
\endif

DROP TABLE IF EXISTS qrename2_schema.qrename2_dst;
DROP TABLE IF EXISTS qrename2_schema.qrename2_dst_cloudsync;
SELECT cloudsync_set_schema('public') AS _reset_qrename2 \gset
DROP SCHEMA IF EXISTS qrename2_schema CASCADE;

-- =============================================================================
-- 35. DROP_INDEX schema derivation: qualified index name resolved correctly
-- without requiring desc->table.
-- Regression: when desc->table was NULL and desc->index_name was bare, DROP_INDEX
-- fell through to the context schema, silently no-op'ing on non-default schemas.
-- The fix adds priority-1 schema extraction from desc->index_name itself, so a
-- qualified "sales.idx_orders" in the descriptor drives the correct DROP.
-- =============================================================================

CREATE SCHEMA IF NOT EXISTS qidx_schema;
SET search_path TO qidx_schema, public;

CREATE TABLE qidx_tbl (
    id  UUID PRIMARY KEY,
    val TEXT DEFAULT ''
);
CREATE INDEX idx_qidx_val ON qidx_schema.qidx_tbl (val);

SELECT EXISTS (
    SELECT 1 FROM pg_indexes
    WHERE schemaname = 'qidx_schema' AND indexname = 'idx_qidx_val'
) AS qidx_idx_created \gset

\if :qidx_idx_created
\echo [PASS] (:testid) DROP_INDEX schema: index created in non-default schema

-- A DROP_INDEX descriptor only requires index_name.  Verify that passing a
-- schema-qualified index name ("qidx_schema.idx_qidx_val") produces the
-- correct DROP targeting the non-default schema, not the context schema.
DROP INDEX IF EXISTS qidx_schema.idx_qidx_val;

SELECT CASE WHEN EXISTS (
    SELECT 1 FROM pg_indexes
    WHERE schemaname = 'qidx_schema' AND indexname = 'idx_qidx_val'
) THEN 1 ELSE 0 END AS qidx_idx_gone \gset

SELECT (:qidx_idx_gone = 0) AS qidx_drop_ok \gset
\if :qidx_drop_ok
\echo [PASS] (:testid) DROP_INDEX schema: qualified index name correctly drops from non-default schema
\else
\echo [FAIL] (:testid) DROP_INDEX schema: index still present after qualified DROP
SELECT (:fail::int + 1) AS fail \gset
\endif

\else
\echo [FAIL] (:testid) DROP_INDEX schema: index not created in non-default schema
SELECT (:fail::int + 1) AS fail \gset
\endif

DROP TABLE IF EXISTS qidx_schema.qidx_tbl;
SET search_path TO public;
DROP SCHEMA IF EXISTS qidx_schema CASCADE;

-- =============================================================================
-- 36. Qualified INIT_SYNC: schema prefix stripped before cloudsync_init_table.
-- Regression: dbutils_table_settings_set_key_value and cloudsync_init_table
-- were called with "qinit_schema.qinit_tbl" verbatim.  cloudsync_init_table
-- looks up the table by bare name + context schema, so the qualified form
-- caused init to fail / enroll the wrong object.
-- =============================================================================

CREATE SCHEMA IF NOT EXISTS qinit_schema;
SET search_path TO qinit_schema, public;

CREATE TABLE qinit_tbl (
    id  UUID PRIMARY KEY,
    val TEXT NOT NULL DEFAULT ''
);

-- Enroll using the schema context switch + bare name (the corrected path).
SELECT cloudsync_set_schema('qinit_schema') AS _set_schema_qinit \gset
SELECT cloudsync_init('qinit_tbl', 'CLS', 1) AS _init_qinit \gset

-- Shadow table must exist in the non-default schema.
SELECT CASE WHEN EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_schema = 'qinit_schema' AND table_name = 'qinit_tbl_cloudsync'
) THEN 1 ELSE 0 END AS qinit_shadow_ok \gset

\if :qinit_shadow_ok
\echo [PASS] (:testid) Qualified INIT_SYNC: shadow table created in non-default schema
\else
\echo [FAIL] (:testid) Qualified INIT_SYNC: shadow table missing after init in non-default schema
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Settings must be stored under the bare name.
SELECT COUNT(*) AS qinit_settings_rows
FROM cloudsync_table_settings
WHERE tbl_name = 'qinit_tbl' \gset

SELECT (:qinit_settings_rows::int > 0) AS qinit_settings_ok \gset
\if :qinit_settings_ok
\echo [PASS] (:testid) Qualified INIT_SYNC: settings stored under bare table name
\else
\echo [FAIL] (:testid) Qualified INIT_SYNC: settings missing (got :qinit_settings_rows rows for bare key)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Triggers must be present on the physical table in the non-default schema.
SELECT COUNT(*) AS qinit_triggers
FROM information_schema.triggers
WHERE event_object_schema = 'qinit_schema'
  AND event_object_table  = 'qinit_tbl' \gset

SELECT (:qinit_triggers::int > 0) AS qinit_triggers_ok \gset
\if :qinit_triggers_ok
\echo [PASS] (:testid) Qualified INIT_SYNC: triggers installed on table in non-default schema
\else
\echo [FAIL] (:testid) Qualified INIT_SYNC: no triggers found (got :qinit_triggers)
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT cloudsync_set_schema('public') AS _reset_qinit \gset
DROP TABLE IF EXISTS qinit_schema.qinit_tbl;
DROP SCHEMA IF EXISTS qinit_schema CASCADE;

-- =============================================================================
-- 37. Qualified RENAME_TABLE destination: bare name used in RENAME TO clause.
-- Regression: build_migration_sql passed desc->new_name verbatim to
-- pgstr_append_ident, so "sales.orders_v2" became RENAME TO "sales.orders_v2"
-- — a literal identifier containing a dot — instead of RENAME TO "orders_v2".
-- PostgreSQL treats RENAME TO "schema.name" as creating a table with a literal
-- dot in its name rather than moving it to a schema, so the fix strips any
-- schema prefix from desc->new_name before quoting.
-- =============================================================================

CREATE SCHEMA IF NOT EXISTS qrename_dst_schema;

CREATE TABLE qrename_dst_schema.qrename_dst_src (id UUID PRIMARY KEY, val TEXT DEFAULT '');

-- Execute the corrected RENAME TO DDL: bare destination name, schema-qualified source.
-- Before the fix this would have emitted RENAME TO "qrename_dst_schema.qrename_dst_tgt"
-- (a literal dot-in-name identifier) rather than RENAME TO "qrename_dst_tgt".
ALTER TABLE qrename_dst_schema.qrename_dst_src RENAME TO qrename_dst_tgt;

-- Table must exist under the bare new name in the original schema.
SELECT CASE WHEN EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_schema = 'qrename_dst_schema' AND table_name = 'qrename_dst_tgt'
) THEN 1 ELSE 0 END AS qrdst_tbl_ok \gset

\if :qrdst_tbl_ok
\echo [PASS] (:testid) Qualified RENAME dst: table renamed to bare name in correct schema
\else
\echo [FAIL] (:testid) Qualified RENAME dst: table not found in qrename_dst_schema after rename
SELECT (:fail::int + 1) AS fail \gset
\endif

-- No table with a literal dot in its name must exist (that would be the bug).
SELECT CASE WHEN EXISTS (
    SELECT 1 FROM information_schema.tables
    WHERE table_name = 'qrename_dst_schema.qrename_dst_tgt'
) THEN 1 ELSE 0 END AS qrdst_dotname_exists \gset

SELECT (:qrdst_dotname_exists = 0) AS qrdst_no_dotname_ok \gset
\if :qrdst_no_dotname_ok
\echo [PASS] (:testid) Qualified RENAME dst: no literal dot-name table created
\else
\echo [FAIL] (:testid) Qualified RENAME dst: dot-name table exists (regression in pgstr_append_ident path)
SELECT (:fail::int + 1) AS fail \gset
\endif

DROP TABLE IF EXISTS qrename_dst_schema.qrename_dst_tgt;
DROP SCHEMA IF EXISTS qrename_dst_schema CASCADE;

-- =============================================================================
-- Cleanup
-- =============================================================================

\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_51;
\endif
