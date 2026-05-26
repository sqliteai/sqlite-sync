//
//  sync_bench.c
//  cloudsync
//
//  Measures end-to-end sync latency from one local SQLite database to another.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#ifdef CLOUDSYNC_NETWORK_TRACE
#include <stdarg.h>
#endif
#include "sqlite3.h"
#include "utils.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define DB_A_PATH       "dist/sync-bench-a.sqlite"
#define DB_B_PATH       "dist/sync-bench-b.sqlite"
#define EXT_PATH        "./dist/cloudsync"
#define DEFAULT_POLL_DELAY_MS 250
#define DEFAULT_MAX_POLLS 40
#define DEFAULT_RANDOM_BLOB_SIZE_BYTES (100 * 1024)
#define DEFAULT_CLEANUP_OLDER_THAN_SECONDS (24 * 60 * 60)

typedef struct {
    const char *operation;
    int attempt;
    int sqlite_rc;
    int rows_received;
    double started_ms;
    double ended_ms;
    double elapsed_ms;
    char *result_json;
} sync_bench_request;

typedef struct {
    int local_version;
    int server_version;
    char *status;
} sync_bench_send_summary;

#ifdef CLOUDSYNC_NETWORK_TRACE
static void bench_trace(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[sync-bench] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}
#else
#define bench_trace(...) ((void)0)
#endif

static double monotonic_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return ((double)counter.QuadPart * 1000.0) / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((double)ts.tv_sec * 1000.0) + ((double)ts.tv_nsec / 1000000.0);
#endif
}

static char *str_dup(const char *value) {
    if (!value) value = "";
    size_t len = strlen(value);
    char *copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, value, len + 1);
    return copy;
}

static int env_int(const char *name, int default_value) {
    const char *value = getenv(name);
    if (!value || !*value) return default_value;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (!end || *end != '\0' || parsed < 0 || parsed > 1000000) return default_value;
    return (int)parsed;
}

static int db_exec(sqlite3 *db, const char *sql) {
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error while executing %s: %s\n", sql, errmsg ? errmsg : sqlite3_errmsg(db));
        sqlite3_free(errmsg);
    }
    return rc;
}

static int query_text(sqlite3 *db, const char *sql, char **out) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error while preparing %s: %s\n", sql, sqlite3_errmsg(db));
        return rc;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char *value = sqlite3_column_text(stmt, 0);
        *out = str_dup((const char *)value);
        if (!*out) rc = SQLITE_NOMEM;
        else rc = SQLITE_OK;
    } else if (rc == SQLITE_DONE) {
        *out = NULL;
        rc = SQLITE_OK;
    } else {
        fprintf(stderr, "Error while stepping %s: %s\n", sql, sqlite3_errmsg(db));
    }

    int finalize_rc = sqlite3_finalize(stmt);
    if (rc == SQLITE_OK && finalize_rc != SQLITE_OK) rc = finalize_rc;
    return rc;
}

static int timed_query_text(sqlite3 *db, const char *sql, char **out, double *started_ms, double *ended_ms) {
    *started_ms = monotonic_ms();
    int rc = query_text(db, sql, out);
    *ended_ms = monotonic_ms();
    return rc;
}

static int open_load_ext(const char *db_path, sqlite3 **out_db) {
    bench_trace("step=open-load-extension db_path=%s begin", db_path);
    sqlite3 *db = NULL;
    int rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Unable to open %s: %s\n", db_path, db ? sqlite3_errmsg(db) : "unknown error");
        if (db) sqlite3_close(db);
        bench_trace("step=open-load-extension db_path=%s end rc=%d", db_path, rc);
        return rc;
    }

    rc = sqlite3_enable_load_extension(db, 1);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Unable to enable load_extension for %s: %s\n", db_path, sqlite3_errmsg(db));
        sqlite3_close(db);
        bench_trace("step=open-load-extension db_path=%s end rc=%d", db_path, rc);
        return rc;
    }

    rc = db_exec(db, "SELECT load_extension('" EXT_PATH "');");
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        bench_trace("step=open-load-extension db_path=%s end rc=%d", db_path, rc);
        return rc;
    }

    *out_db = db;
    bench_trace("step=open-load-extension db_path=%s end rc=%d", db_path, SQLITE_OK);
    return SQLITE_OK;
}

