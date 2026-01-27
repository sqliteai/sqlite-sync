-- Per-Table Schema Tracking Tests
-- Tests from plans/PLAN_per_table_schema_tracking.md

\set testid '13'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

-- Cleanup any existing test databases and schemas
DROP DATABASE IF EXISTS cloudsync_test_13;
CREATE DATABASE cloudsync_test_13;

\connect cloudsync_test_13
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

-- ============================================================================
-- Test 1: Basic Schema Detection
-- ============================================================================

CREATE SCHEMA test_schema;
CREATE TABLE test_schema.products (id TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '');
SELECT cloudsync_set_schema('test_schema') AS _set_schema \gset
SELECT cloudsync_init('products', 'CLS', true) AS _init_products \gset

-- Test: Verify schema is detected correctly
SELECT cloudsync_table_schema('products') AS detected_schema \gset
SELECT (:'detected_schema' = 'test_schema') AS basic_schema_detection_ok \gset
\if :basic_schema_detection_ok
\echo [PASS] (:testid) Basic Schema Detection
\else
\echo [FAIL] (:testid) Basic Schema Detection - Expected 'test_schema', got '':detected_schema
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test 2: Same-Connection Duplicate Prevention
-- ============================================================================

CREATE TABLE public.users (id TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '');

SELECT cloudsync_set_schema('public') AS _set_public \gset
SELECT cloudsync_init('users', 'CLS', true) AS _init_users_public \gset

-- Attempt to init again in same connection (should FAIL)
DO $$
BEGIN
    PERFORM cloudsync_init('users', 'CLS', true);
    RAISE EXCEPTION 'Expected error but init succeeded';
EXCEPTION
    WHEN OTHERS THEN
        -- Expected to fail
        NULL;
END $$;

\echo [PASS] (:testid) Same-Connection Duplicate Prevention

-- ============================================================================
-- Test 3: Schema Not Found (NULL handling)
-- ============================================================================

CREATE TABLE orphan_table (id TEXT PRIMARY KEY);

-- Test: Querying schema of non-initialized table should return NULL or empty
DO $$
DECLARE
    orphan_schema TEXT;
BEGIN
    SELECT cloudsync_table_schema('orphan_table') INTO orphan_schema;
    IF orphan_schema IS NOT NULL THEN
        RAISE EXCEPTION 'Expected NULL for orphan table schema, got %', orphan_schema;
    END IF;
END $$;

\echo [PASS] (:testid) Schema Not Found (NULL handling)

-- ============================================================================
-- Test 4: Schema Setting Does Not Affect Existing Tables
-- ============================================================================

CREATE SCHEMA schema_a;
CREATE SCHEMA schema_b;
CREATE TABLE schema_a.orders (id TEXT PRIMARY KEY, total TEXT NOT NULL DEFAULT '0');
CREATE TABLE schema_b.products_b (id TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '');

-- Initialize in schema_a
SELECT cloudsync_set_schema('schema_a') AS _set_schema_a \gset
SELECT cloudsync_init('orders', 'CLS', true) AS _init_orders \gset

-- Verify schema
SELECT cloudsync_table_schema('orders') AS orders_schema_before \gset
SELECT (:'orders_schema_before' = 'schema_a') AS orders_schema_before_ok \gset
\if :orders_schema_before_ok
\echo [PASS] (:testid) Schema Setting - Initial schema correct (schema_a)
\else
\echo [FAIL] (:testid) Schema Setting - Initial schema incorrect. Expected 'schema_a', got '':orders_schema_before
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Change global schema setting
SELECT cloudsync_set_schema('schema_b') AS _set_schema_b \gset

-- Test: Existing table still uses original schema
SELECT cloudsync_table_schema('orders') AS orders_schema_after \gset
SELECT (:'orders_schema_after' = 'schema_a') AS orders_schema_unchanged_ok \gset
\if :orders_schema_unchanged_ok
\echo [PASS] (:testid) Schema Setting Does Not Affect Existing Tables
\else
\echo [FAIL] (:testid) Schema Setting Does Not Affect Existing Tables - Expected 'schema_a', got '':orders_schema_after
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test: New initialization uses new schema
SELECT cloudsync_init('products_b', 'CLS', true) AS _init_products_b \gset
SELECT cloudsync_table_schema('products_b') AS products_schema \gset
SELECT (:'products_schema' = 'schema_b') AS new_table_uses_new_schema_ok \gset
\if :new_table_uses_new_schema_ok
\echo [PASS] (:testid) New table uses new schema setting (schema_b)
\else
\echo [FAIL] (:testid) New table uses new schema setting - Expected 'schema_b', got '':products_schema
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test 5: Multi-Schema Database with Different Table Names
-- ============================================================================

