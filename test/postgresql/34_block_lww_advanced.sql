-- 'Block-level LWW advanced tests: noconflict, add+edit, three-way, mixed cols, NULL->text, interleaved, custom delimiter, large text, rapid updates'

\set testid '34'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

DROP DATABASE IF EXISTS cloudsync_block_adv_a;
DROP DATABASE IF EXISTS cloudsync_block_adv_b;
DROP DATABASE IF EXISTS cloudsync_block_adv_c;
CREATE DATABASE cloudsync_block_adv_a;
CREATE DATABASE cloudsync_block_adv_b;
CREATE DATABASE cloudsync_block_adv_c;

-- ============================================================
-- Test 1: Non-conflicting edits on different blocks
-- Site A edits line 1, Site B edits line 3 — BOTH should survive
-- ============================================================
\connect cloudsync_block_adv_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS docs;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('docs', 'CLS', true) AS _init_a \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol_a \gset

\connect cloudsync_block_adv_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS docs;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('docs', 'CLS', true) AS _init_b \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol_b \gset

-- Insert initial on A
\connect cloudsync_block_adv_a
INSERT INTO docs (id, body) VALUES ('doc1', 'Line1
Line2
Line3');

-- Sync A -> B
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_init
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_block_adv_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_init', 'hex')) AS _apply_init \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc1') AS _mat_init \gset

-- Site A: edit first line
\connect cloudsync_block_adv_a
UPDATE docs SET body = 'EditedByA
Line2
Line3' WHERE id = 'doc1';

-- Site B: edit third line (no conflict — different block)
\connect cloudsync_block_adv_b
UPDATE docs SET body = 'Line1
Line2
EditedByB' WHERE id = 'doc1';

-- Collect payloads
\connect cloudsync_block_adv_a
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_a
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_block_adv_b
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_b
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

-- Apply A -> B, B -> A
SELECT cloudsync_payload_apply(decode(:'payload_a', 'hex')) AS _apply_ab \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc1') AS _mat_b \gset

\connect cloudsync_block_adv_a
SELECT cloudsync_payload_apply(decode(:'payload_b', 'hex')) AS _apply_ba \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc1') AS _mat_a \gset

-- Both should converge
SELECT body AS body_a FROM docs WHERE id = 'doc1' \gset
\connect cloudsync_block_adv_b
SELECT body AS body_b FROM docs WHERE id = 'doc1' \gset

SELECT (:'body_a' = :'body_b') AS converge_ok \gset
\if :converge_ok
\echo [PASS] (:testid) NoConflict: databases converge
\else
\echo [FAIL] (:testid) NoConflict: databases diverged
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Both edits should be preserved
SELECT (position('EditedByA' in :'body_a') > 0) AS has_a_edit \gset
\if :has_a_edit
\echo [PASS] (:testid) NoConflict: Site A edit preserved
\else
\echo [FAIL] (:testid) NoConflict: Site A edit missing
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (position('EditedByB' in :'body_a') > 0) AS has_b_edit \gset
\if :has_b_edit
\echo [PASS] (:testid) NoConflict: Site B edit preserved
\else
\echo [FAIL] (:testid) NoConflict: Site B edit missing
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (position('Line2' in :'body_a') > 0) AS has_middle \gset
\if :has_middle
\echo [PASS] (:testid) NoConflict: unchanged line preserved
\else
\echo [FAIL] (:testid) NoConflict: unchanged line missing
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 2: Concurrent add + edit
-- Site A adds a line, Site B modifies an existing line
-- ============================================================
\connect cloudsync_block_adv_a
INSERT INTO docs (id, body) VALUES ('doc2', 'Alpha
Bravo');

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_d2_init
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_block_adv_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_d2_init', 'hex')) AS _apply_d2 \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc2') AS _mat_d2 \gset

-- Site A: add a new line at end
\connect cloudsync_block_adv_a
UPDATE docs SET body = 'Alpha
Bravo
Charlie' WHERE id = 'doc2';

-- Site B: modify first line
\connect cloudsync_block_adv_b
UPDATE docs SET body = 'AlphaEdited
Bravo' WHERE id = 'doc2';

\connect cloudsync_block_adv_a
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_d2a
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_block_adv_b
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_d2b
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

SELECT cloudsync_payload_apply(decode(:'payload_d2a', 'hex')) AS _apply_d2ab \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc2') AS _mat_d2b \gset

\connect cloudsync_block_adv_a
SELECT cloudsync_payload_apply(decode(:'payload_d2b', 'hex')) AS _apply_d2ba \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc2') AS _mat_d2a \gset

