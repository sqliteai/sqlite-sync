#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SQLITE3_BIN="${SQLITE3:-sqlite3}"
PGHOST="${PG_DOCKER_DB_HOST:-localhost}"
PGPORT="${PG_DOCKER_DB_PORT:-5432}"
PGDATABASE_ADMIN="${PG_DOCKER_DB_NAME:-postgres}"
PGUSER="${PG_DOCKER_DB_USER:-postgres}"
export PGPASSWORD="${PG_DOCKER_DB_PASSWORD:-postgres}"

EXTENSION_PATH=""
for candidate in "$ROOT_DIR/dist/cloudsync.dylib" "$ROOT_DIR/dist/cloudsync.so" "$ROOT_DIR/dist/cloudsync.dll"; do
    if [ -f "$candidate" ]; then
        EXTENSION_PATH="$candidate"
        break
    fi
done

if [ -z "$EXTENSION_PATH" ]; then
    echo "[FAIL] SQLite extension not found in dist/"
    exit 1
fi

command -v "$SQLITE3_BIN" >/dev/null 2>&1 || { echo "[FAIL] sqlite3 not found"; exit 1; }
command -v psql >/dev/null 2>&1 || { echo "[FAIL] psql not found"; exit 1; }

TMPDIR="$(mktemp -d)"
SQLITE_DB="$TMPDIR/cross.sqlite"
SQLITE_TO_PG="$TMPDIR/sqlite-to-postgresql.json"
PG_TO_SQLITE="$TMPDIR/postgresql-to-sqlite.json"
PG_TEST_DB="cloudsync_cross_migration"
PG_ADMIN_URL="postgresql://$PGUSER@$PGHOST:$PGPORT/$PGDATABASE_ADMIN"
PG_TEST_URL="postgresql://$PGUSER@$PGHOST:$PGPORT/$PG_TEST_DB"

