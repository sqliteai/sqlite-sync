//
//  database_sqlite.c
//  cloudsync
//
//  Created by Marco Bambini on 03/12/25.
//

#include "../cloudsync.h"
#include "../database.h"
#include "../dbutils.h"
#include "../utils.h"
#include "../sql.h"

#include <inttypes.h>
#include <string.h>
#include <stdlib.h>

#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT3
#endif

// MARK: - SQL -

char *sql_build_drop_table (const char *table_name, char *buffer, int bsize, bool is_meta) {
    char *sql = NULL;
    
    if (is_meta) {
        sql = sqlite3_snprintf(bsize, buffer, "DROP TABLE IF EXISTS \"%w_cloudsync\";", table_name);
    } else {
        sql = sqlite3_snprintf(bsize, buffer, "DROP TABLE IF EXISTS \"%w\";", table_name);
    }
    
    return sql;
}

char *sql_escape_identifier (const char *name, char *buffer, size_t bsize) {
    return sqlite3_snprintf((int)bsize, buffer, "%q", name);
}

char *sql_build_select_nonpk_by_pk (cloudsync_context *data, const char *table_name, const char *schema) {
    UNUSED_PARAMETER(schema);
    char *sql = NULL;
    
    /*
    This SQL statement dynamically generates a SELECT query for a specified table.
    It uses Common Table Expressions (CTEs) to construct the column names and
    primary key conditions based on the table schema, which is obtained through
    the `pragma_table_info` function.

    1. `col_names` CTE:
       - Retrieves a comma-separated list of non-primary key column names from
         the specified table's schema.

    2. `pk_where` CTE:
       - Retrieves a condition string representing the primary key columns in the
         format: "column1=? AND column2=? AND ...", used to create the WHERE clause
         for selecting rows based on primary key values.

    3. Final SELECT:
       - Constructs the complete SELECT statement as a string, combining:
         - Column names from `col_names`.
         - The target table name.
         - The WHERE clause conditions from `pk_where`.

    The resulting query can be used to select rows from the table based on primary
    key values, and can be executed within the application to retrieve data dynamically.
    */

    // Unfortunately in SQLite column names (or table names) cannot be bound parameters in a SELECT statement
    // otherwise we should have used something like SELECT 'SELECT ? FROM %w WHERE rowid=?';
    char buffer[1024];
    char *singlequote_escaped_table_name = sql_escape_identifier(table_name, buffer, sizeof(buffer));
    
    #if !CLOUDSYNC_DISABLE_ROWIDONLY_TABLES
    if (table->rowid_only) {
        sql = memory_mprintf(SQL_BUILD_SELECT_NONPK_COLS_BY_ROWID, table->name, table->name);
        goto process_process;
    }
    #endif

    sql = cloudsync_memory_mprintf(SQL_BUILD_SELECT_NONPK_COLS_BY_PK, table_name, table_name, singlequote_escaped_table_name);

#if !CLOUDSYNC_DISABLE_ROWIDONLY_TABLES
process_process:
#endif
    if (!sql) return NULL;
    
    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);
    
    return (rc == DBRES_OK) ? query : NULL;
}

