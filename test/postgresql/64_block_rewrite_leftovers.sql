-- Rewriting a whole row must not leave a shorter block column's old blocks behind.
-- Deleting a row keeps its block values, so recreating it with fewer blocks used to
-- leave the extra ones stored, and a peer could still receive them as content.

\set testid '64-block-rewrite'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_64_source;
DROP DATABASE IF EXISTS cloudsync_test_64_target;
CREATE DATABASE cloudsync_test_64_source;
CREATE DATABASE cloudsync_test_64_target;

\connect cloudsync_test_64_source
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('docs', 'CLS', 1) AS _init \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol \gset

\connect cloudsync_test_64_target
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('docs', 'CLS', 1) AS _init \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol \gset

-- Three blocks, delivered to the target.
\connect cloudsync_test_64_source
INSERT INTO docs (id, body) VALUES ('d1', 'AAA' || chr(10) || 'BBB' || chr(10) || 'CCC');
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload1
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset
\connect cloudsync_test_64_target
SELECT cloudsync_payload_apply(decode(:'payload1', 'hex')) AS _apply1 \gset
SELECT cloudsync_text_materialize('docs', 'body', cloudsync_pk_encode('d1')) AS _mat1 \gset
SELECT (SELECT body FROM docs WHERE id = 'd1') = 'AAA' || chr(10) || 'BBB' || chr(10) || 'CCC' AS first_ok \gset
\if :first_ok
\echo [PASS] (:testid) three blocks delivered
\else
\echo [FAIL] (:testid) three blocks not delivered
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Recreate the row with a single block: the other two must not survive.
\connect cloudsync_test_64_source
SELECT coalesce(max(db_version), 0) AS before_short FROM cloudsync_changes \gset
DELETE FROM docs WHERE id = 'd1';
INSERT INTO docs (id, body) VALUES ('d1', 'ZZZ');
SELECT count(*) AS stored_blocks FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('d1') \gset
SELECT count(*) AS live_meta FROM docs_cloudsync
 WHERE pk = cloudsync_pk_encode('d1') AND col_name LIKE 'body' || chr(31) || '%' AND col_version % 2 = 1 \gset
SELECT (:stored_blocks::int = 1 AND :live_meta::int = 1) AS source_ok \gset
\if :source_ok
\echo [PASS] (:testid) shorter rewrite leaves one stored block and one live block
\else
\echo [FAIL] (:testid) shorter rewrite left :stored_blocks stored block(s), :live_meta live
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload2
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND db_version > :before_short::bigint \gset
\connect cloudsync_test_64_target
SELECT cloudsync_payload_apply(decode(:'payload2', 'hex')) AS _apply2 \gset
SELECT cloudsync_text_materialize('docs', 'body', cloudsync_pk_encode('d1')) AS _mat2 \gset
SELECT (SELECT body FROM docs WHERE id = 'd1') = 'ZZZ' AS target_ok \gset
SELECT count(*) AS target_blocks FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('d1') \gset
\if :target_ok
\echo [PASS] (:testid) peer holds the shorter value
\else
\echo [FAIL] (:testid) peer body is [:target_ok] with :target_blocks stored block(s)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- An upsert of the whole row goes through the same path.
\connect cloudsync_test_64_source
INSERT INTO docs (id, body) VALUES ('d1', 'K1' || chr(10) || 'K2' || chr(10) || 'K3')
  ON CONFLICT (id) DO UPDATE SET body = excluded.body;
INSERT INTO docs (id, body) VALUES ('d1', 'K1')
  ON CONFLICT (id) DO UPDATE SET body = excluded.body;
SELECT count(*) AS upsert_blocks FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('d1') \gset
SELECT (:upsert_blocks::int = 1) AS upsert_ok \gset
\if :upsert_ok
\echo [PASS] (:testid) upsert to a shorter value keeps one stored block
\else
\echo [FAIL] (:testid) upsert to a shorter value left :upsert_blocks stored block(s)
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_64_source;
DROP DATABASE IF EXISTS cloudsync_test_64_target;
