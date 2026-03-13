-- 'Block-level LWW round 4: UUID PK, RLS+blocks, multi-table, 3-site convergence, custom delimiter sync, mixed column updates'

\set testid '37'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql

DROP DATABASE IF EXISTS cloudsync_block_r4_a;
DROP DATABASE IF EXISTS cloudsync_block_r4_b;
DROP DATABASE IF EXISTS cloudsync_block_r4_c;
DROP DATABASE IF EXISTS cloudsync_block_3s_a;
DROP DATABASE IF EXISTS cloudsync_block_3s_b;
DROP DATABASE IF EXISTS cloudsync_block_3s_c;
CREATE DATABASE cloudsync_block_r4_a;
CREATE DATABASE cloudsync_block_r4_b;
CREATE DATABASE cloudsync_block_r4_c;

-- ============================================================
-- Test 1: UUID primary key with block column
-- ============================================================
\connect cloudsync_block_r4_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS uuid_docs;
CREATE TABLE uuid_docs (id UUID PRIMARY KEY NOT NULL DEFAULT gen_random_uuid(), body TEXT);
SELECT cloudsync_init('uuid_docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('uuid_docs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r4_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS uuid_docs;
CREATE TABLE uuid_docs (id UUID PRIMARY KEY NOT NULL DEFAULT gen_random_uuid(), body TEXT);
SELECT cloudsync_init('uuid_docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('uuid_docs', 'body', 'algo', 'block') AS _sc \gset

-- Insert on A with explicit UUID
\connect cloudsync_block_r4_a
INSERT INTO uuid_docs (id, body) VALUES ('a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11', E'UUID-Line1\nUUID-Line2\nUUID-Line3');

SELECT count(*) AS uuid_blocks FROM uuid_docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11') \gset
SELECT (:'uuid_blocks'::int = 3) AS uuid_blk_ok \gset
\if :uuid_blk_ok
\echo [PASS] (:testid) UUID_PK: 3 blocks created
\else
\echo [FAIL] (:testid) UUID_PK: expected 3 blocks, got :uuid_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Sync A -> B
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_uuid
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'uuid_docs' \gset

\connect cloudsync_block_r4_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_uuid', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('uuid_docs', 'body', 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11') AS _mat \gset

SELECT (body = E'UUID-Line1\nUUID-Line2\nUUID-Line3') AS uuid_body_ok FROM uuid_docs WHERE id = 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11' \gset
\if :uuid_body_ok
\echo [PASS] (:testid) UUID_PK: body matches on B
\else
\echo [FAIL] (:testid) UUID_PK: body mismatch on B
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Edit on B, reverse sync
\connect cloudsync_block_r4_b
UPDATE uuid_docs SET body = E'UUID-Line1\nUUID-Edited\nUUID-Line3' WHERE id = 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11';

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_uuid_r
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'uuid_docs' \gset

\connect cloudsync_block_r4_a
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_uuid_r', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('uuid_docs', 'body', 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11') AS _mat \gset

