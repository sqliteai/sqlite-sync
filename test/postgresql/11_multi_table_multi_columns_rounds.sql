-- 'Test multi-table multi-db roundtrip'
-- simulate the sport tracker app from examples
-- Steps:
-- 1) Create three databases, initialize users/activities/workouts and cloudsync
-- 2) Round 1: seed base data on A and sync to B/C
-- 3) Round 2: concurrent updates/inserts on A/B/C, then sync
-- 4) Round 3: more concurrent edits, then sync
-- 5) Verify convergence per table across all three databases

\set testid '11'
\ir helper_test_init.sql

-- Step 1: setup databases and schema
\if :{?DEBUG_MERGE}
\echo '[STEP 1] Setup databases and schema'
\endif
\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_11_a;
DROP DATABASE IF EXISTS cloudsync_test_11_b;
DROP DATABASE IF EXISTS cloudsync_test_11_c;
CREATE DATABASE cloudsync_test_11_a;
CREATE DATABASE cloudsync_test_11_b;
CREATE DATABASE cloudsync_test_11_c;

\connect cloudsync_test_11_a
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS workouts;
DROP TABLE IF EXISTS activities;
DROP TABLE IF EXISTS users;
CREATE TABLE IF NOT EXISTS users (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT UNIQUE NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS activities (
    id TEXT PRIMARY KEY NOT NULL,
    type TEXT NOT NULL DEFAULT 'runnning',
    duration INTEGER,
    distance DOUBLE PRECISION,
    calories INTEGER,
    date TEXT,
    notes TEXT,
    user_id TEXT REFERENCES users (id)
);
CREATE TABLE IF NOT EXISTS workouts (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT,
    type TEXT,
    duration INTEGER,
    exercises TEXT,
    date TEXT,
    completed INTEGER DEFAULT 0,
    user_id TEXT
);
SELECT cloudsync_init('users', 'CLS', true) AS _init_users_a \gset
SELECT cloudsync_init('activities', 'CLS', true) AS _init_activities_a \gset
SELECT cloudsync_init('workouts', 'CLS', true) AS _init_workouts_a \gset

\connect cloudsync_test_11_b
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS workouts;
DROP TABLE IF EXISTS activities;
DROP TABLE IF EXISTS users;
CREATE TABLE IF NOT EXISTS users (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT UNIQUE NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS activities (
    id TEXT PRIMARY KEY NOT NULL,
    type TEXT NOT NULL DEFAULT 'runnning',
    duration INTEGER,
    distance DOUBLE PRECISION,
    calories INTEGER,
    date TEXT,
    notes TEXT,
    user_id TEXT REFERENCES users (id)
);
CREATE TABLE IF NOT EXISTS workouts (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT,
    type TEXT,
    duration INTEGER,
    exercises TEXT,
    date TEXT,
    completed INTEGER DEFAULT 0,
    user_id TEXT
);
SELECT cloudsync_init('users', 'CLS', true) AS _init_users_b \gset
SELECT cloudsync_init('activities', 'CLS', true) AS _init_activities_b \gset
SELECT cloudsync_init('workouts', 'CLS', true) AS _init_workouts_b \gset

\connect cloudsync_test_11_c
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
DROP TABLE IF EXISTS workouts;
DROP TABLE IF EXISTS activities;
DROP TABLE IF EXISTS users;
CREATE TABLE IF NOT EXISTS users (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT UNIQUE NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS activities (
    id TEXT PRIMARY KEY NOT NULL,
    type TEXT NOT NULL DEFAULT 'runnning',
    duration INTEGER,
    distance DOUBLE PRECISION,
    calories INTEGER,
    date TEXT,
    notes TEXT,
    user_id TEXT REFERENCES users (id)
);
CREATE TABLE IF NOT EXISTS workouts (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT,
    type TEXT,
    duration INTEGER,
    exercises TEXT,
    date TEXT,
    completed INTEGER DEFAULT 0,
    user_id TEXT
);
SELECT cloudsync_init('users', 'CLS', true) AS _init_users_c \gset
SELECT cloudsync_init('activities', 'CLS', true) AS _init_activities_c \gset
SELECT cloudsync_init('workouts', 'CLS', true) AS _init_workouts_c \gset

-- Step 2: Round 1 seed base data on A, sync to B/C
\if :{?DEBUG_MERGE}
\echo '[STEP 2] Round 1 seed base data on A, sync to B/C'
\endif
\connect cloudsync_test_11_a
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_11_a INSERT users u1=alice'
\endif
INSERT INTO users (id, name) VALUES ('u1', 'alice');
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_11_a INSERT activities act1'
\endif
INSERT INTO activities (id, type, duration, distance, calories, date, notes, user_id)
VALUES ('act1', 'running', 30, 5.0, 200, '2026-01-01', 'seed', 'u1');
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_11_a INSERT workouts w1'
\endif
INSERT INTO workouts (id, name, type, duration, exercises, date, completed, user_id)
VALUES ('w1', 'base', 'cardio', 30, 'run', '2026-01-01', 0, 'u1');
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_a_r1,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_a_r1_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_11_b
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_b_r1,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_b_r1_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_11_c
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_c_r1,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_c_r1_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_11_a
\if :{?DEBUG_MERGE}
\echo '[INFO] round1 before merge cloudsync_test_11_a users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round1 before merge cloudsync_test_11_a activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round1 before merge cloudsync_test_11_a workouts'
SELECT * FROM workouts ORDER BY id;
\endif
\if :payload_b_r1_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round1 apply b -> a'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r1', 3), 'hex')) AS _apply_a_r1_b \gset
\else
SELECT 0 AS _apply_a_r1_b \gset
\endif
\if :payload_c_r1_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round1 apply c -> a'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r1', 3), 'hex')) AS _apply_a_r1_c \gset
\else
SELECT 0 AS _apply_a_r1_c \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round1 after merge cloudsync_test_11_a users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round1 after merge cloudsync_test_11_a activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round1 after merge cloudsync_test_11_a workouts'
SELECT * FROM workouts ORDER BY id;
\endif

