-- 'Block-level LWW test'

\set testid '32'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

DROP DATABASE IF EXISTS cloudsync_block_test_a;
CREATE DATABASE cloudsync_block_test_a;

\connect cloudsync_block_test_a
\ir helper_psql_conn_setup.sql

CREATE EXTENSION IF NOT EXISTS cloudsync;

-- Create a table with a text column for block-level LWW
DROP TABLE IF EXISTS docs;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);

-- Initialize cloudsync for the table
SELECT cloudsync_init('docs', 'CLS', true) AS _init \gset

-- Configure body column as block-level
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol \gset

-- Test 1: INSERT text, verify blocks table populated
INSERT INTO docs (id, body) VALUES ('doc1', 'line1
line2
line3');

-- Verify blocks table was created
SELECT EXISTS(SELECT 1 FROM information_schema.tables WHERE table_name = 'docs_cloudsync_blocks') AS blocks_table_exists \gset
\if :blocks_table_exists
\echo [PASS] (:testid) Blocks table created
\else
\echo [FAIL] (:testid) Blocks table not created
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify blocks have been stored (3 lines = 3 blocks)
SELECT count(*) AS block_count FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1') \gset
SELECT (:block_count::int = 3) AS insert_blocks_ok \gset
\if :insert_blocks_ok
\echo [PASS] (:testid) Block insert: 3 blocks created
\else
\echo [FAIL] (:testid) Block insert: expected 3 blocks, got :block_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify metadata has block entries (col_name contains \x1F separator)
SELECT count(*) AS meta_block_count FROM docs_cloudsync WHERE col_name LIKE 'body' || chr(31) || '%' \gset
SELECT (:meta_block_count::int = 3) AS meta_blocks_ok \gset
\if :meta_blocks_ok
\echo [PASS] (:testid) Block metadata: 3 block entries in _cloudsync
\else
\echo [FAIL] (:testid) Block metadata: expected 3 entries, got :meta_block_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 2: UPDATE text (modify one line, add one line)
UPDATE docs SET body = 'line1
line2_modified
line3
line4' WHERE id = 'doc1';

-- Verify blocks updated (should now have 4 blocks)
SELECT count(*) AS block_count2 FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1') \gset
SELECT (:block_count2::int = 4) AS update_blocks_ok \gset
\if :update_blocks_ok
\echo [PASS] (:testid) Block update: 4 blocks after update
\else
\echo [FAIL] (:testid) Block update: expected 4 blocks, got :block_count2
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 3: Materialize and verify round-trip
SELECT cloudsync_text_materialize('docs', 'body', 'doc1') AS _mat \gset
SELECT body AS materialized_body FROM docs WHERE id = 'doc1' \gset

SELECT (:'materialized_body' = 'line1
line2_modified
line3
line4') AS materialize_ok \gset
\if :materialize_ok
\echo [PASS] (:testid) Text materialize: reconstructed text matches
\else
\echo [FAIL] (:testid) Text materialize: text mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 4: Verify col_value works for block entries
SELECT count(*) AS col_value_count FROM docs_cloudsync
WHERE col_name LIKE 'body' || chr(31) || '%'
AND cloudsync_col_value('docs', col_name, pk) IS NOT NULL \gset
SELECT (:col_value_count::int > 0) AS col_value_ok \gset
\if :col_value_ok
\echo [PASS] (:testid) col_value works for block entries
\else
\echo [FAIL] (:testid) col_value returned NULL for block entries
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Test 5: Sync roundtrip - encode payload from db A before disconnecting
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS block_payload_hex
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect postgres
\ir helper_psql_conn_setup.sql

DROP DATABASE IF EXISTS cloudsync_block_test_b;
CREATE DATABASE cloudsync_block_test_b;
\connect cloudsync_block_test_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS docs;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('docs', 'CLS', true) AS _init_b \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol_b \gset

SELECT cloudsync_payload_apply(decode(:'block_payload_hex', 'hex')) AS _apply_b \gset

-- Materialize on db B
SELECT cloudsync_text_materialize('docs', 'body', 'doc1') AS _mat_b \gset
SELECT body AS body_b FROM docs WHERE id = 'doc1' \gset

SELECT (:'body_b' = 'line1
line2_modified
line3
line4') AS sync_ok \gset
\if :sync_ok
\echo [PASS] (:testid) Block sync roundtrip: text matches after apply + materialize
\else
\echo [FAIL] (:testid) Block sync roundtrip: text mismatch on db B
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_block_test_a;
DROP DATABASE IF EXISTS cloudsync_block_test_b;
\else
\echo [INFO] !!!!!
\endif
