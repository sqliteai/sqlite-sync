-- 'Block-level LWW extended tests: DELETE, empty text, multi-update, conflict'

\set testid '33'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

DROP DATABASE IF EXISTS cloudsync_block_ext_a;
DROP DATABASE IF EXISTS cloudsync_block_ext_b;
CREATE DATABASE cloudsync_block_ext_a;
CREATE DATABASE cloudsync_block_ext_b;

-- ============================================================
-- Setup db A
-- ============================================================
\connect cloudsync_block_ext_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS docs;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('docs', 'CLS', true) AS _init_a \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol_a \gset

-- ============================================================
-- Test 1: DELETE marks tombstone, block metadata dropped
-- ============================================================
INSERT INTO docs (id, body) VALUES ('doc1', 'line1
line2
line3');

-- Verify 3 block metadata entries exist
SELECT count(*) AS meta_before FROM docs_cloudsync WHERE col_name LIKE 'body' || chr(31) || '%' \gset
SELECT (:meta_before::int = 3) AS meta_before_ok \gset
\if :meta_before_ok
\echo [PASS] (:testid) Delete pre-check: 3 block metadata entries
\else
\echo [FAIL] (:testid) Delete pre-check: expected 3 metadata, got :meta_before
SELECT (:fail::int + 1) AS fail \gset
\endif

DELETE FROM docs WHERE id = 'doc1';

-- Tombstone should exist with even version (deleted)
SELECT count(*) AS tombstone_count FROM docs_cloudsync WHERE col_name = '__[RIP]__' AND col_version % 2 = 0 \gset
SELECT (:tombstone_count::int = 1) AS tombstone_ok \gset
\if :tombstone_ok
\echo [PASS] (:testid) Delete: tombstone exists with even version
\else
\echo [FAIL] (:testid) Delete: expected 1 tombstone, got :tombstone_count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Block metadata should be dropped
SELECT count(*) AS meta_after FROM docs_cloudsync WHERE col_name LIKE 'body' || chr(31) || '%' \gset
SELECT (:meta_after::int = 0) AS meta_dropped_ok \gset
\if :meta_dropped_ok
\echo [PASS] (:testid) Delete: block metadata dropped
\else
\echo [FAIL] (:testid) Delete: expected 0 metadata after delete, got :meta_after
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Row should be gone from base table
SELECT count(*) AS row_after FROM docs WHERE id = 'doc1' \gset
SELECT (:row_after::int = 0) AS row_gone_ok \gset
\if :row_gone_ok
\echo [PASS] (:testid) Delete: row removed from base table
\else
\echo [FAIL] (:testid) Delete: row still in base table
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 2: Empty text creates single block
-- ============================================================
INSERT INTO docs (id, body) VALUES ('doc_empty', '');

SELECT count(*) AS empty_blocks FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc_empty') \gset
SELECT (:empty_blocks::int = 1) AS empty_block_ok \gset
\if :empty_block_ok
\echo [PASS] (:testid) Empty text: 1 block created
\else
\echo [FAIL] (:testid) Empty text: expected 1 block, got :empty_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Update from empty to multi-line
UPDATE docs SET body = 'NewLine1
NewLine2' WHERE id = 'doc_empty';

SELECT count(*) AS updated_blocks FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc_empty') \gset
SELECT (:updated_blocks::int = 2) AS update_from_empty_ok \gset
\if :update_from_empty_ok
\echo [PASS] (:testid) Empty text: 2 blocks after update
\else
\echo [FAIL] (:testid) Empty text: expected 2 blocks after update, got :updated_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 3: Multi-update block counts
-- ============================================================
INSERT INTO docs (id, body) VALUES ('doc_multi', 'A
B
C');

-- Update 1: remove middle line
UPDATE docs SET body = 'A
C' WHERE id = 'doc_multi';

SELECT count(*) AS blocks1 FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc_multi') \gset
SELECT (:blocks1::int = 2) AS multi1_ok \gset
\if :multi1_ok
\echo [PASS] (:testid) Multi-update: 2 blocks after removing middle
\else
\echo [FAIL] (:testid) Multi-update: expected 2, got :blocks1
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Update 2: add two lines
UPDATE docs SET body = 'A
X
C
Y' WHERE id = 'doc_multi';

SELECT count(*) AS blocks2 FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc_multi') \gset
SELECT (:blocks2::int = 4) AS multi2_ok \gset
\if :multi2_ok
\echo [PASS] (:testid) Multi-update: 4 blocks after adding lines
\else
\echo [FAIL] (:testid) Multi-update: expected 4, got :blocks2
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Update 3: collapse to single line
UPDATE docs SET body = 'SINGLE' WHERE id = 'doc_multi';

SELECT count(*) AS blocks3 FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc_multi') \gset
SELECT (:blocks3::int = 1) AS multi3_ok \gset
\if :multi3_ok
\echo [PASS] (:testid) Multi-update: 1 block after collapse
\else
\echo [FAIL] (:testid) Multi-update: expected 1, got :blocks3
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Materialize and verify
SELECT cloudsync_text_materialize('docs', 'body', 'doc_multi') AS _mat_multi \gset
SELECT body AS multi_body FROM docs WHERE id = 'doc_multi' \gset
SELECT (:'multi_body' = 'SINGLE') AS multi_mat_ok \gset
\if :multi_mat_ok
\echo [PASS] (:testid) Multi-update: materialize matches
\else
\echo [FAIL] (:testid) Multi-update: materialize mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 4: Two-database conflict on same block
-- ============================================================

-- Setup db B
\connect cloudsync_block_ext_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS docs;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('docs', 'CLS', true) AS _init_b \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol_b \gset

-- Insert initial doc on db A
\connect cloudsync_block_ext_a
INSERT INTO docs (id, body) VALUES ('doc_conflict', 'Same
Middle
End');

-- Sync A -> B (round 1)
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_a_r1
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_block_ext_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_a_r1', 'hex')) AS _apply_b_r1 \gset

-- Materialize on B to get body
SELECT cloudsync_text_materialize('docs', 'body', 'doc_conflict') AS _mat_b_init \gset

-- Verify B has the initial doc
SELECT body AS body_b_init FROM docs WHERE id = 'doc_conflict' \gset
SELECT (:'body_b_init' = 'Same
Middle
End') AS init_sync_ok \gset
\if :init_sync_ok
\echo [PASS] (:testid) Conflict: initial sync to B matches
\else
\echo [FAIL] (:testid) Conflict: initial sync to B mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Site A edits first line
\connect cloudsync_block_ext_a
UPDATE docs SET body = 'SiteA
Middle
End' WHERE id = 'doc_conflict';

-- Site B edits first line (conflict!)
\connect cloudsync_block_ext_b
UPDATE docs SET body = 'SiteB
Middle
End' WHERE id = 'doc_conflict';

-- Collect payloads from both sites
\connect cloudsync_block_ext_a
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_a_r2
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_block_ext_b
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_b_r2
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

-- Apply A's changes to B
SELECT cloudsync_payload_apply(decode(:'payload_a_r2', 'hex')) AS _apply_b_r2 \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc_conflict') AS _mat_b_r2 \gset

-- Apply B's changes to A
\connect cloudsync_block_ext_a
SELECT cloudsync_payload_apply(decode(:'payload_b_r2', 'hex')) AS _apply_a_r2 \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc_conflict') AS _mat_a_r2 \gset

-- Both should converge
SELECT body AS body_a_final FROM docs WHERE id = 'doc_conflict' \gset

\connect cloudsync_block_ext_b
SELECT body AS body_b_final FROM docs WHERE id = 'doc_conflict' \gset

-- Bodies must match (convergence)
SELECT (:'body_a_final' = :'body_b_final') AS converge_ok \gset
\if :converge_ok
\echo [PASS] (:testid) Conflict: databases converge after sync
\else
\echo [FAIL] (:testid) Conflict: databases diverged
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Unchanged lines must be preserved
SELECT (position('Middle' in :'body_a_final') > 0) AS has_middle \gset
\if :has_middle
\echo [PASS] (:testid) Conflict: unchanged line 'Middle' preserved
\else
\echo [FAIL] (:testid) Conflict: 'Middle' missing from result
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (position('End' in :'body_a_final') > 0) AS has_end \gset
\if :has_end
\echo [PASS] (:testid) Conflict: unchanged line 'End' preserved
\else
\echo [FAIL] (:testid) Conflict: 'End' missing from result
SELECT (:fail::int + 1) AS fail \gset
\endif

-- One of the conflicting edits must win
SELECT (position('SiteA' in :'body_a_final') > 0 OR position('SiteB' in :'body_a_final') > 0) AS has_winner \gset
\if :has_winner
\echo [PASS] (:testid) Conflict: one site edit won (LWW)
\else
\echo [FAIL] (:testid) Conflict: neither SiteA nor SiteB in result
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 5: DELETE then re-INSERT (reinsert)
-- ============================================================
\connect cloudsync_block_ext_a

INSERT INTO docs (id, body) VALUES ('doc_reinsert', 'Old1
Old2');
DELETE FROM docs WHERE id = 'doc_reinsert';

-- Block metadata should be dropped after delete
SELECT count(*) AS meta_reinsert_del FROM docs_cloudsync
WHERE pk = cloudsync_pk_encode('doc_reinsert')
AND col_name LIKE 'body' || chr(31) || '%' \gset
SELECT (:meta_reinsert_del::int = 0) AS reinsert_meta_del_ok \gset
\if :reinsert_meta_del_ok
\echo [PASS] (:testid) Reinsert: metadata dropped after delete
\else
\echo [FAIL] (:testid) Reinsert: expected 0 metadata, got :meta_reinsert_del
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Re-insert with new content
INSERT INTO docs (id, body) VALUES ('doc_reinsert', 'New1
New2
New3');

SELECT count(*) AS meta_reinsert_new FROM docs_cloudsync
WHERE pk = cloudsync_pk_encode('doc_reinsert')
AND col_name LIKE 'body' || chr(31) || '%' \gset
SELECT (:meta_reinsert_new::int = 3) AS reinsert_meta_ok \gset
\if :reinsert_meta_ok
\echo [PASS] (:testid) Reinsert: 3 block metadata after re-insert
\else
\echo [FAIL] (:testid) Reinsert: expected 3 metadata, got :meta_reinsert_new
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Sync to B and materialize
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_reinsert
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_block_ext_b
SELECT cloudsync_payload_apply(decode(:'payload_reinsert', 'hex')) AS _apply_reinsert \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc_reinsert') AS _mat_reinsert \gset
SELECT body AS body_reinsert FROM docs WHERE id = 'doc_reinsert' \gset

SELECT (:'body_reinsert' = 'New1
New2
New3') AS reinsert_sync_ok \gset
\if :reinsert_sync_ok
\echo [PASS] (:testid) Reinsert: sync roundtrip matches
\else
\echo [FAIL] (:testid) Reinsert: sync mismatch on db B
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_block_ext_a;
DROP DATABASE IF EXISTS cloudsync_block_ext_b;
\else
\echo [INFO] !!!!!
\endif
