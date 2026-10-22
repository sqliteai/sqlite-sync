-- Payload chunks with NULL values
--
-- A change row can carry a NULL col_value: a block whose line was edited away or
-- removed, the blocks left behind by a primary-key change, a column set to NULL, a
-- deleted row. Every PostgreSQL builder must encode those rows like any other and the
-- result must apply on a peer: cloudsync_payload_chunks (plain, positional resume,
-- window cap, next to a fragmented value) and cloudsync_payload_blob_checked.

\set testid '73-null-values'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_73_source;
DROP DATABASE IF EXISTS cloudsync_test_73_plain;
DROP DATABASE IF EXISTS cloudsync_test_73_positional;
DROP DATABASE IF EXISTS cloudsync_test_73_capped;
DROP DATABASE IF EXISTS cloudsync_test_73_checked;
CREATE DATABASE cloudsync_test_73_source;
CREATE DATABASE cloudsync_test_73_plain;
CREATE DATABASE cloudsync_test_73_positional;
CREATE DATABASE cloudsync_test_73_capped;
CREATE DATABASE cloudsync_test_73_checked;

\connect cloudsync_test_73_source
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, title TEXT, body TEXT);
SELECT cloudsync_init('docs', 'CLS', 1) AS _init \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol \gset
CREATE TABLE blobs (id TEXT PRIMARY KEY NOT NULL, data BYTEA);
SELECT cloudsync_init('blobs', 'CLS', 1) AS _init_blobs \gset
SELECT cloudsync_set('payload_max_chunk_size', '262144') AS _chunk \gset

-- Block edit: the middle line is replaced, its old block has no stored value.
INSERT INTO docs VALUES ('d1', 't1', 'AAA' || chr(10) || 'BBB' || chr(10) || 'CCC');
UPDATE docs SET body = 'AAA' || chr(10) || 'XXX' || chr(10) || 'CCC' WHERE id = 'd1';
-- Block removal: lines removed, their blocks have no stored value.
INSERT INTO docs VALUES ('d2', 't2', 'L1' || chr(10) || 'L2' || chr(10) || 'L3' || chr(10) || 'L4');
UPDATE docs SET body = 'L1' || chr(10) || 'L4' WHERE id = 'd2';
-- Primary-key change on a row with blocks.
INSERT INTO docs VALUES ('d3', 't3', 'P1' || chr(10) || 'P2');
UPDATE docs SET id = 'd3-moved' WHERE id = 'd3';
-- Plain column set to NULL, block column emptied to NULL.
INSERT INTO docs VALUES ('d4', 't4', 'N1' || chr(10) || 'N2');
UPDATE docs SET title = NULL, body = NULL WHERE id = 'd4';
-- Deleted row.
INSERT INTO docs VALUES ('d5', 't5', 'D1' || chr(10) || 'D2');
DELETE FROM docs WHERE id = 'd5';
-- A value larger than the chunk size, between NULL-valued rows.
INSERT INTO blobs VALUES ('big', decode((SELECT string_agg(md5(j::text), '') FROM generate_series(1, 30000) AS s(j)), 'hex'));
UPDATE docs SET body = 'AAA' || chr(10) || 'CCC' WHERE id = 'd1';

SELECT count(*) AS null_rows FROM cloudsync_changes_select(0, NULL) WHERE col_value IS NULL \gset
SELECT (:null_rows::int > 0) AS has_null_rows \gset
\if :has_null_rows
\echo [PASS] (:testid) history contains change rows with NULL values (:null_rows)
\else
\echo [FAIL] (:testid) history has no change rows with NULL values
SELECT (:fail::int + 1) AS fail \gset
\endif

-- The moved row is compared by id and title.
SELECT md5(string_agg(id || ':' || coalesce(title, '<null>') || ':' || CASE WHEN id = 'd3-moved' THEN '' ELSE coalesce(body, '<null>') END, '|' ORDER BY id)) AS src_docs,
       count(*) AS src_count FROM docs \gset
SELECT md5(string_agg(id || ':' || encode(data, 'hex'), '|' ORDER BY id)) AS src_blobs FROM blobs \gset

-- Plain: every chunk of the window in one call.
SELECT string_agg(encode(payload, 'hex'), ',' ORDER BY chunk_index) AS plain_hex,
       count(*) AS plain_chunks,
       bool_or(next_frag_offset > 0) AS plain_fragmented
