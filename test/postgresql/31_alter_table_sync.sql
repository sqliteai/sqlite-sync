-- Alter Table Sync Test
-- Tests cloudsync_begin_alter and cloudsync_commit_alter functions.
-- Verifies that schema changes (add column) are handled correctly
-- and data syncs after alteration.

\set testid '31'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

-- Cleanup and create test databases
DROP DATABASE IF EXISTS cloudsync_test_31a;
DROP DATABASE IF EXISTS cloudsync_test_31b;
CREATE DATABASE cloudsync_test_31a;
CREATE DATABASE cloudsync_test_31b;

-- ============================================================================
-- Setup Database A
-- ============================================================================

\connect cloudsync_test_31a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE products (
    id UUID PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    price DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    quantity INTEGER NOT NULL DEFAULT 0
);

SELECT cloudsync_init('products', 'CLS', false) AS _init_a \gset

INSERT INTO products VALUES ('11111111-1111-1111-1111-111111111111', 'Product A1', 10.99, 100);
INSERT INTO products VALUES ('22222222-2222-2222-2222-222222222222', 'Product A2', 20.50, 200);

-- ============================================================================
-- Setup Database B with same schema
-- ============================================================================

\connect cloudsync_test_31b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE products (
    id UUID PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    price DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    quantity INTEGER NOT NULL DEFAULT 0
);

SELECT cloudsync_init('products', 'CLS', false) AS _init_b \gset

INSERT INTO products VALUES ('33333333-3333-3333-3333-333333333333', 'Product B1', 30.00, 300);
INSERT INTO products VALUES ('44444444-4444-4444-4444-444444444444', 'Product B2', 40.75, 400);

-- ============================================================================
-- Initial Sync: A -> B and B -> A
-- ============================================================================

\echo [INFO] (:testid) === Initial Sync Before ALTER ===

-- Encode payload from A
\connect cloudsync_test_31a
\ir helper_psql_conn_setup.sql
SELECT cloudsync_init('products', 'CLS', false) AS _reinit \gset
SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_a_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

-- Apply A's payload to B, encode B's payload
\connect cloudsync_test_31b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_init('products', 'CLS', false) AS _reinit \gset
SELECT cloudsync_payload_apply(decode(:'payload_a_hex', 'hex')) AS apply_a_to_b \gset

SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_b_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

-- Apply B's payload to A, verify initial sync
\connect cloudsync_test_31a
\ir helper_psql_conn_setup.sql
SELECT cloudsync_init('products', 'CLS', false) AS _reinit \gset
SELECT cloudsync_payload_apply(decode(:'payload_b_hex', 'hex')) AS apply_b_to_a \gset

SELECT COUNT(*) AS count_a_initial FROM products \gset

\connect cloudsync_test_31b
\ir helper_psql_conn_setup.sql
SELECT COUNT(*) AS count_b_initial FROM products \gset

SELECT (:count_a_initial = 4 AND :count_b_initial = 4) AS initial_sync_ok \gset
\if :initial_sync_ok
\echo [PASS] (:testid) Initial sync complete - both databases have 4 rows
\else
\echo [FAIL] (:testid) Initial sync failed - A: :count_a_initial, B: :count_b_initial
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- ALTER TABLE on Database A (begin_alter + ALTER + commit_alter on SAME connection)
-- ============================================================================

\echo [INFO] (:testid) === ALTER TABLE on Database A ===

\connect cloudsync_test_31a
\ir helper_psql_conn_setup.sql
SELECT cloudsync_init('products', 'CLS', false) AS _reinit \gset

SELECT cloudsync_begin_alter('products') AS begin_alter_a \gset
\if :begin_alter_a
\echo [PASS] (:testid) cloudsync_begin_alter succeeded on Database A
\else
\echo [FAIL] (:testid) cloudsync_begin_alter failed on Database A
SELECT (:fail::int + 1) AS fail \gset
\endif

ALTER TABLE products ADD COLUMN description TEXT NOT NULL DEFAULT '';