static int init_schema(sqlite3 *db, const char *label) {
    bench_trace("step=init-schema db=%s begin", label);
    int rc = db_exec(db,
        "CREATE TABLE IF NOT EXISTS sync_bench_items ("
        "id TEXT PRIMARY KEY NOT NULL,"
        "payload TEXT NOT NULL DEFAULT '',"
        "marker TEXT NOT NULL DEFAULT '',"
        "random_blob BLOB NOT NULL DEFAULT X'',"
        "updated_at TEXT NOT NULL DEFAULT ''"
        ");");
    if (rc != SQLITE_OK) {
        bench_trace("step=init-schema db=%s end rc=%d", label, rc);
        return rc;
    }

    rc = db_exec(db, "SELECT cloudsync_init('sync_bench_items');");
    bench_trace("step=init-schema db=%s end rc=%d", label, rc);
    return rc;
}

static int init_network(sqlite3 *db, const char *label, const char *database_id, const char *address, const char *apikey) {
    char sql[2048];
    if (address && *address) {
        snprintf(sql, sizeof(sql), "SELECT cloudsync_network_init_custom('%s', '%s');", address, database_id);
    } else {
        snprintf(sql, sizeof(sql), "SELECT cloudsync_network_init('%s');", database_id);
    }
    bench_trace("step=network-init db=%s begin mode=%s", label, (address && *address) ? "custom-address" : "default-address");
    int rc = db_exec(db, sql);
    bench_trace("step=network-init db=%s end rc=%d", label, rc);
    if (rc != SQLITE_OK) return rc;

    if (apikey && *apikey) {
        bench_trace("step=set-apikey db=%s begin", label);
        snprintf(sql, sizeof(sql), "SELECT cloudsync_network_set_apikey('%s');", apikey);
        rc = db_exec(db, sql);
        bench_trace("step=set-apikey db=%s end rc=%d", label, rc);
        if (rc != SQLITE_OK) return rc;
    }

    bench_trace("step=pre-measure-sync db=%s begin sql=cloudsync_network_sync(500,4)", label);
    rc = db_exec(db, "SELECT cloudsync_network_sync(500, 4);");
    bench_trace("step=pre-measure-sync db=%s end rc=%d", label, rc);
    return rc;
}

static int setup_database(const char *label, const char *path, const char *database_id, const char *address, const char *apikey, sqlite3 **out_db) {
    bench_trace("step=setup-database db=%s path=%s begin", label, path);
    int rc = open_load_ext(path, out_db);
    if (rc != SQLITE_OK) {
        bench_trace("step=setup-database db=%s end rc=%d", label, rc);
        return rc;
    }

    rc = init_schema(*out_db, label);
    if (rc != SQLITE_OK) {
        bench_trace("step=setup-database db=%s end rc=%d", label, rc);
        return rc;
    }

    rc = init_network(*out_db, label, database_id, address, apikey);
    bench_trace("step=setup-database db=%s end rc=%d", label, rc);
    return rc;
}

