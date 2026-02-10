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
#define CLOUDSYNC_KEY_SCHEMA                "schema"
#define CLOUDSYNC_KEY_DEBUG                 "debug"
#define CLOUDSYNC_KEY_ALGO                  "algo"
#define CLOUDSYNC_KEY_SKIP_SCHEMA_HASH_CHECK    "skip_schema_hash_check"

// settings
int dbutils_settings_init (cloudsync_context *data);
int dbutils_settings_cleanup (cloudsync_context *data);
int dbutils_settings_check_version (cloudsync_context *data, const char *version);
int dbutils_settings_set_key_value (cloudsync_context *data, const char *key, const char *value);
int dbutils_settings_get_int_value (cloudsync_context *data, const char *key);
int64_t dbutils_settings_get_int64_value (cloudsync_context *data, const char *key);

// table settings
int        dbutils_table_settings_set_key_value (cloudsync_context *data, const char *table_name, const char *column_name, const char *key, const char *value);
int64_t    dbutils_table_settings_count_tables (cloudsync_context *data);
int        dbutils_table_settings_get_value (cloudsync_context *data, const char *table_name, const char *column_name, const char *key, char *buffer, size_t blen);
table_algo dbutils_table_settings_get_algo (cloudsync_context *data, const char *table_name);

// others
void dbutils_debug_values (dbvalue_t **argv, int argc);
void dbutils_debug_value (dbvalue_t *value);
int  dbutils_value_compare (dbvalue_t *v1, dbvalue_t *v2);

#endif
