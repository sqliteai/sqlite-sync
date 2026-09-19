-- A denied v3 (fragmented) value fails the apply like any other denied change: the
-- statement that completes the value raises the error, the receive checkpoint does not
-- move, and the pieces staged by the earlier statements stay, so redelivering after the
-- policy allows the row applies the value.

\set testid '58-v3-denied'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_58_src;
DROP DATABASE IF EXISTS cloudsync_test_58_dst;
CREATE DATABASE cloudsync_test_58_src;
CREATE DATABASE cloudsync_test_58_dst;

DO $$ BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'v3_denied_user') THEN
        CREATE ROLE v3_denied_user LOGIN;
    END IF;
END $$;

-- Source: one oversized value, forced to fragment into several v3 chunks.
\connect cloudsync_test_58_src
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE frag_rls (id TEXT PRIMARY KEY NOT NULL, note TEXT DEFAULT '');
SELECT cloudsync_init('frag_rls', 'CLS', 1) AS _init_src \gset
SELECT cloudsync_set('payload_max_chunk_size', '1');  -- clamps to the 256KB minimum
INSERT INTO frag_rls(id, note)
VALUES ('big', repeat('A', 262144) || repeat('B', 262144) || repeat('C', 131072));

SELECT count(*) FILTER (WHERE get_byte(payload, 4) = 3) AS v3_chunks
FROM cloudsync_payload_chunks() \gset
SELECT (:v3_chunks::int >= 2) AS fragmented_ok \gset
\if :fragmented_ok
\echo [PASS] (:testid) oversized value fragmented into :v3_chunks v3 chunks
\else
\echo [FAIL] (:testid) expected >=2 v3 fragments, got :v3_chunks (cannot exercise the v3 path)
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT string_agg(encode(payload, 'hex'), ',' ORDER BY chunk_index) AS chunks_hex
FROM cloudsync_payload_chunks() \gset

-- Target: readable, but every insert is rejected by a WITH CHECK policy.
\connect cloudsync_test_58_dst
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE frag_rls (id TEXT PRIMARY KEY NOT NULL, note TEXT DEFAULT '');
SELECT cloudsync_init('frag_rls', 'CLS', 1) AS _init_dst \gset

GRANT USAGE ON SCHEMA public TO v3_denied_user;
GRANT ALL ON ALL TABLES IN SCHEMA public TO v3_denied_user;
GRANT ALL ON ALL SEQUENCES IN SCHEMA public TO v3_denied_user;
GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA public TO v3_denied_user;
ALTER TABLE frag_rls ENABLE ROW LEVEL SECURITY;
CREATE POLICY frag_sel ON frag_rls FOR SELECT USING (true);
CREATE POLICY frag_ins ON frag_rls FOR INSERT WITH CHECK (false);

CREATE TABLE chunk_transport(ord INT, payload BYTEA);
INSERT INTO chunk_transport(ord, payload)
SELECT ord, decode(hexval, 'hex')
FROM unnest(string_to_array(:'chunks_hex', ',')) WITH ORDINALITY AS t(hexval, ord);
GRANT ALL ON chunk_transport TO v3_denied_user;

SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_before \gset

-- Apply every fragment as the restricted role, one top-level statement per chunk in
-- order. The non-final fragments stage; the final one reassembles and is denied.
-- ON_ERROR_STOP is disabled around it because the denial is expected to raise.
SET ROLE v3_denied_user;
\set ON_ERROR_STOP off
SELECT format('SELECT cloudsync_payload_apply(payload) FROM chunk_transport WHERE ord = %s;', ord)
FROM chunk_transport ORDER BY ord \gexec
\set ON_ERROR_STOP on
RESET ROLE;

-- Reconnect for clean state after the expected denial.
\connect cloudsync_test_58_dst
\ir helper_psql_conn_setup.sql

SELECT COUNT(*) AS applied_count FROM frag_rls WHERE id = 'big' \gset
SELECT (:applied_count::int = 0) AS denied_ok \gset
\if :denied_ok
\echo [PASS] (:testid) the fragmented value was denied by the WITH CHECK policy
\else
\echo [FAIL] (:testid) expected the value to be denied, found :applied_count rows
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_after \gset
SELECT (:ckpt_after::bigint = :ckpt_before::bigint) AS ckpt_pinned \gset
\if :ckpt_pinned
\echo [PASS] (:testid) a denied fragmented value leaves the checkpoint at :ckpt_after
\else
\echo [FAIL] (:testid) checkpoint moved to :ckpt_after after a denied fragmented value
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (SELECT count(*) FROM cloudsync_payload_fragments) > 0 AS staged_kept \gset
\if :staged_kept
\echo [PASS] (:testid) the pieces staged before the denial are kept for a retry
\else
\echo [FAIL] (:testid) the staged pieces were discarded by the denial
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Once the policy allows the row, redelivering the chunks applies the value.
ALTER POLICY frag_ins ON frag_rls WITH CHECK (true);
SET ROLE v3_denied_user;
SELECT format('SELECT cloudsync_payload_apply(payload) FROM chunk_transport WHERE ord = %s;', ord)
FROM chunk_transport ORDER BY ord \gexec
RESET ROLE;
SELECT (SELECT length(note) FROM frag_rls WHERE id = 'big') = 655360
   AND NOT EXISTS (SELECT FROM cloudsync_payload_fragments) AS redelivered_ok \gset
\if :redelivered_ok
\echo [PASS] (:testid) redelivery after the policy change applies the value and clears its pieces
\else
\echo [FAIL] (:testid) redelivery did not apply the fragmented value
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_58_src;
DROP DATABASE IF EXISTS cloudsync_test_58_dst;
DROP ROLE IF EXISTS v3_denied_user;
