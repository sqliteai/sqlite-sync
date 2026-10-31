// Targeted fuzzing of payload apply: v3 value fragments and block-level merge.
//
// Seeds are real payloads (fragment chunks of a large value, block-column edits). Each
// iteration mutates the *decoded body* of one or more seeds, repairs the header checksum so
// the mutation reaches the row decoder and the merge instead of being stopped by the
// checksum, and applies the result to a database that already holds local rows. Errors are
// expected and ignored; the point is to run under AddressSanitizer / UndefinedBehavior-
// Sanitizer / MemorySanitizer and to see no report, crash or hang.
//
//   usage: fuzz_apply [iterations] [seed]
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "sqlite3.h"
#include "cloudsync.h"
#include "cloudsync_sqlite.h"
#include "lz4.h"
#include "pk.h"
#include <pthread.h>

#define MAX_SEEDS 64
#define HEADER_SIZE 32

static uint8_t *seed_data[MAX_SEEDS];
static int seed_len[MAX_SEEDS];
static int nseeds;
static __thread uint64_t rng = 88172645463325252ULL;

static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 11);
}

static sqlite3 *open_db(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK || sqlite3_cloudsync_init(db, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "cannot open database\n");
        exit(2);
    }
    return db;
}

static int sql(sqlite3 *db, const char *q) { return sqlite3_exec(db, q, NULL, NULL, NULL); }

static const char *schema =
    "CREATE TABLE f(id TEXT PRIMARY KEY NOT NULL, v BLOB);"
    "CREATE TABLE d(id TEXT PRIMARY KEY NOT NULL, body TEXT, n INTEGER);"
    "SELECT cloudsync_init('f');"
    "SELECT cloudsync_init('d');"
    "SELECT cloudsync_set_column('d','body','algo','block');";

static void add_seed(const void *p, int n) {
    if (nseeds >= MAX_SEEDS || n <= 0) return;
    seed_data[nseeds] = malloc((size_t)n);
    memcpy(seed_data[nseeds], p, (size_t)n);
    seed_len[nseeds++] = n;
}

static void collect(sqlite3 *db, const char *query) {
    sqlite3_stmt *vm = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &vm, NULL) != SQLITE_OK) { fprintf(stderr, "seed query failed: %s\n", sqlite3_errmsg(db)); exit(2); }
    while (sqlite3_step(vm) == SQLITE_ROW) {
        if (sqlite3_column_type(vm, 0) == SQLITE_BLOB) add_seed(sqlite3_column_blob(vm, 0), sqlite3_column_bytes(vm, 0));
    }
    sqlite3_finalize(vm);
}

static void build_seeds(void) {
    sqlite3 *src = open_db();
    if (sql(src, schema) != SQLITE_OK) exit(2);
    sql(src, "SELECT cloudsync_set('payload_max_chunk_size','262144');");
    sql(src, "INSERT INTO f VALUES('big', randomblob(700000)), ('small', randomblob(100));");
    sql(src, "INSERT INTO d VALUES('doc1', 'alpha'||char(10)||'beta'||char(10)||'gamma'||char(10)||'delta', 1);");
    sql(src, "INSERT INTO d VALUES('doc2', 'one'||char(10)||'two', 2);");
    collect(src, "SELECT payload FROM cloudsync_payload_chunks() ORDER BY chunk_index");
    sql(src, "UPDATE d SET body = 'alpha'||char(10)||'BETA'||char(10)||'gamma'||char(10)||'epsilon'||char(10)||'delta' WHERE id='doc1'");
    sql(src, "UPDATE d SET body = 'one' WHERE id='doc2'");
    sql(src, "DELETE FROM d WHERE id='doc2'");
    sql(src, "UPDATE f SET v = randomblob(300000) WHERE id='big'");
    collect(src, "SELECT payload FROM cloudsync_payload_chunks() ORDER BY chunk_index");
    collect(src, "SELECT cloudsync_payload_encode(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq) FROM cloudsync_changes");
    sql(src, "SELECT cloudsync_terminate()");
    sqlite3_close(src);
    if (nseeds < 4) { fprintf(stderr, "too few seeds (%d)\n", nseeds); exit(2); }
}