SELECT cloudsync_commit_alter('products') AS commit_alter_a \gset
\if :commit_alter_a
\echo [PASS] (:testid) cloudsync_commit_alter succeeded on Database A
\else
\echo [FAIL] (:testid) cloudsync_commit_alter failed on Database A
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Insert and update post-ALTER data on A
INSERT INTO products (id, name, price, quantity, description)
VALUES ('55555555-5555-5555-5555-555555555555', 'New Product A', 55.55, 555, 'Added after alter on A');

UPDATE products SET description = 'Updated on A' WHERE id = '11111111-1111-1111-1111-111111111111';
UPDATE products SET quantity = 150 WHERE id = '11111111-1111-1111-1111-111111111111';

-- Encode post-ALTER payload from A
SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_a2_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

SELECT (length(:'payload_a2_hex') > 0) AS payload_a2_created \gset
\if :payload_a2_created
\echo [PASS] (:testid) Post-alter payload encoded from Database A
\else
\echo [FAIL] (:testid) Post-alter payload empty from Database A
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- ALTER TABLE on Database B (begin_alter + ALTER + commit_alter on SAME connection)
-- Apply A's payload, insert/update, encode B's payload
-- ============================================================================

\echo [INFO] (:testid) === ALTER TABLE on Database B ===

\connect cloudsync_test_31b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_init('products', 'CLS', false) AS _reinit \gset

SELECT cloudsync_begin_alter('products') AS begin_alter_b \gset
\if :begin_alter_b
\echo [PASS] (:testid) cloudsync_begin_alter succeeded on Database B
\else
\echo [FAIL] (:testid) cloudsync_begin_alter failed on Database B
SELECT (:fail::int + 1) AS fail \gset
\endif

ALTER TABLE products ADD COLUMN description TEXT NOT NULL DEFAULT '';

SELECT cloudsync_commit_alter('products') AS commit_alter_b \gset
\if :commit_alter_b
\echo [PASS] (:testid) cloudsync_commit_alter succeeded on Database B
\else
\echo [FAIL] (:testid) cloudsync_commit_alter failed on Database B
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Insert and update post-ALTER data on B
INSERT INTO products (id, name, price, quantity, description)
VALUES ('66666666-6666-6666-6666-666666666666', 'New Product B', 66.66, 666, 'Added after alter on B');

UPDATE products SET description = 'Updated on B' WHERE id = '33333333-3333-3333-3333-333333333333';
UPDATE products SET quantity = 350 WHERE id = '33333333-3333-3333-3333-333333333333';

-- Apply A's post-alter payload to B
SELECT cloudsync_payload_apply(decode(:'payload_a2_hex', 'hex')) AS apply_a2_to_b \gset

SELECT (:apply_a2_to_b >= 0) AS apply_a2_ok \gset
\if :apply_a2_ok
\echo [PASS] (:testid) Post-alter payload from A applied to B
\else
\echo [FAIL] (:testid) Post-alter payload from A failed to apply to B: :apply_a2_to_b
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Encode post-ALTER payload from B
SELECT encode(
    cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq),
    'hex'
) AS payload_b2_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

-- ============================================================================
-- Apply B's payload to A, then verify final state
-- ============================================================================

\echo [INFO] (:testid) === Apply B payload to A and verify ===

\connect cloudsync_test_31a
\ir helper_psql_conn_setup.sql
SELECT cloudsync_init('products', 'CLS', false) AS _reinit \gset
SELECT cloudsync_payload_apply(decode(:'payload_b2_hex', 'hex')) AS apply_b2_to_a \gset

SELECT (:apply_b2_to_a >= 0) AS apply_b2_ok \gset
\if :apply_b2_ok
\echo [PASS] (:testid) Post-alter payload from B applied to A
\else
\echo [FAIL] (:testid) Post-alter payload from B failed to apply to A: :apply_b2_to_a
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Verify final state
-- ============================================================================

\echo [INFO] (:testid) === Verify Final State ===

-- Compute hash of Database A
SELECT md5(
    COALESCE(
        string_agg(
            id::text || ':' ||
            COALESCE(name, 'NULL') || ':' ||
            COALESCE(price::text, 'NULL') || ':' ||
            COALESCE(quantity::text, 'NULL') || ':' ||
            COALESCE(description, 'NULL'),
            '|' ORDER BY id
        ),
        ''
    )
) AS hash_a_final FROM products \gset

