#ifdef SQLITE_WASM_EXTRA_INIT

#include <stdio.h>
#include <stdlib.h>
#include <emscripten/fetch.h>
#include <emscripten/emscripten.h>

#include "sqlite3.h"
#include "netword_private.h"

#include "utils.c"
#include "network.c"
#include "dbutils.c"
#include "cloudsync.c"
#include "vtab.c"
#include "pk.c"
#include "lz4.c"

// MARK: - WASM -

char *substr(const char *start, const char *end) {
    size_t len = end - start;
    char *out = (char *)malloc(len + 1);
    if (out) {
        memcpy(out, start, len);
        out[len] = 0;
    }
    return out;
}

NETWORK_RESULT network_receive_buffer (network_data *data, const char *endpoint, const char *authentication, bool zero_terminated, bool is_post_request, char *json_payload, const char *custom_header) {
    char *buffer = NULL;
    size_t blen = 0;

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);

    // Set method
    if (json_payload || is_post_request) {
        strcpy(attr.requestMethod, "POST");
    } else {
        strcpy(attr.requestMethod, "GET");
    }
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS | EMSCRIPTEN_FETCH_REPLACE;

    // Prepare header array (alternating key, value, NULL-terminated)
    const char *headers[11];
    int h = 0;

    // Custom header (must be "Key: Value", split at ':')
    char *custom_key = NULL;
    if (custom_header) {
        const char *colon = strchr(custom_header, ':');
        if (colon) {
            size_t klen = colon - custom_header;
            custom_key = (char *)malloc(klen + 1);
            strncpy(custom_key, custom_header, klen);
            custom_key[klen] = 0;
            const char *custom_val = colon + 1;
            while (*custom_val == ' ') custom_val++;
            headers[h++] = custom_key;
            headers[h++] = custom_val;
        }
    }

    // Authorization
    char auth_header[256];
    if (authentication) {
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", authentication);
        headers[h++] = "Authorization";
        headers[h++] = auth_header;
    }

    // Content-Type for JSON
    if (json_payload) {
        headers[h++] = "Content-Type";
        headers[h++] = "application/json";
    }
    
    headers[h] = 0;
    attr.requestHeaders = headers;

    // Body
    if (json_payload) {
        attr.requestData = json_payload;
        attr.requestDataSize = strlen(json_payload);
    }

    emscripten_fetch_t *fetch = emscripten_fetch(&attr, endpoint); // Blocks here until the operation is complete.
    NETWORK_RESULT result = {0, NULL, 0, NULL, NULL};

    if(fetch->readyState == 4){
        buffer = fetch->data;
        blen = fetch->totalBytes;
    }
    
    if (fetch->status >= 200 && fetch->status < 300) {
        
        if (blen > 0 && buffer) {
            char *buf = (char*)malloc(blen + 1);
            if (buf) {
                memcpy(buf, buffer, blen);
                buf[blen] = 0;
                result.code = CLOUDSYNC_NETWORK_BUFFER;
                result.buffer = buf;
                result.blen = blen;
                result.xfree = free;
            } else result.code = CLOUDSYNC_NETWORK_ERROR;
        } else result.code = CLOUDSYNC_NETWORK_OK;
    } else {
        result.code = CLOUDSYNC_NETWORK_ERROR;
        if (fetch->statusText && fetch->statusText[0]) {
            result.buffer = strdup(fetch->statusText);
        }
        result.blen = sizeof(fetch->statusText);
        result.xfree = free;
    }

    // cleanup
    emscripten_fetch_close(fetch);
    if (custom_key) free(custom_key);

    return result;
}

bool network_send_buffer(network_data *data, const char *endpoint, const char *authentication, const void *blob, int blob_size) {

    bool result = false;
    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    strcpy(attr.requestMethod, "PUT");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS | EMSCRIPTEN_FETCH_REPLACE;

    // Prepare headers (alternating key, value, NULL-terminated)
    // Max 3 headers: Accept, (optional Auth), Content-Type
    const char *headers[7];
    int h = 0;
    headers[h++] = "Accept";
    headers[h++] = "text/plain";
    char auth_header[256];
    if (authentication) {
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", authentication);
        headers[h++] = "Authorization";
        headers[h++] = auth_header;
    }
    headers[h++] = "Content-Type";
    headers[h++] = "application/octet-stream";
    headers[h] = 0;
    attr.requestHeaders = headers;

    // Set request body
    attr.requestData = (const char *)blob;
    attr.requestDataSize = blob_size;

    emscripten_fetch_t *fetch = emscripten_fetch(&attr, endpoint); // Blocks here until the operation is complete.
    if (fetch->status >= 200 && fetch->status < 300) result = true;

    emscripten_fetch_close(fetch);

    return result;
}

// MARK: -

int sqlite3_wasm_extra_init(const char *z) {
    fprintf(stderr, "%s: %s()\n", __FILE__, __func__);
    return sqlite3_auto_extension((void *) sqlite3_cloudsync_init);
}

#endif
