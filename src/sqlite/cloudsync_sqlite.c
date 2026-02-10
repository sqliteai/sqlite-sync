//
//  cloudsync_sqlite.c
//  cloudsync
//
//  Created by Marco Bambini on 05/12/25.
//

#include "cloudsync_sqlite.h"
#include "cloudsync_changes_sqlite.h"
#include "../pk.h"
#include "../cloudsync.h"
#include "../database.h"
#include "../dbutils.h"

#ifndef CLOUDSYNC_OMIT_NETWORK
#include "../network.h"
#endif

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT1
#endif

#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(X) (void)(X)
#endif

#ifdef _WIN32
#define APIEXPORT   __declspec(dllexport)
#else
#define APIEXPORT
#endif

typedef struct {
    sqlite3_context *context;
    int             index;
} cloudsync_pk_decode_context;

typedef struct {
    sqlite3_value   *table_name;
    sqlite3_value   **new_values;
    sqlite3_value   **old_values;
    int             count;
    int             capacity;
} cloudsync_update_payload;

void dbsync_set_error (sqlite3_context *context, const char *format, ...) {
    char buffer[2048];
    
    va_list arg;
    va_start (arg, format);
    vsnprintf(buffer, sizeof(buffer), format, arg);
    va_end (arg);
    
    if (context) sqlite3_result_error(context, buffer, -1);
}

// MARK: - Public -

void dbsync_version (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_version");
    UNUSED_PARAMETER(argc);
    UNUSED_PARAMETER(argv);
    sqlite3_result_text(context, CLOUDSYNC_VERSION, -1, SQLITE_STATIC);
}

void dbsync_siteid (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_siteid");
    UNUSED_PARAMETER(argc);
    UNUSED_PARAMETER(argv);
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    sqlite3_result_blob(context, cloudsync_siteid(data), UUID_LEN, SQLITE_STATIC);
}

void dbsync_db_version (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_db_version");
    UNUSED_PARAMETER(argc);
    UNUSED_PARAMETER(argv);
    
    // retrieve context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    int rc = cloudsync_dbversion_check_uptodate(data);
    if (rc != SQLITE_OK) {
        dbsync_set_error(context, "Unable to retrieve db_version (%s).", database_errmsg(data));
        return;
    }
    
    sqlite3_result_int64(context, cloudsync_dbversion(data));
}

void dbsync_db_version_next (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_db_version_next");
    
    // retrieve context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    sqlite3_int64 merging_version = (argc == 1) ? database_value_int(argv[0]) : CLOUDSYNC_VALUE_NOTSET;
    sqlite3_int64 value = cloudsync_dbversion_next(data, merging_version);
    if (value == -1) {
        dbsync_set_error(context, "Unable to retrieve next_db_version (%s).", database_errmsg(data));
        return;
    }
    
    sqlite3_result_int64(context, value);
}

void dbsync_seq (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_seq");
    
    // retrieve context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    sqlite3_result_int(context, cloudsync_bumpseq(data));
}

void dbsync_uuid (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_uuid");
    
    char value[UUID_STR_MAXLEN];
    char *uuid = cloudsync_uuid_v7_string(value, true);
    sqlite3_result_text(context, uuid, -1, SQLITE_TRANSIENT);
}

// MARK: -

void dbsync_set (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_set");
    
    // sanity check parameters
    const char *key = (const char *)database_value_text(argv[0]);
    const char *value = (const char *)database_value_text(argv[1]);
    
    // silently fails
    if (key == NULL) return;
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    dbutils_settings_set_key_value(data, key, value);
}

void dbsync_set_column (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_set_column");
    
    const char *tbl = (const char *)database_value_text(argv[0]);
    const char *col = (const char *)database_value_text(argv[1]);
    const char *key = (const char *)database_value_text(argv[2]);
    const char *value = (const char *)database_value_text(argv[3]);
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    dbutils_table_settings_set_key_value(data, tbl, col, key, value);
}

void dbsync_set_table (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_set_table");
    
    const char *tbl = (const char *)database_value_text(argv[0]);
    const char *key = (const char *)database_value_text(argv[1]);
    const char *value = (const char *)database_value_text(argv[2]);
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    dbutils_table_settings_set_key_value(data, tbl, "*", key, value);
}

void dbsync_set_schema (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("dbsync_set_schema");
    
    const char *schema = (const char *)database_value_text(argv[0]);
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    cloudsync_set_schema(data, schema);
}

void dbsync_schema (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("dbsync_schema");
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    const char *schema = cloudsync_schema(data);
    (schema) ? sqlite3_result_text(context, schema, -1, NULL) : sqlite3_result_null(context);
}

