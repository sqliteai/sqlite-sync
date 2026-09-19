-- Row-level security denials stop cloudsync_payload_apply, and redelivery succeeds once
-- the authorization allows the rows.
--
-- 1. A denial raised inside the cloudsync_changes trigger (a block column, a GOS table)
--    raises 42501 like one from the batched path. The statement rolls back, so nothing
--    it wrote remains (no rows, blocks or metadata) and the receive checkpoint stays put.
-- 2. A row authorized only by a row later in the same payload is not reordered or
--    retried internally: the apply fails, and succeeds once the authorizing row is in.
-- 3. A column the policy does not let the session write fails the apply instead of being
--    recorded as applied.
-- Once allowed, block columns and GOS tables write every column of the row although
-- their policies reference another column.

\set testid '59-rls-retry'
\ir helper_test_init.sql

\set USER1 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa'
\set USER2 'bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb'

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_59_src;
DROP DATABASE IF EXISTS cloudsync_test_59_dst;
CREATE DATABASE cloudsync_test_59_src;
CREATE DATABASE cloudsync_test_59_dst;
DO $$ BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'rls_retry_user') THEN
        CREATE ROLE rls_retry_user LOGIN;
    END IF;
END $$;

-- ------------------------------------------------------------------ source
\connect cloudsync_test_59_src
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE notes   (id TEXT PRIMARY KEY NOT NULL, user_id UUID, body TEXT);
CREATE TABLE events  (id TEXT PRIMARY KEY NOT NULL, user_id UUID, kind TEXT);
CREATE TABLE members (project_id TEXT NOT NULL, user_id UUID NOT NULL, PRIMARY KEY (project_id, user_id));
CREATE TABLE tasks   (id TEXT PRIMARY KEY NOT NULL, project_id TEXT, title TEXT);
CREATE TABLE activity (id TEXT PRIMARY KEY NOT NULL, project_id TEXT, kind TEXT);
CREATE TABLE logs    (id TEXT PRIMARY KEY NOT NULL, user_id UUID, msg TEXT);
SELECT cloudsync_init('notes')   AS _i1 \gset
SELECT cloudsync_init('events', 'gos') AS _i2 \gset
SELECT cloudsync_init('members') AS _i3 \gset
SELECT cloudsync_init('tasks')   AS _i4 \gset
SELECT cloudsync_init('activity', 'gos') AS _i5 \gset
SELECT cloudsync_init('logs', 'gos') AS _i6 \gset
SELECT cloudsync_set_column('notes', 'body', 'algo', 'block') AS _b \gset

INSERT INTO notes  VALUES ('n_own',   :'USER1', E'line 1\nline 2');
INSERT INTO notes  VALUES ('n_other', :'USER2', E'secret 1\nsecret 2');
INSERT INTO events VALUES ('e_own',   :'USER1', 'login');
INSERT INTO events VALUES ('e_other', :'USER2', 'login');
-- The task is written BEFORE the membership that authorizes it, so it comes first
-- in the payload.
INSERT INTO tasks   VALUES ('t_p1', 'p1', 'task in p1');
-- Same dependency on a GOS table, whose merge runs inside the cloudsync_changes trigger:
-- only a denial recognized as such is retried.
INSERT INTO activity VALUES ('a_p1', 'p1', 'created');
-- A resurrected task (sentinel plus columns) in the same project, also authorized only
-- by the membership that follows.
INSERT INTO tasks   VALUES ('t_rev', 'p1', 'first');
DELETE FROM tasks WHERE id = 't_rev';
INSERT INTO tasks   VALUES ('t_rev', 'p1', 'revived');
INSERT INTO members VALUES ('p1', :'USER1');
INSERT INTO logs    VALUES ('l_own', :'USER1', 'hello');
-- A task in a project USER1 never joins: denied for good.
INSERT INTO tasks   VALUES ('t_p2', 'p2', 'task in p2');

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS p_notes
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'notes' \gset
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS p_events
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'events' \gset
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS p_order
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl IN ('tasks', 'activity', 'members')
  AND pk <> cloudsync_pk_encode('t_p2') \gset
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS p_members
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'members' \gset
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS p_logs
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'logs' \gset
SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS p_denied
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() AND tbl = 'tasks' AND pk = cloudsync_pk_encode('t_p2') \gset

