-- 'Block-level LWW edge cases: unicode, special chars, delete vs edit, two block cols, text->NULL, payload sync, idempotent, ordering'

\set testid '35'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

DROP DATABASE IF EXISTS cloudsync_block_edge_a;
DROP DATABASE IF EXISTS cloudsync_block_edge_b;
CREATE DATABASE cloudsync_block_edge_a;
CREATE DATABASE cloudsync_block_edge_b;

-- ============================================================
-- Test 1: Unicode / multibyte content (emoji, CJK, accented)
-- ============================================================
\connect cloudsync_block_edge_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS docs;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_edge_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS docs;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _sc \gset

-- Insert unicode text on A
\connect cloudsync_block_edge_a
INSERT INTO docs (id, body) VALUES ('doc1', E'Hello \U0001F600\nBonjour caf\u00e9\n\u65e5\u672c\u8a9e\u30c6\u30b9\u30c8');

-- Sync A -> B
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload1
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_block_edge_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload1', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc1') AS _mat \gset

SELECT (body LIKE E'Hello %') AS unicode_ok FROM docs WHERE id = 'doc1' \gset
\if :unicode_ok
\echo [PASS] (:testid) Unicode: body starts with Hello
\else
\echo [FAIL] (:testid) Unicode: body mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Check line count (3 lines = 2 newlines)
SELECT (length(body) - length(replace(body, E'\n', '')) = 2) AS unicode_lines FROM docs WHERE id = 'doc1' \gset
\if :unicode_lines
\echo [PASS] (:testid) Unicode: 3 lines present
\else
\echo [FAIL] (:testid) Unicode: wrong line count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 2: Special characters (tabs, backslashes, quotes)
-- ============================================================
\connect cloudsync_block_edge_a
INSERT INTO docs (id, body) VALUES ('doc2', E'line\twith\ttabs\nback\\\\slash\nO''Brien said "hi"');

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload2
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'docs'
AND pk = cloudsync_pk_encode('doc2') \gset

\connect cloudsync_block_edge_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload2', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc2') AS _mat \gset

SELECT (body LIKE E'%\t%') AS special_tabs FROM docs WHERE id = 'doc2' \gset
\if :special_tabs
\echo [PASS] (:testid) SpecialChars: tabs preserved
\else
\echo [FAIL] (:testid) SpecialChars: tabs lost
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (body LIKE '%Brien%') AS special_quotes FROM docs WHERE id = 'doc2' \gset
\if :special_quotes
\echo [PASS] (:testid) SpecialChars: quotes preserved
\else
\echo [FAIL] (:testid) SpecialChars: quotes lost
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 3: Delete vs edit — A deletes block 1, B edits block 2
-- ============================================================
\connect cloudsync_block_edge_a
INSERT INTO docs (id, body) VALUES ('doc3', E'Alpha\nBeta\nGamma');

-- Sync initial to B
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload3i
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'docs'
AND pk = cloudsync_pk_encode('doc3') \gset

\connect cloudsync_block_edge_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload3i', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc3') AS _mat \gset

-- A: remove first line
\connect cloudsync_block_edge_a
UPDATE docs SET body = E'Beta\nGamma' WHERE id = 'doc3';

-- B: edit second line
\connect cloudsync_block_edge_b
UPDATE docs SET body = E'Alpha\nBetaEdited\nGamma' WHERE id = 'doc3';

-- Sync A -> B
\connect cloudsync_block_edge_a
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload3a
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'docs'
AND pk = cloudsync_pk_encode('doc3') \gset

\connect cloudsync_block_edge_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload3a', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc3') AS _mat \gset