char *sql_build_delete_by_pk (cloudsync_context *data, const char *table_name, const char *schema) {
    UNUSED_PARAMETER(schema);
    char buffer[1024];
    char *singlequote_escaped_table_name = sql_escape_identifier(table_name, buffer, sizeof(buffer));
    char *sql = cloudsync_memory_mprintf(SQL_BUILD_DELETE_ROW_BY_PK, table_name, singlequote_escaped_table_name);
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

char *sql_build_insert_pk_ignore (cloudsync_context *data, const char *table_name, const char *schema) {
    UNUSED_PARAMETER(schema);
    char buffer[1024];
    char *singlequote_escaped_table_name = sql_escape_identifier(table_name, buffer, sizeof(buffer));
    char *sql = cloudsync_memory_mprintf(SQL_BUILD_INSERT_PK_IGNORE, table_name, table_name, singlequote_escaped_table_name);
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

char *sql_build_upsert_pk_and_col (cloudsync_context *data, const char *table_name, const char *colname, const char *schema) {
    UNUSED_PARAMETER(schema);
    char buffer[1024];
    char buffer2[1024];
    char *singlequote_escaped_table_name = sql_escape_identifier(table_name, buffer, sizeof(buffer));
    char *singlequote_escaped_col_name = sql_escape_identifier(colname, buffer2, sizeof(buffer2));
    char *sql = cloudsync_memory_mprintf(
        SQL_BUILD_UPSERT_PK_AND_COL,
        table_name,
        table_name,
        singlequote_escaped_table_name,
        singlequote_escaped_col_name,
        singlequote_escaped_col_name
    );
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

char *sql_build_upsert_pk_and_multi_cols (cloudsync_context *data, const char *table_name, const char **colnames, int ncolnames, const char *schema) {
    UNUSED_PARAMETER(schema);
    if (ncolnames <= 0 || !colnames) return NULL;

    // Get PK column names via pragma_table_info (same approach as database_pk_names)
    char **pk_names = NULL;
    int npks = 0;
    int rc = database_pk_names(data, table_name, &pk_names, &npks);
    if (rc != DBRES_OK || npks <= 0 || !pk_names) return NULL;

    // Build column list: "pk1","pk2","col_a","col_b"
    char *col_list = cloudsync_memory_mprintf("\"%w\"", pk_names[0]);
    if (!col_list) goto fail;
    for (int i = 1; i < npks; i++) {
        char *prev = col_list;
        col_list = cloudsync_memory_mprintf("%s,\"%w\"", prev, pk_names[i]);
        cloudsync_memory_free(prev);
        if (!col_list) goto fail;
    }
    for (int i = 0; i < ncolnames; i++) {
        char *prev = col_list;
        col_list = cloudsync_memory_mprintf("%s,\"%w\"", prev, colnames[i]);
        cloudsync_memory_free(prev);
        if (!col_list) goto fail;
    }

    // Build bind list: ?,?,?,?
    int total = npks + ncolnames;
    char *binds = (char *)cloudsync_memory_alloc(total * 2);
    if (!binds) { cloudsync_memory_free(col_list); goto fail; }
    int pos = 0;
    for (int i = 0; i < total; i++) {
        if (i > 0) binds[pos++] = ',';
        binds[pos++] = '?';
    }
    binds[pos] = '\0';

    // Build excluded set: "col_a"=EXCLUDED."col_a","col_b"=EXCLUDED."col_b"
    char *excl = cloudsync_memory_mprintf("\"%w\"=EXCLUDED.\"%w\"", colnames[0], colnames[0]);
    if (!excl) { cloudsync_memory_free(col_list); cloudsync_memory_free(binds); goto fail; }
    for (int i = 1; i < ncolnames; i++) {
        char *prev = excl;
        excl = cloudsync_memory_mprintf("%s,\"%w\"=EXCLUDED.\"%w\"", prev, colnames[i], colnames[i]);
        cloudsync_memory_free(prev);
        if (!excl) { cloudsync_memory_free(col_list); cloudsync_memory_free(binds); goto fail; }
    }

    // Assemble final SQL
    char *sql = cloudsync_memory_mprintf(
        "INSERT INTO \"%w\" (%s) VALUES (%s) ON CONFLICT DO UPDATE SET %s;",
        table_name, col_list, binds, excl
    );

    cloudsync_memory_free(col_list);
    cloudsync_memory_free(binds);
    cloudsync_memory_free(excl);
    for (int i = 0; i < npks; i++) cloudsync_memory_free(pk_names[i]);
    cloudsync_memory_free(pk_names);
    return sql;

fail:
    if (pk_names) {
        for (int i = 0; i < npks; i++) cloudsync_memory_free(pk_names[i]);
        cloudsync_memory_free(pk_names);
    }
    return NULL;
}

char *sql_build_update_pk_and_multi_cols (cloudsync_context *data, const char *table_name, const char **colnames, int ncolnames, const char *schema) {
    UNUSED_PARAMETER(schema);
    if (ncolnames <= 0 || !colnames) return NULL;

    // Get PK column names
    char **pk_names = NULL;
    int npks = 0;
    int rc = database_pk_names(data, table_name, &pk_names, &npks);
    if (rc != DBRES_OK || npks <= 0 || !pk_names) return NULL;

    // Build SET clause: "col_a"=?npks+1,"col_b"=?npks+2
    // Uses numbered parameters to match merge_flush_pending bind order:
    // positions 1..npks are PKs, npks+1..npks+ncolnames are column values.
    char *set_clause = cloudsync_memory_mprintf("\"%w\"=?%d", colnames[0], npks + 1);
    if (!set_clause) goto fail;
    for (int i = 1; i < ncolnames; i++) {
        char *prev = set_clause;
        set_clause = cloudsync_memory_mprintf("%s,\"%w\"=?%d", prev, colnames[i], npks + 1 + i);
        cloudsync_memory_free(prev);
        if (!set_clause) goto fail;
    }

    // Build WHERE clause: "pk1"=?1 AND "pk2"=?2
    char *where_clause = cloudsync_memory_mprintf("\"%w\"=?%d", pk_names[0], 1);
    if (!where_clause) { cloudsync_memory_free(set_clause); goto fail; }
    for (int i = 1; i < npks; i++) {
        char *prev = where_clause;
        where_clause = cloudsync_memory_mprintf("%s AND \"%w\"=?%d", prev, pk_names[i], 1 + i);
        cloudsync_memory_free(prev);
        if (!where_clause) { cloudsync_memory_free(set_clause); goto fail; }
    }

    // Assemble: UPDATE "table" SET ... WHERE ...
    char *sql = cloudsync_memory_mprintf(
        "UPDATE \"%w\" SET %s WHERE %s;",
        table_name, set_clause, where_clause
    );

    cloudsync_memory_free(set_clause);
    cloudsync_memory_free(where_clause);
    for (int i = 0; i < npks; i++) cloudsync_memory_free(pk_names[i]);
    cloudsync_memory_free(pk_names);
    return sql;

fail:
    if (pk_names) {
        for (int i = 0; i < npks; i++) cloudsync_memory_free(pk_names[i]);
        cloudsync_memory_free(pk_names);
    }
    return NULL;
}

char *sql_build_select_cols_by_pk (cloudsync_context *data, const char *table_name, const char *colname, const char *schema) {
    UNUSED_PARAMETER(schema);
    char *colnamequote = "\"";
    char buffer[1024];
    char buffer2[1024];
    char *singlequote_escaped_table_name = sql_escape_identifier(table_name, buffer, sizeof(buffer));
    char *singlequote_escaped_col_name = sql_escape_identifier(colname, buffer2, sizeof(buffer2));
    char *sql = cloudsync_memory_mprintf(
        SQL_BUILD_SELECT_COLS_BY_PK_FMT,
        table_name,
        colnamequote,
        singlequote_escaped_col_name,
        colnamequote,
        singlequote_escaped_table_name
    );
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

char *sql_build_rekey_pk_and_reset_version_except_col (cloudsync_context *data, const char *table_name, const char *except_col) {
    UNUSED_PARAMETER(data);

    char *meta_ref = database_build_meta_ref(NULL, table_name);
    if (!meta_ref) return NULL;

    char *result = cloudsync_memory_mprintf(SQL_CLOUDSYNC_REKEY_PK_AND_RESET_VERSION_EXCEPT_COL, meta_ref, except_col);
    cloudsync_memory_free(meta_ref);
    return result;
}

char *database_table_schema (const char *table_name) {
    return NULL;
}

char *database_build_meta_ref (const char *schema, const char *table_name) {
    // schema unused in SQLite
    return cloudsync_memory_mprintf("%s_cloudsync", table_name);
}

char *database_build_base_ref (const char *schema, const char *table_name) {
    // schema unused in SQLite
    return cloudsync_string_dup(table_name);
}

char *database_build_blocks_ref (const char *schema, const char *table_name) {
    // schema unused in SQLite
    return cloudsync_memory_mprintf("%s_cloudsync_blocks", table_name);
}

// SQLite version: schema parameter unused (SQLite has no schemas).
char *sql_build_delete_cols_not_in_schema_query (const char *schema, const char *table_name, const char *meta_ref, const char *pkcol) {
    UNUSED_PARAMETER(schema);
    return cloudsync_memory_mprintf(
        "DELETE FROM \"%w\" WHERE col_name NOT IN ("
        "SELECT name FROM pragma_table_info('%q') "
        "UNION SELECT '%s'"
        ");",
        meta_ref, table_name, pkcol
    );
}

char *sql_build_pk_collist_query (const char *schema, const char *table_name) {
    UNUSED_PARAMETER(schema);
    return cloudsync_memory_mprintf(
        "SELECT group_concat('\"' || format('%%w', name) || '\"', ',') "
        "FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk;",
        table_name
    );
}

char *sql_build_pk_decode_selectlist_query (const char *schema, const char *table_name) {
    UNUSED_PARAMETER(schema);
    return cloudsync_memory_mprintf(
        "SELECT group_concat("
        "'cloudsync_pk_decode(pk, ' || pk || ') AS ' || '\"' || format('%%w', name) || '\"', ','"
        ") "
        "FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk;",
        table_name
    );
}

char *sql_build_pk_qualified_collist_query (const char *schema, const char *table_name) {
    UNUSED_PARAMETER(schema);

    char buffer[1024];
    char *singlequote_escaped_table_name = sql_escape_identifier(table_name, buffer, sizeof(buffer));
    if (!singlequote_escaped_table_name) return NULL;

    return cloudsync_memory_mprintf(
        "SELECT group_concat('\"%w\".\"' || format('%%w', name) || '\"', ',') "
        "FROM pragma_table_info('%s') WHERE pk>0 ORDER BY pk;", singlequote_escaped_table_name, singlequote_escaped_table_name
    );
}

char *sql_build_insert_missing_pks_query(const char *schema, const char *table_name,
                                         const char *pkvalues_identifiers,
                                         const char *base_ref, const char *meta_ref,
                                         const char *filter) {
    UNUSED_PARAMETER(schema);

    // SQLite: Use NOT EXISTS with cloudsync_pk_encode (same approach as PostgreSQL).
    // This avoids needing pk_decode select list which requires executing a query.
    if (filter) {
        return cloudsync_memory_mprintf(
            "SELECT cloudsync_insert('%q', %s) "
            "FROM \"%w\" "
            "WHERE (%s) AND NOT EXISTS ("
            "    SELECT 1 FROM \"%w\" WHERE pk = cloudsync_pk_encode(%s)"
            ");",
            table_name, pkvalues_identifiers, base_ref, filter, meta_ref, pkvalues_identifiers
        );
    }
    return cloudsync_memory_mprintf(
        "SELECT cloudsync_insert('%q', %s) "
        "FROM \"%w\" "
        "WHERE NOT EXISTS ("
        "    SELECT 1 FROM \"%w\" WHERE pk = cloudsync_pk_encode(%s)"
        ");",
        table_name, pkvalues_identifiers, base_ref, meta_ref, pkvalues_identifiers
    );
}

// MARK: - PRIVATE -

static int database_select1_value (cloudsync_context *data, const char *sql, char **ptr_value, int64_t *int_value, DBTYPE expected_type) {
    sqlite3 *db = (sqlite3 *)cloudsync_db(data);
    
    // init values and sanity check expected_type
    if (ptr_value) *ptr_value = NULL;
    *int_value = 0;
    if (expected_type != DBTYPE_INTEGER && expected_type != DBTYPE_TEXT && expected_type != DBTYPE_BLOB) return SQLITE_MISUSE;
    
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2((sqlite3 *)db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup_select;
    
    // ensure at least one column
    if (sqlite3_column_count(vm) < 1) {rc = SQLITE_MISMATCH; goto cleanup_select;}
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) {rc = SQLITE_OK; goto cleanup_select;} // no rows OK
    if (rc != SQLITE_ROW) goto cleanup_select;
    
    // sanity check column type
    DBTYPE type = (DBTYPE)sqlite3_column_type(vm, 0);
    if (type == SQLITE_NULL) {rc = SQLITE_OK; goto cleanup_select;}
    if (type != expected_type) {rc = SQLITE_MISMATCH; goto cleanup_select;}
    
    if (expected_type == DBTYPE_INTEGER) {
        *int_value = (int64_t)sqlite3_column_int64(vm, 0);
    } else {
        const void *value = (expected_type == DBTYPE_TEXT) ? (const void *)sqlite3_column_text(vm, 0) : (const void *)sqlite3_column_blob(vm, 0);
        int len = sqlite3_column_bytes(vm, 0);
        if (len) {
            char *ptr = cloudsync_memory_alloc(len + 1);
            if (!ptr) {rc = SQLITE_NOMEM; goto cleanup_select;}
            
            if (len > 0 && value) memcpy(ptr, value, len);
            if (expected_type == DBTYPE_TEXT) ptr[len] = 0; // NULL terminate in case of TEXT
            
            *ptr_value = ptr;
            *int_value = len;
        }
    }
    rc = SQLITE_OK;
    
cleanup_select:
    if (vm) sqlite3_finalize(vm);
    return rc;
}

static int database_select2_values (cloudsync_context *data, const char *sql, char **value, int64_t *len, int64_t *value2) {
    sqlite3 *db = (sqlite3 *)cloudsync_db(data);
    
    // init values and sanity check expected_type
    *value = NULL;
    *value2 = 0;
    *len = 0;
    
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2((sqlite3 *)db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup_select;
    
    // ensure column count
    if (sqlite3_column_count(vm) < 2) {rc = SQLITE_MISMATCH; goto cleanup_select;}
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) {rc = SQLITE_OK; goto cleanup_select;} // no rows OK
    if (rc != SQLITE_ROW) goto cleanup_select;
    
    // sanity check column types
    if (sqlite3_column_type(vm, 0) != SQLITE_BLOB) {rc = SQLITE_MISMATCH; goto cleanup_select;}
    if (sqlite3_column_type(vm, 1) != SQLITE_INTEGER) {rc = SQLITE_MISMATCH; goto cleanup_select;}
    
    // 1st column is BLOB
    const void *blob = (const void *)sqlite3_column_blob(vm, 0);
    int blob_len = sqlite3_column_bytes(vm, 0);
    if (blob_len) {
        char *ptr = cloudsync_memory_alloc(blob_len);
        if (!ptr) {rc = SQLITE_NOMEM; goto cleanup_select;}
        
        if (blob_len > 0 && blob) memcpy(ptr, blob, blob_len);
        *value = ptr;
        *len = blob_len;
    }
    
    // 2nd column is INTEGER
    *value2 = (int64_t)sqlite3_column_int64(vm, 1);
    
    rc = SQLITE_OK;
    
cleanup_select:
    if (vm) sqlite3_finalize(vm);
    return rc;
}

bool database_system_exists (cloudsync_context *data, const char *name, const char *type) {
    sqlite3 *db = (sqlite3 *)cloudsync_db(data);
    sqlite3_stmt *vm = NULL;
    bool result = false;
    
    char sql[1024];
    snprintf(sql, sizeof(sql), "SELECT EXISTS (SELECT 1 FROM sqlite_master WHERE type='%s' AND name=?1 COLLATE NOCASE);", type);
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_bind_text(vm, 1, name, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_ROW) {
        result = (bool)sqlite3_column_int(vm, 0);
        rc = SQLITE_OK;
    }
    
finalize:
    if (rc != SQLITE_OK) DEBUG_ALWAYS("Error executing %s in dbutils_system_exists for type %s name %s (%s).", sql, type, name, sqlite3_errmsg(db));
    if (vm) sqlite3_finalize(vm);
    return result;
}

// MARK: - GENERAL -

int database_exec (cloudsync_context *data, const char *sql) {
    return sqlite3_exec((sqlite3 *)cloudsync_db(data), sql, NULL, NULL, NULL);
}

int database_exec_callback (cloudsync_context *data, const char *sql, int (*callback)(void *xdata, int argc, char **values, char **names), void *xdata) {
    return sqlite3_exec((sqlite3 *)cloudsync_db(data), sql, callback, xdata, NULL);
}

int database_write (cloudsync_context *data, const char *sql, const char **bind_values, DBTYPE bind_types[], int bind_lens[], int bind_count) {
    sqlite3 *db = (sqlite3 *)cloudsync_db(data);
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2((sqlite3 *)db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup_write;
    
    for (int i=0; i<bind_count; ++i) {
        switch (bind_types[i]) {
            case SQLITE_NULL:
                rc = sqlite3_bind_null(vm, i+1);
                break;
            case SQLITE_TEXT:
                rc = sqlite3_bind_text(vm, i+1, bind_values[i], bind_lens[i], SQLITE_STATIC);
                break;
            case SQLITE_BLOB:
                rc = sqlite3_bind_blob(vm, i+1, bind_values[i], bind_lens[i], SQLITE_STATIC);
                break;
            case SQLITE_INTEGER: {
                sqlite3_int64 value = strtoll(bind_values[i], NULL, 0);
                rc = sqlite3_bind_int64(vm, i+1, value);
            }   break;
            case SQLITE_FLOAT: {
                double value = strtod(bind_values[i], NULL);
                rc = sqlite3_bind_double(vm, i+1, value);
            }   break;
        }
        if (rc != SQLITE_OK) goto cleanup_write;
    }
        
    // execute statement
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    
cleanup_write:
    if (vm) sqlite3_finalize(vm);
    return rc;
}

int database_select_int (cloudsync_context *data, const char *sql, int64_t *value) {
    return database_select1_value(data, sql, NULL, value, DBTYPE_INTEGER);
}

int database_select_text (cloudsync_context *data, const char *sql, char **value) {
    int64_t len = 0;
    return database_select1_value(data, sql, value, &len, DBTYPE_TEXT);
}

int database_select_blob (cloudsync_context *data, const char *sql, char **value, int64_t *len) {
    return database_select1_value(data, sql, value, len, DBTYPE_BLOB);
}

int database_select_blob_int (cloudsync_context *data, const char *sql, char **value, int64_t *len, int64_t *value2) {
    return database_select2_values(data, sql, value, len, value2);
}

const char *database_errmsg (cloudsync_context *data) {
    return sqlite3_errmsg((sqlite3 *)cloudsync_db(data));
}

int database_errcode (cloudsync_context *data) {
    return sqlite3_errcode((sqlite3 *)cloudsync_db(data));
}

bool database_in_transaction (cloudsync_context *data) {
    sqlite3 *db = (sqlite3 *)cloudsync_db(data);
    bool in_transaction = (sqlite3_get_autocommit(db) != true);
    return in_transaction;
}

bool database_table_exists (cloudsync_context *data, const char *name, const char *schema) {
    UNUSED_PARAMETER(schema);
    return database_system_exists(data, name, "table");
}

bool database_internal_table_exists (cloudsync_context *data, const char *name) {
    return database_table_exists(data, name, NULL);
}

bool database_trigger_exists (cloudsync_context *data, const char *name) {
    return database_system_exists(data, name, "trigger");
}

int database_count_pk (cloudsync_context *data, const char *table_name, bool not_null, const char *schema) {
    UNUSED_PARAMETER(schema);
    char buffer[1024];
    char *sql = NULL;
    
    if (not_null) {
        sql = sqlite3_snprintf(sizeof(buffer), buffer, "SELECT count(*) FROM pragma_table_info('%q') WHERE pk>0 AND \"notnull\"=1;", table_name);
    } else {
        sql = sqlite3_snprintf(sizeof(buffer), buffer, "SELECT count(*) FROM pragma_table_info('%q') WHERE pk>0;", table_name);
    }
    
    int64_t count = 0;
    int rc = database_select_int(data, sql, &count);
    if (rc != DBRES_OK) return -1;
    return (int)count;
}

int database_count_nonpk (cloudsync_context *data, const char *table_name, const char *schema) {
    UNUSED_PARAMETER(schema);
    char buffer[1024];
    char *sql = NULL;
    
    sql = sqlite3_snprintf(sizeof(buffer), buffer, "SELECT count(*) FROM pragma_table_info('%q') WHERE pk=0;", table_name);
    int64_t count = 0;
    int rc = database_select_int(data, sql, &count);
    if (rc != DBRES_OK) return -1;
    return (int)count;
}

int database_count_int_pk (cloudsync_context *data, const char *table_name, const char *schema) {
    UNUSED_PARAMETER(schema);
    char buffer[1024];
    char *sql = sqlite3_snprintf(sizeof(buffer), buffer, "SELECT count(*) FROM pragma_table_info('%q') WHERE pk=1 AND \"type\" LIKE '%%INT%%';", table_name);
    
    int64_t count = 0;
    int rc = database_select_int(data, sql, &count);
    if (rc != DBRES_OK) return -1;
    return (int)count;
}

int database_count_notnull_without_default (cloudsync_context *data, const char *table_name, const char *schema) {
    UNUSED_PARAMETER(schema);
    char buffer[1024];
    char *sql = sqlite3_snprintf(sizeof(buffer), buffer, "SELECT count(*) FROM pragma_table_info('%q') WHERE pk=0 AND \"notnull\"=1 AND \"dflt_value\" IS NULL;", table_name);
    
    int64_t count = 0;
    int rc = database_select_int(data, sql, &count);
    if (rc != DBRES_OK) return -1;
    return (int)count;
}

int database_cleanup (cloudsync_context *data) {
    char *sql = "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'cloudsync_%' AND name NOT LIKE '%_cloudsync';";
    sqlite3 *db = (sqlite3 *)cloudsync_db(data);
    
    char **result = NULL;
    char *errmsg = NULL;
    int nrows, ncols;
    int rc = sqlite3_get_table(db, sql, &result, &nrows, &ncols, &errmsg);
    if (rc != SQLITE_OK) {
        cloudsync_set_error(data, (errmsg) ? errmsg : "Error retrieving augmented tables", rc);
        goto exit_cleanup;
    }
    
    for (int i = ncols; i < nrows+ncols; i+=ncols) {
        int rc2 = cloudsync_cleanup(data, result[i]);
        if (rc2 != SQLITE_OK) {rc = rc2; goto exit_cleanup;}
    }
    
exit_cleanup:
    if (result) sqlite3_free_table(result);
    if (errmsg) sqlite3_free(errmsg);
    return rc;
}

// MARK: - TRIGGERS and META -

int database_create_metatable (cloudsync_context *data, const char *table_name) {
    DEBUG_DBFUNCTION("database_create_metatable %s", table_name);
    
    // table_name cannot be longer than 512 characters so static buffer size is computed accordling to that value
    char buffer[2048];
    
    // WITHOUT ROWID is available starting from SQLite version 3.8.2 (2013-12-06) and later
    char *sql = sqlite3_snprintf(sizeof(buffer), buffer, "CREATE TABLE IF NOT EXISTS \"%w_cloudsync\" (pk BLOB NOT NULL, col_name TEXT NOT NULL, col_version INTEGER, db_version INTEGER, site_id INTEGER DEFAULT 0, seq INTEGER, PRIMARY KEY (pk, col_name)) WITHOUT ROWID; CREATE INDEX IF NOT EXISTS \"%w_cloudsync_db_idx\" ON \"%w_cloudsync\" (db_version);", table_name, table_name, table_name);
    
    int rc = database_exec(data, sql);
    DEBUG_SQL("\n%s", sql);
    return rc;
}

int database_create_insert_trigger (cloudsync_context *data, const char *table_name, char *trigger_when) {
    // NEW.prikey1, NEW.prikey2...
    char buffer[1024];
    char *trigger_name = sqlite3_snprintf(sizeof(buffer), buffer, "cloudsync_after_insert_%s", table_name);
    if (database_trigger_exists(data, trigger_name)) return SQLITE_OK;
    
    char buffer2[2048];
    char *sql2 = sqlite3_snprintf(sizeof(buffer2), buffer2, "SELECT group_concat('NEW.\"' || format('%%w', name) || '\"', ',') FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk;", table_name);
    
    char *pkclause = NULL;
    int rc = database_select_text(data, sql2, &pkclause);
    if (rc != SQLITE_OK) return rc;
    char *pkvalues = (pkclause) ? pkclause : "NEW.rowid";
    
    char *sql = cloudsync_memory_mprintf("CREATE TRIGGER \"%w\" AFTER INSERT ON \"%w\" %s BEGIN SELECT cloudsync_insert('%q', %s); END", trigger_name, table_name, trigger_when, table_name, pkvalues);
    if (pkclause) cloudsync_memory_free(pkclause);
    if (!sql) return SQLITE_NOMEM;
    
    rc = database_exec(data, sql);
    DEBUG_SQL("\n%s", sql);
    cloudsync_memory_free(sql);
    return rc;
}

int database_create_update_trigger_gos (cloudsync_context *data, const char *table_name) {
    // Grow Only Set
    // In a grow-only set, the update operation is not allowed.
    // A grow-only set is a type of CRDT (Conflict-free Replicated Data Type) where the only permissible operation is to add elements to the set,
    // without ever removing or modifying them.
    // Once an element is added to the set, it remains there permanently, which guarantees that the set only grows over time.
    char buffer[1024];
    char *trigger_name = sqlite3_snprintf(sizeof(buffer), buffer, "cloudsync_before_update_%s", table_name);
    if (database_trigger_exists(data, trigger_name)) return SQLITE_OK;
    
    char buffer2[2048+512];
    char *sql = sqlite3_snprintf(sizeof(buffer2), buffer2, "CREATE TRIGGER \"%w\" BEFORE UPDATE ON \"%w\" FOR EACH ROW WHEN cloudsync_is_enabled('%q') = 1 BEGIN SELECT RAISE(ABORT, 'Error: UPDATE operation is not allowed on table %w.'); END", trigger_name, table_name, table_name, table_name);
    
    int rc = database_exec(data, sql);
    DEBUG_SQL("\n%s", sql);
    return rc;
}

int database_create_update_trigger (cloudsync_context *data, const char *table_name, const char *trigger_when) {
    // NEW.prikey1, NEW.prikey2, OLD.prikey1, OLD.prikey2, NEW.col1, OLD.col1, NEW.col2, OLD.col2...
    
    char buffer[1024];
    char *trigger_name = sqlite3_snprintf(sizeof(buffer), buffer, "cloudsync_after_update_%s", table_name);
    if (database_trigger_exists(data, trigger_name)) return SQLITE_OK;
    
    // generate VALUES clause for all columns using a CTE to avoid compound SELECT limits
    // first, get all primary key columns in order
    char buffer2[2048];
    char *sql2 = sqlite3_snprintf(sizeof(buffer2), buffer2, "SELECT group_concat('('||quote('%q')||', NEW.\"' || format('%%w', name) || '\", OLD.\"' || format('%%w', name) || '\")', ', ') FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk;", table_name, table_name);
    
    char *pk_values_list = NULL;
    int rc = database_select_text(data, sql2, &pk_values_list);
    if (rc != SQLITE_OK) return rc;
    
    // then get all regular columns in order
    sql2 = sqlite3_snprintf(sizeof(buffer2), buffer2, "SELECT group_concat('('||quote('%q')||', NEW.\"' || format('%%w', name) || '\", OLD.\"' || format('%%w', name) || '\")', ', ') FROM pragma_table_info('%q') WHERE pk=0 ORDER BY cid;", table_name, table_name);
    
    char *col_values_list = NULL;
    rc = database_select_text(data, sql2, &col_values_list);
    if (rc != SQLITE_OK) {
        if (pk_values_list) cloudsync_memory_free(pk_values_list);
        return rc;
    }
    
    // build the complete VALUES query
    char *values_query = NULL;
    if (col_values_list && strlen(col_values_list) > 0) {
        // Table has both primary keys and regular columns
        values_query = cloudsync_memory_mprintf(
                                                "WITH column_data(table_name, new_value, old_value) AS (VALUES %s, %s) "
                                                "SELECT table_name, new_value, old_value FROM column_data",
                                                pk_values_list, col_values_list);
    } else {
        // Table has only primary keys
        values_query = cloudsync_memory_mprintf(
                                                "WITH column_data(table_name, new_value, old_value) AS (VALUES %s) "
                                                "SELECT table_name, new_value, old_value FROM column_data",
                                                pk_values_list);
    }
    
    if (pk_values_list) cloudsync_memory_free(pk_values_list);
    if (col_values_list) cloudsync_memory_free(col_values_list);
    if (!values_query) return SQLITE_NOMEM;
    
    // create the trigger with aggregate function
    char *sql = cloudsync_memory_mprintf(
                                         "CREATE TRIGGER \"%w\" AFTER UPDATE ON \"%w\" %s BEGIN "
                                         "SELECT cloudsync_update(table_name, new_value, old_value) FROM (%s); "
                                         "END",
                                         trigger_name, table_name, trigger_when, values_query);
    
    cloudsync_memory_free(values_query);
    if (!sql) return SQLITE_NOMEM;
    
    rc = database_exec(data, sql);
    DEBUG_SQL("\n%s", sql);
    cloudsync_memory_free(sql);
    return rc;
}

int database_create_delete_trigger_gos (cloudsync_context *data, const char *table_name) {
    // Grow Only Set
    // In a grow-only set, the delete operation is not allowed.
    
    char buffer[1024];
    char *trigger_name = sqlite3_snprintf(sizeof(buffer), buffer, "cloudsync_before_delete_%s", table_name);
    if (database_trigger_exists(data, trigger_name)) return SQLITE_OK;
    
    char buffer2[2048+512];
    char *sql = sqlite3_snprintf(sizeof(buffer2), buffer2, "CREATE TRIGGER \"%w\" BEFORE DELETE ON \"%w\" FOR EACH ROW WHEN cloudsync_is_enabled('%q') = 1 BEGIN SELECT RAISE(ABORT, 'Error: DELETE operation is not allowed on table %w.'); END", trigger_name, table_name, table_name, table_name);
        
    int rc = database_exec(data, sql);
    DEBUG_SQL("\n%s", sql);
    return rc;
}

int database_create_delete_trigger (cloudsync_context *data, const char *table_name, const char *trigger_when) {
    // OLD.prikey1, OLD.prikey2...
    
    char buffer[1024];
    char *trigger_name = sqlite3_snprintf(sizeof(buffer), buffer, "cloudsync_after_delete_%s", table_name);
    if (database_trigger_exists(data, trigger_name)) return SQLITE_OK;
    
    char buffer2[1024];
    char *sql2 = sqlite3_snprintf(sizeof(buffer2), buffer2, "SELECT group_concat('OLD.\"' || format('%%w', name) || '\"', ',') FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk;", table_name);
        
    char *pkclause = NULL;
    int rc = database_select_text(data, sql2, &pkclause);
    if (rc != SQLITE_OK) return rc;
    char *pkvalues = (pkclause) ? pkclause : "OLD.rowid";
    
    char *sql = cloudsync_memory_mprintf("CREATE TRIGGER \"%w\" AFTER DELETE ON \"%w\" %s BEGIN SELECT cloudsync_delete('%q',%s); END", trigger_name, table_name, trigger_when, table_name, pkvalues);
    if (pkclause) cloudsync_memory_free(pkclause);
    if (!sql) return SQLITE_NOMEM;
        
    rc = database_exec(data, sql);
    DEBUG_SQL("\n%s", sql);
    cloudsync_memory_free(sql);
    return rc;
}

// Build trigger WHEN clauses, optionally incorporating a row-level filter.
// INSERT/UPDATE use NEW-prefixed filter, DELETE uses OLD-prefixed filter.
// Returns dynamically-allocated strings that must be freed with cloudsync_memory_free.
static void database_build_trigger_when(
    cloudsync_context *data, const char *table_name, const char *filter,
    char **when_new_out, char **when_old_out)
{
    char *new_filter_str = NULL;
    char *old_filter_str = NULL;

    if (filter) {
        char sql_cols[1024];
        sqlite3_snprintf(sizeof(sql_cols), sql_cols,
            "SELECT name FROM pragma_table_info('%q') ORDER BY cid;", table_name);

        char *col_names[256];
        int ncols = 0;

        sqlite3_stmt *col_vm = NULL;
        int col_rc = sqlite3_prepare_v2((sqlite3 *)cloudsync_db(data), sql_cols, -1, &col_vm, NULL);
        if (col_rc == SQLITE_OK) {
            while (sqlite3_step(col_vm) == SQLITE_ROW && ncols < 256) {
                const char *name = (const char *)sqlite3_column_text(col_vm, 0);
                if (name) {
                    char *dup = cloudsync_memory_mprintf("%s", name);
                    if (!dup) break;
                    col_names[ncols++] = dup;
                }
            }
            sqlite3_finalize(col_vm);
        }

        if (ncols > 0) {
            new_filter_str = cloudsync_filter_add_row_prefix(filter, "NEW", col_names, ncols);
            old_filter_str = cloudsync_filter_add_row_prefix(filter, "OLD", col_names, ncols);
            for (int i = 0; i < ncols; ++i) cloudsync_memory_free(col_names[i]);
        }
    }

    if (new_filter_str) {
        *when_new_out = cloudsync_memory_mprintf(
            "FOR EACH ROW WHEN cloudsync_is_sync('%q') = 0 AND (%s)", table_name, new_filter_str);
    } else {
        *when_new_out = cloudsync_memory_mprintf(
            "FOR EACH ROW WHEN cloudsync_is_sync('%q') = 0", table_name);
    }

    if (old_filter_str) {
        *when_old_out = cloudsync_memory_mprintf(
            "FOR EACH ROW WHEN cloudsync_is_sync('%q') = 0 AND (%s)", table_name, old_filter_str);
    } else {
        *when_old_out = cloudsync_memory_mprintf(
            "FOR EACH ROW WHEN cloudsync_is_sync('%q') = 0", table_name);
    }

    if (new_filter_str) cloudsync_memory_free(new_filter_str);
    if (old_filter_str) cloudsync_memory_free(old_filter_str);
}

int database_create_triggers (cloudsync_context *data, const char *table_name, table_algo algo, const char *filter) {
    DEBUG_DBFUNCTION("database_create_triggers %s", table_name);

    if (dbutils_settings_check_version(data, "0.8.25") <= 0) {
        database_delete_triggers(data, table_name);
    }

    char *trigger_when_new = NULL;
    char *trigger_when_old = NULL;
    database_build_trigger_when(data, table_name, filter,
        &trigger_when_new, &trigger_when_old);

    if (!trigger_when_new || !trigger_when_old) {
        if (trigger_when_new) cloudsync_memory_free(trigger_when_new);
        if (trigger_when_old) cloudsync_memory_free(trigger_when_old);
        return SQLITE_NOMEM;
    }

    // INSERT TRIGGER (uses NEW prefix)
    int rc = database_create_insert_trigger(data, table_name, trigger_when_new);
    if (rc != SQLITE_OK) goto done;

    // UPDATE TRIGGER (uses NEW prefix)
    if (algo == table_algo_crdt_gos) rc = database_create_update_trigger_gos(data, table_name);
    else rc = database_create_update_trigger(data, table_name, trigger_when_new);
    if (rc != SQLITE_OK) goto done;

    // DELETE TRIGGER (uses OLD prefix)
    if (algo == table_algo_crdt_gos) rc = database_create_delete_trigger_gos(data, table_name);
    else rc = database_create_delete_trigger(data, table_name, trigger_when_old);

done:
    if (rc != SQLITE_OK) DEBUG_ALWAYS("database_create_triggers error %s (%d)", sqlite3_errmsg(cloudsync_db(data)), rc);
    cloudsync_memory_free(trigger_when_new);
    cloudsync_memory_free(trigger_when_old);
    return rc;
}

int database_delete_triggers (cloudsync_context *data, const char *table) {
    DEBUG_DBFUNCTION("database_delete_triggers %s", table);
    
    // from cloudsync_table_sanity_check we already know that 2048 is OK
    char buffer[2048];
    size_t blen = sizeof(buffer);
    int rc = SQLITE_ERROR;
    
    char *sql = sqlite3_snprintf((int)blen, buffer, "DROP TRIGGER IF EXISTS \"cloudsync_before_update_%w\";", table);
    rc = database_exec(data, sql);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_snprintf((int)blen, buffer, "DROP TRIGGER IF EXISTS \"cloudsync_before_delete_%w\";", table);
    rc = database_exec(data, sql);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_snprintf((int)blen, buffer, "DROP TRIGGER IF EXISTS \"cloudsync_after_insert_%w\";", table);
    rc = database_exec(data, sql);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_snprintf((int)blen, buffer, "DROP TRIGGER IF EXISTS \"cloudsync_after_update_%w\";", table);
    rc = database_exec(data, sql);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_snprintf((int)blen, buffer, "DROP TRIGGER IF EXISTS \"cloudsync_after_delete_%w\";", table);
    rc = database_exec(data, sql);
    if (rc != SQLITE_OK) goto finalize;
    
finalize:
    if (rc != SQLITE_OK) DEBUG_ALWAYS("dbutils_delete_triggers error %s (%s)", database_errmsg(data), sql);
    return rc;
}

// MARK: - SCHEMA -

int64_t database_schema_version (cloudsync_context *data) {
    int64_t value = 0;
    int rc = database_select_int(data, SQL_SCHEMA_VERSION, &value);
    return (rc == DBRES_OK) ? value : 0;
}

uint64_t database_schema_hash (cloudsync_context *data) {
    int64_t value = 0;
    int rc = database_select_int(data, "SELECT hash FROM cloudsync_schema_versions ORDER BY seq DESC limit 1;", &value);
    return (rc == DBRES_OK) ? (uint64_t)value : 0;
}

bool database_check_schema_hash (cloudsync_context *data, uint64_t hash) {
    // a change from the current version of the schema or from previous known schema can be applied
    // a change from a newer schema version not yet applied to this peer cannot be applied
    // so a schema hash is valid if it exists in the cloudsync_schema_versions table
    
    // the idea is to allow changes on stale peers and to be able to apply these changes on peers with newer schema,
    // but it requires alter table operation on augmented tables only add new columns and never drop columns for backward compatibility
    char sql[1024];
    snprintf(sql, sizeof(sql), "SELECT 1 FROM cloudsync_schema_versions WHERE hash = (%" PRId64 ")", (int64_t)hash);
    
    int64_t value = 0;
    database_select_int(data, sql, &value);
    return (value == 1);
}

int database_update_schema_hash (cloudsync_context *data, uint64_t *hash) {
    // Build normalized schema string using only: column name (lowercase), type (SQLite affinity), pk flag
    // Format: tablename:colname:affinity:pk,... (ordered by table name, then column id)
    // This makes the hash resilient to formatting, quoting, case differences and portable across databases
    //
    // Type mapping (simplified from SQLite affinity rules for cross-database compatibility):
    // - Types containing 'INT' → 'integer'
    // - Types containing 'CHAR', 'CLOB', 'TEXT' → 'text'
    // - Types containing 'BLOB' or empty → 'blob'
    // - Types containing 'REAL', 'FLOA', 'DOUB' → 'real'
    // - Types exactly 'NUMERIC' or 'DECIMAL' → 'numeric'
    // - Everything else → 'text' (default)
    //
    // NOTE: This deviates from SQLite's actual affinity rules where unknown types get NUMERIC affinity.
    // We use 'text' as default to improve cross-database compatibility with PostgreSQL, where types
    // like TIMESTAMPTZ, UUID, JSON, etc. are commonly used and map to 'text' in the PostgreSQL
    // implementation. This ensures schemas with PostgreSQL-specific type names in SQLite DDL
    // will hash consistently across both databases.
    sqlite3 *db = (sqlite3 *)cloudsync_db(data);

    char **tables = NULL;
    int ntables, tcols;
    int rc = sqlite3_get_table(db, "SELECT DISTINCT tbl_name FROM cloudsync_table_settings ORDER BY tbl_name;",
                               &tables, &ntables, &tcols, NULL);
    if (rc != SQLITE_OK || ntables == 0) {
        if (tables) sqlite3_free_table(tables);
        return SQLITE_ERROR;
    }

    char *schema = NULL;
    size_t schema_len = 0;
    size_t schema_cap = 0;

    for (int t = 1; t <= ntables; t++) {
        const char *tbl_name = tables[t];

        // Query pragma_table_info for this table with normalized type
        char *col_sql = cloudsync_memory_mprintf(
            "SELECT LOWER(name), "
            "CASE "
            "  WHEN UPPER(type) LIKE '%%INT%%' THEN 'integer' "
            "  WHEN UPPER(type) LIKE '%%CHAR%%' OR UPPER(type) LIKE '%%CLOB%%' OR UPPER(type) LIKE '%%TEXT%%' THEN 'text' "
            "  WHEN UPPER(type) LIKE '%%BLOB%%' OR type = '' THEN 'blob' "
            "  WHEN UPPER(type) LIKE '%%REAL%%' OR UPPER(type) LIKE '%%FLOA%%' OR UPPER(type) LIKE '%%DOUB%%' THEN 'real' "
            "  WHEN UPPER(type) IN ('NUMERIC', 'DECIMAL') THEN 'numeric' "
            "  ELSE 'text' "
            "END, "
            "CASE WHEN pk > 0 THEN '1' ELSE '0' END "
            "FROM pragma_table_info('%q') ORDER BY cid;", tbl_name);

        if (!col_sql) {
            if (schema) cloudsync_memory_free(schema);
            sqlite3_free_table(tables);
            return SQLITE_NOMEM;
        }

        char **cols = NULL;
        int nrows, ncols;
        rc = sqlite3_get_table(db, col_sql, &cols, &nrows, &ncols, NULL);
        cloudsync_memory_free(col_sql);

        if (rc != SQLITE_OK || ncols != 3) {
            if (cols) sqlite3_free_table(cols);
            if (schema) cloudsync_memory_free(schema);
            sqlite3_free_table(tables);
            return SQLITE_ERROR;
        }

        // Append each column: tablename:colname:affinity:pk
        for (int r = 1; r <= nrows; r++) {
            const char *col_name = cols[r * 3];
            const char *col_type = cols[r * 3 + 1];
            const char *col_pk = cols[r * 3 + 2];

            // Calculate required size: tbl_name:col_name:col_type:col_pk,
            size_t entry_len = strlen(tbl_name) + 1 + strlen(col_name) + 1 + strlen(col_type) + 1 + strlen(col_pk) + 1;

            if (schema_len + entry_len + 1 > schema_cap) {
                schema_cap = (schema_cap == 0) ? 1024 : schema_cap * 2;
                if (schema_cap < schema_len + entry_len + 1) schema_cap = schema_len + entry_len + 1;
                char *new_schema = cloudsync_memory_realloc(schema, schema_cap);
                if (!new_schema) {
                    if (schema) cloudsync_memory_free(schema);
                    sqlite3_free_table(cols);
                    sqlite3_free_table(tables);
                    return SQLITE_NOMEM;
                }
                schema = new_schema;
            }

            int written = snprintf(schema + schema_len, schema_cap - schema_len, "%s:%s:%s:%s,",
                                   tbl_name, col_name, col_type, col_pk);
            schema_len += written;
        }

        sqlite3_free_table(cols);
    }

    sqlite3_free_table(tables);

    if (!schema || schema_len == 0) return SQLITE_ERROR;

    // Remove trailing comma
    if (schema_len > 0 && schema[schema_len - 1] == ',') {
        schema[schema_len - 1] = '\0';
        schema_len--;
    }

    DEBUG_MERGE("database_update_schema_hash len %zu schema %s", schema_len, schema);
    sqlite3_uint64 h = fnv1a_hash(schema, schema_len);
    cloudsync_memory_free(schema);
    if (hash && *hash == h) return SQLITE_CONSTRAINT;

    char sql[1024];
    snprintf(sql, sizeof(sql), "INSERT INTO cloudsync_schema_versions (hash, seq) "
                               "VALUES (%" PRId64 ", COALESCE((SELECT MAX(seq) FROM cloudsync_schema_versions), 0) + 1) "
                               "ON CONFLICT(hash) DO UPDATE SET "
                               "  seq = (SELECT COALESCE(MAX(seq), 0) + 1 FROM cloudsync_schema_versions);", (int64_t)h);
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc == SQLITE_OK && hash) *hash = h;
    return rc;
}

// MARK: - VM -

int databasevm_prepare (cloudsync_context *data, const char *sql, dbvm_t **vm, int flags) {
    return sqlite3_prepare_v3((sqlite3 *)cloudsync_db(data), sql, -1, flags, (sqlite3_stmt **)vm, NULL);
}

int databasevm_step (dbvm_t *vm) {
    return sqlite3_step((sqlite3_stmt *)vm);
}

void databasevm_finalize (dbvm_t *vm) {
    sqlite3_finalize((sqlite3_stmt *)vm);
}

void databasevm_reset (dbvm_t *vm) {
    sqlite3_reset((sqlite3_stmt *)vm);
}

void databasevm_clear_bindings (dbvm_t *vm) {
    sqlite3_clear_bindings((sqlite3_stmt *)vm);
}

const char *databasevm_sql (dbvm_t *vm) {
    return sqlite3_sql((sqlite3_stmt *)vm);
    // the following allocates memory that needs to be freed
    // return sqlite3_expanded_sql((sqlite3_stmt *)vm);
}

static int database_pk_rowid (sqlite3 *db, const char *table_name, char ***names, int *count) {
    char buffer[2048];
    char *sql = sqlite3_snprintf(sizeof(buffer), buffer, "SELECT rowid FROM %Q LIMIT 0;", table_name);
    if (!sql) return SQLITE_NOMEM;
    
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    {
        char **r = (char**)cloudsync_memory_alloc(sizeof(char*));
        if (!r) {rc = SQLITE_NOMEM; goto cleanup;}
        r[0] = cloudsync_string_dup("rowid");
        if (!r[0]) {cloudsync_memory_free(r); rc = SQLITE_NOMEM; goto cleanup;}
        *names = r;
        *count = 1;
    }
    
cleanup:
    if (vm) sqlite3_finalize(vm);
    return rc;
}

int database_pk_names (cloudsync_context *data, const char *table_name, char ***names, int *count) {
    char buffer[2048];
    char *sql = sqlite3_snprintf(sizeof(buffer), buffer, "SELECT name FROM pragma_table_info(%Q) WHERE pk > 0 ORDER BY pk;", table_name);
    if (!sql) return SQLITE_NOMEM;
    
    sqlite3 *db = (sqlite3 *)cloudsync_db(data);
    sqlite3_stmt *vm = NULL;
    
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
    // count PK columns
    int rows = 0;
    while ((rc = sqlite3_step(vm)) == SQLITE_ROW) rows++;
    if (rc != SQLITE_DONE) goto cleanup;
    
    if (rows == 0) {
        sqlite3_finalize(vm);
        // no declared PKs so check for rowid availability
        return database_pk_rowid(db, table_name, names, count);
    }
    
    // reset vm to read PKs again
    rc = sqlite3_reset(vm);
    if (rc != SQLITE_OK) goto cleanup;
    
    // allocate array
    char **r = (char**)cloudsync_memory_zeroalloc(sizeof(char*) * rows);
    if (!r) {rc = SQLITE_NOMEM; goto cleanup;}
    
    int i = 0;
    while ((rc = sqlite3_step(vm)) == SQLITE_ROW) {
        const char *txt = (const char*)sqlite3_column_text(vm, 0);
        if (!txt) {rc = SQLITE_ERROR; goto cleanup_r;}
        r[i] = cloudsync_string_dup(txt);
        if (!r[i]) { rc = SQLITE_NOMEM; goto cleanup_r;}
        i++;
    }
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    
    *names = r;
    *count = rows;
    goto cleanup;
    
cleanup_r:
    for (int j = 0; j < i; j++) {
        if (r[j]) cloudsync_memory_free(r[j]);
    }
    cloudsync_memory_free(r);
    
cleanup:
    if (vm) sqlite3_finalize(vm);
    return rc;
}

// MARK: - BINDING -

int databasevm_bind_blob (dbvm_t *vm, int index, const void *value, uint64_t size) {
    return sqlite3_bind_blob64((sqlite3_stmt *)vm, index, value, size, SQLITE_STATIC);
}

int databasevm_bind_double (dbvm_t *vm, int index, double value) {
    return sqlite3_bind_double((sqlite3_stmt *)vm, index, value);
}

int databasevm_bind_int (dbvm_t *vm, int index, int64_t value) {
    return sqlite3_bind_int64((sqlite3_stmt *)vm, index, value);
}

int databasevm_bind_null (dbvm_t *vm, int index) {
    return sqlite3_bind_null((sqlite3_stmt *)vm, index);
}

int databasevm_bind_text (dbvm_t *vm, int index, const char *value, int size) {
    return sqlite3_bind_text((sqlite3_stmt *)vm, index, value, size, SQLITE_STATIC);
}

int databasevm_bind_value (dbvm_t *vm, int index, dbvalue_t *value) {
    return sqlite3_bind_value((sqlite3_stmt *)vm, index, (const sqlite3_value *)value);
}

// MARK: - VALUE -

const void *database_value_blob (dbvalue_t *value) {
    return sqlite3_value_blob((sqlite3_value *)value);
}

double database_value_double (dbvalue_t *value) {
    return sqlite3_value_double((sqlite3_value *)value);
}

int64_t database_value_int (dbvalue_t *value) {
    return (int64_t)sqlite3_value_int64((sqlite3_value *)value);
}

const char *database_value_text (dbvalue_t *value) {
    return (const char *)sqlite3_value_text((sqlite3_value *)value);
}

int database_value_bytes (dbvalue_t *value) {
    return sqlite3_value_bytes((sqlite3_value *)value);
}

int database_value_type (dbvalue_t *value) {
    return sqlite3_value_type((sqlite3_value *)value);
}

void database_value_free (dbvalue_t *value) {
    sqlite3_value_free((sqlite3_value *)value);
}

void *database_value_dup (dbvalue_t *value) {
    return sqlite3_value_dup((const sqlite3_value *)value);
}


// MARK: - COLUMN -

const void *database_column_blob (dbvm_t *vm, int index, size_t *len) {
    if (len) *len = sqlite3_column_bytes((sqlite3_stmt *)vm, index);
    return sqlite3_column_blob((sqlite3_stmt *)vm, index);
}

double database_column_double (dbvm_t *vm, int index) {
    return sqlite3_column_double((sqlite3_stmt *)vm, index);
}

int64_t database_column_int (dbvm_t *vm, int index) {
    return (int64_t)sqlite3_column_int64((sqlite3_stmt *)vm, index);
}

const char *database_column_text (dbvm_t *vm, int index) {
    return (const char *)sqlite3_column_text((sqlite3_stmt *)vm, index);
}

dbvalue_t *database_column_value (dbvm_t *vm, int index) {
    return (dbvalue_t *)sqlite3_column_value((sqlite3_stmt *)vm, index);
}

int database_column_bytes (dbvm_t *vm, int index) {
    return sqlite3_column_bytes((sqlite3_stmt *)vm, index);
}

int database_column_type (dbvm_t *vm, int index) {
    return sqlite3_column_type((sqlite3_stmt *)vm, index);
}

// MARK: - SAVEPOINT -

int database_begin_savepoint (cloudsync_context *data, const char *savepoint_name) {
    char sql[1024];
    snprintf(sql, sizeof(sql), "SAVEPOINT %s;", savepoint_name);
    return database_exec(data, sql);
}

int database_commit_savepoint (cloudsync_context *data, const char *savepoint_name) {
    char sql[1024];
    snprintf(sql, sizeof(sql), "RELEASE %s;", savepoint_name);
    return database_exec(data, sql);
}

int database_rollback_savepoint (cloudsync_context *data, const char *savepoint_name) {
    char sql[1024];
    snprintf(sql, sizeof(sql), "ROLLBACK TO %s; RELEASE %s;", savepoint_name, savepoint_name);
    return database_exec(data, sql);
}

// MARK: - MEMORY -

void *dbmem_alloc (uint64_t size) {
    return sqlite3_malloc64((sqlite3_uint64)size);
}

void *dbmem_zeroalloc (uint64_t size) {
    void *ptr = (void *)dbmem_alloc(size);
    if (!ptr) return NULL;
    
    memset(ptr, 0, (size_t)size);
    return ptr;
}

void *dbmem_realloc (void *ptr, uint64_t new_size) {
    return sqlite3_realloc64(ptr, (sqlite3_uint64)new_size);
}

char *dbmem_vmprintf (const char *format, va_list list) {
    return sqlite3_vmprintf(format, list);
}

char *dbmem_mprintf(const char *format, ...) {
    va_list ap;
    char *z;
    
    va_start(ap, format);
    z = dbmem_vmprintf(format, ap);
    va_end(ap);
    
    return z;
}

void dbmem_free (void *ptr) {
    sqlite3_free(ptr);
}

uint64_t dbmem_size (void *ptr) {
    return (uint64_t)sqlite3_msize(ptr);
}


