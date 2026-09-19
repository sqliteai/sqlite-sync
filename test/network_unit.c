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
    check("receive stream: capped paging, failure replay, checkpoint errors:", test_stream_paging());
    check("receive stream: fragmented values:", test_stream_fragments());
    check("receive stream: server without watermark:", test_stream_no_watermark());
    if (failures) { printf("\n%d test(s) FAILED\n", failures); return 1; }
    printf("\nAll network unit tests passed\n");
    return 0;
}
