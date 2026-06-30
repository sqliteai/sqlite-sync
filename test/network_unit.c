//
//  network_unit.c
//  cloudsync
//
//  Unit tests for the network layer's pure response-handling logic. Built with
//  networking ENABLED (unlike dist/unit, which is -DCLOUDSYNC_OMIT_NETWORK), so it
//  can call the internal functions directly on crafted in-memory NETWORK_RESULT
//  buffers — no server, no sockets.
//

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
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

static bool test_compute_status(void) {
    bool ok = true;
    ok = ok && strcmp(network_compute_status(100, 100, 0, 100), "synced") == 0;
    ok = ok && strcmp(network_compute_status(100, 50, 0, 100), "syncing") == 0;
    ok = ok && strcmp(network_compute_status(100, 100, 1, 100), "out-of-sync") == 0;  // gaps
    ok = ok && strcmp(network_compute_status(90, 90, 0, 100), "out-of-sync") == 0;     // behind local
    ok = ok && strcmp(network_compute_status(-1, 100, 0, 100), "error") == 0;          // unparsed
    return ok;
}

int main(void) {
    printf("\nNetwork unit tests\n");
    check("optimistic/confirmed version folds latest-valid (allows rollback):", test_optimistic_version_rollback());
    check("non-buffer response is a no-op:", test_non_buffer_is_noop());
    check("network_compute_status:", test_compute_status());
    if (failures) { printf("\n%d test(s) FAILED\n", failures); return 1; }
    printf("\nAll network unit tests passed\n");
    return 0;
}