SELECT (body = E'UUID-Line1\nUUID-Edited\nUUID-Line3') AS uuid_rev_ok FROM uuid_docs WHERE id = 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11' \gset
\if :uuid_rev_ok
\echo [PASS] (:testid) UUID_PK: reverse sync matches
\else
\echo [FAIL] (:testid) UUID_PK: reverse sync mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 2: RLS filter + block columns
-- Only rows matching filter should have block tracking
-- ============================================================
\connect cloudsync_block_r4_a
DROP TABLE IF EXISTS rls_docs;
CREATE TABLE rls_docs (id TEXT PRIMARY KEY NOT NULL, owner_id INTEGER, body TEXT);
SELECT cloudsync_init('rls_docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('rls_docs', 'body', 'algo', 'block') AS _sc \gset
SELECT cloudsync_set_filter('rls_docs', 'owner_id = 1') AS _sf \gset

\connect cloudsync_block_r4_b
DROP TABLE IF EXISTS rls_docs;
CREATE TABLE rls_docs (id TEXT PRIMARY KEY NOT NULL, owner_id INTEGER, body TEXT);
SELECT cloudsync_init('rls_docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('rls_docs', 'body', 'algo', 'block') AS _sc \gset
SELECT cloudsync_set_filter('rls_docs', 'owner_id = 1') AS _sf \gset

-- Insert matching row (owner_id=1) and non-matching row (owner_id=2)
\connect cloudsync_block_r4_a
INSERT INTO rls_docs (id, owner_id, body) VALUES ('match1', 1, E'Filtered-Line1\nFiltered-Line2');
INSERT INTO rls_docs (id, owner_id, body) VALUES ('nomatch', 2, E'Hidden-Line1\nHidden-Line2');

-- Check: matching row has blocks, non-matching does not
SELECT count(*) AS rls_match_blocks FROM rls_docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('match1') \gset
SELECT count(*) AS rls_nomatch_blocks FROM rls_docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('nomatch') \gset

SELECT (:'rls_match_blocks'::int = 2) AS rls_match_ok \gset
\if :rls_match_ok
\echo [PASS] (:testid) RLS+Blocks: matching row has 2 blocks
\else
\echo [FAIL] (:testid) RLS+Blocks: expected 2 blocks for matching row, got :rls_match_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (:'rls_nomatch_blocks'::int = 0) AS rls_nomatch_ok \gset
\if :rls_nomatch_ok
\echo [PASS] (:testid) RLS+Blocks: non-matching row has 0 blocks
\else
\echo [FAIL] (:testid) RLS+Blocks: expected 0 blocks for non-matching row, got :rls_nomatch_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Sync: only matching row should appear in changes
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_rls
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'rls_docs' \gset

\connect cloudsync_block_r4_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_rls', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('rls_docs', 'body', 'match1') AS _mat \gset

SELECT (body = E'Filtered-Line1\nFiltered-Line2') AS rls_sync_ok FROM rls_docs WHERE id = 'match1' \gset
\if :rls_sync_ok
\echo [PASS] (:testid) RLS+Blocks: matching row synced with correct body
\else
\echo [FAIL] (:testid) RLS+Blocks: matching row body mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- non-matching row should NOT exist on B
SELECT (count(*) = 0) AS rls_norow_ok FROM rls_docs WHERE id = 'nomatch' \gset
\if :rls_norow_ok
\echo [PASS] (:testid) RLS+Blocks: non-matching row not synced
\else
\echo [FAIL] (:testid) RLS+Blocks: non-matching row should not exist on B
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 3: Multi-table blocks — two tables with block columns in same payload
-- ============================================================
\connect cloudsync_block_r4_a
DROP TABLE IF EXISTS articles;
DROP TABLE IF EXISTS comments;
CREATE TABLE articles (id TEXT PRIMARY KEY NOT NULL, content TEXT);
CREATE TABLE comments (id TEXT PRIMARY KEY NOT NULL, text_body TEXT);
SELECT cloudsync_init('articles', 'CLS', true) AS _init \gset
SELECT cloudsync_init('comments', 'CLS', true) AS _init2 \gset
SELECT cloudsync_set_column('articles', 'content', 'algo', 'block') AS _sc \gset
SELECT cloudsync_set_column('comments', 'text_body', 'algo', 'block') AS _sc2 \gset

\connect cloudsync_block_r4_b
DROP TABLE IF EXISTS articles;
DROP TABLE IF EXISTS comments;
CREATE TABLE articles (id TEXT PRIMARY KEY NOT NULL, content TEXT);
CREATE TABLE comments (id TEXT PRIMARY KEY NOT NULL, text_body TEXT);
SELECT cloudsync_init('articles', 'CLS', true) AS _init \gset
SELECT cloudsync_init('comments', 'CLS', true) AS _init2 \gset
SELECT cloudsync_set_column('articles', 'content', 'algo', 'block') AS _sc \gset
SELECT cloudsync_set_column('comments', 'text_body', 'algo', 'block') AS _sc2 \gset

\connect cloudsync_block_r4_a
INSERT INTO articles (id, content) VALUES ('art1', E'Para1\nPara2\nPara3');
INSERT INTO comments (id, text_body) VALUES ('cmt1', E'Comment-Line1\nComment-Line2');
UPDATE articles SET content = E'Para1-Edited\nPara2\nPara3' WHERE id = 'art1';

-- Single payload containing changes from both tables
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_mt
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

\connect cloudsync_block_r4_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_mt', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('articles', 'content', 'art1') AS _m1 \gset
SELECT cloudsync_text_materialize('comments', 'text_body', 'cmt1') AS _m2 \gset

SELECT (content = E'Para1-Edited\nPara2\nPara3') AS mt_art_ok FROM articles WHERE id = 'art1' \gset
\if :mt_art_ok
\echo [PASS] (:testid) MultiTable: articles content matches
\else
\echo [FAIL] (:testid) MultiTable: articles content mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (text_body = E'Comment-Line1\nComment-Line2') AS mt_cmt_ok FROM comments WHERE id = 'cmt1' \gset
\if :mt_cmt_ok
\echo [PASS] (:testid) MultiTable: comments text_body matches
\else
\echo [FAIL] (:testid) MultiTable: comments text_body mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 4: Three-site convergence with block columns
-- All three sites make different edits, pairwise sync, verify convergence
-- Uses dedicated databases so all 3 have identical schema
-- ============================================================
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_block_3s_a;
DROP DATABASE IF EXISTS cloudsync_block_3s_b;
DROP DATABASE IF EXISTS cloudsync_block_3s_c;
CREATE DATABASE cloudsync_block_3s_a;
CREATE DATABASE cloudsync_block_3s_b;
CREATE DATABASE cloudsync_block_3s_c;

\connect cloudsync_block_3s_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE tdocs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('tdocs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('tdocs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_3s_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE tdocs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('tdocs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('tdocs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_3s_c
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE tdocs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('tdocs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('tdocs', 'body', 'algo', 'block') AS _sc \gset

-- Initial insert on A, sync to B and C
\connect cloudsync_block_3s_a
INSERT INTO tdocs (id, body) VALUES ('doc1', E'Line1\nLine2\nLine3\nLine4\nLine5');

-- Full changes from A (includes schema info)
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_3s_init
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'tdocs' \gset

\connect cloudsync_block_3s_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_3s_init', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('tdocs', 'body', 'doc1') AS _mat \gset

\connect cloudsync_block_3s_c
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_3s_init', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('tdocs', 'body', 'doc1') AS _mat \gset

-- Each site edits a DIFFERENT line (no conflicts)
-- A edits line 1
\connect cloudsync_block_3s_a
UPDATE tdocs SET body = E'Line1-A\nLine2\nLine3\nLine4\nLine5' WHERE id = 'doc1';

-- B edits line 3
\connect cloudsync_block_3s_b
UPDATE tdocs SET body = E'Line1\nLine2\nLine3-B\nLine4\nLine5' WHERE id = 'doc1';

-- C edits line 5
\connect cloudsync_block_3s_c
UPDATE tdocs SET body = E'Line1\nLine2\nLine3\nLine4\nLine5-C' WHERE id = 'doc1';

-- Collect ALL changes from each site (not filtered by site_id)
-- This includes the schema info that recipients need
\connect cloudsync_block_3s_a
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_3s_a
FROM cloudsync_changes WHERE tbl = 'tdocs' \gset

\connect cloudsync_block_3s_b
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_3s_b
FROM cloudsync_changes WHERE tbl = 'tdocs' \gset

\connect cloudsync_block_3s_c
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_3s_c
FROM cloudsync_changes WHERE tbl = 'tdocs' \gset

-- Apply all to A (B's and C's changes)
\connect cloudsync_block_3s_a
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_3s_b', 'hex')) AS _app_b \gset
SELECT cloudsync_payload_apply(decode(:'payload_3s_c', 'hex')) AS _app_c \gset
SELECT cloudsync_text_materialize('tdocs', 'body', 'doc1') AS _mat \gset

-- Apply all to B (A's and C's changes)
\connect cloudsync_block_3s_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_3s_a', 'hex')) AS _app_a \gset
SELECT cloudsync_payload_apply(decode(:'payload_3s_c', 'hex')) AS _app_c \gset
SELECT cloudsync_text_materialize('tdocs', 'body', 'doc1') AS _mat \gset

-- Apply all to C (A's and B's changes)
\connect cloudsync_block_3s_c
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_3s_a', 'hex')) AS _app_a \gset
SELECT cloudsync_payload_apply(decode(:'payload_3s_b', 'hex')) AS _app_b \gset
SELECT cloudsync_text_materialize('tdocs', 'body', 'doc1') AS _mat \gset

-- All three should converge
\connect cloudsync_block_3s_a
SELECT body AS body_a FROM tdocs WHERE id = 'doc1' \gset
\connect cloudsync_block_3s_b
SELECT body AS body_b FROM tdocs WHERE id = 'doc1' \gset
\connect cloudsync_block_3s_c
SELECT body AS body_c FROM tdocs WHERE id = 'doc1' \gset

SELECT (:'body_a' = :'body_b') AS ab_match \gset
SELECT (:'body_b' = :'body_c') AS bc_match \gset

\if :ab_match
\echo [PASS] (:testid) 3-Site: A and B converge
\else
\echo [FAIL] (:testid) 3-Site: A and B diverged
SELECT (:fail::int + 1) AS fail \gset
\endif

\if :bc_match
\echo [PASS] (:testid) 3-Site: B and C converge
\else
\echo [FAIL] (:testid) 3-Site: B and C diverged
SELECT (:fail::int + 1) AS fail \gset
\endif

-- All edits should be present (non-conflicting)
SELECT (position('Line1-A' in :'body_a') > 0) AS has_a \gset
SELECT (position('Line3-B' in :'body_a') > 0) AS has_b \gset
SELECT (position('Line5-C' in :'body_a') > 0) AS has_c \gset

\if :has_a
\echo [PASS] (:testid) 3-Site: Site A edit present
\else
\echo [FAIL] (:testid) 3-Site: Site A edit missing
SELECT (:fail::int + 1) AS fail \gset
\endif

\if :has_b
\echo [PASS] (:testid) 3-Site: Site B edit present
\else
\echo [FAIL] (:testid) 3-Site: Site B edit missing
SELECT (:fail::int + 1) AS fail \gset
\endif

\if :has_c
\echo [PASS] (:testid) 3-Site: Site C edit present
\else
\echo [FAIL] (:testid) 3-Site: Site C edit missing
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 5: Custom delimiter sync roundtrip
-- Uses paragraph delimiter (double newline), edits, syncs
-- ============================================================
\connect cloudsync_block_r4_a
DROP TABLE IF EXISTS para_docs;
CREATE TABLE para_docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('para_docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('para_docs', 'body', 'algo', 'block') AS _sc \gset
SELECT cloudsync_set_column('para_docs', 'body', 'delimiter', E'\n\n') AS _sd \gset

\connect cloudsync_block_r4_b
DROP TABLE IF EXISTS para_docs;
CREATE TABLE para_docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('para_docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('para_docs', 'body', 'algo', 'block') AS _sc \gset
SELECT cloudsync_set_column('para_docs', 'body', 'delimiter', E'\n\n') AS _sd \gset

\connect cloudsync_block_r4_a
INSERT INTO para_docs (id, body) VALUES ('doc1', E'First paragraph.\n\nSecond paragraph.\n\nThird paragraph.');

SELECT count(*) AS pd_blocks FROM para_docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1') \gset
SELECT (:'pd_blocks'::int = 3) AS pd_blk_ok \gset
\if :pd_blk_ok
\echo [PASS] (:testid) CustomDelimSync: 3 paragraph blocks
\else
\echo [FAIL] (:testid) CustomDelimSync: expected 3 blocks, got :pd_blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Sync A -> B
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_pd
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'para_docs' \gset

\connect cloudsync_block_r4_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_pd', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('para_docs', 'body', 'doc1') AS _mat \gset

SELECT (body = E'First paragraph.\n\nSecond paragraph.\n\nThird paragraph.') AS pd_sync_ok FROM para_docs WHERE id = 'doc1' \gset
\if :pd_sync_ok
\echo [PASS] (:testid) CustomDelimSync: body matches on B
\else
\echo [FAIL] (:testid) CustomDelimSync: body mismatch on B
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Edit paragraph 2 on B, sync back
\connect cloudsync_block_r4_b
UPDATE para_docs SET body = E'First paragraph.\n\nEdited second paragraph.\n\nThird paragraph.' WHERE id = 'doc1';

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_pd_r
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'para_docs' \gset

\connect cloudsync_block_r4_a
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_pd_r', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('para_docs', 'body', 'doc1') AS _mat \gset

SELECT (body = E'First paragraph.\n\nEdited second paragraph.\n\nThird paragraph.') AS pd_rev_ok FROM para_docs WHERE id = 'doc1' \gset
\if :pd_rev_ok
\echo [PASS] (:testid) CustomDelimSync: reverse sync matches
\else
\echo [FAIL] (:testid) CustomDelimSync: reverse sync mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Test 6: Block column + regular LWW column — mixed update
-- Single UPDATE changes both block col and regular col
-- ============================================================
\connect cloudsync_block_r4_a
DROP TABLE IF EXISTS mixed_docs;
CREATE TABLE mixed_docs (id TEXT PRIMARY KEY NOT NULL, body TEXT, title TEXT);
SELECT cloudsync_init('mixed_docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('mixed_docs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r4_b
DROP TABLE IF EXISTS mixed_docs;
CREATE TABLE mixed_docs (id TEXT PRIMARY KEY NOT NULL, body TEXT, title TEXT);
SELECT cloudsync_init('mixed_docs', 'CLS', true) AS _init \gset
SELECT cloudsync_set_column('mixed_docs', 'body', 'algo', 'block') AS _sc \gset

\connect cloudsync_block_r4_a
INSERT INTO mixed_docs (id, body, title) VALUES ('doc1', E'Body-Line1\nBody-Line2', 'Original Title');

-- Sync A -> B
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_mix_i
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'mixed_docs' \gset

\connect cloudsync_block_r4_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_mix_i', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('mixed_docs', 'body', 'doc1') AS _mat \gset

-- Update BOTH columns simultaneously on A
\connect cloudsync_block_r4_a
UPDATE mixed_docs SET body = E'Body-Edited1\nBody-Line2', title = 'New Title' WHERE id = 'doc1';

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_mix_u
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'mixed_docs' \gset

\connect cloudsync_block_r4_b
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(decode(:'payload_mix_u', 'hex')) AS _app \gset
SELECT cloudsync_text_materialize('mixed_docs', 'body', 'doc1') AS _mat \gset

SELECT (body = E'Body-Edited1\nBody-Line2') AS mix_body_ok FROM mixed_docs WHERE id = 'doc1' \gset
\if :mix_body_ok
\echo [PASS] (:testid) MixedUpdate: block column body matches
\else
\echo [FAIL] (:testid) MixedUpdate: block column body mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (title = 'New Title') AS mix_title_ok FROM mixed_docs WHERE id = 'doc1' \gset
\if :mix_title_ok
\echo [PASS] (:testid) MixedUpdate: regular column title matches
\else
\echo [FAIL] (:testid) MixedUpdate: regular column title mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- ============================================================
-- Cleanup
-- ============================================================
\ir helper_test_cleanup.sql
\if :should_cleanup
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_block_r4_a;
DROP DATABASE IF EXISTS cloudsync_block_r4_b;
DROP DATABASE IF EXISTS cloudsync_block_r4_c;
DROP DATABASE IF EXISTS cloudsync_block_3s_a;
DROP DATABASE IF EXISTS cloudsync_block_3s_b;
DROP DATABASE IF EXISTS cloudsync_block_3s_c;
\else
\echo [INFO] !!!!!
\endif