void dbsync_table_schema (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("dbsync_table_schema");
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    const char *table_name = (const char *)database_value_text(argv[0]);
    const char *schema = cloudsync_table_schema(data, table_name);
    (schema) ? sqlite3_result_text(context, schema, -1, NULL) : sqlite3_result_null(context);
}

void dbsync_is_sync (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_is_sync");
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    if (cloudsync_insync(data)) {
        sqlite3_result_int(context, 1);
        return;
    }
    
    const char *table_name = (const char *)database_value_text(argv[0]);
    cloudsync_table_context *table = table_lookup(data, table_name);
    sqlite3_result_int(context, (table) ? (table_enabled(table) == 0) : 0);
}

void dbsync_col_value (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // DEBUG_FUNCTION("cloudsync_col_value");
    
    // argv[0] -> table name
    // argv[1] -> column name
    // argv[2] -> encoded pk
    
    // retrieve column name
    const char *col_name = (const char *)database_value_text(argv[1]);
    if (!col_name) {
        dbsync_set_error(context, "Column name cannot be NULL");
        return;
    }
    
    // check for special tombstone value
    if (strcmp(col_name, CLOUDSYNC_TOMBSTONE_VALUE) == 0) {
        sqlite3_result_null(context);
        return;
    }
    
    // lookup table
    const char *table_name = (const char *)database_value_text(argv[0]);
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) {
        dbsync_set_error(context, "Unable to retrieve table name %s in clousdsync_colvalue.", table_name);
        return;
    }
    
    // extract the right col_value vm associated to the column name
    sqlite3_stmt *vm = table_column_lookup(table, col_name, false, NULL);
    if (!vm) {
        sqlite3_result_error(context, "Unable to retrieve column value precompiled statement in clousdsync_colvalue.", -1);
        return;
    }
    
    // bind primary key values
    int rc = pk_decode_prikey((char *)database_value_blob(argv[2]), (size_t)database_value_bytes(argv[2]), pk_decode_bind_callback, (void *)vm);
    if (rc < 0) goto cleanup;
    
    // execute vm
    rc = databasevm_step(vm);
    if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
        sqlite3_result_text(context, CLOUDSYNC_RLS_RESTRICTED_VALUE, -1, SQLITE_STATIC);
    } else if (rc == SQLITE_ROW) {
        // store value result
        rc = SQLITE_OK;
        sqlite3_result_value(context, database_column_value(vm, 0));
    }
    
cleanup:
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, database_errmsg(data), -1);
    }
    databasevm_reset(vm);
}

void dbsync_pk_encode (sqlite3_context *context, int argc, sqlite3_value **argv) {
    size_t bsize = 0;
    char *buffer = pk_encode_prikey((dbvalue_t **)argv, argc, NULL, &bsize);
    if (!buffer) {
        sqlite3_result_null(context);
        return;
    }
    sqlite3_result_blob(context, (const void *)buffer, (int)bsize, SQLITE_TRANSIENT);
    cloudsync_memory_free(buffer);
}

int dbsync_pk_decode_set_result_callback (void *xdata, int index, int type, int64_t ival, double dval, char *pval) {
    cloudsync_pk_decode_context *decode_context = (cloudsync_pk_decode_context *)xdata;
    // decode_context->index is 1 based
    // index is 0 based
    if (decode_context->index != index+1) return SQLITE_OK;
    
    int rc = 0;
    sqlite3_context *context = decode_context->context;
    switch (type) {
        case SQLITE_INTEGER:
            sqlite3_result_int64(context, ival);
            break;

        case SQLITE_FLOAT:
            sqlite3_result_double(context, dval);
            break;

        case SQLITE_NULL:
            sqlite3_result_null(context);
            break;

        case SQLITE_TEXT:
            sqlite3_result_text(context, pval, (int)ival, SQLITE_TRANSIENT);
            break;

        case SQLITE_BLOB:
            sqlite3_result_blob(context, pval, (int)ival, SQLITE_TRANSIENT);
            break;
    }
    
    return rc;
}


void dbsync_pk_decode (sqlite3_context *context, int argc, sqlite3_value **argv) {
    const char *pk = (const char *)database_value_blob(argv[0]);
    int pk_len = database_value_bytes(argv[0]);
    int i = (int)database_value_int(argv[1]);
    
    cloudsync_pk_decode_context xdata = {.context = context, .index = i};
    pk_decode_prikey((char *)pk, (size_t)pk_len, dbsync_pk_decode_set_result_callback, &xdata);
}

// MARK: -

