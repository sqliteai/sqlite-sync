-- Row-level security denials outside the batched column path, and denials that depend
-- on the order of the payload.
--
-- 1. A denial raised inside the cloudsync_changes trigger (a block column, a GOS table)
--    is skipped like one from the batched path: the rest of the payload applies and the
--    receive checkpoint advances, instead of the whole apply failing.
-- 2. A row denied only because a row granting access arrives later in the same payload
--    is retried once the payload is in, and is applied.
-- 3. A row still denied after the retry stays skipped.
-- Along the way, block columns and GOS tables must write every column of a permitted row
-- although their policies reference another column.

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

SELECT encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS payload_hex
FROM cloudsync_changes WHERE site_id = cloudsync_siteid() \gset

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

SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_before \gset

SET app.current_user_id = :'USER1';
SET ROLE rls_retry_user;
\set ON_ERROR_STOP off
\unset apply_rows
SELECT cloudsync_payload_apply(decode(:'payload_hex', 'hex')) AS apply_rows \gset
\set ON_ERROR_STOP on
RESET ROLE;

\if :{?apply_rows}
\echo [PASS] (:testid) apply with trigger-path and order-dependent denials completed
\else
\echo [FAIL] (:testid) apply failed instead of skipping the denied rows
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 1. Trigger-path denials are skipped, the permitted rows are applied.
SELECT (SELECT count(*) FROM notes WHERE id = 'n_own') = 1
   AND (SELECT count(*) FROM notes WHERE id = 'n_other') = 0 AS notes_ok \gset
\if :notes_ok
\echo [PASS] (:testid) block column: own row applied, other user row skipped
\else
\echo [FAIL] (:testid) block column: unexpected rows in notes
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (SELECT body FROM notes WHERE id = 'n_own') = E'line 1\nline 2' AS notes_body_ok \gset
\if :notes_body_ok
\echo [PASS] (:testid) block column: own row materialized in full
\else
\echo [FAIL] (:testid) block column: own row body not materialized
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (SELECT count(*) FROM notes_cloudsync_blocks WHERE pk = cloudsync_pk_encode('n_other')) = 0 AS notes_blocks_ok \gset
\if :notes_blocks_ok
\echo [PASS] (:testid) block column: a denied row leaves no blocks behind
\else
\echo [FAIL] (:testid) block column: denied row left blocks behind
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (SELECT count(*) FROM events WHERE id = 'e_own' AND kind = 'login') = 1
   AND (SELECT count(*) FROM events WHERE id = 'e_other') = 0 AS events_ok \gset
\if :events_ok
\echo [PASS] (:testid) GOS table: own row applied with every column, other user row skipped
\else
\echo [FAIL] (:testid) GOS table: unexpected rows in events
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 2. The task denied on the first pass is applied once its membership is in.
SELECT (SELECT count(*) FROM members WHERE project_id = 'p1') = 1 AS member_ok \gset
SELECT (SELECT title FROM tasks WHERE id = 't_p1') IS NOT DISTINCT FROM 'task in p1' AS retry_ok \gset
\if :member_ok
\if :retry_ok
\echo [PASS] (:testid) order-dependent denial: task applied after its membership on retry
\else
\echo [FAIL] (:testid) order-dependent denial: task authorized later in the payload was lost
SELECT (:fail::int + 1) AS fail \gset
\endif
\else
\echo [FAIL] (:testid) membership row was not applied
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (SELECT kind FROM activity WHERE id = 'a_p1') IS NOT DISTINCT FROM 'created' AS retry_gos_ok \gset
\if :retry_gos_ok
\echo [PASS] (:testid) order-dependent denial in the trigger path (GOS): applied on retry
\else
\echo [FAIL] (:testid) order-dependent denial in the trigger path (GOS): row was lost
SELECT (:fail::int + 1) AS fail \gset
\endif

-- A column the policy does not let this session write is reported as denied, never
-- recorded as applied while the table does not hold it.
SELECT NOT EXISTS (SELECT FROM logs_cloudsync WHERE col_name = 'msg')
   AND (SELECT msg FROM logs WHERE id = 'l_own') IS NULL AS no_silent_loss_ok \gset
\if :no_silent_loss_ok
\echo [PASS] (:testid) a column hidden from UPDATE is not recorded as applied
\else
\echo [FAIL] (:testid) a column the table does not hold was recorded as applied
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (SELECT title FROM tasks WHERE id = 't_rev') IS NOT DISTINCT FROM 'revived' AS retry_revived_ok \gset
\if :retry_revived_ok
\echo [PASS] (:testid) order-dependent denial of a resurrected row: applied on retry
\else
\echo [FAIL] (:testid) order-dependent denial of a resurrected row: row was lost
SELECT (:fail::int + 1) AS fail \gset
\endif

-- 3. A row still denied after the retry stays out, and the cursor moves on.
SELECT (SELECT count(*) FROM tasks WHERE id = 't_p2') = 0 AS still_denied_ok \gset
\if :still_denied_ok
\echo [PASS] (:testid) a row still denied after the retry is skipped
\else
\echo [FAIL] (:testid) a permanently denied row was applied
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'), 0) AS ckpt_after \gset
SELECT (:ckpt_after::bigint > :ckpt_before::bigint) AS ckpt_ok \gset
\if :ckpt_ok
\echo [PASS] (:testid) receive checkpoint advanced past the skipped rows
\else
\echo [FAIL] (:testid) receive checkpoint stayed at :ckpt_after
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_59_src;
DROP DATABASE IF EXISTS cloudsync_test_59_dst;
DROP ROLE IF EXISTS rls_retry_user;
