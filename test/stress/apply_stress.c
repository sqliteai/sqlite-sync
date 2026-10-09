//
//  apply_stress.c
//  cloudsync
//
//  Concurrent cloudsync_payload_apply stress test. Several devices produce payloads
//  that each span several db_versions; one thread per device then applies its
//  payloads to a shared WAL database file on its own connection, all at once, the
//  way a sync server applies uploads from many devices. Each mode wraps the apply
//  differently:
//
//    autocommit — SELECT cloudsync_payload_apply(?) on its own: the apply commits
//      once per source db_version through its own savepoints.
//    deferred   — BEGIN; apply; COMMIT. What the SQLite Cloud server does by default:
//      it runs every write command inside an implicit BEGIN TRANSACTION.
//    immediate  — BEGIN IMMEDIATE; apply; COMMIT. The write lock is taken before any
//      read, so contention is a lock wait that busy_timeout absorbs.
//    concurrent — BEGIN CONCURRENT; apply; COMMIT. The server's mode when concurrent
//      transactions are enabled. Needs a SQLite built from the begin-concurrent
//      branch; skipped otherwise.
//
//  Retries follow the server: an apply request makes up to 1 + APPLY_STRESS_RETRIES
//  attempts, and a COMMIT that returns BUSY is retried 5 more times (10..50 ms) before
//  the transaction is rolled back. A request that runs out of attempts fails and the
//  device sends it again after APPLY_STRESS_REQUEST_BACKOFF_MS.
//
//  Every mode must converge to the same content as a reference database that applied
//  every payload serially, and in immediate mode the apply statement itself must never
//  return BUSY. Failed requests, retries, BUSY codes, wall time and process CPU time
//  are reported for comparison.
//
//  Not built or run by CI: it needs threads and its timings are machine-dependent.
//  Run it by hand with `make apply-stress`; APPLY_STRESS_SQLITE_DIR=<dir> builds it
//  against another sqlite3.c (for example the server's, for BEGIN CONCURRENT).
//
//  Env: APPLY_STRESS_DEVICES (default 8), APPLY_STRESS_PAYLOADS (payloads per device,
//       default 20), APPLY_STRESS_VERSIONS (db_versions per payload, default 5),
//       APPLY_STRESS_ROWS (rows per db_version, default 4),
//       APPLY_STRESS_SHARED (1 = devices write the same keys, default 0),
//       APPLY_STRESS_MODE (comma-separated modes, default all four),
//       APPLY_STRESS_BUSY_TIMEOUT_MS (default 5000),
//       APPLY_STRESS_RETRIES (retries per request, default 6),
//       APPLY_STRESS_BACKOFF_MS (max jittered sleep between retries, default 5),
//       APPLY_STRESS_REQUEST_BACKOFF_MS (sleep before re-sending a failed request,
//         default 100), APPLY_STRESS_MAX_REQUESTS (per payload, default 1000),
//       APPLY_STRESS_EXT (default ./dist/cloudsync).
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/resource.h>
#include "sqlite3.h"

#define DB_PATH  "dist/apply-stress.sqlite"
#define EXT_PATH "./dist/cloudsync"
#define MAX_CODES 16
#define COMMIT_RETRIES 5

typedef enum {
    MODE_AUTOCOMMIT,
    MODE_DEFERRED,
    MODE_IMMEDIATE,
    MODE_CONCURRENT,
    MODE_COUNT
} apply_mode;

static const char *mode_names[MODE_COUNT] = {"autocommit", "deferred", "immediate", "concurrent"};
static const char *mode_begin[MODE_COUNT] = {NULL, "BEGIN", "BEGIN IMMEDIATE", "BEGIN CONCURRENT"};

typedef enum {
    ATTEMPT_OK,
    ATTEMPT_BUSY,
    ATTEMPT_FATAL
} attempt_result;

typedef struct {
    char *blob;
    int size;
} payload_t;

typedef struct {
    int code;
    int count;
    char message[160];
} code_count;

