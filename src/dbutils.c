//
//  dbutils.c
//  cloudsync
//
//  Created by Marco Bambini on 23/09/24.
//

#include <stdlib.h>
#include "utils.h"
#include "dbutils.h"
#include "cloudsync.h"

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT3
#endif

#if CLOUDSYNC_UNITTEST
char *OUT_OF_MEMORY_BUFFER = "OUT_OF_MEMORY_BUFFER";
#ifndef SQLITE_MAX_ALLOCATION_SIZE
#define SQLITE_MAX_ALLOCATION_SIZE  2147483391
#endif
#endif

int dbutils_settings_check_version (sqlite3 *db, const char *version);
bool table_add_to_context (db_t *db, cloudsync_context *data, table_algo algo, const char *table_name);

// MARK: -

// compares two SQLite values and returns an integer indicating the comparison result
int dbutils_value_compare (sqlite3_value *lvalue, sqlite3_value *rvalue) {
    if (lvalue == rvalue) return 0;
    if (!lvalue) return -1;
    if (!rvalue) return 1;
    
    int l_type = (lvalue) ? database_value_type(lvalue) : SQLITE_NULL;
    int r_type = database_value_type(rvalue);
    
    // early exit if types differ, null is less than all types
    if (l_type != r_type) return (r_type - l_type);
    
    // at this point lvalue and rvalue are of the same type
    switch (l_type) {
        case SQLITE_INTEGER: {
            sqlite3_int64 l_int = database_value_int(lvalue);
            sqlite3_int64 r_int = database_value_int(rvalue);
            return (l_int < r_int) ? -1 : (l_int > r_int);
        } break;
            
        case SQLITE_FLOAT: {
            double l_double = database_value_double(lvalue);
            double r_double = database_value_double(rvalue);
            return (l_double < r_double) ? -1 : (l_double > r_double);
        } break;
            
        case SQLITE_NULL:
            break;
            
        case SQLITE_TEXT: {
            const char *l_text = database_value_text(lvalue);
            const char *r_text = database_value_text(rvalue);
            return strcmp((const char *)l_text, (const char *)r_text);
        } break;
            
        case SQLITE_BLOB: {
            const void *l_blob = database_value_blob(lvalue);
            const void *r_blob = database_value_blob(rvalue);
            int l_size = database_value_bytes(lvalue);
            int r_size = database_value_bytes(rvalue);
            int cmp = memcmp(l_blob, r_blob, (l_size < r_size) ? l_size : r_size);
            return (cmp != 0) ? cmp : (l_size - r_size);
        } break;
    }
    
    return 0;
}

void dbutils_set_error (sqlite3_context *context, const char *format, ...) {
    char buffer[4096];
    
    va_list arg;
    va_start (arg, format);
    vsnprintf(buffer, sizeof(buffer), format, arg);
    va_end (arg);
    
    if (context) sqlite3_result_error(context, buffer, -1);
}

// MARK: -

void dbutils_debug_value (sqlite3_value *value) {
    switch (database_value_type(value)) {
        case SQLITE_INTEGER:
            printf("\t\tINTEGER: %lld\n", database_value_int(value));
            break;
        case SQLITE_FLOAT:
            printf("\t\tFLOAT: %f\n", database_value_double(value));
            break;
        case SQLITE_TEXT:
            printf("\t\tTEXT: %s (%d)\n", database_value_text(value), database_value_bytes(value));
            break;
        case SQLITE_BLOB:
            printf("\t\tBLOB: %p (%d)\n", (char *)database_value_blob(value), database_value_bytes(value));
            break;
        case SQLITE_NULL:
            printf("\t\tNULL\n");
            break;
    }
}

void dbutils_debug_values (int argc, sqlite3_value **argv) {
    for (int i = 0; i < argc; i++) {
        dbutils_debug_value(argv[i]);
    }
}

int dbutils_debug_stmt (sqlite3 *db, bool print_result) {
    sqlite3_stmt *stmt = NULL;
    int counter = 0;
    while ((stmt = sqlite3_next_stmt(db, stmt))) {
        ++counter;
        if (print_result) printf("Unfinalized stmt statement: %p\n", stmt);
    }
    return counter;
}

// MARK: -

bool dbutils_system_exists (sqlite3 *db, const char *name, const char *type) {
    DEBUG_DBFUNCTION("dbutils_system_exists %s: %s", type, name);
    
    sqlite3_stmt *vm = NULL;
    bool result = false;
    
    char sql[1024];
    snprintf(sql, sizeof(sql), "SELECT EXISTS (SELECT 1 FROM sqlite_master WHERE type='%s' AND name=?1 COLLATE NOCASE);", type);
    int rc = database_prepare(db, sql, (void **)&vm, 0);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = databasevm_bind_text(vm, 1, name, -1);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = databasevm_step(vm);
    if (rc == SQLITE_ROW) {
        result = (bool)database_column_int(vm, 0);
        rc = SQLITE_OK;
    }
    
finalize:
    if (rc != SQLITE_OK) DEBUG_ALWAYS("Error executing %s in dbutils_system_exists for type %s name %s (%s).", sql, type, name, database_errmsg(db));
    if (vm) databasevm_finalize(vm);
    return result;
}

