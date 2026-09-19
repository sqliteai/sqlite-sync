-- A large stale-fragment backlog must make bounded progress without exhausting
-- PostgreSQL's shared advisory-lock table. A value being resumed stays protected.
\set testid '61-fragment-cleanup-backlog'
\ir helper_test_init.sql
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_61;
CREATE DATABASE cloudsync_test_61;
\connect cloudsync_test_61
\ir helper_psql_conn_setup.sql
CREATE EXTENSION cloudsync;
CREATE EXTENSION dblink;
CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, value TEXT);
SELECT cloudsync_init('t') AS _init \gset
SELECT cloudsync_set('payload_max_chunk_size', '1') AS _size \gset
INSERT INTO t VALUES ('big', repeat('A', 300000));
CREATE TABLE transport AS SELECT chunk_index, payload FROM cloudsync_payload_chunks();

INSERT INTO cloudsync_payload_fragments
    (value_id, part_index, part_count, total_size, checksum, created_at,
     tbl, pk, col_name, col_version, db_version, site_id, cl, seq, fragment)
SELECT md5(i::text), 0, 2, 2, '0000000000000000',
       extract(epoch FROM now())::bigint - 172800,
       'stale', decode('00','hex'), 'value', 1, 1, decode('00','hex'), 1, 0, decode('00','hex')
FROM generate_series(1, 30000) i;
SELECT value_id AS locked_value FROM cloudsync_payload_fragments ORDER BY value_id LIMIT 1 \gset
SELECT dblink_connect('busy', format('dbname=%s user=%s', current_database(), current_user)) AS _c \gset
SELECT dblink_exec('busy', 'BEGIN') AS _b \gset
SELECT n AS _n FROM dblink('busy', format(
    'SELECT 1 FROM pg_advisory_xact_lock(1129530962, hashtext(%L))', :'locked_value')) AS r(n INT) \gset

BEGIN;
SELECT cloudsync_payload_apply(payload) AS _applied FROM transport WHERE chunk_index = 0 \gset
SELECT count(*) = 29937 AS progress_ok FROM cloudsync_payload_fragments WHERE tbl = 'stale' \gset
\if :progress_ok
\echo [PASS] (:testid) 30000 stale groups: cleanup removes 63 unlocked candidates without exhausting locks
\else
\echo [FAIL] (:testid) cleanup did not make bounded progress through the backlog
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT count(*) <= 65 AS locks_ok FROM pg_locks
WHERE pid = pg_backend_pid() AND locktype = 'advisory' AND classid = 1129530962 \gset
\if :locks_ok
\echo [PASS] (:testid) cleanup retains at most 64 locks plus the incoming value lock
\else
\echo [FAIL] (:testid) cleanup retained too many advisory locks
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT EXISTS (SELECT FROM cloudsync_payload_fragments WHERE value_id = :'locked_value')
   AND (SELECT count(*) FROM cloudsync_payload_fragments WHERE tbl = 't') = 1 AS preserved_ok \gset
\if :preserved_ok
\echo [PASS] (:testid) locked stale value and incoming incomplete value are preserved
\else
\echo [FAIL] (:testid) cleanup removed a protected or fresh value
SELECT (:fail::int + 1) AS fail \gset
\endif
COMMIT;
SELECT dblink_exec('busy', 'COMMIT') AS _c \gset
SELECT dblink_disconnect('busy') AS _d \gset

-- A new connection is eligible for maintenance immediately; an existing connection
-- is throttled to once a minute. The next batch must advance through the backlog.
\connect cloudsync_test_61
\ir helper_psql_conn_setup.sql
SELECT cloudsync_payload_apply(payload) AS _applied FROM transport WHERE chunk_index = 0 \gset
SELECT (SELECT count(*) FROM cloudsync_payload_fragments WHERE tbl = 'stale') = 29873
   AND NOT EXISTS (SELECT FROM cloudsync_payload_fragments WHERE value_id = :'locked_value') AS next_ok \gset
\if :next_ok
\echo [PASS] (:testid) the next cleanup removes another 64 groups, including the released value
\else
\echo [FAIL] (:testid) subsequent cleanup made no progress or left the released candidate
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT cloudsync_payload_apply(payload) AS _applied FROM transport WHERE chunk_index = 1 \gset
SELECT NOT EXISTS (SELECT FROM cloudsync_payload_fragments WHERE tbl = 't') AS complete_ok \gset
\if :complete_ok
\echo [PASS] (:testid) the real fragmented value still completes with a large stale backlog
\else
\echo [FAIL] (:testid) the real fragmented value did not complete
SELECT (:fail::int + 1) AS fail \gset
\endif
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE cloudsync_test_61;
