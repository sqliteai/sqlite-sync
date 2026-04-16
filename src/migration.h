//
//  migration.h
//  cloudsync
//
//  Created by Marco Bambini on 15/04/26.
//

#ifndef __CLOUDSYNC_MIGRATION__
#define __CLOUDSYNC_MIGRATION__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "database.h"

// MARK: - Enumerations -

// Platform-neutral migration operation codes
typedef enum {
    CLOUDSYNC_MIGRATION_ADD_COLUMN    = 1,
    CLOUDSYNC_MIGRATION_DROP_COLUMN   = 2,
    CLOUDSYNC_MIGRATION_RENAME_COLUMN = 3,
    CLOUDSYNC_MIGRATION_SET_DEFAULT   = 4,
    CLOUDSYNC_MIGRATION_CREATE_TABLE  = 5,
    CLOUDSYNC_MIGRATION_DROP_TABLE    = 6,
    CLOUDSYNC_MIGRATION_RENAME_TABLE  = 7,
    CLOUDSYNC_MIGRATION_CREATE_INDEX  = 8,
    CLOUDSYNC_MIGRATION_DROP_INDEX    = 9,
    CLOUDSYNC_MIGRATION_INIT_SYNC     = 10,
    CLOUDSYNC_MIGRATION_CUSTOM        = 11,
} cloudsync_migration_op;

// Abstract column types – each platform generates its own DDL keyword.
// See migration_sqlite.c and migration_postgresql.c for the type maps.
typedef enum {
    CSTYPE_INTEGER   = 1,   // SQLite: INTEGER    PostgreSQL: INTEGER
    CSTYPE_REAL      = 2,   // SQLite: REAL       PostgreSQL: DOUBLE PRECISION
    CSTYPE_TEXT      = 3,   // SQLite: TEXT       PostgreSQL: TEXT
    CSTYPE_BLOB      = 4,   // SQLite: BLOB       PostgreSQL: BYTEA
    CSTYPE_BOOLEAN   = 5,   // SQLite: INTEGER    PostgreSQL: BOOLEAN
    CSTYPE_UUID      = 6,   // SQLite: TEXT       PostgreSQL: UUID
    CSTYPE_TIMESTAMP = 7,   // SQLite: INTEGER    PostgreSQL: BIGINT
    CSTYPE_JSON      = 8,   // SQLite: TEXT       PostgreSQL: JSONB
} cloudsync_column_type;

// CRDT sync algorithms (mirrors table_algo in database.h)
typedef enum {
    CSALGO_CLS = 1,   // CausalLengthSet (default)
    CSALGO_GOS = 2,   // GrowOnlySet
    CSALGO_DWS = 3,   // DeleteWinsSet
    CSALGO_AWS = 4,   // AddWinsSet
} cloudsync_sync_algo;

// MARK: - Structures -

// Column definition used in CREATE_TABLE and ADD_COLUMN
typedef struct {
    char                 *name;
    cloudsync_column_type type;
    bool                  nullable;
    char                 *default_value;  // NULL = no DEFAULT clause
    bool                  is_pk;
} cloudsync_migration_column;

// Platform-neutral migration descriptor.
// Constructed with cloudsync_migration_create() and populated with the setter
// functions below; serialized into a BLOB for storage and transport.
typedef struct {
    cloudsync_migration_op   op;

    // Common: target table
    char  *table;
    char  *new_name;          // RENAME_TABLE, RENAME_COLUMN: destination name

    // Single-column operations (ADD_COLUMN, DROP_COLUMN, RENAME_COLUMN, SET_DEFAULT)
    char                 *col_name;
    cloudsync_column_type col_type;
    bool                  col_nullable;
    bool                  col_has_default;
    char                 *col_default;

    // CREATE_TABLE: column definitions
    cloudsync_migration_column *columns;
    int                         ncolumns;

    // CREATE_INDEX / DROP_INDEX
    char  *index_name;
    char **index_columns;
    int    nindex_columns;
    bool   index_unique;

    // INIT_SYNC
    cloudsync_sync_algo algo;
    char               *filter;         // NULL = no row filter

    // CUSTOM: platform-specific SQL (one or both can be set)
    char *sql_sqlite;
    char *sql_postgresql;
} cloudsync_migration_descriptor;