bool dbutils_table_exists (sqlite3 *db, const char *name) {
    return dbutils_system_exists(db, name, "table");
}

bool dbutils_trigger_exists (sqlite3 *db, const char *name) {
    return dbutils_system_exists(db, name, "trigger");
}

bool dbutils_table_sanity_check (sqlite3 *db, sqlite3_context *context, const char *name, bool skip_int_pk_check) {
    DEBUG_DBFUNCTION("dbutils_table_sanity_check %s", name);
    
    char buffer[2048];
    size_t blen = sizeof(buffer);
    
    // sanity check table name
    if (name == NULL) {
        dbutils_set_error(context, "%s", "cloudsync_init requires a non-null table parameter");
        return false;
    }
    
    // avoid allocating heap memory for SQL statements by setting a maximum length of 1900 characters
    // for table names. This limit is reasonable and helps prevent memory management issues.
    const size_t maxlen = blen - 148;
    if (strlen(name) > maxlen) {
        dbutils_set_error(context, "Table name cannot be longer than %d characters", maxlen);
        return false;
    }
    
    // check if table exists
    if (dbutils_table_exists(db, name) == false) {
        dbutils_set_error(context, "Table %s does not exist", name);
        return false;
    }
    
    // no more than 128 columns can be used as a composite primary key (SQLite hard limit)
    char *sql = sqlite3_snprintf((int)blen, buffer, "SELECT count(*) FROM pragma_table_info('%q') WHERE pk>0;", name);
    db_int64 count = 0;
    int rc = database_select_int(db, sql, &count);
    if (count > 128) {
        dbutils_set_error(context, "No more than 128 columns can be used to form a composite primary key");
        return false;
    } else if (rc != DBRES_OK) {
        dbutils_set_error(context, "%s", database_errmsg(db));
        return false;
    }
    
    #if CLOUDSYNC_DISABLE_ROWIDONLY_TABLES
    // if count == 0 means that rowid will be used as primary key (BTW: very bad choice for the user)
    if (count == 0) {
        dbutils_set_error(context, "Rowid only tables are not supported, all primary keys must be explicitly set and declared as NOT NULL (table %s)", name);
        return false;
    }
    #endif
        
    if (!skip_int_pk_check) {
        if (count == 1) {
            // the affinity of a column is determined by the declared type of the column,
            // according to the following rules in the order shown:
            // 1. If the declared type contains the string "INT" then it is assigned INTEGER affinity.
            sql = sqlite3_snprintf((int)blen, buffer, "SELECT count(*) FROM pragma_table_info('%q') WHERE pk=1 AND \"type\" LIKE '%%INT%%';", name);
            db_int64 count2 = 0;
            int rc = database_select_int(db, sql, &count2);
            if (rc != DBRES_OK) {
                dbutils_set_error(context, "%s", database_errmsg(db));
                return false;
            }
            if (count == count2) {
                dbutils_set_error(context, "Table %s uses an single-column INTEGER primary key. For CRDT replication, primary keys must be globally unique. Consider using a TEXT primary key with UUIDs or ULID to avoid conflicts across nodes. If you understand the risk and still want to use this INTEGER primary key, set the third argument of the cloudsync_init function to 1 to skip this check.", name);
                return false;
            }
            
        }
    }
        
    // if user declared explicit primary key(s) then make sure they are all declared as NOT NULL
    if (count > 0) {
        sql = sqlite3_snprintf((int)blen, buffer, "SELECT count(*) FROM pragma_table_info('%q') WHERE pk>0 AND \"notnull\"=1;", name);
        db_int64 count2 = 0;
        int rc = database_select_int(db, sql, &count2);
        if (rc != DBRES_OK) {
            dbutils_set_error(context, "%s", database_errmsg(db));
            return false;
        }
        if (count != count2) {
            dbutils_set_error(context, "All primary keys must be explicitly declared as NOT NULL (table %s)", name);
            return false;
        }
    }
    
    // check for columns declared as NOT NULL without a DEFAULT value.
    // Otherwise, col_merge_stmt would fail if changes to other columns are inserted first.
    sql = sqlite3_snprintf((int)blen, buffer, "SELECT count(*) FROM pragma_table_info('%q') WHERE pk=0 AND \"notnull\"=1 AND \"dflt_value\" IS NULL;", name);
    db_int64 count3 = 0;
    rc = database_select_int(db, sql, &count3);
    if (rc != DBRES_OK) {
        dbutils_set_error(context, "%s", database_errmsg(db));
        return false;
    }
    if (count3 > 0) {
        dbutils_set_error(context, "All non-primary key columns declared as NOT NULL must have a DEFAULT value. (table %s)", name);
        return false;
    }
    
    return true;
}

