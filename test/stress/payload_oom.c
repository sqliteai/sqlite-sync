// Diagnostic stress test. A nonzero exit reports ANY remaining cleanup failure.
// Explicit recovery lets the sweep continue without hiding open transactions.
#define main regression_main
#include "../review_regressions.c"
#undef main

int main(int argc, char **argv) {
    CHECK(sqlite3_config(SQLITE_CONFIG_GETMALLOC, &memory) == SQLITE_OK);
    sqlite3_mem_methods faults = memory;
    faults.xMalloc = fault_malloc;
    faults.xRealloc = fault_realloc;
    CHECK(sqlite3_config(SQLITE_CONFIG_MALLOC, &faults) == SQLITE_OK);
    CHECK(sqlite3_initialize() == SQLITE_OK);
    sqlite3 *source = open_db();
    const char *schema = "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, value TEXT); SELECT cloudsync_init('t');";
    CHECK(sql(source, schema) == SQLITE_OK);
    CHECK(sql(source, "INSERT INTO t VALUES('a','one'),('b','two'),('c','three')") == SQLITE_OK);
    sqlite3_stmt *read = NULL;
    CHECK(sqlite3_prepare_v2(source, "SELECT cloudsync_payload_encode(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq) FROM cloudsync_changes", -1, &read, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(read) == SQLITE_ROW);
    int start = argc > 1 ? atoi(argv[1]) : 0;
    int end = argc > 2 ? atoi(argv[2]) : 1500;
    int open_transactions = 0, leaks = 0, retries_failed = 0;
    bool exhausted = false;
    for (int n = start; n < end; n++) {
        sqlite3_int64 before = sqlite3_memory_used();
        sqlite3 *target = open_db();
        CHECK(sql(target, schema) == SQLITE_OK);
        sqlite3_stmt *write = NULL;
        CHECK(sqlite3_prepare_v2(target, "SELECT cloudsync_payload_decode(?1)", -1, &write, NULL) == SQLITE_OK);
        CHECK(sqlite3_bind_value(write, 1, sqlite3_column_value(read, 0)) == SQLITE_OK);
        fail_once = true;
        fail_after = n;
        int rc = sqlite3_step(write);
        int left = fail_after;
        fail_after = -1;
        fail_once = false;
        sqlite3_finalize(write);
        if (!sqlite3_get_autocommit(target)) {
            open_transactions++;
            fprintf(stderr, "n=%d rc=%d TRANSACTION LEFT OPEN\n", n, rc);
            CHECK(sql(target, "ROLLBACK") == SQLITE_OK);
        }
        if (rc != SQLITE_ROW) {
            int retry_rc = apply_payload(source, target);
            if (retry_rc != SQLITE_ROW || scalar(target, "SELECT count(*) FROM t") != 3) {
                retries_failed++;
                fprintf(stderr, "n=%d rc=%d RETRY FAILED rc=%d %s\n", n, rc, retry_rc, sqlite3_errmsg(target));
            }
        }
        CHECK(close_db(target) == SQLITE_OK);
        sqlite3_int64 leak = sqlite3_memory_used() - before;
        if (leak) {
            leaks++;
            fprintf(stderr, "n=%d rc=%d LEAK=%lld\n", n, rc, leak);
        }
        if (left > 0) {
            exhausted = true;
            printf("All allocations exhausted at n=%d\n", n);
            break;
        }
    }
    sqlite3_finalize(read);
    CHECK(close_db(source) == SQLITE_OK);
    printf("open_transactions=%d leaks=%d retries_failed=%d final_memory=%lld\n",
           open_transactions, leaks, retries_failed, sqlite3_memory_used());
    return failures || !exhausted || open_transactions || leaks || retries_failed || sqlite3_memory_used() ? 1 : 0;
}
