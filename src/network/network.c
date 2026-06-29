//
//  network.c
//  cloudsync
//
//  Created by Marco Bambini on 12/12/24.
//

#ifndef CLOUDSYNC_OMIT_NETWORK

#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef CLOUDSYNC_NETWORK_TRACE
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#endif

#include "network.h"
#include "../utils.h"
#include "../dbutils.h"
#include "../cloudsync.h"
#include "network_private.h"

#define JSMN_STATIC
#include "jsmn.h"

#ifndef SQLITE_WASM_EXTRA_INIT
#ifndef CLOUDSYNC_OMIT_CURL
#include "curl/curl.h"
#endif
#else
#define curl_free(x) free(x)
char *substr(const char *start, const char *end);
#endif

#ifdef __ANDROID__
#include "cacert.h"
static size_t cacert_len = sizeof(cacert_pem) - 1;
#endif
 
#define CLOUDSYNC_NETWORK_MINBUF_SIZE           512
#define CLOUDSYNC_SESSION_TOKEN_MAXSIZE         4096

#ifndef CLOUDSYNC_CURL_MAXCONNECTS
#define CLOUDSYNC_CURL_MAXCONNECTS              2L
#endif
#ifndef CLOUDSYNC_CURL_MAXAGE_CONN_SECONDS
#define CLOUDSYNC_CURL_MAXAGE_CONN_SECONDS      15L
#endif
#ifndef CLOUDSYNC_CURL_MAXLIFETIME_CONN_SECONDS
#define CLOUDSYNC_CURL_MAXLIFETIME_CONN_SECONDS 60L
#endif
#ifndef CLOUDSYNC_NETWORK_FAST_LANE_MAX_BLOB_SIZE
#define CLOUDSYNC_NETWORK_FAST_LANE_MAX_BLOB_SIZE (128 * 1024)
#endif

#define DEFAULT_SYNC_WAIT_MS                    100
#define DEFAULT_SYNC_MAX_RETRIES                1

// Safety bound on how many chunks a single cloudsync_network_sync call will drain
// back-to-back. The drain is normally terminated by the server's "final" flag; this
// only guards against a buggy/never-final server. On hit, the drain stops with
// complete=false and the durable receive cursor is left unadvanced past un-applied
// rows, so the next sync resumes safely (apply is idempotent).
#define CLOUDSYNC_CHECK_MAX_CHUNKS_PER_DRAIN    10000
 
#define MAX_QUERY_VALUE_LEN                     256

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT3
#endif

// MARK: -

struct network_data {
    char        site_id[UUID_STR_MAXLEN];
    char        *authentication; // apikey or token
    char        *org_id;         // organization ID for X-CloudSync-Org header
    char        *ticket;         // optional short-lived sync runtime ticket
    char        *ticket_expires_at;
    char        *check_endpoint;
    char        *upload_endpoint;
    char        *apply_endpoint;
    char        *status_endpoint;
    int         ticket_enabled;
    // Best-effort spool cursor for the chunked /check download drain. The durable
    // receive cursor (check_dbversion/check_seq) is frozen at "since" for the whole
    // drain, so the server (which is stateless across /check calls) needs the client
    // to echo which spool page to serve next. In-memory only: losing it just
    // restarts the drain from page 0, which is safe because apply is idempotent.
    int64_t     check_cursor;        // next page index to request (0 = fresh drain)
    int64_t     check_cursor_since;  // the check_dbversion check_cursor belongs to
#ifndef CLOUDSYNC_OMIT_CURL
    CURL        *api_curl;
    CURL        *artifact_curl;
    int         curl_pool_enabled;
#endif
};

#ifdef CLOUDSYNC_NETWORK_TRACE
double network_trace_now_ms(void) {
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

const char *network_trace_endpoint_name(network_data *data, const char *endpoint) {
    if (!data || !endpoint) return "unknown";
    if (data->check_endpoint && strcmp(endpoint, data->check_endpoint) == 0) return "check";
    if (data->upload_endpoint && strcmp(endpoint, data->upload_endpoint) == 0) return "upload-url";
    if (data->apply_endpoint && strcmp(endpoint, data->apply_endpoint) == 0) return "apply";
    if (data->status_endpoint && strcmp(endpoint, data->status_endpoint) == 0) return "status";
    return "artifact";
}

const char *network_trace_result_name(int code) {
    switch (code) {
        case CLOUDSYNC_NETWORK_OK: return "ok";
        case CLOUDSYNC_NETWORK_ERROR: return "error";
        case CLOUDSYNC_NETWORK_BUFFER: return "buffer";
        default: return "unknown";
    }
}

void network_trace_log(network_data *data, const char *method, const char *endpoint, long http_status, int result_code, size_t request_bytes, size_t bytes, double elapsed_ms) {
    fprintf(stderr,
            "[cloudsync-network] endpoint=%s method=%s http_status=%ld result=%s request_bytes=%zu bytes=%zu elapsed_ms=%.2f\n",
            network_trace_endpoint_name(data, endpoint), method, http_status,
            network_trace_result_name(result_code), request_bytes, bytes, elapsed_ms);
}

#ifndef CLOUDSYNC_OMIT_CURL
void network_trace_log_curl(network_data *data, const char *method, const char *endpoint, long http_status, int result_code, size_t request_bytes, size_t bytes, CURL *curl, bool pooled, double elapsed_ms) {
    double namelookup = 0.0;
    double connect = 0.0;
    double appconnect = 0.0;
    double starttransfer = 0.0;
    double total = 0.0;
    long num_connects = 0;
    if (curl) {
        curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &namelookup);
        curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect);
        curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &appconnect);
        curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &starttransfer);
        curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total);
        curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &num_connects);
    }
    fprintf(stderr,
            "[cloudsync-network] endpoint=%s method=%s pool=%s http_status=%ld result=%s request_bytes=%zu bytes=%zu elapsed_ms=%.2f curl_total_ms=%.2f dns_ms=%.2f connect_ms=%.2f tls_ms=%.2f starttransfer_ms=%.2f num_connects=%ld\n",
            network_trace_endpoint_name(data, endpoint), method,
            pooled ? "on" : "off", http_status,
            network_trace_result_name(result_code), request_bytes, bytes, elapsed_ms,
            total * 1000.0, namelookup * 1000.0, connect * 1000.0,
            appconnect * 1000.0, starttransfer * 1000.0, num_connects);
}
#endif
#endif

typedef struct {
    char        *buffer;
    size_t      balloc;
    size_t      bused;
    int         zero_term;
} network_buffer;

 
typedef struct {
    const char *data;
    size_t      size;
    size_t      read_pos;
} network_read_data;

typedef struct {
    char        *ticket;
    char        *expires_at;
} network_ticket_headers;

static const char *cloudsync_default_headers[] = {
    CLOUDSYNC_HEADER_VERSION_LINE,
};

static const char *cloudsync_check_headers[] = {
    CLOUDSYNC_HEADER_VERSION_LINE,
    CLOUDSYNC_HEADER_CHECK_CAPABILITIES,
};

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

// MARK: -

void network_result_cleanup (NETWORK_RESULT *res) {
    if (res->xfree) {
        res->xfree(res->xdata);
    } else if (res->buffer) {
        cloudsync_memory_free(res->buffer);
    }
}

char *network_data_get_siteid (network_data *data) {
    return data->site_id;
}

char *network_data_get_orgid (network_data *data) {
    return data->org_id;
}

char *network_data_get_ticket (network_data *data) {
    return data->ticket;
}

static void network_data_clear_ticket (network_data *data) {
    if (!data) return;
    if (data->ticket) cloudsync_memory_free(data->ticket);
    if (data->ticket_expires_at) cloudsync_memory_free(data->ticket_expires_at);
    data->ticket = NULL;
    data->ticket_expires_at = NULL;
}

bool network_data_set_endpoints (network_data *data, char *auth, char *check, char *upload, char *apply, char *status) {
    // sanity check
    if (!check || !upload) return false;

    // always free previous owned pointers
    if (data->authentication) cloudsync_memory_free(data->authentication);
    network_data_clear_ticket(data);
    if (data->check_endpoint) cloudsync_memory_free(data->check_endpoint);
    if (data->upload_endpoint) cloudsync_memory_free(data->upload_endpoint);
    if (data->apply_endpoint) cloudsync_memory_free(data->apply_endpoint);
    if (data->status_endpoint) cloudsync_memory_free(data->status_endpoint);

    // clear pointers
    data->authentication = NULL;
    data->check_endpoint = NULL;
    data->upload_endpoint = NULL;
    data->apply_endpoint = NULL;
    data->status_endpoint = NULL;

    // make a copy of the new endpoints
    char *auth_copy = NULL;
    char *check_copy = NULL;
    char *upload_copy = NULL;
    char *apply_copy = NULL;
    char *status_copy = NULL;

    // auth is optional
    if (auth) {
        auth_copy = cloudsync_string_dup(auth);
        if (!auth_copy) goto abort_endpoints;
    }
    check_copy = cloudsync_string_dup(check);
    if (!check_copy) goto abort_endpoints;

    upload_copy = cloudsync_string_dup(upload);
    if (!upload_copy) goto abort_endpoints;

    apply_copy = cloudsync_string_dup(apply);
    if (!apply_copy) goto abort_endpoints;

    status_copy = cloudsync_string_dup(status);
    if (!status_copy) goto abort_endpoints;

    data->authentication = auth_copy;
    data->check_endpoint = check_copy;
    data->upload_endpoint = upload_copy;
    data->apply_endpoint = apply_copy;
    data->status_endpoint = status_copy;
    return true;

abort_endpoints:
    if (auth_copy) cloudsync_memory_free(auth_copy);
    if (check_copy) cloudsync_memory_free(check_copy);
    if (upload_copy) cloudsync_memory_free(upload_copy);
    if (apply_copy) cloudsync_memory_free(apply_copy);
    if (status_copy) cloudsync_memory_free(status_copy);
    return false;
}

void network_data_free (network_data *data) {
    if (!data) return;

#ifndef CLOUDSYNC_OMIT_CURL
    if (data->api_curl) curl_easy_cleanup(data->api_curl);
    if (data->artifact_curl) curl_easy_cleanup(data->artifact_curl);
#endif
    if (data->authentication) cloudsync_memory_free(data->authentication);
    if (data->org_id) cloudsync_memory_free(data->org_id);
    network_data_clear_ticket(data);
    if (data->check_endpoint) cloudsync_memory_free(data->check_endpoint);
    if (data->upload_endpoint) cloudsync_memory_free(data->upload_endpoint);
    if (data->apply_endpoint) cloudsync_memory_free(data->apply_endpoint);
    if (data->status_endpoint) cloudsync_memory_free(data->status_endpoint);
    cloudsync_memory_free(data);
}

// MARK: - Utils -

static bool network_endpoint_is_api(network_data *data, const char *endpoint) {
    if (!data || !endpoint) return false;
    return (data->check_endpoint && strcmp(endpoint, data->check_endpoint) == 0) ||
           (data->upload_endpoint && strcmp(endpoint, data->upload_endpoint) == 0) ||
           (data->apply_endpoint && strcmp(endpoint, data->apply_endpoint) == 0) ||
           (data->status_endpoint && strcmp(endpoint, data->status_endpoint) == 0);
}

static bool network_env_disabled(const char *value) {
    return value && (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "off") == 0 || strcmp(value, "no") == 0);
}

static bool network_ticket_enabled(network_data *data) {
    if (!data) return false;
    if (data->ticket_enabled == 0) {
        const char *value = getenv("CLOUDSYNC_NETWORK_TICKET");
        data->ticket_enabled = network_env_disabled(value) ? -1 : 1;
    }
    return data->ticket_enabled > 0;
}

bool network_data_should_use_ticket (network_data *data, const char *endpoint, const char *authentication) {
    return data && authentication && authentication[0] != '\0' && data->ticket && data->ticket[0] != '\0' &&
           network_ticket_enabled(data) && network_endpoint_is_api(data, endpoint);
}

void network_data_update_ticket (network_data *data, const char *ticket, const char *expires_at) {
    if (!data || !ticket || ticket[0] == '\0') return;

    char *ticket_copy = cloudsync_string_dup(ticket);
    if (!ticket_copy) return;

    char *expires_copy = NULL;
    if (expires_at && expires_at[0] != '\0') {
        expires_copy = cloudsync_string_dup(expires_at);
        if (!expires_copy) {
            cloudsync_memory_free(ticket_copy);
            return;
        }
    }

    network_data_clear_ticket(data);
    data->ticket = ticket_copy;
    data->ticket_expires_at = expires_copy;

#ifdef CLOUDSYNC_NETWORK_TRACE
    fprintf(stderr,
            "[cloudsync-network] received_ticket=%s expires_at=%s\n",
            data->ticket ? "true" : "false", data->ticket_expires_at ? data->ticket_expires_at : "");
#endif
}

#ifndef CLOUDSYNC_OMIT_CURL
static bool network_curl_pool_enabled(network_data *data) {
    if (!data) return false;
    if (data->curl_pool_enabled == 0) {
        const char *value = getenv("CLOUDSYNC_CURL_POOL");
        data->curl_pool_enabled = network_env_disabled(value) ? -1 : 1;
    }
    return data->curl_pool_enabled > 0;
}