-- B should have: Alpha removed (A wins), BetaEdited kept (B's edit)
SELECT (body NOT LIKE '%Alpha%') AS dve_no_alpha FROM docs WHERE id = 'doc3' \gset
\if :dve_no_alpha
\echo [PASS] (:testid) DelVsEdit: Alpha removed
\else
\echo [FAIL] (:testid) DelVsEdit: Alpha still present
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (body LIKE '%BetaEdited%') AS dve_beta FROM docs WHERE id = 'doc3' \gset
\if :dve_beta
\echo [PASS] (:testid) DelVsEdit: BetaEdited present
\else
\echo [FAIL] (:testid) DelVsEdit: BetaEdited missing
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (body LIKE '%Gamma%') AS dve_gamma FROM docs WHERE id = 'doc3' \gset
\if :dve_gamma
\echo [PASS] (:testid) DelVsEdit: Gamma present
\else
\echo [FAIL] (:testid) DelVsEdit: Gamma missing
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 4: Two block columns on the same table (body + notes)
-- ============================================================
\connect cloudsync_block_edge_a
DROP TABLE IF EXISTS articles;
CREATE TABLE articles (id TEXT PRIMARY KEY NOT NULL, body TEXT, notes TEXT);
SELECT cloudsync_init('articles', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('articles', 'body', 'algo', 'block') AS _sc1 \gset
SELECT cloudsync_set_column('articles', 'notes', 'algo', 'block') AS _sc2 \gset

\connect cloudsync_block_edge_b
DROP TABLE IF EXISTS articles;
CREATE TABLE articles (id TEXT PRIMARY KEY NOT NULL, body TEXT, notes TEXT);
SELECT cloudsync_init('articles', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('articles', 'body', 'algo', 'block') AS _sc1 \gset
SELECT cloudsync_set_column('articles', 'notes', 'algo', 'block') AS _sc2 \gset

-- Insert on A
\connect cloudsync_block_edge_a
INSERT INTO articles (id, body, notes) VALUES ('art1', E'Body line 1\nBody line 2', E'Note 1\nNote 2');

-- Sync A -> B
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload4
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'articles' \gset

\connect cloudsync_block_edge_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload4', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('articles', 'body', 'art1') AS _mb \gset
SELECT cloudsync_text_materialize('articles', 'notes', 'art1') AS _mn \gset

SELECT (body = E'Body line 1\nBody line 2') AS twocol_body FROM articles WHERE id = 'art1' \gset
\if :twocol_body
\echo [PASS] (:testid) TwoBlockCols: body matches
\else
\echo [FAIL] (:testid) TwoBlockCols: body mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (notes = E'Note 1\nNote 2') AS twocol_notes FROM articles WHERE id = 'art1' \gset
\if :twocol_notes
\echo [PASS] (:testid) TwoBlockCols: notes matches
\else
\echo [FAIL] (:testid) TwoBlockCols: notes mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Edit body on A, notes on B — then sync
\connect cloudsync_block_edge_a
UPDATE articles SET body = E'Body EDITED\nBody line 2' WHERE id = 'art1';

\connect cloudsync_block_edge_b
UPDATE articles SET notes = E'Note 1\nNote EDITED' WHERE id = 'art1';

-- Sync A -> B
\connect cloudsync_block_edge_a
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload4b
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'articles' \gset

\connect cloudsync_block_edge_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload4b', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('articles', 'body', 'art1') AS _mb \gset
SELECT cloudsync_text_materialize('articles', 'notes', 'art1') AS _mn \gset

SELECT (body LIKE '%Body EDITED%') AS twocol_body_ed FROM articles WHERE id = 'art1' \gset
\if :twocol_body_ed
\echo [PASS] (:testid) TwoBlockCols: body edited
\else
\echo [FAIL] (:testid) TwoBlockCols: body edit missing
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (notes LIKE '%Note EDITED%') AS twocol_notes_ed FROM articles WHERE id = 'art1' \gset
\if :twocol_notes_ed
\echo [PASS] (:testid) TwoBlockCols: notes kept
\else
\echo [FAIL] (:testid) TwoBlockCols: notes edit lost
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 5: Text -> NULL (update to NULL removes all blocks)
-- ============================================================
\connect cloudsync_block_edge_a
INSERT INTO docs (id, body) VALUES ('doc5', E'Line1\nLine2\nLine3');

-- Verify blocks created
SELECT (count(*) = 3) AS blk_ok FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc5') \gset
\if :blk_ok
\echo [PASS] (:testid) TextToNull: 3 blocks created
\else
\echo [FAIL] (:testid) TextToNull: wrong initial block count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Update to NULL
UPDATE docs SET body = NULL WHERE id = 'doc5';

-- Sync A -> B
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload5
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'docs'
AND pk = cloudsync_pk_encode('doc5') \gset

\connect cloudsync_block_edge_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload5', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc5') AS _mat \gset

SELECT (body IS NULL) AS null_remote FROM docs WHERE id = 'doc5' \gset
\if :null_remote
\echo [PASS] (:testid) TextToNull: body is NULL on remote
\else
\echo [FAIL] (:testid) TextToNull: body not NULL on remote
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 6: Payload-based sync with non-conflicting edits
-- ============================================================
\connect cloudsync_block_edge_a
INSERT INTO docs (id, body) VALUES ('doc6', E'First\nSecond\nThird');

-- Sync A -> B
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload6i
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'docs'
AND pk = cloudsync_pk_encode('doc6') \gset

\connect cloudsync_block_edge_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload6i', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc6') AS _mat \gset

-- A edits line 1
\connect cloudsync_block_edge_a
UPDATE docs SET body = E'FirstEdited\nSecond\nThird' WHERE id = 'doc6';

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload6a
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'docs'
AND pk = cloudsync_pk_encode('doc6') \gset

\connect cloudsync_block_edge_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload6a', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc6') AS _mat \gset

