//
//  network.c
//  cloudsync
//
//  Created by Marco Bambini on 12/12/24.
//

#ifndef CLOUDSYNC_OMIT_NETWORK

#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>

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
    char        *org_id;         // organization ID for X-CloudSync-Org header
    char        *check_endpoint;
    char        *upload_endpoint;
    char        *apply_endpoint;
    char        *status_endpoint;
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

char *network_data_get_orgid (network_data *data) {
    return data->org_id;
}

bool network_data_set_endpoints (network_data *data, char *auth, char *check, char *upload, char *apply, char *status) {
    // sanity check
    if (!check || !upload) return false;

    // always free previous owned pointers
    if (data->authentication) cloudsync_memory_free(data->authentication);
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

    if (data->authentication) cloudsync_memory_free(data->authentication);
    if (data->org_id) cloudsync_memory_free(data->org_id);
    if (data->check_endpoint) cloudsync_memory_free(data->check_endpoint);
    if (data->upload_endpoint) cloudsync_memory_free(data->upload_endpoint);
    if (data->apply_endpoint) cloudsync_memory_free(data->apply_endpoint);
    if (data->status_endpoint) cloudsync_memory_free(data->status_endpoint);
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

// MARK: - JSON helpers (jsmn) -

#define JSMN_MAX_TOKENS 64

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

    jsmn_parser parser;
    jsmntok_t tokens[JSMN_MAX_TOKENS];
    jsmn_init(&parser);
    int ntokens = jsmn_parse(&parser, json, json_len, tokens, JSMN_MAX_TOKENS);
    if (ntokens < 1) return NULL;

    int i = jsmn_find_key(json, tokens, ntokens, key);
    if (i < 0 || i + 1 >= ntokens) return NULL;

    jsmntok_t *val = &tokens[i + 1];
    if (val->type != JSMN_STRING) return NULL;

    return json_unescape_string(json + val->start, val->end - val->start);
}

static int64_t json_extract_int(const char *json, size_t json_len, const char *key, int64_t default_value) {
    if (!json || json_len == 0 || !key) return default_value;

    jsmn_parser parser;
    jsmntok_t tokens[JSMN_MAX_TOKENS];
    jsmn_init(&parser);
    int ntokens = jsmn_parse(&parser, json, json_len, tokens, JSMN_MAX_TOKENS);
    if (ntokens < 1 || tokens[0].type != JSMN_OBJECT) return default_value;

    int i = jsmn_find_key(json, tokens, ntokens, key);
    if (i < 0 || i + 1 >= ntokens) return default_value;

    jsmntok_t *val = &tokens[i + 1];
    if (val->type != JSMN_PRIMITIVE) return default_value;

    return strtoll(json + val->start, NULL, 10);
}