int dbutils_delete_triggers (sqlite3 *db, const char *table) {
    DEBUG_DBFUNCTION("dbutils_delete_triggers %s", table);
    
    // from dbutils_table_sanity_check we already know that 2048 is OK
    char buffer[2048];
    size_t blen = sizeof(buffer);
    int rc = SQLITE_ERROR;
    
    char *sql = sqlite3_snprintf((int)blen, buffer, "DROP TRIGGER IF EXISTS \"cloudsync_before_update_%w\";", table);
    rc = database_exec(db, sql);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_snprintf((int)blen, buffer, "DROP TRIGGER IF EXISTS \"cloudsync_before_delete_%w\";", table);
    rc = database_exec(db, sql);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_snprintf((int)blen, buffer, "DROP TRIGGER IF EXISTS \"cloudsync_after_insert_%w\";", table);
    rc = database_exec(db, sql);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_snprintf((int)blen, buffer, "DROP TRIGGER IF EXISTS \"cloudsync_after_update_%w\";", table);
    rc = database_exec(db, sql);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_snprintf((int)blen, buffer, "DROP TRIGGER IF EXISTS \"cloudsync_after_delete_%w\";", table);
    rc = database_exec(db, sql);
    if (rc != SQLITE_OK) goto finalize;
    
finalize:
    if (rc != SQLITE_OK) DEBUG_ALWAYS("dbutils_delete_triggers error %s (%s)", database_errmsg(db), sql);
    return rc;
}

