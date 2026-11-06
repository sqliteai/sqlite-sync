-- 'Block columns for a role without CREATE'
-- A new session whose first cloudsync call runs as a role that may not create tables
-- (a sync role with the documented grants) writes, applies and reads block columns.

\set testid '76'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_76_src;
DROP DATABASE IF EXISTS cloudsync_test_76_dst;
CREATE DATABASE cloudsync_test_76_src;
CREATE DATABASE cloudsync_test_76_dst;
DO $$ BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'test_block_sync_role') THEN
        CREATE ROLE test_block_sync_role NOLOGIN;
    END IF;
END $$;

-- Source: a peer with block content
\connect cloudsync_test_76_src
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('docs') AS _init \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol \gset
INSERT INTO docs VALUES ('peer1', E'p1\np2\np3');
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS peer_payload
FROM cloudsync_changes \gset

-- Destination: configured by its owner, used by a role with table privileges only
\connect cloudsync_test_76_dst
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE docs (id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('docs') AS _init \gset
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block') AS _setcol \gset
GRANT USAGE ON SCHEMA public TO test_block_sync_role;
GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO test_block_sync_role;
GRANT USAGE, SELECT, UPDATE ON ALL SEQUENCES IN SCHEMA public TO test_block_sync_role;
SELECT has_schema_privilege('test_block_sync_role', 'public', 'CREATE') AS role_can_create \gset

-- New connection: the session's context is loaded by the sync role
\set d1_body ''
\set peer1_body ''
\set block_changes 0
\connect cloudsync_test_76_dst
\ir helper_psql_conn_setup.sql
SET ROLE test_block_sync_role;
INSERT INTO docs VALUES ('d1', E'a\nb\nc');
UPDATE docs SET body = E'a\nB\nc\nd' WHERE id = 'd1';
SELECT cloudsync_payload_apply(decode(:'peer_payload', 'hex')) AS applied \gset
SELECT body AS d1_body FROM docs WHERE id = 'd1' \gset
SELECT body AS peer1_body FROM docs WHERE id = 'peer1' \gset
SELECT count(*) AS block_changes FROM cloudsync_changes WHERE col_name LIKE 'body' || chr(31) || '%' \gset
RESET ROLE;

SELECT (NOT :'role_can_create'::boolean
        AND :'d1_body' = E'a\nB\nc\nd'
        AND :'peer1_body' = E'p1\np2\np3'
        AND :block_changes::int > 0) AS block_role_ok \gset
\if :block_role_ok
\echo [PASS] (:testid) a role without CREATE writes, applies and reads block columns in a new session
\else
\echo [FAIL] (:testid) a role without CREATE writes, applies and reads block columns in a new session
SELECT (:fail::int + 1) AS fail \gset
\endif

\ir helper_test_cleanup.sql
\if :should_cleanup
\connect postgres
DROP DATABASE IF EXISTS cloudsync_test_76_src;
DROP DATABASE IF EXISTS cloudsync_test_76_dst;
\endif
