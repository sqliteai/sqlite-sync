-- 'Block-level LWW round 3: composite PK, empty vs null, delete+reinsert, integer PK, multi-row, non-overlapping add, long line, whitespace'

\set testid '36'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

DROP DATABASE IF EXISTS cloudsync_block_r3_a;
DROP DATABASE IF EXISTS cloudsync_block_r3_b;
CREATE DATABASE cloudsync_block_r3_a;
CREATE DATABASE cloudsync_block_r3_b;

-- ============================================================
-- Test 1: Composite primary key (text + int) with block column
-- ============================================================
\connect cloudsync_block_r3_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS docs;
CREATE TABLE docs (owner TEXT NOT NULL, seq INTEGER NOT NULL, body TEXT, PRIMARY KEY(owner, seq));
SELECT cloudsync_init('docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r3_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS docs;
CREATE TABLE docs (owner TEXT NOT NULL, seq INTEGER NOT NULL, body TEXT, PRIMARY KEY(owner, seq));
SELECT cloudsync_init('docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _sc \gset

-- Insert on A
\connect cloudsync_block_r3_a
INSERT INTO docs (owner, seq, body) VALUES ('alice', 1, E'Line1\nLine2\nLine3');

SELECT count(*) AS cpk_blocks FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('alice', 1) \gset
SELECT (:'cpk_blocks'::int = 3) AS cpk_blk_ok \gset
\if :cpk_blk_ok
\echo [PASS] (:testid) CompositePK: 3 blocks created
\else
\echo [FAIL] (:testid) CompositePK: expected 3 blocks, got :cpk_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Sync A -> B
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload1
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_block_r3_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload1', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('docs', 'body', 'alice', 1) AS _mat \gset

SELECT (body = E'Line1\nLine2\nLine3') AS cpk_body_ok FROM docs WHERE owner = 'alice' AND seq = 1 \gset
\if :cpk_body_ok
\echo [PASS] (:testid) CompositePK: body matches on B
\else
\echo [FAIL] (:testid) CompositePK: body mismatch on B
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Edit on B, sync back
UPDATE docs SET body = E'Line1\nEdited2\nLine3' WHERE owner = 'alice' AND seq = 1;

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload1b
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'docs' \gset

\connect cloudsync_block_r3_a
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload1b', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('docs', 'body', 'alice', 1) AS _mat \gset

SELECT (body = E'Line1\nEdited2\nLine3') AS cpk_rev_ok FROM docs WHERE owner = 'alice' AND seq = 1 \gset
\if :cpk_rev_ok
\echo [PASS] (:testid) CompositePK: reverse sync body matches
\else
\echo [FAIL] (:testid) CompositePK: reverse sync body mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 2: Empty string vs NULL
-- ============================================================
\connect cloudsync_block_r3_a
DROP TABLE IF EXISTS edocs;
CREATE TABLE edocs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('edocs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('edocs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r3_b
DROP TABLE IF EXISTS edocs;
CREATE TABLE edocs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('edocs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('edocs', 'body', 'algo', 'block') AS _sc \gset

-- Insert empty string on A
\connect cloudsync_block_r3_a
INSERT INTO edocs (id, body) VALUES ('doc1', '');

SELECT count(*) AS evn_blocks FROM edocs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1') \gset
SELECT (:'evn_blocks'::int = 1) AS evn_blk_ok \gset
\if :evn_blk_ok
\echo [PASS] (:testid) EmptyVsNull: 1 block for empty string
\else
\echo [FAIL] (:testid) EmptyVsNull: expected 1 block, got :evn_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Sync to B
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload2
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'edocs' \gset

\connect cloudsync_block_r3_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload2', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('edocs', 'body', 'doc1') AS _mat \gset

SELECT (body IS NOT NULL AND body = '') AS evn_empty_ok FROM edocs WHERE id = 'doc1' \gset
\if :evn_empty_ok
\echo [PASS] (:testid) EmptyVsNull: body is empty string (not NULL)
\else
\echo [FAIL] (:testid) EmptyVsNull: body should be empty string
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 3: DELETE row then re-insert with different content
-- ============================================================
\connect cloudsync_block_r3_a
DROP TABLE IF EXISTS rdocs;
CREATE TABLE rdocs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('rdocs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('rdocs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r3_b
DROP TABLE IF EXISTS rdocs;
CREATE TABLE rdocs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('rdocs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('rdocs', 'body', 'algo', 'block') AS _sc \gset

-- Insert and sync
\connect cloudsync_block_r3_a
INSERT INTO rdocs (id, body) VALUES ('doc1', E'Old1\nOld2\nOld3');

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload3i
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'rdocs' \gset

\connect cloudsync_block_r3_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload3i', 'hex')) AS _app \gset

-- Delete on A
\connect cloudsync_block_r3_a
DELETE FROM rdocs WHERE id = 'doc1';

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload3d
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'rdocs' \gset

\connect cloudsync_block_r3_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload3d', 'hex')) AS _app \gset

SELECT (count(*) = 0) AS dr_deleted FROM rdocs WHERE id = 'doc1' \gset
\if :dr_deleted
\echo [PASS] (:testid) DelReinsert: row deleted on B
\else
\echo [FAIL] (:testid) DelReinsert: row not deleted on B
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Re-insert with different content on A
\connect cloudsync_block_r3_a
INSERT INTO rdocs (id, body) VALUES ('doc1', E'New1\nNew2');

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload3r
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'rdocs' \gset

\connect cloudsync_block_r3_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload3r', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('rdocs', 'body', 'doc1') AS _mat \gset

SELECT (body = E'New1\nNew2') AS dr_body_ok FROM rdocs WHERE id = 'doc1' \gset
\if :dr_body_ok
\echo [PASS] (:testid) DelReinsert: body matches after re-insert
\else
\echo [FAIL] (:testid) DelReinsert: body mismatch after re-insert
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 4: INTEGER primary key with block column
-- ============================================================
\connect cloudsync_block_r3_a
DROP TABLE IF EXISTS notes;
CREATE TABLE notes (id INTEGER PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('notes', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('notes', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r3_b
DROP TABLE IF EXISTS notes;
CREATE TABLE notes (id INTEGER PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('notes', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('notes', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r3_a
INSERT INTO notes (id, body) VALUES (42, E'First\nSecond\nThird');

SELECT count(*) AS ipk_blocks FROM notes_cloudsync_blocks WHERE pk = cloudsync_pk_encode(42) \gset
SELECT (:'ipk_blocks'::int = 3) AS ipk_blk_ok \gset
\if :ipk_blk_ok
\echo [PASS] (:testid) IntegerPK: 3 blocks created
\else
\echo [FAIL] (:testid) IntegerPK: expected 3 blocks, got :ipk_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload4
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'notes' \gset

\connect cloudsync_block_r3_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload4', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('notes', 'body', 42) AS _mat \gset

SELECT (body = E'First\nSecond\nThird') AS ipk_body_ok FROM notes WHERE id = 42 \gset
\if :ipk_body_ok
\echo [PASS] (:testid) IntegerPK: body matches on B
\else
\echo [FAIL] (:testid) IntegerPK: body mismatch on B
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 5: Multiple rows with block columns in a single sync
-- ============================================================
\connect cloudsync_block_r3_a
DROP TABLE IF EXISTS mdocs;
CREATE TABLE mdocs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('mdocs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('mdocs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r3_b
DROP TABLE IF EXISTS mdocs;
CREATE TABLE mdocs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('mdocs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('mdocs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r3_a
INSERT INTO mdocs (id, body) VALUES ('r1', E'R1-Line1\nR1-Line2');
INSERT INTO mdocs (id, body) VALUES ('r2', E'R2-Alpha\nR2-Beta\nR2-Gamma');
INSERT INTO mdocs (id, body) VALUES ('r3', 'R3-Only');
UPDATE mdocs SET body = E'R1-Edited\nR1-Line2' WHERE id = 'r1';
UPDATE mdocs SET body = 'R3-Changed' WHERE id = 'r3';

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload5
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'mdocs' \gset

\connect cloudsync_block_r3_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload5', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('mdocs', 'body', 'r1') AS _m1 \gset
SELECT cloudsync_text_materialize('mdocs', 'body', 'r2') AS _m2 \gset
SELECT cloudsync_text_materialize('mdocs', 'body', 'r3') AS _m3 \gset

SELECT (body = E'R1-Edited\nR1-Line2') AS mr_r1 FROM mdocs WHERE id = 'r1' \gset
\if :mr_r1
\echo [PASS] (:testid) MultiRow: r1 matches
\else
\echo [FAIL] (:testid) MultiRow: r1 mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (body = E'R2-Alpha\nR2-Beta\nR2-Gamma') AS mr_r2 FROM mdocs WHERE id = 'r2' \gset
\if :mr_r2
\echo [PASS] (:testid) MultiRow: r2 matches
\else
\echo [FAIL] (:testid) MultiRow: r2 mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (body = 'R3-Changed') AS mr_r3 FROM mdocs WHERE id = 'r3' \gset
\if :mr_r3
\echo [PASS] (:testid) MultiRow: r3 matches
\else
\echo [FAIL] (:testid) MultiRow: r3 mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 6: Concurrent add at non-overlapping positions (top vs bottom)
-- ============================================================
\connect cloudsync_block_r3_a
DROP TABLE IF EXISTS ndocs;
CREATE TABLE ndocs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('ndocs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('ndocs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r3_b
DROP TABLE IF EXISTS ndocs;
CREATE TABLE ndocs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('ndocs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('ndocs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r3_a
INSERT INTO ndocs (id, body) VALUES ('doc1', E'A\nB\nC');

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload6i
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'ndocs' \gset

\connect cloudsync_block_r3_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload6i', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('ndocs', 'body', 'doc1') AS _mat \gset

-- A: add at top -> X A B C
\connect cloudsync_block_r3_a
UPDATE ndocs SET body = E'X\nA\nB\nC' WHERE id = 'doc1';

-- B: add at bottom -> A B C Y
\connect cloudsync_block_r3_b
UPDATE ndocs SET body = E'A\nB\nC\nY' WHERE id = 'doc1';

-- Sync A -> B
\connect cloudsync_block_r3_a
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload6a
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'ndocs' \gset

\connect cloudsync_block_r3_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload6a', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('ndocs', 'body', 'doc1') AS _mat \gset

SELECT (body LIKE '%X%') AS no_x FROM ndocs WHERE id = 'doc1' \gset
\if :no_x
\echo [PASS] (:testid) NonOverlap: X present
\else
\echo [FAIL] (:testid) NonOverlap: X missing
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (body LIKE '%Y%') AS no_y FROM ndocs WHERE id = 'doc1' \gset
\if :no_y
\echo [PASS] (:testid) NonOverlap: Y present
\else
\echo [FAIL] (:testid) NonOverlap: Y missing
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (body LIKE 'X%' OR body LIKE E'%\nX\n%') AS no_x_before FROM ndocs WHERE id = 'doc1' \gset
\if :no_x_before
\echo [PASS] (:testid) NonOverlap: X before A
\else
\echo [FAIL] (:testid) NonOverlap: X not before A
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 7: Very long single line (10K chars)
-- ============================================================
\connect cloudsync_block_r3_a
DROP TABLE IF EXISTS ldocs;
CREATE TABLE ldocs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('ldocs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('ldocs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r3_b
DROP TABLE IF EXISTS ldocs;
CREATE TABLE ldocs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('ldocs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('ldocs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r3_a
INSERT INTO ldocs (id, body) VALUES ('doc1', repeat('ABCDEFGHIJ', 1000));

SELECT count(*) AS ll_blocks FROM ldocs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1') \gset
SELECT (:'ll_blocks'::int = 1) AS ll_blk_ok \gset
\if :ll_blk_ok
\echo [PASS] (:testid) LongLine: 1 block for 10K char line
\else
\echo [FAIL] (:testid) LongLine: expected 1 block, got :ll_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload7
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'ldocs' \gset

\connect cloudsync_block_r3_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload7', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('ldocs', 'body', 'doc1') AS _mat \gset

SELECT (body = repeat('ABCDEFGHIJ', 1000)) AS ll_body_ok FROM ldocs WHERE id = 'doc1' \gset
\if :ll_body_ok
\echo [PASS] (:testid) LongLine: body matches on B
\else
\echo [FAIL] (:testid) LongLine: body mismatch on B
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 8: Whitespace and empty lines (delimiter edge cases)
-- ============================================================
\connect cloudsync_block_r3_a
DROP TABLE IF EXISTS wdocs;
CREATE TABLE wdocs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('wdocs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('wdocs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r3_b
DROP TABLE IF EXISTS wdocs;
CREATE TABLE wdocs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('wdocs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('wdocs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r3_a
-- Text: "Line1\n\n  spaces  \n\t\ttabs\n\nLine6\n" = 7 blocks
INSERT INTO wdocs (id, body) VALUES ('doc1', E'Line1\n\n  spaces  \n\t\ttabs\n\nLine6\n');

SELECT count(*) AS ws_blocks FROM wdocs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1') \gset
SELECT (:'ws_blocks'::int = 7) AS ws_blk_ok \gset
\if :ws_blk_ok
\echo [PASS] (:testid) Whitespace: 7 blocks with empty/whitespace lines
\else
\echo [FAIL] (:testid) Whitespace: expected 7 blocks, got :ws_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload8
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'wdocs' \gset

\connect cloudsync_block_r3_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload8', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('wdocs', 'body', 'doc1') AS _mat \gset

SELECT (body = E'Line1\n\n  spaces  \n\t\ttabs\n\nLine6\n') AS ws_body_ok FROM wdocs WHERE id = 'doc1' \gset
\if :ws_body_ok
\echo [PASS] (:testid) Whitespace: body matches with whitespace preserved
\else
\echo [FAIL] (:testid) Whitespace: body mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Edit: remove empty lines
\connect cloudsync_block_r3_a
UPDATE wdocs SET body = E'Line1\n  spaces  \n\t\ttabs\nLine6' WHERE id = 'doc1';

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload8b
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'wdocs' \gset

\connect cloudsync_block_r3_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload8b', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('wdocs', 'body', 'doc1') AS _mat \gset

SELECT (body = E'Line1\n  spaces  \n\t\ttabs\nLine6') AS ws_edit_ok FROM wdocs WHERE id = 'doc1' \gset
\if :ws_edit_ok
\echo [PASS] (:testid) Whitespace: edited body matches
\else
\echo [FAIL] (:testid) Whitespace: edited body mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Cleanup
-- ============================================================
\ir helper_test_cleanup.sql
\if :should_cleanup
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_block_r3_a;
DROP DATABASE IF EXISTS cloudsync_block_r3_b;
\else
\echo [INFO] !!!!!
\endif
