//
//  network_unit.c
//  cloudsync
//
//  Unit tests for the network layer's pure response-handling logic. Built with
//  networking ENABLED (unlike dist/unit, which is -DCLOUDSYNC_OMIT_NETWORK), so it
//  can call the internal functions directly on crafted in-memory NETWORK_RESULT
//  buffers. The deadline regression also uses a stalled loopback HTTP socket;
//  no external server is contacted.
//

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include "utils.h"
#include "network_private.h"

static int failures = 0;

static void check(const char *name, bool ok) {
    printf("%-64s %s\n", name, ok ? "OK" : "FAIL");
    if (!ok) failures++;
}

static NETWORK_RESULT json_buffer(char *json) {
    NETWORK_RESULT r = {0};
    r.code = CLOUDSYNC_NETWORK_BUFFER;
    r.buffer = json;
    r.blen = strlen(json);
    return r;
}

// Regression: lastOptimisticVersion must track the LATEST valid value, including a
// decrease. The server can roll the optimistic version back when a later send chunk
// fails; since it becomes the durable send checkpoint, a monotonic "max" would mask
// the rollback and skip the rolled-back changes on the next send.
static bool test_optimistic_version_rollback(void) {
    int64_t optimistic = -1, confirmed = -1;
    int gaps = -1;
    char *apply = NULL, *check_fail = NULL;
    bool ok = true;

    char j1[] = "{\"lastOptimisticVersion\":50,\"lastConfirmedVersion\":10}";
    NETWORK_RESULT r1 = json_buffer(j1);
    network_sync_state_update_from_response(&r1, &optimistic, &confirmed, &gaps, &apply, &check_fail);
    ok = ok && optimistic == 50 && confirmed == 10;

    char j2[] = "{\"lastOptimisticVersion\":100,\"lastConfirmedVersion\":20}";
    NETWORK_RESULT r2 = json_buffer(j2);
    network_sync_state_update_from_response(&r2, &optimistic, &confirmed, &gaps, &apply, &check_fail);
    ok = ok && optimistic == 100 && confirmed == 20;

    // Server rolls back on a later chunk error: the value must DECREASE to 50.
    char j3[] = "{\"lastOptimisticVersion\":50,\"lastConfirmedVersion\":20}";
    NETWORK_RESULT r3 = json_buffer(j3);
    network_sync_state_update_from_response(&r3, &optimistic, &confirmed, &gaps, &apply, &check_fail);
    ok = ok && optimistic == 50;

    // A response missing the field (parsed -1) must NOT clobber the current value.
    char j4[] = "{\"lastConfirmedVersion\":20}";
    NETWORK_RESULT r4 = json_buffer(j4);
    network_sync_state_update_from_response(&r4, &optimistic, &confirmed, &gaps, &apply, &check_fail);
    ok = ok && optimistic == 50;

    ok = ok && apply == NULL && check_fail == NULL; // no failures object in these responses
    return ok;
}

// A non-BUFFER result (or NULL buffer) must leave the accumulators untouched.
static bool test_non_buffer_is_noop(void) {
    int64_t optimistic = 7, confirmed = 3;
    int gaps = 0;
    char *apply = NULL, *check_fail = NULL;

    NETWORK_RESULT err = {0};
    err.code = CLOUDSYNC_NETWORK_ERROR;
    network_sync_state_update_from_response(&err, &optimistic, &confirmed, &gaps, &apply, &check_fail);
    return optimistic == 7 && confirmed == 3 && gaps == 0;
}

// One send call = one all-or-nothing batch: every chunk of it announces the same
// global window [checkpoint+1 .. watermark] plus batchId/chunkIndex/isFinal, so the
// server confirms the whole window only when every chunk of the batch applied and a
// failed batch is re-sent whole under a new id.
static bool test_apply_json_payload_batch(void) {
    bool ok = true;

    char *j0 = network_apply_json_payload("blob", "QUJD", 1, 10, "batch-uuid-1", 0, false);
    ok = ok && j0 != NULL;
    if (j0) {
        ok = ok && strstr(j0, "\"blob\":\"QUJD\"") != NULL;
        ok = ok && strstr(j0, "\"dbVersionMin\":1") != NULL;
        ok = ok && strstr(j0, "\"dbVersionMax\":10") != NULL;
        ok = ok && strstr(j0, "\"batchId\":\"batch-uuid-1\"") != NULL;
        ok = ok && strstr(j0, "\"chunkIndex\":0") != NULL;
        ok = ok && strstr(j0, "\"isFinal\":false") != NULL;
        cloudsync_memory_free(j0);
    }

    // last chunk, url transport: same window, higher index, isFinal true
    char *j1 = network_apply_json_payload("url", "https://s3/part", 1, 10, "batch-uuid-1", 3, true);
    ok = ok && j1 != NULL;
    if (j1) {
        ok = ok && strstr(j1, "\"url\":\"https://s3/part\"") != NULL;
        ok = ok && strstr(j1, "\"dbVersionMin\":1") != NULL;
        ok = ok && strstr(j1, "\"dbVersionMax\":10") != NULL;
        ok = ok && strstr(j1, "\"batchId\":\"batch-uuid-1\"") != NULL;
        ok = ok && strstr(j1, "\"chunkIndex\":3") != NULL;
        ok = ok && strstr(j1, "\"isFinal\":true") != NULL;
        cloudsync_memory_free(j1);
    }

    // a batch id is mandatory for the chunked send path
    ok = ok && network_apply_json_payload("blob", "QUJD", 1, 10, NULL, 0, false) == NULL;

    return ok;
}

static bool test_compute_status(void) {
    bool ok = true;
    ok = ok && strcmp(network_compute_status(100, 100, 0, 100), "synced") == 0;
    ok = ok && strcmp(network_compute_status(100, 50, 0, 100), "syncing") == 0;
    ok = ok && strcmp(network_compute_status(100, 100, 1, 100), "out-of-sync") == 0;  // gaps
    ok = ok && strcmp(network_compute_status(90, 90, 0, 100), "out-of-sync") == 0;     // behind local
    ok = ok && strcmp(network_compute_status(-1, 100, 0, 100), "error") == 0;          // unparsed
    return ok;
}

extern char *network_test_unescape(const char *);
extern char *network_test_extract_string(const char *, const char *);
static bool test_json_scope(void) {
    char json[] = "{\"noise\":\"lastOptimisticVersion\",\"nested\":{\"lastConfirmedVersion\":999},\"lastOptimisticVersion\":42,\"lastConfirmedVersion\":7}";
    NETWORK_RESULT r = json_buffer(json);
    int64_t optimistic = -1, confirmed = -1;
    int gaps = -1;
    char *apply = NULL, *check_failure = NULL;
    network_sync_state_update_from_response(&r, &optimistic, &confirmed, &gaps, &apply, &check_failure);
    bool ok = optimistic == 42 && confirmed == 7;
    cloudsync_memory_free(apply);
    cloudsync_memory_free(check_failure);
    char nested[] = "{\"nested\":{\"lastOptimisticVersion\":999,\"lastConfirmedVersion\":999}}";
    r = json_buffer(nested);
    apply = check_failure = NULL;
    network_sync_state_update_from_response(&r, &optimistic, &confirmed, &gaps, &apply, &check_failure);
    ok = ok && optimistic == 42 && confirmed == 7;
    cloudsync_memory_free(apply);
    cloudsync_memory_free(check_failure);
    return ok;
}