int dbutils_check_triggers (sqlite3 *db, const char *table, table_algo algo) {
    DEBUG_DBFUNCTION("dbutils_check_triggers %s", table);
    
    if (dbutils_settings_check_version(db, "0.8.25") <= 0) {
        dbutils_delete_triggers(db, table);
    }
    
    char *trigger_name = NULL;
    int rc = SQLITE_NOMEM;
    
    // common part
    char *trigger_when = cloudsync_memory_mprintf("FOR EACH ROW WHEN cloudsync_is_sync('%q') = 0", table);
    if (!trigger_when) goto finalize;
    
    // INSERT TRIGGER
    // NEW.prikey1, NEW.prikey2...
    trigger_name = cloudsync_memory_mprintf("cloudsync_after_insert_%s", table);
    if (!trigger_name) goto finalize;
    
    if (!dbutils_trigger_exists(db, trigger_name)) {
        rc = SQLITE_NOMEM;
        char *sql = cloudsync_memory_mprintf("SELECT group_concat('NEW.\"' || format('%%w', name) || '\"', ',') FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk;", table);
        if (!sql) goto finalize;
        
        char *pkclause = NULL;
        rc = database_select_text(db, sql, &pkclause);
        if (rc != DBRES_OK) {
            if (pkclause) cloudsync_memory_free(pkclause);
            goto finalize;
        }
        char *pkvalues = (pkclause) ? pkclause : "NEW.rowid";
        cloudsync_memory_free(sql);
        
        sql = cloudsync_memory_mprintf("CREATE TRIGGER \"%w\" AFTER INSERT ON \"%w\" %s BEGIN SELECT cloudsync_insert('%q', %s); END", trigger_name, table, trigger_when, table, pkvalues);
        if (pkclause) cloudsync_memory_free(pkclause);
        if (!sql) goto finalize;
        
        rc = database_exec(db, sql);
        DEBUG_SQL("\n%s", sql);
        cloudsync_memory_free(sql);
        if (rc != SQLITE_OK) goto finalize;
    }
    cloudsync_memory_free(trigger_name);
    trigger_name = NULL;
    rc = SQLITE_NOMEM;

    if (algo != table_algo_crdt_gos) {
        rc = SQLITE_NOMEM;
        
        // UPDATE TRIGGER
        // NEW.prikey1, NEW.prikey2, OLD.prikey1, OLD.prikey2, NEW.col1, OLD.col1, NEW.col2, OLD.col2...
        trigger_name = cloudsync_memory_mprintf("cloudsync_after_update_%s", table);
        if (!trigger_name) goto finalize;
        
        if (!dbutils_trigger_exists(db, trigger_name)) {
            // Generate VALUES clause for all columns using a CTE to avoid compound SELECT limits
            // First, get all primary key columns in order
            char *pk_values_sql = cloudsync_memory_mprintf(
                "SELECT group_concat('('||quote('%q')||', NEW.\"' || format('%%w', name) || '\", OLD.\"' || format('%%w', name) || '\")', ', ') "
                "FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk;", 
                table, table);
            if (!pk_values_sql) goto finalize;
            
            char *pk_values_list = NULL;
            int rc = database_select_text(db, pk_values_sql, &pk_values_list);
            cloudsync_memory_free(pk_values_sql);
            if (rc != DBRES_OK) {
                cloudsync_memory_free(pk_values_list);
                goto finalize;
            }
            
            // Then get all regular columns in order
            char *col_values_sql = cloudsync_memory_mprintf(
                "SELECT group_concat('('||quote('%q')||', NEW.\"' || format('%%w', name) || '\", OLD.\"' || format('%%w', name) || '\")', ', ') "
                "FROM pragma_table_info('%q') WHERE pk=0 ORDER BY cid;", 
                table, table);
            if (!col_values_sql) goto finalize;
            
            char *col_values_list = NULL;
            rc = database_select_text(db, col_values_sql, &col_values_list);
            cloudsync_memory_free(col_values_sql);
            if (rc != DBRES_OK) {
                cloudsync_memory_free(pk_values_list);
                if (col_values_list) cloudsync_memory_free(col_values_list);
                goto finalize;
            }
            
            // Build the complete VALUES query
            char *values_query;
            if (col_values_list && strlen(col_values_list) > 0) {
                // Table has both primary keys and regular columns
                values_query = cloudsync_memory_mprintf(
                    "WITH column_data(table_name, new_value, old_value) AS (VALUES %s, %s) "
                    "SELECT table_name, new_value, old_value FROM column_data",
                    pk_values_list, col_values_list);
                cloudsync_memory_free(col_values_list);
            } else {
                // Table has only primary keys
                values_query = cloudsync_memory_mprintf(
                    "WITH column_data(table_name, new_value, old_value) AS (VALUES %s) "
                    "SELECT table_name, new_value, old_value FROM column_data",
                    pk_values_list);
            }
            
            if (pk_values_list) cloudsync_memory_free(pk_values_list);
            if (!values_query) goto finalize;
            
            // Create the trigger with aggregate function
            char *sql = cloudsync_memory_mprintf(
                "CREATE TRIGGER \"%w\" AFTER UPDATE ON \"%w\" %s BEGIN "
                "SELECT cloudsync_update(table_name, new_value, old_value) FROM (%s); "
                "END", 
                trigger_name, table, trigger_when, values_query);
            
            cloudsync_memory_free(values_query);
            if (!sql) goto finalize;
            
            rc = database_exec(db, sql);
            DEBUG_SQL("\n%s", sql);
            cloudsync_memory_free(sql);
            if (rc != SQLITE_OK) goto finalize;
        }
        cloudsync_memory_free(trigger_name);
        trigger_name = NULL;
    } else {
        // Grow Only Set
        // In a grow-only set, the update operation is not allowed.
        // A grow-only set is a type of CRDT (Conflict-free Replicated Data Type) where the only permissible operation is to add elements to the set,
        // without ever removing or modifying them.
        // Once an element is added to the set, it remains there permanently, which guarantees that the set only grows over time.
        trigger_name = cloudsync_memory_mprintf("cloudsync_before_update_%s", table);
        if (!trigger_name) goto finalize;
        
        if (!dbutils_trigger_exists(db, trigger_name)) {
            char *sql = cloudsync_memory_mprintf("CREATE TRIGGER \"%w\" BEFORE UPDATE ON \"%w\" FOR EACH ROW WHEN cloudsync_is_enabled('%q') = 1 BEGIN SELECT RAISE(ABORT, 'Error: UPDATE operation is not allowed on table %w.'); END", trigger_name, table, table, table);
            if (!sql) goto finalize;
            
            rc = database_exec(db, sql);
            DEBUG_SQL("\n%s", sql);
            cloudsync_memory_free(sql);
            if (rc != SQLITE_OK) goto finalize;
        }
        cloudsync_memory_free(trigger_name);
        trigger_name = NULL;
    }
    
    // DELETE TRIGGER
    // OLD.prikey1, OLD.prikey2...
    if (algo != table_algo_crdt_gos) {
        trigger_name = cloudsync_memory_mprintf("cloudsync_after_delete_%s", table);
        if (!trigger_name) goto finalize;
        
        if (!dbutils_trigger_exists(db, trigger_name)) {
            char *sql = cloudsync_memory_mprintf("SELECT group_concat('OLD.\"' || format('%%w', name) || '\"', ',') FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk;", table);
            if (!sql) goto finalize;
            
            char *pkclause = NULL;
            rc = database_select_text(db, sql, &pkclause);
            cloudsync_memory_free(sql);
            if (rc != DBRES_OK) {
                if (pkclause) cloudsync_memory_free(pkclause);
                goto finalize;
            }
            char *pkvalues = (pkclause) ? pkclause : "OLD.rowid";
            
            sql = cloudsync_memory_mprintf("CREATE TRIGGER \"%w\" AFTER DELETE ON \"%w\" %s BEGIN SELECT cloudsync_delete('%q',%s); END", trigger_name, table, trigger_when, table, pkvalues);
            if (pkclause) cloudsync_memory_free(pkclause);
            if (!sql) goto finalize;
            
            rc = database_exec(db, sql);
            DEBUG_SQL("\n%s", sql);
            cloudsync_memory_free(sql);
            if (rc != SQLITE_OK) goto finalize;
        }
        
        cloudsync_memory_free(trigger_name);
        trigger_name = NULL;
    } else {
        // Grow Only Set
        // In a grow-only set, the delete operation is not allowed.
        trigger_name = cloudsync_memory_mprintf("cloudsync_before_delete_%s", table);
        if (!trigger_name) goto finalize;
        
        if (!dbutils_trigger_exists(db, trigger_name)) {
            char *sql = cloudsync_memory_mprintf("CREATE TRIGGER \"%w\" BEFORE DELETE ON \"%w\" FOR EACH ROW WHEN cloudsync_is_enabled('%q') = 1 BEGIN SELECT RAISE(ABORT, 'Error: DELETE operation is not allowed on table %w.'); END", trigger_name, table, table, table);
            if (!sql) goto finalize;
            
            rc = database_exec(db, sql);
            DEBUG_SQL("\n%s", sql);
            cloudsync_memory_free(sql);
            if (rc != SQLITE_OK) goto finalize;
        }
        cloudsync_memory_free(trigger_name);
        trigger_name = NULL;
    }
    
    rc = SQLITE_OK;
    
finalize:
    if (trigger_name) cloudsync_memory_free(trigger_name);
    if (trigger_when) cloudsync_memory_free(trigger_when);
    if (rc != SQLITE_OK) DEBUG_ALWAYS("dbutils_create_triggers error %s (%d)", database_errmsg(db), rc);
    return rc;
}

