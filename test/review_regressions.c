// Focused audit regressions. No server or on-disk database required.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
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
static int skipped_warnings;
static int skipped_changes;   // sum of N over "skipped N received change(s) that failed to apply"
static void log_callback(void *arg, int code, const char *message) {
    (void)arg;
    if (code == SQLITE_WARNING && message && strstr(message, "failed to apply")) {
        skipped_warnings++;
        const char *n = strstr(message, "skipped ");
        if (n) skipped_changes += atoi(n + 8);
    }
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
    // A write that fails on its data fails the same way on every retry: it is skipped
    // and reported, and the cursor still advances past it.
    for (int denied = 1; denied <= 3; denied++) {
        sqlite3 *source = open_db(), *target = open_db();
        const char *schema = "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL,value TEXT); SELECT cloudsync_init('t');";
        CHECK(sql(source, schema) == SQLITE_OK && sql(target, schema) == SQLITE_OK);
        CHECK(sql(source, "INSERT INTO t VALUES('1','a'),('2','b'),('3','c');") == SQLITE_OK);
        char trigger[256];
        snprintf(trigger, sizeof(trigger), "CREATE TRIGGER deny BEFORE INSERT ON t WHEN NEW.id='%d' BEGIN SELECT RAISE(ABORT,'denied'); END", denied);
        CHECK(sql(target, trigger) == SQLITE_OK);
        skipped_warnings = 0;
        CHECK(apply_payload(source, target) == SQLITE_ROW);
        CHECK(skipped_warnings == 1);
        CHECK(scalar(target, "SELECT count(*) FROM t") == 2);
        char query[128];
        snprintf(query, sizeof(query), "SELECT count(*) FROM t WHERE id='%d'", denied);
        CHECK(scalar(target, query) == 0);
        CHECK(scalar(target, "SELECT coalesce((SELECT value FROM cloudsync_settings WHERE key='check_dbversion'),0)") > 0);
        CHECK(sqlite3_get_autocommit(target));
        CHECK(close_db(source) == SQLITE_OK);
        CHECK(close_db(target) == SQLITE_OK);
    }

    // A transient failure (the database is locked by another connection) could succeed
    // on a retry, so it must fail the apply and leave the cursor in place.
    {
        char path[512];
        const char *dir = getenv("TMPDIR");
        unsigned int nonce = 0;
        sqlite3_randomness(sizeof(nonce), &nonce);
        snprintf(path, sizeof(path), "%s/cloudsync-rr-busy-%08x.db", (dir && *dir) ? dir : ".", nonce);
        sqlite3 *source = open_db(), *target = NULL, *locker = NULL;
        CHECK(sqlite3_open(path, &target) == SQLITE_OK);
        CHECK(sqlite3_cloudsync_init(target, NULL, NULL) == SQLITE_OK);
        const char *schema = "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL,value TEXT); SELECT cloudsync_init('t');";
        CHECK(sql(source, schema) == SQLITE_OK && sql(target, schema) == SQLITE_OK);
        CHECK(sql(source, "INSERT INTO t VALUES('1','a'),('2','b');") == SQLITE_OK);
        CHECK(sqlite3_open(path, &locker) == SQLITE_OK);
        CHECK(sql(locker, "BEGIN IMMEDIATE") == SQLITE_OK);
        skipped_warnings = 0;
        CHECK(apply_payload(source, target) != SQLITE_ROW);
        CHECK(skipped_warnings == 0);
        CHECK(sql(locker, "ROLLBACK") == SQLITE_OK);
        CHECK(sqlite3_close(locker) == SQLITE_OK);
        CHECK(scalar(target, "SELECT coalesce((SELECT value FROM cloudsync_settings WHERE key='check_dbversion'),0)") == 0);
        CHECK(sqlite3_get_autocommit(target));
        // Once the lock is gone the same payload applies in full.
        CHECK(apply_payload(source, target) == SQLITE_ROW);
        CHECK(scalar(target, "SELECT count(*) FROM t") == 2);
        CHECK(close_db(source) == SQLITE_OK);
        CHECK(close_db(target) == SQLITE_OK);
        char aux[600];
        remove(path);
        snprintf(aux, sizeof(aux), "%s-journal", path);
        remove(aux);
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
    // whole group is skipped and counted (3 changes, not 2), and nothing it wrote remains:
    // delivered again once the cause is gone, the row is created.
    for (int transient = 0; transient < 2; transient++) {
        sqlite3 *source = open_db(), *target = open_db();
        CHECK(sql(source, schema) == SQLITE_OK && sql(target, schema) == SQLITE_OK);
        CHECK(sql(source, "INSERT INTO t VALUES('r1','x','y'); DELETE FROM t WHERE id='r1'; INSERT INTO t VALUES('r1','x2','y2');") == SQLITE_OK);
        CHECK(sqlite3_create_function(target, "fail_busy", 0, SQLITE_UTF8, NULL, fail_busy, NULL, NULL) == SQLITE_OK);
        CHECK(sql(target, transient
            ? "CREATE TRIGGER rej BEFORE INSERT ON t WHEN NEW.id='r1' BEGIN SELECT fail_busy(); END"
            : "CREATE TRIGGER rej BEFORE INSERT ON t WHEN NEW.id='r1' BEGIN SELECT RAISE(ABORT,'rejected r1'); END") == SQLITE_OK);
        skipped_warnings = 0; skipped_changes = 0;
        int rc = apply_payload(source, target);
        if (transient) {
            CHECK(rc != SQLITE_ROW);               // transient: the apply fails and is retried
            CHECK(skipped_warnings == 0);
        } else {
            CHECK(rc == SQLITE_ROW);               // data failure: skipped and reported
            CHECK(skipped_warnings == 1);
            CHECK(skipped_changes == 3);
        }
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
    skipped_warnings = 0;
    CHECK(apply_payload(source, target) == SQLITE_ROW);
    CHECK(skipped_warnings >= 1);   // the resurrection was attempted and failed
    CHECK(scalar(target, "SELECT count(*) FROM (SELECT col_name, col_version FROM t_cloudsync EXCEPT SELECT col_name, col_version FROM before_clocks)") == 0);
    CHECK(scalar(target, "SELECT count(*) FROM t WHERE id='r1' AND a='x' AND b='y'") == 1);
    CHECK(close_db(source) == SQLITE_OK);
    CHECK(close_db(target) == SQLITE_OK);
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
    skipped_warnings = 0;
    CHECK(apply_payload(source, target) == SQLITE_ROW);
    CHECK(skipped_warnings == 0);
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
int main(void) {
    CHECK(sqlite3_config(SQLITE_CONFIG_GETMALLOC, &memory) == SQLITE_OK);
    sqlite3_mem_methods faults = memory;
    faults.xMalloc = fault_malloc;
    faults.xRealloc = fault_realloc;
    CHECK(sqlite3_config(SQLITE_CONFIG_MALLOC, &faults) == SQLITE_OK);
    CHECK(sqlite3_config(SQLITE_CONFIG_LOG, log_callback, NULL) == SQLITE_OK);
    CHECK(sqlite3_initialize() == SQLITE_OK);
    test_clocks_and_double();
    test_best_index();
    test_payload_errors();
    test_payload_high_compression();
    test_resurrected_group_rollback();
    test_block_write_errors();
    test_block_materialize_errors();
    test_block_migration_orphan();
    test_block_not_null_payload();
    test_refill_error();
    test_block_oom();
    cloudsync_memory_finalize();
    CHECK(sqlite3_memory_used() == 0);
    printf("Audit regressions: %d failures\n", failures);
    return failures ? 1 : 0;
}
