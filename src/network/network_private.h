//
//  network_private.h
//  cloudsync
//
//  Created by Marco Bambini on 23/05/25.
//

#ifndef __CLOUDSYNC_NETWORK_PRIVATE__
#define __CLOUDSYNC_NETWORK_PRIVATE__

#include <stdint.h>
#include <stddef.h>

#define CLOUDSYNC_DEFAULT_ADDRESS           "https://cloudsync.sqlite.ai"
#define CLOUDSYNC_ENDPOINT_PREFIX           "v2/cloudsync/databases"
#define CLOUDSYNC_ENDPOINT_UPLOAD           "upload"
#define CLOUDSYNC_ENDPOINT_CHECK            "check"
#define CLOUDSYNC_ENDPOINT_APPLY            "apply"
#define CLOUDSYNC_ENDPOINT_STATUS           "status"
#define CLOUDSYNC_HEADER_ORG                "X-CloudSync-Org"
#define CLOUDSYNC_HEADER_VERSION            "X-CloudSync-Version"
#define CLOUDSYNC_HEADER_TICKET             "X-CloudSync-Ticket"
#define CLOUDSYNC_HEADER_TICKET_EXPIRES_AT  "X-CloudSync-Ticket-Expires-At"
// CLOUDSYNC_VERSION is defined in cloudsync.h — include it before this header at use sites.
#define CLOUDSYNC_HEADER_VERSION_LINE       CLOUDSYNC_HEADER_VERSION ": " CLOUDSYNC_VERSION
#define CLOUDSYNC_HEADER_CHECK_CAPABILITIES "X-CloudSync-Capabilities: check-status-response, check-chunks"

#define CLOUDSYNC_NETWORK_OK                1
#define CLOUDSYNC_NETWORK_ERROR             2
#define CLOUDSYNC_NETWORK_BUFFER            3

typedef struct network_data network_data;

typedef struct {
    int     code;                   // network code: OK, ERROR, BUFFER
    char    *buffer;                // network buffer
    size_t  blen;                   // blen if code is SQLITE_OK, rc in case of error
    void    *xdata;                 // optional custom external data
    void    (*xfree) (void *);      // optional custom free callback
} NETWORK_RESULT;

char *network_data_get_siteid (network_data *data);
char *network_data_get_orgid (network_data *data);
char *network_data_get_ticket (network_data *data);
bool network_data_should_use_ticket (network_data *data, const char *endpoint, const char *authentication);
void network_data_update_ticket (network_data *data, const char *ticket, const char *expires_at);
bool network_data_set_endpoints (network_data *data, char *auth, char *check, char *upload, char *apply, char *status);

bool network_send_buffer(network_data *data, const char *endpoint, const char *authentication, const void *blob, int blob_size);
NETWORK_RESULT network_receive_buffer (network_data *data, const char *endpoint, const char *authentication, bool zero_terminated, bool is_post_request, char *json_payload, const char **extra_headers, int nextra_headers);

// Exposed (non-static) for the network unit test; otherwise internal to network.c.
void network_sync_state_update_from_response(NETWORK_RESULT *res, int64_t *last_optimistic_version, int64_t *last_confirmed_version, int *gaps_size, char **apply_failure_json, char **check_failure_json);
const char *network_compute_status(int64_t last_optimistic, int64_t last_confirmed, int gaps_size, int64_t local_version);

// Lower bound of the coverage window a send chunk announces to /apply: the running
// window start, but never above the chunk's own min (so consecutive chunks sharing a
// db_version — a value fragmented across chunks — keep min<=max). The caller advances
// window_lo to chunk_db_version_max+1 after each chunk so the ranges tile contiguously.
static inline int64_t network_announce_min(int64_t window_lo, int64_t chunk_db_version_min) {
    return window_lo < chunk_db_version_min ? window_lo : chunk_db_version_min;
}

#ifdef CLOUDSYNC_NETWORK_TRACE
const char *network_trace_endpoint_name(network_data *data, const char *endpoint);
const char *network_trace_result_name(int code);
void network_trace_log(network_data *data, const char *method, const char *endpoint, long http_status, int result_code, size_t request_bytes, size_t bytes, double elapsed_ms);
double network_trace_now_ms(void);
#endif


#endif
