-- A receive window (exclude_filter_site_id) whose changes are all hidden from the caller
-- by row-level security, or are all the caller's own, ends with one chunk carrying no
-- rows, so the caller still checkpoints at the watermark instead of scanning the same
-- window on every request. A window with nothing new, and the send path, still return
-- no chunk at all.

\set testid '67-empty-final'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_67;
CREATE DATABASE cloudsync_test_67;
DO $$ BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'empty_final_reader') THEN
        CREATE ROLE empty_final_reader NOLOGIN;
    END IF;
END $$;

\connect cloudsync_test_67
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE notes (id TEXT PRIMARY KEY NOT NULL, owner TEXT, body TEXT);
SELECT cloudsync_init('notes') AS _init \gset
INSERT INTO notes VALUES ('n1', 'someone_else', 'a');
INSERT INTO notes VALUES ('n2', 'someone_else', 'b');

ALTER TABLE notes ENABLE ROW LEVEL SECURITY;
CREATE POLICY owner_select ON notes FOR SELECT USING (owner = current_user);
GRANT USAGE ON SCHEMA public TO empty_final_reader;
GRANT ALL ON ALL TABLES IN SCHEMA public TO empty_final_reader;
GRANT ALL ON ALL SEQUENCES IN SCHEMA public TO empty_final_reader;
GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA public TO empty_final_reader;

SELECT max(db_version) AS wm FROM notes_cloudsync \gset
-- a site that never wrote anything to this database
\set fresh 'decode(''aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa01'', ''hex'')'

-- Visible changes (the table owner bypasses the policy): ordinary chunks, none empty.
SELECT (count(*) > 0 AND count(*) FILTER (WHERE rows = 0) = 0 AND max(watermark_db_version) = :wm) AS visible_ok
FROM cloudsync_payload_chunks(0, :fresh, NULL, true) \gset
\if :visible_ok
\echo [PASS] (:testid) visible changes produce ordinary chunks only
\else
\echo [FAIL] (:testid) visible changes did not produce ordinary chunks ending at :wm
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Every change hidden by the policy: one empty final chunk at the watermark.
SET ROLE empty_final_reader;
SELECT (count(*) = 1 AND max(rows) = 0 AND bool_and(is_final) AND NOT bool_or(window_capped)
        AND max(watermark_db_version) = :wm AND max(next_db_version) = :wm
        AND max(payload_size) = 32 AND max(get_byte(payload, 4)) = 2) AS hidden_ok
FROM cloudsync_payload_chunks(0, :fresh, NULL, true) \gset
RESET ROLE;
\if :hidden_ok
\echo [PASS] (:testid) a window hidden by RLS ends with an empty final chunk at the watermark
\else
\echo [FAIL] (:testid) a window hidden by RLS did not end with an empty final chunk at :wm
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Positional resume with nothing left before the watermark: the stream still ends.
SET ROLE empty_final_reader;
SELECT (count(*) = 1 AND max(rows) = 0 AND bool_and(is_final) AND max(watermark_db_version) = :wm) AS positional_ok
FROM cloudsync_payload_chunks(NULL, :fresh, :wm, true, :wm, 0, 0) \gset
RESET ROLE;
\if :positional_ok
\echo [PASS] (:testid) a positional resume with nothing left ends with an empty final chunk
\else
\echo [FAIL] (:testid) a positional resume with nothing left did not end the stream
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Nothing new since the caller's checkpoint: no chunk.
SET ROLE empty_final_reader;
SELECT count(*) AS nothing_new_chunks FROM cloudsync_payload_chunks(:wm, :fresh, NULL, true) \gset
RESET ROLE;
SELECT (:nothing_new_chunks::int = 0) AS nothing_new_ok \gset
\if :nothing_new_ok
\echo [PASS] (:testid) a window with nothing new returns no chunk
\else
\echo [FAIL] (:testid) a window with nothing new returned :nothing_new_chunks chunks
SELECT (:fail::int + 1) AS fail \gset
\endif

-- The window holds only the caller's own changes: empty final chunk at the watermark.
SELECT (count(*) = 1 AND max(rows) = 0 AND bool_and(is_final) AND max(watermark_db_version) = :wm) AS own_ok
FROM cloudsync_payload_chunks(0, cloudsync_siteid(), :wm, true) \gset
\if :own_ok
\echo [PASS] (:testid) a window of only the callers own changes ends with an empty final chunk
\else
\echo [FAIL] (:testid) a window of only the callers own changes did not end with an empty final chunk
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Send path (no exclude_filter_site_id): never an empty chunk.
SELECT count(*) AS send_chunks FROM cloudsync_payload_chunks(:wm, NULL, :wm + 5, false) \gset
SELECT (:send_chunks::int = 0) AS send_ok \gset
\if :send_ok
\echo [PASS] (:testid) the send path never returns an empty chunk
\else
\echo [FAIL] (:testid) the send path returned :send_chunks chunks for an empty window
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_67;
