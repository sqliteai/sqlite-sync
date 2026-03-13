//
//  dbutils.c
//  cloudsync
//
//  Created by Marco Bambini on 23/09/24.
//

#include <stdlib.h>
#include <inttypes.h>

#include "sql.h"
#include "utils.h"
#include "dbutils.h"
#include "cloudsync.h"

#if CLOUDSYNC_UNITTEST
char *OUT_OF_MEMORY_BUFFER = "OUT_OF_MEMORY_BUFFER";
#ifndef SQLITE_MAX_ALLOCATION_SIZE
#define SQLITE_MAX_ALLOCATION_SIZE  2147483391
#endif
#endif

// MARK: - Others -

// compares two SQLite values and returns an integer indicating the comparison result
int dbutils_value_compare (dbvalue_t *lvalue, dbvalue_t *rvalue) {
    if (lvalue == rvalue) return 0;
    if (!lvalue) return -1;
    if (!rvalue) return 1;
    
    int l_type = (lvalue) ? database_value_type(lvalue) : DBTYPE_NULL;
    int r_type = database_value_type(rvalue);
    
    // early exit if types differ, null is less than all types
    if (l_type != r_type) return (r_type - l_type);
    
    // at this point lvalue and rvalue are of the same type
    switch (l_type) {
        case DBTYPE_INTEGER: {
            int64_t l_int = database_value_int(lvalue);
            int64_t r_int = database_value_int(rvalue);
            return (l_int < r_int) ? -1 : (l_int > r_int);
        } break;
            
        case DBTYPE_FLOAT: {
            double l_double = database_value_double(lvalue);
            double r_double = database_value_double(rvalue);
            return (l_double < r_double) ? -1 : (l_double > r_double);
        } break;
            
        case DBTYPE_NULL:
            break;
            
        case DBTYPE_TEXT: {
            const char *l_text = database_value_text(lvalue);
            const char *r_text = database_value_text(rvalue);
            if (l_text == NULL && r_text == NULL) return 0;
            if (l_text == NULL && r_text != NULL) return -1;
            if (l_text != NULL && r_text == NULL) return 1;
            return strcmp((const char *)l_text, (const char *)r_text);
        } break;
            
        case DBTYPE_BLOB: {
            const void *l_blob = database_value_blob(lvalue);
            const void *r_blob = database_value_blob(rvalue);
            if (l_blob == NULL && r_blob == NULL) return 0;
            if (l_blob == NULL && r_blob != NULL) return -1;
            if (l_blob != NULL && r_blob == NULL) return 1;
            int l_size = database_value_bytes(lvalue);
            int r_size = database_value_bytes(rvalue);
            int cmp = memcmp(l_blob, r_blob, (l_size < r_size) ? l_size : r_size);
            return (cmp != 0) ? cmp : (l_size - r_size);
        } break;
    }
    
    return 0;
}

void dbutils_debug_value (dbvalue_t *value) {
    switch (database_value_type(value)) {
        case DBTYPE_INTEGER:
            printf("\t\tINTEGER: %" PRId64 "\n", database_value_int(value));
            break;
        case DBTYPE_FLOAT:
            printf("\t\tFLOAT: %f\n", database_value_double(value));
            break;
        case DBTYPE_TEXT:
            printf("\t\tTEXT: %s (%d)\n", database_value_text(value), database_value_bytes(value));
            break;
        case DBTYPE_BLOB:
            printf("\t\tBLOB: %p (%d)\n", (char *)database_value_blob(value), database_value_bytes(value));
            break;
        case DBTYPE_NULL:
            printf("\t\tNULL\n");
            break;
    }
}

void dbutils_debug_values (dbvalue_t **argv, int argc) {
    for (int i = 0; i < argc; i++) {
        dbutils_debug_value(argv[i]);
    }
}

// MARK: - Settings -

int dbutils_binary_comparison (int x, int y) {
    return (x == y) ? 0 : (x > y ? 1 : -1);
}

