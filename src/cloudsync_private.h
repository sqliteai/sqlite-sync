//
//  cloudsync_private.h
//  cloudsync
//
//  Created by Marco Bambini on 30/05/25.
//

#ifndef __CLOUDSYNC_PRIVATE__
#define __CLOUDSYNC_PRIVATE__

#include <stdbool.h>
#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif


#define CLOUDSYNC_TOMBSTONE_VALUE               "__[RIP]__"
#define CLOUDSYNC_RLS_RESTRICTED_VALUE          "__[RLS]__"
#define CLOUDSYNC_DISABLE_ROWIDONLY_TABLES      1

typedef enum {
    CLOUDSYNC_PAYLOAD_APPLY_WILL_APPLY   = 1,
    CLOUDSYNC_PAYLOAD_APPLY_DID_APPLY    = 2,
    CLOUDSYNC_PAYLOAD_APPLY_CLEANUP      = 3
} CLOUDSYNC_PAYLOAD_APPLY_STEPS;

typedef struct cloudsync_context cloudsync_context;
typedef struct cloudsync_pk_decode_bind_context cloudsync_pk_decode_bind_context;

int cloudsync_merge_insert (sqlite3_vtab *vtab, int argc, sqlite3_value **argv, sqlite3_int64 *rowid);
void cloudsync_sync_key (cloudsync_context *data, const char *key, const char *value);

// used by network layer
const char *cloudsync_context_init (sqlite3 *db, cloudsync_context *data, sqlite3_context *context);
void *cloudsync_get_auxdata (sqlite3_context *context);
void cloudsync_set_auxdata (sqlite3_context *context, void *xdata);
int cloudsync_payload_apply (sqlite3_context *context, const char *payload, int blen);

// used by core
typedef bool (*cloudsync_payload_apply_callback_t)(void **xdata, cloudsync_pk_decode_bind_context *decoded_change, sqlite3 *db, cloudsync_context *data, int step, int rc);
void cloudsync_set_payload_apply_callback(sqlite3 *db, cloudsync_payload_apply_callback_t callback);

bool cloudsync_config_exists (sqlite3 *db);
sqlite3_stmt *cloudsync_colvalue_stmt (sqlite3 *db, cloudsync_context *data, const char *tbl_name, bool *persistent);
char *cloudsync_pk_context_tbl (cloudsync_pk_decode_bind_context *ctx, int64_t *tbl_len);
void *cloudsync_pk_context_pk (cloudsync_pk_decode_bind_context *ctx, int64_t *pk_len);
char *cloudsync_pk_context_colname (cloudsync_pk_decode_bind_context *ctx, int64_t *colname_len);
int64_t cloudsync_pk_context_cl (cloudsync_pk_decode_bind_context *ctx);
int64_t cloudsync_pk_context_dbversion (cloudsync_pk_decode_bind_context *ctx);


#endif
