// Focused audit regressions. No server or on-disk database required.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
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
static void test_payload_errors(void) {
    for (int denied = 1; denied <= 3; denied++) {
        sqlite3 *source = open_db(), *target = open_db();
        const char *schema = "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL,value TEXT); SELECT cloudsync_init('t');";
        CHECK(sql(source, schema) == SQLITE_OK && sql(target, schema) == SQLITE_OK);
        CHECK(sql(source, "INSERT INTO t VALUES('1','a'),('2','b'),('3','c');") == SQLITE_OK);
        char trigger[256];
        snprintf(trigger, sizeof(trigger), "CREATE TRIGGER deny BEFORE INSERT ON t WHEN NEW.id='%d' BEGIN SELECT RAISE(ABORT,'denied'); END", denied);
        CHECK(sql(target, trigger) == SQLITE_OK);
        sqlite3_stmt *read = NULL, *write = NULL;
        CHECK(sqlite3_prepare_v2(source, "SELECT cloudsync_payload_encode(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq) FROM cloudsync_changes", -1, &read, NULL) == SQLITE_OK);
        CHECK(sqlite3_step(read) == SQLITE_ROW);
        CHECK(sqlite3_prepare_v2(target, "SELECT cloudsync_payload_decode(?1)", -1, &write, NULL) == SQLITE_OK);
        CHECK(sqlite3_bind_value(write, 1, sqlite3_column_value(read, 0)) == SQLITE_OK);
        CHECK(sqlite3_step(write) != SQLITE_ROW);
        CHECK(strstr(sqlite3_errmsg(target), "denied") != NULL);
        sqlite3_finalize(write);
        sqlite3_finalize(read);
        CHECK(scalar(target, "SELECT coalesce((SELECT value FROM cloudsync_settings WHERE key='check_dbversion'),0)") == 0);
        CHECK(sqlite3_get_autocommit(target));
        CHECK(close_db(source) == SQLITE_OK);
        CHECK(close_db(target) == SQLITE_OK);
    }
    sqlite3 *db = open_db();
    CHECK(sql(db, "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL); SELECT cloudsync_init('t');") == SQLITE_OK);
    // v1 header: request a 4GB decompression without checksum/schema requirements.
    CHECK(sql(db, "SELECT cloudsync_payload_decode(x'434C535901000000FFFFFFFF00090000000000000000000000000000000000000000')") != SQLITE_OK);
    CHECK(strstr(sqlite3_errmsg(db), "exceeds limit") != NULL);
    CHECK(sql(db, "SELECT cloudsync_payload_decode(x'434C5359010000001000000100090000000000000000000000000000000000000000')") != SQLITE_OK);
    CHECK(strstr(sqlite3_errmsg(db), "exceeds limit") != NULL);
    CHECK(close_db(db) == SQLITE_OK);
}
static void test_block_write_errors(void) {
    for (int update = 0; update < 2; update++) {
        sqlite3 *db = open_db();
        CHECK(sql(db, "CREATE TABLE docs(id TEXT PRIMARY KEY NOT NULL, body TEXT); SELECT cloudsync_init('docs'); SELECT cloudsync_set_column('docs','body','algo','block');") == SQLITE_OK);
        if (update) CHECK(sql(db, "INSERT INTO docs VALUES('1','old')") == SQLITE_OK);
        CHECK(sql(db, "CREATE TRIGGER deny_block BEFORE INSERT ON docs_cloudsync_blocks BEGIN SELECT RAISE(ABORT,'block write denied'); END") == SQLITE_OK);
        CHECK(sql(db, update ? "UPDATE docs SET body='new' WHERE id='1'" : "INSERT INTO docs VALUES('1','new')") != SQLITE_OK);
        CHECK(scalar(db, update ? "SELECT count(*) FROM docs WHERE body='old'" : "SELECT count(*) FROM docs") == update);
        CHECK(close_db(db) == SQLITE_OK);
    }
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
static bool fail_alloc(void) {
    if (fail_after < 0) return false;
    if (fail_after == 0) return true;
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
    test_block_write_errors();
    test_refill_error();
    test_block_oom();
    cloudsync_memory_finalize();
    CHECK(sqlite3_memory_used() == 0);
    printf("Audit regressions: %d failures\n", failures);
    return failures ? 1 : 0;
}