// Gateway success responses wrap the payload in {"data": ...} (API.md, "Success
// envelope"); legacy servers do not. Key lookups stay scoped to one object, so
// readers of a raw response body must unwrap first.
static bool test_json_envelope(void) {
    char enveloped[] = "{\"data\":{\"nested\":{\"lastOptimisticVersion\":999},"
                       "\"lastOptimisticVersion\":15,\"lastConfirmedVersion\":12}}";
    NETWORK_RESULT r = json_buffer(enveloped);
    int64_t optimistic = -1, confirmed = -1;
    int gaps = -1;
    char *apply = NULL, *check_failure = NULL;
    network_sync_state_update_from_response(&r, &optimistic, &confirmed, &gaps, &apply, &check_failure);
    bool ok = optimistic == 15 && confirmed == 12;
    cloudsync_memory_free(apply);
    cloudsync_memory_free(check_failure);

    // the enveloped 202 status payload also carries gaps and failures
    char full[] = "{\"data\":{\"lastOptimisticVersion\":20,\"lastConfirmedVersion\":18,"
                  "\"gaps\":[{\"dbVersionMin\":13,\"dbVersionMax\":15}],"
                  "\"failures\":{\"apply\":null,\"check\":{\"code\":\"boom\",\"retryable\":false}}}}";
    r = json_buffer(full);
    apply = check_failure = NULL;
    network_sync_state_update_from_response(&r, &optimistic, &confirmed, &gaps, &apply, &check_failure);
    ok = ok && optimistic == 20 && confirmed == 18 && gaps == 1;
    ok = ok && check_failure && strstr(check_failure, "boom");
    cloudsync_memory_free(apply);
    cloudsync_memory_free(check_failure);

    // a legacy un-enveloped body still parses
    char legacy[] = "{\"lastOptimisticVersion\":7,\"lastConfirmedVersion\":5}";
    r = json_buffer(legacy);
    apply = check_failure = NULL;
    network_sync_state_update_from_response(&r, &optimistic, &confirmed, &gaps, &apply, &check_failure);
    ok = ok && optimistic == 7 && confirmed == 5;
    cloudsync_memory_free(apply);
    cloudsync_memory_free(check_failure);

    // key lookups remain scoped to one object: an enveloped url is not visible
    // to a root-scoped read, which is why raw-response readers unwrap first
    char *url = network_test_extract_string("{\"data\":{\"url\":\"https://s3/a\"}}", "url");
    ok = ok && url == NULL;
    cloudsync_memory_free(url);

    // an un-enveloped chunk object sliced out of chunks[] resolves directly
    url = network_test_extract_string("{\"cursor\":0,\"url\":\"https://s3/b\",\"watermark\":18}", "url");
    ok = ok && url && strcmp(url, "https://s3/b") == 0;
    cloudsync_memory_free(url);
    return ok;
}
extern char *network_test_receive_json(int, bool, const char *, const char *);
static bool receive_json_is(char *json, const char *expected) {
    bool ok = json && strcmp(json, expected) == 0;
    if (!ok) printf("\n    got:      %s\n    expected: %s\n", json ? json : "(null)", expected);
    cloudsync_memory_free(json);
    return ok;
}
static bool test_receive_json(void) {
    bool ok = receive_json_is(network_test_receive_json(3, true, NULL, NULL),
        "\"receive\":{\"rows\":3,\"tables\":[\"t\"],\"chunks\":1,\"bytes\":10,\"complete\":true}");
    // a failed receive still reports the changes applied before the error
    ok = receive_json_is(network_test_receive_json(2, false, "rejected \"here\"", "{\"code\":\"x\"}"),
        "\"receive\":{\"rows\":2,\"tables\":[\"t\"],\"chunks\":1,\"bytes\":10,\"complete\":false,\"error\":\"rejected \\\"here\\\"\",\"lastFailure\":{\"code\":\"x\"}}") && ok;
    return ok;
}
static bool test_unicode(void) {
    char *s = network_test_unescape("caf\\u00e9 \\u20ac \\ud83d\\ude80 \\/\\n");
    bool ok = s && strcmp(s, "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x9a\x80 /\n") == 0;
    cloudsync_memory_free(s);
    const char *invalid[] = {"\\ud800", "\\udc00", "\\ud800\\u0041", "\\u0000", "\\uZZZZ", "\\u123", "\\"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        s = network_test_unescape(invalid[i]);
        ok = ok && s == NULL;
        cloudsync_memory_free(s);
    }
    return ok;
}

#if !defined(_WIN32) && !defined(CLOUDSYNC_OMIT_CURL)
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "sqlite3.h"
extern bool network_test_curl_timeout(const char *, bool, bool);
extern bool network_test_curl_interrupt(const char *, sqlite3 *);
static bool test_stalled_http_timeout(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0 && listen(fd, 8) == 0;
    socklen_t len = sizeof(address);
    ok = ok && getsockname(fd, (struct sockaddr *)&address, &len) == 0;
    char url[80];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/", ntohs(address.sin_port));
    // A listening socket that never sends HTTP simulates a stalled server.
    // An API endpoint is bounded by elapsed time; an artifact transfer by a stall.
    // Both shapes must abort against a server that accepts and then sends nothing.
    if (ok) ok = network_test_curl_timeout(url, false, true) && network_test_curl_timeout(url, true, true);
    if (ok) ok = network_test_curl_timeout(url, false, false) && network_test_curl_timeout(url, true, false);
    // sqlite3_interrupt() cancels a transfer in flight: with no statement running the
    // flag stays set, so the transfer sees it from its first progress callback.
    sqlite3 *db = NULL;
    if (ok) ok = sqlite3_open(":memory:", &db) == SQLITE_OK;
    if (ok) { sqlite3_interrupt(db); ok = network_test_curl_interrupt(url, db); }
    if (db) sqlite3_close(db);
    close(fd);
    return ok;
}
#endif


// MARK: - Receive stream (canned /check responses, real apply)

#include <stdlib.h>
#include "sqlite3.h"
#include "cloudsync.h"
#include "cloudsync_sqlite.h"
extern void network_test_set_responder(NETWORK_RESULT (*)(const char *, const char *));
extern char *network_test_base64_encode(const unsigned char *, size_t);

// The server spool of one receive window: its pages, and the watermark every chunk
// announces. Each request records the page cursor and the dbVersion it was sent with.
#define SPOOL_MAX 16
static const char *spool[SPOOL_MAX];
static int spool_pages;
static int64_t spool_watermark;   // < 0: chunks carry no watermark (an older server)
static int64_t req_cursor[64], req_since[64];
static int nreq;

static int64_t json_int_after(const char *json, const char *key, int64_t fallback) {
    const char *p = strstr(json, key);
    return p ? strtoll(p + strlen(key), NULL, 10) : fallback;
}

