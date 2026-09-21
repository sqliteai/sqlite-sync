-- Audit: a change whose write fails stops the apply with its own SQLSTATE; nothing is
-- dropped silently and the checkpoint stays where it was until the same payload applies;
-- transient failures are covered by 39_concurrent_write_apply.sql.
\set ON_ERROR_STOP on
\connect postgres
DROP DATABASE IF EXISTS cloudsync_audit_source;
DROP DATABASE IF EXISTS cloudsync_audit_target;
CREATE DATABASE cloudsync_audit_source;
CREATE DATABASE cloudsync_audit_target;
\connect cloudsync_audit_source
CREATE EXTENSION cloudsync;
CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, value TEXT);
SELECT cloudsync_init('t');
INSERT INTO t VALUES ('1','a'),('2','b'),('3','c');
SELECT encode(cloudsync_payload_encode(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq),'hex') AS audit_payload FROM cloudsync_changes \gset
CREATE TABLE plain_audit(id TEXT PRIMARY KEY NOT NULL, value TEXT);
SELECT cloudsync_init('plain_audit') \gset
INSERT INTO plain_audit VALUES ('p1','x');
SELECT encode(cloudsync_payload_encode(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq),'hex') AS plain_payload_hex FROM cloudsync_changes WHERE tbl = 'plain_audit' \gset
CREATE TABLE revived(id TEXT PRIMARY KEY NOT NULL, a TEXT, b TEXT);
SELECT cloudsync_init('revived') \gset
INSERT INTO revived VALUES ('r1','x','y');
DELETE FROM revived WHERE id = 'r1';
INSERT INTO revived VALUES ('r1','x2','y2');
SELECT encode(cloudsync_payload_encode(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq),'hex') AS revived_payload_hex FROM cloudsync_changes WHERE tbl = 'revived' \gset

\connect cloudsync_audit_target
CREATE EXTENSION cloudsync;
CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, value TEXT);
SELECT cloudsync_init('t');
CREATE TEMP TABLE audit_payload(data BYTEA);
INSERT INTO audit_payload VALUES (decode(:'audit_payload','hex'));
CREATE FUNCTION deny_audit_row() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF NEW.id = current_setting('audit.denied_id') THEN
        RAISE EXCEPTION 'audit write denied';
    END IF;
    RETURN NEW;
END $$;
CREATE TRIGGER deny_audit BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION deny_audit_row();
-- The payload is read from a table on purpose: the apply's internal savepoints must
-- not disturb the resource owner of the scan feeding it.
DO $$
DECLARE denied INTEGER; state TEXT; msg TEXT;
BEGIN
    FOR denied IN 1..3 LOOP
        DELETE FROM t;
        DELETE FROM t_cloudsync;
        DELETE FROM cloudsync_settings WHERE key IN ('check_dbversion','check_seq');
        PERFORM set_config('audit.denied_id', denied::TEXT, false);
        state := NULL;
        BEGIN PERFORM cloudsync_payload_apply(data) FROM audit_payload;
        EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS state = RETURNED_SQLSTATE, msg = MESSAGE_TEXT; END;
        IF state IS DISTINCT FROM 'P0001' OR msg NOT LIKE '%audit write denied%' THEN
            RAISE EXCEPTION 'Row % failing surfaced as SQLSTATE % (%)', denied, coalesce(state, 'none'), msg;
        END IF;
        -- the failed statement is rolled back whole, metadata and checkpoint included
        IF EXISTS (SELECT FROM t) OR EXISTS (SELECT FROM t_cloudsync) THEN
            RAISE EXCEPTION 'Row % failing left rows or metadata behind', denied;
        END IF;
        IF coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'),0) <> 0 THEN
            RAISE EXCEPTION 'Row % failing moved the checkpoint', denied;
        END IF;
        -- once the write can succeed, the same payload applies in full
        PERFORM set_config('audit.denied_id', '', false);
        PERFORM cloudsync_payload_apply(data) FROM audit_payload;
        IF (SELECT count(*) FROM t) <> 3 OR coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'),0) = 0 THEN
            RAISE EXCEPTION 'Row % redelivered: expected 3 rows and an advanced checkpoint', denied;
        END IF;
    END LOOP;
END $$;
\echo [PASS] (57-audit) first, middle and final write failures stop the apply with their error and apply when redelivered