FROM cloudsync_payload_chunks(0) \gset
SELECT (:plain_chunks::int >= 3 AND :'plain_fragmented'::bool) AS plain_ok \gset
\if :plain_ok
\echo [PASS] (:testid) payload chunks with NULL values encode next to a fragmented value
\else
\echo [FAIL] (:testid) payload chunks with NULL values: :plain_chunks chunk(s), fragmented=:plain_fragmented
SELECT (:fail::int + 1) AS fail \gset
\endif

-- A NULL value encodes as the NULL marker, exactly as the aggregate encoder writes it.
SELECT (SELECT payload FROM cloudsync_payload_chunks(0) ORDER BY chunk_index LIMIT 1) =
       (SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq ORDER BY db_version, seq)
          FROM cloudsync_changes_select(0, cloudsync_siteid())
         WHERE db_version < (SELECT min(db_version) FROM blobs_cloudsync)) AS same_as_aggregate \gset
\if :same_as_aggregate
\echo [PASS] (:testid) payload chunks with NULL values match the aggregate encoder byte-for-byte
\else
\echo [FAIL] (:testid) payload chunks with NULL values differ from the aggregate encoder
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Positional resume at every row of the window, NULL-valued rows included.
SELECT max(db_version) AS wm FROM cloudsync_changes_select(0, cloudsync_siteid()) \gset
SELECT bool_and(r.ok) AS resume_every_row_ok FROM (
  SELECT (SELECT count(*) > 0 FROM cloudsync_payload_chunks(NULL, cloudsync_siteid(), :wm, false, c.db_version, c.seq, 0)) AS ok
  FROM cloudsync_changes_select(0, cloudsync_siteid()) c
) r \gset
\if :resume_every_row_ok
\echo [PASS] (:testid) payload chunks with NULL values resume at every row
\else
\echo [FAIL] (:testid) payload chunks with NULL values did not resume at every row
SELECT (:fail::int + 1) AS fail \gset
\endif

-- A fragment offset pointing at a NULL-valued row is rejected with an error.
CREATE OR REPLACE FUNCTION _null_frag_resume(wm bigint) RETURNS text LANGUAGE plpgsql AS $$
DECLARE dbv bigint; sq bigint; n bigint;
BEGIN
  SELECT db_version, seq INTO dbv, sq FROM cloudsync_changes_select(0, cloudsync_siteid())
   WHERE col_value IS NULL ORDER BY db_version, seq LIMIT 1;
  SELECT count(*) INTO n FROM cloudsync_payload_chunks(NULL, cloudsync_siteid(), wm, false, dbv, sq, 10);
  RETURN 'accepted';
EXCEPTION WHEN OTHERS THEN
  RETURN SQLERRM;
END $$;
SELECT _null_frag_resume(:wm) AS frag_resume_result \gset
SELECT (:'frag_resume_result' LIKE '%NULL value%') AS frag_resume_ok \gset
\if :frag_resume_ok
\echo [PASS] (:testid) payload chunks reject a fragment offset on a NULL value
\else
\echo [FAIL] (:testid) payload chunks fragment offset on a NULL value: :'frag_resume_result'
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Positional drain: one chunk per call, resuming at each reported cursor.
CREATE OR REPLACE FUNCTION _positional_drain_hex() RETURNS text LANGUAGE plpgsql AS $$
DECLARE
  rdbv bigint; rseq bigint; rfrag bigint; wm bigint := 0;
  rec record; parts text[] := '{}'; guard int := 0;
BEGIN
  LOOP
    guard := guard + 1;
    IF guard > 10000 THEN RAISE EXCEPTION 'positional drain did not terminate'; END IF;
    IF wm = 0 THEN
      SELECT * INTO rec FROM cloudsync_payload_chunks(0, cloudsync_siteid(), NULL, false)
      ORDER BY chunk_index LIMIT 1;
      IF NOT FOUND THEN EXIT; END IF;
      wm := rec.watermark_db_version;
    ELSE
      SELECT * INTO rec FROM cloudsync_payload_chunks(NULL, cloudsync_siteid(), wm, false, rdbv, rseq, rfrag)
      ORDER BY chunk_index LIMIT 1;
      IF NOT FOUND THEN EXIT; END IF;
    END IF;
    parts := array_append(parts, encode(rec.payload, 'hex'));
    rdbv := rec.next_db_version; rseq := rec.next_seq; rfrag := rec.next_frag_offset;
    EXIT WHEN rec.is_final;
  END LOOP;
  RETURN array_to_string(parts, ',');