static CURL *network_curl_for_endpoint(network_data *data, const char *endpoint, bool *pooled) {
    if (pooled) *pooled = false;
    if (!network_curl_pool_enabled(data)) {
        return curl_easy_init();
    }

    CURL **slot = network_endpoint_is_api(data, endpoint) ? &data->api_curl : &data->artifact_curl;
    if (!*slot) {
        *slot = curl_easy_init();
    } else {
        curl_easy_reset(*slot);
    }
    if (!*slot) return NULL;

    curl_easy_setopt(*slot, CURLOPT_MAXCONNECTS, CLOUDSYNC_CURL_MAXCONNECTS);
    curl_easy_setopt(*slot, CURLOPT_MAXAGE_CONN, CLOUDSYNC_CURL_MAXAGE_CONN_SECONDS);
    curl_easy_setopt(*slot, CURLOPT_MAXLIFETIME_CONN, CLOUDSYNC_CURL_MAXLIFETIME_CONN_SECONDS);
    if (pooled) *pooled = true;
    return *slot;
}

static bool network_buffer_check (network_buffer *data, size_t needed) {
    // alloc/resize buffer
    if (data->bused + needed > data->balloc) {
        if (needed < CLOUDSYNC_NETWORK_MINBUF_SIZE) needed = CLOUDSYNC_NETWORK_MINBUF_SIZE;
        size_t balloc = data->balloc + needed;
        
        char *buffer = cloudsync_memory_realloc(data->buffer, balloc);
        if (!buffer) return false;
        
        data->buffer = buffer;
        data->balloc = balloc;
    }
    
    return true;
}

static size_t network_receive_callback (void *ptr, size_t size, size_t nmemb, void *xdata) {
    network_buffer *data = (network_buffer *)xdata;
    
    size_t ptr_size = (size*nmemb);
    if (data->zero_term) ptr_size += 1;
    
    if (network_buffer_check(data, ptr_size) == false) return CURL_WRITEFUNC_ERROR;
    memcpy(data->buffer+data->bused, ptr, size*nmemb);
    data->bused += size*nmemb;
    if (data->zero_term) data->buffer[data->bused] = 0;
    
    return (size * nmemb);
}

static bool network_header_eq(const char *line, size_t len, const char *name) {
    size_t name_len = strlen(name);
    if (len <= name_len || line[name_len] != ':') return false;
    for (size_t i = 0; i < name_len; i++) {
        if (tolower((unsigned char)line[i]) != tolower((unsigned char)name[i])) return false;
    }
    return true;
}

static char *network_header_value_dup(const char *line, size_t len, const char *name) {
    size_t name_len = strlen(name);
    const char *start = line + name_len + 1;
    const char *end = line + len;

    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t')) end--;

    size_t value_len = (size_t)(end - start);
    char *value = cloudsync_memory_zeroalloc(value_len + 1);
    if (!value) return NULL;
    memcpy(value, start, value_len);
    value[value_len] = '\0';
    return value;
}

static size_t network_header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
    network_ticket_headers *ticket_headers = (network_ticket_headers *)userdata;
    size_t len = size * nitems;

    if (network_header_eq(buffer, len, CLOUDSYNC_HEADER_TICKET)) {
        char *ticket = network_header_value_dup(buffer, len, CLOUDSYNC_HEADER_TICKET);
        if (ticket) {
            if (ticket_headers->ticket) cloudsync_memory_free(ticket_headers->ticket);
            ticket_headers->ticket = ticket;
        }
    } else if (network_header_eq(buffer, len, CLOUDSYNC_HEADER_TICKET_EXPIRES_AT)) {
        char *expires_at = network_header_value_dup(buffer, len, CLOUDSYNC_HEADER_TICKET_EXPIRES_AT);
        if (expires_at) {
            if (ticket_headers->expires_at) cloudsync_memory_free(ticket_headers->expires_at);
            ticket_headers->expires_at = expires_at;
        }
    }

    return len;
}

NETWORK_RESULT network_receive_buffer (network_data *data, const char *endpoint, const char *authentication, bool zero_terminated, bool is_post_request, char *json_payload, const char **extra_headers, int nextra_headers) {
    char *buffer = NULL;
    size_t blen = 0;
    struct curl_slist* headers = NULL;
    network_ticket_headers ticket_headers = {NULL, NULL};
    char errbuf[CURL_ERROR_SIZE] = {0};
    long response_code = 0;
    bool pooled = false;
    bool using_ticket = network_data_should_use_ticket(data, endpoint, authentication);
    const char *method = (json_payload || is_post_request) ? "POST" : "GET";
#ifndef CLOUDSYNC_NETWORK_TRACE
    (void)method;
#endif
#ifdef CLOUDSYNC_NETWORK_TRACE
    double trace_start_ms = network_trace_now_ms();
    size_t request_bytes = json_payload ? strlen(json_payload) : 0;
#endif

    CURL *curl = network_curl_for_endpoint(data, endpoint, &pooled);
    if (!curl) return (NETWORK_RESULT){CLOUDSYNC_NETWORK_ERROR, NULL, 0, NULL, NULL};
    
    // a buffer to store errors in
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    CURLcode rc = curl_easy_setopt(curl, CURLOPT_URL, endpoint);
    if (rc != CURLE_OK) goto cleanup;

    // set PEM
    #ifdef __ANDROID__
    struct curl_blob pem_blob = {
        .data = (void *)cacert_pem,
        .len = cacert_len,
        .flags = CURL_BLOB_NOCOPY
    };
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &pem_blob);
    #endif
    
    for (int i = 0; i < nextra_headers; i++) {
        struct curl_slist *tmp = curl_slist_append(headers, extra_headers[i]);
        if (!tmp) {rc = CURLE_OUT_OF_MEMORY; goto cleanup;}
        headers = tmp;
    }

    if (data->org_id) {
        char org_header[512];
        snprintf(org_header, sizeof(org_header), "%s: %s", CLOUDSYNC_HEADER_ORG, data->org_id);
        struct curl_slist *tmp = curl_slist_append(headers, org_header);
        if (!tmp) {rc = CURLE_OUT_OF_MEMORY; goto cleanup;}
        headers = tmp;
    }

    if (json_payload) {
        struct curl_slist *tmp = curl_slist_append(headers, "Content-Type: application/json");
        if (!tmp) {rc = CURLE_OUT_OF_MEMORY; goto cleanup;}
        headers = tmp;
    }
    if (authentication) {
        char auth_header[CLOUDSYNC_SESSION_TOKEN_MAXSIZE];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", authentication);
        struct curl_slist *tmp = curl_slist_append(headers, auth_header);
        if (!tmp) {rc = CURLE_OUT_OF_MEMORY; goto cleanup;}
        headers = tmp;
    }
    if (using_ticket) {
        char ticket_header[CLOUDSYNC_SESSION_TOKEN_MAXSIZE];
        snprintf(ticket_header, sizeof(ticket_header), "%s: %s", CLOUDSYNC_HEADER_TICKET, data->ticket);
        struct curl_slist *tmp = curl_slist_append(headers, ticket_header);
        if (!tmp) {rc = CURLE_OUT_OF_MEMORY; goto cleanup;}
        headers = tmp;
    }
    
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    
    network_buffer netdata = {NULL, 0, 0, (zero_terminated) ? 1 : 0};
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &netdata);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, network_receive_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ticket_headers);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, network_header_callback);

    // add optional JSON payload (implies setting CURLOPT_POST to 1)
    // or set the CURLOPT_POST option
    if (json_payload) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
    } else if (is_post_request) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    }
    
    // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (rc == CURLE_OK) {
        buffer = netdata.buffer;
        blen = netdata.bused;
        if (response_code < 400 && ticket_headers.ticket) {
            network_data_update_ticket(data, ticket_headers.ticket, ticket_headers.expires_at);
        }
    } else if (netdata.buffer) {
        cloudsync_memory_free(netdata.buffer);
        netdata.buffer = NULL;
    }

cleanup:
    if (headers) curl_slist_free_all(headers);
    if (ticket_headers.ticket) cloudsync_memory_free(ticket_headers.ticket);
    if (ticket_headers.expires_at) cloudsync_memory_free(ticket_headers.expires_at);
    
    // build result
    NETWORK_RESULT result = {0, NULL, 0, NULL, NULL};
    if (rc == CURLE_OK && response_code < 400) {
        result.code = (buffer && blen) ? CLOUDSYNC_NETWORK_BUFFER : CLOUDSYNC_NETWORK_OK;
        result.buffer = buffer;
        result.blen = blen;
    } else {
        result.code = CLOUDSYNC_NETWORK_ERROR;
        result.buffer = buffer ? buffer : (errbuf[0]) ? cloudsync_string_dup(errbuf) : NULL;
        result.blen = buffer ? blen : rc;
    }
    
    #ifdef CLOUDSYNC_NETWORK_TRACE
    fprintf(stderr,
            "[cloudsync-network] endpoint=%s using_ticket=%s\n",
            network_trace_endpoint_name(data, endpoint),
            using_ticket ? "true" : "false");
    network_trace_log_curl(data, method, endpoint, response_code, result.code, request_bytes, result.blen, curl, pooled, network_trace_now_ms() - trace_start_ms);
    #endif
    if (curl && !pooled) curl_easy_cleanup(curl);
    return result;
}

static size_t network_read_callback (char *buffer, size_t size, size_t nitems, void *userdata) {
    network_read_data *rd = (network_read_data *)userdata;
    size_t max_read = size * nitems;
    size_t bytes_left = rd->size - rd->read_pos;
    size_t to_copy = bytes_left < max_read ? bytes_left : max_read;
    
    if (to_copy > 0) {
        memcpy(buffer, rd->data + rd->read_pos, to_copy);
        rd->read_pos += to_copy;
    }
    
    return to_copy;
}

bool network_send_buffer (network_data *data, const char *endpoint, const char *authentication, const void *blob, int blob_size) {
    struct curl_slist *headers = NULL;
    bool result = false;
    char errbuf[CURL_ERROR_SIZE] = {0};
    CURLcode rc = CURLE_OK;
    long response_code = 0;
    bool pooled = false;
#ifdef CLOUDSYNC_NETWORK_TRACE
    double trace_start_ms = network_trace_now_ms();
#endif

    // init/reuse curl
    CURL *curl = network_curl_for_endpoint(data, endpoint, &pooled);
    if (!curl) return false;

    // set the URL
    if (curl_easy_setopt(curl, CURLOPT_URL, endpoint) != CURLE_OK) goto cleanup;
    
    // set PEM
    #ifdef __ANDROID__
    struct curl_blob pem_blob = {
        .data = (void *)cacert_pem,
        .len = cacert_len,
        .flags = CURL_BLOB_NOCOPY
    };
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &pem_blob);
    #endif
    
    // a buffer to store errors in
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    
    // type header
    struct curl_slist *tmp = curl_slist_append(headers, "Accept: text/plain");
    if (!tmp) {rc = CURLE_OUT_OF_MEMORY; goto cleanup;}
    headers = tmp;
    
    if (authentication) {
        // init authorization header
        char auth_header[CLOUDSYNC_SESSION_TOKEN_MAXSIZE];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", authentication);
        struct curl_slist *tmp = curl_slist_append(headers, auth_header);
        if (!tmp) {rc = CURLE_OUT_OF_MEMORY; goto cleanup;}
        headers = tmp;
    }

    if (data->org_id) {
        char org_header[512];
        snprintf(org_header, sizeof(org_header), "%s: %s", CLOUDSYNC_HEADER_ORG, data->org_id);
        struct curl_slist *tmp = curl_slist_append(headers, org_header);
        if (!tmp) {rc = CURLE_OUT_OF_MEMORY; goto cleanup;}
        headers = tmp;
    }

    // Set headers if needed (S3 pre-signed URLs usually do not require additional headers)
    tmp = curl_slist_append(headers, "Content-Type: application/octet-stream");
    if (!tmp) {rc = CURLE_OUT_OF_MEMORY; goto cleanup;}
    headers = tmp;
    
    if (!headers) goto cleanup;
    if (curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK) goto cleanup;
    
    // Set HTTP PUT method
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    
    // Set the size of the blob
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)blob_size);
    
    // Provide the data using a custom read callback
    network_read_data rdata = {
        .data = (const char *)blob,
        .size = blob_size,
        .read_pos = 0
    };
    
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, network_read_callback);
    curl_easy_setopt(curl, CURLOPT_READDATA, &rdata);
    
    // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    
    // perform the upload
    rc = curl_easy_perform(curl);
    if (curl) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (rc == CURLE_OK) result = true;
       
cleanup:
    #ifdef CLOUDSYNC_NETWORK_TRACE
    network_trace_log_curl(data, "PUT", endpoint, response_code,
                           result ? CLOUDSYNC_NETWORK_OK : CLOUDSYNC_NETWORK_ERROR,
                           (size_t)blob_size,
                           result ? (size_t)blob_size : 0,
                           curl, pooled, network_trace_now_ms() - trace_start_ms);
    #endif
    if (curl && !pooled) curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);
    return result;
}
#endif