CREATE FUNCTION raise_sqlstate() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN RAISE EXCEPTION USING ERRCODE = TG_ARGV[0], MESSAGE = 'simulated ' || TG_ARGV[0]; END $$;
CREATE TABLE plain_audit(id TEXT PRIMARY KEY NOT NULL, value TEXT);
SELECT cloudsync_init('plain_audit') \gset
CREATE TABLE revived(id TEXT PRIMARY KEY NOT NULL, a TEXT, b TEXT);
SELECT cloudsync_init('revived') \gset

-- A payload apply keeps the SQLSTATE of transient and privilege failures too, and leaves
-- the checkpoint where it was.
CREATE TRIGGER fail_apply BEFORE INSERT ON plain_audit FOR EACH ROW EXECUTE FUNCTION raise_sqlstate('40001');
CREATE TEMP TABLE plain_payload(data BYTEA);
INSERT INTO plain_payload VALUES (decode(:'plain_payload_hex','hex'));
DO $$
DECLARE state TEXT; msg TEXT; code TEXT;
BEGIN
    FOREACH code IN ARRAY ARRAY['40001', '42501'] LOOP
        EXECUTE 'DROP TRIGGER fail_apply ON plain_audit';
        EXECUTE format('CREATE TRIGGER fail_apply BEFORE INSERT ON plain_audit FOR EACH ROW EXECUTE FUNCTION raise_sqlstate(%L)', code);
        DELETE FROM cloudsync_settings WHERE key IN ('check_dbversion','check_seq');
        state := NULL;
        BEGIN PERFORM cloudsync_payload_apply(data) FROM plain_payload;
        EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS state = RETURNED_SQLSTATE, msg = MESSAGE_TEXT; END;
        IF state IS DISTINCT FROM code THEN
            RAISE EXCEPTION 'Apply failing with % surfaced as SQLSTATE % (%)', code, coalesce(state, 'none'), msg;
        END IF;
        IF EXISTS (SELECT FROM plain_audit) OR coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'),0) <> 0 THEN
            RAISE EXCEPTION 'Apply failing with % wrote rows or moved the checkpoint', code;
        END IF;
    END LOOP;
END $$;
DROP TRIGGER fail_apply ON plain_audit;
\echo [PASS] (57-audit) apply keeps the SQLSTATE of transient and privilege failures

-- A resurrected row (sentinel plus columns) whose write fails leaves nothing behind —
-- no sentinel, no zeroed clocks — so once the cause is gone the same payload creates it.
CREATE TRIGGER fail_revived BEFORE INSERT ON revived FOR EACH ROW EXECUTE FUNCTION raise_sqlstate('23514');
CREATE TEMP TABLE revived_payload(data BYTEA);
INSERT INTO revived_payload VALUES (decode(:'revived_payload_hex','hex'));
DO $$
DECLARE state TEXT;
BEGIN
    BEGIN PERFORM cloudsync_payload_apply(data) FROM revived_payload;
    EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS state = RETURNED_SQLSTATE; END;
    IF state IS DISTINCT FROM '23514' THEN RAISE EXCEPTION 'Rejected resurrected row surfaced as SQLSTATE %', coalesce(state, 'none'); END IF;
END $$;
DROP TRIGGER fail_revived ON revived;
DO $$ BEGIN
    IF EXISTS (SELECT FROM revived) THEN RAISE EXCEPTION 'Rejected resurrected row was written'; END IF;
    IF EXISTS (SELECT FROM revived_cloudsync) THEN RAISE EXCEPTION 'Rejected resurrected row left metadata behind'; END IF;
END $$;
SELECT cloudsync_payload_apply(data) FROM revived_payload \gset
DO $$ BEGIN
    IF NOT EXISTS (SELECT FROM revived WHERE id = 'r1' AND a = 'x2' AND b = 'y2') THEN
        RAISE EXCEPTION 'Resurrected row was not created once its write could succeed';
    END IF;
END $$;
\echo [PASS] (57-audit) a failed resurrected row leaves no metadata and applies when delivered again