static NETWORK_RESULT spool_responder(const char *endpoint, const char *request) {
    NETWORK_RESULT r = {0};
    size_t n = endpoint ? strlen(endpoint) : 0;
    if (n < 6 || strcmp(endpoint + n - 6, "/check") != 0 || !request) { r.code = CLOUDSYNC_NETWORK_ERROR; return r; }
    int64_t cursor = json_int_after(request, "\"cursor\":", 0);
    if (nreq < 64) { req_cursor[nreq] = cursor; req_since[nreq] = json_int_after(request, "\"dbVersion\":", -1); nreq++; }
    int64_t max = json_int_after(request, "\"maxChunks\":", 1);
    size_t cap = 256;
    for (int i = 0; i < spool_pages; i++) cap += strlen(spool[i]) + 96;
    char *json = cloudsync_memory_alloc(cap);
    size_t len = (size_t)snprintf(json, cap, "{\"data\":{\"chunks\":[");
    int64_t k = cursor;
    for (; k < spool_pages && k < cursor + max; k++) {
        char watermark[48] = "";
        if (spool_watermark >= 0) snprintf(watermark, sizeof(watermark), ",\"watermark\":%lld", (long long)spool_watermark);
        len += (size_t)snprintf(json + len, cap - len, "%s{\"cursor\":%lld,\"payload\":\"%s\"%s}",
                                k > cursor ? "," : "", (long long)k, spool[k], watermark);
    }
    bool final = k >= spool_pages;
    snprintf(json + len, cap - len, "],\"final\":%s,\"nextCursor\":%lld}}", final ? "true" : "false", (long long)(final ? -1 : k));
    r.code = CLOUDSYNC_NETWORK_BUFFER;
    r.buffer = json;
    r.blen = strlen(json);
    return r;
}

static int db_exec(sqlite3 *db, const char *sql) { return sqlite3_exec(db, sql, NULL, NULL, NULL); }

static int64_t db_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *vm = NULL;
    int64_t v = INT64_MIN;
    if (sqlite3_prepare_v2(db, sql, -1, &vm, NULL) == SQLITE_OK && sqlite3_step(vm) == SQLITE_ROW) v = sqlite3_column_int64(vm, 0);
    sqlite3_finalize(vm);
    return v;
}

static int64_t db_checkpoint(sqlite3 *db) {
    return db_int(db, "SELECT coalesce((SELECT value FROM cloudsync_settings WHERE key='check_dbversion'),0)");
}

static sqlite3 *stream_db(bool network) {
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK || sqlite3_cloudsync_init(db, NULL, NULL) != SQLITE_OK) return NULL;
    if (db_exec(db, "CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL, v BLOB); SELECT cloudsync_init('t');") != SQLITE_OK) return NULL;
    if (network && db_exec(db, "SELECT cloudsync_network_init('test-managed-database-id');") != SQLITE_OK) return NULL;
    return db;
}

static void stream_close(sqlite3 *db) {
    if (!db) return;
    db_exec(db, "SELECT cloudsync_terminate();");
    sqlite3_close(db);
}

// Base64 payloads of every row of src matching where (one monolithic payload each),
// or of the chunks cloudsync_payload_chunks produces. The caller frees them.
static int stream_payloads(sqlite3 *src, const char *query, char **out, int max) {
    sqlite3_stmt *vm = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(src, query, -1, &vm, NULL) != SQLITE_OK) return 0;
    while (n < max && sqlite3_step(vm) == SQLITE_ROW) {
        out[n++] = network_test_base64_encode(sqlite3_column_blob(vm, 0), (size_t)sqlite3_column_bytes(vm, 0));
    }
    sqlite3_finalize(vm);
    return n;
}

// Applies every payload the query returns on src directly to dst, as a SQL caller would.
static bool direct_apply(sqlite3 *dst, sqlite3 *src, const char *query) {
    sqlite3_stmt *read = NULL, *write = NULL;
    bool ok = sqlite3_prepare_v2(src, query, -1, &read, NULL) == SQLITE_OK &&
              sqlite3_prepare_v2(dst, "SELECT cloudsync_payload_apply(?1)", -1, &write, NULL) == SQLITE_OK;
    while (ok && sqlite3_step(read) == SQLITE_ROW) {
        sqlite3_bind_value(write, 1, sqlite3_column_value(read, 0));
        ok = sqlite3_step(write) == SQLITE_ROW;
        sqlite3_reset(write);
    }
    sqlite3_finalize(read);
    sqlite3_finalize(write);
    return ok;
}

// Runs cloudsync_network_receive_changes(max_chunks); returns its JSON or "ERROR: ...".
static char *stream_receive(sqlite3 *db, int max_chunks) {
    static char result[1024];
    sqlite3_stmt *vm = NULL;
    snprintf(result, sizeof(result), "ERROR: prepare");
    if (sqlite3_prepare_v2(db, "SELECT cloudsync_network_receive_changes(?1)", -1, &vm, NULL) != SQLITE_OK) return result;
    sqlite3_bind_int(vm, 1, max_chunks);
    if (sqlite3_step(vm) == SQLITE_ROW) snprintf(result, sizeof(result), "%s", (const char *)sqlite3_column_text(vm, 0));
    else snprintf(result, sizeof(result), "ERROR: %s", sqlite3_errmsg(db));
    sqlite3_finalize(vm);
    return result;
}