void dbsync_insert (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_insert %s", database_value_text(argv[0]));
    // debug_values(argc-1, &argv[1]);
    
    // argv[0] is table name
    // argv[1]..[N] is primary key(s)
    
    // table_cloudsync
    // pk               -> encode(argc-1, &argv[1])
    // col_name         -> name
    // col_version      -> 0/1 +1
    // db_version       -> check
    // site_id          0
    // seq              -> sqlite_master
    
    // retrieve context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    // lookup table
    const char *table_name = (const char *)database_value_text(argv[0]);
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) {
        dbsync_set_error(context, "Unable to retrieve table name %s in cloudsync_insert.", table_name);
        return;
    }
    
    // encode the primary key values into a buffer
    char buffer[1024];
    size_t pklen = sizeof(buffer);
    char *pk = pk_encode_prikey((dbvalue_t **)&argv[1], table_count_pks(table), buffer, &pklen);
    if (!pk) {
        sqlite3_result_error(context, "Not enough memory to encode the primary key(s).", -1);
        return;
    }
    
    // compute the next database version for tracking changes
    int64_t db_version = cloudsync_dbversion_next(data, CLOUDSYNC_VALUE_NOTSET);
    
    // check if a row with the same primary key already exists
    // if so, this means the row might have been previously deleted (sentinel)
    bool pk_exists = table_pk_exists(table, pk, pklen);
    int rc = SQLITE_OK;
    
    if (table_count_cols(table) == 0) {
        // if there are no columns other than primary keys, insert a sentinel record
        rc = local_mark_insert_sentinel_meta(table, pk, pklen, db_version, cloudsync_bumpseq(data));
        if (rc != SQLITE_OK) goto cleanup;
    } else if (pk_exists){
        // if a row with the same primary key already exists, update the sentinel record
        rc = local_update_sentinel(table, pk, pklen, db_version, cloudsync_bumpseq(data));
        if (rc != SQLITE_OK) goto cleanup;
    }
    
    // process each non-primary key column for insert or update
    for (int i=0; i<table_count_cols(table); ++i) {
        // mark the column as inserted or updated in the metadata
        rc = local_mark_insert_or_update_meta(table, pk, pklen, table_colname(table, i), db_version, cloudsync_bumpseq(data));
        if (rc != SQLITE_OK) goto cleanup;
    }
    
cleanup:
    if (rc != SQLITE_OK) sqlite3_result_error(context, database_errmsg(data), -1);
    // free memory if the primary key was dynamically allocated
    if (pk != buffer) cloudsync_memory_free(pk);
}

void dbsync_delete (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_delete %s", database_value_text(argv[0]));
    // debug_values(argc-1, &argv[1]);
    
    // retrieve context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    // lookup table
    const char *table_name = (const char *)database_value_text(argv[0]);
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) {
        dbsync_set_error(context, "Unable to retrieve table name %s in cloudsync_delete.", table_name);
        return;
    }
    
    // compute the next database version for tracking changes
    int64_t db_version = cloudsync_dbversion_next(data, CLOUDSYNC_VALUE_NOTSET);
    int rc = SQLITE_OK;
    
    // encode the primary key values into a buffer
    char buffer[1024];
    size_t pklen = sizeof(buffer);
    char *pk = pk_encode_prikey((dbvalue_t **)&argv[1], table_count_pks(table), buffer, &pklen);
    if (!pk) {
        sqlite3_result_error(context, "Not enough memory to encode the primary key(s).", -1);
        return;
    }
    
    // mark the row as deleted by inserting a delete sentinel into the metadata
    rc = local_mark_delete_meta(table, pk, pklen, db_version, cloudsync_bumpseq(data));
    if (rc != SQLITE_OK) goto cleanup;
    
    // remove any metadata related to the old rows associated with this primary key
    rc = local_drop_meta(table, pk, pklen);
    if (rc != SQLITE_OK) goto cleanup;
    
cleanup:
    if (rc != SQLITE_OK) sqlite3_result_error(context, database_errmsg(data), -1);
    // free memory if the primary key was dynamically allocated
    if (pk != buffer) cloudsync_memory_free(pk);
}

// MARK: -

void dbsync_update_payload_free (cloudsync_update_payload *payload) {
    for (int i=0; i<payload->count; i++) {
        database_value_free(payload->new_values[i]);
        database_value_free(payload->old_values[i]);
    }
    cloudsync_memory_free(payload->new_values);
    cloudsync_memory_free(payload->old_values);
    database_value_free(payload->table_name);
    payload->new_values = NULL;
    payload->old_values = NULL;
    payload->table_name = NULL;
    payload->count = 0;
    payload->capacity = 0;
}