-- ------------------------------------------------------------------ target
\connect cloudsync_test_59_dst
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE notes   (id TEXT PRIMARY KEY NOT NULL, user_id UUID, body TEXT);
CREATE TABLE events  (id TEXT PRIMARY KEY NOT NULL, user_id UUID, kind TEXT);
CREATE TABLE members (project_id TEXT NOT NULL, user_id UUID NOT NULL, PRIMARY KEY (project_id, user_id));
CREATE TABLE tasks   (id TEXT PRIMARY KEY NOT NULL, project_id TEXT, title TEXT);
CREATE TABLE activity (id TEXT PRIMARY KEY NOT NULL, project_id TEXT, kind TEXT);
CREATE TABLE logs    (id TEXT PRIMARY KEY NOT NULL, user_id UUID, msg TEXT);
SELECT cloudsync_init('notes')   AS _i1 \gset
SELECT cloudsync_init('events', 'gos') AS _i2 \gset
SELECT cloudsync_init('members') AS _i3 \gset
SELECT cloudsync_init('tasks')   AS _i4 \gset
SELECT cloudsync_init('activity', 'gos') AS _i5 \gset
SELECT cloudsync_init('logs', 'gos') AS _i6 \gset
SELECT cloudsync_set_column('notes', 'body', 'algo', 'block') AS _b \gset

CREATE SCHEMA IF NOT EXISTS auth;
CREATE OR REPLACE FUNCTION auth.uid() RETURNS UUID LANGUAGE sql STABLE
AS $$ SELECT NULLIF(current_setting('app.current_user_id', true), '')::UUID $$;

DO $$ DECLARE t TEXT; BEGIN
    FOREACH t IN ARRAY ARRAY['notes', 'events', 'members'] LOOP
        EXECUTE format('ALTER TABLE %I ENABLE ROW LEVEL SECURITY', t);
        EXECUTE format('CREATE POLICY own_select ON %I FOR SELECT USING (auth.uid() = user_id)', t);
        EXECUTE format('CREATE POLICY own_insert ON %I FOR INSERT WITH CHECK (auth.uid() = user_id)', t);
        EXECUTE format('CREATE POLICY own_update ON %I FOR UPDATE USING (auth.uid() = user_id) WITH CHECK (auth.uid() = user_id)', t);
    END LOOP;
END $$;
-- logs can be inserted and read but has no UPDATE policy: a GOS table writes its
-- second column as an update, which such a policy silently turns into a no-op.
ALTER TABLE logs ENABLE ROW LEVEL SECURITY;
CREATE POLICY own_select ON logs FOR SELECT USING (auth.uid() = user_id);
CREATE POLICY own_insert ON logs FOR INSERT WITH CHECK (auth.uid() = user_id);
ALTER TABLE tasks ENABLE ROW LEVEL SECURITY;
CREATE POLICY member_select ON tasks FOR SELECT
    USING (EXISTS (SELECT 1 FROM members m WHERE m.project_id = tasks.project_id AND m.user_id = auth.uid()));
CREATE POLICY member_insert ON tasks FOR INSERT
    WITH CHECK (EXISTS (SELECT 1 FROM members m WHERE m.project_id = tasks.project_id AND m.user_id = auth.uid()));
ALTER TABLE activity ENABLE ROW LEVEL SECURITY;
CREATE POLICY member_select ON activity FOR SELECT
    USING (EXISTS (SELECT 1 FROM members m WHERE m.project_id = activity.project_id AND m.user_id = auth.uid()));
CREATE POLICY member_insert ON activity FOR INSERT
    WITH CHECK (EXISTS (SELECT 1 FROM members m WHERE m.project_id = activity.project_id AND m.user_id = auth.uid()));