static bool expect(bool ok, const char *what, const char *detail) {
    if (!ok) printf("\n    %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
    return ok;
}

// Three monolithic pages. A call capped by max_chunks keeps the same window (dbVersion)
// and asks for the next page; the checkpoint moves only when the final page applied.
// A failed page restarts the next call from page 0, keeping the rows applied before it.
static bool test_stream_paging(void) {
    bool ok = true;
    sqlite3 *src = stream_db(false), *dst = stream_db(true);
    char *pages[SPOOL_MAX] = {0};
    ok = ok && src && dst;
    ok = ok && db_exec(src, "INSERT INTO t VALUES('k1',x'01'); INSERT INTO t VALUES('k2',x'02'); INSERT INTO t VALUES('k3',x'03');") == SQLITE_OK;
    int n = ok ? stream_payloads(src, "SELECT cloudsync_payload_encode(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq) FROM cloudsync_changes GROUP BY pk ORDER BY pk", pages, SPOOL_MAX) : 0;
    ok = ok && expect(n == 3, "three pages", NULL);
    for (int i = 0; i < n; i++) spool[i] = pages[i];
    spool_pages = n;
    spool_watermark = ok ? db_int(src, "SELECT max(db_version) FROM cloudsync_changes") : 0;
    nreq = 0;
    network_test_set_responder(spool_responder);

    for (int call = 0; ok && call < 3; call++) {
        char *json = stream_receive(dst, 1);
        bool last = call == 2;
        ok = expect(strstr(json, last ? "\"complete\":true" : "\"complete\":false") != NULL, "capped call", json) && ok;
        ok = expect(nreq == call + 1 && req_cursor[call] == call && req_since[call] == 0, "capped call requests the next page of the same window", NULL) && ok;
        ok = expect(db_checkpoint(dst) == (last ? spool_watermark : 0), "checkpoint moves only after the final page", json) && ok;
    }
    ok = ok && expect(db_int(dst, "SELECT count(*) FROM t") == 3, "all rows applied", NULL);

    // failure in the middle page: the first row stays, nothing is checkpointed, and the
    // next call starts again from page 0 of the same window
    stream_close(dst);
    dst = stream_db(true);
    ok = ok && dst && db_exec(dst, "CREATE TRIGGER deny BEFORE INSERT ON t WHEN NEW.id='k2' BEGIN SELECT RAISE(ABORT,'k2 denied'); END") == SQLITE_OK;
    nreq = 0;
    char *json = ok ? stream_receive(dst, 0) : "";
    ok = ok && expect(strstr(json, "k2 denied") && strstr(json, "\"complete\":false") && strstr(json, "\"rows\":1,\"tables\":[\"t\"]"), "failure reports the error and the rows applied before it", json);
    ok = ok && expect(db_checkpoint(dst) == 0, "no checkpoint after a failure", NULL);
    ok = ok && db_exec(dst, "DROP TRIGGER deny") == SQLITE_OK;
    int before = nreq;
    json = ok ? stream_receive(dst, 0) : "";
    ok = ok && expect(nreq > before && req_cursor[before] == 0 && req_since[before] == 0, "the call after a failure starts from page 0", NULL);
    ok = ok && expect(strstr(json, "\"complete\":true") && db_checkpoint(dst) == spool_watermark, "replay completes the window", json);
    ok = ok && expect(db_int(dst, "SELECT count(*) FROM t") == 3, "all rows applied after the replay", NULL);

    // a checkpoint that cannot be written is reported, not hidden
    stream_close(dst);
    dst = stream_db(true);
    ok = ok && dst && db_exec(dst, "CREATE TRIGGER deny_ckpt BEFORE INSERT ON cloudsync_settings WHEN NEW.key='check_dbversion' BEGIN SELECT RAISE(ABORT,'checkpoint denied'); END") == SQLITE_OK;
    json = ok ? stream_receive(dst, 0) : "";
    ok = ok && expect(strstr(json, "receive checkpoint") && strstr(json, "\"complete\":false") && db_checkpoint(dst) == 0, "checkpoint write failure is reported", json);

    network_test_set_responder(NULL);
    for (int i = 0; i < n; i++) cloudsync_memory_free(pages[i]);
    stream_close(src);
    stream_close(dst);
    return ok;
}

// One value too large for a chunk, sent as fragments.
#define FRAGMENT_CHUNKS "SELECT payload FROM cloudsync_payload_chunks() WHERE substr(payload,5,1)=x'03' ORDER BY chunk_index"
static bool test_stream_fragments(void) {
    bool ok = true;
    sqlite3 *src = stream_db(false), *dst = NULL;
    char *frags[SPOOL_MAX] = {0}, *other[1] = {0};
    ok = ok && src && db_exec(src, "SELECT cloudsync_set('payload_max_chunk_size','262144'); INSERT INTO t VALUES('big', randomblob(700000));") == SQLITE_OK;
    int nf = ok ? stream_payloads(src, "SELECT payload FROM cloudsync_payload_chunks() ORDER BY chunk_index", frags, SPOOL_MAX) : 0;
    ok = ok && expect(nf >= 3, "value split into three or more fragments", NULL);
    ok = ok && db_exec(src, "INSERT INTO t VALUES('small', x'05')") == SQLITE_OK;
    int no = ok ? stream_payloads(src, "SELECT cloudsync_payload_encode(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq) FROM cloudsync_changes WHERE pk=cloudsync_pk_encode('small')", other, 1) : 0;
    ok = ok && expect(no == 1, "monolithic page", NULL);
    spool_watermark = ok ? db_int(src, "SELECT max(db_version) FROM cloudsync_changes") : 0;
    network_test_set_responder(spool_responder);
    const char *big_ok = "SELECT count(*) FROM t WHERE id='big' AND length(v)=700000";
    char *json = "";

    // a stream that ends with a delivered value incomplete fails before checkpointing;
    // the fresh replay from page 0 then completes it
    dst = stream_db(true);
    spool_pages = 0;
    for (int i = 0; i < nf; i++) if (i != 1) spool[spool_pages++] = frags[i];
    json = ok ? stream_receive(dst, 0) : "";
    ok = ok && expect(strstr(json, "incomplete fragmented value") && strstr(json, "\"complete\":false") && db_checkpoint(dst) == 0, "incomplete value at the final chunk", json);
    spool_pages = 0;
    for (int i = 0; i < nf; i++) spool[spool_pages++] = frags[i];
    json = ok ? stream_receive(dst, 0) : "";
    ok = ok && expect(strstr(json, "\"complete\":true") && db_checkpoint(dst) == spool_watermark && db_int(dst, big_ok) == 1, "fresh replay completes the value", json);
    ok = ok && expect(db_int(dst, "SELECT count(*) FROM cloudsync_payload_fragments") == 0, "pieces removed once applied", NULL);
    stream_close(dst);

    // out-of-order and duplicate pieces, across calls capped to one page
    dst = stream_db(true);
    spool_pages = 0;
    spool[spool_pages++] = frags[nf - 1];
    spool[spool_pages++] = frags[0];
    spool[spool_pages++] = frags[0];
    for (int i = 1; i < nf - 1; i++) spool[spool_pages++] = frags[i];
    for (int call = 0; ok && call < spool_pages; call++) json = stream_receive(dst, 1);
    ok = ok && expect(strstr(json, "\"complete\":true") && db_checkpoint(dst) == spool_watermark && db_int(dst, big_ok) == 1, "out-of-order and duplicate pieces", json);
    stream_close(dst);

    // a direct fragment call never moves the checkpoint, and the staging it leaves blocks
    // neither a direct monolithic call nor a stream that does not deliver that value
    dst = stream_db(true);
    ok = ok && expect(direct_apply(dst, src, FRAGMENT_CHUNKS " LIMIT 1") && db_checkpoint(dst) == 0, "direct fragment call does not checkpoint", NULL);
    ok = ok && expect(direct_apply(dst, src, "SELECT cloudsync_payload_encode(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq) FROM cloudsync_changes WHERE pk=cloudsync_pk_encode('small')") && db_checkpoint(dst) == spool_watermark, "staged fragments do not block a direct monolithic call", NULL);
    stream_close(dst);
    dst = stream_db(true);
    ok = ok && direct_apply(dst, src, FRAGMENT_CHUNKS " LIMIT 1");
    spool[0] = other[0];
    spool_pages = 1;
    json = ok ? stream_receive(dst, 0) : "";
    ok = ok && expect(strstr(json, "\"complete\":true") && db_checkpoint(dst) == spool_watermark && db_int(dst, "SELECT count(*) FROM cloudsync_payload_fragments") > 0, "staging from a direct call does not block a stream", json);
    stream_close(dst);

    // a value abandoned by an interrupted stream: the replay after the failure no longer
    // delivers it (replaced upstream), and completes
    dst = stream_db(true);
    spool[0] = frags[0];
    spool[1] = other[0];
    spool_pages = 2;
    json = ok ? stream_receive(dst, 1) : "";
    ok = ok && db_exec(dst, "CREATE TRIGGER deny BEFORE INSERT ON t WHEN NEW.id='small' BEGIN SELECT RAISE(ABORT,'small denied'); END") == SQLITE_OK;
    json = ok ? stream_receive(dst, 1) : "";
    ok = ok && expect(strstr(json, "small denied") != NULL, "interrupting failure", json);
    ok = ok && db_exec(dst, "DROP TRIGGER deny") == SQLITE_OK;
    spool[0] = other[0];
    spool_pages = 1;
    json = ok ? stream_receive(dst, 0) : "";
    ok = ok && expect(strstr(json, "\"complete\":true") && db_checkpoint(dst) == spool_watermark, "abandoned group does not block the replay", json);
    stream_close(dst);

    // completed by another caller before the stream delivers it: the stream stages the
    // pieces again and re-applies the value as a no-op
    dst = stream_db(true);
    ok = ok && direct_apply(dst, src, FRAGMENT_CHUNKS) && db_int(dst, big_ok) == 1;
    spool_pages = 0;
    for (int i = 0; i < nf; i++) spool[spool_pages++] = frags[i];
    json = ok ? stream_receive(dst, 0) : "";
    ok = ok && expect(strstr(json, "\"complete\":true") && db_checkpoint(dst) == spool_watermark, "value completed by another caller first", json);
    stream_close(dst);

    // completed by another caller while the stream is delivering it: the stream's later
    // pieces are staged again, the final check fails once, and the replay passes
    dst = stream_db(true);
    json = ok ? stream_receive(dst, 1) : "";
    ok = ok && direct_apply(dst, src, FRAGMENT_CHUNKS) && db_int(dst, big_ok) == 1;
    json = ok ? stream_receive(dst, 0) : "";
    ok = ok && expect(strstr(json, "incomplete fragmented value") && db_checkpoint(dst) == 0, "value completed by another caller mid-stream fails once", json);
    json = ok ? stream_receive(dst, 0) : "";
    ok = ok && expect(strstr(json, "\"complete\":true") && db_checkpoint(dst) == spool_watermark && db_int(dst, "SELECT count(*) FROM cloudsync_payload_fragments") == 0, "the replay passes", json);
    stream_close(dst);

    network_test_set_responder(NULL);
    for (int i = 0; i < nf; i++) cloudsync_memory_free(frags[i]);
    if (other[0]) cloudsync_memory_free(other[0]);
    stream_close(src);
    return ok;
}

// A server that sends no watermark: a monolithic final chunk still advances to its last
// applied change, while a stream ending in a fragment fails instead of leaving the
// checkpoint in place and replaying the same window forever.
static bool test_stream_no_watermark(void) {
    bool ok = true;
    sqlite3 *src = stream_db(false), *dst = stream_db(true);
    char *pages[SPOOL_MAX] = {0};
    ok = ok && src && dst;
    ok = ok && db_exec(src, "INSERT INTO t VALUES('k1',x'01'); INSERT INTO t VALUES('k2',x'02');") == SQLITE_OK;
    int n = ok ? stream_payloads(src, "SELECT cloudsync_payload_encode(tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq) FROM cloudsync_changes GROUP BY pk ORDER BY pk", pages, SPOOL_MAX) : 0;
    ok = ok && expect(n == 2, "two pages", NULL);
    for (int i = 0; i < n; i++) spool[i] = pages[i];
    spool_pages = n;
    spool_watermark = -1;
    network_test_set_responder(spool_responder);
    char *json = ok ? stream_receive(dst, 0) : "";
    ok = ok && expect(strstr(json, "\"complete\":true") && db_checkpoint(dst) == db_int(src, "SELECT max(db_version) FROM cloudsync_changes"), "monolithic stream without watermark", json);

    stream_close(dst);
    dst = stream_db(true);
    int nf = 0;
    ok = ok && db_exec(src, "SELECT cloudsync_set('payload_max_chunk_size','262144'); INSERT INTO t VALUES('big', randomblob(700000));") == SQLITE_OK;
    char query[256];
    snprintf(query, sizeof(query), "SELECT payload FROM cloudsync_payload_chunks(%lld) WHERE substr(payload,5,1)=x'03' ORDER BY chunk_index", (long long)db_int(src, "SELECT max(db_version) FROM cloudsync_changes") - 1);
    nf = ok ? stream_payloads(src, query, pages + n, SPOOL_MAX - n) : 0;
    ok = ok && expect(nf >= 2, "fragments", NULL);
    for (int i = 0; i < nf; i++) spool[i] = pages[n + i];
    spool_pages = nf;
    json = ok ? stream_receive(dst, 0) : "";
    ok = ok && expect(strstr(json, "has no watermark") && strstr(json, "\"complete\":false") && db_checkpoint(dst) == 0, "fragmented stream without watermark fails", json);
    ok = ok && expect(db_int(dst, "SELECT count(*) FROM t WHERE id='big'") == 0, "the value is not applied", NULL);

    network_test_set_responder(NULL);
    for (int i = 0; i < n + nf; i++) cloudsync_memory_free(pages[i]);
    stream_close(src);
    stream_close(dst);
    return ok;
}

#if !defined(_WIN32) && !defined(CLOUDSYNC_OMIT_CURL)
#include <pthread.h>
#include <time.h>
static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static void *interrupt_after_300ms(void *db) {
    struct timespec delay = {0, 300000000};
    nanosleep(&delay, NULL);
    sqlite3_interrupt((sqlite3 *)db);
    return NULL;
}
// Runs cloudsync_network_receive_changes() and returns its result code and duration.
static int timed_receive(sqlite3 *db, double *seconds) {
    sqlite3_stmt *vm = NULL;
    double start = monotonic_seconds();
    int rc = sqlite3_prepare_v2(db, "SELECT cloudsync_network_receive_changes()", -1, &vm, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(vm);
    if (rc != SQLITE_ROW) rc = sqlite3_errcode(db);
    sqlite3_finalize(vm);
    *seconds = monotonic_seconds() - start;
    return rc;
}
// The deadlines can be tuned at runtime with cloudsync_set, in both directions, and a
// call in flight is cancelled by sqlite3_interrupt() with SQLITE_INTERRUPT. The server is
// a listening socket that never answers; this build compiles a 1 s request deadline.
static bool test_runtime_deadlines(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0 && listen(fd, 8) == 0;
    socklen_t len = sizeof(address);
    ok = ok && getsockname(fd, (struct sockaddr *)&address, &len) == 0;
    char init[160];
    snprintf(init, sizeof(init), "SELECT cloudsync_network_init_custom('http://127.0.0.1:%u', 'test-managed-database-id');", ntohs(address.sin_port));
    sqlite3 *db = ok ? stream_db(false) : NULL;
    ok = ok && db && db_exec(db, init) == SQLITE_OK;
    double seconds = 0;
    char detail[96];

    // raised above the compiled default: the call waits for the longer deadline
    ok = ok && db_exec(db, "SELECT cloudsync_set('network_request_timeout', '3');") == SQLITE_OK;
    int rc = ok ? timed_receive(db, &seconds) : SQLITE_OK;
    snprintf(detail, sizeof(detail), "rc=%d after %.2fs", rc, seconds);
    ok = ok && expect(rc != SQLITE_ROW && rc != SQLITE_INTERRUPT && seconds > 2.5 && seconds < 6, "a raised request deadline takes effect", detail);

    // a non-positive value keeps the compiled default
    ok = ok && db_exec(db, "SELECT cloudsync_set('network_request_timeout', '0');") == SQLITE_OK;
    rc = ok ? timed_receive(db, &seconds) : SQLITE_OK;
    snprintf(detail, sizeof(detail), "rc=%d after %.2fs", rc, seconds);
    ok = ok && expect(rc != SQLITE_ROW && seconds < 2.5, "a non-positive setting keeps the default", detail);

    // cancelled from another thread well before a 30 s deadline
    ok = ok && db_exec(db, "SELECT cloudsync_set('network_request_timeout', '30');") == SQLITE_OK;
    pthread_t thread;
    bool started = ok && pthread_create(&thread, NULL, interrupt_after_300ms, db) == 0;
    rc = started ? timed_receive(db, &seconds) : SQLITE_OK;
    if (started) pthread_join(thread, NULL);
    snprintf(detail, sizeof(detail), "rc=%d after %.2fs", rc, seconds);
    ok = ok && expect(started && (rc & 0xFF) == SQLITE_INTERRUPT && seconds < 5, "sqlite3_interrupt cancels the call with SQLITE_INTERRUPT", detail);

    stream_close(db);
    close(fd);
    return ok;
}
#endif

#ifndef CLOUDSYNC_OMIT_CURL
#include <curl/curl.h>
// With the synchronous resolver and CURLOPT_NOSIGNAL, curl cannot time out a name
// lookup, so a hung DNS server would outlast every deadline. The build must link a
// libcurl that resolves names asynchronously (the threaded resolver).
static bool test_curl_async_dns(void) {
    const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    return info && (info->features & CURL_VERSION_ASYNCHDNS);
}
#endif

// Bounded send: cloudsync_network_send_changes(until) caps the window at until, so a
// backlog uploads as several coherent all-or-nothing batches. Records every /apply
// window so the test can assert all chunks of a batch announce the same one.
static int64_t apply_min[64], apply_max[64];
static int napply;
static int64_t apply_optimistic;
static bool send_no_status;   // status probe answers without a version, so the local fallback governs

static NETWORK_RESULT send_responder(const char *endpoint, const char *request) {
    NETWORK_RESULT r = {0};
    size_t n = endpoint ? strlen(endpoint) : 0;
    bool is_apply = (n >= 6 && strcmp(endpoint + n - 6, "/apply") == 0);
    if (is_apply && request && napply < 64) {
        apply_min[napply] = json_int_after(request, "\"dbVersionMin\":", -1);
        apply_max[napply] = json_int_after(request, "\"dbVersionMax\":", -1);
        if (apply_max[napply] > apply_optimistic) apply_optimistic = apply_max[napply];
        napply++;
    }
    // Both /apply and the status probe answer with the same sync-status shape.
    char *json = cloudsync_memory_alloc(256);
    if (send_no_status && !is_apply) snprintf(json, 256, "{\"data\":{}}");
    else snprintf(json, 256, "{\"data\":{\"lastOptimisticVersion\":%lld,\"lastConfirmedVersion\":%lld,\"gaps\":[]}}",
                  (long long)apply_optimistic, (long long)apply_optimistic);
    r.code = CLOUDSYNC_NETWORK_BUFFER;
    r.buffer = json;
    r.blen = strlen(json);
    return r;
}

static int64_t send_checkpoint(sqlite3 *db) {
    return db_int(db, "SELECT coalesce((SELECT value FROM cloudsync_settings WHERE key='send_dbversion'),0)");
}

static char *send_changes(sqlite3 *db, const char *arg) {
    static char out[1024];
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT cloudsync_network_send_changes(%s)", arg ? arg : "");
    sqlite3_stmt *vm = NULL;
    out[0] = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &vm, NULL) == SQLITE_OK && sqlite3_step(vm) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(vm, 0);
        if (t) snprintf(out, sizeof(out), "%s", t);
    } else {
        snprintf(out, sizeof(out), "ERROR: %s", sqlite3_errmsg(db));
    }
    sqlite3_finalize(vm);
    return out;
}

