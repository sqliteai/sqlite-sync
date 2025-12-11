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

// CLOUDSYNC CONTEXT
typedef struct cloudsync_context cloudsync_context;

cloudsync_context *cloudsync_context_create (void);
const char *cloudsync_context_init (cloudsync_context *data, void *db, void *db_context);
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

void *cloudsync_db (cloudsync_context *data);
void *cloudsync_dbcontext (cloudsync_context *data);
void cloudsync_set_db (cloudsync_context *data, void *value);
void cloudsync_set_dbcontext (cloudsync_context *data, void *value);
const char *cloudsync_errmsg (cloudsync_context *data);

int cloudsync_commit_hook (void *ctx);
void cloudsync_rollback_hook (void *ctx);

// PAYLOAD
int cloudsync_payload_header_size (void);

//#ifdef CLOUDSYNC_DESKTOP_OS
int cloudsync_payload_save (cloudsync_context *data, const char *payload_path, int *blob_size);
//#endif

// END OK

// CLOUDSYNCTABLE CONTEXT
typedef struct cloudsync_table_context cloudsync_table_context;
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

#ifdef __cplusplus
}
#endif

#endif