int dbsync_update_payload_append (cloudsync_update_payload *payload, sqlite3_value *v1, sqlite3_value *v2, sqlite3_value *v3) {
    if (payload->count >= payload->capacity) {
        int newcap = payload->capacity ? payload->capacity * 2 : 128;
        
        sqlite3_value **new_values_2 = (sqlite3_value **)cloudsync_memory_realloc(payload->new_values, newcap * sizeof(*new_values_2));
        if (!new_values_2) return SQLITE_NOMEM;
        payload->new_values = new_values_2;
        
        sqlite3_value **old_values_2 = (sqlite3_value **)cloudsync_memory_realloc(payload->old_values, newcap * sizeof(*old_values_2));
        if (!old_values_2) return SQLITE_NOMEM;
        payload->old_values = old_values_2;
        
        payload->capacity = newcap;
    }
    
    int index = payload->count;
    if (payload->table_name == NULL) payload->table_name = database_value_dup(v1);
    else if (dbutils_value_compare(payload->table_name, v1) != 0) return SQLITE_NOMEM;
    payload->new_values[index] = database_value_dup(v2);
    payload->old_values[index] = database_value_dup(v3);
    payload->count++;
    
    // sanity check memory allocations
    bool v1_can_be_null = (database_value_type(v1) == SQLITE_NULL);
    bool v2_can_be_null = (database_value_type(v2) == SQLITE_NULL);
    bool v3_can_be_null = (database_value_type(v3) == SQLITE_NULL);
    
    if ((payload->table_name == NULL) && (!v1_can_be_null)) return SQLITE_NOMEM;
    if ((payload->new_values[index] == NULL) && (!v2_can_be_null)) return SQLITE_NOMEM;
    if ((payload->old_values[index] == NULL) && (!v3_can_be_null)) return SQLITE_NOMEM;
    
    return SQLITE_OK;
}

void dbsync_update_step (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // argv[0] => table_name
    // argv[1] => new_column_value
    // argv[2] => old_column_value
    
    // allocate/get the update payload
    cloudsync_update_payload *payload = (cloudsync_update_payload *)sqlite3_aggregate_context(context, sizeof(cloudsync_update_payload));
    if (!payload) {sqlite3_result_error_nomem(context); return;}
    
    if (dbsync_update_payload_append(payload, argv[0], argv[1], argv[2]) != SQLITE_OK) {
        sqlite3_result_error_nomem(context);
    }
}

void dbsync_update_final (sqlite3_context *context) {
    cloudsync_update_payload *payload = (cloudsync_update_payload *)sqlite3_aggregate_context(context, sizeof(cloudsync_update_payload));
    if (!payload || payload->count == 0) return;
    
    // retrieve context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    // lookup table
    const char *table_name = (const char *)database_value_text(payload->table_name);
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) {
        dbsync_set_error(context, "Unable to retrieve table name %s in cloudsync_update.", table_name);
        return;
    }

    // compute the next database version for tracking changes
    int64_t db_version = cloudsync_dbversion_next(data, CLOUDSYNC_VALUE_NOTSET);
    int rc = SQLITE_OK;
    
    // Check if the primary key(s) have changed
    bool prikey_changed = false;
    for (int i=0; i<table_count_pks(table); ++i) {
        if (dbutils_value_compare(payload->old_values[i], payload->new_values[i]) != 0) {
            prikey_changed = true;
            break;
        }
    }

    // encode the NEW primary key values into a buffer (used later for indexing)
    char buffer[1024];
    char buffer2[1024];
    size_t pklen = sizeof(buffer);
    size_t oldpklen = sizeof(buffer2);
    char *oldpk = NULL;
    
    char *pk = pk_encode_prikey((dbvalue_t **)payload->new_values, table_count_pks(table), buffer, &pklen);
    if (!pk) {
        sqlite3_result_error(context, "Not enough memory to encode the primary key(s).", -1);
        return;
    }
    
    if (prikey_changed) {
        // if the primary key has changed, we need to handle the row differently:
        // 1. mark the old row (OLD primary key) as deleted
        // 2. create a new row (NEW primary key)
        
        // encode the OLD primary key into a buffer
        oldpk = pk_encode_prikey((dbvalue_t **)payload->old_values, table_count_pks(table), buffer2, &oldpklen);
        if (!oldpk) {
            if (pk != buffer) cloudsync_memory_free(pk);
            sqlite3_result_error(context, "Not enough memory to encode the primary key(s).", -1);
            return;
        }
        
        // mark the rows with the old primary key as deleted in the metadata (old row handling)
        rc = local_mark_delete_meta(table, oldpk, oldpklen, db_version, cloudsync_bumpseq(data));
        if (rc != SQLITE_OK) goto cleanup;
        
        // move non-sentinel metadata entries from OLD primary key to NEW primary key
        // handles the case where some metadata is retained across primary key change
        // see https://github.com/sqliteai/sqlite-sync/blob/main/docs/PriKey.md for more details
        rc = local_update_move_meta(table, pk, pklen, oldpk, oldpklen, db_version);
        if (rc != SQLITE_OK) goto cleanup;
        
        // mark a new sentinel row with the new primary key in the metadata
        rc = local_mark_insert_sentinel_meta(table, pk, pklen, db_version, cloudsync_bumpseq(data));
        if (rc != SQLITE_OK) goto cleanup;
        
        // free memory if the OLD primary key was dynamically allocated
        if (oldpk != buffer2) cloudsync_memory_free(oldpk);
        oldpk = NULL;
    }
    
    // compare NEW and OLD values (excluding primary keys) to handle column updates
    for (int i=0; i<table_count_cols(table); i++) {
        int col_index = table_count_pks(table) + i;  // Regular columns start after primary keys

        if (dbutils_value_compare(payload->old_values[col_index], payload->new_values[col_index]) != 0) {
            // if a column value has changed, mark it as updated in the metadata
            // columns are in cid order
            rc = local_mark_insert_or_update_meta(table, pk, pklen, table_colname(table, i), db_version, cloudsync_bumpseq(data));
            if (rc != SQLITE_OK) goto cleanup;
        }
    }
    
