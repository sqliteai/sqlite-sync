// Focused audit regressions. No server or on-disk database required.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "sqlite3.h"
#include "cloudsync.h"
#include "cloudsync_sqlite.h"
#include "utils.h"
#include "pk.h"
extern int cloudsync_changesvtab_best_index(sqlite3_vtab *, sqlite3_index_info *);
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)
static sqlite3 *open_db(void) {
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    CHECK(sqlite3_cloudsync_init(db, NULL, NULL) == SQLITE_OK);
    return db;
}
static int sql(sqlite3 *db, const char *query) { return sqlite3_exec(db, query, NULL, NULL, NULL); }
static int close_db(sqlite3 *db) {
    CHECK(sql(db, "SELECT cloudsync_terminate()") == SQLITE_OK);
    return sqlite3_close(db);
}
static int64_t scalar(sqlite3 *db, const char *query) {
    sqlite3_stmt *vm = NULL;
    int64_t value = INT64_MIN;
    CHECK(sqlite3_prepare_v2(db, query, -1, &vm, NULL) == SQLITE_OK);
    if (vm && sqlite3_step(vm) == SQLITE_ROW) value = sqlite3_column_int64(vm, 0);
    else CHECK(false);
    sqlite3_finalize(vm);
    return value;
}
static void test_clocks_and_double(void) {
    sqlite3 *db = open_db();
    CHECK(sql(db, "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, value TEXT); SELECT cloudsync_init('t');") == SQLITE_OK);
    CHECK(sql(db, "INSERT INTO cloudsync_changes VALUES ('t', cloudsync_pk_encode('key'), 'value', 'ok', 4294967297, 4294967300, randomblob(16), 4294967297, 4294967310);") == SQLITE_OK);
    CHECK(scalar(db, "SELECT col_version FROM cloudsync_changes WHERE col_name='value'") == INT64_C(4294967297));
    CHECK(scalar(db, "SELECT db_version FROM cloudsync_changes WHERE col_name='value'") >= INT64_C(4294967300));
    CHECK(scalar(db, "SELECT seq FROM cloudsync_changes WHERE col_name='value'") == INT64_C(4294967310));
    CHECK(scalar(db, "SELECT cl FROM cloudsync_changes WHERE col_name='value'") == INT64_C(4294967297));
    CHECK(scalar(db, "SELECT count(*) FROM t WHERE value='ok'") == 1);
    CHECK(scalar(db, "SELECT hex(cloudsync_pk_encode(1.0))='0102000000000000F03F'") == 1);
    CHECK(scalar(db, "SELECT cloudsync_pk_decode(x'0102000000000000F03F',1)=1.0") == 1);
    CHECK(scalar(db, "SELECT cloudsync_pk_decode(cloudsync_pk_encode(-1.5),1)=-1.5") == 1);
    CHECK(close_db(db) == SQLITE_OK);
}
static void test_best_index(void) {
    struct sqlite3_index_constraint constraints[3] = {
        {.iColumn=0, .op=SQLITE_INDEX_CONSTRAINT_EQ, .usable=0},
        {.iColumn=1, .op=SQLITE_INDEX_CONSTRAINT_MATCH, .usable=1},
        {.iColumn=5, .op=SQLITE_INDEX_CONSTRAINT_GT, .usable=1}
    };
    struct sqlite3_index_constraint_usage usage[3] = {{0}};
    sqlite3_index_info info = {.nConstraint=3, .aConstraint=constraints, .aConstraintUsage=usage};
    CHECK(cloudsync_changesvtab_best_index(NULL, &info) == SQLITE_OK);
    CHECK(strcmp(info.idxStr, "WHERE db_version > ? ORDER BY db_version, seq ASC") == 0);
    CHECK(usage[2].argvIndex == 1);
    sqlite3_free(info.idxStr);
    info.nConstraint = 2;
    CHECK(cloudsync_changesvtab_best_index(NULL, &info) == SQLITE_OK);
    CHECK(strcmp(info.idxStr, " ORDER BY db_version, seq ASC") == 0);
    sqlite3_free(info.idxStr);
}
// A private directory for this run's on-disk databases, removed with its content at the
// end; a failed run leaves nothing behind in a shared temporary directory.
static char scratch_dir[256];
static bool scratch_create(void) {
#ifdef _WIN32
    char base[MAX_PATH];
    DWORD n = GetTempPathA(sizeof(base), base);
    if (!n || n >= sizeof(base)) return false;
    int len = snprintf(scratch_dir, sizeof(scratch_dir), "%scloudsync-rr-%lu-%llu", base,
                       (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    return len > 0 && (size_t)len < sizeof(scratch_dir) && CreateDirectoryA(scratch_dir, NULL);
#else
    const char *base = getenv("TMPDIR");
    if (!base || !*base) base = ".";
    int len = snprintf(scratch_dir, sizeof(scratch_dir), "%s/cloudsync-rr-XXXXXX", base);
    return len > 0 && (size_t)len < sizeof(scratch_dir) && mkdtemp(scratch_dir) != NULL;
#endif
}
static void scratch_remove(const char *const *names, int count) {
    char path[512];
    for (int i = 0; i < count; i++) {
        snprintf(path, sizeof(path), "%s/%s", scratch_dir, names[i]);
        remove(path);
    }
#ifdef _WIN32
    RemoveDirectoryA(scratch_dir);
#else
    rmdir(scratch_dir);
#endif
}
static int apply_payload(sqlite3 *source, sqlite3 *target) {
    sqlite3_stmt *read = NULL, *write = NULL;
    CHECK(sqlite3_prepare_v2(source, "SELECT cloudsync_payload_encode(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq) FROM cloudsync_changes", -1, &read, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(read) == SQLITE_ROW);
    CHECK(sqlite3_prepare_v2(target, "SELECT cloudsync_payload_decode(?1)", -1, &write, NULL) == SQLITE_OK);
    CHECK(sqlite3_bind_value(write, 1, sqlite3_column_value(read, 0)) == SQLITE_OK);
    int rc = sqlite3_step(write);
    sqlite3_finalize(write);
    sqlite3_finalize(read);
    return rc;
}
static void test_payload_errors(void) {
    // The apply stops at the first failed write (first, middle or last row) with the
    // database's own error. The rows before it are kept, the failed row leaves neither
    // data nor metadata, and the cursor does not move: once the cause is fixed, the same
    // payload applies in full.
    for (int denied = 1; denied <= 3; denied++) {
        sqlite3 *source = open_db(), *target = open_db();
        const char *schema = "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL,value TEXT); SELECT cloudsync_init('t');";
        CHECK(sql(source, schema) == SQLITE_OK && sql(target, schema) == SQLITE_OK);
        CHECK(sql(source, "INSERT INTO t VALUES('1','a'),('2','b'),('3','c');") == SQLITE_OK);
        char trigger[256];
        snprintf(trigger, sizeof(trigger), "CREATE TRIGGER deny BEFORE INSERT ON t WHEN NEW.id='%d' BEGIN SELECT RAISE(ABORT,'denied'); END", denied);
        CHECK(sql(target, trigger) == SQLITE_OK);
        CHECK(apply_payload(source, target) != SQLITE_ROW);
        CHECK(strstr(sqlite3_errmsg(target), "denied") != NULL);
        CHECK((sqlite3_extended_errcode(target) & 0xFF) == SQLITE_CONSTRAINT);
        char query[160];
        snprintf(query, sizeof(query), "SELECT count(*) FROM t WHERE id<'%d'", denied);
        CHECK(scalar(target, query) == denied - 1);
        CHECK(scalar(target, "SELECT count(*) FROM t") == denied - 1);
        snprintf(query, sizeof(query), "SELECT count(*) FROM t_cloudsync WHERE pk=cloudsync_pk_encode('%d')", denied);
        CHECK(scalar(target, query) == 0);
        CHECK(scalar(target, "SELECT coalesce((SELECT value FROM cloudsync_settings WHERE key='check_dbversion'),0)") == 0);
        CHECK(sqlite3_get_autocommit(target));
        CHECK(sql(target, "DROP TRIGGER deny") == SQLITE_OK);
        CHECK(apply_payload(source, target) == SQLITE_ROW);
        CHECK(scalar(target, "SELECT count(*) FROM t") == 3);
        CHECK(scalar(target, "SELECT coalesce((SELECT value FROM cloudsync_settings WHERE key='check_dbversion'),0)") > 0);
        CHECK(close_db(source) == SQLITE_OK);
        CHECK(close_db(target) == SQLITE_OK);
    }

    // A transient failure (the database is locked by another connection) fails the apply
    // the same way and leaves the cursor in place.
    {
        char path[512];
        CHECK(scratch_create());
        snprintf(path, sizeof(path), "%s/busy.db", scratch_dir);
        sqlite3 *source = open_db(), *target = NULL, *locker = NULL;
        CHECK(sqlite3_open(path, &target) == SQLITE_OK);
        CHECK(sqlite3_cloudsync_init(target, NULL, NULL) == SQLITE_OK);
        const char *schema = "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL,value TEXT); SELECT cloudsync_init('t');";
        CHECK(sql(source, schema) == SQLITE_OK && sql(target, schema) == SQLITE_OK);
        CHECK(sql(source, "INSERT INTO t VALUES('1','a'),('2','b');") == SQLITE_OK);
        CHECK(sqlite3_open(path, &locker) == SQLITE_OK);
        CHECK(sql(locker, "BEGIN IMMEDIATE") == SQLITE_OK);
        CHECK(apply_payload(source, target) != SQLITE_ROW);
        CHECK(sql(locker, "ROLLBACK") == SQLITE_OK);
        CHECK(sqlite3_close(locker) == SQLITE_OK);
        CHECK(scalar(target, "SELECT coalesce((SELECT value FROM cloudsync_settings WHERE key='check_dbversion'),0)") == 0);
        CHECK(sqlite3_get_autocommit(target));
        // Once the lock is gone the same payload applies in full.
        CHECK(apply_payload(source, target) == SQLITE_ROW);
        CHECK(scalar(target, "SELECT count(*) FROM t") == 2);
        CHECK(close_db(source) == SQLITE_OK);
        CHECK(close_db(target) == SQLITE_OK);
        const char *const files[] = {"busy.db", "busy.db-journal"};
        scratch_remove(files, 2);
    }

    sqlite3 *db = open_db();
    CHECK(sql(db, "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL); SELECT cloudsync_init('t');") == SQLITE_OK);
    // v1 headers (no checksum or schema requirements) over a 2-byte compressed body.
    // Declaring 4GB, or 268MB, from 2 bytes is inconsistent with LZ4's maximum ratio.
    CHECK(sql(db, "SELECT cloudsync_payload_decode(x'434C535901000000FFFFFFFF00090000000000000000000000000000000000000000')") != SQLITE_OK);
    CHECK(strstr(sqlite3_errmsg(db), "inconsistent") != NULL);
    CHECK(sql(db, "SELECT cloudsync_payload_decode(x'434C5359010000001000000100090000000000000000000000000000000000000000')") != SQLITE_OK);
    CHECK(strstr(sqlite3_errmsg(db), "inconsistent") != NULL);
    // At the bound (2 * 255 + 64 = 574 bytes) the size is plausible and decompression is
    // attempted, failing on the bogus data; one byte past it is rejected up front.
    CHECK(sql(db, "SELECT cloudsync_payload_decode(x'434C5359010000000000023E00090000000000000000000000000000000000000000')") != SQLITE_OK);
    CHECK(strstr(sqlite3_errmsg(db), "unable to decompress") != NULL);
    CHECK(sql(db, "SELECT cloudsync_payload_decode(x'434C5359010000000000023F00090000000000000000000000000000000000000000')") != SQLITE_OK);
    CHECK(strstr(sqlite3_errmsg(db), "inconsistent") != NULL);
    CHECK(close_db(db) == SQLITE_OK);
}
static void test_payload_high_compression(void) {
    // A genuine payload compresses close to LZ4's maximum ratio when its values repeat;
    // the size check must never reject what the library itself produced.
    sqlite3 *source = open_db(), *target = open_db();
    const char *schema = "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, value TEXT); SELECT cloudsync_init('t');";
    CHECK(sql(source, schema) == SQLITE_OK && sql(target, schema) == SQLITE_OK);
    CHECK(sql(source, "INSERT INTO t VALUES('big', replace(hex(zeroblob(4*1024*1024)), '0', 'A'))") == SQLITE_OK);
    CHECK(sql(source, "INSERT INTO t VALUES('small', 'x')") == SQLITE_OK);
    CHECK(apply_payload(source, target) == SQLITE_ROW);
    CHECK(scalar(target, "SELECT length(value) FROM t WHERE id='big'") == 8 * 1024 * 1024);
    CHECK(scalar(target, "SELECT count(*) FROM t") == 2);
    CHECK(close_db(source) == SQLITE_OK);
    CHECK(close_db(target) == SQLITE_OK);
}
static void fail_busy(sqlite3_context *context, int argc, sqlite3_value **argv) {
    (void)argc; (void)argv;
    sqlite3_result_error(context, "simulated busy", -1);
    sqlite3_result_error_code(context, SQLITE_BUSY);
}
static void test_resurrected_group_rollback(void) {
    const char *schema = "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, a TEXT, b TEXT); SELECT cloudsync_init('t');";

    // A resurrected row arrives as a sentinel plus its columns. When its write fails the
    // apply stops and nothing the group wrote remains: delivered again once the cause is
    // gone, the row is created.
    for (int transient = 0; transient < 2; transient++) {
        sqlite3 *source = open_db(), *target = open_db();
        CHECK(sql(source, schema) == SQLITE_OK && sql(target, schema) == SQLITE_OK);
        CHECK(sql(source, "INSERT INTO t VALUES('r1','x','y'); DELETE FROM t WHERE id='r1'; INSERT INTO t VALUES('r1','x2','y2');") == SQLITE_OK);
        CHECK(sqlite3_create_function(target, "fail_busy", 0, SQLITE_UTF8, NULL, fail_busy, NULL, NULL) == SQLITE_OK);
        CHECK(sql(target, transient
            ? "CREATE TRIGGER rej BEFORE INSERT ON t WHEN NEW.id='r1' BEGIN SELECT fail_busy(); END"
            : "CREATE TRIGGER rej BEFORE INSERT ON t WHEN NEW.id='r1' BEGIN SELECT RAISE(ABORT,'rejected r1'); END") == SQLITE_OK);
        CHECK(apply_payload(source, target) != SQLITE_ROW);
        CHECK(strstr(sqlite3_errmsg(target), transient ? "simulated busy" : "rejected r1") != NULL);
        CHECK(scalar(target, "SELECT count(*) FROM t_cloudsync") == 0);
        CHECK(sql(target, "DROP TRIGGER rej") == SQLITE_OK);
        CHECK(apply_payload(source, target) == SQLITE_ROW);
        CHECK(scalar(target, "SELECT count(*) FROM t WHERE id='r1' AND a='x2' AND b='y2'") == 1);
        CHECK(close_db(source) == SQLITE_OK);
        CHECK(close_db(target) == SQLITE_OK);
    }

    // A row the target already holds keeps its clocks when resurrecting it fails: the
    // zeroed clocks and the new sentinel are rolled back with the failed write.
    sqlite3 *source = open_db(), *target = open_db();
    CHECK(sql(source, schema) == SQLITE_OK && sql(target, schema) == SQLITE_OK);
    CHECK(sql(source, "INSERT INTO t VALUES('r1','x','y')") == SQLITE_OK);
    CHECK(apply_payload(source, target) == SQLITE_ROW);
    CHECK(sql(source, "DELETE FROM t WHERE id='r1'; INSERT INTO t VALUES('r1','x2','y2');") == SQLITE_OK);
    CHECK(sql(target, "CREATE TRIGGER rej BEFORE UPDATE ON t BEGIN SELECT RAISE(ABORT,'rejected update'); END;"
                      "CREATE TRIGGER rej2 BEFORE INSERT ON t BEGIN SELECT RAISE(ABORT,'rejected insert'); END;") == SQLITE_OK);
    CHECK(sql(target, "CREATE TEMP TABLE before_clocks AS SELECT col_name, col_version FROM t_cloudsync") == SQLITE_OK);
    CHECK(apply_payload(source, target) != SQLITE_ROW);   // the resurrection was attempted and failed
    CHECK(scalar(target, "SELECT count(*) FROM (SELECT col_name, col_version FROM t_cloudsync EXCEPT SELECT col_name, col_version FROM before_clocks)") == 0);
    CHECK(scalar(target, "SELECT count(*) FROM t WHERE id='r1' AND a='x' AND b='y'") == 1);
    CHECK(close_db(source) == SQLITE_OK);
    CHECK(close_db(target) == SQLITE_OK);
}
static void test_batched_update_missing_row(void) {
    // Metadata says the row exists but the base row is gone (deleted while sync was
    // disabled). A received multi-column update must write the row, not record winner
    // clocks for an UPDATE that changed nothing.
    const char *schema = "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, a TEXT, b TEXT); SELECT cloudsync_init('t');";
    sqlite3 *source = open_db(), *target = open_db();
    CHECK(sql(source, schema) == SQLITE_OK && sql(target, schema) == SQLITE_OK);
    CHECK(sql(source, "INSERT INTO t VALUES('r1','x','y')") == SQLITE_OK);
    CHECK(apply_payload(source, target) == SQLITE_ROW);
    CHECK(sql(target, "SELECT cloudsync_disable('t'); DELETE FROM t WHERE id='r1'; SELECT cloudsync_enable('t');") == SQLITE_OK);
    CHECK(sql(source, "UPDATE t SET a='x2', b='y2' WHERE id='r1'") == SQLITE_OK);
    CHECK(apply_payload(source, target) == SQLITE_ROW);
    CHECK(scalar(target, "SELECT count(*) FROM t WHERE id='r1' AND a='x2' AND b='y2'") == 1);
    CHECK(close_db(source) == SQLITE_OK);
    CHECK(close_db(target) == SQLITE_OK);
}
// The v3 fragments of one value too large for a chunk, read from a source database.
#define MAX_FRAGS 8
static int frag_count;
static void *frag_data[MAX_FRAGS];
static int frag_size[MAX_FRAGS];
static const char *frag_schema = "CREATE TABLE f(id TEXT PRIMARY KEY NOT NULL, v BLOB); SELECT cloudsync_init('f');";
static void frags_load(void) {
    sqlite3 *src = open_db();
    sqlite3_stmt *vm = NULL;
    CHECK(sql(src, frag_schema) == SQLITE_OK);
    CHECK(sql(src, "SELECT cloudsync_set('payload_max_chunk_size','262144'); INSERT INTO f VALUES('big', randomblob(700000));") == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(src, "SELECT payload FROM cloudsync_payload_chunks() WHERE substr(payload,5,1)=x'03' ORDER BY chunk_index", -1, &vm, NULL) == SQLITE_OK);
    while (frag_count < MAX_FRAGS && vm && sqlite3_step(vm) == SQLITE_ROW) {
        frag_size[frag_count] = sqlite3_column_bytes(vm, 0);
        frag_data[frag_count] = malloc((size_t)frag_size[frag_count]);
        memcpy(frag_data[frag_count], sqlite3_column_blob(vm, 0), (size_t)frag_size[frag_count]);
        frag_count++;
    }
    sqlite3_finalize(vm);
    CHECK(frag_count >= 3);
    CHECK(close_db(src) == SQLITE_OK);
}
static int frag_apply(sqlite3 *db, int i) {
    sqlite3_stmt *vm = NULL;
    CHECK(sqlite3_prepare_v2(db, "SELECT cloudsync_payload_apply(?1)", -1, &vm, NULL) == SQLITE_OK);
    sqlite3_bind_blob(vm, 1, frag_data[i], frag_size[i], SQLITE_STATIC);
    int rc = sqlite3_step(vm);
    sqlite3_finalize(vm);
    return rc;
}
static sqlite3 *frag_target(const char *path) {
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path ? path : ":memory:", &db) == SQLITE_OK);
    CHECK(sqlite3_cloudsync_init(db, NULL, NULL) == SQLITE_OK);
    CHECK(sql(db, "CREATE TABLE IF NOT EXISTS f(id TEXT PRIMARY KEY NOT NULL, v BLOB); SELECT cloudsync_init('f');") == SQLITE_OK);
    return db;
}
// an incomplete group whose only piece is two days old
static const char *stale_group = "INSERT INTO cloudsync_payload_fragments (value_id, part_index, part_count, total_size, checksum, created_at, tbl, pk, col_name, col_version, db_version, site_id, cl, seq, fragment) "
                                 "VALUES ('00000000000000000000000000000000', 0, 2, 2, '0000000000000000', strftime('%s','now') - 172800, 'f', x'00', 'v', 1, 1, x'00', 1, 0, x'00')";
static void test_fragment_retention(void) {
    frags_load();
    const char *big = "SELECT count(*) FROM f WHERE id='big' AND length(v)=700000";
    char path[512];
    CHECK(scratch_create());
    snprintf(path, sizeof(path), "%s/frags.db", scratch_dir);

    // A group resumed after two days keeps its old pieces: the cleanup runs after the
    // new piece is staged, when the group is recent again. A fully stale group goes.
    sqlite3 *db = frag_target(path);
    CHECK(frag_apply(db, 0) == SQLITE_ROW);
    CHECK(sql(db, "UPDATE cloudsync_payload_fragments SET created_at = created_at - 172800") == SQLITE_OK);
    CHECK(sql(db, stale_group) == SQLITE_OK);
    CHECK(close_db(db) == SQLITE_OK);
    db = frag_target(path);   // a new connection: its first fragment runs the cleanup
    CHECK(frag_apply(db, 1) == SQLITE_ROW);
    CHECK(scalar(db, "SELECT count(*) FROM cloudsync_payload_fragments WHERE value_id <> '00000000000000000000000000000000'") == 2);
    CHECK(scalar(db, "SELECT count(*) FROM cloudsync_payload_fragments WHERE value_id = '00000000000000000000000000000000'") == 0);
    for (int i = 2; i < frag_count; i++) CHECK(frag_apply(db, i) == SQLITE_ROW);
    CHECK(scalar(db, big) == 1);
    CHECK(close_db(db) == SQLITE_OK);
    const char *const files[] = {"frags.db", "frags.db-journal"};
    scratch_remove(files, 2);

    // A cleanup that fails is rolled back and logged; the value still applies.
    db = frag_target(NULL);
    CHECK(sql(db, stale_group) == SQLITE_OK);
    CHECK(sql(db, "CREATE TRIGGER no_cleanup BEFORE DELETE ON cloudsync_payload_fragments WHEN OLD.tbl='f' AND OLD.pk=x'00' BEGIN SELECT RAISE(ABORT,'cleanup denied'); END") == SQLITE_OK);
    for (int i = 0; i < frag_count; i++) CHECK(frag_apply(db, i) == SQLITE_ROW);
    CHECK(scalar(db, big) == 1);
    CHECK(scalar(db, "SELECT count(*) FROM cloudsync_payload_fragments") == 1);
    CHECK(close_db(db) == SQLITE_OK);

    // A value whose write fails keeps the pieces staged by earlier calls, and applies
    // once the last piece is delivered again.
    db = frag_target(NULL);
    CHECK(sql(db, "CREATE TRIGGER deny BEFORE INSERT ON f WHEN NEW.id='big' BEGIN SELECT RAISE(ABORT,'big denied'); END") == SQLITE_OK);
    for (int i = 0; i < frag_count - 1; i++) CHECK(frag_apply(db, i) == SQLITE_ROW);
    CHECK(frag_apply(db, frag_count - 1) != SQLITE_ROW);
    CHECK(strstr(sqlite3_errmsg(db), "big denied") != NULL);
    CHECK(scalar(db, "SELECT count(*) FROM cloudsync_payload_fragments") == frag_count - 1);
    CHECK(sql(db, "DROP TRIGGER deny") == SQLITE_OK);
    CHECK(frag_apply(db, frag_count - 1) == SQLITE_ROW);
    CHECK(scalar(db, big) == 1);
    CHECK(scalar(db, "SELECT count(*) FROM cloudsync_payload_fragments") == 0);
    CHECK(close_db(db) == SQLITE_OK);

    // Applying the value and removing its pieces is one unit: when the pieces cannot be
    // removed, the value is not applied either, and the call fails.
    db = frag_target(NULL);
    CHECK(sql(db, "CREATE TRIGGER keep BEFORE DELETE ON cloudsync_payload_fragments BEGIN SELECT RAISE(ABORT,'delete denied'); END") == SQLITE_OK);
    for (int i = 0; i < frag_count - 1; i++) CHECK(frag_apply(db, i) == SQLITE_ROW);
    CHECK(frag_apply(db, frag_count - 1) != SQLITE_ROW);
    CHECK(strstr(sqlite3_errmsg(db), "delete denied") != NULL);
    CHECK(scalar(db, "SELECT count(*) FROM f") == 0);
    CHECK(scalar(db, "SELECT count(*) FROM f_cloudsync") == 0);
    CHECK(scalar(db, "SELECT count(*) FROM cloudsync_payload_fragments") == frag_count - 1);
    CHECK(sql(db, "DROP TRIGGER keep") == SQLITE_OK);
    CHECK(frag_apply(db, frag_count - 1) == SQLITE_ROW);
    CHECK(scalar(db, big) == 1);
    CHECK(close_db(db) == SQLITE_OK);

    for (int i = 0; i < frag_count; i++) free(frag_data[i]);
}
static bool text_is(sqlite3 *db, const char *query, const char *expected) {
    sqlite3_stmt *vm = NULL;
    bool ok = sqlite3_prepare_v2(db, query, -1, &vm, NULL) == SQLITE_OK && sqlite3_step(vm) == SQLITE_ROW &&
              sqlite3_column_text(vm, 0) && strcmp((const char *)sqlite3_column_text(vm, 0), expected) == 0;
    if (!ok) fprintf(stderr, "  got: %s\n  expected: %s\n", vm && sqlite3_column_text(vm, 0) ? (const char *)sqlite3_column_text(vm, 0) : "(null)", expected);
    sqlite3_finalize(vm);
    return ok;
}
static void test_db_version_per_transaction(void) {
    // Every transaction, implicit or explicit, takes one db_version; its changes share it
    // with seq restarting at 0, and a rolled back transaction takes none. Same scenario
    // and expectations as test/postgresql/66_db_version_per_transaction.sql.
    const char *changes = "SELECT string_agg(cloudsync_pk_decode(pk,1) || ':' || col_name || '@' || db_version || '/' || seq, '  ' ORDER BY db_version, seq) FROM cloudsync_changes";
    sqlite3 *db = open_db();
    CHECK(sql(db, "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, v TEXT); SELECT cloudsync_init('t');") == SQLITE_OK);
    CHECK(sql(db, "INSERT INTO t VALUES ('a1','x')") == SQLITE_OK);
    CHECK(sql(db, "INSERT INTO t VALUES ('a2','x')") == SQLITE_OK);
    CHECK(sql(db, "INSERT INTO t VALUES ('b1','x'),('b2','x')") == SQLITE_OK);
    CHECK(text_is(db, changes, "a1:v@1/0  a2:v@2/0  b1:v@3/0  b2:v@3/1"));
    CHECK(sql(db, "BEGIN; INSERT INTO t VALUES ('c1','x');") == SQLITE_OK);
    CHECK(scalar(db, "SELECT cloudsync_db_version()") == 3);   // the last committed one
    CHECK(sql(db, "INSERT INTO t VALUES ('c2','x'); UPDATE t SET v='y' WHERE id='a1'; COMMIT;") == SQLITE_OK);
    CHECK(sql(db, "UPDATE t SET v='z' WHERE id='a2'") == SQLITE_OK);
    CHECK(sql(db, "BEGIN; UPDATE t SET v='y' WHERE id='b1'; UPDATE t SET v='y' WHERE id='b2'; COMMIT;") == SQLITE_OK);
    CHECK(sql(db, "BEGIN; INSERT INTO t VALUES ('r1','x'); ROLLBACK;") == SQLITE_OK);
    CHECK(sql(db, "INSERT INTO t VALUES ('d1','x')") == SQLITE_OK);
    CHECK(sql(db, "BEGIN; UPDATE t SET v='w' WHERE id='a1'; UPDATE t SET v='q' WHERE id='a1'; DELETE FROM t WHERE id='b2'; COMMIT;") == SQLITE_OK);
    CHECK(text_is(db, changes, "c1:v@4/0  c2:v@4/1  a2:v@5/0  b1:v@6/0  d1:v@7/0  a1:v@8/1  b2:__[RIP]__@8/2"));
    CHECK(scalar(db, "SELECT cloudsync_db_version()") == 8);
    CHECK(close_db(db) == SQLITE_OK);
}
static void test_db_version_apply_in_transaction(void) {
    // A payload applied inside the caller's transaction takes its own db_versions, apart
    // from the local writes before and after it, and a rollback gives them all back.
    const char *changes = "SELECT string_agg(cloudsync_pk_decode(pk,1) || '@' || db_version || '/' || seq, '  ' ORDER BY db_version, seq) FROM cloudsync_changes";
    const char *schema = "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, v TEXT); SELECT cloudsync_init('t');";
    sqlite3 *source = open_db();
    CHECK(sql(source, schema) == SQLITE_OK);
    CHECK(sql(source, "INSERT INTO t VALUES ('r1','x'),('r2','x')") == SQLITE_OK);
    CHECK(sql(source, "INSERT INTO t VALUES ('r3','x')") == SQLITE_OK);

    sqlite3 *db = open_db();
    CHECK(sql(db, schema) == SQLITE_OK);
    CHECK(sql(db, "BEGIN; INSERT INTO t VALUES ('l1','x');") == SQLITE_OK);
    CHECK(apply_payload(source, db) == SQLITE_ROW);
    CHECK(sql(db, "INSERT INTO t VALUES ('l2','x'); COMMIT;") == SQLITE_OK);
    CHECK(text_is(db, changes, "l1@1/0  r1@2/0  r2@2/1  r3@3/0  l2@4/0"));
    CHECK(close_db(db) == SQLITE_OK);

    db = open_db();
    CHECK(sql(db, schema) == SQLITE_OK);
    CHECK(sql(db, "BEGIN;") == SQLITE_OK);
    CHECK(apply_payload(source, db) == SQLITE_ROW);
    CHECK(sql(db, "ROLLBACK;") == SQLITE_OK);
    CHECK(scalar(db, "SELECT cloudsync_db_version()") == 0);
    CHECK(sql(db, "INSERT INTO t VALUES ('l1','x')") == SQLITE_OK);
    CHECK(text_is(db, changes, "l1@1/0"));
    CHECK(close_db(db) == SQLITE_OK);
    CHECK(close_db(source) == SQLITE_OK);
}
static void test_block_write_errors(void) {
    for (int update = 0; update < 2; update++) {
        sqlite3 *db = open_db();
        CHECK(sql(db, "CREATE TABLE docs(id TEXT PRIMARY KEY NOT NULL, body TEXT); SELECT cloudsync_init('docs'); SELECT cloudsync_set_column('docs','body','algo','block');") == SQLITE_OK);
        if (update) CHECK(sql(db, "INSERT INTO docs VALUES('1','old')") == SQLITE_OK);
        CHECK(sql(db, "CREATE TRIGGER deny_block BEFORE INSERT ON docs_cloudsync_blocks BEGIN SELECT RAISE(ABORT,'block write denied'); END") == SQLITE_OK);
        CHECK(sql(db, update ? "UPDATE docs SET body='new' WHERE id='1'" : "INSERT INTO docs VALUES('1','new')") != SQLITE_OK);
        // the failure names the column and table, keeps the database's own message, and
        // keeps its result code (RAISE(ABORT) is a constraint failure, not SQLITE_ERROR)
        CHECK(strstr(sqlite3_errmsg(db), "column \"body\" of table \"docs\"") != NULL);
        CHECK(strstr(sqlite3_errmsg(db), "block write denied") != NULL);
        CHECK((sqlite3_errcode(db) & 0xFF) == SQLITE_CONSTRAINT);
        CHECK(scalar(db, update ? "SELECT count(*) FROM docs WHERE body='old'" : "SELECT count(*) FROM docs") == update);
        CHECK(close_db(db) == SQLITE_OK);
    }
}
static void test_block_migration_orphan(void) {
    // Metadata can outlive its base row (deleted while sync was disabled). Converting
    // the column must skip that row, not fail, and still migrate the readable ones.
    sqlite3 *db = open_db();
    CHECK(sql(db, "CREATE TABLE docs(id TEXT PRIMARY KEY NOT NULL, body TEXT); SELECT cloudsync_init('docs');"
                  "INSERT INTO docs VALUES('a','hello world'),('b','x y');"
                  "SELECT cloudsync_disable('docs'); DELETE FROM docs WHERE id='b'; SELECT cloudsync_enable('docs');") == SQLITE_OK);
    CHECK(sql(db, "SELECT cloudsync_set_column('docs','body','algo','block')") == SQLITE_OK);
    CHECK(scalar(db, "SELECT count(*) FROM cloudsync_table_settings WHERE tbl_name='docs' AND key='algo' AND value='block'") == 1);
    CHECK(scalar(db, "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk=cloudsync_pk_encode('a')") == 1);
    CHECK(scalar(db, "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk=cloudsync_pk_encode('b')") == 0);
    CHECK(sql(db, "UPDATE docs SET body='hello world' || char(10) || 'again' WHERE id='a'") == SQLITE_OK);
    CHECK(scalar(db, "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk=cloudsync_pk_encode('a')") == 2);
    CHECK(close_db(db) == SQLITE_OK);
}
static void test_block_not_null_payload(void) {
    // A received block is materialized into a row whose other columns arrive in the same
    // payload; a constraint on one of them (here a trigger requiring an owner, as a NOT
    // NULL column or an RLS policy would) must not reject the block write.
    sqlite3 *source = open_db(), *target = open_db();
    const char *schema = "CREATE TABLE docs(id TEXT PRIMARY KEY NOT NULL, owner TEXT NOT NULL DEFAULT 'x', body TEXT);"
                         "SELECT cloudsync_init('docs'); SELECT cloudsync_set_column('docs','body','algo','block');";
    CHECK(sql(source, schema) == SQLITE_OK && sql(target, schema) == SQLITE_OK);
    CHECK(sql(target, "CREATE TRIGGER no_null_owner BEFORE INSERT ON docs WHEN NEW.owner = 'x' BEGIN SELECT RAISE(ABORT,'owner required'); END") == SQLITE_OK);
    CHECK(sql(source, "INSERT INTO docs VALUES('1','alice','line 1' || char(10) || 'line 2')") == SQLITE_OK);
    CHECK(apply_payload(source, target) == SQLITE_ROW);
    CHECK(scalar(target, "SELECT count(*) FROM docs WHERE id='1' AND owner='alice' AND body='line 1' || char(10) || 'line 2'") == 1);
    CHECK(close_db(source) == SQLITE_OK);
    CHECK(close_db(target) == SQLITE_OK);
}
static void test_refill_error(void) {
    sqlite3 *db = open_db();
    CHECK(sql(db, "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL,a TEXT,b TEXT); SELECT cloudsync_init('t'); INSERT INTO t VALUES('1','a','b'),('2','a','b'); DELETE FROM t_cloudsync WHERE col_name='a';") == SQLITE_OK);
    cloudsync_context *ctx = cloudsync_context_create(db);
    CHECK(ctx && cloudsync_context_init(ctx));
    CHECK(sql(db, "CREATE TRIGGER deny_meta BEFORE INSERT ON t_cloudsync WHEN NEW.col_name='a' BEGIN SELECT RAISE(ABORT,'metadata denied'); END") == SQLITE_OK);
    CHECK(cloudsync_refill_metatable(ctx, "t") != DBRES_OK);
    CHECK(scalar(db, "SELECT count(*) FROM t_cloudsync WHERE col_name='a'") == 0);
    cloudsync_context_free(ctx);
    CHECK(close_db(db) == SQLITE_OK);
}

static sqlite3_mem_methods memory;
static int fail_after = -1;
static bool fail_once = false;   // fail only the selected allocation, not every one after it
static bool fail_alloc(void) {
    if (fail_after < 0) return false;
    if (fail_after == 0) { if (fail_once) fail_after = -1; return true; }
    fail_after--;
    return false;
}
static void *fault_malloc(int size) { return fail_alloc() ? NULL : memory.xMalloc(size); }
static void *fault_realloc(void *ptr, int size) { return fail_alloc() ? NULL : memory.xRealloc(ptr, size); }
static void test_block_oom(void) {
    block_init_allocator();
    for (int kind = 0; kind < 5; kind++) {
        bool succeeded = false;
        for (int n = 0; n < 100 && !succeeded; n++) {
            sqlite3_int64 before = sqlite3_memory_used();
            fail_after = n;
            if (kind < 3) {
                block_list_t *list = block_split(kind == 0 ? "" : "a\nb", kind == 1 ? "" : "\n");
                fail_after = -1;
                if (list) {
                    CHECK(list->count == (kind == 2 ? 2 : 1));
                    block_list_free(list);
                    succeeded = true;
                }
            } else if (kind == 3) {
                block_entry_t old[] = {{.content="old", .position_id="a0"}, {.content="kept", .position_id="a1"}, {.content="removed", .position_id="a2"}};
                const char *parts[] = {"kept", "new"};
                block_diff_t *diff = block_diff(old, 3, parts, 2);
                fail_after = -1;
                if (diff) { CHECK(diff->count == 3); block_diff_free(diff); succeeded = true; }
            } else {
                block_list_t *list = block_list_create_empty();
                if (list) {
                    bool added = block_list_add(list, "content", "a0");
                    fail_after = -1;
                    CHECK(list->count == (added ? 1 : 0));
                    block_list_free(list);
                    succeeded = added;
                } else fail_after = -1;
            }
            CHECK(sqlite3_memory_used() == before);
        }
        CHECK(succeeded);
    }
}
static void test_block_materialize_errors(void) {
    sqlite3 *db = open_db();
    CHECK(sql(db, "CREATE TABLE docs(id TEXT PRIMARY KEY NOT NULL, body TEXT); SELECT cloudsync_init('docs');"
                  "SELECT cloudsync_set_column('docs','body','algo','block'); INSERT INTO docs VALUES('1','a' || char(10) || 'b');") == SQLITE_OK);

    // A failed write names the stage, column and table, keeps the cause and its code.
    CHECK(sql(db, "CREATE TRIGGER deny_body BEFORE UPDATE ON docs BEGIN SELECT RAISE(ABORT,'body rejected'); END") == SQLITE_OK);
    CHECK(sql(db, "SELECT cloudsync_text_materialize('docs','body','1')") != SQLITE_OK);
    CHECK(strstr(sqlite3_errmsg(db), "Unable to write the blocks of column \"body\" of table \"docs\"") != NULL);
    CHECK(strstr(sqlite3_errmsg(db), "body rejected") != NULL);
    CHECK((sqlite3_errcode(db) & 0xFF) == SQLITE_CONSTRAINT);
    CHECK(sql(db, "DROP TRIGGER deny_body") == SQLITE_OK);

    // Fail each allocation of the call in turn: the error is never blank, and cloudsync's
    // own allocation failures say so instead of posing as a read failure.
    int ours = 0, blank = 0;
    bool succeeded = false;
    for (int n = 0; n < 400 && !succeeded; n++) {
        fail_once = true;
        fail_after = n;
        int rc = sql(db, "SELECT cloudsync_text_materialize('docs','body','1')");
        fail_after = -1;
        fail_once = false;
        if (rc == SQLITE_OK) { succeeded = true; break; }
        const char *msg = sqlite3_errmsg(db);
        if (!msg || !msg[0] || strcmp(msg, "not an error") == 0) blank++;
        if (msg && strstr(msg, "Not enough memory to") && strstr(msg, "column \"body\" of table \"docs\"")) ours++;
    }
    CHECK(succeeded);
    CHECK(blank == 0);
    CHECK(ours > 0);
    CHECK(scalar(db, "SELECT body = 'a' || char(10) || 'b' FROM docs WHERE id='1'") == 1);
    CHECK(close_db(db) == SQLITE_OK);
}
static void test_failed_apply_commit(void) {
    const char *schema = "PRAGMA foreign_keys=ON; CREATE TABLE parent(id TEXT PRIMARY KEY);"
        "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, value TEXT REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED);"
        "SELECT cloudsync_init('t'); CREATE TABLE caller_work(value TEXT);";
    for (int iteration = 0; iteration < 100; iteration++) {
        sqlite3 *source = open_db(), *target = open_db();
        CHECK(sql(source, schema) == SQLITE_OK);
        CHECK(sql(target, schema) == SQLITE_OK);
        CHECK(sql(source, "INSERT INTO parent VALUES('valid'),('missing'); INSERT INTO t VALUES('a','valid');"
                          "INSERT INTO t VALUES('b','missing');") == SQLITE_OK);
        if (iteration % 2) CHECK(sql(source, "INSERT INTO t VALUES('c','valid');") == SQLITE_OK);
        CHECK(sql(target, "INSERT INTO parent VALUES('valid');") == SQLITE_OK);
        CHECK(apply_payload(source, target) == SQLITE_CONSTRAINT);
        CHECK(sqlite3_get_autocommit(target));
        CHECK(scalar(target, "SELECT count(*) FROM t WHERE id='b'") == 0);
        CHECK(scalar(target, "SELECT count(*) FROM t_cloudsync WHERE pk=cloudsync_pk_encode('b')") == 0);
        CHECK(scalar(target, "SELECT count(*) FROM t WHERE id='a'") == 1);
        CHECK(scalar(target, "SELECT coalesce((SELECT value FROM cloudsync_settings WHERE key='check_dbversion'),0)") == 0);
        CHECK(sql(target, "BEGIN; INSERT INTO caller_work VALUES('kept'); COMMIT;") == SQLITE_OK);
        CHECK(sql(target, "INSERT INTO parent VALUES('missing');") == SQLITE_OK);
        CHECK(apply_payload(source, target) == SQLITE_ROW);
        CHECK(scalar(target, "SELECT count(*) FROM t") == 2 + iteration % 2);
        CHECK(close_db(source) == SQLITE_OK);
        CHECK(close_db(target) == SQLITE_OK);
    }
    // A caller-owned transaction must survive a rejected group unchanged.
    sqlite3 *source = open_db(), *target = open_db();
    CHECK(sql(source, schema) == SQLITE_OK && sql(target, schema) == SQLITE_OK);
    CHECK(sql(source, "INSERT INTO parent VALUES('valid'); INSERT INTO t VALUES('a','valid');") == SQLITE_OK);
    CHECK(sql(target, "INSERT INTO parent VALUES('valid'); CREATE TRIGGER deny BEFORE INSERT ON t BEGIN SELECT RAISE(ABORT,'denied'); END;"
                      "BEGIN; INSERT INTO caller_work VALUES('kept'); SAVEPOINT caller_sp;") == SQLITE_OK);
    CHECK(apply_payload(source, target) == SQLITE_CONSTRAINT);
    CHECK(!sqlite3_get_autocommit(target));
    CHECK(scalar(target, "SELECT count(*) FROM caller_work") == 1);
    CHECK(sql(target, "ROLLBACK TO caller_sp; RELEASE caller_sp; DROP TRIGGER deny;") == SQLITE_OK);
    CHECK(apply_payload(source, target) == SQLITE_ROW);
    CHECK(sql(target, "COMMIT") == SQLITE_OK);
    CHECK(scalar(target, "SELECT count(*) FROM caller_work") == 1);
    CHECK(close_db(source) == SQLITE_OK && close_db(target) == SQLITE_OK);
    // A reader permits writes but prevents the outer RELEASE from committing.
    CHECK(scratch_create());
    char path[512];
    snprintf(path, sizeof(path), "%s/commit-busy.db", scratch_dir);
    source = open_db(); target = NULL;
    sqlite3 *reader = NULL;
    CHECK(sqlite3_open(path, &target) == SQLITE_OK);
    CHECK(sqlite3_cloudsync_init(target, NULL, NULL) == SQLITE_OK);
    CHECK(sql(source, schema) == SQLITE_OK && sql(target, schema) == SQLITE_OK);
    CHECK(sql(source, "INSERT INTO parent VALUES('valid'); INSERT INTO t VALUES('a','valid');") == SQLITE_OK);
    CHECK(sql(target, "INSERT INTO parent VALUES('valid');") == SQLITE_OK);
    CHECK(sqlite3_open(path, &reader) == SQLITE_OK);
    for (int i = 0; i < 30; i++) {
        CHECK(sql(reader, "BEGIN; SELECT * FROM t;") == SQLITE_OK);
        CHECK(apply_payload(source, target) == SQLITE_BUSY);
        CHECK(sqlite3_get_autocommit(target));
        CHECK(scalar(target, "SELECT count(*) FROM t") == 0);
        CHECK(sql(reader, "ROLLBACK") == SQLITE_OK);
    }
    CHECK(apply_payload(source, target) == SQLITE_ROW);
    CHECK(scalar(target, "SELECT count(*) FROM t") == 1);
    CHECK(sqlite3_close(reader) == SQLITE_OK);
    CHECK(close_db(source) == SQLITE_OK && close_db(target) == SQLITE_OK);
    const char *files[] = {"commit-busy.db", "commit-busy.db-journal"};
    scratch_remove(files, 2);
}
static void test_block_group_atomicity(void) {
    const char *schema = "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, owner TEXT, body TEXT);"
        "SELECT cloudsync_init('t'); SELECT cloudsync_set_column('t','body','algo','block');"
        "CREATE TABLE caller_work(value TEXT);";
    // Insert, update and resurrection; every block position; caller and internal
    // transactions. Repeat with varying payload sizes to exercise batch reuse.
    for (int trial = 0; trial < 120; trial++) {
        int mode = trial % 3, blocks = 3 + (trial / 3) % 8;
        int denied = (trial * 7) % blocks;
        bool caller = (trial / 24) % 2;
        sqlite3 *source = open_db(), *target = open_db();
        CHECK(sql(source, schema) == SQLITE_OK && sql(target, schema) == SQLITE_OK);
        if (mode) {
            CHECK(sql(source, "INSERT INTO t VALUES('a','old','old body');") == SQLITE_OK);
            CHECK(apply_payload(source, target) == SQLITE_ROW);
            if (mode == 2) {
                CHECK(sql(source, "DELETE FROM t;") == SQLITE_OK);
                CHECK(apply_payload(source, target) == SQLITE_ROW);
            }
        }
        char body[1024] = {0}, stmt[2048];
        for (int j = 0; j < blocks; j++) {
            char part[64];
            snprintf(part, sizeof(part), "%sblock-%d-trial-%d", j ? "\n" : "", j, trial);
            strcat(body, part);
        }
        snprintf(stmt, sizeof(stmt), "INSERT INTO t VALUES('a','new','%s') ON CONFLICT(id) DO UPDATE SET owner=excluded.owner,body=excluded.body;", body);
        CHECK(sql(source, stmt) == SQLITE_OK);
        CHECK(sql(target, "CREATE TEMP TABLE before_meta AS SELECT * FROM t_cloudsync;"
                          "CREATE TEMP TABLE before_blocks AS SELECT * FROM t_cloudsync_blocks;"
                          "CREATE TEMP TABLE before_data AS SELECT * FROM t;") == SQLITE_OK);
        int64_t checkpoint = scalar(target, "SELECT coalesce((SELECT value FROM cloudsync_settings WHERE key='check_dbversion'),0)");
        snprintf(stmt, sizeof(stmt), "CREATE TRIGGER deny_block BEFORE INSERT ON t_cloudsync_blocks "
                 "WHEN NEW.col_value='block-%d-trial-%d' BEGIN SELECT RAISE(ABORT,'block rejected'); END", denied, trial);
        CHECK(sql(target, stmt) == SQLITE_OK);
        if (caller) CHECK(sql(target, "BEGIN; INSERT INTO caller_work VALUES('kept'); SAVEPOINT caller_sp;") == SQLITE_OK);
        int rejected_rc = apply_payload(source, target);
        CHECK(rejected_rc != SQLITE_ROW);
        CHECK(strstr(sqlite3_errmsg(target), "block rejected") != NULL);
        CHECK(sqlite3_get_autocommit(target) == !caller);
        CHECK(scalar(target, "SELECT count(*) FROM (SELECT * FROM t EXCEPT SELECT * FROM before_data)") == 0);
        CHECK(scalar(target, "SELECT count(*) FROM (SELECT * FROM before_data EXCEPT SELECT * FROM t)") == 0);
        CHECK(scalar(target, "SELECT count(*) FROM (SELECT * FROM t_cloudsync EXCEPT SELECT * FROM before_meta)") == 0);
        CHECK(scalar(target, "SELECT count(*) FROM (SELECT * FROM before_meta EXCEPT SELECT * FROM t_cloudsync)") == 0);
        CHECK(scalar(target, "SELECT count(*) FROM (SELECT * FROM t_cloudsync_blocks EXCEPT SELECT * FROM before_blocks)") == 0);
        CHECK(scalar(target, "SELECT count(*) FROM (SELECT * FROM before_blocks EXCEPT SELECT * FROM t_cloudsync_blocks)") == 0);
        CHECK(scalar(target, "SELECT coalesce((SELECT value FROM cloudsync_settings WHERE key='check_dbversion'),0)") == checkpoint);
        if (caller) {
            CHECK(scalar(target, "SELECT count(*) FROM caller_work") == 1);
            CHECK(sql(target, "RELEASE caller_sp;") == SQLITE_OK);
        }
        CHECK(sql(target, "DROP TRIGGER deny_block;") == SQLITE_OK);
        CHECK(apply_payload(source, target) == SQLITE_ROW);
        snprintf(stmt, sizeof(stmt), "SELECT count(*) FROM t WHERE owner='new' AND body='%s'", body);
        CHECK(scalar(target, stmt) == 1);
        CHECK(apply_payload(source, target) == SQLITE_ROW);
        CHECK(scalar(target, stmt) == 1);
        if (caller) CHECK(sql(target, "COMMIT") == SQLITE_OK);
        CHECK(close_db(source) == SQLITE_OK && close_db(target) == SQLITE_OK);
    }
}

// A window cap must produce ordinary complete streams over smaller windows: repeated
// capped calls tile the stream exactly, every window ends on a db_version boundary, and
// the drain always advances -- a window that emitted nothing would never progress.
static void test_payload_window_cap(void) {
    sqlite3 *db = open_db();
    // The smallest chunk size the setting allows, so 60 rows of 20 KB span several
    // chunks and a budget has something to split.
    CHECK(sql(db, "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, v BLOB);"
                  "SELECT cloudsync_init('t');"
                  "SELECT cloudsync_set('payload_max_chunk_size','262144');") == SQLITE_OK);
    // Two shapes, both needed. Single-row transactions give chunk boundaries that
    // coincide with db_version boundaries, which is where a window may end and is the
    // common production shape. The two 15-row transactions are ~300 KB each, larger
    // than a chunk, so a chunk boundary also falls *inside* a db_version -- that is
    // what exercises the rule that a window must not end there. Capping mid-version
    // would leave the remainder unsent, because the next window starts past it.
    int row = 0;
    for (int i = 0; i < 40; i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "INSERT INTO t VALUES('r%03d', randomblob(20000));", row++);
        CHECK(sql(db, buf) == SQLITE_OK);
    }
    for (int txn = 0; txn < 2; txn++) {
        CHECK(sql(db, "BEGIN;") == SQLITE_OK);
        for (int i = 0; i < 15; i++) {
            char buf[128];
            snprintf(buf, sizeof(buf), "INSERT INTO t VALUES('r%03d', randomblob(20000));", row++);
            CHECK(sql(db, buf) == SQLITE_OK);
        }
        CHECK(sql(db, "COMMIT;") == SQLITE_OK);
    }
    // A third shape, and the one that matters most: ~13 rows fill a 256 KB chunk, so
    // transactions of 12 then 13 rows keep every chunk boundary one row inside a
    // db_version. Waiting for a chunk to end on a boundary never succeeds here, so a
    // cap that only checks after a chunk is built never fires and the whole history
    // comes out in a single window.
    for (int txn = 0; txn < 20; txn++) {
        CHECK(sql(db, "BEGIN;") == SQLITE_OK);
        for (int i = 0; i < (txn == 0 ? 12 : 13); i++) {
            char buf[128];
            snprintf(buf, sizeof(buf), "INSERT INTO t VALUES('r%03d', randomblob(20000));", row++);
            CHECK(sql(db, buf) == SQLITE_OK);
        }
        CHECK(sql(db, "COMMIT;") == SQLITE_OK);
    }

    // Baseline: the whole window in one uncapped scan.
    sqlite3_stmt *vm = NULL;
    int64_t total_bytes = 0, total_rows = 0, watermark = 0;
    CHECK(sqlite3_prepare_v2(db, "SELECT sum(rows), sum(payload_size), "
                                 "max(CASE WHEN is_final THEN watermark_db_version END), "
                                 "max(window_capped) FROM cloudsync_payload_chunks(0,NULL,NULL,false)",
                             -1, &vm, NULL) == SQLITE_OK);
    if (sqlite3_step(vm) == SQLITE_ROW) {
        total_rows = sqlite3_column_int64(vm, 0);
        total_bytes = sqlite3_column_int64(vm, 1);
        watermark = sqlite3_column_int64(vm, 2);
        CHECK(sqlite3_column_int(vm, 3) == 0);   // nothing is capped without a budget
    }
    sqlite3_finalize(vm);
    CHECK(total_rows > 0 && total_bytes > 0 && watermark == 62);

    // Two budgets: one that spans several chunks, and one below a single chunk so the
    // "always emit one whole db_version" guarantee is what keeps the drain moving.
    const int64_t budgets[] = {200000, 1};
    for (size_t b = 0; b < sizeof(budgets) / sizeof(budgets[0]); ++b) {
        int64_t since = 0, sum_rows = 0;
        int windows = 0;
        bool capped = true;
        while (capped && windows < 100) {
            char q[512];
            snprintf(q, sizeof(q),
                     "SELECT count(*), coalesce(sum(rows),0), coalesce(sum(payload_size),0), "
                     "max(CASE WHEN is_final THEN watermark_db_version END), "
                     "max(window_capped), min(db_version_min) "
                     "FROM cloudsync_payload_chunks(%lld,NULL,NULL,false,NULL,NULL,NULL,%lld)",
                     (long long)since, (long long)budgets[b]);
            vm = NULL;
            CHECK(sqlite3_prepare_v2(db, q, -1, &vm, NULL) == SQLITE_OK);
            if (sqlite3_step(vm) != SQLITE_ROW || sqlite3_column_int64(vm, 0) == 0) { sqlite3_finalize(vm); break; }
            int64_t nrows = sqlite3_column_int64(vm, 1);
            int64_t bytes = sqlite3_column_int64(vm, 2);
            // A window ends at the first db_version boundary at or after the budget, so
            // it can overshoot by at most the chunk that crossed it plus the version in
            // progress -- never by the whole history.
            CHECK(bytes <= budgets[b] + 3 * 262144);
            int64_t wm = sqlite3_column_int64(vm, 3);
            capped = sqlite3_column_int(vm, 4) != 0;
            int64_t first = sqlite3_column_int64(vm, 5);
            sqlite3_finalize(vm);

            CHECK(first == since + 1);     // windows abut, no gap and no overlap
            CHECK(wm > since);             // always advances, so the drain terminates
            sum_rows += nrows;
            since = wm;
            windows++;
        }
        CHECK(windows > 1);                // the budget really did split the stream
        CHECK(since == watermark);         // and the last window reached the end
        // Rows are the invariant, not bytes or chunks: ending a chunk early at a
        // db_version boundary repacks the same rows into more chunks, each carrying
        // its own header, so a capped drain legitimately moves a few more bytes.
        CHECK(sum_rows == total_rows);
    }

    CHECK(close_db(db) == SQLITE_OK);

    // A history of oversized values is emitted entirely as fragment chunks, which the
    // ordinary chunk builder never produces. Those bytes still have to spend the budget,
    // or such a history never reaches the cap at all.
    db = open_db();
    CHECK(sql(db, "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, v BLOB);"
                  "SELECT cloudsync_init('t');"
                  "SELECT cloudsync_set('payload_max_chunk_size','262144');") == SQLITE_OK);
    for (int i = 0; i < 20; i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "INSERT INTO t VALUES('f%03d', randomblob(300000));", i);
        CHECK(sql(db, buf) == SQLITE_OK);
    }
    int64_t frag_rows = 0, frag_wm = 0;
    vm = NULL;
    CHECK(sqlite3_prepare_v2(db, "SELECT sum(rows), max(CASE WHEN is_final THEN watermark_db_version END) "
                                 "FROM cloudsync_payload_chunks(0,NULL,NULL,false)", -1, &vm, NULL) == SQLITE_OK);
    if (sqlite3_step(vm) == SQLITE_ROW) { frag_rows = sqlite3_column_int64(vm, 0); frag_wm = sqlite3_column_int64(vm, 1); }
    sqlite3_finalize(vm);
    CHECK(frag_rows > 0 && frag_wm == 20);

    int64_t since = 0, seen = 0;
    int windows = 0;
    bool capped = true;
    while (capped && windows < 100) {
        vm = NULL;
        char q[384];
        snprintf(q, sizeof(q),
                 "SELECT count(*), coalesce(sum(rows),0), "
                 "max(CASE WHEN is_final THEN watermark_db_version END), max(window_capped), "
                 "min(db_version_min) "
                 "FROM cloudsync_payload_chunks(%lld,NULL,NULL,false,NULL,NULL,NULL,200000)",
                 (long long)since);
        CHECK(sqlite3_prepare_v2(db, q, -1, &vm, NULL) == SQLITE_OK);
        if (sqlite3_step(vm) != SQLITE_ROW || sqlite3_column_int64(vm, 0) == 0) { sqlite3_finalize(vm); break; }
        seen += sqlite3_column_int64(vm, 1);
        int64_t wm = sqlite3_column_int64(vm, 2);
        capped = sqlite3_column_int(vm, 3) != 0;
        CHECK(sqlite3_column_int64(vm, 4) == since + 1);
        CHECK(wm > since);
        sqlite3_finalize(vm);
        since = wm;
        windows++;
    }
    CHECK(windows > 1);            // the budget split a purely fragmented history
    CHECK(since == frag_wm);
    CHECK(seen == frag_rows);
    CHECK(close_db(db) == SQLITE_OK);
}

int main(void) {
    CHECK(sqlite3_config(SQLITE_CONFIG_GETMALLOC, &memory) == SQLITE_OK);
    sqlite3_mem_methods faults = memory;
    faults.xMalloc = fault_malloc;
    faults.xRealloc = fault_realloc;
    CHECK(sqlite3_config(SQLITE_CONFIG_MALLOC, &faults) == SQLITE_OK);
    CHECK(sqlite3_initialize() == SQLITE_OK);
    test_clocks_and_double();
    test_best_index();
    test_payload_errors();
    test_failed_apply_commit();
    test_payload_high_compression();
    test_resurrected_group_rollback();
    test_batched_update_missing_row();
    test_fragment_retention();
    test_db_version_per_transaction();
    test_db_version_apply_in_transaction();
    test_block_write_errors();
    test_block_materialize_errors();
    test_block_migration_orphan();
    test_block_not_null_payload();
    test_block_group_atomicity();
    test_payload_window_cap();
    test_refill_error();
    test_block_oom();
    cloudsync_memory_finalize();
    CHECK(sqlite3_memory_used() == 0);
    printf("Audit regressions: %d failures\n", failures);
    return failures ? 1 : 0;
}