-- Savepoints opened by the apply must leave the caller's snapshots intact when a group is
-- rolled back and the caller's own subtransaction then catches the error: statements
-- after the apply keep running (an unbalanced active-snapshot stack trips an assertion in
-- EnsurePortalSnapshotExists and takes the backend down).
DELETE FROM revived; DELETE FROM revived_cloudsync;
CREATE TRIGGER fail_revived BEFORE INSERT ON revived FOR EACH ROW EXECUTE FUNCTION raise_sqlstate('23514');
DO $$
DECLARE n INTEGER;
BEGIN
    BEGIN
        PERFORM cloudsync_payload_apply(data) FROM revived_payload;
        RAISE EXCEPTION 'the apply must fail';
    EXCEPTION WHEN check_violation THEN NULL;
    END;
    SELECT count(*) INTO n FROM revived_payload;
    BEGIN
        PERFORM cloudsync_payload_apply(data) FROM revived_payload;
    EXCEPTION WHEN check_violation THEN NULL;
    END;
    SELECT count(*) INTO n FROM revived;
    IF n <> 0 THEN RAISE EXCEPTION 'Rejected resurrected row was written'; END IF;
END $$;
DROP TRIGGER fail_revived ON revived;
\echo [PASS] (57-audit) apply savepoints leave the snapshots of the caller intact across rollbacks

DROP TRIGGER deny_audit ON t;
DELETE FROM t;  -- the block cases below start from an empty table
SELECT cloudsync_set_column('t','value','algo','block');
CREATE FUNCTION deny_audit_block() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN RAISE EXCEPTION 'audit block write denied'; END $$;
CREATE TRIGGER deny_block BEFORE INSERT ON t_cloudsync_blocks FOR EACH ROW EXECUTE FUNCTION deny_audit_block();
DO $$
DECLARE failed BOOLEAN := false;
BEGIN
    BEGIN INSERT INTO t VALUES('block','new');
    EXCEPTION WHEN OTHERS THEN failed := true; END;
    IF NOT failed OR EXISTS(SELECT FROM t) THEN RAISE EXCEPTION 'Block insert failed to roll back'; END IF;
END $$;
DROP TRIGGER deny_block ON t_cloudsync_blocks;
INSERT INTO t VALUES('block','old');
CREATE TRIGGER deny_block BEFORE INSERT ON t_cloudsync_blocks FOR EACH ROW EXECUTE FUNCTION deny_audit_block();
DO $$
DECLARE failed BOOLEAN := false;
BEGIN
    BEGIN UPDATE t SET value='new' WHERE id='block';
    EXCEPTION WHEN OTHERS THEN failed := true; END;
    IF NOT failed OR (SELECT value FROM t WHERE id='block') <> 'old' THEN
        RAISE EXCEPTION 'Block update failed to roll back';
    END IF;
END $$;
\echo [PASS] (57-audit) block insert/update failures roll back base rows and metadata

DROP TRIGGER deny_block ON t_cloudsync_blocks;

-- Reading the row back is part of the block write, so a row the session cannot
-- select is an error — but it must be a legible one. A bare code would reach the
-- caller blank, because databasevm_step clears the error text on entry.
DO $$ BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'audit_block_user') THEN
        CREATE ROLE audit_block_user LOGIN;
    END IF;
END $$;
GRANT USAGE ON SCHEMA public TO audit_block_user;
GRANT ALL ON ALL TABLES IN SCHEMA public TO audit_block_user;
GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA public TO audit_block_user;
ALTER TABLE t ENABLE ROW LEVEL SECURITY;
CREATE POLICY t_ins ON t FOR INSERT WITH CHECK (true);
CREATE POLICY t_sel ON t FOR SELECT USING (false);
DO $$
DECLARE msg TEXT := ''; failed BOOLEAN := false;
BEGIN
    SET LOCAL ROLE audit_block_user;
    BEGIN INSERT INTO t VALUES('invisible','text');
    EXCEPTION WHEN OTHERS THEN failed := true; msg := SQLERRM; END;
    RESET ROLE;
    IF NOT failed THEN RAISE EXCEPTION 'Unreadable block row did not report an error'; END IF;
    IF coalesce(btrim(msg), '') = '' THEN RAISE EXCEPTION 'Unreadable block row reported a blank error'; END IF;
    IF msg NOT LIKE '%not visible to this connection%' OR msg NOT LIKE '%value%' THEN
        RAISE EXCEPTION 'Unreadable block row reported an unhelpful error: %', msg;
    END IF;
END $$;
DROP POLICY t_sel ON t;
DROP POLICY t_ins ON t;
ALTER TABLE t DISABLE ROW LEVEL SECURITY;
\echo [PASS] (57-audit) an unreadable block row reports which table and column, not a blank error