int dbutils_check_metatable (sqlite3 *db, const char *table, table_algo algo) {
    DEBUG_DBFUNCTION("dbutils_check_metatable %s", table);
        
    // WITHOUT ROWID is available starting from SQLite version 3.8.2 (2013-12-06) and later
    char *sql = cloudsync_memory_mprintf("CREATE TABLE IF NOT EXISTS \"%w_cloudsync\" (pk BLOB NOT NULL, col_name TEXT NOT NULL, col_version INTEGER, db_version INTEGER, site_id INTEGER DEFAULT 0, seq INTEGER, PRIMARY KEY (pk, col_name)) WITHOUT ROWID; CREATE INDEX IF NOT EXISTS \"%w_cloudsync_db_idx\" ON \"%w_cloudsync\" (db_version);", table, table, table);
    if (!sql) return SQLITE_NOMEM;
    
    int rc = database_exec(db, sql);
    DEBUG_SQL("\n%s", sql);
    cloudsync_memory_free(sql);
    
    return rc;
}

// MARK: - Settings -

int dbutils_binary_comparison (int x, int y) {
    return (x == y) ? 0 : (x > y ? 1 : -1);
}

char *dbutils_settings_get_value (sqlite3 *db, const char *key, char *buffer, size_t blen) {
    DEBUG_SETTINGS("dbutils_settings_get_value key: %s", key);
    
    // check if heap allocation must be forced
    if (!buffer || blen == 0) blen = 0;
    size_t size = 0;
    
    sqlite3_stmt *vm = NULL;
    char *sql = "SELECT value FROM cloudsync_settings WHERE key=?1;";
    int rc = database_prepare(db, sql, (void **)&vm, 0);
    if (rc != SQLITE_OK) goto finalize_get_value;
    
    rc = databasevm_bind_text(vm, 1, key, -1);
    if (rc != SQLITE_OK) goto finalize_get_value;
    
    rc = databasevm_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    else if (rc != SQLITE_ROW) goto finalize_get_value;
    
    // SQLITE_ROW case
    if (database_column_type(vm, 0) == SQLITE_NULL) {
        rc = SQLITE_OK;
        goto finalize_get_value;
    }
    
    const char *value = database_column_text(vm, 0);
    #if CLOUDSYNC_UNITTEST
    size = (buffer == OUT_OF_MEMORY_BUFFER) ? (SQLITE_MAX_ALLOCATION_SIZE + 1) :(size_t)database_column_bytes(vm, 0);
    #else
    size = (size_t)database_column_bytes(vm, 0);
    #endif
    if (size + 1 > blen) {
        buffer = cloudsync_memory_alloc((sqlite3_uint64)(size + 1));
        if (!buffer) {
            rc = SQLITE_NOMEM;
            goto finalize_get_value;
        }
    }
    
    memcpy(buffer, value, size+1);
    rc = SQLITE_OK;
    
finalize_get_value:
    #if CLOUDSYNC_UNITTEST
    if ((rc == SQLITE_NOMEM) && (size == SQLITE_MAX_ALLOCATION_SIZE + 1)) rc = SQLITE_OK;
    #endif
    if (rc != SQLITE_OK) DEBUG_ALWAYS("dbutils_settings_get_value error %s", database_errmsg(db));
    if (vm) databasevm_finalize(vm);
    
    return buffer;
}

