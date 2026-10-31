-- Row-level security isolation of the read and write sides of CloudSync.
--
-- 1. cloudsync_changes and cloudsync_payload_chunks only return the changes of rows the
--    calling role can see: another role's rows come back as nothing, not as a restricted
--    placeholder that still names the key.
-- 2. A cloudsync_changes insert for a row the role may not write leaves the row alone,
--    for an ordinary column and for a block column, while the role's own rows apply.

\set testid '72-rls-isolation'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_72;
CREATE DATABASE cloudsync_test_72;
DO $$ BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'rls72_alice') THEN CREATE ROLE rls72_alice NOLOGIN; END IF;
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'rls72_bob') THEN CREATE ROLE rls72_bob NOLOGIN; END IF;
END $$;

\connect cloudsync_test_72
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE notes (id TEXT PRIMARY KEY NOT NULL, owner TEXT NOT NULL DEFAULT '', body TEXT);
CREATE TABLE docs  (id TEXT PRIMARY KEY NOT NULL, owner TEXT NOT NULL DEFAULT '', body TEXT);
SELECT cloudsync_init('notes') AS _n \gset
SELECT cloudsync_init('docs') AS _d \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _b \gset

INSERT INTO notes VALUES ('a1', 'rls72_alice', 'alice-note');
INSERT INTO notes VALUES ('b1', 'rls72_bob', 'secret-bob-note');
INSERT INTO docs  VALUES ('a1', 'rls72_alice', E'alice line 1\nalice line 2');
INSERT INTO docs  VALUES ('b1', 'rls72_bob', E'secret-bob line 1\nsecret-bob line 2');

ALTER TABLE notes ENABLE ROW LEVEL SECURITY;
ALTER TABLE docs ENABLE ROW LEVEL SECURITY;
CREATE POLICY own_notes ON notes USING (owner = current_user) WITH CHECK (owner = current_user);
CREATE POLICY own_docs  ON docs  USING (owner = current_user) WITH CHECK (owner = current_user);

GRANT USAGE ON SCHEMA public TO rls72_alice, rls72_bob;
GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO rls72_alice, rls72_bob;
GRANT USAGE, SELECT, UPDATE ON ALL SEQUENCES IN SCHEMA public TO rls72_alice, rls72_bob;
GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA public TO rls72_alice, rls72_bob;

-- what alice is entitled to: every change of her own rows (read as the owner of the data)
SELECT count(*) AS alice_expected FROM cloudsync_changes WHERE pk IN (cloudsync_pk_encode('a1')) \gset
SELECT count(*) AS bob_total FROM cloudsync_changes WHERE pk IN (cloudsync_pk_encode('b1')) \gset

-- 1. read side
SET ROLE rls72_alice;
SELECT count(*) AS alice_sees FROM cloudsync_changes \gset
SELECT count(*) AS alice_sees_bob FROM cloudsync_changes WHERE pk = cloudsync_pk_encode('b1') \gset
SELECT COALESCE(sum(rows), 0) AS alice_chunk_rows FROM cloudsync_payload_chunks(since_db_version => 0) \gset
RESET ROLE;
SELECT (:alice_sees = :alice_expected AND :alice_sees_bob = 0 AND :alice_chunk_rows = :alice_expected AND :bob_total > 0) AS read_ok \gset
\if :read_ok
\echo [PASS] (:testid) cloudsync_changes and cloudsync_payload_chunks return only the rows the caller can see
\else
\echo [FAIL] (:testid) read isolation: sees :alice_sees of :alice_expected (of bob: :alice_sees_bob), chunks :alice_chunk_rows
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 2. write side: alice tries to rewrite bob's note and bob's document
SET ROLE rls72_alice;
DO $$
BEGIN
    BEGIN
        INSERT INTO cloudsync_changes(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq)
        VALUES ('notes', cloudsync_pk_encode('b1'), 'body', cloudsync_encode_value('pwned'::text), 99, 99, '\x0102030405060708090a0b0c0d0e0f10', 1, 0);
    EXCEPTION WHEN OTHERS THEN NULL;
    END;
    BEGIN
        INSERT INTO cloudsync_changes(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq)
        VALUES ('docs', cloudsync_pk_encode('b1'), 'body' || chr(31) || 'a5', cloudsync_encode_value('pwned line'::text), 99, 99, '\x0102030405060708090a0b0c0d0e0f10', 1, 0);
    EXCEPTION WHEN OTHERS THEN NULL;
    END;
    -- and her own row does apply
    INSERT INTO cloudsync_changes(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq)
    VALUES ('notes', cloudsync_pk_encode('a2'), 'owner', cloudsync_encode_value('rls72_alice'::text), 1, 100, '\x0102030405060708090a0b0c0d0e0f10', 1, 0);
EXCEPTION WHEN OTHERS THEN
    PERFORM set_config('cloudsync.t72', 'own row refused: ' || SQLERRM, false);
END $$;
RESET ROLE;
SELECT (SELECT body FROM notes WHERE id = 'b1') = 'secret-bob-note' AS note_intact,
       (SELECT body FROM docs WHERE id = 'b1') NOT LIKE '%pwned%' AS doc_intact,
       (SELECT count(*) FROM docs_cloudsync_blocks WHERE col_value::text LIKE '%pwned%') AS pwned_blocks,
       ((SELECT body FROM notes WHERE id = 'b1') = 'secret-bob-note'
        AND (SELECT body FROM docs WHERE id = 'b1') NOT LIKE '%pwned%'
        AND (SELECT count(*) FROM docs_cloudsync_blocks WHERE col_value::text LIKE '%pwned%') = 0
        AND (SELECT count(*) FROM notes WHERE id = 'a2' AND owner = 'rls72_alice') = 1) AS write_ok \gset
\if :write_ok
\echo [PASS] (:testid) a role cannot write the row of another role through cloudsync_changes
\else
\echo [FAIL] (:testid) write isolation: note intact :note_intact, doc intact :doc_intact, :pwned_blocks pwned blocks
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_72;