static void store_checksum(uint8_t *header, uint64_t sum) {
    for (int i = 0; i < 6; i++) header[26 + i] = (uint8_t)(sum >> (8 * (5 - i)));
}

// Returns a malloc'ed mutated payload and its length.
static uint8_t *mutate(const uint8_t *in, int n, int *out_len) {
    uint8_t header[HEADER_SIZE];
    uint8_t *body;
    size_t blen;
    if (n < HEADER_SIZE) {
        body = malloc((size_t)n + 1); memcpy(body, in, (size_t)n); blen = (size_t)n;
        memset(header, 0, HEADER_SIZE);
    } else {
        memcpy(header, in, HEADER_SIZE);
        uint32_t expanded = ((uint32_t)header[8] << 24) | ((uint32_t)header[9] << 16) | ((uint32_t)header[10] << 8) | header[11];
        size_t compressed = (size_t)n - HEADER_SIZE;
        if (expanded) {
            body = malloc(expanded + 1);
            int got = LZ4_decompress_safe((const char *)in + HEADER_SIZE, (char *)body, (int)compressed, (int)expanded);
            blen = got > 0 ? (size_t)got : 0;
        } else {
            body = malloc(compressed + 1); memcpy(body, in + HEADER_SIZE, compressed); blen = compressed;
        }
    }
    int ops = 1 + (int)(rnd() % 4);
    for (int i = 0; i < ops && blen > 0; i++) {
        switch (rnd() % 8) {
            case 0: body[rnd() % blen] ^= (uint8_t)(1u << (rnd() % 8)); break;
            case 1: body[rnd() % blen] = (uint8_t)rnd(); break;
            case 2: { static const uint8_t special[] = {0x00, 0x01, 0x1F, 0x7F, 0x80, 0xFF, 0x08, 0x40, 0x48};
                      body[rnd() % blen] = special[rnd() % sizeof(special)]; } break;
            case 3: { size_t at = rnd() % blen, k = 1 + rnd() % 16; if (at + k > blen) k = blen - at;
                      memmove(body + at, body + at + k, blen - at - k); blen -= k; } break;
            case 4: { size_t at = rnd() % (blen + 1), k = 1 + rnd() % 8; uint8_t *grown = realloc(body, blen + k + 1);
                      if (!grown) break; body = grown; memmove(body + at + k, body + at, blen - at);
                      for (size_t j = 0; j < k; j++) body[at + j] = (uint8_t)rnd(); blen += k; } break;
            case 5: blen = rnd() % (blen + 1); break;
            case 6: { size_t a = rnd() % blen, b = rnd() % blen, k = 1 + rnd() % 8;
                      if (a + k <= blen && b + k <= blen) { uint8_t t[8]; memcpy(t, body + a, k); memmove(body + a, body + b, k); memcpy(body + b, t, k); } } break;
            case 7: { size_t at = rnd() % blen; if (at + 8 <= blen) memset(body + at, rnd() % 2 ? 0xFF : 0x00, 8); } break;
        }
    }
    uint8_t *out = malloc(HEADER_SIZE + blen + 1);
    memcpy(out, header, HEADER_SIZE);
    memcpy(out + HEADER_SIZE, body, blen);
    // uncompressed: the mutated body is what the decoder sees
    out[8] = out[9] = out[10] = out[11] = 0;
    if (rnd() % 10 == 0) { uint32_t nrows = rnd() % 5; out[16] = (uint8_t)(nrows >> 24); out[17] = (uint8_t)(nrows >> 16); out[18] = (uint8_t)(nrows >> 8); out[19] = (uint8_t)nrows; }
    if (rnd() % 20 == 0) out[4] = (uint8_t)(1 + rnd() % 4);
    if (rnd() % 20 != 0) store_checksum(out, pk_checksum((const char *)body, blen));
    free(body);
    *out_len = (int)(HEADER_SIZE + blen);
    return out;
}