static bool test_bounded_send(void) {
    bool ok = true;
    sqlite3 *db = stream_db(true);
    ok = ok && expect(db != NULL, "db", NULL);
    if (!ok) { stream_close(db); return false; }

    // Five rows, one transaction each, so the backlog spans five local db_versions.
    for (int i = 1; i <= 5 && ok; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES('r%d', 'v%d');", i, i);
        ok = ok && db_exec(db, sql) == SQLITE_OK;
    }
    ok = ok && expect(db_int(db, "SELECT max(db_version) FROM cloudsync_changes") == 5, "five local db_versions", NULL);

    // Bad arguments are rejected before any I/O.
    ok = ok && expect(strstr(send_changes(db, "'abc'"), "must be an integer") != NULL, "non-integer count rejected", NULL);
    ok = ok && expect(strstr(send_changes(db, "0"), "greater than zero") != NULL, "zero count rejected", NULL);

    napply = 0; apply_optimistic = 0;
    network_test_set_responder(send_responder);

    // Two db_versions per call: the window is [1,2], and every chunk of the batch
    // announces it.
    char *json = send_changes(db, "2");
    bool same_window = (napply > 0);
    for (int i = 0; i < napply; i++) same_window = same_window && apply_min[i] == 1 && apply_max[i] == 2;
    ok = ok && expect(same_window, "every chunk announces [1,2]", json);
    ok = ok && expect(send_checkpoint(db) == 2, "checkpoint advanced to the bound", json);
    ok = ok && expect(strstr(json, "\"status\":\"out-of-sync\"") != NULL, "backlog reported out-of-sync", json);
    ok = ok && expect(strstr(json, "\"localVersion\":5") != NULL, "localVersion is the real backlog", json);

    // Asking for more versions than remain sends the rest and stops at the backlog.
    napply = 0;
    json = send_changes(db, "100");
    same_window = (napply > 0);
    for (int i = 0; i < napply; i++) same_window = same_window && apply_min[i] == 3 && apply_max[i] == 5;
    ok = ok && expect(same_window, "second batch announces [3,5]", json);
    ok = ok && expect(send_checkpoint(db) == 5, "checkpoint at the end of the backlog", json);
    ok = ok && expect(strstr(json, "\"status\":\"synced\"") != NULL, "drained backlog reports synced", json);

    // Sparse local versions: a merge consumes db_versions that hold no local change, so
    // a count-based window has to skip over them instead of stalling on an empty range.
    ok = ok && db_exec(db, "INSERT INTO t VALUES('remote1','x');"
                           "UPDATE \"t_cloudsync\" SET site_id=1, db_version=6 WHERE pk=cloudsync_pk_encode('remote1');") == SQLITE_OK;
    ok = ok && db_exec(db, "INSERT INTO t VALUES('r6','v6');") == SQLITE_OK;
    int64_t local_tail = db_int(db, "SELECT max(db_version) FROM \"t_cloudsync\" WHERE site_id=0");
    ok = ok && expect(local_tail > 6, "a local change sits past the remote-only version", NULL);
    napply = 0;
    json = send_changes(db, "1");
    same_window = (napply > 0);
    for (int i = 0; i < napply; i++) same_window = same_window && apply_max[i] == local_tail;
    ok = ok && expect(same_window, "window skips the remote-only db_version", json);
    ok = ok && expect(send_checkpoint(db) == local_tail, "checkpoint reaches the local change", json);

    // Nothing pending: no batch, and the checkpoint stays put. Advancing past a range
    // nobody announced would strand later local writes and leave a permanent gap in the
    // server's ranges. Checked with the status probe answering without a version, so the
    // local fallback governs -- otherwise the server's optimistic value masks it.
    napply = 0;
    send_no_status = true;
    int64_t before = send_checkpoint(db);
    json = send_changes(db, "10");
    ok = ok && expect(napply == 0, "nothing pending sends no batch", json);
    ok = ok && expect(send_checkpoint(db) == before, "nothing pending leaves the checkpoint alone", json);
    send_no_status = false;

    network_test_set_responder(NULL);
    stream_close(db);
    return ok;
}