cleanup:
    if (rc != SQLITE_OK) sqlite3_result_error(context, database_errmsg(data), -1);
    if (pk != buffer) cloudsync_memory_free(pk);
    if (oldpk && (oldpk != buffer2)) cloudsync_memory_free(oldpk);
    
    dbsync_update_payload_free(payload);
}

// MARK: -

void dbsync_cleanup (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_cleanup");
    
    const char *table = (const char *)database_value_text(argv[0]);
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    int rc = cloudsync_cleanup(data, table);
    if (rc != DBRES_OK) {
        sqlite3_result_error(context, cloudsync_errmsg(data), -1);
        sqlite3_result_error_code(context, rc);
    }
}

void dbsync_enable_disable (sqlite3_context *context, const char *table_name, bool value) {
    DEBUG_FUNCTION("cloudsync_enable_disable");
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) return;
    
    table_set_enabled(table, value);
}

void dbsync_enable (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_enable");
    
    const char *table = (const char *)database_value_text(argv[0]);
    dbsync_enable_disable(context, table, true);
}

void dbsync_disable (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_disable");
    
    const char *table = (const char *)database_value_text(argv[0]);
    dbsync_enable_disable(context, table, false);
}

void dbsync_is_enabled (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_is_enabled");
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    const char *table_name = (const char *)database_value_text(argv[0]);
    cloudsync_table_context *table = table_lookup(data, table_name);
    
    int result = (table && table_enabled(table)) ? 1 : 0;
    sqlite3_result_int(context, result);
}

void dbsync_terminate (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_terminate");
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    int rc = cloudsync_terminate(data);
    sqlite3_result_int(context, rc);
}

// MARK: -

void dbsync_init (sqlite3_context *context, const char *table, const char *algo, bool skip_int_pk_check) {
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    int rc = database_begin_savepoint(data, "cloudsync_init");
    if (rc != SQLITE_OK) {
        dbsync_set_error(context, "Unable to create cloudsync_init savepoint. %s", database_errmsg(data));
        sqlite3_result_error_code(context, rc);
        return;
    }
    
    rc = cloudsync_init_table(data, table, algo, skip_int_pk_check);
    if (rc == SQLITE_OK) {
        rc = database_commit_savepoint(data, "cloudsync_init");
        if (rc != SQLITE_OK) {
            dbsync_set_error(context, "Unable to release cloudsync_init savepoint. %s", database_errmsg(data));
            sqlite3_result_error_code(context, rc);
        }
    } else {
        // in case of error, rollback transaction
        sqlite3_result_error(context, cloudsync_errmsg(data), -1);
        sqlite3_result_error_code(context, rc);
        database_rollback_savepoint(data, "cloudsync_init");
        return;
    }
    
    cloudsync_update_schema_hash(data);
    
    // returns site_id as TEXT
    char buffer[UUID_STR_MAXLEN];
    cloudsync_uuid_v7_stringify(cloudsync_siteid(data), buffer, false);
    sqlite3_result_text(context, buffer, -1, SQLITE_TRANSIENT);
}

void dbsync_init3 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_init2");
    
    const char *table = (const char *)database_value_text(argv[0]);
    const char *algo = (const char *)database_value_text(argv[1]);
    bool skip_int_pk_check = (bool)database_value_int(argv[2]);
    dbsync_init(context, table, algo, skip_int_pk_check);
}

void dbsync_init2 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_init2");
    
    const char *table = (const char *)database_value_text(argv[0]);
    const char *algo = (const char *)database_value_text(argv[1]);
    dbsync_init(context, table, algo, false);
}

void dbsync_init1 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_init1");
    
    const char *table = (const char *)database_value_text(argv[0]);
    dbsync_init(context, table, NULL, false);
}

// MARK: -

void dbsync_begin_alter (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("dbsync_begin_alter");
     
    //retrieve table argument
    const char *table_name = (const char *)database_value_text(argv[0]);
    
    // retrieve context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    int rc = cloudsync_begin_alter(data, table_name);
    if (rc != DBRES_OK) {
        sqlite3_result_error(context, cloudsync_errmsg(data), -1);
        sqlite3_result_error_code(context, rc);
    }
}