SELECT body AS body_d2a FROM docs WHERE id = 'doc2' \gset
\connect cloudsync_block_adv_b
SELECT body AS body_d2b FROM docs WHERE id = 'doc2' \gset

SELECT (:'body_d2a' = :'body_d2b') AS d2_converge \gset
\if :d2_converge
\echo [PASS] (:testid) Add+Edit: databases converge
\else
\echo [FAIL] (:testid) Add+Edit: databases diverged
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (position('Charlie' in :'body_d2a') > 0) AS has_charlie \gset
\if :has_charlie
\echo [PASS] (:testid) Add+Edit: added line Charlie preserved
\else
\echo [FAIL] (:testid) Add+Edit: added line Charlie missing
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (position('Bravo' in :'body_d2a') > 0) AS has_bravo \gset
\if :has_bravo
\echo [PASS] (:testid) Add+Edit: unchanged Bravo preserved
\else
\echo [FAIL] (:testid) Add+Edit: Bravo missing
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 3: Three-way sync — 3 databases, each edits a different line
-- ============================================================
\connect cloudsync_block_adv_c
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS docs;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('docs', 'CLS', true) AS _init_c \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol_c \gset

-- Insert initial on A
\connect cloudsync_block_adv_a
INSERT INTO docs (id, body) VALUES ('doc3', 'L1
L2
L3
L4');

-- Sync A -> B, A -> C
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_3init
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_block_adv_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_3init', 'hex')) AS _apply_3b \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc3') AS _mat_3b \gset

\connect cloudsync_block_adv_c
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_3init', 'hex')) AS _apply_3c \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc3') AS _mat_3c \gset

-- A edits line 1
\connect cloudsync_block_adv_a
UPDATE docs SET body = 'S0
L2
L3
L4' WHERE id = 'doc3';

-- B edits line 2
\connect cloudsync_block_adv_b
UPDATE docs SET body = 'L1
S1
L3
L4' WHERE id = 'doc3';

-- C edits line 4
\connect cloudsync_block_adv_c
UPDATE docs SET body = 'L1
L2
L3
S2' WHERE id = 'doc3';

-- Collect all payloads
\connect cloudsync_block_adv_a
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_3a
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_block_adv_b
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_3b
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_block_adv_c
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_3c
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

-- Full mesh apply: each site receives from the other two
\connect cloudsync_block_adv_a
SELECT cloudsync_payload_apply(decode(:'payload_3b', 'hex')) AS _3ab \gset
SELECT cloudsync_payload_apply(decode(:'payload_3c', 'hex')) AS _3ac \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc3') AS _mat_3a_final \gset

\connect cloudsync_block_adv_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_3a', 'hex')) AS _3ba \gset
SELECT cloudsync_payload_apply(decode(:'payload_3c', 'hex')) AS _3bc \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc3') AS _mat_3b_final \gset

\connect cloudsync_block_adv_c
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_3a', 'hex')) AS _3ca \gset
SELECT cloudsync_payload_apply(decode(:'payload_3b', 'hex')) AS _3cb \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc3') AS _mat_3c_final \gset

-- All three should converge
\connect cloudsync_block_adv_a
SELECT body AS body_3a FROM docs WHERE id = 'doc3' \gset
\connect cloudsync_block_adv_b
SELECT body AS body_3b FROM docs WHERE id = 'doc3' \gset
\connect cloudsync_block_adv_c
SELECT body AS body_3c FROM docs WHERE id = 'doc3' \gset

SELECT (:'body_3a' = :'body_3b' AND :'body_3b' = :'body_3c') AS three_converge \gset
\if :three_converge
\echo [PASS] (:testid) Three-way: all 3 databases converge
\else
\echo [FAIL] (:testid) Three-way: databases diverged
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (position('S0' in :'body_3a') > 0) AS has_s0 \gset
\if :has_s0
\echo [PASS] (:testid) Three-way: Site A edit preserved
\else
\echo [FAIL] (:testid) Three-way: Site A edit missing
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (position('S1' in :'body_3a') > 0) AS has_s1 \gset
\if :has_s1
\echo [PASS] (:testid) Three-way: Site B edit preserved
\else
\echo [FAIL] (:testid) Three-way: Site B edit missing
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (position('S2' in :'body_3a') > 0) AS has_s2 \gset
\if :has_s2
\echo [PASS] (:testid) Three-way: Site C edit preserved
\else
\echo [FAIL] (:testid) Three-way: Site C edit missing
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 4: Mixed block + normal columns
-- ============================================================
\connect cloudsync_block_adv_a
DROP TABLE IF EXISTS notes;
CREATE TABLE notes (id TEXT PRIMARY KEY NOT NULL, body TEXT, title TEXT);
SELECT cloudsync_init('notes', 'CLS', true) AS _init_notes_a \gset
SELECT cloudsync_set_column('notes', 'body', 'algo', 'block') AS _setcol_notes_a \gset