// MARK: - Real HTTP transport (/apply capability, deferred 202 lifecycle, rollback)

#if !defined(_WIN32) && !defined(CLOUDSYNC_OMIT_CURL)
#include <strings.h>

// A loopback HTTP/1.1 server answering one request per connection, so requests are
// built by the real libcurl transport rather than the test responder (which bypasses
// header construction). /apply and /status replies are scripted; /upload hands out a
// loopback URL for the S3 PUT. Every request records whether it advertised apply-defer.
#define HTTP_MAX_REQ 64
static int http_fd = -1;
static unsigned http_port;
static volatile bool http_stop;
static int http_apply_code, http_status_code;
static char http_apply_body[512], http_status_body[512];
static int http_nreq, http_napply, http_nstatus, http_nupload, http_nput;
static char http_req_endpoint[HTTP_MAX_REQ][8];
static bool http_req_defer[HTTP_MAX_REQ];
static char http_apply_batch[HTTP_MAX_REQ][48];
static int64_t http_apply_chunk[HTTP_MAX_REQ], http_apply_min[HTTP_MAX_REQ];
static bool http_apply_url[HTTP_MAX_REQ];

static const char *http_header(const char *head, const char *name) {
    size_t n = strlen(name);
    for (const char *line = strstr(head, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
        if (strncasecmp(line + 2, name, n) == 0 && line[2 + n] == ':') return line + 3 + n;
    }
    return NULL;
}

static void http_reply(int c, int code, const char *body) {
    const char *reason = code == 200 ? "OK" : code == 202 ? "Accepted" : code == 409 ? "Conflict" : "Service Unavailable";
    char head[256];
    int len = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                       code, reason, strlen(body));
    if (write(c, head, (size_t)len) < 0 || write(c, body, strlen(body)) < 0) return;
}