int dbutils_settings_get_value (cloudsync_context *data, const char *key, char *buffer, size_t *blen, int64_t *intvalue) {
    DEBUG_SETTINGS("dbutils_settings_get_value key: %s", key);
    
    // if intvalue requested: buffer/blen optional
    size_t buffer_len = 0;
    if (intvalue) {
        *intvalue = 0;
    } else {
        if (!buffer || !blen || *blen == 0) return DBRES_MISUSE;
        buffer[0] = 0;
        buffer_len = *blen;
        *blen = 0;
    }
    
    dbvm_t *vm = NULL;
    int rc = databasevm_prepare(data, SQL_SETTINGS_GET_VALUE, (void **)&vm, 0);
    if (rc != DBRES_OK) goto finalize_get_value;
    
    rc = databasevm_bind_text(vm, 1, key, -1);
    if (rc != DBRES_OK) goto finalize_get_value;
    
    rc = databasevm_step(vm);
    if (rc == DBRES_DONE) rc = DBRES_OK;
    else if (rc != DBRES_ROW) goto finalize_get_value;
    
    // SQLITE_ROW case
    if (rc == DBRES_ROW) {
        rc = DBRES_OK;
        
        // NULL case
        if (database_column_type(vm, 0) == DBTYPE_NULL) {
            goto finalize_get_value;
        }
        
        // INT case
        if (intvalue) {
            *intvalue = database_column_int(vm, 0);
            goto finalize_get_value;
        }
        
        // buffer case
        const char *value = database_column_text(vm, 0);
        size_t size = (size_t)database_column_bytes(vm, 0);
        if (!value || size == 0) goto finalize_get_value;
        if (size + 1 > buffer_len) {
            rc = DBRES_NOMEM;
        } else {
            memcpy(buffer, value, size);
            buffer[size] = '\0';
            *blen = size;
        }
    }
    
finalize_get_value:
    if (rc != DBRES_OK) {
        DEBUG_ALWAYS("dbutils_settings_get_value error %s", database_errmsg(data));
    }
    
    if (vm) databasevm_finalize(vm);
    return rc;
}

int dbutils_settings_set_key_value (cloudsync_context *data, const char *key, const char *value) {
    if (!key) return DBRES_MISUSE;
    DEBUG_SETTINGS("dbutils_settings_set_key_value key: %s value: %s", key, value);

    int rc = DBRES_OK;
    if (value) {
        const char *values[] = {key, value};
        DBTYPE types[] = {DBTYPE_TEXT, DBTYPE_TEXT};
        int lens[] = {-1, -1};
        rc = database_write(data, SQL_SETTINGS_SET_KEY_VALUE_REPLACE, values, types, lens, 2);
    } else {
        const char *values[] = {key};
        DBTYPE types[] = {DBTYPE_TEXT};
        int lens[] = {-1};
        rc = database_write(data, SQL_SETTINGS_SET_KEY_VALUE_DELETE, values, types, lens, 1);
    }

    if (rc == DBRES_OK && data) cloudsync_sync_key(data, key, value);
    return rc;
}

int dbutils_settings_get_int_value (cloudsync_context *data, const char *key) {
    DEBUG_SETTINGS("dbutils_settings_get_int_value key: %s", key);
    int64_t value = 0;
    if (dbutils_settings_get_value(data, key, NULL, NULL, &value) != DBRES_OK) return -1;
    
    return (int)value;
}

int64_t dbutils_settings_get_int64_value (cloudsync_context *data, const char *key) {
    DEBUG_SETTINGS("dbutils_settings_get_int_value key: %s", key);
    int64_t value = 0;
    if (dbutils_settings_get_value(data, key, NULL, NULL, &value) != DBRES_OK) return -1;
    
    return value;
}

int dbutils_settings_check_version (cloudsync_context *data, const char *version) {
    DEBUG_SETTINGS("dbutils_settings_check_version");
    char buffer[256];
    size_t len = sizeof(buffer);
    if (dbutils_settings_get_value(data, CLOUDSYNC_KEY_LIBVERSION, buffer, &len, NULL) != DBRES_OK) return -666;
    
    int major1, minor1, patch1;
    int major2, minor2, patch2;
    int count1 = sscanf(buffer, "%d.%d.%d", &major1, &minor1, &patch1);
    int count2 = sscanf((version == NULL ? CLOUDSYNC_VERSION : version), "%d.%d.%d", &major2, &minor2, &patch2);
    
    if (count1 != 3 || count2 != 3) return -666;
    
    int res = 0;
    if ((res = dbutils_binary_comparison(major1, major2)) == 0) {
        if ((res = dbutils_binary_comparison(minor1, minor2)) == 0) {
            return dbutils_binary_comparison(patch1, patch2);
        }
    }
    
    DEBUG_SETTINGS(" %s %s (%d)", buffer, CLOUDSYNC_VERSION, res);
    return res;
}

