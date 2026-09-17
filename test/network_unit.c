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
extern char *network_test_receive_json(int, int, const char *, bool, const char *, const char *);
static bool receive_json_is(char *json, const char *expected) {
    bool ok = json && strcmp(json, expected) == 0;
    if (!ok) printf("\n    got:      %s\n    expected: %s\n", json ? json : "(null)", expected);
    cloudsync_memory_free(json);
    return ok;
}
static bool test_receive_json(void) {
    bool ok = receive_json_is(network_test_receive_json(3, 0, NULL, true, NULL, NULL),
        "\"receive\":{\"rows\":3,\"failed\":0,\"tables\":[\"t\"],\"chunks\":1,\"bytes\":10,\"complete\":true}");
    ok = receive_json_is(network_test_receive_json(2, 1, "rejected \"here\"", true, NULL, NULL),
        "\"receive\":{\"rows\":2,\"failed\":1,\"failedError\":\"rejected \\\"here\\\"\",\"tables\":[\"t\"],\"chunks\":1,\"bytes\":10,\"complete\":true}") && ok;
    ok = receive_json_is(network_test_receive_json(0, 0, NULL, false, "boom", "{\"code\":\"x\"}"),
        "\"receive\":{\"rows\":0,\"failed\":0,\"tables\":[\"t\"],\"chunks\":1,\"bytes\":10,\"complete\":false,\"error\":\"boom\",\"lastFailure\":{\"code\":\"x\"}}") && ok;
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
extern bool network_test_curl_timeout(const char *, bool, bool);
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
    close(fd);
    return ok;
}
#endif

int main(void) {
#if !defined(_WIN32) && !defined(CLOUDSYNC_OMIT_CURL)
    check("HTTP deadlines: API elapsed cap and artifact stall cap:", test_stalled_http_timeout());
#endif
    check("JSON keys only match root object members:", test_json_scope());
    check("Gateway data envelope is unwrapped before scoped lookups:", test_json_envelope());
    check("JSON Unicode, surrogate pairs and malformed escapes:", test_unicode());
    check("receive JSON members (failed, failedError, error, lastFailure):", test_receive_json());
    printf("\nNetwork unit tests\n");
    check("optimistic/confirmed version folds latest-valid (allows rollback):", test_optimistic_version_rollback());
    check("non-buffer response is a no-op:", test_non_buffer_is_noop());
    check("send batch /apply payload (window / batchId / chunkIndex / isFinal):", test_apply_json_payload_batch());
    check("network_compute_status:", test_compute_status());
    if (failures) { printf("\n%d test(s) FAILED\n", failures); return 1; }
    printf("\nAll network unit tests passed\n");
    return 0;
}