void dbsync_commit_alter (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_commit_alter");
    
    //retrieve table argument
    const char *table_name = (const char *)database_value_text(argv[0]);
    
    // retrieve context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    int rc = cloudsync_commit_alter(data, table_name);
    if (rc != DBRES_OK) {
        sqlite3_result_error(context, cloudsync_errmsg(data), -1);
        sqlite3_result_error_code(context, rc);
    }
}

// MARK: - Payload -

void dbsync_payload_encode_step (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // allocate/get the session context
    cloudsync_payload_context *payload = (cloudsync_payload_context *)sqlite3_aggregate_context(context, (int)cloudsync_payload_context_size(NULL));
    if (!payload) {
        sqlite3_result_error(context, "Not enough memory to allocate payload session context", -1);
        sqlite3_result_error_code(context, SQLITE_NOMEM);
        return;
    }
    
    // retrieve context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    int rc = cloudsync_payload_encode_step(payload, data, argc, (dbvalue_t **)argv);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, cloudsync_errmsg(data), -1);
        sqlite3_result_error_code(context, rc);
    }
}

void dbsync_payload_encode_final (sqlite3_context *context) {
    // get the session context
    cloudsync_payload_context *payload = (cloudsync_payload_context *)sqlite3_aggregate_context(context, (int)cloudsync_payload_context_size(NULL));
    if (!payload) {
        sqlite3_result_error(context, "Unable to extract payload session context", -1);
        sqlite3_result_error_code(context, SQLITE_NOMEM);
        return;
    }
    
    // retrieve context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    int rc = cloudsync_payload_encode_final(payload, data);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, cloudsync_errmsg(data), -1);
        sqlite3_result_error_code(context, rc);
        return;
    }
    
    // result is OK so get BLOB and returns it
    int64_t blob_size = 0;
    char *blob = cloudsync_payload_blob (payload, &blob_size, NULL);
    if (!blob) {
        sqlite3_result_null(context);
    } else {
        sqlite3_result_blob64(context, blob, blob_size, SQLITE_TRANSIENT);
        cloudsync_memory_free(blob);
    }
    
    // from: https://sqlite.org/c3ref/aggregate_context.html
    // SQLite automatically frees the memory allocated by sqlite3_aggregate_context() when the aggregate query concludes.
}

void dbsync_payload_decode (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("dbsync_payload_decode");
    //debug_values(argc, argv);
    
    // sanity check payload type
    if (database_value_type(argv[0]) != SQLITE_BLOB) {
        sqlite3_result_error(context, "Error on cloudsync_payload_decode: value must be a BLOB.", -1);
        sqlite3_result_error_code(context, SQLITE_MISUSE);
        return;
    }
    
    // sanity check payload size
    int blen = database_value_bytes(argv[0]);
    size_t header_size = 0;
    cloudsync_payload_context_size(&header_size);
    if (blen < (int)header_size) {
        sqlite3_result_error(context, "Error on cloudsync_payload_decode: invalid input size.", -1);
        sqlite3_result_error_code(context, SQLITE_MISUSE);
        return;
    }
    
    // obtain payload
    const char *payload = (const char *)database_value_blob(argv[0]);
    
    // apply changes
    int nrows = 0;
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    int rc = cloudsync_payload_apply(data, payload, blen, &nrows);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, cloudsync_errmsg(data), -1);
        sqlite3_result_error_code(context, rc);
        return;
    }
    
    // returns number of applied rows
    sqlite3_result_int(context, nrows);
}

#ifdef CLOUDSYNC_DESKTOP_OS
void dbsync_payload_save (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("dbsync_payload_save");
    
    // sanity check argument
    if (database_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "Unable to retrieve file path.", -1);
        return;
    }
    
    // retrieve full path to file
    const char *payload_path = (const char *)database_value_text(argv[0]);
    
    // retrieve global context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    int blob_size = 0;
    int rc = cloudsync_payload_save(data, payload_path, &blob_size);
    if (rc == SQLITE_OK) {
        // if OK then returns blob size
        sqlite3_result_int64(context, (sqlite3_int64)blob_size);
        return;
    }
    
    sqlite3_result_error(context, cloudsync_errmsg(data), -1);
    sqlite3_result_error_code(context, rc);
}