int network_set_sqlite_result (sqlite3_context *context, NETWORK_RESULT *result) {
    int rc = 0;
    switch (result->code) {
        case CLOUDSYNC_NETWORK_OK:
            sqlite3_result_error_code(context, SQLITE_OK);
            sqlite3_result_int(context, 0);
            rc = 0;
            break;
            
        case CLOUDSYNC_NETWORK_ERROR:
            sqlite3_result_error(context, (result->buffer) ? result->buffer : "Memory error.", -1);
            sqlite3_result_error_code(context, SQLITE_ERROR);
            rc = -1;
            break;
            
        case CLOUDSYNC_NETWORK_BUFFER:
            sqlite3_result_error_code(context, SQLITE_OK);
            sqlite3_result_text(context, result->buffer, (int)result->blen, SQLITE_TRANSIENT);
            rc = (int)result->blen;
            break;
    }
    return rc;
}

// If err_out is non-NULL, cloudsync_payload_apply failures are returned via
// *err_out (malloc'd, caller must cloudsync_memory_free) instead of being raised
// on the sqlite3_context. This lets composite callers (cloudsync_network_sync)
// surface apply errors as structured JSON. Endpoint/network errors always raise
// a SQL error regardless of err_out.
int network_download_changes (sqlite3_context *context, const char *download_url, int *pnrows, char **err_out, int64_t checkpoint_db_version, int64_t checkpoint_seq, int64_t *pbytes) {
    DEBUG_FUNCTION("network_download_changes");

    if (pbytes) *pbytes = 0;

    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    network_data *netdata = (network_data *)cloudsync_auxdata(data);
    if (!netdata) {
        sqlite3_result_error(context, "Unable to retrieve network CloudSync context.", -1);
        return -1;
    }

    NETWORK_RESULT result = network_receive_buffer(netdata, download_url, NULL, false, false, NULL, NULL, 0);

    int rc = SQLITE_OK;
    if (result.code == CLOUDSYNC_NETWORK_BUFFER) {
        if (pbytes) *pbytes = (int64_t)result.blen;
        rc = cloudsync_payload_apply(data, result.buffer, (int)result.blen, pnrows, checkpoint_db_version, checkpoint_seq);
        if (rc != DBRES_OK) {
            const char *msg = cloudsync_errmsg(data);
            if (!msg || !msg[0]) msg = "cloudsync_payload_apply failed";
            if (err_out) *err_out = cloudsync_string_dup(msg);
            else sqlite3_result_error(context, msg, -1);
            if (pnrows) *pnrows = 0;
        }
    } else if (result.code == CLOUDSYNC_NETWORK_ERROR) {
        network_set_sqlite_result(context, &result);
        rc = -1;
        if (pnrows) *pnrows = 0;
    } else {
        // CLOUDSYNC_NETWORK_OK — no data, not an error
        if (pnrows) *pnrows = 0;
    }
    network_result_cleanup(&result);

    return rc;
}

static int network_apply_payload_buffer(sqlite3_context *context, const char *payload, int payload_size,
                                        int *pnrows, char **err_out,
                                        int64_t checkpoint_db_version, int64_t checkpoint_seq,
                                        const char *error_prefix) {
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    int rc = cloudsync_payload_apply(data, payload, payload_size, pnrows, checkpoint_db_version, checkpoint_seq);
    if (rc != DBRES_OK) {
        const char *msg = cloudsync_errmsg(data);
        if (!msg || !msg[0]) msg = error_prefix ? error_prefix : "cloudsync_payload_apply failed";
        if (err_out) *err_out = cloudsync_string_dup(msg);
        else sqlite3_result_error(context, msg, -1);
        if (pnrows) *pnrows = 0;
    }
    return rc;
}

char *network_authentication_token (const char *key, const char *value) {
    size_t len = strlen(key) + strlen(value) + 64;
    char *buffer = cloudsync_memory_zeroalloc(len);
    if (!buffer) return NULL;
    
    // build new token
    // we don't need a prefix because the token alreay include a prefix "sqa_"
    snprintf(buffer, len, "%s", value);
    return buffer;
}

// MARK: - JSON helpers (jsmn) -

static bool jsmn_token_eq(const char *json, const jsmntok_t *tok, const char *s) {
    return (tok->type == JSMN_STRING &&
            (int)strlen(s) == tok->end - tok->start &&
            strncmp(json + tok->start, s, tok->end - tok->start) == 0);
}

static int jsmn_find_key(const char *json, const jsmntok_t *tokens, int ntokens, const char *key) {
    for (int i = 1; i + 1 < ntokens; i++) {
        if (jsmn_token_eq(json, &tokens[i], key)) return i;
    }
    return -1;
}

static int jsmn_token_span(const jsmntok_t *tokens, int ntokens, int index) {
    if (!tokens || index < 0 || index >= ntokens) return 0;
    int start = tokens[index].start;
    int end = tokens[index].end;
    int i = index + 1;
    while (i < ntokens && tokens[i].start >= start && tokens[i].end <= end) i++;
    return i - index;
}

static bool jsmn_find_object_value(const char *json, const jsmntok_t *tokens, int ntokens,
                                   int object_index, const char *key, int *value_index) {
    if (!json || !tokens || !key || object_index < 0 || object_index >= ntokens ||
        tokens[object_index].type != JSMN_OBJECT) {
        return false;
    }

    int i = object_index + 1;
    for (int pair = 0; pair < tokens[object_index].size && i + 1 < ntokens; pair++) {
        if (jsmn_token_eq(json, &tokens[i], key)) {
            if (value_index) *value_index = i + 1;
            return true;
        }
        i++;
        i += jsmn_token_span(tokens, ntokens, i);
    }
    return false;
}

static jsmntok_t *json_parse_tokens_alloc(const char *json, size_t json_len, int *ntokens_out) {
    if (ntokens_out) *ntokens_out = 0;
    if (!json || json_len == 0 || !ntokens_out) return NULL;

    jsmn_parser parser;
    jsmn_init(&parser);
    int ntokens = jsmn_parse(&parser, json, json_len, NULL, 0);
    if (ntokens < 1) return NULL;

    jsmntok_t *tokens = cloudsync_memory_alloc((uint64_t)ntokens * sizeof(jsmntok_t));
    if (!tokens) return NULL;

    jsmn_init(&parser);
    int parsed = jsmn_parse(&parser, json, json_len, tokens, (unsigned int)ntokens);
    if (parsed < 1) {
        cloudsync_memory_free(tokens);
        return NULL;
    }

    *ntokens_out = parsed;
    return tokens;
}

static char *json_unescape_string(const char *src, int len) {
    char *out = cloudsync_memory_zeroalloc(len + 1);
    if (!out) return NULL;

    int j = 0;
    for (int i = 0; i < len; ) {
        if (src[i] == '\\' && i + 1 < len) {
            char c = src[i + 1];
            if (c == '"' || c == '\\' || c == '/') { out[j++] = c; i += 2; }
            else if (c == 'n') { out[j++] = '\n'; i += 2; }
            else if (c == 'r') { out[j++] = '\r'; i += 2; }
            else if (c == 't') { out[j++] = '\t'; i += 2; }
            else if (c == 'b') { out[j++] = '\b'; i += 2; }
            else if (c == 'f') { out[j++] = '\f'; i += 2; }
            else if (c == 'u' && i + 5 < len) {
                unsigned int cp = 0;
                for (int k = 0; k < 4; k++) {
                    char h = src[i + 2 + k];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= h - '0';
                    else if (h >= 'a' && h <= 'f') cp |= 10 + h - 'a';
                    else if (h >= 'A' && h <= 'F') cp |= 10 + h - 'A';
                }
                if (cp < 0x80) { out[j++] = (char)cp; }
                else { out[j++] = '?'; } // non-ASCII: replace
                i += 6;
            }
            else { out[j++] = src[i]; i++; }
        } else {
            out[j++] = src[i]; i++;
        }
    }
    out[j] = '\0';
    return out;
}

static char *json_extract_string(const char *json, size_t json_len, const char *key) {
    if (!json || json_len == 0 || !key) return NULL;

    int ntokens = 0;
    jsmntok_t *tokens = json_parse_tokens_alloc(json, json_len, &ntokens);
    if (!tokens) return NULL;

    char *result = NULL;
    int i = jsmn_find_key(json, tokens, ntokens, key);
    if (i >= 0 && i + 1 < ntokens) {
        jsmntok_t *val = &tokens[i + 1];
        if (val->type == JSMN_STRING)
            result = json_unescape_string(json + val->start, val->end - val->start);
    }
    cloudsync_memory_free(tokens);
    return result;
}

static int64_t json_extract_int(const char *json, size_t json_len, const char *key, int64_t default_value) {
    if (!json || json_len == 0 || !key) return default_value;

    int ntokens = 0;
    jsmntok_t *tokens = json_parse_tokens_alloc(json, json_len, &ntokens);
    if (!tokens) return default_value;

    int64_t result = default_value;
    if (tokens[0].type == JSMN_OBJECT) {
        int i = jsmn_find_key(json, tokens, ntokens, key);
        if (i >= 0 && i + 1 < ntokens && tokens[i + 1].type == JSMN_PRIMITIVE)
            result = strtoll(json + tokens[i + 1].start, NULL, 10);
    }
    cloudsync_memory_free(tokens);
    return result;
}

static bool json_extract_bool(const char *json, size_t json_len, const char *key, bool default_value) {
    if (!json || json_len == 0 || !key) return default_value;

    int ntokens = 0;
    jsmntok_t *tokens = json_parse_tokens_alloc(json, json_len, &ntokens);
    if (!tokens) return default_value;

    bool result = default_value;
    if (tokens[0].type == JSMN_OBJECT) {
        int i = jsmn_find_key(json, tokens, ntokens, key);
        if (i >= 0 && i + 1 < ntokens && tokens[i + 1].type == JSMN_PRIMITIVE) {
            // JSON booleans (true/false) and numeric flags (1/0) are both accepted.
            char c = json[tokens[i + 1].start];
            if (c == 't' || c == 'T') result = true;
            else if (c == 'f' || c == 'F' || c == 'n' || c == 'N') result = false;
            else result = strtoll(json + tokens[i + 1].start, NULL, 10) != 0;
        }
    }
    cloudsync_memory_free(tokens);
    return result;
}

static int json_extract_array_size(const char *json, size_t json_len, const char *key) {
    if (!json || json_len == 0 || !key) return -1;

    int ntokens = 0;
    jsmntok_t *tokens = json_parse_tokens_alloc(json, json_len, &ntokens);
    if (!tokens) return -1;

    int result = -1;
    if (tokens[0].type == JSMN_OBJECT) {
        int i = jsmn_find_key(json, tokens, ntokens, key);
        if (i >= 0 && i + 1 < ntokens && tokens[i + 1].type == JSMN_ARRAY)
            result = tokens[i + 1].size;
    }
    cloudsync_memory_free(tokens);
    return result;
}

// Escape a string for safe embedding as a JSON string value (without surrounding quotes).
// Caller must free with cloudsync_memory_free.
static char *json_escape_string(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    // worst case: every char becomes \uXXXX (6 bytes)
    char *out = cloudsync_memory_zeroalloc(len * 6 + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
            case '"':  out[j++] = '\\'; out[j++] = '"'; break;
            case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
            case '\b': out[j++] = '\\'; out[j++] = 'b'; break;
            case '\f': out[j++] = '\\'; out[j++] = 'f'; break;
            case '\n': out[j++] = '\\'; out[j++] = 'n'; break;
            case '\r': out[j++] = '\\'; out[j++] = 'r'; break;
            case '\t': out[j++] = '\\'; out[j++] = 't'; break;
            default:
                if (c < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    out[j++] = '\\'; out[j++] = 'u';
                    out[j++] = '0'; out[j++] = '0';
                    out[j++] = hex[(c >> 4) & 0xf];
                    out[j++] = hex[c & 0xf];
                } else {
                    out[j++] = (char)c;
                }
        }
    }
    out[j] = '\0';
    return out;
}

// Returns a malloc'd copy of the raw JSON substring for an object-valued key
// (found at any depth). Caller must free with cloudsync_memory_free.
static char *json_extract_object_raw(const char *json, size_t json_len, const char *key) {
    if (!json || json_len == 0 || !key) return NULL;

    int ntokens = 0;
    jsmntok_t *tokens = json_parse_tokens_alloc(json, json_len, &ntokens);
    if (!tokens) return NULL;

    char *out = NULL;
    int i = jsmn_find_key(json, tokens, ntokens, key);
    if (i >= 0 && i + 1 < ntokens) {
        jsmntok_t *val = &tokens[i + 1];
        int len = val->end - val->start;
        if (val->type == JSMN_OBJECT && len > 0) {
            out = cloudsync_memory_zeroalloc(len + 1);
            if (out) {
                memcpy(out, json + val->start, len);
                out[len] = '\0';
            }
        }
    }
    cloudsync_memory_free(tokens);
    return out;
}

