//
//  database_sqlite.c
//  cloudsync
//
//  Created by Marco Bambini on 03/12/25.
//

#include "cloudsync.h"
#include "database.h"
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