static void http_handle(int c) {
    size_t cap = 4096, used = 0, head_len = 0, body_len = 0;
    char *buf = malloc(cap);
    bool continued = false;
    while (buf) {
        if (used + 1 >= cap) { char *grown = realloc(buf, cap *= 2); if (!grown) break; buf = grown; }
        ssize_t r = read(c, buf + used, cap - used - 1);
        if (r <= 0) break;
        used += (size_t)r;
        buf[used] = 0;
        if (!head_len) {
            char *end = strstr(buf, "\r\n\r\n");
            if (!end) continue;
            head_len = (size_t)(end - buf) + 4;
            const char *length = http_header(buf, "Content-Length");
            body_len = length ? (size_t)strtoull(length, NULL, 10) : 0;
        }
        const char *expect = http_header(buf, "Expect");
        if (!continued && expect && strncasecmp(expect + strspn(expect, " "), "100-continue", 12) == 0) {
            continued = true;
            if (write(c, "HTTP/1.1 100 Continue\r\n\r\n", 25) < 0) break;
        }
        if (used >= head_len + body_len) break;
    }
    if (!buf || !head_len) { free(buf); return; }

    buf[head_len - 2] = 0;  // headers only, body follows at buf + head_len
    const char *body = buf + head_len;
    char path[256] = "";
    sscanf(buf, "%*s %255s", path);
    const char *slash = strrchr(path, '/');
    const char *endpoint = slash ? slash + 1 : path;
    const char *capabilities = http_header(buf, "X-CloudSync-Capabilities");
    int i = http_nreq < HTTP_MAX_REQ ? http_nreq++ : HTTP_MAX_REQ - 1;
    snprintf(http_req_endpoint[i], sizeof(http_req_endpoint[i]), "%s", endpoint);
    http_req_defer[i] = capabilities && strstr(capabilities, "apply-defer");

    if (strcmp(endpoint, "apply") == 0) {
        int k = http_napply < HTTP_MAX_REQ ? http_napply++ : HTTP_MAX_REQ - 1;
        const char *batch = strstr(body, "\"batchId\":\"");
        http_apply_batch[k][0] = 0;
        if (batch) sscanf(batch + 11, "%47[^\"]", http_apply_batch[k]);
        http_apply_chunk[k] = json_int_after(body, "\"chunkIndex\":", -1);
        http_apply_min[k] = json_int_after(body, "\"dbVersionMin\":", -1);
        http_apply_url[k] = strstr(body, "{\"url\":") == body;
        http_reply(c, http_apply_code, http_apply_body);
    } else if (strcmp(endpoint, "status") == 0) {
        http_nstatus++;
        http_reply(c, http_status_code, http_status_body);
    } else if (strcmp(endpoint, "upload") == 0) {
        http_nupload++;
        char url[128];
        snprintf(url, sizeof(url), "{\"url\":\"http://127.0.0.1:%u/s3\"}", http_port);
        http_reply(c, 200, url);
    } else {
        http_nput++;
        http_reply(c, 200, "");
    }
    free(buf);
}

static void *http_serve(void *arg) {
    (void)arg;
    while (!http_stop) {
        int c = accept(http_fd, NULL, NULL);
        if (c < 0) continue;
        if (!http_stop) http_handle(c);
        close(c);
    }
    return NULL;
}

static bool http_start(pthread_t *thread) {
    http_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (http_fd < 0) return false;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(address);
    if (bind(http_fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(http_fd, 8) != 0 ||
        getsockname(http_fd, (struct sockaddr *)&address, &len) != 0) {
        close(http_fd);
        return false;
    }
    http_port = ntohs(address.sin_port);
    http_stop = false;
    return pthread_create(thread, NULL, http_serve, NULL) == 0;
}

static void http_shutdown(pthread_t thread) {
    // accept() is not woken by close() everywhere: unblock it with one last connection.
    http_stop = true;
    int wake = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)http_port);
    if (wake >= 0) { connect(wake, (struct sockaddr *)&address, sizeof(address)); close(wake); }
    pthread_join(thread, NULL);
    close(http_fd);
}

// Server state for one send: what /apply answers and what a later /status reports.
static void http_script(int apply_code, int64_t apply_optimistic, int64_t apply_confirmed,
                        int64_t status_optimistic, int64_t status_confirmed, const char *status_failure) {
    http_apply_code = apply_code;
    if (apply_code >= 400) {
        snprintf(http_apply_body, sizeof(http_apply_body), "{\"error\":{\"code\":\"%s\",\"retryable\":true}}",
                 apply_code == 409 ? "apply_batch_superseded" : "database_unreachable");
    } else {
        snprintf(http_apply_body, sizeof(http_apply_body),
                 "{\"data\":{\"lastOptimisticVersion\":%lld,\"lastConfirmedVersion\":%lld,\"gaps\":[]}}",
                 (long long)apply_optimistic, (long long)apply_confirmed);
    }
    http_status_code = 200;
    snprintf(http_status_body, sizeof(http_status_body),
             "{\"data\":{\"lastOptimisticVersion\":%lld,\"lastConfirmedVersion\":%lld,\"gaps\":[],\"failures\":{\"apply\":%s,\"check\":null}}}",
             (long long)status_optimistic, (long long)status_confirmed, status_failure ? status_failure : "null");
}

static int64_t has_unsent(sqlite3 *db) {
    return db_int(db, "SELECT cloudsync_network_has_unsent_changes()");
}

// A rolled-back range is re-sent as a new batch that starts at chunk zero, so the
// server's supersession rules can replace the failed attempt.
static bool resent_as_new_batch(int prev, int64_t min, const char *what, const char *json) {
    int last = http_napply - 1;
    bool ok = last > prev && strcmp(http_apply_batch[last], http_apply_batch[prev]) != 0 &&
              http_apply_chunk[last] == 0 && http_apply_min[last] == min;
    return expect(ok, what, json);
}

