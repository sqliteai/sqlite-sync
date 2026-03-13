-- 'Block-level LWW round 5: large blocks, payload idempotency composite PK, init with existing data, drop/re-add block config, delimiter-in-content'

\set testid '38'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

DROP DATABASE IF EXISTS cloudsync_block_r5_a;
DROP DATABASE IF EXISTS cloudsync_block_r5_b;
CREATE DATABASE cloudsync_block_r5_a;
CREATE DATABASE cloudsync_block_r5_b;

-- ============================================================
-- Test 7: Large number of blocks (200+ lines)
-- Verify diff and materialize work correctly at scale
-- ============================================================
\connect cloudsync_block_r5_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS big_docs;
CREATE TABLE big_docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('big_docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('big_docs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r5_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS big_docs;
CREATE TABLE big_docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('big_docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('big_docs', 'body', 'algo', 'block') AS _sc \gset

-- Generate 250-line text
\connect cloudsync_block_r5_a
INSERT INTO big_docs (id, body)
SELECT 'doc1', string_agg('Line-' || gs::text, E'\n' ORDER BY gs)
FROM generate_series(1, 250) gs;

SELECT count(*) AS big_blocks FROM big_docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1') \gset
SELECT (:'big_blocks'::int = 250) AS big_blk_ok \gset
\if :big_blk_ok
\echo [PASS] (:testid) LargeBlocks: 250 blocks created
\else
\echo [FAIL] (:testid) LargeBlocks: expected 250 blocks, got :big_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Edit a few lines scattered through the document
UPDATE big_docs SET body = (
    SELECT string_agg(
        CASE
            WHEN gs = 50 THEN 'EDITED-50'
            WHEN gs = 150 THEN 'EDITED-150'
            WHEN gs = 200 THEN 'EDITED-200'
            ELSE 'Line-' || gs::text
        END,
        E'\n' ORDER BY gs
    ) FROM generate_series(1, 250) gs
) WHERE id = 'doc1';

-- Sync A -> B
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_big
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'big_docs' \gset

\connect cloudsync_block_r5_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_big', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('big_docs', 'body', 'doc1') AS _mat \gset

-- Verify edited lines are present
SELECT (position('EDITED-50' in body) > 0) AS big_e50 FROM big_docs WHERE id = 'doc1' \gset
SELECT (position('EDITED-150' in body) > 0) AS big_e150 FROM big_docs WHERE id = 'doc1' \gset
SELECT (position('EDITED-200' in body) > 0) AS big_e200 FROM big_docs WHERE id = 'doc1' \gset

\if :big_e50
\echo [PASS] (:testid) LargeBlocks: EDITED-50 present
\else
\echo [FAIL] (:testid) LargeBlocks: EDITED-50 missing
SELECT (:fail::int + 1) AS fail \gset
\endif

\if :big_e150
\echo [PASS] (:testid) LargeBlocks: EDITED-150 present
\else
\echo [FAIL] (:testid) LargeBlocks: EDITED-150 missing
SELECT (:fail::int + 1) AS fail \gset
\endif

\if :big_e200
\echo [PASS] (:testid) LargeBlocks: EDITED-200 present
\else
\echo [FAIL] (:testid) LargeBlocks: EDITED-200 missing
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Verify block count still 250 (edits don't change count)
SELECT count(*) AS big_blocks2 FROM big_docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1') \gset
SELECT (:'big_blocks2'::int = 250) AS big_cnt_ok \gset
\if :big_cnt_ok
\echo [PASS] (:testid) LargeBlocks: block count stable after sync
\else
\echo [FAIL] (:testid) LargeBlocks: expected 250 blocks after sync, got :big_blocks2
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 8: Payload idempotency with composite PK
-- Apply same payload twice, verify no duplication or corruption
-- ============================================================
\connect cloudsync_block_r5_a
DROP TABLE IF EXISTS idem_docs;
CREATE TABLE idem_docs (owner TEXT NOT NULL, seq INTEGER NOT NULL, body TEXT, PRIMARY KEY(owner, seq));
SELECT cloudsync_init('idem_docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('idem_docs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r5_b
DROP TABLE IF EXISTS idem_docs;
CREATE TABLE idem_docs (owner TEXT NOT NULL, seq INTEGER NOT NULL, body TEXT, PRIMARY KEY(owner, seq));
SELECT cloudsync_init('idem_docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('idem_docs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r5_a
INSERT INTO idem_docs (owner, seq, body) VALUES ('bob', 1, E'Idem-Line1\nIdem-Line2\nIdem-Line3');
UPDATE idem_docs SET body = E'Idem-Line1\nIdem-Edited\nIdem-Line3' WHERE owner = 'bob' AND seq = 1;

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_idem
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'idem_docs' \gset

-- Apply on B — first time
\connect cloudsync_block_r5_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_idem', 'hex')) AS _app1 \gset
SELECT cloudsync_text_materialize('idem_docs', 'body', 'bob', 1) AS _mat1 \gset

SELECT (body = E'Idem-Line1\nIdem-Edited\nIdem-Line3') AS idem1_ok FROM idem_docs WHERE owner = 'bob' AND seq = 1 \gset
\if :idem1_ok
\echo [PASS] (:testid) Idempotent: first apply correct
\else
\echo [FAIL] (:testid) Idempotent: first apply mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT count(*) AS idem_meta1 FROM idem_docs_cloudsync WHERE pk = cloudsync_pk_encode('bob', 1) \gset

-- Apply SAME payload again — second time (idempotent)
SELECT cloudsync_payload_apply(decode(:'payload_idem', 'hex')) AS _app2 \gset
SELECT cloudsync_text_materialize('idem_docs', 'body', 'bob', 1) AS _mat2 \gset

SELECT (body = E'Idem-Line1\nIdem-Edited\nIdem-Line3') AS idem2_ok FROM idem_docs WHERE owner = 'bob' AND seq = 1 \gset
\if :idem2_ok
\echo [PASS] (:testid) Idempotent: second apply still correct
\else
\echo [FAIL] (:testid) Idempotent: body corrupted after double apply
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Metadata count should not change
SELECT count(*) AS idem_meta2 FROM idem_docs_cloudsync WHERE pk = cloudsync_pk_encode('bob', 1) \gset
SELECT (:'idem_meta1' = :'idem_meta2') AS idem_meta_ok \gset
\if :idem_meta_ok
\echo [PASS] (:testid) Idempotent: metadata count unchanged after double apply
\else
\echo [FAIL] (:testid) Idempotent: metadata count changed (:idem_meta1 vs :idem_meta2)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 9: Init with pre-existing data, then enable block column
-- Table has rows before cloudsync_set_column algo=block
-- ============================================================
\connect cloudsync_block_r5_a
DROP TABLE IF EXISTS predata;
CREATE TABLE predata (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('predata', 'CLS', true) AS _init \gset

-- Insert rows BEFORE enabling block algorithm
INSERT INTO predata (id, body) VALUES ('pre1', E'Pre-Line1\nPre-Line2');
INSERT INTO predata (id, body) VALUES ('pre2', E'Pre-Alpha\nPre-Beta\nPre-Gamma');

-- Now enable block on the column
SELECT cloudsync_set_column('predata', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r5_b
DROP TABLE IF EXISTS predata;
CREATE TABLE predata (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('predata', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('predata', 'body', 'algo', 'block') AS _sc \gset

-- Update a pre-existing row on A to trigger block creation
\connect cloudsync_block_r5_a
UPDATE predata SET body = E'Pre-Line1\nPre-Edited2' WHERE id = 'pre1';

SELECT count(*) AS pre_blocks FROM predata_cloudsync_blocks WHERE pk = cloudsync_pk_encode('pre1') \gset
SELECT (:'pre_blocks'::int >= 2) AS pre_blk_ok \gset
\if :pre_blk_ok
\echo [PASS] (:testid) PreExisting: blocks created after update
\else
\echo [FAIL] (:testid) PreExisting: expected >= 2 blocks, got :pre_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Sync to B
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_pre
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'predata' \gset

\connect cloudsync_block_r5_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_pre', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('predata', 'body', 'pre1') AS _mat \gset

SELECT (body = E'Pre-Line1\nPre-Edited2') AS pre_sync_ok FROM predata WHERE id = 'pre1' \gset
\if :pre_sync_ok
\echo [PASS] (:testid) PreExisting: synced body matches after late block enable
\else
\echo [FAIL] (:testid) PreExisting: synced body mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- pre2 should also sync (as regular LWW or with insert sentinel)
SELECT (count(*) = 1) AS pre2_exists FROM predata WHERE id = 'pre2' \gset
\if :pre2_exists
\echo [PASS] (:testid) PreExisting: pre2 row synced
\else
\echo [FAIL] (:testid) PreExisting: pre2 row missing
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 10: Remove block algo then re-add
-- ============================================================
\connect cloudsync_block_r5_a
DROP TABLE IF EXISTS toggle_docs;
CREATE TABLE toggle_docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('toggle_docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('toggle_docs', 'body', 'algo', 'block') AS _sc1 \gset

\connect cloudsync_block_r5_b
DROP TABLE IF EXISTS toggle_docs;
CREATE TABLE toggle_docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('toggle_docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('toggle_docs', 'body', 'algo', 'block') AS _sc1 \gset

-- Insert with blocks on A
\connect cloudsync_block_r5_a
INSERT INTO toggle_docs (id, body) VALUES ('doc1', E'Toggle-Line1\nToggle-Line2');

SELECT count(*) AS tog_blocks1 FROM toggle_docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1') \gset
SELECT (:'tog_blocks1'::int = 2) AS tog_blk1_ok \gset
\if :tog_blk1_ok
\echo [PASS] (:testid) Toggle: blocks created initially
\else
\echo [FAIL] (:testid) Toggle: expected 2 blocks initially, got :tog_blocks1
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Remove block algo (set to default LWW)
SELECT cloudsync_set_column('toggle_docs', 'body', 'algo', 'lww') AS _sc2 \gset

-- Update while in LWW mode — should NOT create new blocks
UPDATE toggle_docs SET body = E'Toggle-LWW-Updated' WHERE id = 'doc1';

-- Re-enable block algo
SELECT cloudsync_set_column('toggle_docs', 'body', 'algo', 'block') AS _sc3 \gset

-- Update with blocks re-enabled
UPDATE toggle_docs SET body = E'Toggle-Block-Again1\nToggle-Block-Again2\nToggle-Block-Again3' WHERE id = 'doc1';

SELECT count(*) AS tog_blocks2 FROM toggle_docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1') \gset
SELECT (:'tog_blocks2'::int = 3) AS tog_blk2_ok \gset
\if :tog_blk2_ok
\echo [PASS] (:testid) Toggle: 3 blocks after re-enable
\else
\echo [FAIL] (:testid) Toggle: expected 3 blocks after re-enable, got :tog_blocks2
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Sync to B
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_tog
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'toggle_docs' \gset

\connect cloudsync_block_r5_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_tog', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('toggle_docs', 'body', 'doc1') AS _mat \gset

SELECT (body = E'Toggle-Block-Again1\nToggle-Block-Again2\nToggle-Block-Again3') AS tog_sync_ok FROM toggle_docs WHERE id = 'doc1' \gset
\if :tog_sync_ok
\echo [PASS] (:testid) Toggle: body matches after re-enable and sync
\else
\echo [FAIL] (:testid) Toggle: body mismatch after re-enable and sync
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 11: Text containing the delimiter character as content
-- Default delimiter is \n — content has no real structure, just embedded newlines
-- ============================================================
\connect cloudsync_block_r5_a
DROP TABLE IF EXISTS delim_docs;
CREATE TABLE delim_docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('delim_docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('delim_docs', 'body', 'algo', 'block') AS _sc \gset
-- Use paragraph delimiter (double newline)
SELECT cloudsync_set_column('delim_docs', 'body', 'delimiter', E'\n\n') AS _sd \gset

\connect cloudsync_block_r5_b
DROP TABLE IF EXISTS delim_docs;
CREATE TABLE delim_docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('delim_docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('delim_docs', 'body', 'algo', 'block') AS _sc \gset
SELECT cloudsync_set_column('delim_docs', 'body', 'delimiter', E'\n\n') AS _sd \gset

-- Content with single newlines inside paragraphs (not delimiters)
\connect cloudsync_block_r5_a
INSERT INTO delim_docs (id, body) VALUES ('doc1', E'Paragraph one\nstill paragraph one.\n\nParagraph two\nstill para two.\n\nParagraph three.');

-- Should be 3 blocks (split by double newline)
SELECT count(*) AS dc_blocks FROM delim_docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1') \gset
SELECT (:'dc_blocks'::int = 3) AS dc_blk_ok \gset
\if :dc_blk_ok
\echo [PASS] (:testid) DelimContent: 3 paragraph blocks (single newlines inside)
\else
\echo [FAIL] (:testid) DelimContent: expected 3 blocks, got :dc_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Sync A -> B
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_dc
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'delim_docs' \gset

\connect cloudsync_block_r5_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_dc', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('delim_docs', 'body', 'doc1') AS _mat \gset

SELECT (body = E'Paragraph one\nstill paragraph one.\n\nParagraph two\nstill para two.\n\nParagraph three.') AS dc_sync_ok FROM delim_docs WHERE id = 'doc1' \gset
\if :dc_sync_ok
\echo [PASS] (:testid) DelimContent: body matches on B (embedded newlines preserved)
\else
\echo [FAIL] (:testid) DelimContent: body mismatch on B
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Edit paragraph 2 on B (change only the second paragraph), sync back
\connect cloudsync_block_r5_b
UPDATE delim_docs SET body = E'Paragraph one\nstill paragraph one.\n\nEdited paragraph two.\n\nParagraph three.' WHERE id = 'doc1';

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_dc_r
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'delim_docs' \gset

\connect cloudsync_block_r5_a
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_dc_r', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('delim_docs', 'body', 'doc1') AS _mat \gset

SELECT (body = E'Paragraph one\nstill paragraph one.\n\nEdited paragraph two.\n\nParagraph three.') AS dc_rev_ok FROM delim_docs WHERE id = 'doc1' \gset
\if :dc_rev_ok
\echo [PASS] (:testid) DelimContent: reverse sync matches (paragraph edit)
\else
\echo [FAIL] (:testid) DelimContent: reverse sync mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Concurrent edit: A edits para 1, B edits para 3
\connect cloudsync_block_r5_a
UPDATE delim_docs SET body = E'Edited para one by A.\n\nEdited paragraph two.\n\nParagraph three.' WHERE id = 'doc1';

\connect cloudsync_block_r5_b
UPDATE delim_docs SET body = E'Paragraph one\nstill paragraph one.\n\nEdited paragraph two.\n\nEdited para three by B.' WHERE id = 'doc1';

\connect cloudsync_block_r5_a
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_dc_a
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'delim_docs' \gset

\connect cloudsync_block_r5_b
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_dc_b
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'delim_docs' \gset

-- Apply cross
\connect cloudsync_block_r5_a
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_dc_b', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('delim_docs', 'body', 'doc1') AS _mat \gset

\connect cloudsync_block_r5_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_dc_a', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('delim_docs', 'body', 'doc1') AS _mat \gset

-- Both should converge and both edits should be present
\connect cloudsync_block_r5_a
SELECT md5(body) AS dc_md5_a FROM delim_docs WHERE id = 'doc1' \gset
\connect cloudsync_block_r5_b
SELECT md5(body) AS dc_md5_b FROM delim_docs WHERE id = 'doc1' \gset

SELECT (:'dc_md5_a' = :'dc_md5_b') AS dc_converge \gset
\if :dc_converge
\echo [PASS] (:testid) DelimContent: concurrent paragraph edits converge
\else
\echo [FAIL] (:testid) DelimContent: concurrent paragraph edits diverged
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect cloudsync_block_r5_a
SELECT (position('Edited para one by A.' in body) > 0) AS dc_has_a FROM delim_docs WHERE id = 'doc1' \gset
SELECT (position('Edited para three by B.' in body) > 0) AS dc_has_b FROM delim_docs WHERE id = 'doc1' \gset

\if :dc_has_a
\echo [PASS] (:testid) DelimContent: site A paragraph edit present
\else
\echo [FAIL] (:testid) DelimContent: site A paragraph edit missing
SELECT (:fail::int + 1) AS fail \gset
\endif

\if :dc_has_b
\echo [PASS] (:testid) DelimContent: site B paragraph edit present
\else
\echo [FAIL] (:testid) DelimContent: site B paragraph edit missing
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Cleanup
-- ============================================================
\ir helper_test_cleanup.sql
\if :should_cleanup
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_block_r5_a;
DROP DATABASE IF EXISTS cloudsync_block_r5_b;
\else
\echo [INFO] !!!!!
\endif