int dbutils_settings_set_key_value (sqlite3 *db, sqlite3_context *context, const char *key, const char *value) {
    DEBUG_SETTINGS("dbutils_settings_set_key_value key: %s value: %s", key, value);
    
    int rc = SQLITE_OK;
    if (db == NULL) db = sqlite3_context_db_handle(context);
    
    if (key && value) {
        char *sql = "REPLACE INTO cloudsync_settings (key, value) VALUES (?1, ?2);";
        const char *values[] = {key, value};
        DBTYPE types[] = {SQLITE_TEXT, SQLITE_TEXT};
        int lens[] = {-1, -1};
        rc = database_write(db, sql, values, types, lens, 2);
    }
    
    if (value == NULL) {
        char *sql = "DELETE FROM cloudsync_settings WHERE key = ?1;";
        const char *values[] = {key};
        DBTYPE types[] = {SQLITE_TEXT};
        int lens[] = {-1};
        rc = database_write(db, sql, values, types, lens, 1);
    }
    
    cloudsync_context *data = (context) ? (cloudsync_context *)sqlite3_user_data(context) : NULL;
    if (rc == SQLITE_OK && data) cloudsync_sync_key(data, key, value);
    return rc;
}

int dbutils_settings_get_int_value (sqlite3 *db, const char *key) {
    DEBUG_SETTINGS("dbutils_settings_get_int_value key: %s", key);
    char buffer[256] = {0};
    if (dbutils_settings_get_value(db, key, buffer, sizeof(buffer)) == NULL) return -1;
    
    return (int)strtol(buffer, NULL, 0);
}