int network_extract_query_param (const char *query, const char *key, char *output, size_t output_size) {
    if (!query || !key || !output || output_size == 0) {
        return -1; // Invalid input
    }

    size_t key_len = strlen(key);
    const char *p = query;
    #ifdef SQLITE_WASM_EXTRA_INIT
    if (*p == '?') p++;
    #endif

    while (p && *p) {
        // Find the start of a key=value pair
        const char *key_start = p;
        const char *eq = strchr(key_start, '=');
        if (!eq) break; // No '=' found, malformed query string

        size_t current_key_len = eq - key_start;
        
        // Check if the key matches (ensuring it's the full key)
        if (current_key_len == key_len && strncmp(key_start, key, key_len) == 0) {
            // Extract the value
            const char *value_start = eq + 1;
            const char *end = strchr(value_start, '&'); // Find end of value

            size_t value_len = (end) ? (size_t)(end - value_start) : strlen(value_start);
            if (value_len >= output_size) {
                return -2; // Output buffer too small
            }

            strncpy(output, value_start, value_len);
            output[value_len] = '\0'; // Null-terminate
            return 0; // Success
        }

        // Move to the next parameter
        p = strchr(p, '&');
        if (p) p++; // Skip '&'
    }

    return -3; // Key not found
}

static bool network_compute_endpoints_with_address (sqlite3_context *context, network_data *data, const char *address, const char *managedDatabaseId) {
    if (!managedDatabaseId || managedDatabaseId[0] == '\0') {
        sqlite3_result_error(context, "managedDatabaseId cannot be empty", -1);
        sqlite3_result_error_code(context, SQLITE_ERROR);
        return false;
    }

    if (!address || address[0] == '\0') {
        sqlite3_result_error(context, "address cannot be empty", -1);
        sqlite3_result_error_code(context, SQLITE_ERROR);
        return false;
    }

    // build endpoints: {address}/v2/cloudsync/databases/{managedDatabaseId}/{siteId}/{action}
    size_t requested = strlen(address) + 1
                     + strlen(CLOUDSYNC_ENDPOINT_PREFIX) + 1 + strlen(managedDatabaseId) + 1
                     + UUID_STR_MAXLEN + 1 + 16;
    char *check_endpoint = (char *)cloudsync_memory_zeroalloc(requested);
    char *upload_endpoint = (char *)cloudsync_memory_zeroalloc(requested);
    char *apply_endpoint = (char *)cloudsync_memory_zeroalloc(requested);
    char *status_endpoint = (char *)cloudsync_memory_zeroalloc(requested);

    if (!check_endpoint || !upload_endpoint || !apply_endpoint || !status_endpoint) {
        sqlite3_result_error_code(context, SQLITE_NOMEM);
        if (check_endpoint) cloudsync_memory_free(check_endpoint);
        if (upload_endpoint) cloudsync_memory_free(upload_endpoint);
        if (apply_endpoint) cloudsync_memory_free(apply_endpoint);
        if (status_endpoint) cloudsync_memory_free(status_endpoint);
        return false;
    }

    // format: {address}/v2/cloudsync/databases/{managedDatabaseID}/{siteId}/{action}
    snprintf(check_endpoint, requested, "%s/%s/%s/%s/%s",
             address, CLOUDSYNC_ENDPOINT_PREFIX, managedDatabaseId, data->site_id, CLOUDSYNC_ENDPOINT_CHECK);
    snprintf(upload_endpoint, requested, "%s/%s/%s/%s/%s",
             address, CLOUDSYNC_ENDPOINT_PREFIX, managedDatabaseId, data->site_id, CLOUDSYNC_ENDPOINT_UPLOAD);
    snprintf(apply_endpoint, requested, "%s/%s/%s/%s/%s",
             address, CLOUDSYNC_ENDPOINT_PREFIX, managedDatabaseId, data->site_id, CLOUDSYNC_ENDPOINT_APPLY);
    snprintf(status_endpoint, requested, "%s/%s/%s/%s/%s",
             address, CLOUDSYNC_ENDPOINT_PREFIX, managedDatabaseId, data->site_id, CLOUDSYNC_ENDPOINT_STATUS);

    network_data_clear_ticket(data);

    if (data->check_endpoint) cloudsync_memory_free(data->check_endpoint);
    data->check_endpoint = check_endpoint;

    if (data->upload_endpoint) cloudsync_memory_free(data->upload_endpoint);
    data->upload_endpoint = upload_endpoint;

    if (data->apply_endpoint) cloudsync_memory_free(data->apply_endpoint);
    data->apply_endpoint = apply_endpoint;

    if (data->status_endpoint) cloudsync_memory_free(data->status_endpoint);
    data->status_endpoint = status_endpoint;

    return true;
}

void network_result_to_sqlite_error (sqlite3_context *context, NETWORK_RESULT res, const char *default_error_message) {
    sqlite3_result_error(context, ((res.code == CLOUDSYNC_NETWORK_ERROR) && (res.buffer)) ? res.buffer : default_error_message, -1);
    sqlite3_result_error_code(context, SQLITE_ERROR);
}

// MARK: - Init / Cleanup -

network_data *cloudsync_network_data (sqlite3_context *context) {
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    network_data *netdata = (network_data *)cloudsync_auxdata(data);
    if (netdata) return netdata;
    
    netdata = (network_data *)cloudsync_memory_zeroalloc(sizeof(network_data));
    if (netdata) cloudsync_set_auxdata(data, netdata);
    return netdata;
}

static void cloudsync_network_init_internal (sqlite3_context *context, const char *address, const char *managedDatabaseId) {
    #ifndef CLOUDSYNC_OMIT_CURL
    curl_global_init(CURL_GLOBAL_ALL);
    #endif

    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    network_data *netdata = cloudsync_network_data(context);
    if (!netdata) goto abort_memory;

    // init context
    uint8_t *site_id = (uint8_t *)cloudsync_context_init(data);
    if (!site_id) goto abort_siteid;

    // save site_id string representation: 01957493c6c07e14803727e969f1d2cc
    cloudsync_uuid_v7_stringify(site_id, netdata->site_id, false);

    // compute endpoints
    // authentication can be set later via cloudsync_network_set_token/cloudsync_network_set_apikey
    if (network_compute_endpoints_with_address(context, netdata, address, managedDatabaseId) == false) {
        goto abort_cleanup;
    }

    cloudsync_set_auxdata(data, netdata);
    sqlite3_result_int(context, SQLITE_OK);
    return;

abort_memory:
    sqlite3_result_error(context, "Unable to allocate memory in cloudsync_network_init.", -1);
    sqlite3_result_error_code(context, SQLITE_NOMEM);
    goto abort_cleanup;

abort_siteid:
    sqlite3_result_error(context, "Unable to compute/retrieve site_id.", -1);
    sqlite3_result_error_code(context, SQLITE_MISUSE);
    goto abort_cleanup;

abort_cleanup:
    cloudsync_set_auxdata(data, NULL);
    network_data_free(netdata);
}

void cloudsync_network_init (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_init");
    const char *managedDatabaseId = (const char *)sqlite3_value_text(argv[0]);
    cloudsync_network_init_internal(context, CLOUDSYNC_DEFAULT_ADDRESS, managedDatabaseId);
}

void cloudsync_network_init_custom (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_init_custom");
    const char *address = (const char *)sqlite3_value_text(argv[0]);
    const char *managedDatabaseId = (const char *)sqlite3_value_text(argv[1]);
    cloudsync_network_init_internal(context, address, managedDatabaseId);
}

void cloudsync_network_cleanup_internal (sqlite3_context *context) {    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    network_data *netdata = cloudsync_network_data(context);
    cloudsync_set_auxdata(data, NULL);
    network_data_free(netdata);
    
    #ifndef CLOUDSYNC_OMIT_CURL
    curl_global_cleanup();
    #endif
}

void cloudsync_network_cleanup (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_cleanup");
    
    cloudsync_network_cleanup_internal(context);
    sqlite3_result_int(context, SQLITE_OK);
}

// MARK: - Public -

bool cloudsync_network_set_authentication_token (sqlite3_context *context, const char *value, bool is_token) {
    network_data *data = cloudsync_network_data(context);
    if (!data) return false;
   
    const char *key = (is_token) ? "token" : "apikey";
    char *new_auth_token = network_authentication_token(key, value);
    if (!new_auth_token) return false;
    
    if (data->authentication) cloudsync_memory_free(data->authentication);
    network_data_clear_ticket(data);
    data->authentication = new_auth_token;
    
    return true;
}

void cloudsync_network_set_token (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_set_token");
    
    const char *value = (const char *)sqlite3_value_text(argv[0]);
    bool result = cloudsync_network_set_authentication_token(context, value, true);
    (result) ? sqlite3_result_int(context, SQLITE_OK) : sqlite3_result_error_code(context, SQLITE_NOMEM);
}

void cloudsync_network_set_apikey (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_set_apikey");

    const char *value = (const char *)sqlite3_value_text(argv[0]);
    bool result = cloudsync_network_set_authentication_token(context, value, false);
    (result) ? sqlite3_result_int(context, SQLITE_OK) : sqlite3_result_error_code(context, SQLITE_NOMEM);
}

// Returns a malloc'd JSON array string like '["tasks","users"]', or NULL on error/no results.
// Caller must free with cloudsync_memory_free.
static char *network_get_affected_tables(sqlite3 *db, int64_t since_db_version) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT json_group_array(DISTINCT tbl) FROM cloudsync_changes WHERE db_version > ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    sqlite3_bind_int64(stmt, 1, since_db_version);

    char *result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *json = (const char *)sqlite3_column_text(stmt, 0);
        if (json) result = cloudsync_string_dup(json);
    }
    sqlite3_finalize(stmt);
    return result;
}

// MARK: - Sync result
//
// Error-handling contract for send/check/sync functions:
//  - Endpoint/network errors (server unreachable, auth failure, bad URL)
//    always raise a SQL error via sqlite3_result_error.
//  - cloudsync_payload_apply failures (unknown schema hash, invalid checksum,
//    decompression error) are returned as structured JSON via receive.error.
//  - Server-reported failures from the SyncStatusResponse failures object are
//    forwarded as send.lastFailure (failures.apply) and receive.lastFailure
//    (failures.check). Per-function scoping: send_changes emits send.lastFailure
//    only; receive_changes emits receive.lastFailure only; sync emits both.
//  - A non-retryable failures.check (retryable:false) is a permanent
//    configuration/authorization problem: the receive drain stops polling
//    immediately rather than waiting it out. receive_changes raises it as a SQL
//    error (fail fast — no send block to preserve); sync still emits structured
//    JSON with receive.lastFailure so its send block survives.
//
// Callers that receive JSON can trust that the server was reachable.
// A SQL error means connectivity or configuration is broken.

typedef struct {
    int64_t     server_version;     // lastOptimisticVersion
    int64_t     local_version;      // new_db_version (max local)
    const char  *status;            // computed status string
    int         rows_received;      // rows from check
    char        *tables_json;       // JSON array of affected table names, caller must cloudsync_memory_free
    char        *apply_failure_json; // raw JSON object for server-reported failures.apply, caller must cloudsync_memory_free
    char        *check_failure_json; // raw JSON object for server-reported failures.check, caller must cloudsync_memory_free
    // Per-call chunked-download signals written by cloudsync_network_check_internal.
    bool        page_delivered;     // one or more url/inline chunks were fetched + applied this call (rc==OK)
    bool        more_pending;       // this was a non-final chunked batch with a next cursor (drain should continue)
    bool        defer_tables;       // input: when true, check_internal skips its per-call tables query
                                    // so a multi-chunk drain can compute the union once at the end
    int64_t     bytes_received;     // serialized payload bytes received this call (0 on 202/no page)
    int         chunks_received;    // payload chunks received this call (0 on 202/no page)
    // Send-path transport volume, written by cloudsync_network_send_changes_internal.
    int         send_chunks;        // number of payload chunks sent this call
    int64_t     send_bytes;         // serialized payload bytes sent this call
} sync_result;

// Returns a malloc'd raw JSON copy of failures.<stage_key> ("apply" or "check"),
// or NULL when the field is missing or is JSON null. Caller frees with cloudsync_memory_free.
static char *json_extract_failure_stage(const char *json, size_t json_len, const char *stage_key) {
    if (!json || json_len == 0 || !stage_key) return NULL;

    char *failures = json_extract_object_raw(json, json_len, "failures");
    if (!failures) return NULL;

    char *stage = json_extract_object_raw(failures, strlen(failures), stage_key);
    cloudsync_memory_free(failures);
    return stage;
}