static int apply(sqlite3 *db, const uint8_t *p, int n) {
    sqlite3_stmt *vm = NULL;
    if (sqlite3_prepare_v2(db, "SELECT cloudsync_payload_apply(?1)", -1, &vm, NULL) != SQLITE_OK) return SQLITE_ERROR;
    sqlite3_bind_blob(vm, 1, p, n, SQLITE_STATIC);
    int rc = sqlite3_step(vm);
    sqlite3_finalize(vm);
    return rc;
}

typedef struct { int iterations; uint64_t seed; int ok, failed; } worker_t;

static void *worker(void *arg) {
    worker_t *w = (worker_t *)arg;
    rng = 88172645463325252ULL ^ (w->seed * 0x9E3779B97F4A7C15ULL);
    if (rng == 0) rng = 1;
    for (int it = 0; it < w->iterations; it++) {
        sqlite3 *db = open_db();
        sql(db, schema);
        // local state to merge with: same documents, different content
        sql(db, "INSERT INTO d VALUES('doc1', 'alpha'||char(10)||'local'||char(10)||'delta', 9);");
        sql(db, "INSERT INTO f VALUES('big', randomblob(1000));");
        int mutate_count = 1 + (int)(rnd() % 3);
        int hits[3] = {-1, -1, -1};
        for (int m = 0; m < mutate_count; m++) hits[m] = (int)(rnd() % nseeds);
        for (int i = 0; i < nseeds; i++) {
            int do_mutate = 0;
            for (int m = 0; m < mutate_count; m++) if (hits[m] == i) do_mutate = 1;
            if (do_mutate) {
                int n = 0;
                uint8_t *p = mutate(seed_data[i], seed_len[i], &n);
                int rc = apply(db, p, n);
                if (rc == SQLITE_ROW || rc == SQLITE_DONE) w->ok++; else w->failed++;
                free(p);
            } else {
                apply(db, seed_data[i], seed_len[i]);
            }
        }
        // read side and local edits over whatever state is left
        sql(db, "SELECT count(*) FROM cloudsync_changes");
        sql(db, "SELECT count(*), sum(length(payload)) FROM cloudsync_payload_chunks()");
        sql(db, "UPDATE d SET body = 'x'||char(10)||coalesce(body,'') WHERE id='doc1'");
        sql(db, "SELECT cloudsync_terminate()");
        sqlite3_close(db);
    }
    return NULL;
}

int main(int argc, char **argv) {
    int iterations = argc > 1 ? atoi(argv[1]) : 2000;
    uint64_t seed = argc > 2 ? (uint64_t)strtoull(argv[2], NULL, 10) : 1;
    int threads = argc > 3 ? atoi(argv[3]) : 1;
    if (threads < 1) threads = 1;
    if (threads > 16) threads = 16;
    build_seeds();
    printf("seeds: %d, threads: %d\n", nseeds, threads);

    // one connection per thread, all in one process: shared globals would show here
    pthread_t tid[16];
    worker_t w[16];
    for (int t = 0; t < threads; t++) {
        w[t] = (worker_t){iterations, seed + (uint64_t)t * 7919, 0, 0};
        if (threads == 1) worker(&w[t]);
        else pthread_create(&tid[t], NULL, worker, &w[t]);
    }
    int ok = 0, failed = 0;
    for (int t = 0; t < threads; t++) {
        if (threads > 1) pthread_join(tid[t], NULL);
        ok += w[t].ok; failed += w[t].failed;
    }
    printf("iterations: %d x %d, mutated applied ok: %d, rejected: %d\n", iterations, threads, ok, failed);
    return 0;
}
