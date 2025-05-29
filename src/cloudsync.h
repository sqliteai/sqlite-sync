//
//  cloudsync.h
//  cloudsync
//
//  Created by Marco Bambini on 16/05/24.
//

#ifndef __CLOUDSYNC__
#define __CLOUDSYNC__

#include <stdint.h>
#include <stdbool.h>
#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif

#define CLOUDSYNC_VERSION                       "0.7.2"
#define CLOUDSYNC_TOMBSTONE_VALUE               "__[RIP]__"
#define CLOUDSYNC_RLS_RESTRICTED_VALUE          "__[RLS]__"
#define CLOUDSYNC_DISABLE_ROWIDONLY_TABLES      1

typedef enum {
    CLOUDSYNC_PAYLOAD_APPLY_WILL_APPLY   = 1,
    CLOUDSYNC_PAYLOAD_APPLY_DID_APPLY    = 2,
    CLOUDSYNC_PAYLOAD_APPLY_CLEANUP      = 3
} CLOUDSYNC_PAYLOAD_APPLY_STEPS;

typedef struct {
    sqlite3_stmt *vm;
    char         *tbl;
    int64_t      tbl_len;
    const void   *pk;
    int64_t      pk_len;
    char         *col_name;
    int64_t      col_name_len;
    int64_t      col_version;
    int64_t      db_version;
    const void   *site_id;
    int64_t      site_id_len;
    int64_t      cl;
    int64_t      seq;
} cloudsync_pk_decode_bind_context;

typedef struct cloudsync_context cloudsync_context;
typedef bool (*cloudsync_payload_apply_callback_t)(void **xdata, cloudsync_pk_decode_bind_context *decoded_change, sqlite3 *db, cloudsync_context *data, int step, int rc);

int sqlite3_cloudsync_init (sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi);
bool cloudsync_config_exists (sqlite3 *db);
const char *cloudsync_context_init (sqlite3 *db, cloudsync_context *data, sqlite3_context *context);
int cloudsync_merge_insert (sqlite3_vtab *vtab, int argc, sqlite3_value **argv, sqlite3_int64 *rowid);
void cloudsync_sync_key (cloudsync_context *data, const char *key, const char *value);
void cloudsync_sync_table_key (cloudsync_context *data, const char *table, const char *column, const char *key, const char *value);
void *cloudsync_get_auxdata (sqlite3_context *context);
void cloudsync_set_auxdata (sqlite3_context *context, void *xdata);
int cloudsync_payload_apply (sqlite3_context *context, const char *payload, int blen);
void cloudsync_set_payload_apply_callback(sqlite3 *db, cloudsync_payload_apply_callback_t callback);
sqlite3_stmt *cloudsync_col_value_stmt (sqlite3 *db, cloudsync_context *data, const char *tbl_name, bool *persistent);

#endif
