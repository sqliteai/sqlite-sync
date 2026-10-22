-- Changes respect row visibility for every change type: the sentinel of a live row
-- (resurrected row, new pk after a pk change, row of a pk-only table) and block-column
-- values are returned only when the caller can read the base row, like column changes.
-- Delete sentinels (even causal length) have no base row and are always returned.

\set testid '75-changes-visibility'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_75;
CREATE DATABASE cloudsync_test_75;
DO $$ BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'changes_vis_reader') THEN
        CREATE ROLE changes_vis_reader NOLOGIN;
    END IF;
END $$;

\connect cloudsync_test_75
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE items (id TEXT PRIMARY KEY NOT NULL, owner TEXT NOT NULL DEFAULT '', title TEXT, body TEXT);
CREATE TABLE notes (id TEXT PRIMARY KEY NOT NULL, owner TEXT NOT NULL DEFAULT '', title TEXT);
CREATE TABLE tags (owner TEXT NOT NULL, tag TEXT NOT NULL, PRIMARY KEY (owner, tag));
SELECT cloudsync_init('items') AS _init \gset
SELECT cloudsync_set_column('items', 'body', 'algo', 'block') AS _setcol \gset
SELECT cloudsync_init('notes') AS _init \gset
SELECT cloudsync_init('tags') AS _init \gset

-- the same history for a visible owner (changes_vis_reader) and a hidden one (other_owner)
DO $$
DECLARE o TEXT; p TEXT;
BEGIN
    FOREACH o IN ARRAY ARRAY['changes_vis_reader', 'other_owner'] LOOP
        p := CASE WHEN o = 'other_owner' THEN 'other' ELSE 'mine' END;
        INSERT INTO items VALUES (p || '_live', o, 'title', E'line1\nline2');
        UPDATE items SET body = E'line1\nline2\nline3' WHERE id = p || '_live';
        INSERT INTO items VALUES (p || '_del', o, 'title', 'body');
        DELETE FROM items WHERE id = p || '_del';
        INSERT INTO items VALUES (p || '_res', o, 'v1', 'x');
        DELETE FROM items WHERE id = p || '_res';
        INSERT INTO items VALUES (p || '_res', o, 'v2', E'res1\nres2');
        INSERT INTO notes VALUES (p || '_old', o, 'title');
        UPDATE notes SET id = p || '_new' WHERE id = p || '_old';
        INSERT INTO tags VALUES (o, p || '_tag');
        INSERT INTO tags VALUES (o, p || '_tag_deleted');
        DELETE FROM tags WHERE tag = p || '_tag_deleted';
    END LOOP;
END $$;

ALTER TABLE items ENABLE ROW LEVEL SECURITY;
ALTER TABLE notes ENABLE ROW LEVEL SECURITY;
ALTER TABLE tags ENABLE ROW LEVEL SECURITY;
CREATE POLICY owner_select ON items FOR SELECT USING (owner = current_user);
CREATE POLICY owner_select ON notes FOR SELECT USING (owner = current_user);
CREATE POLICY owner_select ON tags FOR SELECT USING (owner = current_user);
GRANT USAGE ON SCHEMA public TO changes_vis_reader;
GRANT ALL ON ALL TABLES IN SCHEMA public TO changes_vis_reader;
GRANT ALL ON ALL SEQUENCES IN SCHEMA public TO changes_vis_reader;
GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA public TO changes_vis_reader;

\set change_rows '(SELECT tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq, cloudsync_pk_decode(pk, 1)::text AS k1 FROM cloudsync_changes_select(0, NULL)) c'

-- Reference (table owner, not subject to the policy): the rows the reader is expected to
-- receive are those of its own base rows plus every delete sentinel.
SELECT md5(string_agg(tbl || ':' || encode(pk, 'hex') || ':' || col_name || ':' || coalesce(encode(col_value, 'hex'), 'N') || ':' ||
                      col_version || ':' || db_version || ':' || encode(site_id, 'hex') || ':' || cl || ':' || seq,
                      '|' ORDER BY db_version, seq, tbl, pk, col_name)) AS expected_digest,
       count(*) AS expected_rows