\connect cloudsync_test_11_b
\if :{?DEBUG_MERGE}
\echo '[INFO] round1 before merge cloudsync_test_11_b users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round1 before merge cloudsync_test_11_b activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round1 before merge cloudsync_test_11_b workouts'
SELECT * FROM workouts ORDER BY id;
\endif
\if :payload_a_r1_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round1 apply a -> b'
\echo ### payload_a_r1: :payload_a_r1
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r1', 3), 'hex')) AS _apply_b_r1_a \gset
\else
SELECT 0 AS _apply_b_r1_a \gset
\endif
\if :payload_c_r1_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round1 apply c -> b'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r1', 3), 'hex')) AS _apply_b_r1_c \gset
\else
SELECT 0 AS _apply_b_r1_c \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round1 after merge cloudsync_test_11_b users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round1 after merge cloudsync_test_11_b activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round1 after merge cloudsync_test_11_b workouts'
SELECT * FROM workouts ORDER BY id;
\endif

\connect cloudsync_test_11_c
\if :{?DEBUG_MERGE}
\echo '[INFO] round1 before merge cloudsync_test_11_c users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round1 before merge cloudsync_test_11_c activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round1 before merge cloudsync_test_11_c workouts'
SELECT * FROM workouts ORDER BY id;
\endif
\if :payload_a_r1_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round1 apply a -> c'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r1', 3), 'hex')) AS _apply_c_r1_a \gset
\else
SELECT 0 AS _apply_c_r1_a \gset
\endif
\if :payload_b_r1_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round1 apply b -> c'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r1', 3), 'hex')) AS _apply_c_r1_b \gset
\else
SELECT 0 AS _apply_c_r1_b \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round1 after merge cloudsync_test_11_c users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round1 after merge cloudsync_test_11_c activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round1 after merge cloudsync_test_11_c workouts'
SELECT * FROM workouts ORDER BY id;
\endif