\connect cloudsync_block_adv_b
DROP TABLE IF EXISTS notes;
CREATE TABLE notes (id TEXT PRIMARY KEY NOT NULL, body TEXT, title TEXT);
SELECT cloudsync_init('notes', 'CLS', true) AS _init_notes_b \gset
SELECT cloudsync_set_column('notes', 'body', 'algo', 'block') AS _setcol_notes_b \gset

\connect cloudsync_block_adv_a
INSERT INTO notes (id, body, title) VALUES ('n1', 'Line1
Line2
Line3', 'My Title');

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_notes_init
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'notes' \gset

\connect cloudsync_block_adv_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_notes_init', 'hex')) AS _apply_notes \gset
SELECT cloudsync_text_materialize('notes', 'body', 'n1') AS _mat_notes \gset

-- A: edit block line 1 + title
\connect cloudsync_block_adv_a
UPDATE notes SET body = 'EditedLine1
Line2
Line3', title = 'Title From A' WHERE id = 'n1';

-- B: edit block line 3 + title (title conflicts via normal LWW)
\connect cloudsync_block_adv_b
UPDATE notes SET body = 'Line1
Line2
EditedLine3', title = 'Title From B' WHERE id = 'n1';

\connect cloudsync_block_adv_a
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_notes_a
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'notes' \gset

\connect cloudsync_block_adv_b
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_notes_b
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'notes' \gset

SELECT cloudsync_payload_apply(decode(:'payload_notes_a', 'hex')) AS _apply_notes_ab \gset
SELECT cloudsync_text_materialize('notes', 'body', 'n1') AS _mat_notes_b \gset

\connect cloudsync_block_adv_a
SELECT cloudsync_payload_apply(decode(:'payload_notes_b', 'hex')) AS _apply_notes_ba \gset
SELECT cloudsync_text_materialize('notes', 'body', 'n1') AS _mat_notes_a \gset

SELECT body AS notes_body_a FROM notes WHERE id = 'n1' \gset
SELECT title AS notes_title_a FROM notes WHERE id = 'n1' \gset
\connect cloudsync_block_adv_b
SELECT body AS notes_body_b FROM notes WHERE id = 'n1' \gset
SELECT title AS notes_title_b FROM notes WHERE id = 'n1' \gset

SELECT (:'notes_body_a' = :'notes_body_b') AS mixed_body_ok \gset
\if :mixed_body_ok
\echo [PASS] (:testid) MixedCols: body converges
\else
\echo [FAIL] (:testid) MixedCols: body diverged
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (position('EditedLine1' in :'notes_body_a') > 0 AND position('EditedLine3' in :'notes_body_a') > 0) AS both_edits \gset
\if :both_edits
\echo [PASS] (:testid) MixedCols: both block edits preserved
\else
\echo [FAIL] (:testid) MixedCols: block edits missing
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (:'notes_title_a' = :'notes_title_b') AS mixed_title_ok \gset
\if :mixed_title_ok
\echo [PASS] (:testid) MixedCols: title converges (normal LWW)
\else
\echo [FAIL] (:testid) MixedCols: title diverged
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 5: NULL to text transition
-- ============================================================
\connect cloudsync_block_adv_a
INSERT INTO docs (id, body) VALUES ('doc_null', NULL);

-- Verify 1 block for NULL
SELECT count(*) AS null_blocks FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc_null') \gset
SELECT (:null_blocks::int = 1) AS null_block_ok \gset
\if :null_block_ok
\echo [PASS] (:testid) NULL->Text: 1 block for NULL body
\else
\echo [FAIL] (:testid) NULL->Text: expected 1 block, got :null_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Update to multi-line
UPDATE docs SET body = 'Hello
World
Foo' WHERE id = 'doc_null';

SELECT count(*) AS text_blocks FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc_null') \gset
SELECT (:text_blocks::int = 3) AS text_block_ok \gset
\if :text_block_ok
\echo [PASS] (:testid) NULL->Text: 3 blocks after update
\else
\echo [FAIL] (:testid) NULL->Text: expected 3 blocks, got :text_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Sync and verify
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_null
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_block_adv_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_null', 'hex')) AS _apply_null \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc_null') AS _mat_null \gset