SELECT (body = E'FirstEdited\nSecond\nThird') AS payload_ok FROM docs WHERE id = 'doc6' \gset
\if :payload_ok
\echo [PASS] (:testid) PayloadSync: body matches
\else
\echo [FAIL] (:testid) PayloadSync: body mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 7: Idempotent apply — same payload twice is a no-op
-- ============================================================
\connect cloudsync_block_edge_a
INSERT INTO docs (id, body) VALUES ('doc7', E'AAA\nBBB\nCCC');

-- Sync initial
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload7i
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'docs'
AND pk = cloudsync_pk_encode('doc7') \gset

\connect cloudsync_block_edge_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload7i', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc7') AS _mat \gset

-- A edits
\connect cloudsync_block_edge_a
UPDATE docs SET body = E'AAA-edited\nBBB\nCCC' WHERE id = 'doc7';

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload7e
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'docs'
AND pk = cloudsync_pk_encode('doc7') \gset

-- Apply TWICE to B
\connect cloudsync_block_edge_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload7e', 'hex')) AS _app1 \gset
SELECT cloudsync_payload_apply(decode(:'payload7e', 'hex')) AS _app2 \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc7') AS _mat \gset

SELECT (body LIKE '%AAA-edited%') AS idemp_ok FROM docs WHERE id = 'doc7' \gset
\if :idemp_ok
\echo [PASS] (:testid) Idempotent: body matches after double apply
\else
\echo [FAIL] (:testid) Idempotent: body mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 8: Block position ordering — sequential inserts preserve order after sync
-- ============================================================
\connect cloudsync_block_edge_a
INSERT INTO docs (id, body) VALUES ('doc8', E'Top\nBottom');

-- Sync initial to B
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload8i
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'docs'
AND pk = cloudsync_pk_encode('doc8') \gset

\connect cloudsync_block_edge_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload8i', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc8') AS _mat \gset

-- A: add two lines between Top and Bottom
\connect cloudsync_block_edge_a
UPDATE docs SET body = E'Top\nMiddle1\nMiddle2\nBottom' WHERE id = 'doc8';

-- Sync A -> B
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload8a
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'docs'
AND pk = cloudsync_pk_encode('doc8') \gset

\connect cloudsync_block_edge_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload8a', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('docs', 'body', 'doc8') AS _mat \gset

SELECT (body LIKE 'Top%') AS ord_top FROM docs WHERE id = 'doc8' \gset
\if :ord_top
\echo [PASS] (:testid) Ordering: Top first
\else
\echo [FAIL] (:testid) Ordering: Top not first
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (body LIKE '%Bottom') AS ord_bottom FROM docs WHERE id = 'doc8' \gset
\if :ord_bottom
\echo [PASS] (:testid) Ordering: Bottom last
\else
\echo [FAIL] (:testid) Ordering: Bottom not last
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Middle1 should come before Middle2
SELECT (position('Middle1' IN body) < position('Middle2' IN body)) AS ord_correct FROM docs WHERE id = 'doc8' \gset
\if :ord_correct
\echo [PASS] (:testid) Ordering: Middle1 before Middle2
\else
\echo [FAIL] (:testid) Ordering: wrong order
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (body = E'Top\nMiddle1\nMiddle2\nBottom') AS ord_exact FROM docs WHERE id = 'doc8' \gset
\if :ord_exact
\echo [PASS] (:testid) Ordering: exact match
\else
\echo [FAIL] (:testid) Ordering: content mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Cleanup
-- ============================================================
\ir helper_test_cleanup.sql
\if :should_cleanup
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_block_edge_a;
DROP DATABASE IF EXISTS cloudsync_block_edge_b;
\else
\echo [INFO] !!!!!
\endif