-- Step 3: Round 2 concurrent updates and inserts across nodes
\if :{?DEBUG_MERGE}
\echo '[STEP 3] Round 2 concurrent updates and inserts across nodes'
\endif
\connect cloudsync_test_11_a
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_11_a UPDATE users u1=alice_a2'
\endif
UPDATE users SET name = 'alice_a2' WHERE id = 'u1';
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_11_a UPDATE activities act1 duration/calories'
\endif
UPDATE activities SET duration = 35, calories = 220 WHERE id = 'act1';
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_11_a INSERT workouts w2'
\endif
INSERT INTO workouts (id, name, type, duration, exercises, date, completed, user_id)
VALUES ('w2', 'tempo', 'cardio', 40, 'run', '2026-01-02', 0, 'u1');
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_a_r2,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_a_r2_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_11_b
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_11_b UPDATE users u1=alice_b2'
\endif
UPDATE users SET name = 'alice_b2' WHERE id = 'u1';
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_11_b UPDATE workouts w1 completed=1'
\endif
UPDATE workouts SET completed = 1 WHERE id = 'w1';
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_11_b INSERT users u2=bob'
\endif
INSERT INTO users (id, name) VALUES ('u2', 'bob');
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_11_b INSERT activities act2'
\endif
INSERT INTO activities (id, type, duration, distance, calories, date, notes, user_id)
VALUES ('act2', 'cycling', 60, 20.0, 500, '2026-01-02', 'b_seed', 'u2');
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_b_r2,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_b_r2_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_11_c
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_11_c UPDATE activities act1 notes=c_note'
\endif
UPDATE activities SET notes = 'c_note' WHERE id = 'act1';
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_11_c UPDATE workouts w1 type=strength'
\endif
UPDATE workouts SET type = 'strength' WHERE id = 'w1';
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_11_c INSERT workouts w3'
\endif
INSERT INTO workouts (id, name, type, duration, exercises, date, completed, user_id)
VALUES ('w3', 'lift', 'strength', 45, 'squat', '2026-01-02', 0, 'u1');
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_c_r2,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_c_r2_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_11_a
\if :{?DEBUG_MERGE}
\echo '[INFO] round2 before merge cloudsync_test_11_a users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round2 before merge cloudsync_test_11_a activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round2 before merge cloudsync_test_11_a workouts'
SELECT * FROM workouts ORDER BY id;
\endif
\if :payload_b_r2_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round2 apply b -> a'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r2', 3), 'hex')) AS _apply_a_r2_b \gset
\else
SELECT 0 AS _apply_a_r2_b \gset
\endif
\if :payload_c_r2_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round2 apply c -> a'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r2', 3), 'hex')) AS _apply_a_r2_c \gset
\else
SELECT 0 AS _apply_a_r2_c \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round2 after merge cloudsync_test_11_a users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round2 after merge cloudsync_test_11_a activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round2 after merge cloudsync_test_11_a workouts'
SELECT * FROM workouts ORDER BY id;
\endif

\connect cloudsync_test_11_b
\if :{?DEBUG_MERGE}
\echo '[INFO] round2 before merge cloudsync_test_11_b users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round2 before merge cloudsync_test_11_b activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round2 before merge cloudsync_test_11_b workouts'
SELECT * FROM workouts ORDER BY id;
\endif
\if :payload_a_r2_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round2 apply a -> b'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r2', 3), 'hex')) AS _apply_b_r2_a \gset
\else
SELECT 0 AS _apply_b_r2_a \gset
\endif
\if :payload_c_r2_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round2 apply c -> b'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r2', 3), 'hex')) AS _apply_b_r2_c \gset
\else
SELECT 0 AS _apply_b_r2_c \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round2 after merge cloudsync_test_11_b users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round2 after merge cloudsync_test_11_b activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round2 after merge cloudsync_test_11_b workouts'
SELECT * FROM workouts ORDER BY id;
\endif

\connect cloudsync_test_11_c
\if :{?DEBUG_MERGE}
\echo '[INFO] round2 before merge cloudsync_test_11_c users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round2 before merge cloudsync_test_11_c activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round2 before merge cloudsync_test_11_c workouts'
SELECT * FROM workouts ORDER BY id;
\endif
\if :payload_a_r2_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round2 apply a -> c'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r2', 3), 'hex')) AS _apply_c_r2_a \gset
\else
SELECT 0 AS _apply_c_r2_a \gset
\endif
\if :payload_b_r2_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round2 apply b -> c'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r2', 3), 'hex')) AS _apply_c_r2_b \gset
\else
SELECT 0 AS _apply_c_r2_b \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round2 after merge cloudsync_test_11_c users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round2 after merge cloudsync_test_11_c activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round2 after merge cloudsync_test_11_c workouts'
SELECT * FROM workouts ORDER BY id;
\endif

