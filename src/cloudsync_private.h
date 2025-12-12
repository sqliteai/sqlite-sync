//
//  cloudsync_private.h
//  cloudsync
//
//  Created by Marco Bambini on 30/05/25.
//

#ifndef __CLOUDSYNC_PRIVATE__
#define __CLOUDSYNC_PRIVATE__

#include <stdbool.h>
#include "cloudsync.h"

#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif

#define CLOUDSYNC_VALUE_NOTSET                  -1
#define CLOUDSYNC_TOMBSTONE_VALUE               "__[RIP]__"
#define CLOUDSYNC_RLS_RESTRICTED_VALUE          "__[RLS]__"
#define CLOUDSYNC_DISABLE_ROWIDONLY_TABLES      1
#define CLOUDSYNC_DEFAULT_ALGO                  "cls"

typedef enum {
    CLOUDSYNC_PAYLOAD_APPLY_WILL_APPLY   = 1,
    CLOUDSYNC_PAYLOAD_APPLY_DID_APPLY    = 2,
    CLOUDSYNC_PAYLOAD_APPLY_CLEANUP      = 3
} CLOUDSYNC_PAYLOAD_APPLY_STEPS;


// used by vtab.c
int merge_insert_col (cloudsync_context *data, cloudsync_table_context *table, const char *pk, int pklen, const char *col_name, dbvalue_t *col_value, db_int64 col_version, db_int64 db_version, const char *site_id, int site_len, db_int64 seq, db_int64 *rowid);

int merge_insert (cloudsync_context *data, cloudsync_table_context *table, const char *insert_pk, int insert_pk_len, db_int64 insert_cl, const char *insert_name, dbvalue_t *insert_value, db_int64 insert_col_version, db_int64 insert_db_version, const char *insert_site_id, int insert_site_id_len, db_int64 insert_seq, db_int64 *rowid);

typedef struct cloudsync_pk_decode_bind_context cloudsync_pk_decode_bind_context;

void cloudsync_sync_key (cloudsync_context *data, const char *key, const char *value);

// used by network layer
int cloudsync_payload_get (cloudsync_context *data, char **blob, int *blob_size, int *db_version, int *seq, db_int64 *new_db_version, db_int64 *new_seq);

// used by core
typedef bool (*cloudsync_payload_apply_callback_t)(void **xdata, cloudsync_pk_decode_bind_context *decoded_change, sqlite3 *db, cloudsync_context *data, int step, int rc);
void cloudsync_set_payload_apply_callback(db_t *db, cloudsync_payload_apply_callback_t callback);

bool cloudsync_config_exists (db_t *db);
dbvm_t *cloudsync_colvalue_stmt (db_t *db, cloudsync_context *data, const char *tbl_name, bool *persistent);
char *cloudsync_pk_context_tbl (cloudsync_pk_decode_bind_context *ctx, int64_t *tbl_len);
void *cloudsync_pk_context_pk (cloudsync_pk_decode_bind_context *ctx, int64_t *pk_len);
char *cloudsync_pk_context_colname (cloudsync_pk_decode_bind_context *ctx, int64_t *colname_len);
int64_t cloudsync_pk_context_cl (cloudsync_pk_decode_bind_context *ctx);
int64_t cloudsync_pk_context_dbversion (cloudsync_pk_decode_bind_context *ctx);


#endif