CREATE POLICY member_update ON activity FOR UPDATE
    USING (EXISTS (SELECT 1 FROM members m WHERE m.project_id = activity.project_id AND m.user_id = auth.uid()))
    WITH CHECK (EXISTS (SELECT 1 FROM members m WHERE m.project_id = activity.project_id AND m.user_id = auth.uid()));
CREATE POLICY member_update ON tasks FOR UPDATE
    USING (EXISTS (SELECT 1 FROM members m WHERE m.project_id = tasks.project_id AND m.user_id = auth.uid()))
    WITH CHECK (EXISTS (SELECT 1 FROM members m WHERE m.project_id = tasks.project_id AND m.user_id = auth.uid()));

GRANT USAGE ON SCHEMA public, auth TO rls_retry_user;
GRANT ALL ON ALL TABLES IN SCHEMA public TO rls_retry_user;
GRANT ALL ON ALL SEQUENCES IN SCHEMA public TO rls_retry_user;
GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA public, auth TO rls_retry_user;

-- 1. Trigger-path denial on a block column: nothing is kept, the apply fails.
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_before \gset
SET app.current_user_id = :'USER1';
SET ROLE rls_retry_user;
\set ON_ERROR_STOP off
SELECT cloudsync_payload_apply(decode(:'p_notes', 'hex')) AS _applied \gset
\set notes1_state :SQLSTATE
\set ON_ERROR_STOP on
RESET ROLE;
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_after \gset
SELECT (:'notes1_state' = '42501' AND :ckpt_after::bigint = :ckpt_before::bigint) AS notes1_denied_ok \gset
\if :notes1_denied_ok
\echo [PASS] (:testid) block column denial: raised 42501 and left the checkpoint in place
\else
\echo [FAIL] (:testid) block column denial: SQLSTATE :notes1_state, checkpoint :ckpt_before -> :ckpt_after
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT (SELECT count(*) FROM notes) = 0
   AND (SELECT count(*) FROM notes_cloudsync_blocks) = 0
   AND (SELECT count(*) FROM notes_cloudsync) = 0 AS notes_clean_ok \gset
\if :notes_clean_ok
\echo [PASS] (:testid) block column: the failed statement left no rows, blocks or metadata
\else
\echo [FAIL] (:testid) block column: the failed apply left state behind
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Allowing the rows, the same payload applies in full.
ALTER POLICY own_select ON notes USING (true);
ALTER POLICY own_insert ON notes WITH CHECK (true);
ALTER POLICY own_update ON notes USING (true) WITH CHECK (true);
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_before \gset
SET app.current_user_id = :'USER1';
SET ROLE rls_retry_user;
\set ON_ERROR_STOP off
SELECT cloudsync_payload_apply(decode(:'p_notes', 'hex')) AS _applied \gset
\set notes2_state :SQLSTATE
\set ON_ERROR_STOP on
RESET ROLE;
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_after \gset
SELECT (:'notes2_state' = '00000' AND :ckpt_after::bigint >= :ckpt_before::bigint) AS notes2_ok \gset
\if :notes2_ok
\echo [PASS] (:testid) block column redelivery: applied
\else
\echo [FAIL] (:testid) block column redelivery: SQLSTATE :notes2_state, checkpoint :ckpt_before -> :ckpt_after
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT (SELECT body FROM notes WHERE id = 'n_own') = E'line 1\nline 2'
   AND (SELECT body FROM notes WHERE id = 'n_other') = E'secret 1\nsecret 2' AS notes_body_ok \gset
\if :notes_body_ok
\echo [PASS] (:testid) block column: both rows materialized in full
\else
\echo [FAIL] (:testid) block column: rows not materialized after redelivery
SELECT (:fail::int + 1) AS fail \gset
\endif