// MARK: - C API -

// Constructor / destructor
cloudsync_migration_descriptor *cloudsync_migration_create(cloudsync_migration_op op);
void                            cloudsync_migration_free(cloudsync_migration_descriptor *desc);

// Table / rename setters
void cloudsync_migration_set_table(cloudsync_migration_descriptor *desc, const char *table);
void cloudsync_migration_set_new_name(cloudsync_migration_descriptor *desc, const char *new_name);

// Single-column setters (ADD_COLUMN, DROP_COLUMN, RENAME_COLUMN, SET_DEFAULT)
void cloudsync_migration_set_column(cloudsync_migration_descriptor *desc, const char *col_name);
void cloudsync_migration_set_type(cloudsync_migration_descriptor *desc, cloudsync_column_type type);
void cloudsync_migration_set_nullable(cloudsync_migration_descriptor *desc, bool nullable);
void cloudsync_migration_set_default(cloudsync_migration_descriptor *desc, const char *default_val);

// CREATE_TABLE: add a column to the column list
void cloudsync_migration_add_column(cloudsync_migration_descriptor *desc,
                                    const char *name, cloudsync_column_type type,
                                    bool nullable, const char *default_val);
// Mark a previously added column as primary key
void cloudsync_migration_set_primary_key(cloudsync_migration_descriptor *desc,
                                         const char *col_name);

// Index setters (CREATE_INDEX / DROP_INDEX)
void cloudsync_migration_set_index_name(cloudsync_migration_descriptor *desc,
                                        const char *index_name);
void cloudsync_migration_add_index_column(cloudsync_migration_descriptor *desc,
                                          const char *col_name);
void cloudsync_migration_set_index_unique(cloudsync_migration_descriptor *desc, bool unique);

// INIT_SYNC setters
void cloudsync_migration_set_algo(cloudsync_migration_descriptor *desc, cloudsync_sync_algo algo);
void cloudsync_migration_set_filter(cloudsync_migration_descriptor *desc, const char *filter);

// CUSTOM setters
void cloudsync_migration_set_sql_sqlite(cloudsync_migration_descriptor *desc, const char *sql);
void cloudsync_migration_set_sql_postgresql(cloudsync_migration_descriptor *desc, const char *sql);

// Store a migration in the local cloudsync_migrations table.
// version: monotonic server-assigned integer (must be unique)
// desc:    descriptor to serialize and store
int cloudsync_migration_register(cloudsync_context *ctx, int64_t version,
                                 cloudsync_migration_descriptor *desc);

// Apply all unapplied migrations (applied_at IS NULL) in version order.
// For DDL operations this wraps each in cloudsync_begin_alter /
// cloudsync_commit_alter; for INIT_SYNC it calls cloudsync_init_table directly.
// Stops on the first failure and leaves applied_at NULL for the failing row.
int cloudsync_migration_apply_pending(cloudsync_context *ctx);

// Serialization: produce / consume the binary descriptor BLOB
int  cloudsync_migration_serialize(const cloudsync_migration_descriptor *desc,
                                   void **out_blob, size_t *out_size);
cloudsync_migration_descriptor *cloudsync_migration_deserialize(const void *blob, size_t size);

// FNV-1a 64-bit checksum over arbitrary bytes (same algorithm as schema_hash)
uint64_t cloudsync_migration_checksum(const void *data, size_t size);

// Human-readable names for diagnostic output
const char *cloudsync_migration_op_name(cloudsync_migration_op op);
const char *cloudsync_sync_algo_name(cloudsync_sync_algo algo);

// Platform-specific DDL generation and execution (implemented in
// src/sqlite/migration_sqlite.c and src/postgresql/migration_postgresql.c)
int   database_migration_execute(cloudsync_context *ctx,
                                 cloudsync_migration_descriptor *desc);
char *database_migration_sql(const cloudsync_migration_descriptor *desc);

#endif // __CLOUDSYNC_MIGRATION__
