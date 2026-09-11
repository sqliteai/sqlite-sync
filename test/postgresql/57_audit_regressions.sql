-- Audit: database errors must not be mistaken for skippable RLS denials.
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
DO $$
DECLARE denied INTEGER; failed BOOLEAN;
BEGIN
    FOR denied IN 1..3 LOOP
        PERFORM set_config('audit.denied_id', denied::TEXT, false);
        failed := false;
        BEGIN
            PERFORM cloudsync_payload_apply(data) FROM audit_payload;
        EXCEPTION WHEN OTHERS THEN
            IF SQLERRM NOT LIKE '%audit write denied%' THEN RAISE; END IF;
            failed := true;
        END;
        IF NOT failed THEN RAISE EXCEPTION 'Payload silently ignored error at row %', denied; END IF;
        IF EXISTS (SELECT FROM t) THEN RAISE EXCEPTION 'Failed payload committed partial data'; END IF;
        IF coalesce((SELECT value::BIGINT FROM cloudsync_settings WHERE key='check_dbversion'),0) <> 0 THEN
            RAISE EXCEPTION 'Failed payload advanced its checkpoint';
        END IF;
    END LOOP;
END $$;
\echo [PASS] (57-audit) first, middle and final merge errors are propagated without checkpoint advancement

DROP TRIGGER deny_audit ON t;
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
\connect postgres
DROP DATABASE cloudsync_audit_source;
DROP DATABASE cloudsync_audit_target;
\set ON_ERROR_STOP off
