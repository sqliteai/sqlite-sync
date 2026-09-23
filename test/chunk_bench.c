//
//  chunk_bench.c
//  cloudsync
//
//  Local-only benchmark for the positional /check drain. Two measurements, on
//  one generated window:
//
//    Phase 1 — drain the whole window one chunk per call through the
//      (resume_db_version, resume_seq, resume_frag_offset) cursor on
//      cloudsync_payload_chunks, timing every chunk. Latency that rises with the
//      chunk index means each resume re-scans cloudsync_changes from the start of
//      the window, making the drain O(N^2); flat latency means the resume seeks.
//
//    Phase 2 — replay the same resume points straight against cloudsync_changes
//      in two SQL shapes: the one the positional branch emits today, and the same
//      clause plus the redundant AND-connected `db_version>=?` lower bound
//      proposed in docs/internal/payload-chunks-resume-scan.md. Same rows, two
//      plans, so the difference is the value of that proposal.
//
//  CI builds this with the other test binaries but never runs it: the timings are
//  machine-dependent. Run it by hand with `make chunk-bench`.
//
//  Env: CHUNK_BENCH_ROWS (default 3000), CHUNK_BENCH_ROW_BYTES (default 8000),
//       CHUNK_BENCH_TXNS (default = rows, i.e. one db_version per row; 1 is the
//         degenerate single-version window where no lower bound can help),
//       CHUNK_BENCH_REPEATS (default 3), CHUNK_BENCH_CHUNK_SIZE (default 262144),
//       CHUNK_BENCH_VERBOSE (1 = print every chunk, not just deciles).
//
//  For a production-sized window, seed once and measure two builds against it:
//       CHUNK_BENCH_DB (database path), CHUNK_BENCH_KEEP=1 (do not delete it),
//       CHUNK_BENCH_REUSE=1 (skip seeding when it already exists),
//       CHUNK_BENCH_EXT (which cloudsync to load), CHUNK_BENCH_PHASE2=0 (skip
//         Phase 2, which is itself quadratic and dominates at that size).
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "sqlite3.h"

#define DB_PATH  "dist/chunk-bench.sqlite"
#define EXT_PATH "./dist/cloudsync"
#define MAX_POINTS 20000
#define BUCKETS 10

static const char *env_str(const char *name, const char *dflt) {
    const char *v = getenv(name);
    return (v && *v) ? v : dflt;
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// Every shape binds ?1=until, ?2=site_id, ?3/?4/?6=resume_db_version, ?5=resume_seq,
// and selects exactly the same rows. Only the spelling of the lower bound differs.

// What the positional branch emits today (cloudsync_sqlite.c:1403-1414): the resume
// point lives entirely inside a disjunction whose two arms carry *distinct*
// parameters. Distinct parameters are what defeats the planner here — see below.
#define SHAPE_CURRENT \
    "SELECT db_version, seq FROM cloudsync_changes " \
    "WHERE db_version<=?1 AND site_id<>?2 AND (db_version>?3 OR (db_version=?4 AND seq>=?5)) " \
    "ORDER BY db_version, seq ASC LIMIT 1"

// The doc's proposal: a redundant conjunct stating the lower bound where a virtual
// table's xBestIndex can see it, alongside the untouched disjunction.
#define SHAPE_PROPOSED \
    "SELECT db_version, seq FROM cloudsync_changes " \
    "WHERE db_version<=?1 AND site_id<>?2 AND db_version>=?6 AND (db_version>?3 OR (db_version=?4 AND seq>=?5)) " \
    "ORDER BY db_version, seq ASC LIMIT 1"

// The same disjunction with one parameter reused across both arms. SQLite derives
// the common lower bound itself once it can see the two arms compare against the
// same value, so this needs no extra term — and it means the defect is a matter of
// parameter identity, not of disjunctions being opaque to xBestIndex.
#define SHAPE_REUSED_PARAM \
    "SELECT db_version, seq FROM cloudsync_changes " \
    "WHERE db_version<=?1 AND site_id<>?2 AND (db_version>?3 OR (db_version=?3 AND seq>=?5)) " \
    "ORDER BY db_version, seq ASC LIMIT 1"

typedef struct {
    int64_t dbv, seq, frag;
} resume_point;

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

// env_int treats 0 as "unset" so that a stray empty value cannot ask for zero rows.
// Flags need the opposite, since 0 is how you turn one off.
static bool env_flag(const char *name, bool dflt) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    return !(strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "no") == 0);
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