-- Step 4: Round 3 more concurrent edits
\if :{?DEBUG_MERGE}
\echo '[STEP 4] Round 3 more concurrent edits'
\endif
\connect cloudsync_test_11_a
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_11_a UPDATE workouts w2 completed=1'
\endif
UPDATE workouts SET completed = 1 WHERE id = 'w2';
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_a_r3,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_a_r3_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_11_b
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_11_b UPDATE activities act1 distance=6.5'
\endif
UPDATE activities SET distance = 6.5 WHERE id = 'act1';
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_b_r3,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_b_r3_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_11_c
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_11_c UPDATE users u1=alice_c3'
\endif
UPDATE users SET name = 'alice_c3' WHERE id = 'u1';
\if :{?DEBUG_MERGE}
\echo '[INFO] cloudsync_test_11_c INSERT activities act3'
\endif
INSERT INTO activities (id, type, duration, distance, calories, date, notes, user_id)
VALUES ('act3', 'yoga', 45, 0.0, 150, '2026-01-03', 'c_seed', 'u1');
SELECT CASE WHEN payload IS NULL OR octet_length(payload) = 0
            THEN ''
            ELSE '\x' || encode(payload, 'hex')
       END AS payload_c_r3,
       (payload IS NOT NULL AND octet_length(payload) > 0) AS payload_c_r3_ok
FROM (
  SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
  FROM cloudsync_changes
  WHERE site_id = cloudsync_siteid()
) AS p \gset

\connect cloudsync_test_11_a
\if :{?DEBUG_MERGE}
\echo '[INFO] round3 before merge cloudsync_test_11_a users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round3 before merge cloudsync_test_11_a activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round3 before merge cloudsync_test_11_a workouts'
SELECT * FROM workouts ORDER BY id;
\endif
\if :payload_b_r3_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round3 apply b -> a'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r3', 3), 'hex')) AS _apply_a_r3_b \gset
\else
SELECT 0 AS _apply_a_r3_b \gset
\endif
\if :payload_c_r3_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round3 apply c -> a'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r3', 3), 'hex')) AS _apply_a_r3_c \gset
\else
SELECT 0 AS _apply_a_r3_c \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round3 after merge cloudsync_test_11_a users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round3 after merge cloudsync_test_11_a activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round3 after merge cloudsync_test_11_a workouts'
SELECT * FROM workouts ORDER BY id;
\endif

\connect cloudsync_test_11_b
\if :{?DEBUG_MERGE}
\echo '[INFO] round3 before merge cloudsync_test_11_b users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round3 before merge cloudsync_test_11_b activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round3 before merge cloudsync_test_11_b workouts'
SELECT * FROM workouts ORDER BY id;
\endif
\if :payload_a_r3_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round3 apply a -> b'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r3', 3), 'hex')) AS _apply_b_r3_a \gset
\else
SELECT 0 AS _apply_b_r3_a \gset
\endif
\if :payload_c_r3_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round3 apply c -> b'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_c_r3', 3), 'hex')) AS _apply_b_r3_c \gset
\else
SELECT 0 AS _apply_b_r3_c \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round3 after merge cloudsync_test_11_b users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round3 after merge cloudsync_test_11_b activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round3 after merge cloudsync_test_11_b workouts'
SELECT * FROM workouts ORDER BY id;
\endif

\connect cloudsync_test_11_c
\if :{?DEBUG_MERGE}
\echo '[INFO] round3 before merge cloudsync_test_11_c users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round3 before merge cloudsync_test_11_c activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round3 before merge cloudsync_test_11_c workouts'
SELECT * FROM workouts ORDER BY id;
\endif
\if :payload_a_r3_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round3 apply a -> c'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_a_r3', 3), 'hex')) AS _apply_c_r3_a \gset
\else
SELECT 0 AS _apply_c_r3_a \gset
\endif
\if :payload_b_r3_ok
\if :{?DEBUG_MERGE}
\echo '[MERGE] round3 apply b -> c'
\endif
SELECT cloudsync_payload_apply(decode(substr(:'payload_b_r3', 3), 'hex')) AS _apply_c_r3_b \gset
\else
SELECT 0 AS _apply_c_r3_b \gset
\endif
\if :{?DEBUG_MERGE}
\echo '[INFO] round3 after merge cloudsync_test_11_c users'
SELECT * FROM users ORDER BY id;
\echo '[INFO] round3 after merge cloudsync_test_11_c activities'
SELECT * FROM activities ORDER BY id;
\echo '[INFO] round3 after merge cloudsync_test_11_c workouts'
SELECT * FROM workouts ORDER BY id;
\endif

