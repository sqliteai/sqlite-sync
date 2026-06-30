//
//  chunk_bench.c
//  cloudsync
//
//  Local-only benchmark for the positional /check drain: build a window of N
//  chunks and time paging the whole window one chunk per call via the
//  (resume_db_version, resume_seq, resume_frag_offset) cursor on
//  cloudsync_payload_chunks. Reports wall time and per-chunk cost so the
//  computational growth of the drain (currently O(N^2): each resume re-scans
//  cloudsync_changes) can be tracked — e.g. to confirm a future indexed
//  (db_version, seq) seek flattens it to O(N).
//
//  Env: CHUNK_BENCH_ROWS (default 400), CHUNK_BENCH_ROW_BYTES (default 60000),
//       CHUNK_BENCH_TXNS (default 1; rows split across this many db_versions),
//       CHUNK_BENCH_REPEATS (default 5), CHUNK_BENCH_CHUNK_SIZE (default 262144).
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include "sqlite3.h"

#define DB_PATH  "dist/chunk-bench.sqlite"
#define EXT_PATH "./dist/cloudsync"

static double monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((double)ts.tv_sec * 1000.0) + ((double)ts.tv_nsec / 1000000.0);
}

static int env_int(const char *name, int dflt) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    char *end = NULL;
    long p = strtol(v, &end, 10);
    if (!end || *end != '\0' || p <= 0) return dflt;
    return (int)p;
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

// Drain the whole window via the positional cursor, one chunk per query. Returns
// the chunk count and accumulates total payload bytes touched into *bytes.
static int drain_positional(sqlite3 *db, int *chunks_out, long long *bytes_out) {
    const char *first_sql =
        "SELECT payload, next_db_version, next_seq, next_frag_offset, is_final, watermark_db_version "
        "FROM cloudsync_payload_chunks WHERE since_db_version=0 LIMIT 1;";
    const char *resume_sql =
        "SELECT payload, next_db_version, next_seq, next_frag_offset, is_final "
        "FROM cloudsync_payload_chunks "
        "WHERE until_db_version=?1 AND resume_db_version=?2 AND resume_seq=?3 AND resume_frag_offset=?4 LIMIT 1;";
    sqlite3_stmt *first = NULL, *resume = NULL;
    int rc = sqlite3_prepare_v2(db, first_sql, -1, &first, NULL);
    if (rc != SQLITE_OK) goto done;
    rc = sqlite3_prepare_v2(db, resume_sql, -1, &resume, NULL);
    if (rc != SQLITE_OK) goto done;

    int chunks = 0;
    long long bytes = 0;
    long long watermark = 0, rdbv = 0, rseq = 0, rfrag = 0;
    bool is_final = false;

    rc = sqlite3_step(first);
    if (rc == SQLITE_ROW) {
        bytes += sqlite3_column_bytes(first, 0);
        rdbv = sqlite3_column_int64(first, 1);
        rseq = sqlite3_column_int64(first, 2);
        rfrag = sqlite3_column_int64(first, 3);
        is_final = sqlite3_column_int(first, 4) != 0;
        watermark = sqlite3_column_int64(first, 5);
        chunks++;
    } else if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
        goto done; // empty window
    } else {
        goto done;
    }

    while (!is_final) {
        sqlite3_reset(resume);
        sqlite3_bind_int64(resume, 1, watermark);
        sqlite3_bind_int64(resume, 2, rdbv);
        sqlite3_bind_int64(resume, 3, rseq);
        sqlite3_bind_int64(resume, 4, rfrag);
        rc = sqlite3_step(resume);
        if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_OK; break; }
        bytes += sqlite3_column_bytes(resume, 0);
        rdbv = sqlite3_column_int64(resume, 1);
        rseq = sqlite3_column_int64(resume, 2);
        rfrag = sqlite3_column_int64(resume, 3);
        is_final = sqlite3_column_int(resume, 4) != 0;
        chunks++;
    }
    rc = SQLITE_OK;
    *chunks_out = chunks;
    *bytes_out = bytes;

done:
    if (first) sqlite3_finalize(first);
    if (resume) sqlite3_finalize(resume);
    return rc;
}

int main(void) {
    int rows = env_int("CHUNK_BENCH_ROWS", 400);
    int row_bytes = env_int("CHUNK_BENCH_ROW_BYTES", 60000);
    int repeats = env_int("CHUNK_BENCH_REPEATS", 5);
    int chunk_size = env_int("CHUNK_BENCH_CHUNK_SIZE", 262144);

    remove(DB_PATH);
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) { fprintf(stderr, "open failed\n"); return 1; }
    if (sqlite3_enable_load_extension(db, 1) != SQLITE_OK) return 1;
    if (db_exec(db, "SELECT load_extension('" EXT_PATH "');") != SQLITE_OK) return 1;

    char setup[256];
    snprintf(setup, sizeof(setup),
        "CREATE TABLE chunk_bench (id TEXT PRIMARY KEY, body BLOB);"
        "SELECT cloudsync_init('chunk_bench');"
        "SELECT cloudsync_set('payload_max_chunk_size', '%d');", chunk_size);
    if (db_exec(db, setup) != SQLITE_OK) return 1;

    // Split the rows across CHUNK_BENCH_TXNS transactions: each is one db_version,
    // so TXNS=1 is the pathological single-version window and TXNS=rows is the
    // many-versions case a real /check window resembles. Incompressible bodies keep
    // the window many-chunked.
    int txns = env_int("CHUNK_BENCH_TXNS", 1);
    if (txns < 1) txns = 1;
    if (txns > rows) txns = rows;
    int idbase = 0;
    for (int t = 0; t < txns; ++t) {
        int n = rows / txns + (t < rows % txns ? 1 : 0);
        if (n <= 0) continue;
        char insert[256];
        snprintf(insert, sizeof(insert),
            "WITH RECURSIVE c(i) AS (SELECT %d UNION ALL SELECT i+1 FROM c WHERE i < %d) "
            "INSERT INTO chunk_bench(id, body) SELECT printf('row-%%06d', i), randomblob(%d) FROM c;",
            idbase + 1, idbase + n, row_bytes);
        if (db_exec(db, insert) != SQLITE_OK) return 1;
        idbase += n;
    }

    int chunks = 0;
    long long bytes = 0;
    double best = 1e18, sum = 0;
    for (int r = 0; r < repeats; ++r) {
        double t0 = monotonic_ms();
        if (drain_positional(db, &chunks, &bytes) != SQLITE_OK) { fprintf(stderr, "positional drain failed\n"); return 1; }
        double dt = monotonic_ms() - t0;
        sum += dt; if (dt < best) best = dt;
    }

    double mean = sum / repeats;
    printf("\nPositional /check drain benchmark (local SQLite, no network)\n");
    printf("rows: %d   row_bytes: %d   txns: %d   chunk_size: %d   repeats: %d\n",
           rows, row_bytes, txns, chunk_size, repeats);
    printf("chunks: %d   payload_bytes: %lld\n", chunks, bytes);
    printf("drain: best=%.2f ms   mean=%.2f ms\n", best, mean);
    if (chunks > 0)
        printf("per-chunk: best=%.3f ms   throughput: %.1f MB/s\n",
               best / chunks, (double)bytes / 1024.0 / 1024.0 / (best / 1000.0));

    sqlite3_close(db);
    remove(DB_PATH);
    return 0;
}