void dbsync_payload_load (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("dbsync_payload_load");
    
    // sanity check argument
    if (database_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "Unable to retrieve file path.", -1);
        return;
    }
    
    // retrieve full path to file
    const char *path = (const char *)database_value_text(argv[0]);
    
    int64_t payload_size = 0;
    char *payload = cloudsync_file_read(path, &payload_size);
    if (!payload) {
        if (payload_size < 0) {
            sqlite3_result_error(context, "Unable to read payload from file path.", -1);
            sqlite3_result_error_code(context, SQLITE_IOERR);
            return;
        }
        // no rows affected but no error either
        sqlite3_result_int(context, 0);
        return;
    }
    
    int nrows = 0;
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    int rc = cloudsync_payload_apply (data, payload, (int)payload_size, &nrows);
    if (payload) cloudsync_memory_free(payload);
    
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, cloudsync_errmsg(data), -1);
        sqlite3_result_error_code(context, rc);
        return;
    }
    
    // returns number of applied rows
    sqlite3_result_int(context, nrows);
}
#endif

// MARK: - Register -

int dbsync_register (sqlite3 *db, const char *name, void (*xfunc)(sqlite3_context*,int,sqlite3_value**), void (*xstep)(sqlite3_context*,int,sqlite3_value**), void (*xfinal)(sqlite3_context*), int nargs, char **pzErrMsg, void *ctx, void (*ctx_free)(void *)) {
    
    const int DEFAULT_FLAGS = SQLITE_UTF8 | SQLITE_INNOCUOUS | SQLITE_DETERMINISTIC;
    int rc = sqlite3_create_function_v2(db, name, nargs, DEFAULT_FLAGS, ctx, xfunc, xstep, xfinal, ctx_free);
    
    if (rc != SQLITE_OK) {
        if (pzErrMsg) *pzErrMsg = sqlite3_mprintf("Error creating function %s: %s", name, sqlite3_errmsg(db));
        return rc;
    }
    return SQLITE_OK;
}

int dbsync_register_function (sqlite3 *db, const char *name, void (*xfunc)(sqlite3_context*,int,sqlite3_value**), int nargs, char **pzErrMsg, void *ctx, void (*ctx_free)(void *)) {
    DEBUG_DBFUNCTION("dbsync_register_function %s", name);
    return dbsync_register(db, name, xfunc, NULL, NULL, nargs, pzErrMsg, ctx, ctx_free);
}

int dbsync_register_aggregate (sqlite3 *db, const char *name, void (*xstep)(sqlite3_context*,int,sqlite3_value**), void (*xfinal)(sqlite3_context*), int nargs, char **pzErrMsg, void *ctx, void (*ctx_free)(void *)) {
    DEBUG_DBFUNCTION("dbsync_register_aggregate %s", name);
    return dbsync_register(db, name, NULL, xstep, xfinal, nargs, pzErrMsg, ctx, ctx_free);
}

// MARK: - Row Filter -

void dbsync_set_filter (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_set_filter");

    const char *tbl = (const char *)database_value_text(argv[0]);
    const char *filter_expr = (const char *)database_value_text(argv[1]);
    if (!tbl || !filter_expr) {
        dbsync_set_error(context, "cloudsync_set_filter: table and filter expression required");
        return;
    }

    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);

    // Store filter in table settings
    dbutils_table_settings_set_key_value(data, tbl, "*", "filter", filter_expr);

    // Read current algo
    table_algo algo = dbutils_table_settings_get_algo(data, tbl);
    if (algo == table_algo_none) algo = table_algo_crdt_cls;

    // Drop and recreate triggers with the filter
    database_delete_triggers(data, tbl);
    int rc = database_create_triggers(data, tbl, algo, filter_expr);
    if (rc != DBRES_OK) {
        dbsync_set_error(context, "cloudsync_set_filter: error recreating triggers");
        sqlite3_result_error_code(context, rc);
        return;
    }

    sqlite3_result_int(context, 1);
}

void dbsync_clear_filter (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_clear_filter");

    const char *tbl = (const char *)database_value_text(argv[0]);
    if (!tbl) {
        dbsync_set_error(context, "cloudsync_clear_filter: table name required");
        return;
    }

    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);

    // Remove filter from table settings (set to NULL/empty)
    dbutils_table_settings_set_key_value(data, tbl, "*", "filter", NULL);

    // Read current algo
    table_algo algo = dbutils_table_settings_get_algo(data, tbl);
    if (algo == table_algo_none) algo = table_algo_crdt_cls;

    // Drop and recreate triggers without filter
    database_delete_triggers(data, tbl);
    int rc = database_create_triggers(data, tbl, algo, NULL);
    if (rc != DBRES_OK) {
        dbsync_set_error(context, "cloudsync_clear_filter: error recreating triggers");
        sqlite3_result_error_code(context, rc);
        return;
    }

    sqlite3_result_int(context, 1);
}