static bool test_apply_defer_transport(void) {
    pthread_t thread;
    if (!http_start(&thread)) return false;
    http_nreq = http_napply = http_nstatus = http_nupload = http_nput = 0;

    char init[160];
    snprintf(init, sizeof(init), "SELECT cloudsync_network_init_custom('http://127.0.0.1:%u', 'test-managed-database-id');", http_port);
    sqlite3 *db = stream_db(false);
    bool ok = expect(db && db_exec(db, init) == SQLITE_OK, "network init", NULL);
    ok = ok && db_exec(db, "SELECT cloudsync_set('network_request_timeout', '10');") == SQLITE_OK;
    char *json = NULL;

    // Deferred send: /apply answers 202 with the range accepted but not confirmed.
    ok = ok && db_exec(db, "INSERT INTO t VALUES('r1', 'v1');") == SQLITE_OK;
    http_script(202, 1, 0, 1, 0, NULL);
    if (ok) json = send_changes(db, NULL);
    ok = ok && expect(http_napply == 1 && !http_apply_url[0] && http_apply_chunk[0] == 0, "one inline /apply", json);
    ok = ok && expect(strstr(json, "\"status\":\"syncing\"") != NULL, "202 reports syncing", json);
    ok = ok && expect(send_checkpoint(db) == 1, "202 advances the checkpoint to optimistic", json);
    ok = ok && expect(has_unsent(db) == 1, "accepted but unconfirmed counts as unsent", NULL);

    // Confirmation arrives through a later /status, without repeating /apply.
    http_script(202, 1, 0, 1, 1, NULL);
    ok = ok && expect(has_unsent(db) == 0, "confirmed range is no longer unsent", NULL);
    if (ok) json = send_changes(db, NULL);
    ok = ok && expect(http_napply == 1, "confirmation does not repeat /apply", json);
    ok = ok && expect(strstr(json, "\"status\":\"synced\"") != NULL, "confirmed reports synced", json);

    // Rollback with no visible failure: a lower optimistic version alone rolls the
    // checkpoint back, and the next send re-sends the range.
    ok = ok && db_exec(db, "INSERT INTO t VALUES('r2', 'v2');") == SQLITE_OK;
    http_script(202, 2, 1, 1, 1, NULL);
    if (ok) json = send_changes(db, NULL);
    int prev = http_napply - 1;
    ok = ok && expect(send_checkpoint(db) == 2, "deferred send accepted", json);
    if (ok) json = send_changes(db, NULL);
    ok = ok && expect(send_checkpoint(db) == 1, "lower optimistic rolls the checkpoint back", json);
    ok = ok && expect(strstr(json, "\"status\":\"out-of-sync\"") && !strstr(json, "lastFailure"), "out-of-sync, no failure", json);
    http_script(200, 2, 2, 2, 2, NULL);
    if (ok) json = send_changes(db, NULL);
    ok = ok && resent_as_new_batch(prev, 2, "rolled-back range re-sent as a new batch", json);
    ok = ok && expect(strstr(json, "\"status\":\"synced\"") != NULL, "re-send converges", json);

    // Rollback with a visible failure: same recovery, and the diagnostic is surfaced.
    ok = ok && db_exec(db, "INSERT INTO t VALUES('r3', 'v3');") == SQLITE_OK;
    http_script(202, 3, 2, 2, 2, "{\"jobId\":7,\"code\":\"apply_deferred_expired\",\"retryable\":true}");
    if (ok) json = send_changes(db, NULL);
    prev = http_napply - 1;
    if (ok) json = send_changes(db, NULL);
    ok = ok && expect(send_checkpoint(db) == 2, "failed deferred range rolls the checkpoint back", json);
    ok = ok && expect(strstr(json, "\"lastFailure\":{\"jobId\":7") != NULL, "failures.apply surfaced as lastFailure", json);
    http_script(200, 3, 3, 3, 3, NULL);
    if (ok) json = send_changes(db, NULL);
    ok = ok && resent_as_new_batch(prev, 3, "failed range re-sent as a new batch", json);

    // 503 and 409 fail the call and leave the checkpoint alone; the retry is a new batch.
    const int rejections[] = {503, 409};
    for (int i = 0; i < 2 && ok; i++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES('x%d', 'v');", i);
        ok = db_exec(db, sql) == SQLITE_OK;
        int64_t before = send_checkpoint(db);
        http_script(rejections[i], 0, 0, before, before, NULL);
        if (ok) json = send_changes(db, NULL);
        prev = http_napply - 1;
        ok = ok && expect(strncmp(json, "ERROR:", 6) == 0, "rejected /apply is an error", json);
        ok = ok && expect(send_checkpoint(db) == before, "rejected /apply keeps the checkpoint", json);
        http_script(200, before + 1, before + 1, before + 1, before + 1, NULL);
        if (ok) json = send_changes(db, NULL);
        ok = ok && resent_as_new_batch(prev, before + 1, "retry after rejection is a new batch", json);
    }

    // The S3 form (payload above the inline limit) advertises the capability too.
    int napply_before = http_napply;
    ok = ok && db_exec(db, "INSERT INTO t VALUES('big', randomblob(200000));") == SQLITE_OK;
    int64_t big = db_int(db, "SELECT max(db_version) FROM cloudsync_changes");
    http_script(202, big, big - 1, big, big - 1, NULL);
    if (ok) json = send_changes(db, NULL);
    ok = ok && expect(http_nupload == 1 && http_nput == 1 && http_napply == napply_before + 1 && http_apply_url[http_napply - 1],
                      "large payload goes through /upload + S3 + /apply", json);

    // Only /apply negotiates deferral.
    bool headers_ok = http_nreq > 0;
    for (int i = 0; i < http_nreq && i < HTTP_MAX_REQ; i++) {
        headers_ok = headers_ok && http_req_defer[i] == (strcmp(http_req_endpoint[i], "apply") == 0);
    }
    ok = ok && expect(headers_ok, "apply-defer sent on every /apply and nowhere else", NULL);

    stream_close(db);
    http_shutdown(thread);
    return ok;
}
#endif

int main(void) {
#if !defined(_WIN32) && !defined(CLOUDSYNC_OMIT_CURL)
    check("HTTP deadlines and interrupt: API cap, artifact stall, cancel:", test_stalled_http_timeout());
#endif
#ifndef CLOUDSYNC_OMIT_CURL
    check("libcurl resolves names asynchronously (DNS honors deadlines):", test_curl_async_dns());
#endif
    check("JSON keys only match root object members:", test_json_scope());
    check("Gateway data envelope is unwrapped before scoped lookups:", test_json_envelope());
    check("JSON Unicode, surrogate pairs and malformed escapes:", test_unicode());
    check("receive JSON members (rows, error, lastFailure):", test_receive_json());
    printf("\nNetwork unit tests\n");
    check("optimistic/confirmed version folds latest-valid (allows rollback):", test_optimistic_version_rollback());
    check("non-buffer response is a no-op:", test_non_buffer_is_noop());
    check("send batch /apply payload (window / batchId / chunkIndex / isFinal):", test_apply_json_payload_batch());
    check("network_compute_status:", test_compute_status());
    check("bounded send: window, checkpoint, empty-window guard:", test_bounded_send());
    check("receive stream: capped paging, failure replay, checkpoint errors:", test_stream_paging());
    check("receive stream: fragmented values:", test_stream_fragments());
    check("receive stream: server without watermark:", test_stream_no_watermark());
#if !defined(_WIN32) && !defined(CLOUDSYNC_OMIT_CURL)
    check("runtime deadlines (cloudsync_set) and interrupt of a call:", test_runtime_deadlines());
    check("apply-defer over HTTP: header, 202 lifecycle, rollback, 503/409:", test_apply_defer_transport());
#endif
    if (failures) { printf("\n%d test(s) FAILED\n", failures); return 1; }
    printf("\nAll network unit tests passed\n");
    return 0;
}