END $$;
SELECT _positional_drain_hex() AS positional_hex \gset

-- Window cap: the smallest budget ends every window at the first db_version boundary.
CREATE OR REPLACE FUNCTION _capped_drain_hex(OUT hex text, OUT windows int) LANGUAGE plpgsql AS $$
DECLARE
  since bigint := 0; rec record; parts text[] := '{}'; got boolean;
BEGIN
  windows := 0;
  LOOP
    IF windows > 1000 THEN RAISE EXCEPTION 'capped drain did not terminate'; END IF;
    got := false;
    FOR rec IN SELECT * FROM cloudsync_payload_chunks(since, cloudsync_siteid(), NULL, false, NULL, NULL, NULL, 1)
               ORDER BY chunk_index LOOP
      parts := array_append(parts, encode(rec.payload, 'hex'));
      IF rec.is_final THEN since := rec.watermark_db_version; END IF;
      got := true;
    END LOOP;
    EXIT WHEN NOT got;
    windows := windows + 1;
  END LOOP;
  hex := array_to_string(parts, ',');
END $$;
SELECT hex AS capped_hex, windows AS capped_windows FROM _capped_drain_hex() \gset
SELECT (:capped_windows::int > 1) AS capped_ok \gset
\if :capped_ok
\echo [PASS] (:testid) payload chunks with NULL values drain through capped windows (:capped_windows)
\else
\echo [FAIL] (:testid) payload chunks with NULL values: capped drain produced :capped_windows window(s)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- cloudsync_payload_blob_checked over the same history.
SELECT encode(cloudsync_payload_blob_checked(0, 0, NULL, false, 1073741824), 'hex') AS checked_hex \gset

-- Apply each stream into its own fresh database and compare with the source.
\connect cloudsync_test_73_plain
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, title TEXT, body TEXT);
SELECT cloudsync_init('docs', 'CLS', 1) AS _init \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol \gset
CREATE TABLE blobs (id TEXT PRIMARY KEY NOT NULL, data BYTEA);
SELECT cloudsync_init('blobs', 'CLS', 1) AS _init_blobs \gset
SELECT coalesce(sum(cloudsync_payload_apply(decode(h, 'hex'))), 0) AS _applied
FROM unnest(string_to_array(:'plain_hex', ',')) WITH ORDINALITY AS t(h, ord) \gset
SELECT count(*) AS _mat FROM (SELECT cloudsync_text_materialize('docs', 'body', id) FROM docs) m \gset
SELECT md5(string_agg(id || ':' || coalesce(title, '<null>') || ':' || CASE WHEN id = 'd3-moved' THEN '' ELSE coalesce(body, '<null>') END, '|' ORDER BY id)) AS dst_docs,
       count(*) AS dst_count FROM docs \gset
SELECT md5(string_agg(id || ':' || encode(data, 'hex'), '|' ORDER BY id)) AS dst_blobs FROM blobs \gset
SELECT (:'dst_docs' = :'src_docs' AND :dst_count::int = :src_count::int AND :'dst_blobs' = :'src_blobs') AS plain_apply_ok \gset
\if :plain_apply_ok
\echo [PASS] (:testid) payload chunks with NULL values: peer matches the source (:dst_count rows)
\else
\echo [FAIL] (:testid) payload chunks with NULL values: peer differs from the source (:dst_count of :src_count rows)
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect cloudsync_test_73_positional
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, title TEXT, body TEXT);
SELECT cloudsync_init('docs', 'CLS', 1) AS _init \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol \gset
CREATE TABLE blobs (id TEXT PRIMARY KEY NOT NULL, data BYTEA);
SELECT cloudsync_init('blobs', 'CLS', 1) AS _init_blobs \gset
SELECT coalesce(sum(cloudsync_payload_apply(decode(h, 'hex'))), 0) AS _applied
FROM unnest(string_to_array(:'positional_hex', ',')) WITH ORDINALITY AS t(h, ord) \gset
SELECT count(*) AS _mat FROM (SELECT cloudsync_text_materialize('docs', 'body', id) FROM docs) m \gset
SELECT md5(string_agg(id || ':' || coalesce(title, '<null>') || ':' || CASE WHEN id = 'd3-moved' THEN '' ELSE coalesce(body, '<null>') END, '|' ORDER BY id)) AS dst_docs,
       count(*) AS dst_count FROM docs \gset
