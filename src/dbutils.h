//
//  dbutils.h
//  cloudsync
//
//  Created by Marco Bambini on 23/09/24.
//

#ifndef __CLOUDSYNC_DBUTILS__
#define __CLOUDSYNC_DBUTILS__

#include <stdbool.h>
#include "utils.h"
#include "cloudsync_private.h"

#define CLOUDSYNC_SETTINGS_NAME             "cloudsync_settings"
#define CLOUDSYNC_SITEID_NAME               "cloudsync_site_id"
#define CLOUDSYNC_TABLE_SETTINGS_NAME       "cloudsync_table_settings"
#define CLOUDSYNC_SCHEMA_VERSIONS_NAME      "cloudsync_schema_versions"

#define CLOUDSYNC_KEY_LIBVERSION            "version"
#define CLOUDSYNC_KEY_SCHEMAVERSION         "schemaversion"
#define CLOUDSYNC_KEY_CHECK_DBVERSION       "check_dbversion"
#define CLOUDSYNC_KEY_CHECK_SEQ             "check_seq"
#define CLOUDSYNC_KEY_SEND_DBVERSION        "send_dbversion"
#define CLOUDSYNC_KEY_SEND_SEQ              "send_seq"
#define CLOUDSYNC_KEY_DEBUG                 "debug"
#define CLOUDSYNC_KEY_ALGO                  "algo"

// general
int dbutils_write_simple (sqlite3 *db, const char *sql);
int dbutils_write (sqlite3 *db, sqlite3_context *context, const char *sql, const char **values, int types[], int len[], int count);
sqlite3_int64 dbutils_int_select (sqlite3 *db, const char *sql);
char *dbutils_text_select (sqlite3 *db, const char *sql);
char *dbutils_blob_select (sqlite3 *db, const char *sql, int *size, sqlite3_context *context, int *rc);
int dbutils_blob_int_int_select (sqlite3 *db, const char *sql, char **blob, int *size, sqlite3_int64 *int1, sqlite3_int64 *int2);

int dbutils_register_function (sqlite3 *db, const char *name, void (*ptr)(sqlite3_context*,int,sqlite3_value**), int nargs, char **pzErrMsg, void *ctx, void (*ctx_free)(void *));
int dbutils_register_aggregate (sqlite3 *db, const char *name, void (*xstep)(sqlite3_context*,int,sqlite3_value**), void (*xfinal)(sqlite3_context*), int nargs, char **pzErrMsg, void *ctx, void (*ctx_free)(void *));

int dbutils_debug_stmt (sqlite3 *db, bool print_result);
void dbutils_debug_values (int argc, sqlite3_value **argv);
void dbutils_debug_value (sqlite3_value *value);

int dbutils_value_compare (sqlite3_value *v1, sqlite3_value *v2);
void dbutils_context_result_error (sqlite3_context *context, const char *format, ...);

bool dbutils_system_exists (sqlite3 *db, const char *name, const char *type);
bool dbutils_table_exists (sqlite3 *db, const char *name);
bool dbutils_trigger_exists (sqlite3 *db, const char *name);
bool dbutils_table_sanity_check (sqlite3 *db, sqlite3_context *context, const char *name, bool skip_int_pk_check);
bool dbutils_is_star_table (const char *table_name);

int dbutils_delete_triggers (sqlite3 *db, const char *table);
int dbutils_check_triggers (sqlite3 *db, const char *table, table_algo algo);
int dbutils_check_metatable (sqlite3 *db, const char *table, table_algo algo);
sqlite3_int64 dbutils_schema_version (sqlite3 *db);

// settings
int dbutils_settings_cleanup (sqlite3 *db);
int dbutils_settings_init (sqlite3 *db, void *cloudsync_data, sqlite3_context *context);
int dbutils_settings_set_key_value (sqlite3 *db, sqlite3_context *context, const char *key, const char *value);
int dbutils_settings_get_int_value (sqlite3 *db, const char *key);
char *dbutils_settings_get_value (sqlite3 *db, const char *key, char *buffer, size_t blen);
int dbutils_table_settings_set_key_value (sqlite3 *db, sqlite3_context *context, const char *table, const char *column, const char *key, const char *value);
sqlite3_int64 dbutils_table_settings_count_tables (sqlite3 *db);
char *dbutils_table_settings_get_value (sqlite3 *db, const char *table, const char *column, const char *key, char *buffer, size_t blen);
table_algo dbutils_table_settings_get_algo (sqlite3 *db, const char *table_name);
int dbutils_update_schema_hash(sqlite3 *db, uint64_t *hash);
sqlite3_uint64 dbutils_schema_hash (sqlite3 *db);
bool dbutils_check_schema_hash (sqlite3 *db, sqlite3_uint64 hash);

#endif