// Print the clause cloudsync_changesvtab_best_index hands the planner for one
// shape. This is the doc's verification step 3: whether the resume lower bound
// reaches the virtual table at all is visible right here, in the idxStr.
static void print_plan(sqlite3 *db, const char *label, const char *sql) {
    char *eqp = sqlite3_mprintf("EXPLAIN QUERY PLAN %s", sql);
    if (!eqp) return;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, eqp, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW)
            printf("  %-9s %s\n", label, sqlite3_column_text(stmt, 3));
    }
    sqlite3_finalize(stmt);
    sqlite3_free(eqp);
}

// Drain the whole window via the positional cursor, one chunk per query, timing
// each call and recording the resume point it was issued with so phase 2 can
// replay exactly the same positions.
static int drain_positional(sqlite3 *db, double *per_chunk, resume_point *points,
                            int *chunks_out, long long *bytes_out, int64_t *until_out) {
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
    int64_t watermark = 0, rdbv = 0, rseq = 0, rfrag = 0;
    bool is_final = false;

    double t0 = monotonic_ms();
    rc = sqlite3_step(first);
    double dt = monotonic_ms() - t0;
    if (rc == SQLITE_ROW) {
        bytes += sqlite3_column_bytes(first, 0);
        rdbv = sqlite3_column_int64(first, 1);
        rseq = sqlite3_column_int64(first, 2);
        rfrag = sqlite3_column_int64(first, 3);
        is_final = sqlite3_column_int(first, 4) != 0;
        watermark = sqlite3_column_int64(first, 5);
        if (per_chunk) per_chunk[0] = dt;
        chunks++;
    } else if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
        goto done; // empty window
    } else {
        goto done;
    }

    while (!is_final) {
        if (points && chunks < MAX_POINTS) {
            points[chunks].dbv = rdbv; points[chunks].seq = rseq; points[chunks].frag = rfrag;
        }
        sqlite3_reset(resume);
        sqlite3_bind_int64(resume, 1, watermark);
        sqlite3_bind_int64(resume, 2, rdbv);
        sqlite3_bind_int64(resume, 3, rseq);
        sqlite3_bind_int64(resume, 4, rfrag);
        t0 = monotonic_ms();
        rc = sqlite3_step(resume);
        dt = monotonic_ms() - t0;
        if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_OK; break; }
        bytes += sqlite3_column_bytes(resume, 0);
        rdbv = sqlite3_column_int64(resume, 1);
        rseq = sqlite3_column_int64(resume, 2);
        rfrag = sqlite3_column_int64(resume, 3);
        is_final = sqlite3_column_int(resume, 4) != 0;
        if (per_chunk && chunks < MAX_POINTS) per_chunk[chunks] = dt;
        chunks++;
    }
    rc = SQLITE_OK;
    *chunks_out = chunks;
    *bytes_out = bytes;
    *until_out = watermark;

done:
    if (first) sqlite3_finalize(first);
    if (resume) sqlite3_finalize(resume);
    return rc;
}

// Mean of the samples falling in decile b, so a curve over hundreds of chunks
// reads as ten numbers. Returns -1 for an empty bucket.
static double bucket_mean(const double *v, int n, int b) {
    double sum = 0; int count = 0;
    for (int i = 0; i < n; ++i) {
        if (i * BUCKETS / n != b) continue;
        sum += v[i]; count++;
    }
    return count ? sum / count : -1.0;
}

static void report_curve(const char *title, const double *v, int n) {
    printf("%s\n  decile:", title);
    for (int b = 0; b < BUCKETS; ++b) printf(" %7d%%", (b + 1) * 10);
    printf("\n  ms/call:");
    for (int b = 0; b < BUCKETS; ++b) {
        double m = bucket_mean(v, n, b);
        if (m < 0) printf("       --"); else printf(" %8.3f", m);
    }
    double head = bucket_mean(v, n, 0), tail = bucket_mean(v, n, BUCKETS - 1);
    printf("\n  last/first decile: ");
    if (head > 0 && tail > 0) printf("%.2fx\n", tail / head); else printf("n/a\n");
}