static char *network_base64_encode(const unsigned char *src, size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    if (!src && len > 0) return NULL;
    if (len > (SIZE_MAX - 1) / 4 * 3) return NULL;

    size_t out_len = 4 * ((len + 2) / 3);
    char *out = cloudsync_memory_alloc((uint64_t)out_len + 1);
    if (!out) return NULL;

    size_t i = 0;
    size_t j = 0;
    while (i < len) {
        uint32_t octet_a = i < len ? src[i++] : 0;
        uint32_t octet_b = i < len ? src[i++] : 0;
        uint32_t octet_c = i < len ? src[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out[j++] = table[(triple >> 18) & 0x3f];
        out[j++] = table[(triple >> 12) & 0x3f];
        out[j++] = table[(triple >> 6) & 0x3f];
        out[j++] = table[triple & 0x3f];
    }

    if (len % 3 == 1) {
        out[out_len - 1] = '=';
        out[out_len - 2] = '=';
    } else if (len % 3 == 2) {
        out[out_len - 1] = '=';
    }

    out[out_len] = '\0';
    return out;
}

static int network_base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static unsigned char *network_base64_decode(const char *src, size_t len, size_t *out_len) {
    if (!src || !out_len) return NULL;
    *out_len = 0;

    size_t effective_len = 0;
    for (size_t i = 0; i < len; ++i) {
        if (!isspace((unsigned char)src[i])) ++effective_len;
    }
    if (effective_len == 0 || effective_len % 4 != 0) return NULL;

    size_t max_out_len = (effective_len / 4) * 3;
    unsigned char *out = cloudsync_memory_alloc((uint64_t)max_out_len);
    if (!out) return NULL;

    int quartet[4];
    int q = 0;
    size_t j = 0;
    bool seen_padding = false;

    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (isspace(c)) continue;

        if (c == '=') {
            quartet[q++] = -2;
            seen_padding = true;
        } else {
            int v = network_base64_value((char)c);
            if (v < 0 || seen_padding) goto invalid;
            quartet[q++] = v;
        }

        if (q == 4) {
            if (quartet[0] < 0 || quartet[1] < 0) goto invalid;
            if (quartet[2] == -2 && quartet[3] != -2) goto invalid;

            uint32_t triple = ((uint32_t)quartet[0] << 18) | ((uint32_t)quartet[1] << 12);
            out[j++] = (unsigned char)((triple >> 16) & 0xff);

            if (quartet[2] >= 0) {
                triple |= (uint32_t)quartet[2] << 6;
                out[j++] = (unsigned char)((triple >> 8) & 0xff);
            }
            if (quartet[3] >= 0) {
                triple |= (uint32_t)quartet[3];
                out[j++] = (unsigned char)(triple & 0xff);
            }
            q = 0;
        }
    }

    if (q != 0) goto invalid;
    *out_len = j;
    return out;

invalid:
    cloudsync_memory_free(out);
    *out_len = 0;
    return NULL;
}

static int network_apply_check_chunk(sqlite3_context *context, const char *chunk_json, size_t chunk_json_len,
                                     bool final_chunk, int *pnrows, char **err_out, int64_t *pbytes) {
    if (pnrows) *pnrows = 0;
    if (pbytes) *pbytes = 0;

    char *download_url = json_extract_string(chunk_json, chunk_json_len, "url");
    char *inline_payload = download_url ? NULL : json_extract_string(chunk_json, chunk_json_len, "payload");
    if (!download_url && !inline_payload) {
        sqlite3_result_error(context, "cloudsync_network_receive_changes: check chunk is missing url or payload.", -1);
        return SQLITE_ERROR;
    }

    // A non-final chunk must never advance the receive cursor (see cloudsync.h):
    // landing mid-db_version would let the next /check skip the unapplied
    // remainder. Only the final chunk advances -- to the explicit watermark, or
    // the legacy last-applied fallback when it is absent.
    int64_t watermark = json_extract_int(chunk_json, chunk_json_len, "watermark", -1);
    int64_t checkpoint_db_version;
    if (!final_chunk) {
        checkpoint_db_version = CLOUDSYNC_CHECKPOINT_NONE;
    } else {
        checkpoint_db_version = (watermark < 0) ? CLOUDSYNC_CHECKPOINT_LAST_APPLIED : watermark;
    }
    int64_t checkpoint_seq = 0;

    int rc = SQLITE_OK;
    if (download_url) {
        rc = network_download_changes(context, download_url, pnrows, err_out,
                                      checkpoint_db_version, checkpoint_seq, pbytes);
    } else {
        size_t decoded_size = 0;
        unsigned char *decoded = network_base64_decode(inline_payload, strlen(inline_payload), &decoded_size);
        if (!decoded || decoded_size > INT_MAX) {
            if (decoded) cloudsync_memory_free(decoded);
            sqlite3_result_error(context, "cloudsync_network_receive_changes: invalid inline payload in check response.", -1);
            rc = SQLITE_ERROR;
        } else {
            if (pbytes) *pbytes = (int64_t)decoded_size;
            rc = network_apply_payload_buffer(context, (const char *)decoded, (int)decoded_size, pnrows, err_out,
                                              checkpoint_db_version, checkpoint_seq,
                                              "cloudsync_network_receive_changes: inline payload apply failed");
            cloudsync_memory_free(decoded);
        }
    }

    #ifdef CLOUDSYNC_NETWORK_TRACE
    fprintf(stderr,
        "[cloudsync-network] check chunk transport=%s chunked=%d final=%d rc=%d\n",
        download_url ? "url" : (inline_payload ? "inline" : "none"),
        (watermark >= 0), final_chunk, rc);
    #endif

    if (download_url) cloudsync_memory_free(download_url);
    if (inline_payload) cloudsync_memory_free(inline_payload);
    return rc;
}

static char *network_apply_json_payload(const char *transport_key, const char *transport_value,
                                        int64_t db_version_min, int64_t db_version_max) {
    if (!transport_key || !transport_value) return NULL;

    char *escaped_value = json_escape_string(transport_value);
    if (!escaped_value) return NULL;

    size_t requested = strlen(transport_key) + strlen(escaped_value) + 128;
    char *json_payload = cloudsync_memory_alloc((uint64_t)requested);
    if (!json_payload) {
        cloudsync_memory_free(escaped_value);
        return NULL;
    }

    snprintf(json_payload, requested,
             "{\"%s\":\"%s\", \"dbVersionMin\":%" PRId64 ", \"dbVersionMax\":%" PRId64 "}",
             transport_key, escaped_value, db_version_min, db_version_max);

    cloudsync_memory_free(escaped_value);
    return json_payload;
}

static int network_send_payload_to_apply(sqlite3_context *context, network_data *netdata,
                                         const void *blob, int blob_size,
                                         int64_t db_version_min, int64_t db_version_max,
                                         NETWORK_RESULT *res_out) {
    memset(res_out, 0, sizeof(*res_out));
    if (!blob || blob_size <= 0) {
        sqlite3_result_error(context, "cloudsync_network_send_changes: invalid empty payload chunk.", -1);
        return SQLITE_ERROR;
    }

    #ifdef CLOUDSYNC_NETWORK_TRACE
    fprintf(stderr,
        "[cloudsync-network] send_changes chunk_size=%d fast-lane:%s db_version_min=%" PRId64 " db_version_max=%" PRId64 "\n",
        blob_size,
        blob_size <= CLOUDSYNC_NETWORK_FAST_LANE_MAX_BLOB_SIZE ? "true" : "false",
        db_version_min,
        db_version_max);
    #endif

    if (blob_size <= CLOUDSYNC_NETWORK_FAST_LANE_MAX_BLOB_SIZE) {
        char *blob_base64 = network_base64_encode((const unsigned char *)blob, (size_t)blob_size);
        if (!blob_base64) {
            sqlite3_result_error(context, "cloudsync_network_send_changes: unable to encode payload chunk.", -1);
            sqlite3_result_error_code(context, SQLITE_NOMEM);
            return SQLITE_NOMEM;
        }

        char *json_payload = network_apply_json_payload("blob", blob_base64, db_version_min, db_version_max);
        cloudsync_memory_free(blob_base64);
        if (!json_payload) {
            sqlite3_result_error(context, "cloudsync_network_send_changes: unable to allocate apply request payload.", -1);
            sqlite3_result_error_code(context, SQLITE_NOMEM);
            return SQLITE_NOMEM;
        }

        *res_out = network_receive_buffer(netdata, netdata->apply_endpoint, netdata->authentication, true, true,
                                          json_payload, cloudsync_default_headers, ARRAY_LEN(cloudsync_default_headers));
        cloudsync_memory_free(json_payload);
        return SQLITE_OK;
    }

    NETWORK_RESULT upload_res = network_receive_buffer(netdata, netdata->upload_endpoint, netdata->authentication, true, false,
                                                       NULL, cloudsync_default_headers, ARRAY_LEN(cloudsync_default_headers));
    if (upload_res.code != CLOUDSYNC_NETWORK_BUFFER) {
        network_result_to_sqlite_error(context, upload_res, "cloudsync_network_send_changes unable to receive upload URL");
        network_result_cleanup(&upload_res);
        return SQLITE_ERROR;
    }

    char *s3_url = json_extract_string(upload_res.buffer, upload_res.blen, "url");
    if (!s3_url) {
        sqlite3_result_error(context, "cloudsync_network_send_changes: missing 'url' in upload response.", -1);
        network_result_cleanup(&upload_res);
        return SQLITE_ERROR;
    }

    bool sent = network_send_buffer(netdata, s3_url, NULL, blob, blob_size);
    if (sent == false) {
        cloudsync_memory_free(s3_url);
        network_result_to_sqlite_error(context, upload_res, "cloudsync_network_send_changes unable to upload payload chunk to remote host.");
        network_result_cleanup(&upload_res);
        return SQLITE_ERROR;
    }

    char *json_payload = network_apply_json_payload("url", s3_url, db_version_min, db_version_max);
    cloudsync_memory_free(s3_url);
    if (!json_payload) {
        sqlite3_result_error(context, "cloudsync_network_send_changes: unable to allocate apply request payload.", -1);
        sqlite3_result_error_code(context, SQLITE_NOMEM);
        network_result_cleanup(&upload_res);
        return SQLITE_NOMEM;
    }

    network_result_cleanup(&upload_res);
    *res_out = network_receive_buffer(netdata, netdata->apply_endpoint, netdata->authentication, true, true,
                                      json_payload, cloudsync_default_headers, ARRAY_LEN(cloudsync_default_headers));
    cloudsync_memory_free(json_payload);
    return SQLITE_OK;
}

static void network_sync_state_update_from_response(NETWORK_RESULT *res,
                                                    int64_t *last_optimistic_version,
                                                    int64_t *last_confirmed_version,
                                                    int *gaps_size,
                                                    char **apply_failure_json,
                                                    char **check_failure_json) {
    if (!res || res->code != CLOUDSYNC_NETWORK_BUFFER || !res->buffer) return;

    int64_t parsed_version = json_extract_int(res->buffer, res->blen, "lastOptimisticVersion", -1);
    if (parsed_version > *last_optimistic_version) *last_optimistic_version = parsed_version;
    parsed_version = json_extract_int(res->buffer, res->blen, "lastConfirmedVersion", -1);
    if (parsed_version > *last_confirmed_version) *last_confirmed_version = parsed_version;
    int parsed_gaps_size = json_extract_array_size(res->buffer, res->blen, "gaps");
    if (parsed_gaps_size >= 0) *gaps_size = parsed_gaps_size;

    char *apply_failure = json_extract_failure_stage(res->buffer, res->blen, "apply");
    if (apply_failure) {
        if (*apply_failure_json) cloudsync_memory_free(*apply_failure_json);
        *apply_failure_json = apply_failure;
    }

    char *check_failure = json_extract_failure_stage(res->buffer, res->blen, "check");
    if (check_failure) {
        if (*check_failure_json) cloudsync_memory_free(*check_failure_json);
        *check_failure_json = check_failure;
    }
}

static const char *network_compute_status(int64_t last_optimistic, int64_t last_confirmed,
                                           int gaps_size, int64_t local_version) {
    if (last_optimistic < 0 || last_confirmed < 0) return "error";
    if (gaps_size > 0 || last_optimistic < local_version) return "out-of-sync";
    if (last_optimistic == last_confirmed) return "synced";
    return "syncing";
}

// MARK: -

void cloudsync_network_has_unsent_changes (sqlite3_context *context, int argc, sqlite3_value **argv) {
    sqlite3 *db = sqlite3_context_db_handle(context);
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    network_data *netdata = (network_data *)cloudsync_auxdata(data);
    if (!netdata) {sqlite3_result_error(context, "Unable to retrieve CloudSync network context.", -1); return;}
    
    char *sql = "SELECT max(db_version) FROM cloudsync_changes WHERE site_id == (SELECT site_id FROM cloudsync_site_id WHERE rowid=0)";
    int64_t last_local_change = 0;
    int rc = database_select_int(data, sql, &last_local_change);
    if (rc != DBRES_OK) {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        sqlite3_result_error_code(context, rc);
        return;
    }
    
    if (last_local_change == 0) {
        sqlite3_result_int(context, 0);
        return;
    }
    
    NETWORK_RESULT res = network_receive_buffer(netdata, netdata->status_endpoint, netdata->authentication, true, false, NULL, cloudsync_default_headers, ARRAY_LEN(cloudsync_default_headers));

    int64_t last_optimistic_version = -1;

    if (res.code == CLOUDSYNC_NETWORK_BUFFER && res.buffer) {
        last_optimistic_version = json_extract_int(res.buffer, res.blen, "lastOptimisticVersion", -1);
    } else if (res.code != CLOUDSYNC_NETWORK_OK) {
        network_result_to_sqlite_error(context, res, "unable to retrieve current status from remote host.");
        network_result_cleanup(&res);
        return;
    }
    
    network_result_cleanup(&res);
    sqlite3_result_int(context, (last_optimistic_version >= 0 && last_optimistic_version < last_local_change));
}

