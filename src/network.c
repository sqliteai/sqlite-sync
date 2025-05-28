//
//  network.c
//  cloudsync
//
//  Created by Marco Bambini on 12/12/24.
//

#ifndef CLOUDSYNC_OMIT_NETWORK

#include <stdint.h>
#include "network.h"
#include "dbutils.h"
#include "utils.h"
#include "curl/curl.h"

#define CLOUDSYNC_ENDPOINT_PREFIX           "v1/cloudsync"
#define CLOUDSYNC_ENDPOINT_UPLOAD           "upload"
#define CLOUDSYNC_ENDPOINT_CHECK            "check"
#define CLOUDSYNC_DEFAULT_ENDPOINT_PORT     "443"
#define CLOUDSYNC_HEADER_SQLITECLOUD        "Accept: sqlc/plain"

#define CLOUDSYNC_NETWORK_MINBUF_SIZE       512
#define CLOUDSYNC_SESSION_TOKEN_MAXSIZE     4096

#define CLOUDSYNC_NETWORK_OK                1
#define CLOUDSYNC_NETWORK_ERROR             2
#define CLOUDSYNC_NETWORK_BUFFER            3

#define MAX_QUERY_VALUE_LEN                 256

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT3
#endif

// MARK: -

typedef struct {
    char    site_id[UUID_STR_MAXLEN];
    char    *authentication; // apikey or token
    char    *check_endpoint;
    char    *upload_endpoint;
} network_data;

typedef struct {
    char        *buffer;
    size_t      balloc;
    size_t      bused;
    int         zero_term;
} network_buffer;

typedef struct {
    int         code;
    char        *buffer;
    size_t      blen;       // blen if code is SQLITE_OK, rc in case of error
} NETWORK_RESULT;

// MARK: -

static bool network_buffer_check (network_buffer *data, size_t needed) {
    // alloc/resize buffer
    if (data->bused + needed > data->balloc) {
        if (needed < CLOUDSYNC_NETWORK_MINBUF_SIZE) needed = CLOUDSYNC_NETWORK_MINBUF_SIZE;
        size_t balloc = (data->balloc * 2) + needed;
        
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
    
    if (network_buffer_check(data, ptr_size) == false) return -1;
    memcpy(data->buffer+data->bused, ptr, size*nmemb);
    data->bused += size*nmemb;
    if (data->zero_term) data->buffer[data->bused] = 0;
    
    return (size * nmemb);
}

static NETWORK_RESULT network_receive_buffer (network_data *data, const char *endpoint, const char *authentication, bool zero_terminated, bool is_post_request, char *json_payload, const char *custom_header) {
    char *buffer = NULL;
    size_t blen = 0;
    struct curl_slist* headers = NULL;
    char errbuf[CURL_ERROR_SIZE] = {0};
    
    CURL *curl = curl_easy_init();
    if (!curl) return (NETWORK_RESULT){CLOUDSYNC_NETWORK_ERROR, NULL, 0};
    
    // a buffer to store errors in
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    CURLcode rc = curl_easy_setopt(curl, CURLOPT_URL, endpoint);
    if (rc != CURLE_OK) goto cleanup;
    
    if (custom_header) headers = curl_slist_append(headers, custom_header);

    if (authentication) {
        char auth_header[CLOUDSYNC_SESSION_TOKEN_MAXSIZE];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", authentication);
        headers = curl_slist_append(headers, auth_header);
        
        if (json_payload) headers = curl_slist_append(headers, "Content-Type: application/json");
    }
    
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    
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
    if (rc == CURLE_OK) {
        buffer = netdata.buffer;
        blen = netdata.bused;
    } else {
        if (netdata.buffer) cloudsync_memory_free(netdata.buffer);
    }

cleanup:
    if (curl) curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);
    
    // build result
    NETWORK_RESULT result = {0, NULL, 0};
    if (rc == CURLE_OK) {
        result.code = (buffer && blen) ? CLOUDSYNC_NETWORK_BUFFER : CLOUDSYNC_NETWORK_OK;
        result.buffer = buffer;
        result.blen = blen;
    } else {
        result.code = CLOUDSYNC_NETWORK_ERROR;
        result.buffer = (errbuf[0]) ? cloudsync_string_dup(errbuf, false) : NULL;
        result.blen = rc;
    }
    
    return result;
}

typedef struct {
    const char *data;
    size_t size;
    size_t read_pos;
} network_read_data;

static size_t network_read_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
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

