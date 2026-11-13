-- 'Site id lookup during apply'
-- Applying changes from a site records it once in cloudsync_site_id, also when two applies
-- bring the same new site at the same time.

\set testid '79'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_79_src;
DROP DATABASE IF EXISTS cloudsync_test_79_dst;
CREATE DATABASE cloudsync_test_79_src;
CREATE DATABASE cloudsync_test_79_dst;

-- Source: two transactions, so two payloads from the same site
\connect cloudsync_test_79_src
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE items (id TEXT PRIMARY KEY NOT NULL, name TEXT);
SELECT cloudsync_init('items') AS _init \gset
INSERT INTO items VALUES ('r1', 'one'), ('r2', 'two');
INSERT INTO items VALUES ('r3', 'three');
SELECT encode(cloudsync_siteid(), 'hex') AS src_site \gset
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload1
FROM cloudsync_changes WHERE db_version = 1 \gset
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload2
FROM cloudsync_changes WHERE db_version = 2 \gset

\connect cloudsync_test_79_dst
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE EXTENSION IF NOT EXISTS dblink;
CREATE TABLE items (id TEXT PRIMARY KEY NOT NULL, name TEXT);
SELECT cloudsync_init('items') AS _init \gset

-- Two applies of the same new site at once: this session records the site and keeps its
-- transaction open, as the first apply does; the apply in the other session has to wait for it
SELECT dblink_connect('peer', format('dbname=%s user=%s', current_database(), current_user)) AS _peer \gset
SELECT dblink_connect('mon', format('dbname=%s user=%s', current_database(), current_user)) AS _mon \gset
BEGIN;
INSERT INTO cloudsync_site_id (site_id) VALUES (decode(:'src_site', 'hex'));
SELECT dblink_send_query('peer', format('SELECT cloudsync_payload_apply(decode(%L, %L))', :'payload2', 'hex')) AS _sent \gset
DO $$
DECLARE waited boolean := false;
BEGIN
    FOR i IN 1..100 LOOP
        SELECT EXISTS (SELECT 1 FROM dblink('mon', format(
            'SELECT 1 FROM pg_stat_activity WHERE %s = ANY (pg_blocking_pids(pid))', pg_backend_pid())) AS t(x int))
        INTO waited;
        EXIT WHEN waited;
        PERFORM pg_sleep(0.05);
    END LOOP;
    PERFORM set_config('cloudsync_test.peer_waited', waited::text, false);
END $$;
COMMIT;
SELECT coalesce(current_setting('cloudsync_test.peer_waited', true), 'false') AS peer_waited \gset
-- An error in the other session's apply is reported here: keep going and count it as a failure
\unset peer_applied
\set ON_ERROR_STOP off
SELECT * FROM dblink_get_result('peer') AS t(applied int) \gset peer_
SELECT count(*) AS _drain FROM dblink_get_result('peer') AS t(applied int) \gset
\set ON_ERROR_STOP on
SELECT dblink_disconnect('peer') AS _d1 \gset
SELECT dblink_disconnect('mon') AS _d2 \gset
SELECT cloudsync_payload_apply(decode(:'payload1', 'hex')) AS _apply1 \gset

\set site_rows -1
\set item_rows -1
\set meta_rows -1
\set all_meta_rows -1
SELECT (SELECT count(*) FROM cloudsync_site_id WHERE site_id = decode(:'src_site', 'hex')) AS site_rows,
       (SELECT count(*) FROM items) AS item_rows,
       (SELECT count(*) FROM items_cloudsync
         WHERE site_id = (SELECT id FROM cloudsync_site_id WHERE site_id = decode(:'src_site', 'hex'))) AS meta_rows,
       (SELECT count(*) FROM items_cloudsync) AS all_meta_rows \gset
SELECT (:{?peer_applied} AND :'peer_waited'::boolean AND :site_rows::int = 1 AND :item_rows::int = 3
        AND :meta_rows::int = :all_meta_rows::int AND :all_meta_rows::int > 0) AS concurrent_ok \gset
\if :concurrent_ok
\echo [PASS] (:testid) two applies of the same new site record it once
\else
\echo [FAIL] (:testid) two applies of the same new site record it once (waited :peer_waited, applied :peer_applied, site rows :site_rows, items :item_rows, meta :meta_rows of :all_meta_rows)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- A later apply from the known site reuses its row
\connect cloudsync_test_79_src
\ir helper_psql_conn_setup.sql
UPDATE items SET name = 'one again' WHERE id = 'r1';
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload3
FROM cloudsync_changes WHERE db_version = 3 \gset
\connect cloudsync_test_79_dst
\ir helper_psql_conn_setup.sql
\set site_id_before -1
SELECT id AS site_id_before FROM cloudsync_site_id WHERE site_id = decode(:'src_site', 'hex') \gset
SELECT cloudsync_payload_apply(decode(:'payload3', 'hex')) AS _apply3 \gset
SELECT ((SELECT name FROM items WHERE id = 'r1') = 'one again'
        AND (SELECT count(*) FROM cloudsync_site_id WHERE site_id = decode(:'src_site', 'hex')) = 1
        AND (SELECT id FROM cloudsync_site_id WHERE site_id = decode(:'src_site', 'hex')) = :site_id_before) AS known_site_ok \gset
\if :known_site_ok
\echo [PASS] (:testid) an apply from a known site reuses its row
\else
\echo [FAIL] (:testid) an apply from a known site reuses its row
SELECT (:fail::int + 1) AS fail \gset
\endif

\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_79_src;
DROP DATABASE IF EXISTS cloudsync_test_79_dst;
\endif