-- GOS table, whose merge runs inside the cloudsync_changes trigger.
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_before \gset
SET app.current_user_id = :'USER1';
SET ROLE rls_retry_user;
\set ON_ERROR_STOP off
SELECT cloudsync_payload_apply(decode(:'p_events', 'hex')) AS _applied \gset
\set events1_state :SQLSTATE
\set ON_ERROR_STOP on
RESET ROLE;
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_after \gset
SELECT (:'events1_state' = '42501' AND :ckpt_after::bigint = :ckpt_before::bigint) AS events1_denied_ok \gset
\if :events1_denied_ok
\echo [PASS] (:testid) GOS denial: raised 42501 and left the checkpoint in place
\else
\echo [FAIL] (:testid) GOS denial: SQLSTATE :events1_state, checkpoint :ckpt_before -> :ckpt_after
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT (SELECT count(*) FROM events) = 0 AND (SELECT count(*) FROM events_cloudsync) = 0 AS events_clean_ok \gset
\if :events_clean_ok
\echo [PASS] (:testid) GOS table: the failed statement left no rows or metadata
\else
\echo [FAIL] (:testid) GOS table: the failed apply left state behind
SELECT (:fail::int + 1) AS fail \gset
\endif
ALTER POLICY own_select ON events USING (true);
ALTER POLICY own_insert ON events WITH CHECK (true);
ALTER POLICY own_update ON events USING (true) WITH CHECK (true);
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_before \gset
SET app.current_user_id = :'USER1';
SET ROLE rls_retry_user;
\set ON_ERROR_STOP off
SELECT cloudsync_payload_apply(decode(:'p_events', 'hex')) AS _applied \gset
\set events2_state :SQLSTATE
\set ON_ERROR_STOP on
RESET ROLE;
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_after \gset
SELECT (:'events2_state' = '00000' AND :ckpt_after::bigint >= :ckpt_before::bigint) AS events2_ok \gset
\if :events2_ok
\echo [PASS] (:testid) GOS redelivery: applied
\else
\echo [FAIL] (:testid) GOS redelivery: SQLSTATE :events2_state, checkpoint :ckpt_before -> :ckpt_after
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT (SELECT count(*) FROM events WHERE kind = 'login') = 2 AS events_rows_ok \gset
\if :events_rows_ok
\echo [PASS] (:testid) GOS table: every column of both rows written
\else
\echo [FAIL] (:testid) GOS table: rows incomplete after redelivery
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 2. The tasks come before the membership that authorizes them: no internal retry.
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_before \gset
SET app.current_user_id = :'USER1';
SET ROLE rls_retry_user;
\set ON_ERROR_STOP off
SELECT cloudsync_payload_apply(decode(:'p_order', 'hex')) AS _applied \gset
\set order1_state :SQLSTATE
\set ON_ERROR_STOP on
RESET ROLE;
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_after \gset
SELECT (:'order1_state' = '42501' AND :ckpt_after::bigint = :ckpt_before::bigint) AS order1_denied_ok \gset
\if :order1_denied_ok
\echo [PASS] (:testid) order-dependent denial: raised 42501 and left the checkpoint in place
\else
\echo [FAIL] (:testid) order-dependent denial: SQLSTATE :order1_state, checkpoint :ckpt_before -> :ckpt_after
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT (SELECT count(*) FROM members) = 0 AND (SELECT count(*) FROM tasks) = 0 AND (SELECT count(*) FROM activity) = 0 AS order_clean_ok \gset
\if :order_clean_ok
\echo [PASS] (:testid) order-dependent denial: nothing applied, membership included
\else
\echo [FAIL] (:testid) order-dependent denial: partial state left behind
SELECT (:fail::int + 1) AS fail \gset
\endif
-- Deliver the membership first, then the same payload again.
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_before \gset
SET app.current_user_id = :'USER1';
SET ROLE rls_retry_user;
\set ON_ERROR_STOP off
SELECT cloudsync_payload_apply(decode(:'p_members', 'hex')) AS _applied \gset
\set members_state :SQLSTATE
\set ON_ERROR_STOP on
RESET ROLE;
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_after \gset
SELECT (:'members_state' = '00000' AND :ckpt_after::bigint >= :ckpt_before::bigint) AS members_ok \gset
\if :members_ok
\echo [PASS] (:testid) membership: applied
\else
\echo [FAIL] (:testid) membership: SQLSTATE :members_state, checkpoint :ckpt_before -> :ckpt_after
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_before \gset
SET app.current_user_id = :'USER1';
SET ROLE rls_retry_user;
\set ON_ERROR_STOP off
SELECT cloudsync_payload_apply(decode(:'p_order', 'hex')) AS _applied \gset
\set order2_state :SQLSTATE
\set ON_ERROR_STOP on
RESET ROLE;
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_after \gset
SELECT (:'order2_state' = '00000' AND :ckpt_after::bigint >= :ckpt_before::bigint) AS order2_ok \gset
\if :order2_ok
\echo [PASS] (:testid) order-dependent redelivery: applied
\else
\echo [FAIL] (:testid) order-dependent redelivery: SQLSTATE :order2_state, checkpoint :ckpt_before -> :ckpt_after
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT (SELECT title FROM tasks WHERE id = 't_p1') IS NOT DISTINCT FROM 'task in p1'
   AND (SELECT kind FROM activity WHERE id = 'a_p1') IS NOT DISTINCT FROM 'created'
   AND (SELECT title FROM tasks WHERE id = 't_rev') IS NOT DISTINCT FROM 'revived' AS order_rows_ok \gset
