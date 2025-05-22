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

#define CLOUDSYNC_VERSION                       "0.7.1"
#define CLOUDSYNC_TOMBSTONE_VALUE               "__[RIP]__"
#define CLOUDSYNC_RLS_RESTRICTED_VALUE          "__[RLS]__"
#define CLOUDSYNC_DISABLE_ROWIDONLY_TABLES      1

typedef struct cloudsync_context cloudsync_context;

int sqlite3_cloudsync_init (sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi);
bool cloudsync_config_exists (sqlite3 *db);
const char *cloudsync_context_init (sqlite3 *db, cloudsync_context *data, sqlite3_context *context);
int cloudsync_merge_insert (sqlite3_vtab *vtab, int argc, sqlite3_value **argv, sqlite3_int64 *rowid);
void cloudsync_sync_key (cloudsync_context *data, const char *key, const char *value);
void cloudsync_sync_table_key (cloudsync_context *data, const char *table, const char *column, const char *key, const char *value);
void *cloudsync_get_auxdata (sqlite3_context *context);
void cloudsync_set_auxdata (sqlite3_context *context, void *xdata);
int cloudsync_payload_apply (sqlite3_context *context, const char *payload, int blen);

#endif
