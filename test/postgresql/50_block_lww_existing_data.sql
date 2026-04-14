-- 'Block-level LWW: migration of existing tracked rows when algo=block is enabled'
-- Mirrors the SQLite unit test: Block LWW Existing Data

\set testid '50'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

DROP DATABASE IF EXISTS cloudsync_block_existing_a;
CREATE DATABASE cloudsync_block_existing_a;

\connect cloudsync_block_existing_a
\ir helper_psql_conn_setup.sql

CREATE EXTENSION IF NOT EXISTS cloudsync;

-- Create a table and init cloudsync WITHOUT block algo first
DROP TABLE IF EXISTS docs;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('docs', 'CLS', 1) AS _init \gset

-- Insert rows BEFORE enabling block algorithm (they will be tracked as regular CLS rows)
INSERT INTO docs (id, body) VALUES ('d1', E'Line1\nLine2\nLine3');
INSERT INTO docs (id, body) VALUES ('d2', E'Alpha\nBeta');

-- Now enable block algo on the column that already has data
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _sc \gset

-- Test 1: Blocks table should have 5 entries (3 for d1, 2 for d2) immediately after set_column
SELECT count(*) AS block_count FROM docs_cloudsync_blocks \gset
SELECT (:block_count::int = 5) AS block_count_ok \gset
\if :block_count_ok
\echo [PASS] (:testid) Migration: 5 block entries after set_column on existing data
\else
\echo [FAIL] (:testid) Migration: expected 5 block entries, got :block_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 2: Metadata should have 5 alive block entries
SELECT count(*) AS meta_count FROM docs_cloudsync
WHERE col_name LIKE 'body' || chr(31) || '%' AND col_version % 2 = 1 \gset
SELECT (:meta_count::int = 5) AS meta_count_ok \gset
\if :meta_count_ok
\echo [PASS] (:testid) Migration: 5 alive block metadata entries
\else
\echo [FAIL] (:testid) Migration: expected 5 alive metadata entries, got :meta_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 3: Calling set_column again should be idempotent (count stays at 5)
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _sc2 \gset

SELECT count(*) AS block_count2 FROM docs_cloudsync_blocks \gset
SELECT (:block_count2::int = 5) AS idempotent_ok \gset
\if :idempotent_ok
\echo [PASS] (:testid) Migration: idempotent (still 5 blocks after second set_column)
\else
\echo [FAIL] (:testid) Migration: idempotency broken, got :block_count2 blocks (expected 5)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 4: UPDATE on d1 should still work correctly after migration
UPDATE docs SET body = E'Line1\nLine2-edited\nLine3' WHERE id = 'd1';

SELECT count(*) AS block_count3 FROM docs_cloudsync_blocks \gset
SELECT (:block_count3::int = 5) AS update_count_ok \gset
\if :update_count_ok
\echo [PASS] (:testid) Migration: 5 blocks after UPDATE (d1 edited in-place)
\else
\echo [FAIL] (:testid) Migration: expected 5 blocks after update, got :block_count3
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 5: Materialized value should reflect the update
SELECT cloudsync_text_materialize('docs', 'body', 'd1') AS _mat \gset

SELECT (body = E'Line1\nLine2-edited\nLine3') AS mat_ok FROM docs WHERE id = 'd1' \gset
\if :mat_ok
\echo [PASS] (:testid) Migration: materialized value correct after update
\else
\echo [FAIL] (:testid) Migration: materialized value mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_block_existing_a;
\else
\echo [INFO] !!!!!
\endif