int cloudsync_network_send_changes_internal (sqlite3_context *context, int argc, sqlite3_value **argv, sync_result *out) {
    DEBUG_FUNCTION("cloudsync_network_send_changes");
    UNUSED_PARAMETER(argc);
    UNUSED_PARAMETER(argv);
    
    // retrieve global context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    network_data *netdata = (network_data *)cloudsync_auxdata(data);
    if (!netdata) {sqlite3_result_error(context, "Unable to retrieve CloudSync network context.", -1); return SQLITE_ERROR;}
    
    int64_t db_version = dbutils_settings_get_int64_value(data, CLOUDSYNC_KEY_SEND_DBVERSION);
    if (db_version < 0) {
        sqlite3_result_error(context, "Unable to retrieve db_version.", -1);
        return SQLITE_ERROR;
    }

    sqlite3 *db = sqlite3_context_db_handle(context);
    sqlite3_stmt *stmt = NULL;
    const char *chunk_sql =
        "SELECT payload, payload_size, db_version_min, db_version_max, watermark_db_version "
        "FROM cloudsync_payload_chunks WHERE since_db_version = ?";
    int rc = sqlite3_prepare_v2(db, chunk_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        sqlite3_result_error_code(context, rc);
        return rc;
    }
    sqlite3_bind_int64(stmt, 1, db_version);

    int64_t new_db_version = db_version;
    int64_t last_optimistic_version = -1;
    int64_t last_confirmed_version = -1;
    int gaps_size = -1;
    char *apply_failure_json = NULL;
    char *check_failure_json = NULL;
    bool sent_any = false;
    int sent_chunks = 0;       // payload chunks sent this call
    int64_t sent_bytes = 0;    // serialized payload bytes sent this call

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(stmt, 0);
        int blob_size = sqlite3_column_bytes(stmt, 0);
        int64_t payload_size = sqlite3_column_int64(stmt, 1);
        int64_t db_version_min = sqlite3_column_int64(stmt, 2);
        int64_t db_version_max = sqlite3_column_int64(stmt, 3);
        int64_t watermark = sqlite3_column_int64(stmt, 4);

        if (!blob || blob_size <= 0 || payload_size != blob_size || payload_size > INT_MAX ||
            db_version_min <= 0 || db_version_max <= 0 || db_version_min > db_version_max) {
            sqlite3_result_error(context, "cloudsync_network_send_changes: invalid payload chunk generated.", -1);
            rc = SQLITE_ERROR;
            goto cleanup;
        }

        NETWORK_RESULT res = {0};
        rc = network_send_payload_to_apply(context, netdata, blob, blob_size, db_version_min, db_version_max, &res);
        if (rc != SQLITE_OK) goto cleanup;

        if (res.code == CLOUDSYNC_NETWORK_BUFFER && res.buffer) {
            network_sync_state_update_from_response(&res, &last_optimistic_version, &last_confirmed_version, &gaps_size,
                                                    &apply_failure_json, &check_failure_json);
        } else if (res.code != CLOUDSYNC_NETWORK_OK) {
            network_result_to_sqlite_error(context, res, "cloudsync_network_send_changes unable to apply changes to remote host.");
            network_result_cleanup(&res);
            rc = SQLITE_ERROR;
            goto cleanup;
        }
        network_result_cleanup(&res);

        sent_any = true;
        sent_chunks++;
        sent_bytes += payload_size;
        if (watermark > new_db_version) new_db_version = watermark;
    }
    if (rc != SQLITE_DONE) {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        sqlite3_result_error_code(context, rc);
        goto cleanup;
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    if (!sent_any) {
        // Empty local db with no server state: preserve the previous fast no-op path.
        if (db_version == 0) {
            if (out) {
                out->server_version = 0;
                out->local_version = 0;
                out->status = network_compute_status(0, 0, 0, 0);
            }
            rc = SQLITE_OK;
            goto cleanup;
        }

        NETWORK_RESULT res = network_receive_buffer(netdata, netdata->status_endpoint, netdata->authentication, true, false,
                                                    NULL, cloudsync_default_headers, ARRAY_LEN(cloudsync_default_headers));
        if (res.code == CLOUDSYNC_NETWORK_BUFFER && res.buffer) {
            network_sync_state_update_from_response(&res, &last_optimistic_version, &last_confirmed_version, &gaps_size,
                                                    &apply_failure_json, &check_failure_json);
        } else if (res.code != CLOUDSYNC_NETWORK_OK) {
            network_result_to_sqlite_error(context, res, "cloudsync_network_send_changes unable to apply changes to remote host.");
            network_result_cleanup(&res);
            rc = SQLITE_ERROR;
            goto cleanup;
        }
        network_result_cleanup(&res);
    }
    if (gaps_size < 0) gaps_size = 0;

    // update db_version in settings
    char buf[256];
    if (last_optimistic_version >= 0) {
        if (last_optimistic_version != db_version) {
            snprintf(buf, sizeof(buf), "%" PRId64, last_optimistic_version);
            dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_SEND_DBVERSION, buf);
        }
    } else if (new_db_version != db_version) {
        snprintf(buf, sizeof(buf), "%" PRId64, new_db_version);
        dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_SEND_DBVERSION, buf);
    }

    // populate sync result
    if (out) {
        out->server_version = last_optimistic_version;
        out->local_version = new_db_version;
        out->status = network_compute_status(last_optimistic_version, last_confirmed_version, gaps_size, new_db_version);
        out->send_chunks = sent_chunks;
        out->send_bytes = sent_bytes;
        out->apply_failure_json = apply_failure_json;
        out->check_failure_json = check_failure_json;
        apply_failure_json = NULL;
        check_failure_json = NULL;
    }
    if (apply_failure_json) cloudsync_memory_free(apply_failure_json);
    if (check_failure_json) cloudsync_memory_free(check_failure_json);
    return SQLITE_OK;

cleanup:
    if (stmt) sqlite3_finalize(stmt);
    if (apply_failure_json) cloudsync_memory_free(apply_failure_json);
    if (check_failure_json) cloudsync_memory_free(check_failure_json);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

void cloudsync_network_send_changes (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_send_changes");

    // send-scoped: emits send.lastFailure (from failures.apply) only.
    // failures.check arriving in the same response is parsed but discarded here.
    sync_result sr = {.server_version = -1};
    int rc = cloudsync_network_send_changes_internal(context, argc, argv, &sr);
    if (rc != SQLITE_OK) {
        if (sr.apply_failure_json) cloudsync_memory_free(sr.apply_failure_json);
        if (sr.check_failure_json) cloudsync_memory_free(sr.check_failure_json);
        return;
    }

    char *buf;
    if (sr.apply_failure_json) {
        buf = cloudsync_memory_mprintf(
            "{\"send\":{\"status\":\"%s\",\"localVersion\":%lld,\"serverVersion\":%lld,\"chunks\":%d,\"bytes\":%lld,\"lastFailure\":%s}}",
            sr.status ? sr.status : "error",
            (long long)sr.local_version, (long long)sr.server_version,
            sr.send_chunks, (long long)sr.send_bytes,
            sr.apply_failure_json);
    } else {
        buf = cloudsync_memory_mprintf(
            "{\"send\":{\"status\":\"%s\",\"localVersion\":%lld,\"serverVersion\":%lld,\"chunks\":%d,\"bytes\":%lld}}",
            sr.status ? sr.status : "error",
            (long long)sr.local_version, (long long)sr.server_version,
            sr.send_chunks, (long long)sr.send_bytes);
    }
    sqlite3_result_text(context, buf, -1, cloudsync_memory_free);
    if (sr.apply_failure_json) cloudsync_memory_free(sr.apply_failure_json);
    if (sr.check_failure_json) cloudsync_memory_free(sr.check_failure_json);
}

int cloudsync_network_check_internal(sqlite3_context *context, int *pnrows, sync_result *out, char **err_out, int request_max_chunks) {
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    network_data *netdata = (network_data *)cloudsync_auxdata(data);
    if (!netdata) {sqlite3_result_error(context, "Unable to retrieve CloudSync network context.", -1); return -1;}

    int64_t db_version = dbutils_settings_get_int64_value(data, CLOUDSYNC_KEY_CHECK_DBVERSION);
    if (db_version<0) {sqlite3_result_error(context, "Unable to retrieve db_version.", -1); return -1;}

    int seq = dbutils_settings_get_int_value(data, CLOUDSYNC_KEY_CHECK_SEQ);
    if (seq<0) {sqlite3_result_error(context, "Unable to retrieve seq.", -1); return -1;}

    // Restart paging whenever the durable receive window changes: the page cursor
    // is only meaningful within a single drain (check_dbversion held at "since").
    if (netdata->check_cursor_since != db_version) {
        netdata->check_cursor = 0;
        netdata->check_cursor_since = db_version;
    }

    // Capture local db_version before download so we can query cloudsync_changes afterwards
    int64_t prev_dbv = cloudsync_dbversion(data);

    // "cursor" is the spool page to serve. "maxChunks" lets cursor-spool servers
    // batch several contiguous prepared pages in one response when present.
    // Old/legacy servers ignore the unknown fields and omit them from the response;
    // the client then never self-pages (check_cursor stays 0), preserving current behavior.
    char json_payload[2048];
    if (request_max_chunks > 0) {
        snprintf(json_payload, sizeof(json_payload), "{\"dbVersion\":%lld, \"seq\":%d, \"cursor\":%lld, \"maxChunks\":%d}",
                 (long long)db_version, seq, (long long)netdata->check_cursor, request_max_chunks);
    } else {
        snprintf(json_payload, sizeof(json_payload), "{\"dbVersion\":%lld, \"seq\":%d, \"cursor\":%lld}",
                 (long long)db_version, seq, (long long)netdata->check_cursor);
    }

    if (out) { out->page_delivered = false; out->more_pending = false; out->bytes_received = 0; out->chunks_received = 0; }

    NETWORK_RESULT result = network_receive_buffer(netdata, netdata->check_endpoint, netdata->authentication, true, true, json_payload, cloudsync_check_headers, ARRAY_LEN(cloudsync_check_headers));
    int rc = SQLITE_OK;
    if (result.code == CLOUDSYNC_NETWORK_BUFFER) {
        // The /check endpoint returns one of these shapes:
        //   HTTP 200 -> {"url": "..."}                       (legacy artifact URL)
        //   HTTP 200 -> {"data":{"payload": "...", ...}}     (single inline payload)
        //   HTTP 200 -> {"data":{"chunks":[...], ...}}       (cursor-spool batch)
        //   HTTP 202 -> SyncStatusResponse                   (no artifact yet/status)
        // Branch on the presence of transport fields rather than HTTP status; all
        // shapes arrive as BUFFER. Newer servers wrap page metadata in "data";
        // legacy responses put "url" at the top level.
        char *data_json = json_extract_object_raw(result.buffer, result.blen, "data");
        const char *check_json = data_json ? data_json : result.buffer;
        size_t check_json_len = data_json ? strlen(data_json) : result.blen;

        int rows_total = 0;
        int chunks_total = 0;
        int64_t bytes_total = 0;
        bool delivered = false;
        bool more_pending = false;
        int64_t next_cursor = json_extract_int(check_json, check_json_len, "nextCursor", -1);

        int ntokens = 0;
        int chunks_index = -1;
        jsmntok_t *tokens = json_parse_tokens_alloc(check_json, check_json_len, &ntokens);
        bool has_chunks = tokens && tokens[0].type == JSMN_OBJECT &&
                          jsmn_find_object_value(check_json, tokens, ntokens, 0, "chunks", &chunks_index) &&
                          chunks_index >= 0 && chunks_index < ntokens &&
                          tokens[chunks_index].type == JSMN_ARRAY;

        if (!tokens) {
            // The body is BUFFER (non-empty) but unparseable (malformed, truncated,
            // or token allocation failed). Without this, a chunks batch we failed to
            // tokenize would fall through to the single-payload branch, find no
            // url/payload, and be misreported as an empty "up to date" response,
            // silently dropping the pending batch.
            sqlite3_result_error(context, "cloudsync_network_receive_changes: unable to parse check response.", -1);
            rc = SQLITE_ERROR;
        } else if (has_chunks && request_max_chunks > 0 && tokens[chunks_index].size > request_max_chunks) {
            sqlite3_result_error(context, "cloudsync_network_receive_changes: check response exceeded requested maxChunks.", -1);
            rc = SQLITE_ERROR;
        } else if (has_chunks && tokens[chunks_index].size > 0) {
            bool final_batch = json_extract_bool(check_json, check_json_len, "final", true);
            int64_t last_cursor = -1;
            int child = chunks_index + 1;

            for (int i = 0; rc == SQLITE_OK && i < tokens[chunks_index].size; i++) {
                if (child >= ntokens || tokens[child].type != JSMN_OBJECT) {
                    sqlite3_result_error(context, "cloudsync_network_receive_changes: invalid check chunks response.", -1);
                    rc = SQLITE_ERROR;
                    break;
                }

                const char *chunk_json = check_json + tokens[child].start;
                size_t chunk_json_len = (size_t)(tokens[child].end - tokens[child].start);
                bool final_chunk = final_batch && (i == tokens[chunks_index].size - 1);
                int chunk_rows = 0;
                int64_t chunk_bytes = 0;
                rc = network_apply_check_chunk(context, chunk_json, chunk_json_len, final_chunk,
                                               &chunk_rows, err_out, &chunk_bytes);
                if (rc == SQLITE_OK) {
                    rows_total += chunk_rows;
                    bytes_total += chunk_bytes;
                    chunks_total++;
                    delivered = true;
                    last_cursor = json_extract_int(chunk_json, chunk_json_len, "cursor", last_cursor);
                    netdata->check_cursor = final_chunk ? 0 : last_cursor + 1;
                }
                child += jsmn_token_span(tokens, ntokens, child);
            }

            if (rc == SQLITE_OK) {
                if (!final_batch && next_cursor < 0 && last_cursor >= 0) next_cursor = last_cursor + 1;
                if (!final_batch && next_cursor < 0) {
                    // Non-final batch with no resumable cursor. The server contract
                    // always supplies nextCursor (or a per-chunk cursor) on a non-final
                    // batch, so a missing one would otherwise make us silently drop the
                    // rest of the stream and report a false "complete". Fail loudly.
                    sqlite3_result_error(context, "cloudsync_network_receive_changes: non-final check batch missing next cursor.", -1);
                    rc = SQLITE_ERROR;
                } else {
                    more_pending = !final_batch && next_cursor >= 0;
                }
            }
        } else if (has_chunks) {
            // Explicit empty cursor-spool batch: no artifact in flight.
            netdata->check_cursor = 0;
            if (pnrows) *pnrows = 0;
        } else {
            char *download_url = json_extract_string(check_json, check_json_len, "url");
            char *inline_payload = download_url ? NULL : json_extract_string(check_json, check_json_len, "payload");
            if (download_url || inline_payload) {
                bool final_chunk = json_extract_bool(check_json, check_json_len, "final", true);
                int chunk_rows = 0;
                int64_t chunk_bytes = 0;
                rc = network_apply_check_chunk(context, check_json, check_json_len, final_chunk,
                                               &chunk_rows, err_out, &chunk_bytes);
                if (rc == SQLITE_OK && !final_chunk && next_cursor < 0) {
                    // Symmetric with the chunks-array path: a non-final response
                    // with no resumable cursor would otherwise silently drop the
                    // rest of the stream and report a false "complete". Fail loudly.
                    sqlite3_result_error(context, "cloudsync_network_receive_changes: non-final check response missing next cursor.", -1);
                    rc = SQLITE_ERROR;
                } else if (rc == SQLITE_OK) {
                    rows_total = chunk_rows;
                    bytes_total = chunk_bytes;
                    chunks_total = 1;
                    delivered = true;
                    more_pending = !final_chunk && next_cursor >= 0;
                }
            } else {
                // 202 / "up to date": no artifact in flight -> reset paging.
                netdata->check_cursor = 0;
                if (pnrows) *pnrows = 0;
            }
            if (download_url) cloudsync_memory_free(download_url);
            if (inline_payload) cloudsync_memory_free(inline_payload);
        }

        if (tokens) cloudsync_memory_free(tokens);

        if (rc == SQLITE_OK && delivered) {
            // Finalize cursor state after the returned batch is applied/staged.
            // Batched responses advance the in-memory spool cursor after each
            // successful chunk, so a later failure retries from the failed chunk.
            netdata->check_cursor = more_pending ? next_cursor : 0;
            if (pnrows) *pnrows = rows_total;
            if (out) {
                out->page_delivered = true;
                out->more_pending = more_pending;
                out->bytes_received = bytes_total;
                out->chunks_received = chunks_total;
            }
        } else {
            if (pnrows) *pnrows = 0;
        }
        if (data_json) cloudsync_memory_free(data_json);
        // failures.check may appear in either shape; extract opportunistically.
        if (out) {
            char *check_failure = json_extract_failure_stage(result.buffer, result.blen, "check");
            if (check_failure) {
                if (out->check_failure_json) cloudsync_memory_free(out->check_failure_json);
                out->check_failure_json = check_failure;
            }
        }
    } else if (result.code == CLOUDSYNC_NETWORK_ERROR) {
        network_set_sqlite_result(context, &result);
        rc = -1;
    } else {
        // CLOUDSYNC_NETWORK_OK — no body (older server) — not an error
        rc = 0;
    }

    if (out && pnrows) out->rows_received = *pnrows;

    // Query cloudsync_changes for affected tables after successful download.
    // Skipped when the caller defers it (a multi-chunk drain computes the union once
    // at the end from a db_version captured before the whole drain started).
    if (out && !out->defer_tables && rc == SQLITE_OK && pnrows && *pnrows > 0) {
        sqlite3 *db = (sqlite3 *)cloudsync_db(data);
        out->tables_json = network_get_affected_tables(db, prev_dbv);
    }

    network_result_cleanup(&result);
    return rc;
}