// Time one shape at every recorded resume point. Returns the first row each query
// produced so the caller can prove the two shapes select identically.
static int probe_shape(sqlite3 *db, const char *sql, int64_t until, const void *site_id, int site_len,
                       const resume_point *points, int n, double *out, resume_point *rows) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { fprintf(stderr, "prepare failed: %s\n", sqlite3_errmsg(db)); return rc; }
    for (int i = 0; i < n; ++i) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, until);
        sqlite3_bind_blob(stmt, 2, site_id, site_len, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, points[i].dbv);
        sqlite3_bind_int64(stmt, 4, points[i].dbv);
        sqlite3_bind_int64(stmt, 5, points[i].seq);
        sqlite3_bind_int64(stmt, 6, points[i].dbv); // SQLITE_RANGE where unused
        double t0 = monotonic_ms();
        rc = sqlite3_step(stmt);
        out[i] = monotonic_ms() - t0;
        if (rc == SQLITE_ROW) {
            rows[i].dbv = sqlite3_column_int64(stmt, 0);
            rows[i].seq = sqlite3_column_int64(stmt, 1);
        } else {
            rows[i].dbv = -1; rows[i].seq = -1;
        }
    }
    sqlite3_finalize(stmt);
    return SQLITE_OK;
}

int main(void) {
    int rows = env_int("CHUNK_BENCH_ROWS", 3000);
    int row_bytes = env_int("CHUNK_BENCH_ROW_BYTES", 8000);
    int repeats = env_int("CHUNK_BENCH_REPEATS", 3);
    int chunk_size = env_int("CHUNK_BENCH_CHUNK_SIZE", 262144);
    int txns = env_int("CHUNK_BENCH_TXNS", rows);
    bool verbose = env_flag("CHUNK_BENCH_VERBOSE", false);
    // A large window costs minutes to seed and the unfixed arm re-reads it on every
    // call, so allow one seeded database to be reused across both builds and the
    // quadratic Phase 2 to be skipped.
    const char *db_path = env_str("CHUNK_BENCH_DB", DB_PATH);
    const char *ext_path = env_str("CHUNK_BENCH_EXT", EXT_PATH);
    bool keep = env_flag("CHUNK_BENCH_KEEP", false);
    bool reuse = env_flag("CHUNK_BENCH_REUSE", false) && file_exists(db_path);
    bool want_phase2 = env_flag("CHUNK_BENCH_PHASE2", true);
    if (txns > rows) txns = rows;

    if (!reuse) remove(db_path);
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) { fprintf(stderr, "open failed\n"); return 1; }
    if (sqlite3_enable_load_extension(db, 1) != SQLITE_OK) return 1;
    char load[512];
    snprintf(load, sizeof(load), "SELECT load_extension('%s');", ext_path);
    if (db_exec(db, load) != SQLITE_OK) return 1;
    printf("extension: %s   database: %s%s\n", ext_path, db_path, reuse ? " (reused)" : "");

    char setup[256];
    snprintf(setup, sizeof(setup), "SELECT cloudsync_set('payload_max_chunk_size', '%d');", chunk_size);
    if (!reuse) {
        char create[256];
        snprintf(create, sizeof(create),
            "PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF;"
            "CREATE TABLE chunk_bench (id TEXT PRIMARY KEY, body BLOB);"
            "SELECT cloudsync_init('chunk_bench');");
        if (db_exec(db, create) != SQLITE_OK) return 1;
    }
    if (db_exec(db, setup) != SQLITE_OK) return 1;

    // Each transaction is one db_version, so TXNS controls how many distinct
    // db_versions the window spans. That is the axis the proposed lower bound acts
    // on: with TXNS=1 every row shares one db_version and no bound on db_version can
    // narrow anything. Incompressible bodies keep the window many-chunked.
    if (!reuse) {
        printf("seeding %d rows of %d bytes across %d transaction(s)...\n", rows, row_bytes, txns);
        double seed_t0 = monotonic_ms();
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
        printf("seeded in %.1f s\n", (monotonic_ms() - seed_t0) / 1000.0);
    }

    double *per_chunk = calloc(MAX_POINTS, sizeof(double));
    resume_point *points = calloc(MAX_POINTS, sizeof(resume_point));
    if (!per_chunk || !points) { fprintf(stderr, "oom\n"); return 1; }

    // ---- Phase 1: the real drain, one chunk per call ----
    int chunks = 0;
    long long bytes = 0;
    int64_t until = 0;
    double best = 1e18, sum = 0;
    for (int r = 0; r < repeats; ++r) {
        double t0 = monotonic_ms();
        if (drain_positional(db, per_chunk, points, &chunks, &bytes, &until) != SQLITE_OK) {
            fprintf(stderr, "positional drain failed: %s\n", sqlite3_errmsg(db));
            return 1;
        }
        double dt = monotonic_ms() - t0;
        sum += dt; if (dt < best) best = dt;
    }
    int n = chunks < MAX_POINTS ? chunks : MAX_POINTS;

    printf("\n=== Positional /check drain (local SQLite, no network) ===\n");
    printf("rows: %d   row_bytes: %d   txns: %d   chunk_size: %d   repeats: %d\n",
           rows, row_bytes, txns, chunk_size, repeats);
    printf("chunks: %d   payload_bytes: %lld   until_db_version: %lld\n", chunks, bytes, (long long)until);
    printf("drain: best=%.2f ms   mean=%.2f ms   per-chunk=%.3f ms\n",
           best, sum / repeats, chunks ? best / chunks : 0.0);
    if (n > 0) report_curve("\nPhase 1 - cloudsync_payload_chunks latency vs chunk index", per_chunk, n);
    if (verbose) for (int i = 0; i < n; ++i) printf("  chunk %5d  %8.3f ms\n", i, per_chunk[i]);

    // ---- Phase 2: the same resume points, straight at cloudsync_changes ----
    // points[0] is unset (the first chunk carries no resume point), so start at 1.
    int np = (want_phase2 && n > 1) ? n - 1 : 0;
    if (np > 0) {
        // Exclude a site_id that cannot exist, so the filter matches the real
        // query's shape without removing any row.
        static const unsigned char absent_site[16] = {0};
        static const char *shape_sql[3] = { SHAPE_CURRENT, SHAPE_PROPOSED, SHAPE_REUSED_PARAM };
        static const char *shape_name[3] = {
            "2a - current  (disjunction, distinct parameters)",
            "2b - proposed (+ redundant AND db_version>=?)",
            "2c - variant  (one parameter reused in both arms)",
        };
        static const char *shape_tag[3] = { "current:", "proposed:", "reused:" };
        double *t[3];
        resume_point *sel[3];
        for (int s = 0; s < 3; ++s) {
            t[s] = calloc(np, sizeof(double));
            sel[s] = calloc(np, sizeof(resume_point));
            if (!t[s] || !sel[s]) { fprintf(stderr, "oom\n"); return 1; }
        }

        printf("\n=== Resume seek against cloudsync_changes, %d points ===\n", np);
        printf("clause reaching xBestIndex (a missing db_version >= ? is the replay):\n");
        for (int s = 0; s < 3; ++s) print_plan(db, shape_tag[s], shape_sql[s]);

        for (int s = 0; s < 3; ++s)
            if (probe_shape(db, shape_sql[s], until, absent_site, (int)sizeof(absent_site),
                            points + 1, np, t[s], sel[s]) != SQLITE_OK) return 1;

        for (int s = 0; s < 3; ++s) {
            char title[128];
            snprintf(title, sizeof(title), "\nPhase %s", shape_name[s]);
            report_curve(title, t[s], np);
        }

        printf("\ntotal seek time:");
        for (int s = 0; s < 3; ++s) {
            double total = 0;
            for (int i = 0; i < np; ++i) total += t[s][i];
            printf("  %s %.2f ms", shape_tag[s], total);
        }
        printf("\n");

        // The point of the redundant term is that it is logically implied, so every
        // shape must select the identical row at every resume point.
        for (int s = 1; s < 3; ++s) {
            int mismatches = 0;
            for (int i = 0; i < np; ++i)
                if (sel[0][i].dbv != sel[s][i].dbv || sel[0][i].seq != sel[s][i].seq) mismatches++;
            printf("rows selected by %-9s differ from current at %d of %d resume points\n",
                   shape_tag[s], mismatches, np);
        }

        for (int s = 0; s < 3; ++s) { free(t[s]); free(sel[s]); }
    }

    free(per_chunk); free(points);
    sqlite3_close(db);
    if (!keep) remove(db_path);
    return 0;
}