int dbsync_register_functions (sqlite3 *db, char **pzErrMsg) {
    int rc = SQLITE_OK;
    
    // there's no built-in way to verify if sqlite3_cloudsync_init has already been called
    // for this specific database connection, we use a workaround: we attempt to retrieve the
    // cloudsync_version and check for an error, an error indicates that initialization has not been performed
    if (sqlite3_exec(db, "SELECT cloudsync_version();", NULL, NULL, NULL) == SQLITE_OK) return SQLITE_OK;
    
    // init memory debugger (NOOP in production)
    cloudsync_memory_init(1);
    
    // init context
    void *ctx = cloudsync_context_create(db);
    if (!ctx) {
        if (pzErrMsg) *pzErrMsg = sqlite3_mprintf("Not enought memory to create a database context");
        return SQLITE_NOMEM;
    }
    
    // register functions
    
    // PUBLIC functions
    rc = dbsync_register_function(db, "cloudsync_version", dbsync_version, 0, pzErrMsg, ctx, cloudsync_context_free);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_init", dbsync_init1, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_init", dbsync_init2, 2, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_init", dbsync_init3, 3, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = dbsync_register_function(db, "cloudsync_enable", dbsync_enable, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_disable", dbsync_disable, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_is_enabled", dbsync_is_enabled, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_cleanup", dbsync_cleanup, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_terminate", dbsync_terminate, 0, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_set", dbsync_set, 2, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_set_table", dbsync_set_table, 3, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = dbsync_register_function(db, "cloudsync_set_filter", dbsync_set_filter, 2, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = dbsync_register_function(db, "cloudsync_clear_filter", dbsync_clear_filter, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = dbsync_register_function(db, "cloudsync_set_schema", dbsync_set_schema, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_schema", dbsync_schema, 0, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_table_schema", dbsync_table_schema, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = dbsync_register_function(db, "cloudsync_set_column", dbsync_set_column, 4, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_siteid", dbsync_siteid, 0, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_db_version", dbsync_db_version, 0, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_db_version_next", dbsync_db_version_next, 0, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_db_version_next", dbsync_db_version_next, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_begin_alter", dbsync_begin_alter, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_commit_alter", dbsync_commit_alter, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_uuid", dbsync_uuid, 0, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    // PAYLOAD
    rc = dbsync_register_aggregate(db, "cloudsync_payload_encode", dbsync_payload_encode_step, dbsync_payload_encode_final, -1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    // alias
    rc = dbsync_register_function(db, "cloudsync_payload_decode", dbsync_payload_decode, -1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    rc = dbsync_register_function(db, "cloudsync_payload_apply", dbsync_payload_decode, -1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    #ifdef CLOUDSYNC_DESKTOP_OS
    rc = dbsync_register_function(db, "cloudsync_payload_save", dbsync_payload_save, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_payload_load", dbsync_payload_load, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    #endif
    
    // PRIVATE functions
    rc = dbsync_register_function(db, "cloudsync_is_sync", dbsync_is_sync, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_insert", dbsync_insert, -1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_aggregate(db, "cloudsync_update", dbsync_update_step, dbsync_update_final, 3, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_delete", dbsync_delete, -1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_col_value", dbsync_col_value, 3, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_pk_encode", dbsync_pk_encode, -1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_pk_decode", dbsync_pk_decode, 2, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbsync_register_function(db, "cloudsync_seq", dbsync_seq, 0, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;

    // NETWORK LAYER
    #ifndef CLOUDSYNC_OMIT_NETWORK
    rc = cloudsync_network_register(db, pzErrMsg, ctx);
    if (rc != SQLITE_OK) return rc;
    #endif
    
    cloudsync_context *data = (cloudsync_context *)ctx;
    sqlite3_commit_hook(db, cloudsync_commit_hook, ctx);
    sqlite3_rollback_hook(db, cloudsync_rollback_hook, ctx);
    
    // register eponymous only changes virtual table
    rc = cloudsync_vtab_register_changes (db, data);
    if (rc != SQLITE_OK) {
        if (pzErrMsg) *pzErrMsg = sqlite3_mprintf("Error creating changes virtual table: %s", sqlite3_errmsg(db));
        return rc;
    }
    
    // load config, if exists
    if (cloudsync_config_exists(data)) {
        if (cloudsync_context_init(data) == NULL) {
            cloudsync_context_free(data);
            if (pzErrMsg) *pzErrMsg = sqlite3_mprintf("An error occurred while trying to initialize context");
            return SQLITE_ERROR;
        }
        
        // make sure to update internal version to current version
        dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_LIBVERSION, CLOUDSYNC_VERSION);
    }
    
    return SQLITE_OK;
}

// MARK: - Main Entrypoint -

APIEXPORT int sqlite3_cloudsync_init (sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
    DEBUG_FUNCTION("sqlite3_cloudsync_init");
    
    #ifndef SQLITE_CORE
    SQLITE_EXTENSION_INIT2(pApi);
    #endif
    
    return dbsync_register_functions(db, pzErrMsg);
}