static int verify_row(sqlite3 *db, const char *id, const char *payload, const char *marker,
                      const void *random_blob, int random_blob_size, bool *verified) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT payload, marker, random_blob FROM sync_bench_items WHERE id = ?;";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error while preparing verification query: %s\n", sqlite3_errmsg(db));
        return rc;
    }

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char *actual_payload = (const char *)sqlite3_column_text(stmt, 0);
        const char *actual_marker = (const char *)sqlite3_column_text(stmt, 1);
        const void *actual_blob = sqlite3_column_blob(stmt, 2);
        int actual_blob_size = sqlite3_column_bytes(stmt, 2);
        bool blob_matches = (actual_blob_size == random_blob_size) &&
                            (random_blob_size == 0 ||
                             (actual_blob && random_blob && memcmp(actual_blob, random_blob, (size_t)random_blob_size) == 0));
        *verified = actual_payload && actual_marker &&
                    strcmp(actual_payload, payload) == 0 &&
                    strcmp(actual_marker, marker) == 0 &&
                    blob_matches;
        rc = SQLITE_OK;
    } else if (rc == SQLITE_DONE) {
        *verified = false;
        rc = SQLITE_OK;
    } else {
        fprintf(stderr, "Error while verifying row: %s\n", sqlite3_errmsg(db));
    }

    int finalize_rc = sqlite3_finalize(stmt);
    if (rc == SQLITE_OK && finalize_rc != SQLITE_OK) rc = finalize_rc;
    return rc;
}

static int insert_benchmark_row(sqlite3 *db, const char *id, const char *payload, const char *marker,
                                const void *random_blob, int random_blob_size) {
    bench_trace("step=insert-source-row db=db_a row_id=%s random_blob_size_bytes=%d begin", id, random_blob_size);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO sync_bench_items (id, payload, marker, random_blob, updated_at) VALUES (?, ?, ?, ?, datetime('now'));";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error while preparing insert: %s\n", sqlite3_errmsg(db));
        bench_trace("step=insert-source-row db=db_a row_id=%s end rc=%d", id, rc);
        return rc;
    }

    rc = sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, payload, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, marker, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(stmt, 4, random_blob, random_blob_size, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error while binding benchmark row: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        bench_trace("step=insert-source-row db=db_a row_id=%s random_blob_size_bytes=%d end rc=%d", id, random_blob_size, rc);
        return rc;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    else fprintf(stderr, "Error while inserting benchmark row: %s\n", sqlite3_errmsg(db));

    int finalize_rc = sqlite3_finalize(stmt);
    if (rc == SQLITE_OK && finalize_rc != SQLITE_OK) rc = finalize_rc;
    bench_trace("step=insert-source-row db=db_a row_id=%s random_blob_size_bytes=%d end rc=%d", id, random_blob_size, rc);
    return rc;
}

static int cleanup_old_benchmark_rows(sqlite3 *db, int older_than_seconds, int *deleted_count) {
    if (deleted_count) *deleted_count = 0;
    if (older_than_seconds <= 0) {
        bench_trace("step=cleanup-old-source-rows db=db_a enabled=false");
        return SQLITE_OK;
    }

    char modifier[64];
    snprintf(modifier, sizeof(modifier), "-%d seconds", older_than_seconds);

    bench_trace("step=cleanup-old-source-rows db=db_a older_than_seconds=%d begin", older_than_seconds);
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "DELETE FROM sync_bench_items "
        "WHERE marker LIKE 'sync-bench-%' "
        "AND updated_at < datetime('now', ?);";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error while preparing cleanup delete: %s\n", sqlite3_errmsg(db));
        bench_trace("step=cleanup-old-source-rows db=db_a end rc=%d deleted=0", rc);
        return rc;
    }

    rc = sqlite3_bind_text(stmt, 1, modifier, -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error while binding cleanup delete: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        bench_trace("step=cleanup-old-source-rows db=db_a end rc=%d deleted=0", rc);
        return rc;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
        if (deleted_count) *deleted_count = sqlite3_changes(db);
    } else {
        fprintf(stderr, "Error while deleting old benchmark rows: %s\n", sqlite3_errmsg(db));
    }

    int finalize_rc = sqlite3_finalize(stmt);
    if (rc == SQLITE_OK && finalize_rc != SQLITE_OK) rc = finalize_rc;
    bench_trace("step=cleanup-old-source-rows db=db_a end rc=%d deleted=%d", rc, deleted_count ? *deleted_count : 0);
    return rc;
}