// Result of a receive drain (see network_drain_changes).
typedef struct {
    int     rows;          // cumulative rows applied across the drain
    int     chunks;        // payload chunks applied this drain
    int64_t bytes;         // serialized payload bytes received this drain
    bool    complete;      // true iff the receive stream is fully drained (nothing pending)
    bool    check_permanent_failure; // server reported a non-retryable failures.check: polling stopped early
    char   *receive_err;   // owned by the caller; client-side apply error, or NULL
} drain_result;

// Drains chunked /check responses into the local database. Chunks that are already
// available are fetched back-to-back with no delay; wait_ms/max_retries are spent
// only while the server payload is not yet ready (HTTP 202). max_chunks > 0 caps how
// many chunks are applied this call (caller-driven progress/traffic control);
// max_chunks <= 0 drains everything available. The in-memory page cursor persists on
// the network context, so a capped drain resumes where it left off on the next call.
// Fills *dr and sr->tables_json (the union of tables touched across the drain).
static int network_drain_changes (sqlite3_context *context, sync_result *sr,
                                  int wait_ms, int max_retries, int max_chunks,
                                  drain_result *dr) {
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    sqlite3 *db = (sqlite3 *)cloudsync_db(data);
    // Capture the local db_version once before draining so the affected-tables union
    // across all drained chunks can be computed with a single query at the end.
    int64_t drain_prev_dbv = cloudsync_dbversion(data);
    sr->defer_tables = true;

    int ntries = 0;          // counts only "nothing ready" (202) polls
    int nrows_total = 0;     // cumulative rows applied across the whole drain
    int nchunks = 0;         // payload chunks applied this call
    int64_t bytes_total = 0; // serialized payload bytes received this call
    bool complete = true;    // false iff the stream is known to have more pending
    bool check_permanent_failure = false; // server reported a non-retryable failures.check
    char *receive_err = NULL;
    int rc = SQLITE_OK;
    for (;;) {
        if (receive_err) { cloudsync_memory_free(receive_err); receive_err = NULL; }

        int request_max_chunks = 0;
        if (max_chunks > 0) {
            int remaining = max_chunks - nchunks;
            if (remaining <= 0) break;
            request_max_chunks = remaining;
        }
        int safety_remaining = CLOUDSYNC_CHECK_MAX_CHUNKS_PER_DRAIN - nchunks;
        if (safety_remaining <= 0) { complete = false; break; }
        if (request_max_chunks > 0) {
            if (safety_remaining < request_max_chunks) request_max_chunks = safety_remaining;
        } else if (nchunks > 0) {
            request_max_chunks = safety_remaining;
        }

        int nrows = 0;
        rc = cloudsync_network_check_internal(context, &nrows, sr, &receive_err, request_max_chunks);
        // a receive error (network or apply) won't fix itself across retries
        if (rc != SQLITE_OK) { complete = false; break; }

        if (sr->page_delivered) {
            nrows_total += nrows;                 // a staged (incomplete) fragment contributes 0
            bytes_total += sr->bytes_received;
            nchunks += sr->chunks_received;
            complete = !sr->more_pending;         // reflects whether the stream is finished
            if (!sr->more_pending) break;                                 // final batch -> drained
            if (max_chunks > 0 && nchunks >= max_chunks) break;           // caller cap: more pending
            if (nchunks >= CLOUDSYNC_CHECK_MAX_CHUNKS_PER_DRAIN) break;   // safety bound: more pending
            continue;                                                     // keep draining immediately
        }

        // A non-retryable server-side check failure (e.g. a permission/authorization
        // error) won't clear by waiting: stop draining now instead of polling out the
        // remaining retries. The failure object stays in sr->check_failure_json so the
        // caller can surface it (receive.lastFailure / a raised error).
        if (sr->check_failure_json &&
            !json_extract_bool(sr->check_failure_json, strlen(sr->check_failure_json), "retryable", true)) {
            check_permanent_failure = true;
            complete = false;
            break;
        }

        // nothing delivered (202 / up to date): preserve the polling-for-changes semantics.
        // complete is left as-is (true if no page was ever delivered; false if the last
        // delivered page was non-final), so a 202 after partial pages reports incomplete.
        if (ntries + 1 >= max_retries) break;
        ntries++;
        sqlite3_sleep(wait_ms);
    }

    // Compute the affected-tables union once, over the whole drain window.
    if (!receive_err && rc == SQLITE_OK && nrows_total > 0) {
        sr->tables_json = network_get_affected_tables(db, drain_prev_dbv);
    }

    dr->rows = nrows_total;
    dr->chunks = nchunks;
    dr->bytes = bytes_total;
    dr->complete = complete;
    dr->check_permanent_failure = check_permanent_failure;
    dr->receive_err = receive_err;
    return rc;
}

void cloudsync_network_sync (sqlite3_context *context, int wait_ms, int max_retries) {
    sync_result sr = {.server_version = -1};
    int rc = cloudsync_network_send_changes_internal(context, 0, NULL, &sr);
    if (rc != SQLITE_OK) {
        if (sr.apply_failure_json) cloudsync_memory_free(sr.apply_failure_json);
        if (sr.check_failure_json) cloudsync_memory_free(sr.check_failure_json);
        return;
    }

    // Drain the whole receive stream in one call (max_chunks=0 => unlimited).
    drain_result dr = {0};
    rc = network_drain_changes(context, &sr, wait_ms, max_retries, 0, &dr);
    char *receive_err = dr.receive_err;
    int nrows_total = dr.rows;

    // If the receive phase failed, still emit structured JSON so the caller
    // sees that the send phase completed and understands why receive did not.
    if (rc != SQLITE_OK && !receive_err) {
        receive_err = cloudsync_string_dup("receive failed");
    }
    if (receive_err) {
        rc = SQLITE_OK;
        nrows_total = 0;
        if (sr.tables_json) { cloudsync_memory_free(sr.tables_json); sr.tables_json = NULL; }
    }

    const char *tables = sr.tables_json ? sr.tables_json : "[]";
    const char *complete_str = dr.complete ? "true" : "false";
    const char *status = sr.status ? sr.status : "error";
    char *escaped_err = receive_err ? json_escape_string(receive_err) : NULL;

    // Build send and receive blocks separately to avoid combinatorial explosion
    // across optional fields (send.lastFailure, receive.error, receive.lastFailure).
    char *send_part = sr.apply_failure_json
        ? cloudsync_memory_mprintf(
            "\"send\":{\"status\":\"%s\",\"localVersion\":%lld,\"serverVersion\":%lld,\"chunks\":%d,\"bytes\":%lld,\"lastFailure\":%s}",
            status, (long long)sr.local_version, (long long)sr.server_version, sr.send_chunks, (long long)sr.send_bytes, sr.apply_failure_json)
        : cloudsync_memory_mprintf(
            "\"send\":{\"status\":\"%s\",\"localVersion\":%lld,\"serverVersion\":%lld,\"chunks\":%d,\"bytes\":%lld}",
            status, (long long)sr.local_version, (long long)sr.server_version, sr.send_chunks, (long long)sr.send_bytes);

    char *recv_part;
    if (escaped_err && sr.check_failure_json) {
        recv_part = cloudsync_memory_mprintf(
            "\"receive\":{\"rows\":%d,\"tables\":%s,\"chunks\":%d,\"bytes\":%lld,\"complete\":%s,\"error\":\"%s\",\"lastFailure\":%s}",
            nrows_total, tables, dr.chunks, (long long)dr.bytes, complete_str, escaped_err, sr.check_failure_json);
    } else if (escaped_err) {
        recv_part = cloudsync_memory_mprintf(
            "\"receive\":{\"rows\":%d,\"tables\":%s,\"chunks\":%d,\"bytes\":%lld,\"complete\":%s,\"error\":\"%s\"}",
            nrows_total, tables, dr.chunks, (long long)dr.bytes, complete_str, escaped_err);
    } else if (sr.check_failure_json) {
        recv_part = cloudsync_memory_mprintf(
            "\"receive\":{\"rows\":%d,\"tables\":%s,\"chunks\":%d,\"bytes\":%lld,\"complete\":%s,\"lastFailure\":%s}",
            nrows_total, tables, dr.chunks, (long long)dr.bytes, complete_str, sr.check_failure_json);
    } else {
        recv_part = cloudsync_memory_mprintf(
            "\"receive\":{\"rows\":%d,\"tables\":%s,\"chunks\":%d,\"bytes\":%lld,\"complete\":%s}",
            nrows_total, tables, dr.chunks, (long long)dr.bytes, complete_str);
    }

    char *buf = cloudsync_memory_mprintf("{%s,%s}", send_part, recv_part);
    cloudsync_memory_free(send_part);
    cloudsync_memory_free(recv_part);

    sqlite3_result_text(context, buf, -1, cloudsync_memory_free);
    if (escaped_err) cloudsync_memory_free(escaped_err);
    if (receive_err) cloudsync_memory_free(receive_err);
    if (sr.tables_json) cloudsync_memory_free(sr.tables_json);
    if (sr.apply_failure_json) cloudsync_memory_free(sr.apply_failure_json);
    if (sr.check_failure_json) cloudsync_memory_free(sr.check_failure_json);
}

