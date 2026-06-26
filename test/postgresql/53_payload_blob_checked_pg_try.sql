-- Regression: cloudsync_payload_blob_checked must restore PG_exception_stack on
-- its SUCCESS path.
--
-- The function returns its result with PG_RETURN_BYTEA_P(result) (and
-- PG_RETURN_NULL on the empty-blob path) from INSIDE its PG_TRY()/PG_END_TRY()
-- block. PG_RETURN_* expands to `return`, so PG_END_TRY() never runs and
-- PG_exception_stack is left pointing at this function's now-dead stack frame.
-- A later ereport(ERROR) raised in the SAME query then siglongjmp()s into that
-- freed frame (before the enclosing portal's PG_END_TRY can restore the stack)
-- and the backend segfaults. This is the exact hazard the PR documents and
-- fixes in cloudsync_changes_select, reintroduced here.
--
-- Reproduction: one top-level query that evaluates blob_checked (the divide's
-- operand forces it to run first) and then raises integer division-by-zero.
--   BUG PRESENT -> backend crash; psql reports "server closed the connection
--                  unexpectedly" and the run aborts at the SELECT below.
--   BUG FIXED   -> a clean "division by zero" SQL error; the session survives
--                  and the [PASS] line is reached.

\set testid '53-pg-try-stack'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_53_pg_try;
CREATE DATABASE cloudsync_test_53_pg_try;

\connect cloudsync_test_53_pg_try
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;

CREATE TABLE blob_checked_test (
  id TEXT PRIMARY KEY,
  note TEXT DEFAULT ''
);
SELECT cloudsync_init('blob_checked_test', 'CLS', 1) AS _init \gset
INSERT INTO blob_checked_test(id, note) VALUES ('a', 'hello'), ('b', 'world');

-- Sanity: blob_checked returns a non-NULL payload, so the call exercises the
-- success return path that carries the bug. (A standalone call is healed when
-- the statement completes and the portal's PG_END_TRY restores the stack, so
-- this line is safe on its own.)
SELECT cloudsync_payload_blob_checked(0, 0, cloudsync_siteid(), false, 1000000000) IS NOT NULL AS have_blob \gset
\if :have_blob
\echo [PASS] (:testid) blob_checked produced a payload (exercises the success return path)
\else
\echo [FAIL] (:testid) blob_checked returned NULL - cannot exercise the buggy path
SELECT (:fail::int + 1) AS fail \gset
\endif

-- The post-call error is expected; it must not abort the script so we can check
-- whether the session survived it.
\set ON_ERROR_STOP off

-- Single top-level query: blob_checked runs first (operand of the divide),
-- leaving PG_exception_stack dangling on the buggy build, then int4div raises
-- ereport(ERROR) while that dangling frame is still the active longjmp target.
SELECT (cloudsync_payload_blob_checked(0, 0, cloudsync_siteid(), false, 1000000000) IS NOT NULL)::int / 0 AS boom;

\set ON_ERROR_STOP on

-- Reaching here means the backend is still alive: the dangling-frame siglongjmp
-- did NOT happen, i.e. PG_END_TRY ran on the success path. On the buggy build
-- the connection is already gone and psql never runs this.
SELECT 1 AS still_alive \gset
\if :{?still_alive}
\echo [PASS] (:testid) session survived a post-call error - PG_exception_stack was restored
\else
\echo [FAIL] (:testid) session did not survive a post-call error
SELECT (:fail::int + 1) AS fail \gset
\endif

\ir helper_test_cleanup.sql
\if :should_cleanup
\connect postgres
DROP DATABASE IF EXISTS cloudsync_test_53_pg_try;
\endif
