//
//  cloudsync.h
//  cloudsync
//
//  Created by Marco Bambini on 16/05/24.
//

#ifndef __CLOUDSYNC__
#define __CLOUDSYNC__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "database.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLOUDSYNC_VERSION                       "0.9.0"

// Opaque structures
typedef struct cloudsync_context cloudsync_context;
typedef struct cloudsync_payload_context cloudsync_payload_context;
typedef struct cloudsync_table_context cloudsync_table_context;

cloudsync_context *cloudsync_context_create (void *db);
const char *cloudsync_context_init (cloudsync_context *data, void *db);
void cloudsync_context_free (void *ctx);

// OK
int cloudsync_cleanup (cloudsync_context *data, const char *table_name);
int cloudsync_init_table (cloudsync_context *data, const char *table_name, const char *algo_name, bool skip_int_pk_check);

int cloudsync_terminate (cloudsync_context *data);
int cloudsync_insync (cloudsync_context *data);
int cloudsync_bumpseq (cloudsync_context *data);
void *cloudsync_siteid (cloudsync_context *data);
void cloudsync_reset_siteid (cloudsync_context *data);
db_int64 cloudsync_dbversion_next (cloudsync_context *data, db_int64 merging_version);
db_int64 cloudsync_dbversion (cloudsync_context *data);
void cloudsync_update_schema_hash (cloudsync_context *data);
int cloudsync_dbversion_check_uptodate (cloudsync_context *data);

int cloudsync_begin_alter (cloudsync_context *data, const char *table_name);
int cloudsync_commit_alter (cloudsync_context *data, const char *table_name);

void *cloudsync_db (cloudsync_context *data);
const char *cloudsync_errmsg (cloudsync_context *data);
void *cloudsync_auxdata (cloudsync_context *data);
void cloudsync_set_auxdata (cloudsync_context *data, void *xdata);

int cloudsync_commit_hook (void *ctx);
void cloudsync_rollback_hook (void *ctx);

// PAYLOAD

//#ifdef CLOUDSYNC_DESKTOP_OS
int cloudsync_payload_save (cloudsync_context *data, const char *payload_path, int *blob_size);
//#endif

int cloudsync_payload_apply (cloudsync_context *data, const char *payload, int blen, int *nrows);

// Payload context (used to encode changes)
int    cloudsync_payload_encode_step  (cloudsync_payload_context *payload, cloudsync_context *data, int argc, dbvalue_t **argv);
int    cloudsync_payload_encode_final (cloudsync_payload_context *payload, cloudsync_context *data);
char  *cloudsync_payload_blob (cloudsync_payload_context *payload, db_int64 *blob_size, db_int64 *nrows);
size_t cloudsync_payload_context_size (size_t *header_size);

// END OK

// CLOUDSYNCTABLE CONTEXT
cloudsync_table_context *table_lookup (cloudsync_context *data, const char *table_name);
void *table_column_lookup (cloudsync_table_context *table, const char *col_name, bool is_merge, int *index);
bool table_enabled (cloudsync_table_context *table);
void table_set_enabled (cloudsync_table_context *table, bool value);

bool table_pk_exists (cloudsync_table_context *table, const char *value, size_t len);
int table_count_cols (cloudsync_table_context *table);
int table_count_pks (cloudsync_table_context *table);
const char *table_colname (cloudsync_table_context *table, int index);

char **table_pknames (cloudsync_table_context *table);
void table_set_pknames (cloudsync_table_context *table, char **pknames);
bool table_algo_isgos (cloudsync_table_context *table);

int table_remove (cloudsync_context *data, cloudsync_table_context *table);
void table_free (cloudsync_table_context *table);

int local_mark_insert_sentinel_meta (cloudsync_table_context *table, const char *pk, size_t pklen, db_int64 db_version, int seq);
int local_update_sentinel (cloudsync_table_context *table, const char *pk, size_t pklen, db_int64 db_version, int seq);
int local_mark_insert_or_update_meta (cloudsync_table_context *table, const char *pk, size_t pklen, const char *col_name, db_int64 db_version, int seq);
int local_mark_delete_meta (cloudsync_table_context *table, const char *pk, size_t pklen, db_int64 db_version, int seq);
int local_drop_meta (cloudsync_table_context *table, const char *pk, size_t pklen);
int local_update_move_meta (cloudsync_table_context *table, const char *pk, size_t pklen, const char *pk2, size_t pklen2, db_int64 db_version);

#ifdef __cplusplus
}
#endif

#endif
