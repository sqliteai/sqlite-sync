//
//  network.c
//  cloudsync
//
//  Created by Marco Bambini on 12/12/24.
//

#ifndef CLOUDSYNC_OMIT_NETWORK

#include <stdint.h>
#include <inttypes.h>

#include "network.h"
#include "utils.h"
#include "dbutils.h"
#include "cloudsync.h"
#include "network_private.h"

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

#define DEFAULT_SYNC_WAIT_MS                    100
#define DEFAULT_SYNC_MAX_RETRIES                1
 
#define MAX_QUERY_VALUE_LEN                     256

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT3
#endif

// MARK: -

struct network_data {
    char        site_id[UUID_STR_MAXLEN];
    char        *authentication; // apikey or token
    char        *check_endpoint;
    char        *upload_endpoint;
    char        *apply_endpoint;
};

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

bool network_data_set_endpoints (network_data *data, char *auth, char *check, char *upload, char *apply) {
    // sanity check
    if (!check || !upload) return false;
    
    // always free previous owned pointers
    if (data->authentication) cloudsync_memory_free(data->authentication);
    if (data->check_endpoint) cloudsync_memory_free(data->check_endpoint);
    if (data->upload_endpoint) cloudsync_memory_free(data->upload_endpoint);
    if (data->apply_endpoint) cloudsync_memory_free(data->apply_endpoint);

    // clear pointers
    data->authentication = NULL;
    data->check_endpoint = NULL;
    data->upload_endpoint = NULL;
    data->apply_endpoint = NULL;

    // make a copy of the new endpoints
    char *auth_copy = NULL;
    char *check_copy = NULL;
    char *upload_copy = NULL;
    char *apply_copy = NULL;

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

    data->authentication = auth_copy;
    data->check_endpoint = check_copy;
    data->upload_endpoint = upload_copy;
    data->apply_endpoint = apply_copy;
    return true;
    
abort_endpoints:
    if (auth_copy) cloudsync_memory_free(auth_copy);
    if (check_copy) cloudsync_memory_free(check_copy);
    if (upload_copy) cloudsync_memory_free(upload_copy);
    if (apply_copy) cloudsync_memory_free(apply_copy);
    return false;
}

void network_data_free (network_data *data) {
    if (!data) return;
    
    if (data->authentication) cloudsync_memory_free(data->authentication);
    if (data->check_endpoint) cloudsync_memory_free(data->check_endpoint);
    if (data->upload_endpoint) cloudsync_memory_free(data->upload_endpoint);
    if (data->apply_endpoint) cloudsync_memory_free(data->apply_endpoint);
    cloudsync_memory_free(data);
}

// MARK: - Utils -

#ifndef CLOUDSYNC_OMIT_CURL
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

NETWORK_RESULT network_receive_buffer (network_data *data, const char *endpoint, const char *authentication, bool zero_terminated, bool is_post_request, char *json_payload, const char *custom_header) {
    char *buffer = NULL;
    size_t blen = 0;
    struct curl_slist* headers = NULL;
    char errbuf[CURL_ERROR_SIZE] = {0};
    long response_code = 0;

    CURL *curl = curl_easy_init();
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
    
    if (custom_header) {
        struct curl_slist *tmp = curl_slist_append(headers, custom_header);
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
    
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    
    network_buffer netdata = {NULL, 0, 0, (zero_terminated) ? 1 : 0};
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &netdata);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, network_receive_callback);

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
    } else if (netdata.buffer) {
        cloudsync_memory_free(netdata.buffer);
        netdata.buffer = NULL;
    }

cleanup:
    if (curl) curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);
    
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

    // init curl
    CURL *curl = curl_easy_init();
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
    if (rc == CURLE_OK) result = true;
       
cleanup:
    if (curl) curl_easy_cleanup(curl);
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