\if :order_rows_ok
\echo [PASS] (:testid) order-dependent rows (batched, GOS trigger path, resurrected) applied after the membership
\else
\echo [FAIL] (:testid) order-dependent rows missing after redelivery
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 3. logs has no UPDATE policy: a GOS table writes its second column as an update,
-- which that policy hides. The write fails instead of being recorded as applied.
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_before \gset
SET app.current_user_id = :'USER1';
SET ROLE rls_retry_user;
\set ON_ERROR_STOP off
SELECT cloudsync_payload_apply(decode(:'p_logs', 'hex')) AS _applied \gset
\set logs_state :SQLSTATE
\set ON_ERROR_STOP on
RESET ROLE;
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_after \gset
SELECT (:'logs_state' = '42501' AND :ckpt_after::bigint = :ckpt_before::bigint) AS logs_denied_ok \gset
\if :logs_denied_ok
\echo [PASS] (:testid) column hidden from UPDATE: raised 42501 and left the checkpoint in place
\else
\echo [FAIL] (:testid) column hidden from UPDATE: SQLSTATE :logs_state, checkpoint :ckpt_before -> :ckpt_after
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT NOT EXISTS (SELECT FROM logs_cloudsync) AND NOT EXISTS (SELECT FROM logs) AS no_silent_loss_ok \gset
\if :no_silent_loss_ok
\echo [PASS] (:testid) a column hidden from UPDATE is not recorded as applied
\else
\echo [FAIL] (:testid) a column the table does not hold was recorded as applied
SELECT (:fail::int + 1) AS fail \gset
\endif

-- A task in a project USER1 never joins stays denied on every delivery.
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_before \gset
SET app.current_user_id = :'USER1';
SET ROLE rls_retry_user;
\set ON_ERROR_STOP off
SELECT cloudsync_payload_apply(decode(:'p_denied', 'hex')) AS _applied \gset
\set denied_state :SQLSTATE
\set ON_ERROR_STOP on
RESET ROLE;
SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_after \gset
SELECT (:'denied_state' = '42501' AND :ckpt_after::bigint = :ckpt_before::bigint) AS denied_denied_ok \gset
\if :denied_denied_ok
\echo [PASS] (:testid) permanent denial: raised 42501 and left the checkpoint in place
\else
\echo [FAIL] (:testid) permanent denial: SQLSTATE :denied_state, checkpoint :ckpt_before -> :ckpt_after
SELECT (:fail::int + 1) AS fail \gset
\endif
SELECT (SELECT count(*) FROM tasks WHERE id = 't_p2') = 0 AS still_denied_ok \gset
\if :still_denied_ok
\echo [PASS] (:testid) a permanently denied row is not applied
\else
\echo [FAIL] (:testid) a permanently denied row was applied
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_59_src;
DROP DATABASE IF EXISTS cloudsync_test_59_dst;
DROP ROLE IF EXISTS rls_retry_user;