FROM :change_rows
WHERE k1 LIKE 'mine\_%' OR k1 = 'changes_vis_reader' OR (col_name = '__[RIP]__' AND cl % 2 = 0) \gset

SET ROLE changes_vis_reader;
SELECT md5(string_agg(tbl || ':' || encode(pk, 'hex') || ':' || col_name || ':' || coalesce(encode(col_value, 'hex'), 'N') || ':' ||
                      col_version || ':' || db_version || ':' || encode(site_id, 'hex') || ':' || cl || ':' || seq,
                      '|' ORDER BY db_version, seq, tbl, pk, col_name)) AS reader_digest,
       count(*) AS reader_rows
FROM :change_rows \gset
CREATE TEMP TABLE reader_changes AS SELECT * FROM :change_rows;
RESET ROLE;

SELECT (:'reader_digest' = :'expected_digest') AS digest_ok \gset
\if :digest_ok
\echo [PASS] (:testid) changes respect visibility: the reader receives exactly its rows and every delete, with identical bytes
\else
\echo [FAIL] (:testid) reader changes differ from the expected set (:reader_rows rows vs :expected_rows expected)
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT count(*) AS hidden_rows FROM reader_changes
WHERE (tbl IN ('items', 'notes') AND k1 IN ('other_live', 'other_res', 'other_new')) OR (tbl = 'tags' AND k1 = 'other_owner' AND cl % 2 = 1) \gset
SELECT (:hidden_rows::int = 0) AS hidden_ok \gset
\if :hidden_ok
\echo [PASS] (:testid) changes respect visibility for resurrected rows, pk changes, pk-only tables and block columns
\else
\echo [FAIL] (:testid) :hidden_rows changes of rows hidden from the reader were returned
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (count(*) FILTER (WHERE tbl = 'items' AND k1 = 'mine_res' AND col_name = '__[RIP]__' AND cl = 3) = 1
    AND count(*) FILTER (WHERE tbl = 'notes' AND k1 = 'mine_new' AND col_name = '__[RIP]__' AND cl = 1) = 1
    AND count(*) FILTER (WHERE tbl = 'tags' AND k1 = 'changes_vis_reader' AND cl = 1) = 1
    AND count(*) FILTER (WHERE tbl = 'items' AND k1 = 'mine_live' AND col_name LIKE 'body' || chr(31) || '%' AND col_value IS NOT NULL) > 0
    ) AS visible_ok
FROM reader_changes \gset
\if :visible_ok
\echo [PASS] (:testid) changes of visible resurrected rows, pk changes, pk-only rows and block columns are returned
\else
\echo [FAIL] (:testid) changes of visible rows are missing
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (count(*) FILTER (WHERE tbl IN ('items', 'notes') AND k1 IN ('other_del', 'other_old', 'mine_del', 'mine_old') AND col_name = '__[RIP]__' AND cl = 2) = 4
    AND count(*) FILTER (WHERE tbl = 'tags' AND col_name = '__[RIP]__' AND cl = 2) = 2
    AND bool_and(col_value = cloudsync_encode_value(NULL::text)) FILTER (WHERE col_name = '__[RIP]__')
    ) AS deletes_ok
FROM reader_changes \gset
\if :deletes_ok
\echo [PASS] (:testid) delete sentinels are returned unchanged
\else
\echo [FAIL] (:testid) delete sentinels are not returned unchanged
SELECT (:fail::int + 1) AS fail \gset
\endif

-- The payload stream of a receive carries the same rows.
SET ROLE changes_vis_reader;
SELECT coalesce(sum(rows), 0) AS chunk_rows
FROM cloudsync_payload_chunks(0, decode('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa68', 'hex'), NULL, true) \gset
RESET ROLE;
SELECT (:chunk_rows::int = :reader_rows::int) AS chunks_ok \gset
\if :chunks_ok
\echo [PASS] (:testid) payload chunks carry the same rows as the change read
\else
\echo [FAIL] (:testid) payload chunks carry :chunk_rows rows, expected :reader_rows
SELECT (:fail::int + 1) AS fail \gset
\endif

\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_75;
\endif