int dbutils_settings_check_version (sqlite3 *db, const char *version) {
    DEBUG_SETTINGS("dbutils_settings_check_version");
    char buffer[256];
    if (dbutils_settings_get_value(db, CLOUDSYNC_KEY_LIBVERSION, buffer, sizeof(buffer)) == NULL) return -666;
    
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

char *dbutils_table_settings_get_value (sqlite3 *db, const char *table, const char *column, const char *key, char *buffer, size_t blen) {
    DEBUG_SETTINGS("dbutils_table_settings_get_value table: %s column: %s key: %s", table, column, key);
    
    // check if heap allocation must be forced
    if (!buffer || blen == 0) blen = 0;
    size_t size = 0;
    
    sqlite3_stmt *vm = NULL;
    char *sql = "SELECT value FROM cloudsync_table_settings WHERE (tbl_name=?1 AND col_name=?2 AND key=?3);";
    int rc = database_prepare(db, sql, (void **)&vm, 0);
    if (rc != SQLITE_OK) goto finalize_get_value;
    
    rc = databasevm_bind_text(vm, 1, table, -1);
    if (rc != SQLITE_OK) goto finalize_get_value;
    
    rc = databasevm_bind_text(vm, 2, (column) ? column : "*", -1);
    if (rc != SQLITE_OK) goto finalize_get_value;
    
    rc = databasevm_bind_text(vm, 3, key, -1);
    if (rc != SQLITE_OK) goto finalize_get_value;
    
    rc = databasevm_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    else if (rc != SQLITE_ROW) goto finalize_get_value;
    
    // SQLITE_ROW case
    if (database_column_type(vm, 0) == SQLITE_NULL) {
        rc = SQLITE_OK;
        goto finalize_get_value;
    }
    
    const char *value = database_column_text(vm, 0);
    #if CLOUDSYNC_UNITTEST
    size = (buffer == OUT_OF_MEMORY_BUFFER) ? (SQLITE_MAX_ALLOCATION_SIZE + 1) :(size_t)database_column_bytes(vm, 0);
    #else
    size = (size_t)database_column_bytes(vm, 0);
    #endif
    if (size + 1 > blen) {
        buffer = cloudsync_memory_alloc((sqlite3_uint64)(size + 1));
        if (!buffer) {
            rc = SQLITE_NOMEM;
            goto finalize_get_value;
        }
    }
    
    memcpy(buffer, value, size+1);
    rc = SQLITE_OK;
    
finalize_get_value:
    #if CLOUDSYNC_UNITTEST
    if ((rc == SQLITE_NOMEM) && (size == SQLITE_MAX_ALLOCATION_SIZE + 1)) rc = SQLITE_OK;
    #endif
    if (rc != SQLITE_OK) {
        DEBUG_ALWAYS("cloudsync_table_settings error %s", database_errmsg(db));
    }
    if (vm) databasevm_finalize(vm);
    
    return buffer;
}

int dbutils_table_settings_set_key_value (sqlite3 *db, sqlite3_context *context, const char *table, const char *column, const char *key, const char *value) {
    DEBUG_SETTINGS("dbutils_table_settings_set_key_value table: %s column: %s key: %s", table, column, key);
    
    int rc = SQLITE_OK;
    if (db == NULL) db = sqlite3_context_db_handle(context);
    
    // sanity check tbl_name
    if (table == NULL) {
        if (context) sqlite3_result_error(context, "cloudsync_set_table/set_column requires a non-null table parameter", -1);
        return SQLITE_ERROR;
    }
    
    // sanity check column name
    if (column == NULL) column = "*";
    
    // remove all table_name entries
    if (key == NULL) {
        char *sql = "DELETE FROM cloudsync_table_settings WHERE tbl_name=?1;";
        const char *values[] = {table};
        DBTYPE types[] = {SQLITE_TEXT};
        int lens[] = {-1};
        rc = database_write(db, sql, values, types, lens, 1);
        return rc;
    }
    
    if (key && value) {
        char *sql = "REPLACE INTO cloudsync_table_settings (tbl_name, col_name, key, value) VALUES (?1, ?2, ?3, ?4);";
        const char *values[] = {table, column, key, value};
        DBTYPE types[] = {SQLITE_TEXT, SQLITE_TEXT, SQLITE_TEXT, SQLITE_TEXT};
        int lens[] = {-1, -1, -1, -1};
        rc = database_write(db, sql, values, types, lens, 4);
    }
    
    if (value == NULL) {
        char *sql = "DELETE FROM cloudsync_table_settings WHERE (tbl_name=?1 AND col_name=?2 AND key=?3);";
        const char *values[] = {table, column, key};
        DBTYPE types[] = {SQLITE_TEXT, SQLITE_TEXT, SQLITE_TEXT};
        int lens[] = {-1, -1, -1};
        rc = database_write(db, sql, values, types, lens, 3);
    }
    
    // unused in this version
    // cloudsync_context *data = (context) ? (cloudsync_context *)sqlite3_user_data(context) : NULL;
    // if (rc == SQLITE_OK && data) cloudsync_sync_table_key(data, table, column, key, value);
    return rc;
}

db_int64 dbutils_table_settings_count_tables (sqlite3 *db) {
    DEBUG_SETTINGS("dbutils_table_settings_count_tables");
    db_int64 count = 0;
    int rc = database_select_int(db, "SELECT count(*) FROM cloudsync_table_settings WHERE key='algo';", &count);
    return (rc == DBRES_OK) ? count : 0;
}

table_algo dbutils_table_settings_get_algo (sqlite3 *db, const char *table_name) {
    DEBUG_SETTINGS("dbutils_table_settings_get_algo %s", table_name);
    
    char buffer[512];
    char *value = dbutils_table_settings_get_value(db, table_name, "*", "algo", buffer, sizeof(buffer));
    return (value) ? crdt_algo_from_name(value) : table_algo_none;
}

int dbutils_settings_load_callback (void *xdata, int ncols, char **values, char **names) {
    cloudsync_context *data = (cloudsync_context *)xdata;
    
    for (int i=0; i<ncols; i+=2) {
        const char *key = values[i];
        const char *value = values[i+1];
        cloudsync_sync_key(data, key, value);
        DEBUG_SETTINGS("key: %s value: %s", key, value);
    }
    
    return 0;
}

int dbutils_settings_table_load_callback (void *xdata, int ncols, char **values, char **names) {
    cloudsync_context *data = (cloudsync_context *)xdata;
    sqlite3 *db = cloudsync_db(data);

    for (int i=0; i<ncols; i+=4) {
        const char *table_name = values[i];
        // const char *col_name = values[i+1];
        const char *key = values[i+2];
        const char *value = values[i+3];
        if (strcmp(key, "algo")!=0) continue;
        
        if (dbutils_check_triggers(db, table_name, crdt_algo_from_name(value)) != SQLITE_OK) return SQLITE_MISUSE;
        if (table_add_to_context(db, data, crdt_algo_from_name(value), table_name) == false) return SQLITE_MISUSE;
        
        DEBUG_SETTINGS("load tbl_name: %s value: %s", key, value);
    }
    
    return 0;
}

bool dbutils_migrate (sqlite3 *db) {
    // dbutils_settings_check_version comparison failed
    // so check for logic migration here (if necessary)
    return true;
}

int dbutils_settings_load (sqlite3 *db, cloudsync_context *data) {
    DEBUG_SETTINGS("dbutils_settings_load %p", data);
    
    // load global settings
    const char *sql = "SELECT key, value FROM cloudsync_settings;";
    int rc = database_exec_callback(db, sql, dbutils_settings_load_callback, data);
    if (rc != SQLITE_OK) DEBUG_ALWAYS("cloudsync_load_settings error: %s", database_errmsg(db));
    
    // load table-specific settings
    sql = "SELECT lower(tbl_name), lower(col_name), key, value FROM cloudsync_table_settings ORDER BY tbl_name;";
    rc = database_exec_callback(db, sql, dbutils_settings_table_load_callback, data);
    if (rc != SQLITE_OK) DEBUG_ALWAYS("cloudsync_load_settings error: %s", database_errmsg(db));
    
    return SQLITE_OK;
}

int dbutils_settings_init (sqlite3 *db, void *cloudsync_data, sqlite3_context *context) {
    DEBUG_SETTINGS("dbutils_settings_init %p", context);
    
    cloudsync_context *data = (cloudsync_context *)cloudsync_data;
    if (!data) data = (cloudsync_context *)sqlite3_user_data(context);
    
    // check if cloudsync_settings table exists
    int rc = SQLITE_OK;
    bool settings_exists = dbutils_table_exists(db, CLOUDSYNC_SETTINGS_NAME);
    if (settings_exists == false) {
        DEBUG_SETTINGS("cloudsync_settings does not exist (creating a new one)");
        
        char sql[1024];
        
        // create table and fill-in initial data
        snprintf(sql, sizeof(sql), "CREATE TABLE IF NOT EXISTS cloudsync_settings (key TEXT PRIMARY KEY NOT NULL COLLATE NOCASE, value TEXT);");
        rc = database_exec(db, sql);
        if (rc != SQLITE_OK) {if (context) sqlite3_result_error(context, database_errmsg(db), -1); return rc;}
        
        // library version
        snprintf(sql, sizeof(sql), "INSERT INTO cloudsync_settings (key, value) VALUES ('%s', '%s');", CLOUDSYNC_KEY_LIBVERSION, CLOUDSYNC_VERSION);
        rc = database_exec(db, sql);
        if (rc != SQLITE_OK) {if (context) sqlite3_result_error(context, database_errmsg(db), -1); return rc;}
        
        // schema version
        snprintf(sql, sizeof(sql), "INSERT INTO cloudsync_settings (key, value) VALUES ('%s', %lld);", CLOUDSYNC_KEY_SCHEMAVERSION, (long long)database_schema_version(db));
        rc = database_exec(db, sql);
        if (rc != SQLITE_OK) {if (context) sqlite3_result_error(context, database_errmsg(db), -1); return rc;}
    }
    
    if (dbutils_table_exists(db, CLOUDSYNC_SITEID_NAME) == false) {
        DEBUG_SETTINGS("cloudsync_site_id does not exist (creating a new one)");
        
        // create table and fill-in initial data
        // site_id is implicitly indexed
        // the rowid column is the primary key
        char *sql = "CREATE TABLE IF NOT EXISTS cloudsync_site_id (site_id BLOB UNIQUE NOT NULL);";
        rc = database_exec(db, sql);
        if (rc != SQLITE_OK) {if (context) sqlite3_result_error(context, database_errmsg(db), -1); return rc;}
        
        // siteid (to uniquely identify this local copy of the database)
        uint8_t site_id[UUID_LEN];
        if (cloudsync_uuid_v7(site_id) == -1) {if (context) sqlite3_result_error(context, "Unable to create UUIDv7 site_id", -1); return SQLITE_ERROR;}
        
        // rowid 0 means local site_id
        sql = "INSERT INTO cloudsync_site_id (rowid, site_id) VALUES (?, ?);";
        const char *values[] = {"0", (const char *)&site_id};
        DBTYPE types[] = {SQLITE_INTEGER, SQLITE_BLOB};
        int lens[] = {-1, UUID_LEN};
        rc = database_write(db, sql, values, types, lens, 2);
        if (rc != SQLITE_OK) return rc;
    }
    
    // check if cloudsync_table_settings table exists
    if (dbutils_table_exists(db, CLOUDSYNC_TABLE_SETTINGS_NAME) == false) {
        DEBUG_SETTINGS("cloudsync_table_settings does not exist (creating a new one)");
        
        char *sql = "CREATE TABLE IF NOT EXISTS cloudsync_table_settings (tbl_name TEXT NOT NULL COLLATE NOCASE, col_name TEXT NOT NULL COLLATE NOCASE, key TEXT, value TEXT, PRIMARY KEY(tbl_name,key));";
        rc = database_exec(db, sql);
        if (rc != SQLITE_OK) {if (context) sqlite3_result_error(context, database_errmsg(db), -1); return rc;}
    }
    
    // check if cloudsync_settings table exists
    bool schema_versions_exists = dbutils_table_exists(db, CLOUDSYNC_SCHEMA_VERSIONS_NAME);
    if (schema_versions_exists == false) {
        DEBUG_SETTINGS("cloudsync_schema_versions does not exist (creating a new one)");
        
        // create table
        char *sql = "CREATE TABLE IF NOT EXISTS cloudsync_schema_versions (hash INTEGER PRIMARY KEY, seq INTEGER NOT NULL)";
        rc = database_exec(db, sql);
        if (rc != SQLITE_OK) {if (context) sqlite3_result_error(context, database_errmsg(db), -1); return rc;}
    }
    
    // cloudsync_settings table exists so load it
    dbutils_settings_load(db, data);
    
    // check if some process changed schema outside of the lib
    /*
    if ((settings_exists == true) && (data->schema_version != dbutils_schema_version(db))) {
        // SOMEONE CHANGED SCHEMAs SO WE NEED TO RECHECK AUGMENTED TABLES and RELATED TRIGGERS
        assert(0);
    }
     */
    
    return SQLITE_OK;
}

int dbutils_settings_cleanup (sqlite3 *db) {
    const char *sql = "DROP TABLE IF EXISTS cloudsync_settings; DROP TABLE IF EXISTS cloudsync_site_id; DROP TABLE IF EXISTS cloudsync_table_settings; DROP TABLE IF EXISTS cloudsync_schema_versions; ";
    return database_exec(db, sql);
}