CREATE SCHEMA sales;
CREATE SCHEMA analytics;

CREATE TABLE sales.orders_sales (id TEXT PRIMARY KEY, total TEXT NOT NULL DEFAULT '0');
CREATE TABLE analytics.reports (id TEXT PRIMARY KEY, data TEXT NOT NULL DEFAULT '{}');

SELECT cloudsync_set_schema('sales') AS _set_sales \gset
SELECT cloudsync_init('orders_sales', 'CLS', true) AS _init_orders_sales \gset

SELECT cloudsync_set_schema('analytics') AS _set_analytics \gset
SELECT cloudsync_init('reports', 'CLS', true) AS _init_reports \gset

-- Both should work independently
INSERT INTO sales.orders_sales VALUES (cloudsync_uuid()::text, '100.00');
INSERT INTO analytics.reports VALUES (cloudsync_uuid()::text, '{"type":"summary"}');

-- Verify changes tracked in correct schemas
SELECT
  (SELECT COUNT(*) > 0 FROM sales.orders_sales_cloudsync) AND
  (SELECT COUNT(*) > 0 FROM analytics.reports_cloudsync) AS multi_schema_ok \gset
\if :multi_schema_ok
\echo [PASS] (:testid) Multi-Schema Database with Different Table Names
\else
\echo [FAIL] (:testid) Multi-Schema Database with Different Table Names
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test 6: System Tables in Public Schema
-- ============================================================================

CREATE SCHEMA custom_schema;
SELECT cloudsync_set_schema('custom_schema') AS _set_custom \gset
CREATE TABLE custom_schema.test_table (id TEXT PRIMARY KEY, val TEXT NOT NULL DEFAULT '');
SELECT cloudsync_init('test_table', 'CLS', true) AS _init_test_table \gset

-- System tables should still be in public
SELECT COUNT(*) AS system_tables_in_public FROM pg_tables
WHERE tablename IN ('cloudsync_settings', 'cloudsync_site_id', 'cloudsync_table_settings', 'cloudsync_schema_versions')
  AND schemaname = 'public' \gset

-- Metadata table should be in custom_schema
SELECT schemaname AS metadata_schema FROM pg_tables
WHERE tablename = 'test_table_cloudsync' \gset

SELECT (:system_tables_in_public = 4 AND :'metadata_schema' = 'custom_schema') AS system_tables_ok \gset
\if :system_tables_ok
\echo [PASS] (:testid) System Tables in Public Schema
\else
\echo [FAIL] (:testid) System Tables in Public Schema - System tables: :system_tables_in_public/4, metadata schema: ':metadata_schema'
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test 7: Metadata Table Location Detection
-- ============================================================================

-- Verify that metadata tables are created in the correct schemas
SELECT COUNT(*) AS products_meta FROM pg_tables
WHERE tablename = 'products_cloudsync' AND schemaname = 'test_schema' \gset

SELECT COUNT(*) AS users_meta FROM pg_tables
WHERE tablename = 'users_cloudsync' AND schemaname = 'public' \gset

SELECT COUNT(*) AS orders_meta FROM pg_tables
WHERE tablename = 'orders_cloudsync' AND schemaname = 'schema_a' \gset

SELECT (:products_meta = 1 AND :users_meta = 1 AND :orders_meta = 1) AS metadata_locations_ok \gset
\if :metadata_locations_ok
\echo [PASS] (:testid) Metadata Table Location Detection
\else
\echo [FAIL] (:testid) Metadata Table Location Detection - products: :products_meta, users: :users_meta, orders: :orders_meta
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Test 8: Schema-Qualified Queries Work Correctly
-- ============================================================================

-- Insert data into different schemas and verify they're independent
INSERT INTO test_schema.products VALUES (cloudsync_uuid()::text, 'Product A');
INSERT INTO public.users VALUES (cloudsync_uuid()::text, 'User A');
INSERT INTO schema_a.orders VALUES (cloudsync_uuid()::text, '50.00');

-- Count rows in each table
SELECT COUNT(*) AS products_count FROM test_schema.products \gset
SELECT COUNT(*) AS users_count FROM public.users \gset
SELECT COUNT(*) AS orders_count FROM schema_a.orders \gset

-- All should have at least one row
SELECT (:products_count > 0 AND :users_count > 0 AND :orders_count > 0) AS qualified_queries_ok \gset
\if :qualified_queries_ok
\echo [PASS] (:testid) Schema-Qualified Queries Work Correctly
\else
\echo [FAIL] (:testid) Schema-Qualified Queries Work Correctly - products: :products_count, users: :users_count, orders: :orders_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Cleanup: Drop test databases if not in DEBUG mode and no failures
-- ============================================================================

\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_13;
\endif