int dbutils_table_settings_get_value (cloudsync_context *data, const char *table, const char *column_name, const char *key, char *buffer, size_t blen) {
    DEBUG_SETTINGS("dbutils_table_settings_get_value table: %s column: %s key: %s", table, column_name, key);
        
    if (!buffer || blen == 0) return DBRES_MISUSE;
    buffer[0] = 0;
    
    dbvm_t *vm = NULL;
    int rc = databasevm_prepare(data, SQL_TABLE_SETTINGS_GET_VALUE, (void **)&vm, 0);
    if (rc != DBRES_OK) goto finalize_get_value;
    
    rc = databasevm_bind_text(vm, 1, table, -1);
    if (rc != DBRES_OK) goto finalize_get_value;
    
    rc = databasevm_bind_text(vm, 2, (column_name) ? column_name : "*", -1);
    if (rc != DBRES_OK) goto finalize_get_value;
    
    rc = databasevm_bind_text(vm, 3, key, -1);
    if (rc != DBRES_OK) goto finalize_get_value;
    
    rc = databasevm_step(vm);
    if (rc == DBRES_DONE) rc = DBRES_OK;
    else if (rc != DBRES_ROW) goto finalize_get_value;
    
    // SQLITE_ROW case
    if (rc == DBRES_ROW) {
        rc = DBRES_OK;

        // NULL case
        if (database_column_type(vm, 0) == DBTYPE_NULL) {
            goto finalize_get_value;
        }
    
        const char *value = database_column_text(vm, 0);
        size_t size = (size_t)database_column_bytes(vm, 0);
        if (size + 1 > blen) {
            rc = DBRES_NOMEM;
        } else {
            memcpy(buffer, value, size);
            buffer[size] = '\0';
        }
    }
    
finalize_get_value:   
    if (rc != DBRES_OK) {
        DEBUG_ALWAYS("cloudsync_table_settings error %s", database_errmsg(data));
    }
    if (vm) databasevm_finalize(vm); 
    return rc;
}

int dbutils_table_settings_set_key_value (cloudsync_context *data, const char *table_name, const char *column_name, const char *key, const char *value) {
    DEBUG_SETTINGS("dbutils_table_settings_set_key_value table: %s column: %s key: %s", table_name, column_name, key);
    
    int rc = DBRES_OK;
    
    // sanity check tbl_name
    if (table_name == NULL) {
        return cloudsync_set_error(data, "cloudsync_set_table/set_column requires a non-null table parameter", DBRES_ERROR);
    }
    
    // sanity check column name
    if (column_name == NULL) column_name = "*";
    
    // remove all table_name entries
    if (key == NULL) {
        const char *values[] = {table_name};
        DBTYPE types[] = {DBTYPE_TEXT};
        int lens[] = {-1};
        rc = database_write(data, SQL_TABLE_SETTINGS_DELETE_ALL_FOR_TABLE, values, types, lens, 1);
        return rc;
    }
    
    if (key && value) {
        const char *values[] = {table_name, column_name, key, value};
        DBTYPE types[] = {DBTYPE_TEXT, DBTYPE_TEXT, DBTYPE_TEXT, DBTYPE_TEXT};
        int lens[] = {-1, -1, -1, -1};
        rc = database_write(data, SQL_TABLE_SETTINGS_REPLACE, values, types, lens, 4);
    }
    
    if (value == NULL) {
        const char *values[] = {table_name, column_name, key};
        DBTYPE types[] = {DBTYPE_TEXT, DBTYPE_TEXT, DBTYPE_TEXT};
        int lens[] = {-1, -1, -1};
        rc = database_write(data, SQL_TABLE_SETTINGS_DELETE_ONE, values, types, lens, 3);
    }
    
    // unused in this version
    // cloudsync_context *data = (context) ? (cloudsync_context *)sqlite3_user_data(context) : NULL;
    // if (rc == DBRES_OK && data) cloudsync_sync_table_key(data, table, column, key, value);
    return rc;
}