SELECT md5(string_agg(id || ':' || encode(data, 'hex'), '|' ORDER BY id)) AS dst_blobs FROM blobs \gset
SELECT (:'dst_docs' = :'src_docs' AND :dst_count::int = :src_count::int AND :'dst_blobs' = :'src_blobs') AS positional_apply_ok \gset
\if :positional_apply_ok
\echo [PASS] (:testid) positional payload chunks with NULL values: peer matches the source (:dst_count rows)
\else
\echo [FAIL] (:testid) positional payload chunks with NULL values: peer differs from the source (:dst_count of :src_count rows)
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect cloudsync_test_73_capped
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, title TEXT, body TEXT);
SELECT cloudsync_init('docs', 'CLS', 1) AS _init \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol \gset
CREATE TABLE blobs (id TEXT PRIMARY KEY NOT NULL, data BYTEA);
SELECT cloudsync_init('blobs', 'CLS', 1) AS _init_blobs \gset
SELECT coalesce(sum(cloudsync_payload_apply(decode(h, 'hex'))), 0) AS _applied
FROM unnest(string_to_array(:'capped_hex', ',')) WITH ORDINALITY AS t(h, ord) \gset
SELECT count(*) AS _mat FROM (SELECT cloudsync_text_materialize('docs', 'body', id) FROM docs) m \gset
SELECT md5(string_agg(id || ':' || coalesce(title, '<null>') || ':' || CASE WHEN id = 'd3-moved' THEN '' ELSE coalesce(body, '<null>') END, '|' ORDER BY id)) AS dst_docs,
       count(*) AS dst_count FROM docs \gset
SELECT md5(string_agg(id || ':' || encode(data, 'hex'), '|' ORDER BY id)) AS dst_blobs FROM blobs \gset
SELECT (:'dst_docs' = :'src_docs' AND :dst_count::int = :src_count::int AND :'dst_blobs' = :'src_blobs') AS capped_apply_ok \gset
\if :capped_apply_ok
\echo [PASS] (:testid) capped payload chunks with NULL values: peer matches the source (:dst_count rows)
\else
\echo [FAIL] (:testid) capped payload chunks with NULL values: peer differs from the source (:dst_count of :src_count rows)
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect cloudsync_test_73_checked
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, title TEXT, body TEXT);
SELECT cloudsync_init('docs', 'CLS', 1) AS _init \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol \gset
CREATE TABLE blobs (id TEXT PRIMARY KEY NOT NULL, data BYTEA);
SELECT cloudsync_init('blobs', 'CLS', 1) AS _init_blobs \gset
SELECT coalesce(sum(cloudsync_payload_apply(decode(h, 'hex'))), 0) AS _applied
FROM unnest(string_to_array(:'checked_hex', ',')) WITH ORDINALITY AS t(h, ord) \gset
SELECT count(*) AS _mat FROM (SELECT cloudsync_text_materialize('docs', 'body', id) FROM docs) m \gset
SELECT md5(string_agg(id || ':' || coalesce(title, '<null>') || ':' || CASE WHEN id = 'd3-moved' THEN '' ELSE coalesce(body, '<null>') END, '|' ORDER BY id)) AS dst_docs,
       count(*) AS dst_count FROM docs \gset
SELECT md5(string_agg(id || ':' || encode(data, 'hex'), '|' ORDER BY id)) AS dst_blobs FROM blobs \gset
SELECT (:'dst_docs' = :'src_docs' AND :dst_count::int = :src_count::int AND :'dst_blobs' = :'src_blobs') AS checked_apply_ok \gset
\if :checked_apply_ok
\echo [PASS] (:testid) payload blob checked with NULL values: peer matches the source (:dst_count rows)
\else
\echo [FAIL] (:testid) payload blob checked with NULL values: peer differs from the source (:dst_count of :src_count rows)
SELECT (:fail::int + 1) AS fail \gset
\endif

\ir helper_test_cleanup.sql
\if :should_cleanup
\connect postgres
DROP DATABASE IF EXISTS cloudsync_test_73_source;
DROP DATABASE IF EXISTS cloudsync_test_73_plain;
DROP DATABASE IF EXISTS cloudsync_test_73_positional;
DROP DATABASE IF EXISTS cloudsync_test_73_capped;
DROP DATABASE IF EXISTS cloudsync_test_73_checked;
\endif