static int json_int_at_path(sqlite3 *db, const char *json, const char *path, int default_value) {
    sqlite3_stmt *stmt = NULL;
    int value = default_value;
    int rc = sqlite3_prepare_v2(db, "SELECT json_extract(?, ?);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return default_value;
    sqlite3_bind_text(stmt, 1, json ? json : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) value = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

static char *json_text_at_path(sqlite3 *db, const char *json, const char *path) {
    sqlite3_stmt *stmt = NULL;
    char *value = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT json_extract(?, ?);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, json ? json : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        value = str_dup((const char *)sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return value;
}

static int timed_request(sqlite3 *db, sync_bench_request *request, const char *operation, int attempt, const char *sql) {
    request->operation = operation;
    request->attempt = attempt;
    request->rows_received = -1;
    request->result_json = NULL;
    request->sqlite_rc = timed_query_text(db, sql, &request->result_json, &request->started_ms, &request->ended_ms);
    request->elapsed_ms = request->ended_ms - request->started_ms;
    if (strcmp(operation, "check") == 0 && request->result_json) {
        request->rows_received = json_int_at_path(db, request->result_json, "$.receive.rows", -1);
    }
    return request->sqlite_rc;
}

static void json_print_escaped(const char *value) {
    putchar('"');
    if (value) {
        for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
            switch (*p) {
                case '\\': printf("\\\\"); break;
                case '"': printf("\\\""); break;
                case '\n': printf("\\n"); break;
                case '\r': printf("\\r"); break;
                case '\t': printf("\\t"); break;
                default:
                    if (*p < 0x20) printf("\\u%04x", *p);
                    else putchar(*p);
            }
        }
    }
    putchar('"');
}

static void print_text_report(const char *database_id, int poll_delay_ms, int max_polls, int random_blob_size,
                              int cleanup_older_than_seconds, int cleanup_deleted_rows,
                              const char *row_id, bool applied,
                              int polls, double total_ms, double verify_ms, double request_ms, double poll_sleep_ms,
                              double measured_overhead_ms, sync_bench_send_summary send_summary,
                              sync_bench_request *requests, int request_count) {
    printf("\nSync Performance Benchmark\n");
    printf("database_id: %s\n", database_id);
    printf("poll_delay_ms: %d\n", poll_delay_ms);
    printf("max_polls: %d\n", max_polls);
    printf("random_blob_size_bytes: %d\n", random_blob_size);
    printf("cleanup_older_than_seconds: %d\n", cleanup_older_than_seconds);
    printf("cleanup_deleted_rows: %d\n", cleanup_deleted_rows);
    printf("row_id: %s\n", row_id);
    printf("\nRequests:\n");
    for (int i = 0; i < request_count; i++) {
        if (strcmp(requests[i].operation, "check") == 0) {
            printf("%s[%d]  %.2f ms  rc=%d rows=%d\n", requests[i].operation, requests[i].attempt,
                   requests[i].elapsed_ms, requests[i].sqlite_rc, requests[i].rows_received);
        } else {
            printf("%s[%d]   %.2f ms  rc=%d status=%s localVersion=%d serverVersion=%d\n",
                   requests[i].operation, requests[i].attempt, requests[i].elapsed_ms, requests[i].sqlite_rc,
                   send_summary.status ? send_summary.status : "unknown",
                   send_summary.local_version, send_summary.server_version);
        }
    }
    printf("\nResult:\n");
    printf("applied: %s\n", applied ? "true" : "false");
    printf("polls: %d\n", polls);
    printf("total_send_to_apply_check_end_ms: %.2f\n", total_ms);
    printf("network_request_elapsed_ms: %.2f\n", request_ms);
    printf("poll_sleep_elapsed_ms: %.2f\n", poll_sleep_ms);
    printf("local_overhead_elapsed_ms: %.2f\n", measured_overhead_ms);
    printf("verification_select_ms: %.2f\n", verify_ms);
}

static void print_json_report(int poll_delay_ms, int max_polls, int random_blob_size,
                              int cleanup_older_than_seconds, int cleanup_deleted_rows,
                              const char *row_id, bool applied,
                              int polls, double total_ms, double verify_ms, double request_ms, double poll_sleep_ms,
                              double measured_overhead_ms, sync_bench_send_summary send_summary,
                              sync_bench_request *requests, int request_count) {
    printf("{\n");
    printf("  \"applied\": %s,\n", applied ? "true" : "false");
    printf("  \"pollDelayMs\": %d,\n", poll_delay_ms);
    printf("  \"maxPolls\": %d,\n", max_polls);
    printf("  \"randomBlobSizeBytes\": %d,\n", random_blob_size);
    printf("  \"cleanupOlderThanSeconds\": %d,\n", cleanup_older_than_seconds);
    printf("  \"cleanupDeletedRows\": %d,\n", cleanup_deleted_rows);
    printf("  \"polls\": %d,\n", polls);
    printf("  \"rowId\": "); json_print_escaped(row_id); printf(",\n");
    printf("  \"totalSendToApplyCheckEndMs\": %.2f,\n", total_ms);
    printf("  \"networkRequestElapsedMs\": %.2f,\n", request_ms);
    printf("  \"pollSleepElapsedMs\": %.2f,\n", poll_sleep_ms);
    printf("  \"localOverheadElapsedMs\": %.2f,\n", measured_overhead_ms);
    printf("  \"verificationSelectMs\": %.2f,\n", verify_ms);
    printf("  \"send\": {\"status\": "); json_print_escaped(send_summary.status);
    printf(", \"localVersion\": %d, \"serverVersion\": %d},\n", send_summary.local_version, send_summary.server_version);
    printf("  \"requests\": [\n");
    for (int i = 0; i < request_count; i++) {
        printf("    {\"operation\": "); json_print_escaped(requests[i].operation);
        printf(", \"attempt\": %d, \"sqliteRc\": %d, \"elapsedMs\": %.2f", requests[i].attempt,
               requests[i].sqlite_rc, requests[i].elapsed_ms);
        if (strcmp(requests[i].operation, "check") == 0) printf(", \"rows\": %d", requests[i].rows_received);
        printf(", \"result\": "); json_print_escaped(requests[i].result_json);
        printf("}%s\n", i + 1 == request_count ? "" : ",");
    }
    printf("  ]\n");
    printf("}\n");
}

static void free_requests(sync_bench_request *requests, int request_count) {
    for (int i = 0; i < request_count; i++) free(requests[i].result_json);
}

int main(void) {
    int rc = SQLITE_OK;
    sqlite3 *db_a = NULL;
    sqlite3 *db_b = NULL;
    sync_bench_request *requests = NULL;
    int request_count = 0;
    bool applied = false;
    int polls = 0;
    double total_ms = 0.0;
    double verify_ms = 0.0;
    double poll_sleep_ms = 0.0;
    double request_ms = 0.0;
    double measured_overhead_ms = 0.0;
    sync_bench_send_summary send_summary = {-1, -1, NULL};
    int cleanup_deleted_rows = 0;
    char row_id[UUID_STR_MAXLEN] = "";
    char marker[96] = "";
    char payload[128] = "";
    unsigned char empty_blob = 0;
    void *random_blob = NULL;

    const char *database_id = getenv("SYNC_BENCH_DATABASE_ID");
    const char *address = getenv("SYNC_BENCH_CLOUDSYNC_ADDRESS");
    const char *apikey = getenv("SYNC_BENCH_APIKEY");
    const char *output = getenv("SYNC_BENCH_OUTPUT");
    int poll_delay_ms = env_int("SYNC_BENCH_POLL_DELAY_MS", DEFAULT_POLL_DELAY_MS);
    int max_polls = env_int("SYNC_BENCH_MAX_POLLS", DEFAULT_MAX_POLLS);
    int random_blob_size = env_int("SYNC_BENCH_RANDOM_BLOB_SIZE_BYTES", DEFAULT_RANDOM_BLOB_SIZE_BYTES);
    int cleanup_older_than_seconds = env_int("SYNC_BENCH_CLEANUP_OLDER_THAN_SECONDS", DEFAULT_CLEANUP_OLDER_THAN_SECONDS);

    if (!database_id || !*database_id) {
        fprintf(stderr, "Error: SYNC_BENCH_DATABASE_ID not set.\n");
        return SQLITE_MISUSE;
    }

    requests = (sync_bench_request *)calloc((size_t)max_polls + 1, sizeof(sync_bench_request));
    if (!requests) return SQLITE_NOMEM;

    remove(DB_A_PATH);
    remove(DB_B_PATH);
    cloudsync_memory_init(1);

    bench_trace("step=benchmark-setup begin database_id=%s poll_delay_ms=%d max_polls=%d", database_id, poll_delay_ms, max_polls);
    rc = setup_database("db_a", DB_A_PATH, database_id, address, apikey, &db_a);
    if (rc != SQLITE_OK) goto cleanup;
    rc = setup_database("db_b", DB_B_PATH, database_id, address, apikey, &db_b);
    if (rc != SQLITE_OK) goto cleanup;
    bench_trace("step=benchmark-setup end rc=%d", rc);

    rc = cleanup_old_benchmark_rows(db_a, cleanup_older_than_seconds, &cleanup_deleted_rows);
    if (rc != SQLITE_OK) goto cleanup;
    if (cleanup_deleted_rows > 0) {
        bench_trace("step=cleanup-send db=db_a deleted=%d begin sql=cloudsync_network_send_changes", cleanup_deleted_rows);
        rc = db_exec(db_a, "SELECT cloudsync_network_send_changes();");
        bench_trace("step=cleanup-send db=db_a deleted=%d end rc=%d", cleanup_deleted_rows, rc);
        if (rc != SQLITE_OK) goto cleanup;
    }

    cloudsync_uuid_v7_string(row_id, true);
    snprintf(marker, sizeof(marker), "sync-bench-%s", row_id);
    snprintf(payload, sizeof(payload), "payload-%s", row_id);

    if (random_blob_size > 0) {
        random_blob = malloc((size_t)random_blob_size);
        if (!random_blob) {
            rc = SQLITE_NOMEM;
            goto cleanup;
        }
        sqlite3_randomness(random_blob_size, random_blob);
    } else {
        random_blob = &empty_blob;
    }

    rc = insert_benchmark_row(db_a, row_id, payload, marker, random_blob, random_blob_size);
    if (rc != SQLITE_OK) goto cleanup;

    bench_trace("step=verify-before-send db=db_b row_id=%s begin", row_id);
    rc = verify_row(db_b, row_id, payload, marker, random_blob, random_blob_size, &applied);
    bench_trace("step=verify-before-send db=db_b row_id=%s end rc=%d applied=%s", row_id, rc, applied ? "true" : "false");
    if (rc != SQLITE_OK) goto cleanup;
    if (applied) {
        fprintf(stderr, "Error: benchmark row already exists in receiver before send.\n");
        rc = SQLITE_ERROR;
        goto cleanup;
    }

    bench_trace("step=send db=db_a row_id=%s begin sql=cloudsync_network_send_changes", row_id);
    rc = timed_request(db_a, &requests[request_count++], "send", 1, "SELECT cloudsync_network_send_changes();");
    bench_trace("step=send db=db_a row_id=%s end rc=%d elapsed_ms=%.2f", row_id, rc, requests[request_count - 1].elapsed_ms);
    if (rc != SQLITE_OK) goto cleanup;
    send_summary.status = json_text_at_path(db_a, requests[0].result_json, "$.send.status");
    send_summary.local_version = json_int_at_path(db_a, requests[0].result_json, "$.send.localVersion", -1);
    send_summary.server_version = json_int_at_path(db_a, requests[0].result_json, "$.send.serverVersion", -1);
    double total_start_ms = requests[0].started_ms;

    for (int i = 0; i < max_polls; i++) {
        if (i > 0 && poll_delay_ms > 0) {
            bench_trace("step=poll-sleep attempt=%d delay_ms=%d begin", i + 1, poll_delay_ms);
            double sleep_start_ms = monotonic_ms();
            sqlite3_sleep(poll_delay_ms);
            double sleep_elapsed_ms = monotonic_ms() - sleep_start_ms;
            poll_sleep_ms += sleep_elapsed_ms;
            bench_trace("step=poll-sleep attempt=%d end elapsed_ms=%.2f", i + 1, sleep_elapsed_ms);
        }
        bench_trace("step=check db=db_b attempt=%d row_id=%s begin sql=cloudsync_network_check_changes", i + 1, row_id);
        rc = timed_request(db_b, &requests[request_count++], "check", i + 1, "SELECT cloudsync_network_check_changes();");
        polls = i + 1;
        bench_trace("step=check db=db_b attempt=%d row_id=%s end rc=%d rows=%d elapsed_ms=%.2f", i + 1, row_id, rc, requests[request_count - 1].rows_received, requests[request_count - 1].elapsed_ms);
        if (rc != SQLITE_OK) goto cleanup;

        bench_trace("step=verify-after-check db=db_b attempt=%d row_id=%s begin", i + 1, row_id);
        double verify_start_ms = monotonic_ms();
        rc = verify_row(db_b, row_id, payload, marker, random_blob, random_blob_size, &applied);
        double verify_end_ms = monotonic_ms();
        verify_ms = verify_end_ms - verify_start_ms;
        bench_trace("step=verify-after-check db=db_b attempt=%d row_id=%s end rc=%d applied=%s elapsed_ms=%.2f", i + 1, row_id, rc, applied ? "true" : "false", verify_ms);
        if (rc != SQLITE_OK) goto cleanup;

        if (applied) {
            total_ms = requests[request_count - 1].ended_ms - total_start_ms;
            break;
        }
    }

    if (!applied) {
        total_ms = request_count > 0 ? requests[request_count - 1].ended_ms - requests[0].started_ms : 0.0;
        rc = SQLITE_BUSY;
    }

    for (int i = 0; i < request_count; i++) request_ms += requests[i].elapsed_ms;
    measured_overhead_ms = total_ms - request_ms - poll_sleep_ms;
    if (measured_overhead_ms < 0.0 && measured_overhead_ms > -0.01) measured_overhead_ms = 0.0;

cleanup:
    bench_trace("step=report begin rc=%d applied=%s request_count=%d", rc, applied ? "true" : "false", request_count);
    if (output && strcmp(output, "json") == 0) {
        print_json_report(poll_delay_ms, max_polls, random_blob_size, cleanup_older_than_seconds, cleanup_deleted_rows,
                          row_id, applied, polls, total_ms, verify_ms,
                          request_ms, poll_sleep_ms, measured_overhead_ms, send_summary,
                          requests, request_count);
    } else {
        print_text_report(database_id, poll_delay_ms, max_polls, random_blob_size, cleanup_older_than_seconds, cleanup_deleted_rows,
                          row_id, applied, polls, total_ms, verify_ms,
                          request_ms, poll_sleep_ms, measured_overhead_ms, send_summary,
                          requests, request_count);
    }
    bench_trace("step=report end rc=%d", rc);

    if (!applied && rc == SQLITE_BUSY) {
        fprintf(stderr, "Error: row was not applied to receiver after %d polls.\n", max_polls);
    }

    if (db_a) {
        bench_trace("step=terminate db=db_a begin");
        db_exec(db_a, "SELECT cloudsync_terminate();");
        sqlite3_close(db_a);
        bench_trace("step=terminate db=db_a end");
    }
    if (db_b) {
        bench_trace("step=terminate db=db_b begin");
        db_exec(db_b, "SELECT cloudsync_terminate();");
        sqlite3_close(db_b);
        bench_trace("step=terminate db=db_b end");
    }
    free_requests(requests, request_count);
    free(send_summary.status);
    if (random_blob && random_blob != &empty_blob) free(random_blob);
    free(requests);
    cloudsync_memory_finalize();
    return rc == SQLITE_OK ? 0 : rc;
}