int64_t dbutils_table_settings_count_tables (cloudsync_context *data) {
    DEBUG_SETTINGS("dbutils_table_settings_count_tables");
    
    int64_t count = 0;
    int rc = database_select_int(data, SQL_TABLE_SETTINGS_COUNT_TABLES, &count);
    return (rc == DBRES_OK) ? count : 0;
}

table_algo dbutils_table_settings_get_algo (cloudsync_context *data, const char *table_name) {
    DEBUG_SETTINGS("dbutils_table_settings_get_algo %s", table_name);
    
    char buffer[512];
    int rc = dbutils_table_settings_get_value(data, table_name, "*", "algo", buffer, sizeof(buffer));
    return (rc == DBRES_OK) ? cloudsync_algo_from_name(buffer) : table_algo_none;
}

int dbutils_settings_load_callback (void *xdata, int ncols, char **values, char **names) {
    cloudsync_context *data = (cloudsync_context *)xdata;

    for (int i=0; i+1<ncols; i+=2) {
        const char *key = values[i];
        const char *value = values[i+1];
        cloudsync_sync_key(data, key, value);
        DEBUG_SETTINGS("key: %s value: %s", key, value);
    }

    return 0;
}

int dbutils_settings_table_load_callback (void *xdata, int ncols, char **values, char **names) {
    cloudsync_context *data = (cloudsync_context *)xdata;

    for (int i=0; i+3<ncols; i+=4) {
        const char *table_name = values[i];
        const char *col_name = values[i+1];
        const char *key = values[i+2];
        const char *value = values[i+3];

        // Table-level algo setting (col_name == "*")
        if (strcmp(key, "algo") == 0 && col_name && strcmp(col_name, "*") == 0) {
            table_algo algo = cloudsync_algo_from_name(value);
            char fbuf[2048];
            int frc = dbutils_table_settings_get_value(data, table_name, "*", "filter", fbuf, sizeof(fbuf));
            const char *filt = (frc == DBRES_OK && fbuf[0]) ? fbuf : NULL;
            if (database_create_triggers(data, table_name, algo, filt) != DBRES_OK) return DBRES_MISUSE;
            if (table_add_to_context(data, algo, table_name) == false) return DBRES_MISUSE;
            DEBUG_SETTINGS("load tbl_name: %s value: %s", key, value);
            continue;
        }

        // Column-level algo=block setting (col_name != "*")
        if (strcmp(key, "algo") == 0 && value && strcmp(value, "block") == 0 &&
            col_name && strcmp(col_name, "*") != 0) {
            // Read optional delimiter
            char dbuf[256];
            int drc = dbutils_table_settings_get_value(data, table_name, col_name, "delimiter", dbuf, sizeof(dbuf));
            const char *delim = (drc == DBRES_OK && dbuf[0]) ? dbuf : NULL;
            cloudsync_setup_block_column(data, table_name, col_name, delim);
            DEBUG_SETTINGS("load block column: %s.%s delimiter: %s", table_name, col_name, delim ? delim : "(default)");
            continue;
        }
    }

    return 0;
}

bool dbutils_settings_migrate (cloudsync_context *data) {
    // dbutils_settings_check_version comparison failed
    // so check for logic migration here (if necessary)
    return true;
}

int dbutils_settings_load (cloudsync_context *data) {
    DEBUG_SETTINGS("dbutils_settings_load %p", data);
    
    // load global settings
    const char *sql = SQL_SETTINGS_LOAD_GLOBAL;
    int rc = database_exec_callback(data, sql, dbutils_settings_load_callback, data);
    if (rc != DBRES_OK) DEBUG_ALWAYS("cloudsync_load_settings error: %s", database_errmsg(data));
    
    // load table-specific settings
    sql = SQL_SETTINGS_LOAD_TABLE;
    rc = database_exec_callback(data, sql, dbutils_settings_table_load_callback, data);
    if (rc != DBRES_OK) DEBUG_ALWAYS("cloudsync_load_settings error: %s", database_errmsg(data));
    
    return DBRES_OK;
}

