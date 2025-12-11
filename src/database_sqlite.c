//
//  database_sqlite.c
//  cloudsync
//
//  Created by Marco Bambini on 03/12/25.
//

#include "cloudsync.h"
#include "database.h"
#include "utils.h"

#include <string.h>

#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT3
#endif

// MARK: GENERAL -

int database_exec (db_t *db, const char *sql) {
    return sqlite3_exec((sqlite3 *)db, sql, NULL, NULL, NULL);
}

int database_exec_callback (db_t *db, const char *sql, int (*callback)(void *xdata, int argc, char **values, char **names), void *xdata) {
    return sqlite3_exec((sqlite3 *)db, sql, callback, xdata, NULL);
}

const char *database_errmsg (db_t *db) {
    return sqlite3_errmsg((sqlite3 *)db);
}

int database_errcode (db_t *db) {
    return sqlite3_errcode((sqlite3 *)db);
}

bool database_in_transaction (db_t *db) {
    bool in_transaction = (sqlite3_get_autocommit(db) != true);
    return in_transaction;
}

// MARK: - VM and BINDING -

int database_prepare (db_t *db, const char *sql, dbvm_t **vm, int flags) {
    return sqlite3_prepare_v3((sqlite3 *)db, sql, -1, flags, (sqlite3_stmt **)vm, NULL);
}

int database_step (dbvm_t *vm) {
    return sqlite3_step((sqlite3_stmt *)vm);
}

void database_finalize (dbvm_t *vm) {
    sqlite3_finalize((sqlite3_stmt *)vm);
}

void database_reset (dbvm_t *vm) {
    sqlite3_reset((sqlite3_stmt *)vm);
}

void database_clear_bindings (dbvm_t *vm) {
    sqlite3_clear_bindings((sqlite3_stmt *)vm);
}

const char *database_sql (dbvm_t *vm) {
    return sqlite3_expanded_sql((sqlite3_stmt *)vm);
}

int database_pk_rowid (db_t *db, const char *table_name, char ***names, int *count) {
    char buffer[2048];
    char *sql = sqlite3_snprintf(sizeof(buffer), buffer, "SELECT rowid FROM %Q LIMIT 0;", table_name);
    if (!sql) return SQLITE_NOMEM;
    
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
    if (rc == SQLITE_OK) {
        char **r = (char**)dbmem_alloc(sizeof(char*));
        if (!r) return SQLITE_NOMEM;
        r[0] = cloudsync_string_dup("rowid", false);
        *names = r;
        *count = 1;
    } else {
        // WITHOUT ROWID + no declared PKs => return empty set
        *names = NULL;
        *count = 0;
        rc = SQLITE_OK;
    }
    
cleanup:
    if (vm) sqlite3_finalize(vm);
    return rc;
}

int database_pk_names (db_t *db, const char *table_name, char ***names, int *count) {
    char buffer[2048];
    char *sql = sqlite3_snprintf(sizeof(buffer), buffer, "SELECT name FROM pragma_table_info(%Q) WHERE pk > 0 ORDER BY pk;", table_name);
    if (!sql) return SQLITE_NOMEM;
    
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
    char **r = (char**)dbmem_alloc(sizeof(char*) * rows);
    if (!r) {rc = SQLITE_NOMEM; goto cleanup;}
    
    int i = 0;
    while ((rc = sqlite3_step(vm)) == SQLITE_ROW) {
        const char *txt = (const char*)sqlite3_column_text(vm, 0);
        if (!txt) {rc = SQLITE_ERROR; goto cleanup;}
        r[i] = cloudsync_string_dup(txt, false);
        if (!r[i]) { rc = SQLITE_NOMEM; goto cleanup;}
        i++;
    }
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    
    *names = r;
    *count = rows;
    
cleanup:
    if (vm) sqlite3_finalize(vm);
    return rc;
}

// MARK: -

int database_bind_blob (dbvm_t *vm, int index, const void *value, db_uint64 size) {
    return sqlite3_bind_blob64((sqlite3_stmt *)vm, index, value, size, SQLITE_STATIC);
}

int database_bind_double (dbvm_t *vm, int index, double value) {
    return sqlite3_bind_double((sqlite3_stmt *)vm, index, value);
}

int database_bind_int (dbvm_t *vm, int index, db_int64 value) {
    return sqlite3_bind_int64((sqlite3_stmt *)vm, index, value);
}

int database_bind_null (dbvm_t *vm, int index) {
    return sqlite3_bind_null((sqlite3_stmt *)vm, index);
}

int database_bind_text (dbvm_t *vm, int index, const char *value, int size) {
    return sqlite3_bind_text((sqlite3_stmt *)vm, index, value, size, SQLITE_STATIC);
}

int database_bind_value (dbvm_t *vm, int index, dbvalue_t *value) {
    return sqlite3_bind_value((sqlite3_stmt *)vm, index, (const sqlite3_value *)value);
}

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

// MARK: - VALUE -

const void *database_value_blob (dbvalue_t *value) {
    return sqlite3_value_blob((sqlite3_value *)value);
}

double database_value_double (dbvalue_t *value) {
    return sqlite3_value_double((sqlite3_value *)value);
}

db_int64 database_value_int (dbvalue_t *value) {
    return (db_int64)sqlite3_value_int64((sqlite3_value *)value);
}

const unsigned char *database_value_text (dbvalue_t *value) {
    return sqlite3_value_text((sqlite3_value *)value);
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

const void *database_column_blob (dbvm_t *vm, int index) {
    return sqlite3_column_blob((sqlite3_stmt *)vm, index);
}

double database_column_double (dbvm_t *vm, int index) {
    return sqlite3_column_double((sqlite3_stmt *)vm, index);
}

db_int64 database_column_int (dbvm_t *vm, int index) {
    return (db_int64)sqlite3_column_int64((sqlite3_stmt *)vm, index);
}

const unsigned char *database_column_text (dbvm_t *vm, int index) {
    return sqlite3_column_text((sqlite3_stmt *)vm, index);
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

// MARK: - MEMORY -

void *dbmem_alloc (db_uint64 size) {
    return sqlite3_malloc64((sqlite3_uint64)size);
}

void *dbmem_zeroalloc (uint64_t size) {
    void *ptr = (void *)dbmem_alloc(size);
    if (!ptr) return NULL;
    
    memset(ptr, 0, (size_t)size);
    return ptr;
}

void *dbmem_realloc (void *ptr, db_uint64 new_size) {
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

db_uint64 dbmem_size (void *ptr) {
    return (db_uint64)sqlite3_msize(ptr);
}