static bool network_send_buffer (network_data *data, const char *endpoint, const char *authentication, const void *blob, int blob_size) {
    struct curl_slist *headers = NULL;
    curl_mime *mime = NULL;
    bool result = false;
    char errbuf[CURL_ERROR_SIZE] = {0};

    // init curl
    CURL *curl = curl_easy_init();
    if (!curl) return false;

    // set the URL
    if (curl_easy_setopt(curl, CURLOPT_URL, endpoint) != CURLE_OK) goto cleanup;

    // a buffer to store errors in
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    
    // type header
    headers = curl_slist_append(headers, "Accept: text/plain");
    
    if (authentication) {
        // init authorization header
        char auth_header[CLOUDSYNC_SESSION_TOKEN_MAXSIZE];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", data->authentication);
        headers = curl_slist_append(headers, auth_header);
    }
    
    // Set headers if needed (S3 pre-signed URLs usually do not require additional headers)
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    
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
    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) result = true;
       
cleanup:
    if (mime) curl_mime_free(mime);
    if (curl) curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);
    return result;
}

int network_set_sqlite_result (sqlite3_context *context, NETWORK_RESULT *result) {
    int len = 0;
    switch (result->code) {
        case CLOUDSYNC_NETWORK_OK:
            sqlite3_result_int(context, SQLITE_OK);
            len = 0;
            break;
            
        case CLOUDSYNC_NETWORK_ERROR:
            sqlite3_result_error(context, (result->buffer) ? result->buffer : "Memory error.", -1);
            sqlite3_result_error_code(context, (int)result->blen);
            len = -1;
            break;
            
        case CLOUDSYNC_NETWORK_BUFFER:
            sqlite3_result_text(context, result->buffer, (int)result->blen, SQLITE_TRANSIENT);
            len = (int)result->blen;
            break;
    }
    
    if (result->buffer) cloudsync_memory_free(result->buffer);
    
    return len;
}

int network_download_changes (sqlite3_context *context, const char *download_url) {
    DEBUG_FUNCTION("network_download_changes");
    
    int nrows = 0;
    network_data *data = (network_data *)cloudsync_get_auxdata(context);
    if (!data) {sqlite3_result_error(context, "Unable to retrieve CloudSync context.", -1); return -1;}
    
    NETWORK_RESULT result = network_receive_buffer(data, download_url, NULL, false, false, NULL, NULL);
    if (result.code == CLOUDSYNC_NETWORK_BUFFER) {
        nrows = cloudsync_payload_apply(context, result.buffer, (int)result.blen);
        cloudsync_memory_free(result.buffer);
    } else {
        nrows = network_set_sqlite_result(context, &result);
    }
    
    return nrows;
}

char *network_authentication_token(const char *key, const char *value) {
    size_t len = strlen(key) + strlen(value) + 64;
    char *buffer = cloudsync_memory_zeroalloc(len);
    if (!buffer) return NULL;
    
    // build new token
    // we don't need a prefix because the token alreay include a prefix "sqa_"
    snprintf(buffer, len, "%s", value);
    
    return buffer;
}