-- Step 5: final consistency check across all three databases
\if :{?DEBUG_MERGE}
\echo '[STEP 5] Final consistency check across all three databases'
\endif
\connect cloudsync_test_11_a
SELECT md5(COALESCE(string_agg(id || ':' || name, ',' ORDER BY id), '')) AS users_hash_a
FROM users \gset
SELECT md5(COALESCE(string_agg(
    id || ':' || COALESCE(type, '') || ':' || COALESCE(duration::text, '') || ':' ||
    COALESCE(distance::text, '') || ':' || COALESCE(calories::text, '') || ':' ||
    COALESCE(date, '') || ':' || COALESCE(notes, '') || ':' || COALESCE(user_id, ''),
    ',' ORDER BY id
), '')) AS activities_hash_a
FROM activities \gset
SELECT md5(COALESCE(string_agg(
    id || ':' || COALESCE(name, '') || ':' || COALESCE(type, '') || ':' ||
    COALESCE(duration::text, '') || ':' || COALESCE(exercises, '') || ':' ||
    COALESCE(date, '') || ':' || COALESCE(completed::text, '') || ':' || COALESCE(user_id, ''),
    ',' ORDER BY id
), '')) AS workouts_hash_a
FROM workouts \gset

\connect cloudsync_test_11_b
SELECT md5(COALESCE(string_agg(id || ':' || name, ',' ORDER BY id), '')) AS users_hash_b
FROM users \gset
SELECT md5(COALESCE(string_agg(
    id || ':' || COALESCE(type, '') || ':' || COALESCE(duration::text, '') || ':' ||
    COALESCE(distance::text, '') || ':' || COALESCE(calories::text, '') || ':' ||
    COALESCE(date, '') || ':' || COALESCE(notes, '') || ':' || COALESCE(user_id, ''),
    ',' ORDER BY id
), '')) AS activities_hash_b
FROM activities \gset
SELECT md5(COALESCE(string_agg(
    id || ':' || COALESCE(name, '') || ':' || COALESCE(type, '') || ':' ||
    COALESCE(duration::text, '') || ':' || COALESCE(exercises, '') || ':' ||
    COALESCE(date, '') || ':' || COALESCE(completed::text, '') || ':' || COALESCE(user_id, ''),
    ',' ORDER BY id
), '')) AS workouts_hash_b
FROM workouts \gset

\connect cloudsync_test_11_c
SELECT md5(COALESCE(string_agg(id || ':' || name, ',' ORDER BY id), '')) AS users_hash_c
FROM users \gset
SELECT md5(COALESCE(string_agg(
    id || ':' || COALESCE(type, '') || ':' || COALESCE(duration::text, '') || ':' ||
    COALESCE(distance::text, '') || ':' || COALESCE(calories::text, '') || ':' ||
    COALESCE(date, '') || ':' || COALESCE(notes, '') || ':' || COALESCE(user_id, ''),
    ',' ORDER BY id
), '')) AS activities_hash_c
FROM activities \gset
SELECT md5(COALESCE(string_agg(
    id || ':' || COALESCE(name, '') || ':' || COALESCE(type, '') || ':' ||
    COALESCE(duration::text, '') || ':' || COALESCE(exercises, '') || ':' ||
    COALESCE(date, '') || ':' || COALESCE(completed::text, '') || ':' || COALESCE(user_id, ''),
    ',' ORDER BY id
), '')) AS workouts_hash_c
FROM workouts \gset

SELECT (:'users_hash_a' = :'users_hash_b' AND :'users_hash_a' = :'users_hash_c') AS users_ok \gset
\if :users_ok
\echo [PASS] (:testid) Multi-table users convergence
\else
\echo [FAIL] (:testid) Multi-table users convergence
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (:'activities_hash_a' = :'activities_hash_b' AND :'activities_hash_a' = :'activities_hash_c') AS activities_ok \gset
\if :activities_ok
\echo [PASS] (:testid) Multi-table activities convergence
\else
\echo [FAIL] (:testid) Multi-table activities convergence
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (:'workouts_hash_a' = :'workouts_hash_b' AND :'workouts_hash_a' = :'workouts_hash_c') AS workouts_ok \gset
\if :workouts_ok
\echo [PASS] (:testid) Multi-table workouts convergence
\else
\echo [FAIL] (:testid) Multi-table workouts convergence
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Cleanup: Drop test databases if not in DEBUG mode and no failures
\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_11_a;
DROP DATABASE IF EXISTS cloudsync_test_11_b;
DROP DATABASE IF EXISTS cloudsync_test_11_c;
\endif