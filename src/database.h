//
//  database.h
//  cloudsync
//
//  Created by Marco Bambini on 03/12/25.
//

#ifndef __CLOUDSYNC_DATABASE__
#define __CLOUDSYNC_DATABASE__

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct db_t db_t;
typedef void dbvm_t;
typedef void dbvalue_t;
typedef void dbcontext_t;

typedef enum {
    DBRES_OK         = 0,
    DBRES_ERROR      = 1,
    DBRES_ABORT      = 4,
    DBRES_NOMEM      = 7,
    DBRES_IOERR      = 10,
    DBRES_CONSTRAINT = 19,
    DBRES_MISUSE     = 21,
    DBRES_ROW        = 100,
    DBRES_DONE       = 101
} DBRES;

typedef enum {
    DBTYPE_INTEGER = 1,
    DBTYPE_FLOAT   = 2,
    DBTYPE_TEXT    = 3,
    DBTYPE_BLOB    = 4,
    DBTYPE_NULL    = 5
} DBTYPE;
    
typedef enum {
    DBFLAG_PERSISTENT = 0x01
} DBFLAG;

// The type of CRDT chosen for a table controls what rows are included or excluded when merging tables together from different databases
typedef enum {
    table_algo_none = 0,
    table_algo_crdt_cls = 100,   // CausalLengthSet
    table_algo_crdt_gos,         // GrowOnlySet
    table_algo_crdt_dws,         // DeleteWinsSet
    table_algo_crdt_aws          // AddWinsSet
} table_algo;
 
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(X) (void)(X)
#endif

// OPAQUE STRUCT
typedef struct cloudsync_context cloudsync_context;

// GENERAL
typedef int (*database_exec_cb) (void *xdata, int argc, char **values, char **names);

int  database_exec (cloudsync_context *data, const char *sql);
int  database_exec_callback (cloudsync_context *data, const char *sql, database_exec_cb, void *xdata);
int  database_select_int (cloudsync_context *data, const char *sql, int64_t *value);
int  database_select_text (cloudsync_context *data, const char *sql, char **value);
int  database_select_blob (cloudsync_context *data, const char *sql, char **value, int64_t *value_len);
int  database_select_blob_2int (cloudsync_context *data, const char *sql, char **value, int64_t *value_len, int64_t *value2, int64_t *value3);
int  database_write (cloudsync_context *data, const char *sql, const char **values, DBTYPE types[], int lens[], int count);
bool database_table_exists (cloudsync_context *data, const char *table_name);
bool database_trigger_exists (cloudsync_context *data, const char *table_name);
int  database_create_metatable (cloudsync_context *data, const char *table_name);
int  database_create_triggers (cloudsync_context *data, const char *table_name, table_algo algo);
int  database_delete_triggers (cloudsync_context *data, const char *table_name);
int  database_debug (db_t *db, bool print_result);
int  database_pk_names (cloudsync_context *data, const char *table_name, char ***names, int *count);

int database_count_pk (cloudsync_context *data, const char *table_name, bool not_null);
int database_count_nonpk (cloudsync_context *data, const char *table_name);
int database_count_int_pk (cloudsync_context *data, const char *table_name);
int database_count_notnull_without_default (cloudsync_context *data, const char *table_name);

int64_t  database_schema_version (cloudsync_context *data);
uint64_t database_schema_hash (cloudsync_context *data);
bool     database_check_schema_hash (cloudsync_context *data, uint64_t hash);
int      database_update_schema_hash (cloudsync_context *data, uint64_t *hash);

int database_begin_savepoint (cloudsync_context *data, const char *savepoint_name);
int database_commit_savepoint (cloudsync_context *data, const char *savepoint_name);
int database_rollback_savepoint (cloudsync_context *data, const char *savepoint_name);
bool database_in_transaction (cloudsync_context *data);
int database_errcode (db_t *db);
const char *database_errmsg (db_t *db);

// VM
int  database_prepare (cloudsync_context *data, const char *sql, dbvm_t **vm, int flags);
int  databasevm_step (dbvm_t *vm);
void databasevm_finalize (dbvm_t *vm);
void databasevm_reset (dbvm_t *vm);
void databasevm_clear_bindings (dbvm_t *vm);
const char *databasevm_sql (dbvm_t *vm);

// BINDING
int databasevm_bind_blob (dbvm_t *vm, int index, const void *value, uint64_t size);
int databasevm_bind_double (dbvm_t *vm, int index, double value);
int databasevm_bind_int (dbvm_t *vm, int index, int64_t value);
int databasevm_bind_null (dbvm_t *vm, int index);
int databasevm_bind_text (dbvm_t *vm, int index, const char *value, int size);
int databasevm_bind_value (dbvm_t *vm, int index, dbvalue_t *value);

// VALUE
const void *database_value_blob (dbvalue_t *value);
double database_value_double (dbvalue_t *value);
int64_t database_value_int (dbvalue_t *value);
const char *database_value_text (dbvalue_t *value);
int database_value_bytes (dbvalue_t *value);
int database_value_type (dbvalue_t *value);
void database_value_free (dbvalue_t *value);
void *database_value_dup (dbvalue_t *value);

// COLUMN
const void *database_column_blob (dbvm_t *vm, int index);
double database_column_double (dbvm_t *vm, int index);
int64_t database_column_int (dbvm_t *vm, int index);
const char *database_column_text (dbvm_t *vm, int index);
dbvalue_t *database_column_value (dbvm_t *vm, int index);
int database_column_bytes (dbvm_t *vm, int index);
int database_column_type (dbvm_t *vm, int index);

// RESULT
void database_result_blob (dbcontext_t *context, const void *value, uint64_t size, void(*)(void*));
void database_result_double (dbcontext_t *context, double value);
void database_result_int (dbcontext_t *context, int64_t value);
void database_result_null (dbcontext_t *context);
void database_result_text (dbcontext_t *context, const char *value, int size, void(*)(void*));
void database_result_value (dbcontext_t *context, dbvalue_t *value);

// MEMORY
void *dbmem_alloc (uint64_t size);
void *dbmem_zeroalloc (uint64_t size);
void *dbmem_realloc (void *ptr, uint64_t new_size);
char *dbmem_mprintf(const char *format, ...);
char *dbmem_vmprintf (const char *format, va_list list);
void dbmem_free (void *ptr);
uint64_t dbmem_size (void *ptr);

// SQL
char *sql_build_drop_table (const char *table_name, char *buffer, int bsize, bool is_meta);
char *sql_escape_name (const char *name, char *buffer, size_t bsize);
char *sql_build_select_nonpk_by_pk (cloudsync_context *data, const char *table_name);
char *sql_build_delete_by_pk (cloudsync_context *data, const char *table_name);
char *sql_build_insert_pk_ignore (cloudsync_context *data, const char *table_name);
char *sql_build_upsert_pk_and_col (cloudsync_context *data, const char *table_name, const char *colname);
char *sql_build_select_cols_by_pk (cloudsync_context *data, const char *table_name, const char *colname);

// USED ONLY by SQLite Cloud to implement RLS
typedef struct cloudsync_pk_decode_bind_context cloudsync_pk_decode_bind_context;
typedef bool (*cloudsync_payload_apply_callback_t)(void **xdata, cloudsync_pk_decode_bind_context *decoded_change, db_t *db, void *data, int step, int rc);
void cloudsync_set_payload_apply_callback(db_t *db, cloudsync_payload_apply_callback_t callback);
cloudsync_payload_apply_callback_t cloudsync_get_payload_apply_callback(db_t *db);

#endif
