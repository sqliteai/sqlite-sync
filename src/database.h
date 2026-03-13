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

typedef void dbvm_t;
typedef void dbvalue_t;

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

// CALLBACK
typedef int (*database_exec_cb) (void *xdata, int argc, char **values, char **names);

int  database_exec (cloudsync_context *data, const char *sql);
int  database_exec_callback (cloudsync_context *data, const char *sql, database_exec_cb, void *xdata);
int  database_select_int (cloudsync_context *data, const char *sql, int64_t *value);
int  database_select_text (cloudsync_context *data, const char *sql, char **value);
int  database_select_blob (cloudsync_context *data, const char *sql, char **value, int64_t *value_len);
int  database_select_blob_int (cloudsync_context *data, const char *sql, char **value, int64_t *value_len, int64_t *value2);
int  database_write (cloudsync_context *data, const char *sql, const char **values, DBTYPE types[], int lens[], int count);
bool database_table_exists (cloudsync_context *data, const char *table_name, const char *schema);
bool database_internal_table_exists (cloudsync_context *data, const char *name);
bool database_trigger_exists (cloudsync_context *data, const char *table_name);
int  database_create_metatable (cloudsync_context *data, const char *table_name);
int  database_create_triggers (cloudsync_context *data, const char *table_name, table_algo algo, const char *filter);
int  database_delete_triggers (cloudsync_context *data, const char *table_name);
int  database_pk_names (cloudsync_context *data, const char *table_name, char ***names, int *count);
int  database_cleanup (cloudsync_context *data);

int database_count_pk (cloudsync_context *data, const char *table_name, bool not_null, const char *schema);
int database_count_nonpk (cloudsync_context *data, const char *table_name, const char *schema);
int database_count_int_pk (cloudsync_context *data, const char *table_name, const char *schema);
int database_count_notnull_without_default (cloudsync_context *data, const char *table_name, const char *schema);

int64_t  database_schema_version (cloudsync_context *data);
uint64_t database_schema_hash (cloudsync_context *data);
bool     database_check_schema_hash (cloudsync_context *data, uint64_t hash);
int      database_update_schema_hash (cloudsync_context *data, uint64_t *hash);

int database_begin_savepoint (cloudsync_context *data, const char *savepoint_name);
int database_commit_savepoint (cloudsync_context *data, const char *savepoint_name);
int database_rollback_savepoint (cloudsync_context *data, const char *savepoint_name);
bool database_in_transaction (cloudsync_context *data);
int database_errcode (cloudsync_context *data);
const char *database_errmsg (cloudsync_context *data);

// VM
int  databasevm_prepare (cloudsync_context *data, const char *sql, dbvm_t **vm, int flags);
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
const void *database_column_blob (dbvm_t *vm, int index, size_t *len);
double database_column_double (dbvm_t *vm, int index);
int64_t database_column_int (dbvm_t *vm, int index);
const char *database_column_text (dbvm_t *vm, int index);
dbvalue_t *database_column_value (dbvm_t *vm, int index);
int database_column_bytes (dbvm_t *vm, int index);
int database_column_type (dbvm_t *vm, int index);
 
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
char *sql_build_select_nonpk_by_pk (cloudsync_context *data, const char *table_name, const char *schema);
char *sql_build_delete_by_pk (cloudsync_context *data, const char *table_name, const char *schema);
char *sql_build_insert_pk_ignore (cloudsync_context *data, const char *table_name, const char *schema);
char *sql_build_upsert_pk_and_col (cloudsync_context *data, const char *table_name, const char *colname, const char *schema);
char *sql_build_upsert_pk_and_multi_cols (cloudsync_context *data, const char *table_name, const char **colnames, int ncolnames, const char *schema);
char *sql_build_update_pk_and_multi_cols (cloudsync_context *data, const char *table_name, const char **colnames, int ncolnames, const char *schema);
char *sql_build_select_cols_by_pk (cloudsync_context *data, const char *table_name, const char *colname, const char *schema);
char *sql_build_rekey_pk_and_reset_version_except_col (cloudsync_context *data, const char *table_name, const char *except_col);
char *sql_build_delete_cols_not_in_schema_query(const char *schema, const char *table_name, const char *meta_ref, const char *pkcol);
char *sql_build_pk_collist_query(const char *schema, const char *table_name);
char *sql_build_pk_decode_selectlist_query(const char *schema, const char *table_name);
char *sql_build_pk_qualified_collist_query(const char *schema, const char *table_name);
char *sql_build_insert_missing_pks_query(const char *schema, const char *table_name, const char *pkvalues_identifiers, const char *base_ref, const char *meta_ref, const char *filter);

char *database_table_schema(const char *table_name);
char *database_build_meta_ref(const char *schema, const char *table_name);
char *database_build_base_ref(const char *schema, const char *table_name);
char *database_build_blocks_ref(const char *schema, const char *table_name);

// OPAQUE STRUCT used by pk_context functions
typedef struct cloudsync_pk_decode_bind_context cloudsync_pk_decode_bind_context;

#endif