int extract_query_param(const char *query, const char *key, char *output, size_t output_size) {
    if (!query || !key || !output || output_size == 0) {
        return -1; // Invalid input
    }

    size_t key_len = strlen(key);
    const char *p = query;

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
    
    char *conn_string_https = NULL;
    
    CURLUcode rc = CURLUE_OUT_OF_MEMORY;
    CURLU *url = curl_url();
    if (!url) goto finalize;
    
    conn_string_https = cloudsync_string_replace_prefix(conn_string, "sqlitecloud://", "https://");
    
    // set URL: https://UUID.g5.sqlite.cloud:443/chinook.sqlite?apikey=hWDanFolRT9WDK0p54lufNrIyfgLZgtMw6tb6fbPmpo
    rc = curl_url_set(url, CURLUPART_URL, conn_string_https, 0);
    if (rc != CURLE_OK) goto finalize;
    
    // https (MANDATORY)
    rc = curl_url_get(url, CURLUPART_SCHEME, &scheme, 0);
    if (rc != CURLE_OK) goto finalize;
    
    // UUID.g5.sqlite.cloud (MANDATORY)
    rc = curl_url_get(url, CURLUPART_HOST, &host, 0);
    if (rc != CURLE_OK) goto finalize;
    
    // 443 (OPTIONAL)
    rc = curl_url_get(url, CURLUPART_PORT, &port, 0);
    if (rc != CURLE_OK && rc != CURLUE_NO_PORT) goto finalize;
    char *port_or_default = port && strcmp(port, "8860") != 0 ? port : CLOUDSYNC_DEFAULT_ENDPOINT_PORT;

    // /chinook.sqlite (MANDATORY)
    rc = curl_url_get(url, CURLUPART_PATH, &database, 0);
    if (rc != CURLE_OK) goto finalize;
    
    // apikey=hWDanFolRT9WDK0p54lufNrIyfgLZgtMw6tb6fbPmpo (OPTIONAL)
    rc = curl_url_get(url, CURLUPART_QUERY, &query, 0);
    if (rc != CURLE_OK && rc != CURLUE_NO_QUERY) goto finalize;
    if (query != NULL) {
        char value[MAX_QUERY_VALUE_LEN];
        if (!authentication && extract_query_param(query, "apikey", value, sizeof(value)) == 0) {
            authentication = network_authentication_token("apikey", value);
        }
        if (!authentication && extract_query_param(query, "token", value, sizeof(value)) == 0) {
            authentication = network_authentication_token("token", value);
        }
    }
    
    size_t requested = strlen(scheme) + strlen(host) + strlen(port_or_default) + strlen(CLOUDSYNC_ENDPOINT_PREFIX) + strlen(database) + 64;
    check_endpoint = (char *)cloudsync_memory_zeroalloc(requested);
    upload_endpoint = (char *)cloudsync_memory_zeroalloc(requested);
    if ((!upload_endpoint) || (!check_endpoint)) goto finalize;
    
    snprintf(check_endpoint, requested, "%s://%s:%s/%s%s/%s", scheme, host, port_or_default, CLOUDSYNC_ENDPOINT_PREFIX, database, data->site_id);
    snprintf(upload_endpoint, requested, "%s://%s:%s/%s%s/%s/%s", scheme, host, port_or_default, CLOUDSYNC_ENDPOINT_PREFIX, database, data->site_id, CLOUDSYNC_ENDPOINT_UPLOAD);
    
    result = true;
    
finalize:
    if (result == false) {
        // store proper result code/message
        if (rc != CURLE_OK) sqlite3_result_error(context, curl_url_strerror(rc), -1);
        sqlite3_result_error_code(context, (rc != CURLE_OK) ? SQLITE_ERROR : SQLITE_NOMEM);
        
        // cleanup memory managed by the extension
        if (authentication) cloudsync_memory_free(authentication);
        if (check_endpoint) cloudsync_memory_free(check_endpoint);
        if (upload_endpoint) cloudsync_memory_free(upload_endpoint);
    }
        
    if (result) {
        data->authentication = authentication;
        data->check_endpoint = check_endpoint;
        data->upload_endpoint = upload_endpoint;
    }
    
    // cleanup memory managed by libcurl
    if (scheme) curl_free(scheme);
    if (host) curl_free(host);
    if (port) curl_free(port);
    if (database) curl_free(database);
    if (query) curl_free(query);
    if (url) curl_url_cleanup(url);
    if (conn_string_https && conn_string_https != conn_string) cloudsync_memory_free(conn_string_https);
    
    return result;
}

// MARK: -

void cloudsync_network_init (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_init");
    
    curl_global_init(CURL_GLOBAL_ALL);
    
    // no real network operations here
    // just setup the network_data struct
    network_data *data = (network_data *)cloudsync_memory_zeroalloc(sizeof(network_data));
    if (!data) goto abort_memory;
    
    // init context
    uint8_t *site_id = (uint8_t *)cloudsync_context_init(sqlite3_context_db_handle(context), NULL, context);
    if (!site_id) goto abort_siteid;
    
    // save site_id string representation: 01957493c6c07e14803727e969f1d2cc
    cloudsync_uuid_v7_stringify(site_id, data->site_id, false);
    
    // connection string is something like:
    // https://UUID.g5.sqlite.cloud:443/chinook.sqlite?apikey=hWDanFolRT9WDK0p54lufNrIyfgLZgtMw6tb6fbPmpo
    // or https://UUID.g5.sqlite.cloud:443/chinook.sqlite
    // apikey part is optional and can be replaced by a session token once client is authenticated
    
    const char *connection_param = (const char *)sqlite3_value_text(argv[0]);
    
    // compute endpoints
    if (network_compute_endpoints(context, data, connection_param) == false) {
        // error message/code already set inside network_compute_endpoints
        goto abort_cleanup;
    }
    
    cloudsync_set_auxdata(context, data);
    sqlite3_result_int(context, SQLITE_OK);
    return;
    
abort_memory:
    dbutils_context_result_error(context, "Unable to allocate memory in cloudsync_network_init.");
    sqlite3_result_error_code(context, SQLITE_NOMEM);
    goto abort_cleanup;
    
abort_siteid:
    dbutils_context_result_error(context, "Unable to compute/retrieve site_id.");
    sqlite3_result_error_code(context, SQLITE_MISUSE);
    goto abort_cleanup;
    
abort_cleanup:
    if (data) {
        if (data->authentication) cloudsync_memory_free(data->authentication);
        if (data->check_endpoint) cloudsync_memory_free(data->check_endpoint);
        if (data->upload_endpoint) cloudsync_memory_free(data->upload_endpoint);
        cloudsync_memory_free(data);
    }
}