cleanup() {
    PGPASSWORD="$PGPASSWORD" psql "$PG_ADMIN_URL" -qAt -v ON_ERROR_STOP=0 -c "DROP DATABASE IF EXISTS $PG_TEST_DB;" >/dev/null 2>&1 || true
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

sqlite_exec() {
    "$SQLITE3_BIN" "$SQLITE_DB" <<SQL
.bail on
.load $EXTENSION_PATH
$1
.output /dev/null
SELECT cloudsync_terminate();
SQL
}

sqlite_scalar() {
    "$SQLITE3_BIN" "$SQLITE_DB" "$1"
}

sql_quote_file_for_sqlite() {
    sed "s/'/''/g" "$1"
}

sqlite_apply_payload() {
    local payload_file="$1"
    local payload
    payload="$(sql_quote_file_for_sqlite "$payload_file")"
    sqlite_exec "SELECT cloudsync_migration_apply('$payload');"
}

pg_exec() {
    PGPASSWORD="$PGPASSWORD" psql "$PG_TEST_URL" -qAt -v ON_ERROR_STOP=1 -c "$1"
}

pg_scalar() {
    PGPASSWORD="$PGPASSWORD" psql "$PG_TEST_URL" -qAt -v ON_ERROR_STOP=1 -c "$1"
}

pg_apply_payload() {
    local payload_file="$1"
    {
        printf 'SELECT cloudsync_migration_apply($cloudsync_migration$\n'
        cat "$payload_file"
        printf '\n$cloudsync_migration$);\n'
    } | PGPASSWORD="$PGPASSWORD" psql "$PG_TEST_URL" -qAt -v ON_ERROR_STOP=1 >/dev/null
}

expect_eq() {
    local label="$1"
    local actual="$2"
    local expected="$3"
    if [ "$actual" != "$expected" ]; then
        echo "[FAIL] $label: expected $expected, got $actual"
        exit 1
    fi
}

PGPASSWORD="$PGPASSWORD" psql "$PG_ADMIN_URL" -qAt -v ON_ERROR_STOP=1 <<SQL >/dev/null
DROP DATABASE IF EXISTS $PG_TEST_DB;
CREATE DATABASE $PG_TEST_DB;
SQL
pg_exec "CREATE EXTENSION cloudsync;" >/dev/null

"$SQLITE3_BIN" "$SQLITE_DB" <<SQL >/dev/null
.bail on
.load $EXTENSION_PATH
SELECT cloudsync_alter_create_table('events');
SELECT cloudsync_alter_add_column('events', 'id', 'text', 0);
SELECT cloudsync_alter_add_primary_key('events', 'id');
SELECT cloudsync_alter_add_column('events', 'body', 'text', 0, '');
SELECT cloudsync_alter_add_column('events', 'created_at', 'timestamp', 0, '2026-04-28T00:00:00Z');
SELECT cloudsync_alter_augment_table('events');
SELECT cloudsync_alter_set_block_lww('events', 'body', char(10));
SELECT cloudsync_alter_sql('CREATE INDEX events_created_common_idx ON events(created_at)');
SELECT cloudsync_alter_sqlite('CREATE INDEX events_body_sqlite_idx ON events(body)');
SELECT cloudsync_alter_postgresql('CREATE INDEX events_body_pg_idx ON events(body)');
.once $SQLITE_TO_PG
SELECT cloudsync_alter_preview();
.output /dev/null
SELECT cloudsync_terminate();
SQL

sqlite_apply_payload "$SQLITE_TO_PG" >/dev/null
pg_apply_payload "$SQLITE_TO_PG"

sqlite_exec "INSERT INTO events (id, body) VALUES ('s1', 'one' || char(10) || 'two');" >/dev/null
pg_exec "INSERT INTO events (id, body) VALUES ('p1', E'one\\ntwo');" >/dev/null

expect_eq "SQLite->PostgreSQL table" "$(pg_scalar "SELECT count(*) FROM information_schema.tables WHERE table_name='events';")" "1"
expect_eq "SQLite->PostgreSQL block settings" "$(pg_scalar "SELECT count(*) FROM cloudsync_table_settings WHERE tbl_name='events' AND col_name='body' AND key='algo' AND value='block';")" "1"
expect_eq "SQLite common raw SQL index" "$(sqlite_scalar "SELECT count(*) FROM sqlite_master WHERE type='index' AND name='events_created_common_idx';")" "1"
expect_eq "SQLite dialect raw SQL index" "$(sqlite_scalar "SELECT count(*) FROM sqlite_master WHERE type='index' AND name='events_body_sqlite_idx';")" "1"
expect_eq "SQLite skipped PostgreSQL raw SQL index" "$(sqlite_scalar "SELECT count(*) FROM sqlite_master WHERE type='index' AND name='events_body_pg_idx';")" "0"
expect_eq "PostgreSQL common raw SQL index" "$(pg_scalar "SELECT count(*) FROM pg_indexes WHERE indexname='events_created_common_idx';")" "1"
expect_eq "PostgreSQL dialect raw SQL index" "$(pg_scalar "SELECT count(*) FROM pg_indexes WHERE indexname='events_body_pg_idx';")" "1"
expect_eq "PostgreSQL skipped SQLite raw SQL index" "$(pg_scalar "SELECT count(*) FROM pg_indexes WHERE indexname='events_body_sqlite_idx';")" "0"
expect_eq "SQLite block rows" "$(sqlite_scalar "SELECT count(*) FROM events_cloudsync_blocks;")" "2"
expect_eq "PostgreSQL block rows" "$(pg_scalar "SELECT count(*) FROM events_cloudsync_blocks;")" "2"
echo "[PASS] SQLite -> PostgreSQL createTable/augment/block migration"

PGPASSWORD="$PGPASSWORD" psql "$PG_TEST_URL" -qAt -v ON_ERROR_STOP=1 <<SQL >/dev/null
\\o /dev/null
SELECT cloudsync_alter_add_column('events', 'metadata', 'json', false, '{}');
SELECT cloudsync_alter_add_column_sqlite('events', 'metadata', 'TEXT', false, '''{}''');
SELECT cloudsync_alter_add_column_postgresql('events', 'metadata', 'JSONB', false, '''{}''::jsonb');
SELECT cloudsync_alter_sql('CREATE INDEX events_metadata_common_idx ON events(created_at)');
SELECT cloudsync_alter_sqlite('CREATE INDEX events_metadata_sqlite_idx ON events(metadata)');
SELECT cloudsync_alter_postgresql('CREATE INDEX events_metadata_pg_idx ON events(metadata)');
\\o $PG_TO_SQLITE
SELECT cloudsync_alter_preview();
\\o
SQL

pg_apply_payload "$PG_TO_SQLITE"
sqlite_apply_payload "$PG_TO_SQLITE" >/dev/null

expect_eq "PostgreSQL->SQLite column on SQLite" "$(sqlite_scalar "SELECT count(*) FROM pragma_table_info('events') WHERE name='metadata';")" "1"
expect_eq "PostgreSQL JSONB override" "$(pg_scalar "SELECT count(*) FROM information_schema.columns WHERE table_name='events' AND column_name='metadata' AND data_type='jsonb';")" "1"
expect_eq "PostgreSQL->SQLite common raw SQL index on SQLite" "$(sqlite_scalar "SELECT count(*) FROM sqlite_master WHERE type='index' AND name='events_metadata_common_idx';")" "1"
expect_eq "PostgreSQL->SQLite dialect raw SQL index on SQLite" "$(sqlite_scalar "SELECT count(*) FROM sqlite_master WHERE type='index' AND name='events_metadata_sqlite_idx';")" "1"
expect_eq "PostgreSQL skipped SQLite-specific raw SQL index" "$(pg_scalar "SELECT count(*) FROM pg_indexes WHERE indexname='events_metadata_sqlite_idx';")" "0"
expect_eq "PostgreSQL dialect raw SQL index" "$(pg_scalar "SELECT count(*) FROM pg_indexes WHERE indexname='events_metadata_pg_idx';")" "1"

sqlite_exec "INSERT INTO events (id, body) VALUES ('s2', 'three');" >/dev/null
pg_exec "INSERT INTO events (id, body) VALUES ('p2', 'three');" >/dev/null
expect_eq "SQLite default override" "$(sqlite_scalar "SELECT metadata FROM events WHERE id='s2';")" "{}"
expect_eq "PostgreSQL default override" "$(pg_scalar "SELECT metadata::text FROM events WHERE id='p2';")" "{}"
echo "[PASS] PostgreSQL -> SQLite dialect override migration"

echo "[PASS] Cross-dialect schema migration tests completed"
