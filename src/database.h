//
//  database.h
//  cloudsync
//
//  Created by Marco Bambini on 03/12/25.
//

#ifndef __CLOUDSYNC_DATABASE__
#define __CLOUDSYNC_DATABASE__

#include <stdbool.h>

typedef long long int db_int64;
typedef unsigned long long int db_uint64;
typedef void db_t;
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

// GENERAL
typedef int (*database_exec_cb) (void *xdata, int argc, char **values, char **names);

int  database_exec (db_t *db, const char *sql);
int  database_exec_callback (db_t *db, const char *sql, database_exec_cb, void *xdata);
int  database_select_int (db_t *db, const char *sql, db_int64 *value);
int  database_select_text (db_t *db, const char *sql, char **value);
int  database_select_blob (db_t *db, const char *sql, char **value, db_int64 *value_len);
int  database_select_blob_2int (db_t *db, const char *sql, char **value, db_int64 *value_len, db_int64 *value2, db_int64 *value3);
int  database_write (db_t *db, const char *sql, const char **values, DBTYPE types[], int lens[], int count);
bool database_table_exists (db_t *db, const char *table_name);
bool database_trigger_exists (db_t *db, const char *table_name);
int  database_create_metatable (db_t *db, const char *table_name);
int  database_create_triggers (db_t *db, const char *table_name, table_algo algo);
int  database_delete_triggers (db_t *db, const char *table_name);
int  database_debug (db_t *db, bool print_result);

int database_count_pk (db_t *db, const char *table_name, bool not_null);
int database_count_int_pk (db_t *db, const char *table_name);
int database_count_notnull_without_default (db_t *db, const char *table_name);

db_int64 database_schema_version (db_t *db);
uint64_t database_schema_hash (db_t *db);
bool     database_check_schema_hash (db_t *db, uint64_t hash);
int      database_update_schema_hash (db_t *db, uint64_t *hash);

int database_begin_savepoint (db_t *db, const char *savepoint_name);
int database_commit_savepoint (db_t *db, const char *savepoint_name);
int database_rollback_savepoint (db_t *db, const char *savepoint_name);
int database_errcode (db_t *db);
bool database_in_transaction (db_t *db);
const char *database_errmsg (db_t *db);

// VM
int  database_prepare (db_t *db, const char *sql, dbvm_t **vm, int flags);
int  databasevm_step (dbvm_t *vm);
void databasevm_finalize (dbvm_t *vm);
void databasevm_reset (dbvm_t *vm);
void databasevm_clear_bindings (dbvm_t *vm);
const char *databasevm_sql (dbvm_t *vm);

// BINDING
int databasevm_bind_blob (dbvm_t *vm, int index, const void *value, db_uint64 size);
int databasevm_bind_double (dbvm_t *vm, int index, double value);
int databasevm_bind_int (dbvm_t *vm, int index, db_int64 value);
int databasevm_bind_null (dbvm_t *vm, int index);
int databasevm_bind_text (dbvm_t *vm, int index, const char *value, int size);
int databasevm_bind_value (dbvm_t *vm, int index, dbvalue_t *value);

// VALUE
const void *database_value_blob (dbvalue_t *value);
double database_value_double (dbvalue_t *value);
db_int64 database_value_int (dbvalue_t *value);
const char *database_value_text (dbvalue_t *value);
int database_value_bytes (dbvalue_t *value);
int database_value_type (dbvalue_t *value);
void database_value_free (dbvalue_t *value);
void *database_value_dup (dbvalue_t *value);

// COLUMN
const void *database_column_blob (dbvm_t *vm, int index);
double database_column_double (dbvm_t *vm, int index);
db_int64 database_column_int (dbvm_t *vm, int index);
const char *database_column_text (dbvm_t *vm, int index);
dbvalue_t *database_column_value (dbvm_t *vm, int index);
int database_column_bytes (dbvm_t *vm, int index);
int database_column_type (dbvm_t *vm, int index);

// RESULT
void database_result_blob (dbcontext_t *context, const void *value, db_uint64 size, void(*)(void*));
void database_result_double (dbcontext_t *context, double value);
void database_result_int (dbcontext_t *context, db_int64 value);
void database_result_null (dbcontext_t *context);
void database_result_text (dbcontext_t *context, const char *value, int size, void(*)(void*));
void database_result_value (dbcontext_t *context, dbvalue_t *value);

// MEMORY
void *dbmem_alloc (db_uint64 size);
void *dbmem_zeroalloc (db_uint64 size);
void *dbmem_realloc (void *ptr, db_uint64 new_size);
char *dbmem_mprintf(const char *format, ...);
void dbmem_free (void *ptr);
db_uint64 dbmem_size (void *ptr);

int database_pk_names (db_t *db, const char *table_name, char ***names, int *count);
char *sql_build_drop_table (const char *table_name, char *buffer, int bsize, bool is_meta);

// USED ONLY by SQLite Cloud to implement RLS
typedef struct cloudsync_pk_decode_bind_context cloudsync_pk_decode_bind_context;
typedef bool (*cloudsync_payload_apply_callback_t)(void **xdata, cloudsync_pk_decode_bind_context *decoded_change, db_t *db, void *data, int step, int rc);
void cloudsync_set_payload_apply_callback(db_t *db, cloudsync_payload_apply_callback_t callback);
cloudsync_payload_apply_callback_t cloudsync_get_payload_apply_callback(db_t *db);

#endif