void cloudsync_network_cleanup (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_cleanup");
    
    network_data *data = (network_data *)cloudsync_get_auxdata(context);
    if (data) {
        if (data->authentication) cloudsync_memory_free(data->authentication);
        if (data->check_endpoint) cloudsync_memory_free(data->check_endpoint);
        if (data->upload_endpoint) cloudsync_memory_free(data->upload_endpoint);
        cloudsync_memory_free(data);
    }
    
    sqlite3_result_int(context, SQLITE_OK);
    curl_global_cleanup();
}

// MARK: -

bool cloudsync_network_set_authentication_token (sqlite3_context *context, const char *value, bool is_token) {
    network_data *data = (network_data *)cloudsync_get_auxdata(context);
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

void network_result_to_sqlite_error (sqlite3_context *context, NETWORK_RESULT res, const char *default_error_message) {
    sqlite3_result_error(context, ((res.code == CLOUDSYNC_NETWORK_ERROR) && (res.buffer)) ? res.buffer : default_error_message, -1);
    sqlite3_result_error_code(context, ((res.code == CLOUDSYNC_NETWORK_ERROR) && (res.blen)) ? (int)res.blen : SQLITE_ERROR);
    if (res.buffer) cloudsync_memory_free(res.buffer);
}

void cloudsync_network_send_changes (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_send_changes");
    
    network_data *data = (network_data *)cloudsync_get_auxdata(context);
    if (!data) {sqlite3_result_error(context, "Unable to retrieve CloudSync context.", -1); return;}
    
    sqlite3 *db = sqlite3_context_db_handle(context);

    int db_version = dbutils_settings_get_int_value(db, CLOUDSYNC_KEY_SEND_DBVERSION);
    if (db_version<0) {sqlite3_result_error(context, "Unable to retrieve db_version.", -1); return;}

    int seq = dbutils_settings_get_int_value(db, CLOUDSYNC_KEY_SEND_SEQ);
    if (seq<0) {sqlite3_result_error(context, "Unable to retrieve seq.", -1); return;}
    
    // retrieve BLOB
    char sql[1024];
    snprintf(sql, sizeof(sql), "WITH max_db_version AS (SELECT MAX(db_version) AS max_db_version FROM cloudsync_changes) "
                               "SELECT cloudsync_network_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), max_db_version AS max_db_version, MAX(IIF(db_version = max_db_version, seq, NULL)) FROM cloudsync_changes, max_db_version WHERE site_id=cloudsync_siteid() AND (db_version>%d OR (db_version=%d AND seq>%d))", db_version, db_version, seq);
    int blob_size = 0;
    char *blob = NULL;
    sqlite3_int64 new_db_version = 0;
    sqlite3_int64 new_seq = 0;
    int rc = dbutils_blob_int_int_select(db, sql, &blob, &blob_size, &new_db_version, &new_seq);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, "cloudsync_network_send_changes unable to get changes", -1);
        sqlite3_result_error_code(context, rc);
        return;
    }
    
    // exit if there are no data to send
    if (blob == NULL || blob_size == 0) return;
    
    NETWORK_RESULT res = network_receive_buffer(data, data->upload_endpoint, data->authentication, true, false, NULL, CLOUDSYNC_HEADER_SQLITECLOUD);
    if (res.code != CLOUDSYNC_NETWORK_BUFFER) {
        network_result_to_sqlite_error(context, res, "cloudsync_network_send_changes unable to receive upload URL");
        return;
    }
    
    const char *s3_url = res.buffer;
    bool sent = network_send_buffer(data, s3_url, NULL, blob, blob_size);
    cloudsync_memory_free(blob);
    if (sent == false) {
        network_result_to_sqlite_error(context, res, "cloudsync_network_send_changes unable to upload BLOB changes to remote host.");
        return;
    }
    
    char json_payload[2024];
    snprintf(json_payload, sizeof(json_payload), "{\"url\":\"%s\"}", s3_url);
    
    // free res
    if (res.buffer) cloudsync_memory_free(res.buffer);
    
    // notify remote host that we succesfully uploaded changes
    res = network_receive_buffer(data, data->upload_endpoint, data->authentication, true, true, json_payload, CLOUDSYNC_HEADER_SQLITECLOUD);
    if (res.code != CLOUDSYNC_NETWORK_OK) {
        network_result_to_sqlite_error(context, res, "cloudsync_network_send_changes unable to notify BLOB upload to remote host.");
        return;
    }
    
    char buf[256];
    if (new_db_version != db_version) {
        snprintf(buf, sizeof(buf), "%lld", new_db_version);
        dbutils_settings_set_key_value(db, context, CLOUDSYNC_KEY_SEND_DBVERSION, buf);
    }
    if (new_seq != seq) {
        snprintf(buf, sizeof(buf), "%lld", new_seq);
        dbutils_settings_set_key_value(db, context, CLOUDSYNC_KEY_SEND_SEQ, buf);
    }
    
    if (res.buffer) cloudsync_memory_free(res.buffer);
}