SELECT body AS body_null FROM docs WHERE id = 'doc_null' \gset
SELECT (:'body_null' = 'Hello
World
Foo') AS null_text_ok \gset
\if :null_text_ok
\echo [PASS] (:testid) NULL->Text: sync roundtrip matches
\else
\echo [FAIL] (:testid) NULL->Text: sync mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 6: Interleaved inserts — multiple rounds between existing lines
-- ============================================================
\connect cloudsync_block_adv_a
INSERT INTO docs (id, body) VALUES ('doc_inter', 'A
B');

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_inter_init
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_block_adv_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_inter_init', 'hex')) AS _apply_inter \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc_inter') AS _mat_inter \gset

-- Round 1: A inserts between A and B
\connect cloudsync_block_adv_a
UPDATE docs SET body = 'A
C
B' WHERE id = 'doc_inter';

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_inter_r1
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset
\connect cloudsync_block_adv_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_inter_r1', 'hex')) AS _r1 \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc_inter') AS _mat_r1 \gset

-- Round 2: B inserts between A and C
\connect cloudsync_block_adv_b
UPDATE docs SET body = 'A
D
C
B' WHERE id = 'doc_inter';

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_inter_r2
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset
\connect cloudsync_block_adv_a
SELECT cloudsync_payload_apply(decode(:'payload_inter_r2', 'hex')) AS _r2 \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc_inter') AS _mat_r2 \gset

-- Round 3: A inserts between D and C
\connect cloudsync_block_adv_a
UPDATE docs SET body = 'A
D
E
C
B' WHERE id = 'doc_inter';

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_inter_r3
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset
\connect cloudsync_block_adv_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_inter_r3', 'hex')) AS _r3 \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc_inter') AS _mat_r3 \gset

\connect cloudsync_block_adv_a
SELECT body AS inter_body_a FROM docs WHERE id = 'doc_inter' \gset
\connect cloudsync_block_adv_b
SELECT body AS inter_body_b FROM docs WHERE id = 'doc_inter' \gset

SELECT (:'inter_body_a' = :'inter_body_b') AS inter_converge \gset
\if :inter_converge
\echo [PASS] (:testid) Interleaved: databases converge
\else
\echo [FAIL] (:testid) Interleaved: databases diverged
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT count(*) AS inter_blocks FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc_inter') \gset
SELECT (:inter_blocks::int = 5) AS inter_count_ok \gset
\if :inter_count_ok
\echo [PASS] (:testid) Interleaved: 5 blocks after 3 rounds
\else
\echo [FAIL] (:testid) Interleaved: expected 5 blocks, got :inter_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 7: Custom delimiter (paragraph separator: double newline)
-- ============================================================
\connect cloudsync_block_adv_a
DROP TABLE IF EXISTS paragraphs;
CREATE TABLE paragraphs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('paragraphs', 'CLS', true) AS _init_para \gset
SELECT cloudsync_set_column('paragraphs', 'body', 'algo', 'block') AS _setcol_para \gset
SELECT cloudsync_set_column('paragraphs', 'body', 'delimiter', E'\n\n') AS _setdelim \gset

\connect cloudsync_block_adv_b
DROP TABLE IF EXISTS paragraphs;
CREATE TABLE paragraphs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('paragraphs', 'CLS', true) AS _init_para_b \gset
SELECT cloudsync_set_column('paragraphs', 'body', 'algo', 'block') AS _setcol_para_b \gset
SELECT cloudsync_set_column('paragraphs', 'body', 'delimiter', E'\n\n') AS _setdelim_b \gset

\connect cloudsync_block_adv_a
INSERT INTO paragraphs (id, body) VALUES ('p1', E'Para one line1\nline2\n\nPara two\n\nPara three');

-- Should produce 3 blocks (3 paragraphs)
SELECT count(*) AS para_blocks FROM paragraphs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('p1') \gset
SELECT (:para_blocks::int = 3) AS para_ok \gset
\if :para_ok
\echo [PASS] (:testid) CustomDelim: 3 paragraph blocks
\else
\echo [FAIL] (:testid) CustomDelim: expected 3 blocks, got :para_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Sync and verify roundtrip
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_para
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'paragraphs' \gset

\connect cloudsync_block_adv_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_para', 'hex')) AS _apply_para \gset
SELECT cloudsync_text_materialize('paragraphs', 'body', 'p1') AS _mat_para \gset