\echo [INFO] (:testid) Database A final hash: :hash_a_final

-- Row count on A
SELECT COUNT(*) AS count_a_final FROM products \gset

-- Verify new row from B exists in A
SELECT COUNT(*) = 1 AS new_row_b_ok
FROM products
WHERE id = '66666666-6666-6666-6666-666666666666'
  AND name = 'New Product B'
  AND price = 66.66
  AND quantity = 666
  AND description = 'Added after alter on B' \gset

-- Verify updated row from B synced to A
SELECT COUNT(*) = 1 AS updated_row_b_ok
FROM products
WHERE id = '33333333-3333-3333-3333-333333333333'
  AND description = 'Updated on B'
  AND quantity = 350 \gset

\connect cloudsync_test_31b
\ir helper_psql_conn_setup.sql

-- Compute hash of Database B
SELECT md5(
    COALESCE(
        string_agg(
            id::text || ':' ||
            COALESCE(name, 'NULL') || ':' ||
            COALESCE(price::text, 'NULL') || ':' ||
            COALESCE(quantity::text, 'NULL') || ':' ||
            COALESCE(description, 'NULL'),
            '|' ORDER BY id
        ),
        ''
    )
) AS hash_b_final FROM products \gset

\echo [INFO] (:testid) Database B final hash: :hash_b_final

-- Row count on B
SELECT COUNT(*) AS count_b_final FROM products \gset

-- Verify new row from A exists in B
SELECT COUNT(*) = 1 AS new_row_a_ok
FROM products
WHERE id = '55555555-5555-5555-5555-555555555555'
  AND name = 'New Product A'
  AND price = 55.55
  AND quantity = 555
  AND description = 'Added after alter on A' \gset

-- Verify updated row from A synced to B
SELECT COUNT(*) = 1 AS updated_row_a_ok
FROM products
WHERE id = '11111111-1111-1111-1111-111111111111'
  AND description = 'Updated on A'
  AND quantity = 150 \gset

-- Verify new column exists
SELECT COUNT(*) = 1 AS description_column_exists
FROM information_schema.columns
WHERE table_name = 'products' AND column_name = 'description' \gset

-- ============================================================================
-- Report results
-- ============================================================================

-- Compare final hashes
SELECT (:'hash_a_final' = :'hash_b_final') AS final_hashes_match \gset
\if :final_hashes_match
\echo [PASS] (:testid) Final data integrity verified - hashes match after ALTER
\else
\echo [FAIL] (:testid) Final data integrity check failed - A: :hash_a_final, B: :hash_b_final
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (:count_a_final = 6 AND :count_b_final = 6) AS row_counts_ok \gset
\if :row_counts_ok
\echo [PASS] (:testid) Row counts match (6 rows each)
\else
\echo [FAIL] (:testid) Row counts mismatch - A: :count_a_final, B: :count_b_final
SELECT (:fail::int + 1) AS fail \gset
\endif

\if :new_row_a_ok
\echo [PASS] (:testid) New row from A synced to B with new schema
\else
\echo [FAIL] (:testid) New row from A not found or incorrect in B
SELECT (:fail::int + 1) AS fail \gset
\endif

\if :new_row_b_ok
\echo [PASS] (:testid) New row from B synced to A with new schema
\else
\echo [FAIL] (:testid) New row from B not found or incorrect in A
SELECT (:fail::int + 1) AS fail \gset
\endif

\if :updated_row_a_ok
\echo [PASS] (:testid) Updated row from A synced with new column values
\else
\echo [FAIL] (:testid) Updated row from A not synced correctly
SELECT (:fail::int + 1) AS fail \gset
\endif

\if :updated_row_b_ok
\echo [PASS] (:testid) Updated row from B synced with new column values
\else
\echo [FAIL] (:testid) Updated row from B not synced correctly
SELECT (:fail::int + 1) AS fail \gset
\endif

\if :description_column_exists
\echo [PASS] (:testid) Added column 'description' exists
\else
\echo [FAIL] (:testid) Added column 'description' not found
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================================
-- Cleanup
-- ============================================================================

\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_31a;
DROP DATABASE IF EXISTS cloudsync_test_31b;
\endif
