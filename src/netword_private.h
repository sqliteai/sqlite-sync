//
//  netword_private.h
//  cloudsync
//
//  Created by Marco Bambini on 23/05/25.
//

#ifndef __CLOUDSYNC_NETWORK_PRIVATE__
#define __CLOUDSYNC_NETWORK_PRIVATE__

#define CLOUDSYNC_ENDPOINT_PREFIX           "v1/cloudsync"
#define CLOUDSYNC_ENDPOINT_UPLOAD           "upload"
#define CLOUDSYNC_ENDPOINT_CHECK            "check"
#define CLOUDSYNC_DEFAULT_ENDPOINT_PORT     "443"
#define CLOUDSYNC_HEADER_SQLITECLOUD        "Accept: sqlc/plain"

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
bool network_data_set_endpoints (network_data *data, char *auth, char *check, char *upload, bool duplicate);

bool network_compute_endpoints (sqlite3_context *context, network_data *data, const char *conn_string);
bool network_send_buffer(network_data *data, const char *endpoint, const char *authentication, const void *blob, int blob_size);
NETWORK_RESULT network_receive_buffer (network_data *data, const char *endpoint, const char *authentication, bool zero_terminated, bool is_post_request, char *json_payload, const char *custom_header);


#endif