SELECT body AS para_body FROM paragraphs WHERE id = 'p1' \gset
SELECT (:'para_body' = E'Para one line1\nline2\n\nPara two\n\nPara three') AS para_roundtrip \gset
\if :para_roundtrip
\echo [PASS] (:testid) CustomDelim: sync roundtrip matches
\else
\echo [FAIL] (:testid) CustomDelim: sync mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 8: Large text — 200 lines
-- ============================================================
\connect cloudsync_block_adv_a
\ir helper_psql_conn_setup.sql
INSERT INTO docs (id, body)
SELECT 'bigdoc', string_agg('Line ' || lpad(i::text, 3, '0') || ' content', E'\n' ORDER BY i)
FROM generate_series(0, 199) AS s(i);

SELECT count(*) AS big_blocks FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('bigdoc') \gset
SELECT (:big_blocks::int = 200) AS big_ok \gset
\if :big_ok
\echo [PASS] (:testid) LargeText: 200 blocks created
\else
\echo [FAIL] (:testid) LargeText: expected 200 blocks, got :big_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- All positions unique
SELECT count(DISTINCT col_name) AS big_distinct FROM docs_cloudsync
WHERE col_name LIKE 'body' || chr(31) || '%'
AND pk = cloudsync_pk_encode('bigdoc') \gset
SELECT (:big_distinct::int = 200) AS big_unique \gset
\if :big_unique
\echo [PASS] (:testid) LargeText: 200 unique position IDs
\else
\echo [FAIL] (:testid) LargeText: expected 200 unique positions, got :big_distinct
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Sync and verify
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_big
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_block_adv_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_big', 'hex')) AS _apply_big \gset
SELECT cloudsync_text_materialize('docs', 'body', 'bigdoc') AS _mat_big \gset

SELECT body AS big_body_b FROM docs WHERE id = 'bigdoc' \gset
\connect cloudsync_block_adv_a
SELECT body AS big_body_a FROM docs WHERE id = 'bigdoc' \gset

SELECT (:'big_body_a' = :'big_body_b') AS big_match \gset
\if :big_match
\echo [PASS] (:testid) LargeText: sync roundtrip matches
\else
\echo [FAIL] (:testid) LargeText: sync mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 9: Rapid sequential updates — 50 updates on same row
-- ============================================================
\connect cloudsync_block_adv_a
\ir helper_psql_conn_setup.sql
INSERT INTO docs (id, body) VALUES ('rapid', 'Start');

DO $$
DECLARE
    i INT;
    new_body TEXT := '';
BEGIN
    FOR i IN 0..49 LOOP
        IF i > 0 THEN new_body := new_body || E'\n'; END IF;
        new_body := new_body || 'Update' || i;
        UPDATE docs SET body = new_body WHERE id = 'rapid';
    END LOOP;
END $$;

SELECT count(*) AS rapid_blocks FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('rapid') \gset
SELECT (:rapid_blocks::int = 50) AS rapid_ok \gset
\if :rapid_ok
\echo [PASS] (:testid) RapidUpdates: 50 blocks after 50 updates
\else
\echo [FAIL] (:testid) RapidUpdates: expected 50 blocks, got :rapid_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Sync and verify
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_rapid
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_block_adv_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_rapid', 'hex')) AS _apply_rapid \gset
SELECT cloudsync_text_materialize('docs', 'body', 'rapid') AS _mat_rapid \gset

SELECT body AS rapid_body_b FROM docs WHERE id = 'rapid' \gset
\connect cloudsync_block_adv_a
SELECT body AS rapid_body_a FROM docs WHERE id = 'rapid' \gset

SELECT (:'rapid_body_a' = :'rapid_body_b') AS rapid_match \gset
\if :rapid_match
\echo [PASS] (:testid) RapidUpdates: sync roundtrip matches
\else
\echo [FAIL] (:testid) RapidUpdates: sync mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (position('Update0' in :'rapid_body_a') > 0) AS has_first \gset
\if :has_first
\echo [PASS] (:testid) RapidUpdates: first update present
\else
\echo [FAIL] (:testid) RapidUpdates: first update missing
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (position('Update49' in :'rapid_body_a') > 0) AS has_last \gset
\if :has_last
\echo [PASS] (:testid) RapidUpdates: last update present
\else
\echo [FAIL] (:testid) RapidUpdates: last update missing
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_block_adv_a;
DROP DATABASE IF EXISTS cloudsync_block_adv_b;
DROP DATABASE IF EXISTS cloudsync_block_adv_c;
\else
\echo [INFO] !!!!!
\endif