typedef struct {
    // input
    int device;
    apply_mode mode;
    payload_t *payloads;
    int npayloads;
    // output
    int applied;
    int attempts;
    int failed_requests;
    int busy_begin;
    int busy_apply;
    int busy_commit;
    int fatal;
    char fatal_message[256];
    code_count codes[MAX_CODES];
    int ncodes;
} worker_t;

static struct {
    int devices, payloads, versions, rows;
    bool shared;
    int busy_timeout_ms, retries, backoff_ms, request_backoff_ms, max_requests;
    const char *ext_path;
} cfg;

static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static bool gate_open = false;

static int env_int(const char *name, int dflt) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    char *end = NULL;
    long p = strtol(v, &end, 10);
    if (!end || *end != '\0' || p < 0) return dflt;
    return (int)p;
}

static const char *env_str(const char *name, const char *dflt) {
    const char *v = getenv(name);
    return (v && *v) ? v : dflt;
}

static double monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((double)ts.tv_sec * 1000.0) + ((double)ts.tv_nsec / 1000000.0);
}

static double cpu_ms(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0 + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
}

static int db_exec(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "exec failed: %s: %s\n", sql, err ? err : sqlite3_errmsg(db));
        sqlite3_free(err);
    }
    return rc;
}

static sqlite3 *open_db(const char *path) {
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "open failed: %s: %s\n", path, db ? sqlite3_errmsg(db) : "out of memory");
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_extended_result_codes(db, 1);
    sqlite3_enable_load_extension(db, 1);
    char *err = NULL;
    if (sqlite3_load_extension(db, cfg.ext_path, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "load_extension failed: %s: %s\n", cfg.ext_path, err ? err : sqlite3_errmsg(db));
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

static const char *schema_sql =
    "CREATE TABLE t (id TEXT PRIMARY KEY NOT NULL, dev INTEGER, n INTEGER, body TEXT);"
    "SELECT cloudsync_init('t');";

// Builds one device's payloads: each covers cfg.versions local transactions.
static int build_device_payloads(int device, payload_t *out) {
    sqlite3 *db = open_db(":memory:");
    if (!db) return 1;
    int rc = db_exec(db, schema_sql);
    sqlite3_stmt *ins = NULL, *enc = NULL;
    if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(db,
        "INSERT INTO t (id, dev, n, body) VALUES (?1, ?2, ?3, ?4) "
        "ON CONFLICT(id) DO UPDATE SET dev=excluded.dev, n=excluded.n, body=excluded.body", -1, &ins, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(db,
        "SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) "
        "FROM cloudsync_changes WHERE db_version > ?1", -1, &enc, NULL);
    int64_t last = 0;
    int n = 0;
    for (int p = 0; rc == SQLITE_OK && p < cfg.payloads; p++) {
        for (int v = 0; rc == SQLITE_OK && v < cfg.versions; v++) {
            rc = db_exec(db, "BEGIN");
            for (int r = 0; rc == SQLITE_OK && r < cfg.rows; r++, n++) {
                char id[64], body[64];
                if (cfg.shared) snprintf(id, sizeof(id), "row-%d", (v * cfg.rows) + r);
                else snprintf(id, sizeof(id), "d%d-p%d-v%d-r%d", device, p, v, r);
                snprintf(body, sizeof(body), "device %d write %d", device, n);
                sqlite3_bind_text(ins, 1, id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(ins, 2, device);
                sqlite3_bind_int(ins, 3, n);
                sqlite3_bind_text(ins, 4, body, -1, SQLITE_TRANSIENT);
                rc = sqlite3_step(ins);
                rc = (rc == SQLITE_DONE) ? SQLITE_OK : rc;
                sqlite3_reset(ins);
            }
            if (rc == SQLITE_OK) rc = db_exec(db, "COMMIT");
        }
        if (rc != SQLITE_OK) break;
        sqlite3_bind_int64(enc, 1, last);
        rc = sqlite3_step(enc);
        if (rc == SQLITE_ROW && sqlite3_column_type(enc, 0) == SQLITE_BLOB) {
            out[p].size = sqlite3_column_bytes(enc, 0);
            out[p].blob = malloc((size_t)out[p].size);
            if (out[p].blob) memcpy(out[p].blob, sqlite3_column_blob(enc, 0), (size_t)out[p].size);
            rc = out[p].blob ? SQLITE_OK : SQLITE_NOMEM;
        } else {
            fprintf(stderr, "device %d payload %d: encode failed: %s\n", device, p, sqlite3_errmsg(db));
            rc = SQLITE_ERROR;
        }
        sqlite3_reset(enc);
        sqlite3_stmt *mx = NULL;
        if (rc == SQLITE_OK && sqlite3_prepare_v2(db, "SELECT max(db_version) FROM cloudsync_changes", -1, &mx, NULL) == SQLITE_OK) {
            if (sqlite3_step(mx) == SQLITE_ROW) last = sqlite3_column_int64(mx, 0);
        }
        sqlite3_finalize(mx);
    }
    if (rc != SQLITE_OK) fprintf(stderr, "device %d: %s\n", device, sqlite3_errmsg(db));
    sqlite3_finalize(ins);
    sqlite3_finalize(enc);
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : 1;
}

// Ordered content of t, used to compare a stressed database with the reference.
static char *table_content(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    char *out = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT count(*) || ':' || ifnull(group_concat(id || '|' || dev || '|' || n || '|' || body, ','), '') "
            "FROM (SELECT * FROM t ORDER BY id)", -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(stmt, 0);
        out = s ? strdup(s) : NULL;
    }
    sqlite3_finalize(stmt);
    return out;
}

static bool is_busy(int rc, const char *msg) {
    if ((rc & 0xff) == SQLITE_BUSY || (rc & 0xff) == SQLITE_LOCKED) return true;
    return msg && strstr(msg, "database is locked") != NULL;
}

static void record_code(worker_t *w, int rc, const char *msg) {
    for (int i = 0; i < w->ncodes; i++) {
        if (w->codes[i].code == rc) { w->codes[i].count++; return; }
    }
    if (w->ncodes == MAX_CODES) return;
    code_count *c = &w->codes[w->ncodes++];
    c->code = rc;
    c->count = 1;
    snprintf(c->message, sizeof(c->message), "%s", msg ? msg : "");
}

static void sleep_jittered(unsigned int *seed, int max_ms) {
    if (max_ms <= 0) return;
    usleep((useconds_t)(rand_r(seed) % (unsigned)(max_ms * 1000)) + 100);
}

static void rollback_if_open(sqlite3 *db) {
    if (!sqlite3_get_autocommit(db)) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
}

// One attempt to apply a payload: an error that is not BUSY is fatal.
static attempt_result apply_once(worker_t *w, sqlite3 *db, sqlite3_stmt *apply, const payload_t *payload) {
    const char *begin = mode_begin[w->mode];
    int rc;
    if (begin) {
        rc = sqlite3_exec(db, begin, NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            record_code(w, sqlite3_extended_errcode(db), sqlite3_errmsg(db));
            if (!is_busy(rc, sqlite3_errmsg(db))) goto fatal;
            rollback_if_open(db);
            w->busy_begin++;
            return ATTEMPT_BUSY;
        }
    }

    sqlite3_bind_blob(apply, 1, payload->blob, payload->size, SQLITE_STATIC);
    rc = sqlite3_step(apply);
    if (rc != SQLITE_ROW) {
        int code = sqlite3_extended_errcode(db);
        char msg[256];
        snprintf(msg, sizeof(msg), "%s", sqlite3_errmsg(db));
        sqlite3_reset(apply);
        record_code(w, code, msg);
        rollback_if_open(db);
        if (!is_busy(code, msg)) {
            snprintf(w->fatal_message, sizeof(w->fatal_message), "apply: %s (%d)", msg, code);
            return ATTEMPT_FATAL;
        }
        w->busy_apply++;
        return ATTEMPT_BUSY;
    }
    sqlite3_reset(apply);
    if (!begin) return ATTEMPT_OK;

    rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    for (int i = 0; is_busy(rc, NULL) && i < COMMIT_RETRIES; i++) {
        usleep((useconds_t)(10000 * (i + 1)));
        rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    }
    if (rc == SQLITE_OK) return ATTEMPT_OK;
    record_code(w, sqlite3_extended_errcode(db), sqlite3_errmsg(db));
    if (!is_busy(rc, sqlite3_errmsg(db))) goto fatal;
    rollback_if_open(db);
    w->busy_commit++;
    return ATTEMPT_BUSY;

fatal:
    snprintf(w->fatal_message, sizeof(w->fatal_message), "%s", sqlite3_errmsg(db));
    rollback_if_open(db);
    return ATTEMPT_FATAL;
}

static void *worker_main(void *arg) {
    worker_t *w = (worker_t *)arg;
    unsigned int seed = (unsigned int)(w->device * 7919 + 17);
    sqlite3 *db = open_db(DB_PATH);
    sqlite3_stmt *apply = NULL;
    if (!db || sqlite3_prepare_v2(db, "SELECT cloudsync_payload_apply(?1)", -1, &apply, NULL) != SQLITE_OK) {
        w->fatal = 1;
        snprintf(w->fatal_message, sizeof(w->fatal_message), "setup: %s", db ? sqlite3_errmsg(db) : "open failed");
        goto done;
    }
    sqlite3_busy_timeout(db, cfg.busy_timeout_ms);

    pthread_mutex_lock(&gate_lock);
    while (!gate_open) pthread_cond_wait(&gate_cond, &gate_lock);
    pthread_mutex_unlock(&gate_lock);

    for (int p = 0; p < w->npayloads && !w->fatal; p++) {
        bool applied = false;
        for (int request = 0; !applied && !w->fatal; request++) {
            if (request == cfg.max_requests) {
                snprintf(w->fatal_message, sizeof(w->fatal_message), "payload %d: gave up after %d failed requests", p, request);
                w->fatal = 1;
                break;
            }
            if (request > 0) sleep_jittered(&seed, cfg.request_backoff_ms);
            for (int attempt = 0; attempt <= cfg.retries; attempt++) {
                if (attempt > 0) sleep_jittered(&seed, cfg.backoff_ms);
                w->attempts++;
                attempt_result res = apply_once(w, db, apply, &w->payloads[p]);
                if (res == ATTEMPT_OK) { applied = true; break; }
                if (res == ATTEMPT_FATAL) { w->fatal = 1; break; }
            }
            if (!applied && !w->fatal) w->failed_requests++;
        }
        if (applied) w->applied++;
    }

done:
    sqlite3_finalize(apply);
    sqlite3_close(db);
    return NULL;
}

static void remove_db_files(void) {
    remove(DB_PATH);
    remove(DB_PATH "-wal");
    remove(DB_PATH "-shm");
}

static bool begin_concurrent_supported(void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    bool ok = sqlite3_open(":memory:", &db) == SQLITE_OK &&
              sqlite3_prepare_v2(db, "BEGIN CONCURRENT", -1, &stmt, NULL) == SQLITE_OK;
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return ok;
}

// Runs one mode against a fresh database. Returns the number of failed checks.
static int run_mode(apply_mode mode, payload_t **payloads, const char *reference) {
    remove_db_files();
    sqlite3 *setup = open_db(DB_PATH);
    if (!setup) return 1;
    if (db_exec(setup, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;") != SQLITE_OK ||
        db_exec(setup, schema_sql) != SQLITE_OK) {
        sqlite3_close(setup);
        return 1;
    }
    sqlite3_close(setup);

    worker_t *workers = calloc((size_t)cfg.devices, sizeof(worker_t));
    pthread_t *threads = calloc((size_t)cfg.devices, sizeof(pthread_t));
    if (!workers || !threads) { free(workers); free(threads); return 1; }
    gate_open = false;
    for (int d = 0; d < cfg.devices; d++) {
        workers[d].device = d;
        workers[d].mode = mode;
        workers[d].payloads = payloads[d];
        workers[d].npayloads = cfg.payloads;
        pthread_create(&threads[d], NULL, worker_main, &workers[d]);
    }

    // Let every thread open its connection before the clock starts.
    usleep(200 * 1000);
    double cpu0 = cpu_ms(), t0 = monotonic_ms();
    pthread_mutex_lock(&gate_lock);
    gate_open = true;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_lock);
    for (int d = 0; d < cfg.devices; d++) pthread_join(threads[d], NULL);
    double wall = monotonic_ms() - t0, cpu = cpu_ms() - cpu0;

    int failures = 0, applied = 0, attempts = 0, failed_requests = 0, busy_begin = 0, busy_apply = 0, busy_commit = 0;
    code_count totals[MAX_CODES];
    int ntotals = 0;
    printf("\n[%s]\n", mode_names[mode]);
    for (int d = 0; d < cfg.devices; d++) {
        worker_t *w = &workers[d];
        applied += w->applied;
        attempts += w->attempts;
        failed_requests += w->failed_requests;
        busy_begin += w->busy_begin;
        busy_apply += w->busy_apply;
        busy_commit += w->busy_commit;
        if (w->fatal) {
            printf("  FAIL device %d: %s\n", d, w->fatal_message);
            failures++;
        }
        for (int i = 0; i < w->ncodes; i++) {
            int j = 0;
            while (j < ntotals && totals[j].code != w->codes[i].code) j++;
            if (j == ntotals) {
                if (ntotals == MAX_CODES) continue;
                totals[ntotals++] = w->codes[i];
            } else {
                totals[j].count += w->codes[i].count;
            }
        }
    }

    int expected = cfg.devices * cfg.payloads;
    printf("  payloads applied   %d / %d\n", applied, expected);
    printf("  apply attempts     %d (%d retries)\n", attempts, attempts - applied);
    printf("  failed requests    %d\n", failed_requests);
    printf("  BUSY at BEGIN      %d\n", busy_begin);
    printf("  BUSY in apply      %d\n", busy_apply);
    printf("  BUSY at COMMIT     %d\n", busy_commit);
    for (int i = 0; i < ntotals; i++)
        printf("    code %-5d x%-6d %s\n", totals[i].code, totals[i].count, totals[i].message);
    printf("  wall time          %.0f ms\n", wall);
    printf("  process CPU time   %.0f ms (%.2f cores busy)\n", cpu, wall > 0 ? cpu / wall : 0);
    printf("  CPU per payload    %.2f ms\n", applied > 0 ? cpu / applied : 0);

    if (applied != expected) failures++;
    if (mode == MODE_IMMEDIATE && busy_apply > 0) {
        printf("  FAIL the apply statement returned BUSY while holding the write lock\n");
        failures++;
    }

    sqlite3 *check = open_db(DB_PATH);
    char *content = check ? table_content(check) : NULL;
    if (!content || strcmp(content, reference) != 0) {
        printf("  FAIL content differs from the serial reference\n");
        failures++;
    } else {
        printf("  content            matches the serial reference\n");
    }
    free(content);
    sqlite3_close(check);
    free(workers);
    free(threads);
    return failures;
}

static bool mode_selected(const char *list, const char *name) {
    size_t len = strlen(name);
    for (const char *p = list; (p = strstr(p, name)) != NULL; p += len) {
        bool starts = (p == list || p[-1] == ',');
        bool ends = (p[len] == '\0' || p[len] == ',');
        if (starts && ends) return true;
    }
    return false;
}

int main(void) {
    cfg.devices = env_int("APPLY_STRESS_DEVICES", 8);
    cfg.payloads = env_int("APPLY_STRESS_PAYLOADS", 20);
    cfg.versions = env_int("APPLY_STRESS_VERSIONS", 5);
    cfg.rows = env_int("APPLY_STRESS_ROWS", 4);
    cfg.shared = env_int("APPLY_STRESS_SHARED", 0) != 0;
    cfg.busy_timeout_ms = env_int("APPLY_STRESS_BUSY_TIMEOUT_MS", 5000);
    cfg.retries = env_int("APPLY_STRESS_RETRIES", 6);
    cfg.backoff_ms = env_int("APPLY_STRESS_BACKOFF_MS", 5);
    cfg.request_backoff_ms = env_int("APPLY_STRESS_REQUEST_BACKOFF_MS", 100);
    cfg.max_requests = env_int("APPLY_STRESS_MAX_REQUESTS", 1000);
    cfg.ext_path = env_str("APPLY_STRESS_EXT", EXT_PATH);
    const char *modes = env_str("APPLY_STRESS_MODE", "autocommit,deferred,immediate,concurrent");
    if (cfg.devices < 1 || cfg.payloads < 1 || cfg.versions < 1 || cfg.rows < 1 || cfg.max_requests < 1) {
        fprintf(stderr, "devices, payloads, versions, rows and max requests must be positive\n");
        return 1;
    }
    if (sqlite3_threadsafe() == 0) {
        fprintf(stderr, "SQLite was built without thread safety\n");
        return 1;
    }

    printf("SQLite %s, devices %d, payloads/device %d, db_versions/payload %d, rows/db_version %d, %s keys\n",
           sqlite3_libversion(), cfg.devices, cfg.payloads, cfg.versions, cfg.rows, cfg.shared ? "shared" : "distinct");
    printf("busy_timeout %d ms, %d retries per request (backoff up to %d ms), request re-send after up to %d ms\n",
           cfg.busy_timeout_ms, cfg.retries, cfg.backoff_ms, cfg.request_backoff_ms);

    payload_t **payloads = calloc((size_t)cfg.devices, sizeof(payload_t *));
    if (!payloads) return 1;
    for (int d = 0; d < cfg.devices; d++) {
        payloads[d] = calloc((size_t)cfg.payloads, sizeof(payload_t));
        if (!payloads[d] || build_device_payloads(d, payloads[d]) != 0) return 1;
    }

    // Apply order does not matter to a converged CRDT, so a serial apply is the oracle.
    sqlite3 *ref = open_db(":memory:");
    if (!ref || db_exec(ref, schema_sql) != SQLITE_OK) return 1;
    sqlite3_stmt *apply = NULL;
    if (sqlite3_prepare_v2(ref, "SELECT cloudsync_payload_apply(?1)", -1, &apply, NULL) != SQLITE_OK) return 1;
    for (int d = 0; d < cfg.devices; d++) {
        for (int p = 0; p < cfg.payloads; p++) {
            sqlite3_bind_blob(apply, 1, payloads[d][p].blob, payloads[d][p].size, SQLITE_STATIC);
            if (sqlite3_step(apply) != SQLITE_ROW) {
                fprintf(stderr, "reference apply failed: %s\n", sqlite3_errmsg(ref));
                return 1;
            }
            sqlite3_reset(apply);
        }
    }
    sqlite3_finalize(apply);
    char *reference = table_content(ref);
    sqlite3_close(ref);
    if (!reference) return 1;

    int failures = 0;
    for (int m = 0; m < MODE_COUNT; m++) {
        if (!mode_selected(modes, mode_names[m])) continue;
        if (m == MODE_CONCURRENT && !begin_concurrent_supported()) {
            printf("\n[%s]\n  skipped: this SQLite build has no BEGIN CONCURRENT (set APPLY_STRESS_SQLITE_DIR)\n", mode_names[m]);
            continue;
        }
        failures += run_mode((apply_mode)m, payloads, reference);
    }

    free(reference);
    for (int d = 0; d < cfg.devices; d++) {
        for (int p = 0; p < cfg.payloads; p++) free(payloads[d][p].blob);
        free(payloads[d]);
    }
    free(payloads);
    remove_db_files();

    printf("\n%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