void cloudsync_network_sync0 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_sync2");

    cloudsync_network_sync(context, DEFAULT_SYNC_WAIT_MS, DEFAULT_SYNC_MAX_RETRIES);
}


void cloudsync_network_sync2 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_sync2");

    int wait_ms = sqlite3_value_int(argv[0]);
    int max_retries = sqlite3_value_int(argv[1]);

    cloudsync_network_sync(context, wait_ms, max_retries);
}


// Shared implementation for cloudsync_network_receive_changes() and its deprecated
// alias cloudsync_network_check_changes(). Drains available chunks (no polling-wait
// for server preparation: wait_ms=0, max_retries=1) and emits a receive-only JSON
// block. max_chunks <= 0 drains everything available; max_chunks > 0 caps the chunks
// applied this call so a caller can drive progress/traffic — the in-memory page
// cursor persists on the network context, so the next call resumes the drain.
static void network_receive_changes_impl (sqlite3_context *context, int max_chunks) {
    // check-scoped: emits receive.error (client-side apply) and/or
    // receive.lastFailure (server-side failures.check) only — never a send block.
    sync_result sr = {.server_version = -1};
    drain_result dr = {0};
    int rc = network_drain_changes(context, &sr, 0, 1, max_chunks, &dr);
    char *receive_err = dr.receive_err;
    int nrows = dr.rows;

    // Endpoint/network errors already raised a SQL error on the context
    if (rc != SQLITE_OK && !receive_err) {
        if (sr.tables_json) cloudsync_memory_free(sr.tables_json);
        if (sr.check_failure_json) cloudsync_memory_free(sr.check_failure_json);
        return;
    }

    // A non-retryable server-side check failure is a permanent configuration/
    // authorization problem, not a transient "nothing ready yet": raise a SQL error so
    // a polling caller fails fast instead of looping until it times out. (receive is a
    // pure receive op with no send block to preserve, so a raised error is unambiguous.)
    if (dr.check_permanent_failure && sr.check_failure_json) {
        char *code = json_extract_string(sr.check_failure_json, strlen(sr.check_failure_json), "code");
        char *message = json_extract_string(sr.check_failure_json, strlen(sr.check_failure_json), "message");
        char *err = cloudsync_memory_mprintf(
            "cloudsync_network_receive_changes: server rejected check (non-retryable): %s%s%s",
            code ? code : "check failed",
            message ? " - " : "",
            message ? message : "");
        sqlite3_result_error(context, err ? err : "cloudsync_network_receive_changes: server rejected check (non-retryable).", -1);
        if (err) cloudsync_memory_free(err);
        if (code) cloudsync_memory_free(code);
        if (message) cloudsync_memory_free(message);
        if (receive_err) cloudsync_memory_free(receive_err);
        if (sr.tables_json) cloudsync_memory_free(sr.tables_json);
        if (sr.check_failure_json) cloudsync_memory_free(sr.check_failure_json);
        return;
    }

    // Apply errors → structured JSON with receive.error
    if (receive_err) {
        nrows = 0;
        if (sr.tables_json) { cloudsync_memory_free(sr.tables_json); sr.tables_json = NULL; }
    }

    const char *tables = sr.tables_json ? sr.tables_json : "[]";
    const char *complete_str = dr.complete ? "true" : "false";
    char *escaped = receive_err ? json_escape_string(receive_err) : NULL;
    char *buf;
    if (escaped && sr.check_failure_json) {
        buf = cloudsync_memory_mprintf("{\"receive\":{\"rows\":%d,\"tables\":%s,\"chunks\":%d,\"bytes\":%lld,\"complete\":%s,\"error\":\"%s\",\"lastFailure\":%s}}",
                                       nrows, tables, dr.chunks, (long long)dr.bytes, complete_str, escaped, sr.check_failure_json);
    } else if (escaped) {
        buf = cloudsync_memory_mprintf("{\"receive\":{\"rows\":%d,\"tables\":%s,\"chunks\":%d,\"bytes\":%lld,\"complete\":%s,\"error\":\"%s\"}}",
                                       nrows, tables, dr.chunks, (long long)dr.bytes, complete_str, escaped);
    } else if (sr.check_failure_json) {
        buf = cloudsync_memory_mprintf("{\"receive\":{\"rows\":%d,\"tables\":%s,\"chunks\":%d,\"bytes\":%lld,\"complete\":%s,\"lastFailure\":%s}}",
                                       nrows, tables, dr.chunks, (long long)dr.bytes, complete_str, sr.check_failure_json);
    } else {
        buf = cloudsync_memory_mprintf("{\"receive\":{\"rows\":%d,\"tables\":%s,\"chunks\":%d,\"bytes\":%lld,\"complete\":%s}}",
                                       nrows, tables, dr.chunks, (long long)dr.bytes, complete_str);
    }
    sqlite3_result_text(context, buf, -1, cloudsync_memory_free);
    if (escaped) cloudsync_memory_free(escaped);
    if (receive_err) cloudsync_memory_free(receive_err);
    if (sr.tables_json) cloudsync_memory_free(sr.tables_json);
    if (sr.check_failure_json) cloudsync_memory_free(sr.check_failure_json);
}

void cloudsync_network_receive_changes (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_receive_changes");
    network_receive_changes_impl(context, 0);
}

void cloudsync_network_receive_changes1 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_receive_changes");
    int max_chunks = sqlite3_value_int(argv[0]);
    network_receive_changes_impl(context, max_chunks);
}

// Deprecated: use cloudsync_network_receive_changes(). Kept as a thin alias for
// backward compatibility; the name will be removed in a future major version.
void cloudsync_network_check_changes (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_check_changes");
    network_receive_changes_impl(context, 0);
}

void cloudsync_network_check_changes1 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_check_changes");
    int max_chunks = sqlite3_value_int(argv[0]);
    network_receive_changes_impl(context, max_chunks);
}

void cloudsync_network_reset_sync_version (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_reset_sync_version");
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    char *buf = "0";
    dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_CHECK_DBVERSION, buf);
    dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_CHECK_SEQ, buf);
    dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_SEND_DBVERSION, buf);
    dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_SEND_SEQ, buf);
}

/**
 * Cleanup all local data from cloudsync-enabled tables, so the database can be safely reused
 * by another user without exposing any data from the previous session.
 *
 * Warning: this function deletes all data from the tables. Use with caution.
 */
void cloudsync_network_logout (sqlite3_context *context, int argc, sqlite3_value **argv) {
    bool savepoint_created = false;
    bool completed = false;
    char *errmsg = NULL;
    int rc = SQLITE_ERROR;
    sqlite3 *db = sqlite3_context_db_handle(context);
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);

    // if the network layer is enabled, remove the token or apikey
    sqlite3_exec(db, "SELECT cloudsync_network_set_token('');", NULL, NULL, NULL);
    
    // get the list of cloudsync-enabled tables
    char *sql = "SELECT tbl_name, key, value FROM cloudsync_table_settings;";
    char **result = NULL;
    int nrows, ncols;
    rc = sqlite3_get_table(db, sql, &result, &nrows, &ncols, NULL);
    if (rc != SQLITE_OK) {
        errmsg = cloudsync_memory_mprintf("Unable to get current cloudsync configuration %s", sqlite3_errmsg(db));
        goto finalize;
    }
    
    // run everything in a savepoint
    rc = database_begin_savepoint(data, "cloudsync_logout_savepoint");
    if (rc != SQLITE_OK) {
        errmsg = cloudsync_memory_mprintf("Unable to create cloudsync_logout savepoint %s", cloudsync_errmsg(data));
        goto finalize;
    }
    savepoint_created = true;

    rc = cloudsync_cleanup_all(data);
    if (rc != SQLITE_OK) {
        errmsg = cloudsync_memory_mprintf("Unable to cleanup current database %s", cloudsync_errmsg(data));
        goto finalize;
    }
    
    // delete all the local data for each previously enabled table
    // re-enable cloudsync on previously enabled tables
    for (int i = 1; i <= nrows; i++) {
        char *tbl_name  = result[i * ncols + 0];
        char *key       = result[i * ncols + 1];
        char *value     = result[i * ncols + 2];
        
        if (strcmp(key, "algo") != 0) continue;
        
        sql = cloudsync_memory_mprintf("DELETE FROM \"%w\";", tbl_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        cloudsync_memory_free(sql);
        if (rc != SQLITE_OK) {
            errmsg = cloudsync_memory_mprintf("Unable to delete data from table %s. %s", tbl_name, sqlite3_errmsg(db));
            goto finalize;
        }
        
        sql = cloudsync_memory_mprintf("SELECT cloudsync_init('%q', '%q', 1);", tbl_name, value);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        cloudsync_memory_free(sql);
        if (rc != SQLITE_OK) {
            errmsg = cloudsync_memory_mprintf("Unable to enable cloudsync on table %s. %s", tbl_name, sqlite3_errmsg(db));
            goto finalize;
        }
    }
    
    completed = true;
        
finalize:
    if (completed) {
        database_commit_savepoint(data, "cloudsync_logout_savepoint");
        cloudsync_network_cleanup_internal(context);
        sqlite3_result_int(context, SQLITE_OK);
    } else {
        // cleanup:
        // ROLLBACK TO command reverts the state of the database back to what it was just after the corresponding SAVEPOINT
        // then RELEASE to remove the SAVEPOINT from the transaction stack
        if (savepoint_created) database_rollback_savepoint(data, "cloudsync_logout_savepoint");
        sqlite3_result_error(context, errmsg, -1);
        sqlite3_result_error_code(context, rc);
    }
    sqlite3_free_table(result);
    cloudsync_memory_free(errmsg);
}

void cloudsync_network_status (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_status");

    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    network_data *netdata = (network_data *)cloudsync_auxdata(data);
    if (!netdata) {
        sqlite3_result_error(context, "Unable to retrieve CloudSync network context.", -1);
        return;
    }

    NETWORK_RESULT res = network_receive_buffer(netdata, netdata->status_endpoint, netdata->authentication, true, false, NULL, cloudsync_default_headers, ARRAY_LEN(cloudsync_default_headers));
    network_set_sqlite_result(context, &res);
    network_result_cleanup(&res);
}

// MARK: -

int cloudsync_network_register (sqlite3 *db, char **pzErrMsg, void *ctx) {
    const int DEFAULT_FLAGS = SQLITE_UTF8 | SQLITE_INNOCUOUS;
    int rc = SQLITE_OK;
    
    rc = sqlite3_create_function(db, "cloudsync_network_init", 1, DEFAULT_FLAGS, ctx, cloudsync_network_init, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_create_function(db, "cloudsync_network_init_custom", 2, DEFAULT_FLAGS, ctx, cloudsync_network_init_custom, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function(db, "cloudsync_network_cleanup", 0, DEFAULT_FLAGS, ctx, cloudsync_network_cleanup, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function(db, "cloudsync_network_set_token", 1, DEFAULT_FLAGS, ctx, cloudsync_network_set_token, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function(db, "cloudsync_network_set_apikey", 1, DEFAULT_FLAGS, ctx, cloudsync_network_set_apikey, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function(db, "cloudsync_network_has_unsent_changes", 0, DEFAULT_FLAGS, ctx, cloudsync_network_has_unsent_changes, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function(db, "cloudsync_network_send_changes", 0, DEFAULT_FLAGS, ctx, cloudsync_network_send_changes, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function(db, "cloudsync_network_receive_changes", 0, DEFAULT_FLAGS, ctx, cloudsync_network_receive_changes, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function(db, "cloudsync_network_receive_changes", 1, DEFAULT_FLAGS, ctx, cloudsync_network_receive_changes1, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    // Deprecated alias of cloudsync_network_receive_changes(); kept for backward compatibility.
    rc = sqlite3_create_function(db, "cloudsync_network_check_changes", 0, DEFAULT_FLAGS, ctx, cloudsync_network_check_changes, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function(db, "cloudsync_network_check_changes", 1, DEFAULT_FLAGS, ctx, cloudsync_network_check_changes1, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function(db, "cloudsync_network_sync", 0, DEFAULT_FLAGS, ctx, cloudsync_network_sync0, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function(db, "cloudsync_network_sync", 2, DEFAULT_FLAGS, ctx, cloudsync_network_sync2, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function(db, "cloudsync_network_reset_sync_version", 0, DEFAULT_FLAGS, ctx, cloudsync_network_reset_sync_version, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function(db, "cloudsync_network_logout", 0, DEFAULT_FLAGS, ctx, cloudsync_network_logout, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function(db, "cloudsync_network_status", 0, DEFAULT_FLAGS, ctx, cloudsync_network_status, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

cleanup:
    if ((rc != SQLITE_OK) && (pzErrMsg)) {
        *pzErrMsg = sqlite3_mprintf("Error creating function in cloudsync_network_register: %s", sqlite3_errmsg(db));
    }
    
    return rc;
}

#endif