int dbutils_settings_init (cloudsync_context *data) {
    DEBUG_SETTINGS("dbutils_settings_init %p", data);
        
    // check if cloudsync_settings table exists
    int rc = DBRES_OK;
    bool settings_exists = database_internal_table_exists(data, CLOUDSYNC_SETTINGS_NAME);
    if (settings_exists == false) {
        DEBUG_SETTINGS("cloudsync_settings does not exist (creating a new one)");
        
        // create table and fill-in initial data
        rc = database_exec(data, SQL_CREATE_SETTINGS_TABLE);
        if (rc != DBRES_OK) return rc;
        
        // library version
        char *sql = cloudsync_memory_mprintf(SQL_INSERT_SETTINGS_STR_FORMAT, CLOUDSYNC_KEY_LIBVERSION, CLOUDSYNC_VERSION);
        if (!sql) return DBRES_NOMEM;
        rc = database_exec(data, sql);
        cloudsync_memory_free(sql);
        if (rc != DBRES_OK) return rc;

        // schema version
        char sql_int[1024];
        snprintf(sql_int, sizeof(sql_int), SQL_INSERT_SETTINGS_INT_FORMAT, CLOUDSYNC_KEY_SCHEMAVERSION, (long long)database_schema_version(data));
        rc = database_exec(data, sql_int);
        if (rc != DBRES_OK) return rc;
    }
    
    if (database_internal_table_exists(data, CLOUDSYNC_SITEID_NAME) == false) {
        DEBUG_SETTINGS("cloudsync_site_id does not exist (creating a new one)");
        
        // create table and fill-in initial data
        // site_id is implicitly indexed
        // the rowid column is the primary key
        rc = database_exec(data, SQL_CREATE_SITE_ID_TABLE);
        if (rc != DBRES_OK) return rc;
        
        // siteid (to uniquely identify this local copy of the database)
        uint8_t site_id[UUID_LEN];
        if (cloudsync_uuid_v7(site_id) == -1) return DBRES_ERROR;
        
        // rowid 0 means local site_id
        const char *values[] = {"0", (const char *)&site_id};
        DBTYPE types[] = {DBTYPE_INTEGER, DBTYPE_BLOB};
        int lens[] = {-1, UUID_LEN};
        rc = database_write(data, SQL_INSERT_SITE_ID_ROWID, values, types, lens, 2);
        if (rc != DBRES_OK) return rc;
    }
    
    // check if cloudsync_table_settings table exists
    if (database_internal_table_exists(data, CLOUDSYNC_TABLE_SETTINGS_NAME) == false) {
        DEBUG_SETTINGS("cloudsync_table_settings does not exist (creating a new one)");
        
        rc = database_exec(data, SQL_CREATE_TABLE_SETTINGS_TABLE);
        if (rc != DBRES_OK) return rc;
    }
    
    // check if cloudsync_settings table exists
    bool schema_versions_exists = database_internal_table_exists(data, CLOUDSYNC_SCHEMA_VERSIONS_NAME);
    if (schema_versions_exists == false) {
        DEBUG_SETTINGS("cloudsync_schema_versions does not exist (creating a new one)");
        
        // create table
        rc = database_exec(data, SQL_CREATE_SCHEMA_VERSIONS_TABLE);
        if (rc != DBRES_OK) return rc;
    }
    
    // cloudsync_settings table exists so load it
    dbutils_settings_load(data);
    
    // check if some process changed schema outside of the lib
    /*
    if ((settings_exists == true) && (data->schema_version != database_schema_version(data))) {
        // SOMEONE CHANGED SCHEMAs SO WE NEED TO RECHECK AUGMENTED TABLES and RELATED TRIGGERS
        assert(0);
    }
     */
    
    return DBRES_OK;
}

int dbutils_settings_cleanup (cloudsync_context *data) {
    return database_exec(data, SQL_SETTINGS_CLEANUP_DROP_ALL);
}