static int json_extract_array_size(const char *json, size_t json_len, const char *key) {
    if (!json || json_len == 0 || !key) return -1;

    jsmn_parser parser;
    jsmntok_t tokens[JSMN_MAX_TOKENS];
    jsmn_init(&parser);
    int ntokens = jsmn_parse(&parser, json, json_len, tokens, JSMN_MAX_TOKENS);
    if (ntokens < 1 || tokens[0].type != JSMN_OBJECT) return -1;

    int i = jsmn_find_key(json, tokens, ntokens, key);
    if (i < 0 || i + 1 >= ntokens) return -1;

    jsmntok_t *val = &tokens[i + 1];
    if (val->type != JSMN_ARRAY) return -1;

    return val->size;
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

typedef struct {
    int64_t     server_version;   // lastOptimisticVersion
    int64_t     local_version;    // new_db_version (max local)
    const char  *status;          // computed status string
    int         rows_received;    // rows from check
    char        *tables_json;     // JSON array of affected table names, caller must cloudsync_memory_free
} sync_result;

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
    
    NETWORK_RESULT res = network_receive_buffer(netdata, netdata->status_endpoint, netdata->authentication, true, false, NULL, CLOUDSYNC_HEADER_SQLITECLOUD);
    
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
    
    // retrieve global context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    network_data *netdata = (network_data *)cloudsync_auxdata(data);
    if (!netdata) {sqlite3_result_error(context, "Unable to retrieve CloudSync network context.", -1); return SQLITE_ERROR;}
    
    // retrieve payload
    char *blob = NULL;
    int blob_size = 0, db_version = 0;
    int64_t new_db_version = 0;
    int rc = cloudsync_payload_get(data, &blob, &blob_size, &db_version, &new_db_version);
    if (rc != SQLITE_OK) {
        if (db_version < 0) sqlite3_result_error(context, "Unable to retrieve db_version.", -1);
        else sqlite3_result_error(context, "Unable to retrieve changes in cloudsync_network_send_changes", -1);
        return rc;
    }
    
    // Case 1: empty local db — no payload and no server state, skip network entirely
    if ((blob == NULL || blob_size == 0) && db_version == 0) {
        if (out) {
            out->server_version = 0;
            out->local_version = 0;
            out->status = network_compute_status(0, 0, 0, 0);
        }
        return SQLITE_OK;
    }

    NETWORK_RESULT res;
    if (blob != NULL && blob_size > 0) {
        // there is data to send
        res = network_receive_buffer(netdata, netdata->upload_endpoint, netdata->authentication, true, false, NULL, CLOUDSYNC_HEADER_SQLITECLOUD);
        if (res.code != CLOUDSYNC_NETWORK_BUFFER) {
            cloudsync_memory_free(blob);
            network_result_to_sqlite_error(context, res, "cloudsync_network_send_changes unable to receive upload URL");
            network_result_cleanup(&res);
            return SQLITE_ERROR;
        }
        
        char *s3_url = json_extract_string(res.buffer, res.blen, "url");
        if (!s3_url) {
            cloudsync_memory_free(blob);
            sqlite3_result_error(context, "cloudsync_network_send_changes: missing 'url' in upload response.", -1);
            network_result_cleanup(&res);
            return SQLITE_ERROR;
        }
        bool sent = network_send_buffer(netdata, s3_url, NULL, blob, blob_size);
        cloudsync_memory_free(blob);
        if (sent == false) {
            cloudsync_memory_free(s3_url);
            network_result_to_sqlite_error(context, res, "cloudsync_network_send_changes unable to upload BLOB changes to remote host.");
            network_result_cleanup(&res);
            return SQLITE_ERROR;
        }
        
        int db_version_min = db_version+1;
        int db_version_max = (int)new_db_version;
        if (db_version_min > db_version_max) db_version_min = db_version_max;
        char json_payload[4096];
        snprintf(json_payload, sizeof(json_payload), "{\"url\":\"%s\", \"dbVersionMin\":%d, \"dbVersionMax\":%d}", s3_url, db_version_min, db_version_max);
        cloudsync_memory_free(s3_url);
        
        // free res
        network_result_cleanup(&res);
        
        // notify remote host that we succesfully uploaded changes
        res = network_receive_buffer(netdata, netdata->apply_endpoint, netdata->authentication, true, true, json_payload, CLOUDSYNC_HEADER_SQLITECLOUD);
    } else {
        // there is no data to send, just check the status to update the db_version value in settings and to reply the status
        new_db_version = db_version;
        res = network_receive_buffer(netdata, netdata->status_endpoint, netdata->authentication, true, false, NULL, CLOUDSYNC_HEADER_SQLITECLOUD);
    }

    int64_t last_optimistic_version = -1;
    int64_t last_confirmed_version = -1;
    int gaps_size = -1;

    if (res.code == CLOUDSYNC_NETWORK_BUFFER && res.buffer) {
        last_optimistic_version = json_extract_int(res.buffer, res.blen, "lastOptimisticVersion", -1);
        last_confirmed_version = json_extract_int(res.buffer, res.blen, "lastConfirmedVersion", -1);
        gaps_size = json_extract_array_size(res.buffer, res.blen, "gaps");
        if (gaps_size < 0) gaps_size = 0;
    } else if (res.code != CLOUDSYNC_NETWORK_OK) {
        network_result_to_sqlite_error(context, res, "cloudsync_network_send_changes unable to notify BLOB upload to remote host.");
        network_result_cleanup(&res);
        return SQLITE_ERROR;
    }

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
    }

    network_result_cleanup(&res);
    return SQLITE_OK;
}

void cloudsync_network_send_changes (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_send_changes");

    sync_result sr = {-1, 0, NULL, 0, NULL};
    int rc = cloudsync_network_send_changes_internal(context, argc, argv, &sr);
    if (rc != SQLITE_OK) return;

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"send\":{\"status\":\"%s\",\"localVersion\":%" PRId64 ",\"serverVersion\":%" PRId64 "}}",
        sr.status ? sr.status : "error", sr.local_version, sr.server_version);
    sqlite3_result_text(context, buf, -1, SQLITE_TRANSIENT);
}