int cloudsync_network_check_internal(sqlite3_context *context, int argc, sqlite3_value **argv) {
    network_data *data = (network_data *)cloudsync_get_auxdata(context);
    if (!data) {sqlite3_result_error(context, "Unable to retrieve CloudSync context.", -1); return -1;}
     
    sqlite3 *db = sqlite3_context_db_handle(context);
    
    int db_version = dbutils_settings_get_int_value(db, CLOUDSYNC_KEY_CHECK_DBVERSION);
    if (db_version<0) {sqlite3_result_error(context, "Unable to retrieve db_version.", -1); return -1;}

    int seq = dbutils_settings_get_int_value(db, CLOUDSYNC_KEY_CHECK_SEQ);
    if (seq<0) {sqlite3_result_error(context, "Unable to retrieve seq.", -1); return -1;}

    // http://uuid.g5.sqlite.cloud/v1/cloudsync/{dbname}/{site_id}/{db_version}/{seq}/check
    // the data->check_endpoint stops after {site_id}, just need to append /{db_version}/{seq}/check
    char endpoint[2024];
    snprintf(endpoint, sizeof(endpoint), "%s/%lld/%d/%s", data->check_endpoint, (long long)db_version, seq, CLOUDSYNC_ENDPOINT_CHECK);
    
    NETWORK_RESULT result = network_receive_buffer(data, endpoint, data->authentication, true, true, NULL, CLOUDSYNC_HEADER_SQLITECLOUD);
    int rc = SQLITE_OK;
    if (result.code == CLOUDSYNC_NETWORK_BUFFER) {
        rc = network_download_changes (context, result.buffer);
    } else {
        rc = network_set_sqlite_result(context, &result);
    }
    
    return rc;
}

void cloudsync_network_check_changes_sync (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_check_changes_sync");

    int sleep_ms = sqlite3_value_int(argv[0]);
    int max_retries = sqlite3_value_int(argv[1]);

    int nrows = 0;
    int retries = 0;
    while (retries < max_retries) {
        nrows = cloudsync_network_check_internal(context, argc, argv);
        if (nrows > 0) break;
        else sqlite3_sleep(sleep_ms);
        retries++;
    }
}


void cloudsync_network_check_changes (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_check_changes");
    
    cloudsync_network_check_internal(context, argc, argv);
}

void cloudsync_network_reset_check_version (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_network_reset_check_version");
    
    sqlite3 *db = sqlite3_context_db_handle(context);
    char *buf = "0";
    dbutils_settings_set_key_value(db, context, CLOUDSYNC_KEY_CHECK_DBVERSION, buf);
    dbutils_settings_set_key_value(db, context, CLOUDSYNC_KEY_CHECK_SEQ, buf);
}

// MARK: -

int cloudsync_network_register (sqlite3 *db, char **pzErrMsg, void *ctx) {
    int rc = SQLITE_OK;
    
    rc = dbutils_register_function(db, "cloudsync_network_init", cloudsync_network_init, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_network_cleanup", cloudsync_network_cleanup, 0, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_set_token", cloudsync_network_set_token, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_set_apikey", cloudsync_network_set_apikey, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_network_send_changes", cloudsync_network_send_changes, 0, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_network_check_changes", cloudsync_network_check_changes, 0, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_network_check_changes_sync", cloudsync_network_check_changes_sync, 2, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = dbutils_register_function(db, "cloudsync_network_reset_check_version", cloudsync_network_reset_check_version, 0, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    return rc;
}
#endif