-- Materializing a block column names the stage, column and table of a failed write
-- and keeps its SQLSTATE, also when called directly rather than from a merge.
CREATE FUNCTION reject_value() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN RAISE EXCEPTION USING ERRCODE = '23514', MESSAGE = 'value rejected'; END $$;
CREATE TRIGGER reject_value BEFORE UPDATE ON t FOR EACH ROW EXECUTE FUNCTION reject_value();
DO $$
DECLARE state TEXT; msg TEXT;
BEGIN
    BEGIN PERFORM cloudsync_text_materialize('t', 'value', 'block');
    EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS state = RETURNED_SQLSTATE, msg = MESSAGE_TEXT; END;
    IF state IS DISTINCT FROM '23514' THEN RAISE EXCEPTION 'Materialize write failure surfaced as SQLSTATE % (%)', state, msg; END IF;
    IF msg NOT LIKE 'Unable to write the blocks of column "value" of table "t"%' OR msg NOT LIKE '%value rejected%' THEN
        RAISE EXCEPTION 'Materialize write failure message lacks stage, context or cause: %', msg;
    END IF;
END $$;
DROP TRIGGER reject_value ON t;
\echo [PASS] (57-audit) a failed block materialization names the stage, column and table and keeps its SQLSTATE

-- A failure inside cloudsync's tracking triggers keeps its SQLSTATE, so an application
-- can still retry a serialization failure or handle a unique violation, and names the
-- table and column it was writing.
CREATE TRIGGER fail_block BEFORE INSERT ON t_cloudsync_blocks FOR EACH ROW EXECUTE FUNCTION raise_sqlstate('40001');
CREATE TRIGGER fail_meta BEFORE INSERT ON plain_audit_cloudsync FOR EACH ROW EXECUTE FUNCTION raise_sqlstate('23505');
DO $$
DECLARE state TEXT; msg TEXT;
BEGIN
    BEGIN INSERT INTO t VALUES('sqlstate','text');
    EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS state = RETURNED_SQLSTATE, msg = MESSAGE_TEXT; END;
    IF state IS DISTINCT FROM '40001' THEN RAISE EXCEPTION 'Block write failure surfaced as SQLSTATE % (%), expected 40001', state, msg; END IF;
    IF msg NOT LIKE '%column "value" of table "t"%' OR msg NOT LIKE '%simulated 40001%' THEN
        RAISE EXCEPTION 'Block write failure message lacks context or cause: %', msg;
    END IF;
    state := NULL;
    BEGIN INSERT INTO plain_audit VALUES('sqlstate','text');
    EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS state = RETURNED_SQLSTATE, msg = MESSAGE_TEXT; END;
    IF state IS DISTINCT FROM '23505' THEN RAISE EXCEPTION 'Metadata write failure surfaced as SQLSTATE % (%), expected 23505', state, msg; END IF;
END $$;
DROP TRIGGER fail_block ON t_cloudsync_blocks;
DROP TRIGGER fail_meta ON plain_audit_cloudsync;
\echo [PASS] (57-audit) tracking-trigger failures keep their SQLSTATE and name the column and table

-- Converting a column to block must skip metadata whose base row is gone, rather
-- than fail the conversion.
CREATE TABLE orphan_docs(id TEXT PRIMARY KEY NOT NULL, body TEXT);
SELECT cloudsync_init('orphan_docs') \gset
INSERT INTO orphan_docs VALUES ('a','hello world'), ('b','x y');
SELECT cloudsync_disable('orphan_docs') \gset
DELETE FROM orphan_docs WHERE id = 'b';
SELECT cloudsync_enable('orphan_docs') \gset
SELECT cloudsync_set_column('orphan_docs','body','algo','block') \gset
DO $$ BEGIN
    IF NOT EXISTS (SELECT FROM cloudsync_table_settings WHERE tbl_name='orphan_docs' AND key='algo' AND value='block') THEN
        RAISE EXCEPTION 'Block conversion was not persisted';
    END IF;
    IF (SELECT count(*) FROM orphan_docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('a')) <> 1 THEN
        RAISE EXCEPTION 'Readable row was not migrated';
    END IF;
    IF EXISTS (SELECT FROM orphan_docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('b')) THEN
        RAISE EXCEPTION 'Orphan row produced blocks';
    END IF;
END $$;
\echo [PASS] (57-audit) block conversion skips metadata whose base row is gone

\connect postgres
DROP DATABASE cloudsync_audit_source;
DROP DATABASE cloudsync_audit_target;
\set ON_ERROR_STOP off