int cloudsync_network_check_internal(sqlite3_context *context, int *pnrows, sync_result *out) {
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    network_data *netdata = (network_data *)cloudsync_auxdata(data);
    if (!netdata) {sqlite3_result_error(context, "Unable to retrieve CloudSync network context.", -1); return -1;}

    int64_t db_version = dbutils_settings_get_int64_value(data, CLOUDSYNC_KEY_CHECK_DBVERSION);
    if (db_version<0) {sqlite3_result_error(context, "Unable to retrieve db_version.", -1); return -1;}

    int seq = dbutils_settings_get_int_value(data, CLOUDSYNC_KEY_CHECK_SEQ);
    if (seq<0) {sqlite3_result_error(context, "Unable to retrieve seq.", -1); return -1;}

    // Capture local db_version before download so we can query cloudsync_changes afterwards
    int64_t prev_dbv = cloudsync_dbversion(data);

    char json_payload[2024];
    snprintf(json_payload, sizeof(json_payload), "{\"dbVersion\":%lld, \"seq\":%d}", (long long)db_version, seq);

    NETWORK_RESULT result = network_receive_buffer(netdata, netdata->check_endpoint, netdata->authentication, true, true, json_payload, CLOUDSYNC_HEADER_SQLITECLOUD);
    int rc = SQLITE_OK;
    if (result.code == CLOUDSYNC_NETWORK_BUFFER) {
        char *download_url = json_extract_string(result.buffer, result.blen, "url");
        if (!download_url) {
            sqlite3_result_error(context, "cloudsync_network_check_changes: missing 'url' in check response.", -1);
            network_result_cleanup(&result);
            return SQLITE_ERROR;
        }
        rc = network_download_changes(context, download_url, pnrows);
        cloudsync_memory_free(download_url);
    } else {
        rc = network_set_sqlite_result(context, &result);
    }

    if (out && pnrows) out->rows_received = *pnrows;

    // Query cloudsync_changes for affected tables after successful download
    if (out && rc == SQLITE_OK && pnrows && *pnrows > 0) {
        sqlite3 *db = (sqlite3 *)cloudsync_db(data);
        out->tables_json = network_get_affected_tables(db, prev_dbv);
    }

    network_result_cleanup(&result);
    return rc;
}

void cloudsync_network_sync (sqlite3_context *context, int wait_ms, int max_retries) {
    sync_result sr = {-1, 0, NULL, 0, NULL};
    int rc = cloudsync_network_send_changes_internal(context, 0, NULL, &sr);
    if (rc != SQLITE_OK) return;

    int ntries = 0;
    int nrows = 0;
    while (ntries < max_retries) {
        if (ntries > 0) sqlite3_sleep(wait_ms);
        if (sr.tables_json) { cloudsync_memory_free(sr.tables_json); sr.tables_json = NULL; }
        rc = cloudsync_network_check_internal(context, &nrows, &sr);
        if (rc == SQLITE_OK && nrows > 0) break;
        ntries++;
    }
    if (rc != SQLITE_OK) { if (sr.tables_json) cloudsync_memory_free(sr.tables_json); return; }

    const char *tables = sr.tables_json ? sr.tables_json : "[]";
    char *buf = cloudsync_memory_mprintf(
        "{\"send\":{\"status\":\"%s\",\"localVersion\":%" PRId64 ",\"serverVersion\":%" PRId64 "},"
        "\"receive\":{\"rows\":%d,\"tables\":%s}}",
        sr.status ? sr.status : "error", sr.local_version, sr.server_version, nrows, tables);
    sqlite3_result_text(context, buf, -1, cloudsync_memory_free);
    if (sr.tables_json) cloudsync_memory_free(sr.tables_json);
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

    sync_result sr = {-1, 0, NULL, 0, NULL};
    int nrows = 0;
    int rc = cloudsync_network_check_internal(context, &nrows, &sr);
    if (rc != SQLITE_OK) { if (sr.tables_json) cloudsync_memory_free(sr.tables_json); return; }

    const char *tables = sr.tables_json ? sr.tables_json : "[]";
    char *buf = cloudsync_memory_mprintf("{\"receive\":{\"rows\":%d,\"tables\":%s}}", nrows, tables);
    sqlite3_result_text(context, buf, -1, cloudsync_memory_free);
    if (sr.tables_json) cloudsync_memory_free(sr.tables_json);
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

    NETWORK_RESULT res = network_receive_buffer(netdata, netdata->status_endpoint, netdata->authentication, true, false, NULL, CLOUDSYNC_HEADER_SQLITECLOUD);
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

    rc = sqlite3_create_function(db, "cloudsync_network_status", 0, DEFAULT_FLAGS, ctx, cloudsync_network_status, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

cleanup:
    if ((rc != SQLITE_OK) && (pzErrMsg)) {
        *pzErrMsg = sqlite3_mprintf("Error creating function in cloudsync_network_register: %s", sqlite3_errmsg(db));
    }
    
    return rc;
}

#endif