int network_download_changes (sqlite3_context *context, const char *download_url, int *pnrows) {
    DEBUG_FUNCTION("network_download_changes");
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    network_data *netdata = (network_data *)cloudsync_auxdata(data);
    if (!netdata) {
        sqlite3_result_error(context, "Unable to retrieve network CloudSync context.", -1);
        return -1;
    }
    
    NETWORK_RESULT result = network_receive_buffer(netdata, download_url, NULL, false, false, NULL, NULL);
    
    int rc = SQLITE_OK;
    if (result.code == CLOUDSYNC_NETWORK_BUFFER) {
        rc = cloudsync_payload_apply(data, result.buffer, (int)result.blen, pnrows);
    } else {
        rc = network_set_sqlite_result(context, &result);
        if (pnrows) *pnrows = 0;
    }
    network_result_cleanup(&result);
    
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

#if !defined(CLOUDSYNC_OMIT_CURL) || defined(SQLITE_WASM_EXTRA_INIT)
bool network_compute_endpoints (sqlite3_context *context, network_data *data, const char *conn_string) {
    // compute endpoints
    bool result = false;
    
    char *scheme = NULL;
    char *host = NULL;
    char *port = NULL;
    char *database = NULL;
    char *query = NULL;
    
    char *authentication = NULL;
    char *check_endpoint = NULL;
    char *upload_endpoint = NULL;
    char *apply_endpoint = NULL;

    char *conn_string_https = NULL;
    
    #ifndef SQLITE_WASM_EXTRA_INIT
    CURLUcode rc = CURLUE_OUT_OF_MEMORY;
    CURLU *url = curl_url();
    if (!url) goto finalize;
    #endif
    
    conn_string_https = cloudsync_string_replace_prefix(conn_string, "sqlitecloud://", "https://");
    if (!conn_string_https) goto finalize;
    
    #ifndef SQLITE_WASM_EXTRA_INIT
    // set URL: https://UUID.g5.sqlite.cloud:443/chinook.sqlite?apikey=hWDanFolRT9WDK0p54lufNrIyfgLZgtMw6tb6fbPmpo
    rc = curl_url_set(url, CURLUPART_URL, conn_string_https, 0);
    if (rc != CURLUE_OK) goto finalize;
    
    // https (MANDATORY)
    rc = curl_url_get(url, CURLUPART_SCHEME, &scheme, 0);
    if (rc != CURLUE_OK) goto finalize;
    
    // UUID.g5.sqlite.cloud (MANDATORY)
    rc = curl_url_get(url, CURLUPART_HOST, &host, 0);
    if (rc != CURLUE_OK) goto finalize;
    
    // 443 (OPTIONAL)
    rc = curl_url_get(url, CURLUPART_PORT, &port, 0);
    if (rc != CURLUE_OK && rc != CURLUE_NO_PORT) goto finalize;
    char *port_or_default = port && strcmp(port, "8860") != 0 ? port : CLOUDSYNC_DEFAULT_ENDPOINT_PORT;

    // /chinook.sqlite (MANDATORY)
    rc = curl_url_get(url, CURLUPART_PATH, &database, 0);
    if (rc != CURLUE_OK) goto finalize;
    
    // apikey=hWDanFolRT9WDK0p54lufNrIyfgLZgtMw6tb6fbPmpo (OPTIONAL)
    rc = curl_url_get(url, CURLUPART_QUERY, &query, 0);
    if (rc != CURLUE_OK && rc != CURLUE_NO_QUERY) goto finalize;
    #else
    // Parse: scheme://host[:port]/path?query
    const char *p = strstr(conn_string_https, "://");
    if (!p) goto finalize;
    scheme = substr(conn_string_https, p);
    p += 3;
    const char *host_start = p;
    const char *host_end = strpbrk(host_start, ":/?");
    if (!host_end) goto finalize;
    host = substr(host_start, host_end);
    p = host_end;
    if (*p == ':') {
        ++p;
        const char *port_end = strpbrk(p, "/?");
        if (!port_end) goto finalize;
        port = substr(p, port_end);
        p = port_end;
    }
    if (*p == '/') {
        const char *path_start = p;
        const char *path_end = strchr(path_start, '?');
        if (!path_end) path_end = path_start + strlen(path_start);
        database = substr(path_start, path_end);
        p = path_end;
    }
    if (*p == '?') {
        query = strdup(p);
    }
    if (!scheme || !host || !database) goto finalize;
    char *port_or_default = port && strcmp(port, "8860") != 0 ? port : CLOUDSYNC_DEFAULT_ENDPOINT_PORT;
    #endif
    
    if (query != NULL) {
        char value[CLOUDSYNC_SESSION_TOKEN_MAXSIZE];
        if (!authentication && network_extract_query_param(query, "apikey", value, sizeof(value)) == 0) {
            authentication = network_authentication_token("apikey", value);
        }
        if (!authentication && network_extract_query_param(query, "token", value, sizeof(value)) == 0) {
            authentication = network_authentication_token("token", value);
        }
    }
    
    size_t requested = strlen(scheme) + strlen(host) + strlen(port_or_default) + strlen(CLOUDSYNC_ENDPOINT_PREFIX) + strlen(database) + 64;
    check_endpoint = (char *)cloudsync_memory_zeroalloc(requested);
    upload_endpoint = (char *)cloudsync_memory_zeroalloc(requested);
    apply_endpoint = (char *)cloudsync_memory_zeroalloc(requested);

    if ((!upload_endpoint) || (!check_endpoint) || (!apply_endpoint)) goto finalize;

    snprintf(check_endpoint, requested, "%s://%s:%s/%s%s/%s/%s", scheme, host, port_or_default, CLOUDSYNC_ENDPOINT_PREFIX, database, data->site_id, CLOUDSYNC_ENDPOINT_CHECK);
    snprintf(upload_endpoint, requested, "%s://%s:%s/%s%s/%s/%s", scheme, host, port_or_default, CLOUDSYNC_ENDPOINT_PREFIX, database, data->site_id, CLOUDSYNC_ENDPOINT_UPLOAD);
    snprintf(apply_endpoint, requested, "%s://%s:%s/%s%s/%s/%s", scheme, host, port_or_default, CLOUDSYNC_ENDPOINT_PREFIX, database, data->site_id, CLOUDSYNC_ENDPOINT_APPLY);

    result = true;
    
finalize:
    if (result == false) {
        // store proper result code/message
        #ifndef SQLITE_WASM_EXTRA_INIT
        if (rc != CURLUE_OK) sqlite3_result_error(context, curl_url_strerror(rc), -1);
        sqlite3_result_error_code(context, (rc != CURLUE_OK) ? SQLITE_ERROR : SQLITE_NOMEM);
        #else
        sqlite3_result_error(context, "URL parse error", -1);
        sqlite3_result_error_code(context, SQLITE_ERROR);
        #endif
        
        // cleanup memory managed by the extension
        if (authentication) cloudsync_memory_free(authentication);
        if (check_endpoint) cloudsync_memory_free(check_endpoint);
        if (upload_endpoint) cloudsync_memory_free(upload_endpoint);
        if (apply_endpoint) cloudsync_memory_free(apply_endpoint);
    }
        
    if (result) {
        if (authentication) {
            if (data->authentication) cloudsync_memory_free(data->authentication);
            data->authentication = authentication;
        }
        
        if (data->check_endpoint) cloudsync_memory_free(data->check_endpoint);
        data->check_endpoint = check_endpoint;
        
        if (data->upload_endpoint) cloudsync_memory_free(data->upload_endpoint);
        data->upload_endpoint = upload_endpoint;

        if (data->apply_endpoint) cloudsync_memory_free(data->apply_endpoint);
        data->apply_endpoint = apply_endpoint;
    }
    
    // cleanup memory
    #ifndef SQLITE_WASM_EXTRA_INIT
    if (url) curl_url_cleanup(url);
    #endif
    if (scheme) curl_free(scheme);
    if (host) curl_free(host);
    if (port) curl_free(port);
    if (database) curl_free(database);
    if (query) curl_free(query);
    if (conn_string_https && conn_string_https != conn_string) cloudsync_memory_free(conn_string_https);
    
    return result;
}
#endif

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

void cloudsync_network_init (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_init");
    
    #ifndef CLOUDSYNC_OMIT_CURL
    curl_global_init(CURL_GLOBAL_ALL);
    #endif
    
    // no real network operations here
    // just setup the network_data struct
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    network_data *netdata = cloudsync_network_data(context);
    if (!netdata) goto abort_memory;
    
    // init context
    uint8_t *site_id = (uint8_t *)cloudsync_context_init(data);
    if (!site_id) goto abort_siteid;
    
    // save site_id string representation: 01957493c6c07e14803727e969f1d2cc
    cloudsync_uuid_v7_stringify(site_id, netdata->site_id, false);
    
    // connection string is something like:
    // https://UUID.g5.sqlite.cloud:443/chinook.sqlite?apikey=hWDanFolRT9WDK0p54lufNrIyfgLZgtMw6tb6fbPmpo
    // or https://UUID.g5.sqlite.cloud:443/chinook.sqlite
    // apikey part is optional and can be replaced by a session token once client is authenticated
    
    const char *connection_param = (const char *)sqlite3_value_text(argv[0]);
    
    // compute endpoints
    if (network_compute_endpoints(context, netdata, connection_param) == false) {
        // error message/code already set inside network_compute_endpoints
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

// MARK: -

void cloudsync_network_has_unsent_changes (sqlite3_context *context, int argc, sqlite3_value **argv) {
    sqlite3 *db = sqlite3_context_db_handle(context);
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
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
    
    int sent_db_version = dbutils_settings_get_int_value(data, CLOUDSYNC_KEY_SEND_DBVERSION);
    sqlite3_result_int(context, (sent_db_version < last_local_change));
}

int cloudsync_network_send_changes_internal (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_send_changes");
    
    // retrieve global context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    network_data *netdata = (network_data *)cloudsync_auxdata(data);
    if (!netdata) {sqlite3_result_error(context, "Unable to retrieve CloudSync network context.", -1); return SQLITE_ERROR;}
    
    // retrieve payload
    char *blob = NULL;
    int blob_size = 0, db_version = 0, seq = 0;
    int64_t new_db_version = 0, new_seq = 0;
    int rc = cloudsync_payload_get(data, &blob, &blob_size, &db_version, &seq, &new_db_version, &new_seq);
    if (rc != SQLITE_OK) {
        if (db_version < 0) sqlite3_result_error(context, "Unable to retrieve db_version.", -1);
        else if (seq < 0) sqlite3_result_error(context, "Unable to retrieve seq.", -1);
        else sqlite3_result_error(context, "Unable to retrieve changes in cloudsync_network_send_changes", -1);
        return rc;
    }
        
    // exit if there is no data to send
    if (blob == NULL || blob_size == 0) return SQLITE_OK;
    
    NETWORK_RESULT res = network_receive_buffer(netdata, netdata->upload_endpoint, netdata->authentication, true, false, NULL, CLOUDSYNC_HEADER_SQLITECLOUD);
    if (res.code != CLOUDSYNC_NETWORK_BUFFER) {
        cloudsync_memory_free(blob);
        network_result_to_sqlite_error(context, res, "cloudsync_network_send_changes unable to receive upload URL");
        network_result_cleanup(&res);
        return SQLITE_ERROR;
    }
    
    const char *s3_url = res.buffer;
    bool sent = network_send_buffer(netdata, s3_url, NULL, blob, blob_size);
    cloudsync_memory_free(blob);
    if (sent == false) {
        network_result_to_sqlite_error(context, res, "cloudsync_network_send_changes unable to upload BLOB changes to remote host.");
        network_result_cleanup(&res);
        return SQLITE_ERROR;
    }
    
    char json_payload[2024];
    snprintf(json_payload, sizeof(json_payload), "{\"url\":\"%s\", \"dbVersionMin\":%d, \"dbVersionMax\":%lld}", s3_url, db_version, (long long)new_db_version);
    
    // free res
    network_result_cleanup(&res);
    
    // notify remote host that we succesfully uploaded changes
    res = network_receive_buffer(netdata, netdata->apply_endpoint, netdata->authentication, true, true, json_payload, CLOUDSYNC_HEADER_SQLITECLOUD);
    if (res.code != CLOUDSYNC_NETWORK_OK) {
        network_result_to_sqlite_error(context, res, "cloudsync_network_send_changes unable to notify BLOB upload to remote host.");
        network_result_cleanup(&res);
        return SQLITE_ERROR;
    }
    
    // update db_version and seq
    char buf[256];
    if (new_db_version != db_version) {
        snprintf(buf, sizeof(buf), "%" PRId64, new_db_version);
        dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_SEND_DBVERSION, buf);
    }
    if (new_seq != seq) {
        snprintf(buf, sizeof(buf), "%" PRId64, new_seq);
        dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_SEND_SEQ, buf);
    }
    
    network_result_cleanup(&res);
    return SQLITE_OK;
}

void cloudsync_network_send_changes (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_send_changes");
    
    cloudsync_network_send_changes_internal(context, argc, argv);
}

int cloudsync_network_check_internal(sqlite3_context *context, int *pnrows) {
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    network_data *netdata = (network_data *)cloudsync_auxdata(data);
    if (!netdata) {sqlite3_result_error(context, "Unable to retrieve CloudSync network context.", -1); return -1;}

    int64_t db_version = dbutils_settings_get_int64_value(data, CLOUDSYNC_KEY_CHECK_DBVERSION);
    if (db_version<0) {sqlite3_result_error(context, "Unable to retrieve db_version.", -1); return -1;}

    int seq = dbutils_settings_get_int_value(data, CLOUDSYNC_KEY_CHECK_SEQ);
    if (seq<0) {sqlite3_result_error(context, "Unable to retrieve seq.", -1); return -1;}

    char json_payload[2024];
    snprintf(json_payload, sizeof(json_payload), "{\"dbVersion\":%lld, \"seq\":%d}", (long long)db_version, seq);

    // http://uuid.g5.sqlite.cloud/v2/cloudsync/{dbname}/{site_id}/check
    NETWORK_RESULT result = network_receive_buffer(netdata, netdata->check_endpoint, netdata->authentication, true, true, json_payload, CLOUDSYNC_HEADER_SQLITECLOUD);
    int rc = SQLITE_OK;
    if (result.code == CLOUDSYNC_NETWORK_BUFFER) {
        rc = network_download_changes(context, result.buffer, pnrows);
    } else {
        rc = network_set_sqlite_result(context, &result);
    }
    
    network_result_cleanup(&result);
    return rc;
}

void cloudsync_network_sync (sqlite3_context *context, int wait_ms, int max_retries) {
    int rc = cloudsync_network_send_changes_internal(context, 0, NULL);
    if (rc != SQLITE_OK) return;
    
    int ntries = 0;
    int nrows = 0;
    while (ntries < max_retries) {
        if (ntries > 0) sqlite3_sleep(wait_ms);
        rc = cloudsync_network_check_internal(context, &nrows);
        if (rc == SQLITE_OK && nrows > 0) break;
        ntries++;
    }
    
    sqlite3_result_error_code(context, (nrows == -1) ? SQLITE_ERROR : SQLITE_OK);
    if (nrows >= 0) sqlite3_result_int(context, nrows);
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


void cloudsync_network_check_changes (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_check_changes");
    
    int nrows = 0;
    cloudsync_network_check_internal(context, &nrows);
    
    // returns number of applied rows
    sqlite3_result_int(context, nrows);
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

// MARK: -

int cloudsync_network_register (sqlite3 *db, char **pzErrMsg, void *ctx) {
    const int DEFAULT_FLAGS = SQLITE_UTF8 | SQLITE_INNOCUOUS;
    int rc = SQLITE_OK;
    
    rc = sqlite3_create_function(db, "cloudsync_network_init", 1, DEFAULT_FLAGS, ctx, cloudsync_network_init, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
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
    
    rc = sqlite3_create_function(db, "cloudsync_network_check_changes", 0, DEFAULT_FLAGS, ctx, cloudsync_network_check_changes, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function(db, "cloudsync_network_sync", 0, DEFAULT_FLAGS, ctx, cloudsync_network_sync0, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function(db, "cloudsync_network_sync", 2, DEFAULT_FLAGS, ctx, cloudsync_network_sync2, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function(db, "cloudsync_network_reset_sync_version", 0, DEFAULT_FLAGS, ctx, cloudsync_network_reset_sync_version, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function(db, "cloudsync_network_logout", 0, DEFAULT_FLAGS, ctx, cloudsync_network_logout, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
cleanup:
    if ((rc != SQLITE_OK) && (pzErrMsg)) {
        *pzErrMsg = sqlite3_mprintf("Error creating function in cloudsync_network_register: %s", sqlite3_errmsg(db));
    }
    
    return rc;
}

#endif
