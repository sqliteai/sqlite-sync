//
//  cloudsync_postgresql.c
//  cloudsync
//
//  Created by Claude Code on 18/12/25.
//

// Define POSIX feature test macros before any includes
#define _POSIX_C_SOURCE 200809L

// PostgreSQL requires postgres.h to be included FIRST
#include "postgres.h"
#include "utils/datum.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "catalog/namespace.h"
#include "executor/spi.h"
#include "utils/lsyscache.h"
#include "utils/array.h"
#include "fmgr.h"
#include "funcapi.h"
#include "pgvalue.h"
#include "storage/ipc.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/uuid.h"
#include "nodes/nodeFuncs.h"   // exprTypmod, exprCollation
#include "nodes/pg_list.h"     // linitial
#include "nodes/primnodes.h"   // FuncExpr

// CloudSync headers (after PostgreSQL headers)
#include "../cloudsync.h"
#include "../block.h"
#include "../database.h"
#include "../dbutils.h"
#include "../pk.h"
#include "../utils.h"

// Note: network.h is not needed for PostgreSQL implementation

PG_MODULE_MAGIC;

// Note: PG_FUNCTION_INFO_V1 macros are declared before each function implementation below
// They should NOT be duplicated here to avoid redefinition errors

#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(X) (void)(X)
#endif

#define CLOUDSYNC_RLS_RESTRICTED_VALUE_BYTEA "E'\\\\x0b095f5f5b524c535d5f5f'::bytea"
#define CLOUDSYNC_NULL_VALUE_BYTEA "E'\\\\x05'::bytea"

// External declaration
Datum database_column_datum (dbvm_t *vm, int index);

// MARK: - Context Management -

// Global context stored per backend
static cloudsync_context *pg_cloudsync_context = NULL;

static void cloudsync_pg_context_init (cloudsync_context *data) {
    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %d", spi_rc)));
    }

    PG_TRY();
    {
        if (cloudsync_config_exists(data)) {
            if (cloudsync_context_init(data) == NULL) {
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("An error occurred while trying to initialize context")));
            }

            // update schema hash if upgrading from an older version
            if (dbutils_settings_check_version(data, NULL) != 0) {
                cloudsync_update_schema_hash(data);
            }

            // make sure to update internal version to current version
            dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_LIBVERSION, CLOUDSYNC_VERSION);
        }

        if (SPI_tuptable) {
            SPI_freetuptable(SPI_tuptable);
            SPI_tuptable = NULL;
        }
        SPI_finish();
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();
}

// Get or create the CloudSync context for this backend
static cloudsync_context *get_cloudsync_context(void) {
    if (pg_cloudsync_context == NULL) {
        // Create context - db_t is not used in PostgreSQL mode
        MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
        cloudsync_context *data = cloudsync_context_create(NULL);
        MemoryContextSwitchTo(old);
        if (!data) {
            ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("Not enough memory to create a database context")));
        }
        // Set early to prevent infinite recursion: during init, SQL queries may call
        // cloudsync_schema() which calls get_cloudsync_context(). Without early assignment,
        // each nested call sees NULL and tries to reinitialize, causing stack overflow.
        pg_cloudsync_context = data;
        PG_TRY();
        {
            cloudsync_pg_context_init(data);
        }
        PG_CATCH();
        {
            pg_cloudsync_context = NULL;
            cloudsync_context_free(data);
            PG_RE_THROW();
        }
        PG_END_TRY();
    }

    return pg_cloudsync_context;
}

// MARK: - Extension Entry Points -

void _PG_init (void) {
    // Extension initialization
    // SPI will be connected per-function call
    elog(DEBUG1, "CloudSync extension loading");
    
    // Initialize memory debugger (NOOP in production)
    cloudsync_memory_init(1);

    // Set fractional-indexing allocator to use cloudsync memory
    block_init_allocator();
}

void _PG_fini (void) {
    // Extension cleanup
    elog(DEBUG1, "CloudSync extension unloading");

    // Free global context if it exists
    if (pg_cloudsync_context) {
        cloudsync_context_free(pg_cloudsync_context);
        pg_cloudsync_context = NULL;
    }
}

// MARK: - Public SQL Functions -

// cloudsync_version() - Returns extension version
PG_FUNCTION_INFO_V1(cloudsync_version);
Datum cloudsync_version (PG_FUNCTION_ARGS) {
    UNUSED_PARAMETER(fcinfo);
    PG_RETURN_TEXT_P(cstring_to_text(CLOUDSYNC_VERSION));
}

// cloudsync_siteid() - Get site identifier (UUID)
PG_FUNCTION_INFO_V1(pg_cloudsync_siteid);
Datum pg_cloudsync_siteid (PG_FUNCTION_ARGS) {
    UNUSED_PARAMETER(fcinfo);

    cloudsync_context *data = get_cloudsync_context();
    const void *siteid = cloudsync_siteid(data);

    if (!siteid) {
        PG_RETURN_NULL();
    }

    // Return as bytea (binary UUID)
    bytea *result = (bytea *)palloc(VARHDRSZ + UUID_LEN);
    SET_VARSIZE(result, VARHDRSZ + UUID_LEN);
    memcpy(VARDATA(result), siteid, UUID_LEN);

    PG_RETURN_BYTEA_P(result);
}

// cloudsync_uuid() - Generate a new UUID
PG_FUNCTION_INFO_V1(cloudsync_uuid);
Datum cloudsync_uuid (PG_FUNCTION_ARGS) {
    UNUSED_PARAMETER(fcinfo);

    uint8_t uuid_bytes[UUID_LEN];
    cloudsync_uuid_v7(uuid_bytes);

    // Format as text with dashes (matches SQLite implementation)
    char uuid_str[UUID_STR_MAXLEN];
    cloudsync_uuid_v7_stringify(uuid_bytes, uuid_str, true);

    // Parse into PostgreSQL UUID type
    Datum uuid_datum = DirectFunctionCall1(uuid_in, CStringGetDatum(uuid_str));
    
    PG_RETURN_DATUM(uuid_datum);
}

// cloudsync_db_version() - Get current database version
PG_FUNCTION_INFO_V1(cloudsync_db_version);
Datum cloudsync_db_version (PG_FUNCTION_ARGS) {
    UNUSED_PARAMETER(fcinfo);

    cloudsync_context *data = get_cloudsync_context();
    int64_t version = 0;
    bool spi_connected = false;

    // Connect SPI for database operations
    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %d", spi_rc)));
    }
    spi_connected = true;

    PG_TRY();
    {
        int rc = cloudsync_dbversion_check_uptodate(data);
        if (rc != DBRES_OK) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("Unable to retrieve db_version (%s)", database_errmsg(data))));
        }

        version = cloudsync_dbversion(data);
    }
    PG_CATCH();
    {
        if (spi_connected) SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    if (spi_connected) SPI_finish();
    PG_RETURN_INT64(version);
}

// cloudsync_db_version_next([merging_version]) - Get next database version
PG_FUNCTION_INFO_V1(cloudsync_db_version_next);
Datum cloudsync_db_version_next (PG_FUNCTION_ARGS) {
    cloudsync_context *data = get_cloudsync_context();
    int64_t next_version = 0;
    bool spi_connected = false;

    int64_t merging_version = CLOUDSYNC_VALUE_NOTSET;
    if (PG_NARGS() == 1 && !PG_ARGISNULL(0)) {
        merging_version = PG_GETARG_INT64(0);
    }

    // Connect SPI for database operations
    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %d", spi_rc)));
    }
    spi_connected = true;

    PG_TRY();
    {
        next_version = cloudsync_dbversion_next(data, merging_version);
    }
    PG_CATCH();
    {
        if (spi_connected) SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    if (spi_connected) SPI_finish();
    PG_RETURN_INT64(next_version);
}

// MARK: - Table Initialization -

// Internal helper for cloudsync_init - replicates dbsync_init logic from SQLite
// Returns site_id as bytea on success, raises error on failure
static bytea *cloudsync_init_internal (cloudsync_context *data, const char *table, const char *algo, bool skip_int_pk_check) {
    bytea *result = NULL;

    // Connect SPI for database operations
    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %d", spi_rc)));
    }

    PG_TRY();
    {
        // Begin savepoint for transactional init
        int rc = database_begin_savepoint(data, "cloudsync_init");
        if (rc != DBRES_OK) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("Unable to create cloudsync_init savepoint: %s", database_errmsg(data))));
        }

        // Initialize table for sync
        rc = cloudsync_init_table(data, table, algo, skip_int_pk_check);
        ereport(DEBUG1, (errmsg("cloudsync_init_internal cloudsync_init_table %d", rc)));

        if (rc == DBRES_OK) {
            rc = database_commit_savepoint(data, "cloudsync_init");
            if (rc != DBRES_OK) {
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("Unable to release cloudsync_init savepoint: %s", database_errmsg(data))));
            }

            // Persist schema to settings now that the settings table exists
            const char *cur_schema = cloudsync_schema(data);
            if (cur_schema) {
                dbutils_settings_set_key_value(data, "schema", cur_schema);
            }
        } else {
            // In case of error, rollback transaction
            char err[1024];
            snprintf(err, sizeof(err), "%s", cloudsync_errmsg(data));
            database_rollback_savepoint(data, "cloudsync_init");
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s", err)));
        }

        cloudsync_update_schema_hash(data);

        // Build site_id as bytea to return
        // Use SPI_palloc so the allocation survives SPI_finish
        result = (bytea *)SPI_palloc(UUID_LEN + VARHDRSZ);
        SET_VARSIZE(result, UUID_LEN + VARHDRSZ);
        memcpy(VARDATA(result), cloudsync_siteid(data), UUID_LEN);

        SPI_finish();
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    return result;
}

// cloudsync_init(table_name, [algo], [skip_int_pk_check]) - Initialize table for sync
// Supports 1-3 arguments with defaults: algo=NULL, skip_int_pk_check=false
PG_FUNCTION_INFO_V1(cloudsync_init);
Datum cloudsync_init (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("table_name cannot be NULL")));
    }

    const char *table = text_to_cstring(PG_GETARG_TEXT_PP(0));

    // Default values
    const char *algo = NULL;
    bool skip_int_pk_check = false;

    // Handle optional arguments
    int nargs = PG_NARGS();

    if (nargs >= 2 && !PG_ARGISNULL(1)) {
        algo = text_to_cstring(PG_GETARG_TEXT_PP(1));
    }

    if (nargs >= 3 && !PG_ARGISNULL(2)) {
        skip_int_pk_check = PG_GETARG_BOOL(2);
    }

    cloudsync_context *data = get_cloudsync_context();

    // Call internal helper and return site_id as bytea
    bytea *result = cloudsync_init_internal(data, table, algo, skip_int_pk_check);
    PG_RETURN_BYTEA_P(result);
}

// MARK: - Table Enable/Disable Functions -

// Internal helper for enable/disable
static void cloudsync_enable_disable (const char *table_name, bool value) {
    cloudsync_context *data = get_cloudsync_context();
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (table) table_set_enabled(table, value);
}

// cloudsync_enable - Enable sync for a table
PG_FUNCTION_INFO_V1(cloudsync_enable);
Datum cloudsync_enable (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("table_name cannot be NULL")));
    }

    const char *table = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_enable_disable(table, true);
    PG_RETURN_BOOL(true);
}

// cloudsync_disable - Disable sync for a table
PG_FUNCTION_INFO_V1(cloudsync_disable);
Datum cloudsync_disable (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("table_name cannot be NULL")));
    }

    const char *table = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_enable_disable(table, false);
    PG_RETURN_BOOL(true);
}

// cloudsync_is_enabled - Check if table is sync-enabled
PG_FUNCTION_INFO_V1(cloudsync_is_enabled);
Datum cloudsync_is_enabled (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("table_name cannot be NULL")));
    }

    cloudsync_context *data = get_cloudsync_context();
    const char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_table_context *table = table_lookup(data, table_name);

    bool result = (table && table_enabled(table));
    PG_RETURN_BOOL(result);
}

// MARK: - Cleanup and Termination -

// cloudsync_cleanup - Cleanup orphaned metadata for a table
PG_FUNCTION_INFO_V1(pg_cloudsync_cleanup);
Datum pg_cloudsync_cleanup (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("table_name cannot be NULL")));
    }

    const char *table = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_context *data = get_cloudsync_context();
    int rc = DBRES_OK;
    bool spi_connected = false;

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %d", spi_rc)));
    }
    spi_connected = true;

    PG_TRY();
    {
        rc = cloudsync_cleanup(data, table);
    }
    PG_CATCH();
    {
        if (spi_connected) SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    if (SPI_tuptable) {
        SPI_freetuptable(SPI_tuptable);
        SPI_tuptable = NULL;
    }
    if (spi_connected) SPI_finish();
    if (rc != DBRES_OK) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s", cloudsync_errmsg(data))));
    }

    PG_RETURN_BOOL(true);
}

// cloudsync_terminate - Terminate CloudSync
PG_FUNCTION_INFO_V1(pg_cloudsync_terminate);
Datum pg_cloudsync_terminate (PG_FUNCTION_ARGS) {
    UNUSED_PARAMETER(fcinfo);

    cloudsync_context *data = get_cloudsync_context();
    int rc = DBRES_OK;
    bool spi_connected = false;

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %d", spi_rc)));
    }
    spi_connected = true;

    PG_TRY();
    {
        rc = cloudsync_terminate(data);
    }
    PG_CATCH();
    {
        if (spi_connected) SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    if (spi_connected) SPI_finish();
    PG_RETURN_BOOL(rc == DBRES_OK);
}

// MARK: - Settings Functions -

// cloudsync_set - Set global configuration
PG_FUNCTION_INFO_V1(cloudsync_set);
Datum cloudsync_set (PG_FUNCTION_ARGS) {
    const char *key = NULL;
    const char *value = NULL;

    if (!PG_ARGISNULL(0)) {
        key = text_to_cstring(PG_GETARG_TEXT_PP(0));
    }
    if (!PG_ARGISNULL(1)) {
        value = text_to_cstring(PG_GETARG_TEXT_PP(1));
    }

    // Silently fail if key is NULL (matches SQLite behavior)
    if (key == NULL) {
        PG_RETURN_BOOL(true);
    }

    cloudsync_context *data = get_cloudsync_context();
    bool spi_connected = false;

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("SPI_connect failed: %d", spi_rc)));
    }
    spi_connected = true;

    PG_TRY();
    {
        dbutils_settings_set_key_value(data, key, value);
    }
    PG_CATCH();
    {
        if (spi_connected) SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    if (spi_connected) SPI_finish();
    PG_RETURN_BOOL(true);
}

// cloudsync_set_table - Set table-level configuration
PG_FUNCTION_INFO_V1(cloudsync_set_table);
Datum cloudsync_set_table (PG_FUNCTION_ARGS) {
    const char *tbl = NULL;
    const char *key = NULL;
    const char *value = NULL;

    if (!PG_ARGISNULL(0)) {
        tbl = text_to_cstring(PG_GETARG_TEXT_PP(0));
    }
    if (!PG_ARGISNULL(1)) {
        key = text_to_cstring(PG_GETARG_TEXT_PP(1));
    }
    if (!PG_ARGISNULL(2)) {
        value = text_to_cstring(PG_GETARG_TEXT_PP(2));
    }

    cloudsync_context *data = get_cloudsync_context();
    bool spi_connected = false;

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %d", spi_rc)));
    }
    spi_connected = true;

    PG_TRY();
    {
        dbutils_table_settings_set_key_value(data, tbl, "*", key, value);
    }
    PG_CATCH();
    {
        if (spi_connected) SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    if (spi_connected) SPI_finish();
    PG_RETURN_BOOL(true);
}

// cloudsync_set_column - Set column-level configuration
PG_FUNCTION_INFO_V1(cloudsync_set_column);
Datum cloudsync_set_column (PG_FUNCTION_ARGS) {
    const char *tbl = NULL;
    const char *col = NULL;
    const char *key = NULL;
    const char *value = NULL;

    if (!PG_ARGISNULL(0)) {
        tbl = text_to_cstring(PG_GETARG_TEXT_PP(0));
    }
    if (!PG_ARGISNULL(1)) {
        col = text_to_cstring(PG_GETARG_TEXT_PP(1));
    }
    if (!PG_ARGISNULL(2)) {
        key = text_to_cstring(PG_GETARG_TEXT_PP(2));
    }
    if (!PG_ARGISNULL(3)) {
        value = text_to_cstring(PG_GETARG_TEXT_PP(3));
    }

    cloudsync_context *data = get_cloudsync_context();
    bool spi_connected = false;

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("SPI_connect failed: %d", spi_rc)));
    }
    spi_connected = true;

    PG_TRY();
    {
        // Handle block column setup: cloudsync_set_column('tbl', 'col', 'algo', 'block')
        if (key && value && strcmp(key, "algo") == 0 && strcmp(value, "block") == 0) {
            int rc = cloudsync_setup_block_column(data, tbl, col, NULL);
            if (rc != DBRES_OK) {
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s", cloudsync_errmsg(data))));
            }
        } else {
            // Handle delimiter setting: cloudsync_set_column('tbl', 'col', 'delimiter', '\n\n')
            if (key && strcmp(key, "delimiter") == 0) {
                cloudsync_table_context *table = table_lookup(data, tbl);
                if (table) {
                    int col_idx = table_col_index(table, col);
                    if (col_idx >= 0 && table_col_algo(table, col_idx) == col_algo_block) {
                        table_set_col_delimiter(table, col_idx, value);
                    }
                }
            }
            dbutils_table_settings_set_key_value(data, tbl, col, key, value);
        }
    }
    PG_CATCH();
    {
        if (spi_connected) SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    if (spi_connected) SPI_finish();
    PG_RETURN_BOOL(true);
}

// MARK: - Row Filter -

// cloudsync_set_filter - Set a row-level filter for conditional sync
PG_FUNCTION_INFO_V1(cloudsync_set_filter);
Datum cloudsync_set_filter (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("cloudsync_set_filter: table and filter expression required")));
    }

    const char *tbl = text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *filter_expr = text_to_cstring(PG_GETARG_TEXT_PP(1));

    cloudsync_context *data = get_cloudsync_context();
    bool spi_connected = false;

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %d", spi_rc)));
    }
    spi_connected = true;

    PG_TRY();
    {
        // Store filter in table settings
        dbutils_table_settings_set_key_value(data, tbl, "*", "filter", filter_expr);

        // Read current algo
        table_algo algo = dbutils_table_settings_get_algo(data, tbl);
        if (algo == table_algo_none) algo = table_algo_crdt_cls;

        // Drop triggers
        database_delete_triggers(data, tbl);

        // Reconnect SPI so that the catalog changes from DROP are visible
        SPI_finish();
        spi_connected = false;
        spi_rc = SPI_connect();
        if (spi_rc != SPI_OK_CONNECT) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %d", spi_rc)));
        }
        spi_connected = true;

        // Recreate triggers with filter
        int rc = database_create_triggers(data, tbl, algo, filter_expr);
        if (rc != DBRES_OK) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("cloudsync_set_filter: error recreating triggers")));
        }
    }
    PG_CATCH();
    {
        if (spi_connected) SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    if (spi_connected) SPI_finish();
    PG_RETURN_BOOL(true);
}

// cloudsync_clear_filter - Remove the row-level filter for a table
PG_FUNCTION_INFO_V1(cloudsync_clear_filter);
Datum cloudsync_clear_filter (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("cloudsync_clear_filter: table name required")));
    }

    const char *tbl = text_to_cstring(PG_GETARG_TEXT_PP(0));

    cloudsync_context *data = get_cloudsync_context();
    bool spi_connected = false;

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %d", spi_rc)));
    }
    spi_connected = true;

    PG_TRY();
    {
        // Remove filter from settings
        dbutils_table_settings_set_key_value(data, tbl, "*", "filter", NULL);

        // Read current algo
        table_algo algo = dbutils_table_settings_get_algo(data, tbl);
        if (algo == table_algo_none) algo = table_algo_crdt_cls;

        // Drop triggers
        database_delete_triggers(data, tbl);

        // Reconnect SPI so that the catalog changes from DROP are visible
        SPI_finish();
        spi_connected = false;
        spi_rc = SPI_connect();
        if (spi_rc != SPI_OK_CONNECT) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %d", spi_rc)));
        }
        spi_connected = true;

        // Recreate triggers without filter
        int rc = database_create_triggers(data, tbl, algo, NULL);
        if (rc != DBRES_OK) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("cloudsync_clear_filter: error recreating triggers")));
        }
    }
    PG_CATCH();
    {
        if (spi_connected) SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    if (spi_connected) SPI_finish();
    PG_RETURN_BOOL(true);
}

// MARK: - Schema Alteration -

// cloudsync_begin_alter - Begin schema alteration
PG_FUNCTION_INFO_V1(pg_cloudsync_begin_alter);
Datum pg_cloudsync_begin_alter (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("table_name cannot be NULL")));
    }

    const char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_context *data = get_cloudsync_context();
    int rc = DBRES_OK;

    if (SPI_connect() != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed")));
    }

    PG_TRY();
    {
        database_begin_savepoint(data, "cloudsync_alter");
        rc = cloudsync_begin_alter(data, table_name);
        if (rc != DBRES_OK) {
            database_rollback_savepoint(data, "cloudsync_alter");
        }
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    SPI_finish();
    if (rc != DBRES_OK) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("%s", cloudsync_errmsg(data))));
    }
    PG_RETURN_BOOL(true);
}

// cloudsync_commit_alter - Commit schema alteration
//
// This wrapper manages SPI in two phases to avoid the PostgreSQL warning
// "subtransaction left non-empty SPI stack". The subtransaction was opened
// by a prior cloudsync_begin_alter call, so SPI_connect() here creates a
// connection at the subtransaction level. We must disconnect SPI before
// cloudsync_commit_alter releases that subtransaction, then reconnect
// for post-commit work (cloudsync_update_schema_hash).
// Prepared statements survive SPI_finish via SPI_keepplan/TopMemoryContext.
PG_FUNCTION_INFO_V1(pg_cloudsync_commit_alter);
Datum pg_cloudsync_commit_alter (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("table_name cannot be NULL")));
    }

    const char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_context *data = get_cloudsync_context();
    int rc = DBRES_OK;

    // Phase 1: SPI work before savepoint release
    if (SPI_connect() != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed")));
    }

    PG_TRY();
    {
        rc = cloudsync_commit_alter(data, table_name);
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    // Disconnect SPI before savepoint boundary
    SPI_finish();

    if (rc != DBRES_OK) {
        // Rollback savepoint (SPI disconnected, no warning)
        database_rollback_savepoint(data, "cloudsync_alter");
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s", cloudsync_errmsg(data))));
    }

    // Release savepoint (SPI disconnected, no warning)
    rc = database_commit_savepoint(data, "cloudsync_alter");
    if (rc != DBRES_OK) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("Unable to release cloudsync_alter savepoint: %s", database_errmsg(data))));
    }

    // Phase 2: reconnect SPI for post-commit work
    if (SPI_connect() != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed after savepoint release")));
    }

    PG_TRY();
    {
        cloudsync_update_schema_hash(data);
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    SPI_finish();
    PG_RETURN_BOOL(true);
}

// MARK: - Payload Functions -

// Aggregate function: cloudsync_payload_encode transition function
PG_FUNCTION_INFO_V1(cloudsync_payload_encode_transfn);
Datum cloudsync_payload_encode_transfn (PG_FUNCTION_ARGS) {
    MemoryContext aggContext;
    cloudsync_payload_context *payload = NULL;

    if (!AggCheckCallContext(fcinfo, &aggContext)) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("cloudsync_payload_encode_transfn called in non-aggregate context")));
    }

    // Get or allocate aggregate state
    if (PG_ARGISNULL(0)) {
        MemoryContext oldContext = MemoryContextSwitchTo(aggContext);
        payload = (cloudsync_payload_context *)palloc0(cloudsync_payload_context_size(NULL));
        MemoryContextSwitchTo(oldContext);
    } else {
        payload = (cloudsync_payload_context *)PG_GETARG_POINTER(0);
    }

    int argc = 0;
    cloudsync_context *data = get_cloudsync_context();
    pgvalue_t **argv = pgvalues_from_args(fcinfo, 1, &argc);

    // Wrap variadic args into pgvalue_t so pk/payload helpers can read types safely.
    if (argc > 0) {
        int rc = cloudsync_payload_encode_step(payload, data, argc, (dbvalue_t **)argv);
        if (rc != DBRES_OK) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s", cloudsync_errmsg(data))));
        }
    }

    // payload_encode_step does not retain pgvalue_t*, free transient wrappers now
    for (int i = 0; i < argc; i++) {
        pgvalue_free(argv[i]);
    }
    if (argv) cloudsync_memory_free(argv);

    PG_RETURN_POINTER(payload);
}

// Aggregate function: cloudsync_payload_encode finalize function
PG_FUNCTION_INFO_V1(cloudsync_payload_encode_finalfn);
Datum cloudsync_payload_encode_finalfn (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }

    cloudsync_payload_context *payload = (cloudsync_payload_context *)PG_GETARG_POINTER(0);
    cloudsync_context *data = get_cloudsync_context();

    int rc = cloudsync_payload_encode_final(payload, data);
    if (rc != DBRES_OK) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s", cloudsync_errmsg(data))));
    }

    int64_t blob_size = 0;
    char *blob = cloudsync_payload_blob(payload, &blob_size, NULL);

    if (!blob) {
        PG_RETURN_NULL();
    }

    bytea *result = (bytea *)palloc(VARHDRSZ + blob_size);
    SET_VARSIZE(result, VARHDRSZ + blob_size);
    memcpy(VARDATA(result), blob, blob_size);

    cloudsync_memory_free(blob);

    PG_RETURN_BYTEA_P(result);
}

// Payload decode - Apply changes from payload
PG_FUNCTION_INFO_V1(cloudsync_payload_decode);
Datum cloudsync_payload_decode (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("payload cannot be NULL")));
    }

    bytea *payload_data = PG_GETARG_BYTEA_P(0);
    int blen = VARSIZE(payload_data) - VARHDRSZ;

    // Sanity check payload size
    size_t header_size = 0;
    cloudsync_payload_context_size(&header_size);
    if (blen < (int)header_size) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("Invalid payload size")));
    }

    const char *payload = VARDATA(payload_data);
    cloudsync_context *data = get_cloudsync_context();
    int rc = DBRES_OK;
    int nrows = 0;
    bool spi_connected = false;

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %d", spi_rc)));
    }
    spi_connected = true;

    PG_TRY();
    {
        rc = cloudsync_payload_apply(data, payload, blen, &nrows);
    }
    PG_CATCH();
    {
        if (spi_connected) SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    if (spi_connected) SPI_finish();
    if (rc != DBRES_OK) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s", cloudsync_errmsg(data))));
    }
    PG_RETURN_INT32(nrows);
}

// Alias for payload_decode
PG_FUNCTION_INFO_V1(pg_cloudsync_payload_apply);
Datum pg_cloudsync_payload_apply (PG_FUNCTION_ARGS) {
    return cloudsync_payload_decode(fcinfo);
}

// MARK: - Private/Internal Functions -

typedef struct cloudsync_pg_cleanup_state {
    char *pk;
    char pk_buffer[1024];
    pgvalue_t **argv;
    int argc;
    bool spi_connected;
} cloudsync_pg_cleanup_state;

static void cloudsync_pg_cleanup(int code, Datum arg) {
    cloudsync_pg_cleanup_state *state = (cloudsync_pg_cleanup_state *)DatumGetPointer(arg);
    if (!state) return;
    UNUSED_PARAMETER(code);

    if (state->pk && state->pk != state->pk_buffer) {
        cloudsync_memory_free(state->pk);
    }
    state->pk = NULL;

    for (int i = 0; i < state->argc; i++) {
        pgvalue_free(state->argv[i]);
    }
    if (state->argv) cloudsync_memory_free(state->argv);
    state->argv = NULL;
    state->argc = 0;

    if (state->spi_connected) {
        SPI_finish();
        state->spi_connected = false;
    }
}

// cloudsync_is_sync - Check if table has sync metadata
PG_FUNCTION_INFO_V1(cloudsync_is_sync);
Datum cloudsync_is_sync (PG_FUNCTION_ARGS) {
    cloudsync_context *data = get_cloudsync_context();

    if (cloudsync_insync(data)) {
        PG_RETURN_BOOL(true);
    }

    if (PG_ARGISNULL(0)) {
        PG_RETURN_BOOL(false);
    }

    const char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_table_context *table = table_lookup(data, table_name);

    bool result = (table && (table_enabled(table) == 0));
    PG_RETURN_BOOL(result);
}

typedef struct cloudsync_update_payload {
    pgvalue_t       *table_name;
    pgvalue_t       **new_values;
    pgvalue_t       **old_values;
    int             count;
    int             capacity;
    MemoryContext   mcxt;
    // Context-owned callback info for early-exit cleanup.
    // We null the payload pointer on normal finalization to avoid double-free.
    struct cloudsync_mcxt_cb_info *mcxt_cb_info;
} cloudsync_update_payload;

static void cloudsync_update_payload_free (cloudsync_update_payload *payload);

typedef struct cloudsync_mcxt_cb_info {
    MemoryContext               mcxt;
    const char                  *name;
    cloudsync_update_payload    *payload;
} cloudsync_mcxt_cb_info;

static void cloudsync_mcxt_reset_cb (void *arg) {
    cloudsync_mcxt_cb_info *info = (cloudsync_mcxt_cb_info *)arg;
    if (!info) return;
    if (!info->payload) return;
  
    // Context reset means the aggregate state would be lost; clean it here.
    cloudsync_update_payload_free(info->payload);
    info->payload = NULL;
}

static void cloudsync_update_payload_free (cloudsync_update_payload *payload) {
    if (!payload) return;

    if (payload->mcxt_cb_info) {
        // Normal finalize path: prevent the reset callback from double-free.
        payload->mcxt_cb_info->payload = NULL;
    }

    for (int i = 0; i < payload->count; i++) {
        pgvalue_free(payload->new_values[i]);
        pgvalue_free(payload->old_values[i]);
    }
    if (payload->new_values) pfree(payload->new_values);
    if (payload->old_values) pfree(payload->old_values);
    if (payload->table_name) pgvalue_free(payload->table_name);

    payload->new_values = NULL;
    payload->old_values = NULL;
    payload->table_name = NULL;
    payload->count = 0;
    payload->capacity = 0;
    payload->mcxt = NULL;
    payload->mcxt_cb_info = NULL;
}

static bool cloudsync_update_payload_append (cloudsync_update_payload *payload, pgvalue_t *table_name, pgvalue_t *new_value, pgvalue_t *old_value) {
    if (!payload) return false;
    if (!payload->mcxt || !MemoryContextIsValid(payload->mcxt)) {
        elog(DEBUG1, "cloudsync_update_payload_append invalid payload context payload=%p mcxt=%p", payload, payload->mcxt);
        return false;
    }
    if (payload->count < 0 || payload->capacity < 0) {
        elog(DEBUG1, "cloudsync_update_payload_append invalid counters payload=%p count=%d cap=%d", payload, payload->count, payload->capacity);
        return false;
    }

    if (payload->count >= payload->capacity) {
        int newcap = payload->capacity ? payload->capacity * 2 : 128;
        elog(DEBUG1, "cloudsync_update_payload_append newcap=%d", newcap);
        MemoryContext old = MemoryContextSwitchTo(payload->mcxt);
        if (payload->capacity == 0) {
            payload->new_values = (pgvalue_t **)palloc0(newcap * sizeof(*payload->new_values));
            payload->old_values = (pgvalue_t **)palloc0(newcap * sizeof(*payload->old_values));
        } else {
            payload->new_values = (pgvalue_t **)repalloc(payload->new_values, newcap * sizeof(*payload->new_values));
            payload->old_values = (pgvalue_t **)repalloc(payload->old_values, newcap * sizeof(*payload->old_values));
        }
        payload->capacity = newcap;
        MemoryContextSwitchTo(old);
    }

    if (payload->count >= payload->capacity) {
        elog(DEBUG1,
            "cloudsync_update_payload_append count>=capacity payload=%p count=%d "
            "cap=%d new_values=%p old_values=%p",
            payload, payload->count, payload->capacity, payload->new_values,
            payload->old_values);
        return false;
    }

    int index = payload->count;
    if (payload->table_name == NULL) {
        payload->table_name = table_name;
    } else {
        // Compare within the payload context so any lazy text/detoast buffers
        // are allocated in a stable context (not ExprContext).
        MemoryContext old = MemoryContextSwitchTo(payload->mcxt);
        int cmp = dbutils_value_compare((dbvalue_t *)payload->table_name, (dbvalue_t *)table_name);
        MemoryContextSwitchTo(old);
        if (cmp != 0) {
            return false;
        }
        pgvalue_free(table_name);
    }

    payload->new_values[index] = new_value;
    payload->old_values[index] = old_value;
    payload->count++;

    return true;
}

// cloudsync_seq - Get sequence number
PG_FUNCTION_INFO_V1(cloudsync_seq);
Datum cloudsync_seq (PG_FUNCTION_ARGS) {
    UNUSED_PARAMETER(fcinfo);

    cloudsync_context *data = get_cloudsync_context();
    int seq = cloudsync_bumpseq(data);

    PG_RETURN_INT32(seq);
}

// cloudsync_pk_encode - Encode primary key from variadic arguments
PG_FUNCTION_INFO_V1(cloudsync_pk_encode);
Datum cloudsync_pk_encode (PG_FUNCTION_ARGS) {
    int argc = 0;
    pgvalue_t **argv = NULL;

    // Signature is VARIADIC "any", so extract all arguments starting from index 0
    argv = pgvalues_from_args(fcinfo, 0, &argc);
    if (!argv || argc == 0) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("cloudsync_pk_encode requires at least one primary key value")));
    }

    // Normalize all values to text for consistent PK encoding
    // (PG triggers cast PK values to ::text; SQL callers must match)
    pgvalues_normalize_to_text(argv, argc);

    size_t pklen = 0;
    char *encoded = pk_encode_prikey((dbvalue_t **)argv, argc, NULL, &pklen);
    if (!encoded || encoded == PRIKEY_NULL_CONSTRAINT_ERROR) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("cloudsync_pk_encode failed to encode primary key")));
    }

    bytea *result = (bytea *)palloc(pklen + VARHDRSZ);
    SET_VARSIZE(result, pklen + VARHDRSZ);
    memcpy(VARDATA(result), encoded, pklen);
    cloudsync_memory_free(encoded);

    for (int i = 0; i < argc; i++) {
        pgvalue_free(argv[i]);
    }
    if (argv) cloudsync_memory_free(argv);

    PG_RETURN_BYTEA_P(result);
}

typedef struct cloudsync_pk_decode_ctx {
    int target_index;
    text *result;
    bool found;
} cloudsync_pk_decode_ctx;

static int cloudsync_pk_decode_set_result (void *xdata, int index, int type, int64_t ival, double dval, char *pval) {
    cloudsync_pk_decode_ctx *ctx = (cloudsync_pk_decode_ctx *)xdata;
    if (!ctx || ctx->found || (index + 1) != ctx->target_index) return DBRES_OK;

    switch (type) {
        case DBTYPE_INTEGER: {
            char *cstr = DatumGetCString(DirectFunctionCall1(int8out, Int64GetDatum(ival)));
            ctx->result = cstring_to_text(cstr);
            pfree(cstr);
            break;
        }
        case DBTYPE_FLOAT: {
            char *cstr = DatumGetCString(DirectFunctionCall1(float8out, Float8GetDatum(dval)));
            ctx->result = cstring_to_text(cstr);
            pfree(cstr);
            break;
        }
        case DBTYPE_TEXT: {
            ctx->result = cstring_to_text_with_len(pval, (int)ival);
            break;
        }
        case DBTYPE_BLOB: {
            bytea *ba = (bytea *)palloc(ival + VARHDRSZ);
            SET_VARSIZE(ba, ival + VARHDRSZ);
            memcpy(VARDATA(ba), pval, (size_t)ival);
            char *cstr = DatumGetCString(DirectFunctionCall1(byteaout, PointerGetDatum(ba)));
            ctx->result = cstring_to_text(cstr);
            pfree(cstr);
            pfree(ba);
            break;
        }
        case DBTYPE_NULL:
        default:
            ctx->result = NULL;
            break;
    }

    ctx->found = true;
    return DBRES_OK;
}

// cloudsync_pk_decode - Decode primary key component at given index
PG_FUNCTION_INFO_V1(cloudsync_pk_decode);
Datum cloudsync_pk_decode (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1)) {
        PG_RETURN_NULL();
    }

    bytea *ba = PG_GETARG_BYTEA_P(0);
    int index = PG_GETARG_INT32(1);
    if (index < 1) PG_RETURN_NULL();

    cloudsync_pk_decode_ctx ctx = {
        .target_index = index,
        .result = NULL,
        .found = false
    };

    char *buffer = VARDATA(ba);
    size_t blen = (size_t)(VARSIZE(ba) - VARHDRSZ);
    if (pk_decode_prikey(buffer, blen, cloudsync_pk_decode_set_result, &ctx) < 0) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("cloudsync_pk_decode failed to decode primary key")));
    }

    if (!ctx.found || ctx.result == NULL) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(ctx.result);
}

// cloudsync_insert - Internal insert handler
// Signature: cloudsync_insert(table_name text, VARIADIC pk_values anyarray)
PG_FUNCTION_INFO_V1(cloudsync_insert);
Datum cloudsync_insert (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("table_name cannot be NULL")));
    }

    const char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_context *data = get_cloudsync_context();
    cloudsync_pg_cleanup_state cleanup = {0};

    // Connect SPI for database operations
    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %d", spi_rc)));
    }
    cleanup.spi_connected = true;

    PG_ENSURE_ERROR_CLEANUP(cloudsync_pg_cleanup, PointerGetDatum(&cleanup));
    {
        // Lookup table (load from settings if needed)
        cloudsync_table_context *table = table_lookup(data, table_name);
        if (!table) {
            char meta_name[1024];
            snprintf(meta_name, sizeof(meta_name), "%s_cloudsync", table_name);
            if (!database_table_exists(data, meta_name, cloudsync_schema(data))) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("Unable to retrieve table name %s in cloudsync_insert", table_name)));
            }

            table_algo algo = dbutils_table_settings_get_algo(data, table_name);
            if (algo == table_algo_none) algo = table_algo_crdt_cls;
            if (!table_add_to_context(data, algo, table_name)) {
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("Unable to load table context for %s", table_name)));
            }

            table = table_lookup(data, table_name);
            if (!table) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("Unable to retrieve table name %s in cloudsync_insert", table_name)));
            }
        }

        // Extract PK values from VARIADIC "any" (args starting from index 1)
        cleanup.argv = pgvalues_from_args(fcinfo, 1, &cleanup.argc);

        // Normalize PK values to text for consistent encoding
        pgvalues_normalize_to_text(cleanup.argv, cleanup.argc);

        // Verify we have the correct number of PK columns
        int expected_pks = table_count_pks(table);
        if (cleanup.argc != expected_pks) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("Expected %d primary key values, got %d", expected_pks, cleanup.argc)));
        }

        // Encode the primary key values into a buffer
        size_t pklen = sizeof(cleanup.pk_buffer);
        cleanup.pk = pk_encode_prikey((dbvalue_t **)cleanup.argv, cleanup.argc, cleanup.pk_buffer, &pklen);

        if (!cleanup.pk) {
            ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("Not enough memory to encode the primary key(s)")));
        }
        if (cleanup.pk == PRIKEY_NULL_CONSTRAINT_ERROR) {
            cleanup.pk = NULL;
            ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg("Insert aborted because primary key in table %s contains NULL values", table_name)));
        }

        // Compute the next database version for tracking changes
        int64_t db_version = cloudsync_dbversion_next(data, CLOUDSYNC_VALUE_NOTSET);

        // Check if a row with the same primary key already exists
        // (if so, this might be a previously deleted sentinel)
        bool pk_exists = table_pk_exists(table, cleanup.pk, pklen);
        int rc = DBRES_OK;

        if (table_count_cols(table) == 0) {
            // If there are no columns other than primary keys, insert a sentinel record
            rc = local_mark_insert_sentinel_meta(table, cleanup.pk, pklen, db_version, cloudsync_bumpseq(data));
        } else if (pk_exists) {
            // If a row with the same primary key already exists, update the sentinel record
            rc = local_update_sentinel(table, cleanup.pk, pklen, db_version, cloudsync_bumpseq(data));
        }

        if (rc == DBRES_OK) {
            // Process each non-primary key column for insert or update
            for (int i = 0; i < table_count_cols(table); i++) {
                if (table_col_algo(table, i) == col_algo_block) {
                    // Block column: read value from base table, split into blocks, store each block
                    dbvm_t *val_vm = table_column_lookup(table, table_colname(table, i), false, NULL);
                    if (!val_vm) { rc = DBRES_ERROR; break; }

                    int bind_rc = pk_decode_prikey(cleanup.pk, pklen, pk_decode_bind_callback, (void *)val_vm);
                    if (bind_rc < 0) { databasevm_reset(val_vm); rc = DBRES_ERROR; break; }

                    int step_rc = databasevm_step(val_vm);
                    if (step_rc == DBRES_ROW) {
                        const char *text = database_column_text(val_vm, 0);
                        const char *delim = table_col_delimiter(table, i);
                        const char *col = table_colname(table, i);

                        block_list_t *blocks = block_split(text ? text : "", delim);
                        if (blocks) {
                            char **positions = block_initial_positions(blocks->count);
                            if (positions) {
                                for (int b = 0; b < blocks->count; b++) {
                                    char *block_cn = block_build_colname(col, positions[b]);
                                    if (block_cn) {
                                        rc = local_mark_insert_or_update_meta(table, cleanup.pk, pklen, block_cn, db_version, cloudsync_bumpseq(data));

                                        // Store block value in blocks table
                                        dbvm_t *wvm = table_block_value_write_stmt(table);
                                        if (wvm && rc == DBRES_OK) {
                                            databasevm_bind_blob(wvm, 1, cleanup.pk, (int)pklen);
                                            databasevm_bind_text(wvm, 2, block_cn, -1);
                                            databasevm_bind_text(wvm, 3, blocks->entries[b].content, -1);
                                            databasevm_step(wvm);
                                            databasevm_reset(wvm);
                                        }

                                        cloudsync_memory_free(block_cn);
                                    }
                                    cloudsync_memory_free(positions[b]);
                                    if (rc != DBRES_OK) break;
                                }
                                cloudsync_memory_free(positions);
                            }
                            block_list_free(blocks);
                        }
                    }
                    databasevm_reset(val_vm);
                    if (step_rc == DBRES_ROW || step_rc == DBRES_DONE) { if (rc == DBRES_OK) continue; }
                    if (rc != DBRES_OK) break;
                } else {
                    rc = local_mark_insert_or_update_meta(table, cleanup.pk, pklen, table_colname(table, i), db_version, cloudsync_bumpseq(data));
                    if (rc != DBRES_OK) break;
                }
            }
        }

        if (rc != DBRES_OK) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s", database_errmsg(data))));
        }
    }
    PG_END_ENSURE_ERROR_CLEANUP(cloudsync_pg_cleanup, PointerGetDatum(&cleanup));

    cloudsync_pg_cleanup(0, PointerGetDatum(&cleanup));
    PG_RETURN_BOOL(true);
}

// cloudsync_delete - Internal delete handler
// Signature: cloudsync_delete(table_name text, VARIADIC pk_values anyarray)
PG_FUNCTION_INFO_V1(cloudsync_delete);
Datum cloudsync_delete (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("table_name cannot be NULL")));
    }

    const char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_context *data = get_cloudsync_context();
    cloudsync_pg_cleanup_state cleanup = {0};

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %d", spi_rc)));
    }
    cleanup.spi_connected = true;

    PG_ENSURE_ERROR_CLEANUP(cloudsync_pg_cleanup, PointerGetDatum(&cleanup));
    {
        cloudsync_table_context *table = table_lookup(data, table_name);
        if (!table) {
            char meta_name[1024];
            snprintf(meta_name, sizeof(meta_name), "%s_cloudsync", table_name);
            if (!database_table_exists(data, meta_name, cloudsync_schema(data))) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("Unable to retrieve table name %s in cloudsync_delete", table_name)));
            }

            table_algo algo = dbutils_table_settings_get_algo(data, table_name);
            if (algo == table_algo_none) algo = table_algo_crdt_cls;
            if (!table_add_to_context(data, algo, table_name)) {
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("Unable to load table context for %s", table_name)));
            }

            table = table_lookup(data, table_name);
            if (!table) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("Unable to retrieve table name %s in cloudsync_delete", table_name)));
            }
        }

        // Extract PK values from VARIADIC "any" (args starting from index 1)
        cleanup.argv = pgvalues_from_args(fcinfo, 1, &cleanup.argc);

        // Normalize PK values to text for consistent encoding
        pgvalues_normalize_to_text(cleanup.argv, cleanup.argc);

        int expected_pks = table_count_pks(table);
        if (cleanup.argc != expected_pks) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("Expected %d primary key values, got %d", expected_pks, cleanup.argc)));
        }
        int rc = DBRES_OK;

        size_t pklen = sizeof(cleanup.pk_buffer);
        cleanup.pk = pk_encode_prikey((dbvalue_t **)cleanup.argv, cleanup.argc, cleanup.pk_buffer, &pklen);
        if (!cleanup.pk) {
            ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("Not enough memory to encode the primary key(s)")));
        }
        if (cleanup.pk == PRIKEY_NULL_CONSTRAINT_ERROR) {
            cleanup.pk = NULL;
            ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg("Delete aborted because primary key in table %s contains NULL values", table_name)));
        }

        int64_t db_version = cloudsync_dbversion_next(data, CLOUDSYNC_VALUE_NOTSET);

        rc = local_mark_delete_meta(table, cleanup.pk, pklen, db_version, cloudsync_bumpseq(data));
        if (rc == DBRES_OK) {
            rc = local_drop_meta(table, cleanup.pk, pklen);
        }

        if (rc != DBRES_OK) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s", database_errmsg(data))));
        }
    }
    PG_END_ENSURE_ERROR_CLEANUP(cloudsync_pg_cleanup, PointerGetDatum(&cleanup));

    cloudsync_pg_cleanup(0, PointerGetDatum(&cleanup));
    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(cloudsync_update_transfn);
Datum cloudsync_update_transfn (PG_FUNCTION_ARGS) {
    MemoryContext aggContext;
    MemoryContext allocContext = NULL;
    cloudsync_update_payload *payload = NULL;

    if (!AggCheckCallContext(fcinfo, &aggContext)) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("cloudsync_update_transfn called in non-aggregate context")));
    }

    allocContext = aggContext;
    if (aggContext && aggContext->name && strcmp(aggContext->name, "ExprContext") == 0 && aggContext->parent) {
        allocContext = aggContext->parent;
    }

    if (PG_ARGISNULL(0)) {
        MemoryContext old = MemoryContextSwitchTo(allocContext);
        payload = (cloudsync_update_payload *)palloc0(sizeof(cloudsync_update_payload));
        payload->mcxt = allocContext;
        MemoryContextSwitchTo(old);
    } else {
        payload = (cloudsync_update_payload *)PG_GETARG_POINTER(0);
        if (payload->mcxt == NULL || payload->mcxt != allocContext) {
            elog(DEBUG1, "cloudsync_update_transfn repairing payload context payload=%p old_mcxt=%p new_mcxt=%p", payload, payload->mcxt, allocContext);
            payload->mcxt = allocContext;
        }
  }

    if (!payload) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("cloudsync_update_transfn payload is null")));
    }

    if (payload->mcxt_cb_info && payload->mcxt_cb_info->mcxt != allocContext) {
        payload->mcxt_cb_info->payload = NULL;
        payload->mcxt_cb_info = NULL;
    }

    if (!payload->mcxt_cb_info) {
        MemoryContext old = MemoryContextSwitchTo(allocContext);
        // info and cb are automatically freed when that context is reset or deleted
        cloudsync_mcxt_cb_info *info = (cloudsync_mcxt_cb_info *)palloc0(sizeof(*info));
        info->mcxt = allocContext;
        info->name = allocContext ? allocContext->name : "<null>";
        info->payload = payload;
        MemoryContextCallback *cb = (MemoryContextCallback *)palloc0(sizeof(*cb));
        cb->func = cloudsync_mcxt_reset_cb;
        cb->arg = info;
        MemoryContextRegisterResetCallback(allocContext, cb);
        payload->mcxt_cb_info = info;
        MemoryContextSwitchTo(old);
    }

    if (payload->count < 0 || payload->capacity < 0 ||payload->count > payload->capacity) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("cloudsync_update_transfn invalid payload state: count=%d cap=%d", payload->count, payload->capacity)));
  }

  elog(DEBUG1,
       "cloudsync_update_transfn contexts current=%p name=%s agg=%p name=%s "
       "alloc=%p name=%s",
       CurrentMemoryContext,
       CurrentMemoryContext ? CurrentMemoryContext->name : "<null>", aggContext,
       aggContext ? aggContext->name : "<null>", allocContext,
       allocContext ? allocContext->name : "<null>");

    Oid table_type = get_fn_expr_argtype(fcinfo->flinfo, 1);
    bool table_null = PG_ARGISNULL(1);
    Datum table_datum = table_null ? (Datum)0 : PG_GETARG_DATUM(1);
    Oid new_type = get_fn_expr_argtype(fcinfo->flinfo, 2);
    bool new_null = PG_ARGISNULL(2);
    Datum new_datum = new_null ? (Datum)0 : PG_GETARG_DATUM(2);
    Oid old_type = get_fn_expr_argtype(fcinfo->flinfo, 3);
    bool old_null = PG_ARGISNULL(3);
    Datum old_datum = old_null ? (Datum)0 : PG_GETARG_DATUM(3);

    if (!OidIsValid(table_type) || !OidIsValid(new_type) || !OidIsValid(old_type)) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("cloudsync_update_transfn invalid argument types")));
    }

    MemoryContext old_ctx = MemoryContextSwitchTo(allocContext);
    // debug code
    // MemoryContextStats(allocContext);
    pgvalue_t *table_name = pgvalue_create(table_datum, table_type, -1, fcinfo->fncollation, table_null);
    pgvalue_t *new_value = pgvalue_create(new_datum, new_type, -1, fcinfo->fncollation, new_null);
    pgvalue_t *old_value = pgvalue_create(old_datum, old_type, -1, fcinfo->fncollation, old_null);
    if (table_name) pgvalue_ensure_detoast(table_name);
    if (new_value) pgvalue_ensure_detoast(new_value);
    if (old_value) pgvalue_ensure_detoast(old_value);
    MemoryContextSwitchTo(old_ctx);

    if (!table_name || !new_value || !old_value) {
        if (table_name) pgvalue_free(table_name);
        if (new_value) pgvalue_free(new_value);
        if (old_value) pgvalue_free(old_value);
        ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("cloudsync_update_transfn failed to allocate values")));
    }

    if (!cloudsync_update_payload_append(payload, table_name, new_value, old_value)) {
        if (table_name && payload->table_name != table_name) pgvalue_free(table_name);
        if (new_value) pgvalue_free(new_value);
        if (old_value) pgvalue_free(old_value);
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("cloudsync_update_transfn failed to append payload")));
    }

    PG_RETURN_POINTER(payload);
}

PG_FUNCTION_INFO_V1(cloudsync_update_finalfn);
Datum cloudsync_update_finalfn (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) {
        PG_RETURN_BOOL(true);
    }

    cloudsync_update_payload *payload = (cloudsync_update_payload *)PG_GETARG_POINTER(0);
    if (!payload || payload->count == 0) {
        PG_RETURN_BOOL(true);
    }

    cloudsync_context *data = get_cloudsync_context();
    cloudsync_table_context *table = NULL;
    int rc = DBRES_OK;
    bool spi_connected = false;
    char buffer[1024];
    char buffer2[1024];
    size_t pklen = sizeof(buffer);
    size_t oldpklen = sizeof(buffer2);
    char *pk = NULL;
    char *oldpk = NULL;

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %d", spi_rc)));
    }
    spi_connected = true;

    PG_TRY();
    {
        const char *table_name = database_value_text((dbvalue_t *)payload->table_name);
        table = table_lookup(data, table_name);
        if (!table) {
            char meta_name[1024];
            snprintf(meta_name, sizeof(meta_name), "%s_cloudsync", table_name);
            if (!database_table_exists(data, meta_name, cloudsync_schema(data))) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("Unable to retrieve table name %s in cloudsync_update", table_name)));
            }

            table_algo algo = dbutils_table_settings_get_algo(data, table_name);
            if (algo == table_algo_none) algo = table_algo_crdt_cls;
            if (!table_add_to_context(data, algo, table_name)) {
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("Unable to load table context for %s", table_name)));
            }

            table = table_lookup(data, table_name);
            if (!table) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("Unable to retrieve table name %s in cloudsync_update", table_name)));
            }
        }

        int64_t db_version = cloudsync_dbversion_next(data, CLOUDSYNC_VALUE_NOTSET);

        int pk_count = table_count_pks(table);
        if (payload->count < pk_count) { 
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("Not enough primary key values in cloudsync_update payload")));
        }
        int max_expected = pk_count + table_count_cols(table);
        if (payload->count > max_expected) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("Too many values in cloudsync_update payload: got "
                                "%d expected <= %d",
                                payload->count, max_expected)));
        }

        bool prikey_changed = false;
        for (int i = 0; i < pk_count; i++) {
            if (dbutils_value_compare((dbvalue_t *)payload->old_values[i], (dbvalue_t *)payload->new_values[i]) != 0) {
                prikey_changed = true;
                break;
            }
        }

        pk = pk_encode_prikey((dbvalue_t **)payload->new_values, pk_count, buffer, &pklen);
        if (!pk) {
            ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("Not enough memory to encode the primary key(s)")));
        }
        if (pk == PRIKEY_NULL_CONSTRAINT_ERROR) {
            pk = NULL;
            ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg("Update aborted because primary key in table %s contains NULL values", table_name)));
        }
        if (prikey_changed) {
            oldpk = pk_encode_prikey((dbvalue_t **)payload->old_values, pk_count, buffer2, &oldpklen);
            if (!oldpk) {
                rc = DBRES_NOMEM;
                goto cleanup;
            }

            rc = local_mark_delete_meta(table, oldpk, oldpklen, db_version, cloudsync_bumpseq(data));
            if (rc != DBRES_OK) goto cleanup;

            rc = local_update_move_meta(table, pk, pklen, oldpk, oldpklen, db_version);
            if (rc != DBRES_OK) goto cleanup;

            rc = local_mark_insert_sentinel_meta(table, pk, pklen, db_version, cloudsync_bumpseq(data));
            if (rc != DBRES_OK) goto cleanup;
        }

        for (int i = 0; i < table_count_cols(table); i++) {
            int col_index = pk_count + i;
            if (col_index >= payload->count) break;

            if (dbutils_value_compare((dbvalue_t *)payload->old_values[col_index], (dbvalue_t *)payload->new_values[col_index]) != 0) {
                if (table_col_algo(table, i) == col_algo_block) {
                    // Block column: diff old and new text, emit per-block metadata changes
                    const char *new_text = (const char *)database_value_text(payload->new_values[col_index]);
                    const char *delim = table_col_delimiter(table, i);
                    const char *col = table_colname(table, i);

                    // Read existing blocks from blocks table
                    block_list_t *old_blocks = block_list_create_empty();
                    char *like_pattern = block_build_colname(col, "%");
                    if (like_pattern && old_blocks) {
                        char *list_sql = cloudsync_memory_mprintf(
                            "SELECT col_name, col_value FROM %s WHERE pk = $1 AND col_name LIKE $2 ORDER BY col_name COLLATE \"C\"",
                            table_blocks_ref(table));
                        if (list_sql) {
                            dbvm_t *list_vm = NULL;
                            if (databasevm_prepare(data, list_sql, &list_vm, 0) == DBRES_OK) {
                                databasevm_bind_blob(list_vm, 1, pk, (int)pklen);
                                databasevm_bind_text(list_vm, 2, like_pattern, -1);
                                while (databasevm_step(list_vm) == DBRES_ROW) {
                                    const char *bcn = database_column_text(list_vm, 0);
                                    const char *bval = database_column_text(list_vm, 1);
                                    const char *pos = block_extract_position_id(bcn);
                                    if (pos && old_blocks) {
                                        block_list_add(old_blocks, bval ? bval : "", pos);
                                    }
                                }
                                databasevm_finalize(list_vm);
                            }
                            cloudsync_memory_free(list_sql);
                        }
                    }

                    // Split new text into parts (NULL text = all blocks removed)
                    block_list_t *new_blocks = new_text ? block_split(new_text, delim) : block_list_create_empty();
                    if (new_blocks && old_blocks) {
                        // Build array of new content strings (NULL when count is 0)
                        const char **new_parts = NULL;
                        if (new_blocks->count > 0) {
                            new_parts = (const char **)cloudsync_memory_alloc(
                                (uint64_t)(new_blocks->count * sizeof(char *)));
                            if (new_parts) {
                                for (int b = 0; b < new_blocks->count; b++) {
                                    new_parts[b] = new_blocks->entries[b].content;
                                }
                            }
                        }

                        if (new_parts || new_blocks->count == 0) {
                            block_diff_t *diff = block_diff(old_blocks->entries, old_blocks->count,
                                                             new_parts, new_blocks->count);
                            if (diff) {
                                for (int d = 0; d < diff->count; d++) {
                                    block_diff_entry_t *de = &diff->entries[d];
                                    char *block_cn = block_build_colname(col, de->position_id);
                                    if (!block_cn) continue;

                                    if (de->type == BLOCK_DIFF_ADDED || de->type == BLOCK_DIFF_MODIFIED) {
                                        rc = local_mark_insert_or_update_meta(table, pk, pklen, block_cn,
                                                                              db_version, cloudsync_bumpseq(data));
                                        // Store block value
                                        if (rc == DBRES_OK && table_block_value_write_stmt(table)) {
                                            dbvm_t *wvm = table_block_value_write_stmt(table);
                                            databasevm_bind_blob(wvm, 1, pk, (int)pklen);
                                            databasevm_bind_text(wvm, 2, block_cn, -1);
                                            databasevm_bind_text(wvm, 3, de->content, -1);
                                            databasevm_step(wvm);
                                            databasevm_reset(wvm);
                                        }
                                    } else if (de->type == BLOCK_DIFF_REMOVED) {
                                        // Mark block as deleted in metadata (even col_version)
                                        rc = local_mark_delete_block_meta(table, pk, pklen, block_cn,
                                                                          db_version, cloudsync_bumpseq(data));
                                        // Remove from blocks table
                                        if (rc == DBRES_OK) {
                                            block_delete_value_external(data, table, pk, pklen, block_cn);
                                        }
                                    }
                                    cloudsync_memory_free(block_cn);
                                    if (rc != DBRES_OK) break;
                                }
                                block_diff_free(diff);
                            }
                            if (new_parts) cloudsync_memory_free((void *)new_parts);
                        }
                    }
                    if (new_blocks) block_list_free(new_blocks);
                    if (old_blocks) block_list_free(old_blocks);
                    if (like_pattern) cloudsync_memory_free(like_pattern);
                    if (rc != DBRES_OK) goto cleanup;
                } else {
                    rc = local_mark_insert_or_update_meta(table, pk, pklen, table_colname(table, i), db_version, cloudsync_bumpseq(data));
                    if (rc != DBRES_OK) goto cleanup;
                }
            }
        }

cleanup:
        if (pk != buffer) cloudsync_memory_free(pk);
        if (oldpk && (oldpk != buffer2)) cloudsync_memory_free(oldpk);
    }
    PG_CATCH();
    {
        if (payload) {
            cloudsync_update_payload_free(payload);
        }
        if (spi_connected) SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    if (payload) {
        cloudsync_update_payload_free(payload);
    }
    if (spi_connected) SPI_finish();

    if (rc != DBRES_OK) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s", database_errmsg(data))));
    }

    PG_RETURN_BOOL(true);
}

// Placeholder - not implemented yet
PG_FUNCTION_INFO_V1(cloudsync_payload_encode);
Datum cloudsync_payload_encode (PG_FUNCTION_ARGS) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cloudsync_payload_encode should not be called directly - use aggregate version")));
    PG_RETURN_NULL();
}

// MARK: - Schema -

PG_FUNCTION_INFO_V1(pg_cloudsync_set_schema);
Datum pg_cloudsync_set_schema (PG_FUNCTION_ARGS) {
    const char *schema = NULL;

    if (!PG_ARGISNULL(0)) {
        schema = text_to_cstring(PG_GETARG_TEXT_PP(0));
    }

    cloudsync_context *data = get_cloudsync_context();
    cloudsync_set_schema(data, schema);

    // Persist schema to settings so it is restored on context re-initialization.
    // Only persist if settings table exists (it may not exist before cloudsync_init).
    int spi_rc = SPI_connect();
    if (spi_rc == SPI_OK_CONNECT) {
        if (database_internal_table_exists(data, CLOUDSYNC_SETTINGS_NAME)) {
            dbutils_settings_set_key_value(data, "schema", schema);
        }
        SPI_finish();
    }

    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(pg_cloudsync_schema);
Datum pg_cloudsync_schema (PG_FUNCTION_ARGS) {
    cloudsync_context *data = get_cloudsync_context();
    const char *schema = cloudsync_schema(data);

    if (!schema) {
        PG_RETURN_NULL();
    }
    
    PG_RETURN_TEXT_P(cstring_to_text(schema));
}

PG_FUNCTION_INFO_V1(pg_cloudsync_table_schema);
Datum pg_cloudsync_table_schema (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("table_name cannot be NULL")));
    }

    const char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_context *data = get_cloudsync_context();
    const char *schema = cloudsync_table_schema(data, table_name);

    if (!schema) {
        PG_RETURN_NULL();
    }
    
    PG_RETURN_TEXT_P(cstring_to_text(schema));
}

// MARK: - Changes -

// Encode a single value using cloudsync pk encoding
static bytea *cloudsync_encode_value_from_datum (Datum val, Oid typeid, int32 typmod, Oid collation, bool isnull) {
    pgvalue_t *v = pgvalue_create(val, typeid, typmod, collation, isnull);
    if (!v) {
        ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("cloudsync: failed to allocate value")));
    }
    if (!isnull) {
        pgvalue_ensure_detoast(v);
    }

    size_t encoded_len = pk_encode_size((dbvalue_t **)&v, 1, 0, -1);
    bytea *out = (bytea *)palloc(VARHDRSZ + encoded_len);
    if (!out) {
        pgvalue_free(v);
        ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("cloudsync: failed to allocate encoding buffer")));
    }
    
    pk_encode((dbvalue_t **)&v, 1, VARDATA(out), false, &encoded_len, -1);
    SET_VARSIZE(out, VARHDRSZ + encoded_len);
    
    pgvalue_free(v);
    return out;
}

// Encode a NULL value using cloudsync pk encoding
static bytea *cloudsync_encode_null_value (void) {
    return cloudsync_encode_value_from_datum((Datum)0, TEXTOID, -1, InvalidOid, true);
}
 
// Hold a decoded pk-encoded value with its original type
typedef struct {
    int dbtype;
    int64_t ival;
    double dval;
    char *pval;
    int64_t len;
    bool isnull;
} cloudsync_decoded_value;

// Decode a single pk-encoded value into a typed representation
static int cloudsync_decode_value_cb (void *xdata, int index, int type, int64_t ival, double dval, char *pval) {
    cloudsync_decoded_value *out = (cloudsync_decoded_value *)xdata;
    if (!out || index != 0) return DBRES_ERROR;

    out->dbtype = type;
    out->isnull = false;
    out->ival = 0;
    out->dval = 0.0;
    out->pval = NULL;
    out->len = 0;

    switch (type) {
        case DBTYPE_INTEGER:
            out->ival = ival;
            break;
        case DBTYPE_FLOAT:
            out->dval = dval;
            break;
        case DBTYPE_TEXT:
            out->pval = pnstrdup(pval, (int)ival);
            out->len = ival;
            break;
        case DBTYPE_BLOB:
            if (ival > 0) {
                out->pval = (char *)palloc((size_t)ival);
                memcpy(out->pval, pval, (size_t)ival);
            }
            out->len = ival;
            break;
        case DBTYPE_NULL:
            out->isnull = true;
            break;
        default:
            return DBRES_ERROR;
    }
    return DBRES_OK;
}

// Map a column Oid to the decoded type Oid that would be used for non-NULL values.
// This ensures NULL and non-NULL values use consistent types for SPI plan caching.
// The mapping must match pgvalue_dbtype() in pgvalue.c which determines encode/decode types.
// For example, INT4OID columns decode to INT8OID, UUIDOID columns decode to TEXTOID.
static Oid map_column_oid_to_decoded_oid(Oid col_oid) {
    switch (col_oid) {
        // Integer types → INT8OID (all integers decode to int64)
        // Must match DBTYPE_INTEGER cases in pgvalue_dbtype()
        case INT2OID:
        case INT4OID:
        case INT8OID:
        case BOOLOID:   // BOOLEAN encodes/decodes as INTEGER
        case CHAROID:   // "char" encodes/decodes as INTEGER
        case OIDOID:    // OID encodes/decodes as INTEGER
            return INT8OID;
        // Float types → FLOAT8OID (all floats decode to double)
        // Must match DBTYPE_FLOAT cases in pgvalue_dbtype()
        case FLOAT4OID:
        case FLOAT8OID:
        case NUMERICOID:
            return FLOAT8OID;
        // Binary types → BYTEAOID
        // Must match DBTYPE_BLOB cases in pgvalue_dbtype()
        case BYTEAOID:
            return BYTEAOID;
        // All other types (text, varchar, uuid, json, date, timestamp, etc.) → TEXTOID
        // These all encode/decode as DBTYPE_TEXT
        default:
            return TEXTOID;
    }
}

// Get the Oid of a column from the system catalog.
// Requires SPI to be connected. Returns InvalidOid if not found.
static Oid get_column_oid(const char *schema, const char *table_name, const char *column_name) {
    if (!table_name || !column_name) return InvalidOid;

    const char *query =
        "SELECT a.atttypid "
        "FROM pg_attribute a "
        "JOIN pg_class c ON c.oid = a.attrelid "
        "LEFT JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE c.relname = $1 "
        "AND a.attname = $2 "
        "AND a.attnum > 0 "
        "AND NOT a.attisdropped "
        "AND (n.nspname = $3 OR $3 IS NULL)";

    Oid argtypes[3] = {TEXTOID, TEXTOID, TEXTOID};
    Datum values[3];
    char nulls[3] = {' ', ' ', schema ? ' ' : 'n'};

    values[0] = CStringGetTextDatum(table_name);
    values[1] = CStringGetTextDatum(column_name);
    values[2] = schema ? CStringGetTextDatum(schema) : (Datum)0;

    int ret = SPI_execute_with_args(query, 3, argtypes, values, nulls, true, 1);

    pfree(DatumGetPointer(values[0]));
    pfree(DatumGetPointer(values[1]));
    if (schema) pfree(DatumGetPointer(values[2]));

    if (ret != SPI_OK_SELECT || SPI_processed == 0) {
        if (SPI_tuptable) SPI_freetuptable(SPI_tuptable);
        return InvalidOid;
    }

    bool isnull;
    Datum col_oid = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
    Oid result = isnull ? InvalidOid : DatumGetObjectId(col_oid);
    SPI_freetuptable(SPI_tuptable);
    return result;
}

// Decode encoded bytea into a pgvalue_t with the decoded base type.
// Type casting to the target column type is handled by the SQL statement.
static pgvalue_t *cloudsync_decode_bytea_to_pgvalue (bytea *encoded, bool *out_isnull) {
    // Decode input guardrails.
    if (out_isnull) *out_isnull = true;
    if (!encoded) return NULL;

    // Decode bytea into C types with dbtype info.
    cloudsync_decoded_value dv = {.isnull = true};
    int blen = (int)VARSIZE_ANY_EXHDR(encoded);
    int decoded = pk_decode((char *)VARDATA_ANY(encoded), (size_t)blen, 1, NULL, -1, cloudsync_decode_value_cb, &dv);
    if (decoded != 1) ereport(ERROR, (errmsg("cloudsync: failed to decode encoded value")));
    if (out_isnull) *out_isnull = dv.isnull;
    if (dv.isnull) return NULL;

    // Map decoded C types into a PostgreSQL Datum with the base type.
    // The SQL statement handles casting to the target column type via $n::typename.
    Oid typoid = TEXTOID;
    Datum datum;

    switch (dv.dbtype) {
        case DBTYPE_INTEGER:
            typoid = INT8OID;
            datum = Int64GetDatum(dv.ival);
            break;
        case DBTYPE_FLOAT:
            typoid = FLOAT8OID;
            datum = Float8GetDatum(dv.dval);
            break;
        case DBTYPE_TEXT: {
            typoid = TEXTOID;
            Size tlen = dv.pval ? (Size)dv.len : 0;
            text *t = (text *)palloc(VARHDRSZ + tlen);
            SET_VARSIZE(t, VARHDRSZ + tlen);
            if (tlen > 0) memmove(VARDATA(t), dv.pval, tlen);
            datum = PointerGetDatum(t);
        } break;
        case DBTYPE_BLOB: {
            typoid = BYTEAOID;
            bytea *ba = (bytea *)palloc(VARHDRSZ + dv.len);
            SET_VARSIZE(ba, VARHDRSZ + dv.len);
            if (dv.len > 0) memcpy(VARDATA(ba), dv.pval, (size_t)dv.len);
            datum = PointerGetDatum(ba);
        } break;
        case DBTYPE_NULL:
            if (out_isnull) *out_isnull = true;
            if (dv.pval) pfree(dv.pval);
            return NULL;
        default:
            if (dv.pval) pfree(dv.pval);
            ereport(ERROR, (errmsg("cloudsync: unsupported decoded type")));
    }

    if (dv.pval) pfree(dv.pval);

    return pgvalue_create(datum, typoid, -1, InvalidOid, false);
}

PG_FUNCTION_INFO_V1(cloudsync_encode_value);
Datum cloudsync_encode_value(PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) {
        bytea *null_encoded = cloudsync_encode_null_value();
        PG_RETURN_BYTEA_P(null_encoded);
    }
    
    Oid   typeoid = get_fn_expr_argtype(fcinfo->flinfo, 0);
    int32 typmod  = -1;
    Oid   collid  = PG_GET_COLLATION();
    
    if (!OidIsValid(typeoid) || typeoid == ANYELEMENTOID) {
        if (fcinfo->flinfo->fn_expr && IsA(fcinfo->flinfo->fn_expr, FuncExpr)) {
            FuncExpr *fexpr = (FuncExpr *) fcinfo->flinfo->fn_expr;
            if (fexpr->args && list_length(fexpr->args) >= 1) {
                Node *arg = (Node *) linitial(fexpr->args);
                typeoid = exprType(arg);
                typmod  = exprTypmod(arg);
                collid  = exprCollation(arg);
            }
        }
    }
    
    if (!OidIsValid(typeoid) || typeoid == ANYELEMENTOID) {
        ereport(ERROR, (errmsg("cloudsync_encode_any: unable to resolve argument type")));
    }
    
    Datum val = PG_GETARG_DATUM(0);
    bytea *result = cloudsync_encode_value_from_datum(val, typeoid, typmod, collid, false);
    PG_RETURN_BYTEA_P(result);
}

PG_FUNCTION_INFO_V1(cloudsync_col_value);
Datum cloudsync_col_value(PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("cloudsync_col_value arguments cannot be NULL")));
    }
    
    // argv[0] -> table name
    // argv[1] -> column name
    // argv[2] -> encoded pk
    
    char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char *col_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
    bytea *encoded_pk = PG_GETARG_BYTEA_P(2);
    
    // check for special tombstone value
    if (strcmp(col_name, CLOUDSYNC_TOMBSTONE_VALUE) == 0) {
        bytea *null_encoded = cloudsync_encode_null_value();
        PG_RETURN_BYTEA_P(null_encoded);
    }
    
    cloudsync_context *data = get_cloudsync_context();
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) {
        ereport(ERROR, (errmsg("Unable to retrieve table name %s in clousdsync_col_value.", table_name)));
    }

    // Block column: if col_name contains \x1F, read from blocks table
    if (block_is_block_colname(col_name) && table_has_block_cols(table)) {
        dbvm_t *bvm = table_block_value_read_stmt(table);
        if (!bvm) {
            bytea *null_encoded = cloudsync_encode_null_value();
            PG_RETURN_BYTEA_P(null_encoded);
        }

        bytea *encoded_pk_b = PG_GETARG_BYTEA_P(2);
        size_t b_pk_len = (size_t)VARSIZE_ANY_EXHDR(encoded_pk_b);
        int brc = databasevm_bind_blob(bvm, 1, VARDATA_ANY(encoded_pk_b), (uint64_t)b_pk_len);
        if (brc != DBRES_OK) { databasevm_reset(bvm); ereport(ERROR, (errmsg("cloudsync_col_value block bind error"))); }
        brc = databasevm_bind_text(bvm, 2, col_name, -1);
        if (brc != DBRES_OK) { databasevm_reset(bvm); ereport(ERROR, (errmsg("cloudsync_col_value block bind error"))); }

        brc = databasevm_step(bvm);
        if (brc == DBRES_ROW) {
            size_t blob_len = 0;
            const void *blob = database_column_blob(bvm, 0, &blob_len);
            bytea *result = NULL;
            if (blob && blob_len > 0) {
                result = (bytea *)palloc(VARHDRSZ + blob_len);
                SET_VARSIZE(result, VARHDRSZ + blob_len);
                memcpy(VARDATA(result), blob, blob_len);
            }
            databasevm_reset(bvm);
            if (result) PG_RETURN_BYTEA_P(result);
            PG_RETURN_NULL();
        } else {
            databasevm_reset(bvm);
            bytea *null_encoded = cloudsync_encode_null_value();
            PG_RETURN_BYTEA_P(null_encoded);
        }
    }

    // extract the right col_value vm associated to the column name
    dbvm_t *vm = table_column_lookup(table, col_name, false, NULL);
    if (!vm) {
        ereport(ERROR, (errmsg("Unable to retrieve column value precompiled statement in clousdsync_col_value.")));
    }
    
    // bind primary key values
    size_t pk_len = (size_t)VARSIZE_ANY_EXHDR(encoded_pk);
    int count = pk_decode_prikey((char *)VARDATA_ANY(encoded_pk), pk_len, pk_decode_bind_callback, (void *)vm);
    if (count <= 0) {
        ereport(ERROR, (errmsg("Unable to decode primary key value in clousdsync_col_value.")));
    }
    
    // execute vm
    int rc = databasevm_step(vm);
    if (rc == DBRES_DONE) {
        databasevm_reset(vm);
        // row not found (RLS or genuinely missing) — return the RLS sentinel as bytea
        const char *rls = CLOUDSYNC_RLS_RESTRICTED_VALUE;
        size_t rls_len = strlen(rls);
        bytea *result = (bytea *)palloc(VARHDRSZ + rls_len);
        SET_VARSIZE(result, VARHDRSZ + rls_len);
        memcpy(VARDATA(result), rls, rls_len);
        PG_RETURN_BYTEA_P(result);
    } else if (rc == DBRES_ROW) {
        // copy value before reset invalidates SPI tuple memory
        size_t blob_len = 0;
        const void *blob = database_column_blob(vm, 0, &blob_len);
        bytea *result = NULL;
        if (blob && blob_len > 0) {
            result = (bytea *)palloc(VARHDRSZ + blob_len);
            SET_VARSIZE(result, VARHDRSZ + blob_len);
            memcpy(VARDATA(result), blob, blob_len);
        }
        databasevm_reset(vm);
        if (result) PG_RETURN_BYTEA_P(result);
        PG_RETURN_NULL();
    }

    databasevm_reset(vm);
    ereport(ERROR, (errmsg("cloudsync_col_value error: %s", cloudsync_errmsg(data))));
    PG_RETURN_NULL(); // unreachable, silences compiler
}

// MARK: - Block-level LWW -

PG_FUNCTION_INFO_V1(cloudsync_text_materialize);
Datum cloudsync_text_materialize (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("cloudsync_text_materialize: table_name and col_name cannot be NULL")));
    }

    const char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *col_name = text_to_cstring(PG_GETARG_TEXT_PP(1));

    cloudsync_context *data = get_cloudsync_context();
    cloudsync_pg_cleanup_state cleanup = {0};

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %d", spi_rc)));
    }
    cleanup.spi_connected = true;

    PG_ENSURE_ERROR_CLEANUP(cloudsync_pg_cleanup, PointerGetDatum(&cleanup));
    {
        cloudsync_table_context *table = table_lookup(data, table_name);
        if (!table) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("Unable to retrieve table name %s in cloudsync_text_materialize", table_name)));
        }

        int col_idx = table_col_index(table, col_name);
        if (col_idx < 0 || table_col_algo(table, col_idx) != col_algo_block) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("Column %s in table %s is not configured as block-level", col_name, table_name)));
        }

        // Extract PK values from VARIADIC "any" (args starting from index 2)
        cleanup.argv = pgvalues_from_args(fcinfo, 2, &cleanup.argc);

        // Normalize PK values to text for consistent encoding
        pgvalues_normalize_to_text(cleanup.argv, cleanup.argc);

        int expected_pks = table_count_pks(table);
        if (cleanup.argc != expected_pks) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("Expected %d primary key values, got %d", expected_pks, cleanup.argc)));
        }

        size_t pklen = sizeof(cleanup.pk_buffer);
        cleanup.pk = pk_encode_prikey((dbvalue_t **)cleanup.argv, cleanup.argc, cleanup.pk_buffer, &pklen);
        if (!cleanup.pk || cleanup.pk == PRIKEY_NULL_CONSTRAINT_ERROR) {
            if (cleanup.pk == PRIKEY_NULL_CONSTRAINT_ERROR) cleanup.pk = NULL;
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("Failed to encode primary key(s)")));
        }

        int rc = block_materialize_column(data, table, cleanup.pk, (int)pklen, col_name);
        if (rc != DBRES_OK) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("%s", cloudsync_errmsg(data))));
        }
    }
    PG_END_ENSURE_ERROR_CLEANUP(cloudsync_pg_cleanup, PointerGetDatum(&cleanup));

    cloudsync_pg_cleanup(0, PointerGetDatum(&cleanup));
    PG_RETURN_BOOL(true);
}

// Track SRF execution state across calls
typedef struct {
    Portal    portal;
    TupleDesc outdesc;
    bool      spi_connected;
} SRFState;
 
// Build the UNION ALL SQL for cloudsync_changes SRF
static char * build_union_sql (void) {
    char *result = NULL;
    MemoryContext caller_ctx = CurrentMemoryContext;
    
    if (SPI_connect() != SPI_OK_CONNECT) {
        ereport(ERROR, (errmsg("cloudsync: SPI_connect failed")));
    }
    
    PG_TRY();
    {
        const char *sql =
        "SELECT n.nspname, c.relname "
        "FROM pg_class c "
        "JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE c.relkind = 'r' "
        "  AND n.nspname NOT IN ('pg_catalog','information_schema') "
        "  AND c.relname LIKE '%\\_cloudsync' ESCAPE '\\' "
        "ORDER BY n.nspname, c.relname";
        
        int rc = SPI_execute(sql, true, 0);
        if (rc != SPI_OK_SELECT || !SPI_tuptable) {
            ereport(ERROR, (errmsg("cloudsync: SPI_execute failed while listing *_cloudsync")));
        }
        
        StringInfoData buf;
        initStringInfo(&buf);
        
        uint64 ntables = SPI_processed;
        char **nsp_list = NULL;
        char **rel_list = NULL;
        if (ntables > 0) {
            nsp_list = (char **)palloc0(sizeof(char *) * ntables);
            rel_list = (char **)palloc0(sizeof(char *) * ntables);
        }
        for (uint64 i = 0; i < ntables; i++) {
            HeapTuple tup = SPI_tuptable->vals[i];
            TupleDesc td = SPI_tuptable->tupdesc;
            char *nsp = SPI_getvalue(tup, td, 1);
            char *rel = SPI_getvalue(tup, td, 2);
            if (!nsp || !rel) {
                if (nsp) pfree(nsp);
                if (rel) pfree(rel);
                continue;
            }
            nsp_list[i] = pstrdup(nsp);
            rel_list[i] = pstrdup(rel);
            pfree(nsp);
            pfree(rel);
        }
        SPI_freetuptable(SPI_tuptable);
        SPI_tuptable = NULL;
        
        bool first = true;
        for (uint64 i = 0; i < ntables; i++) {
            char *nsp = nsp_list ? nsp_list[i] : NULL;
            char *rel = rel_list ? rel_list[i] : NULL;
            if (!nsp || !rel) {
                if (nsp) pfree(nsp);
                if (rel) pfree(rel);
                continue;
            }
            
            size_t rlen = strlen(rel);
            if (rlen <= 10) {pfree(nsp); pfree(rel); continue;} /* "_cloudsync" */
            
            char *base = pstrdup(rel);
            base[rlen - 10] = '\0';
            
            char *quoted_base = quote_literal_cstr(base);
            const char *quoted_nsp = quote_identifier(nsp);
            const char *quoted_rel = quote_identifier(rel);
            
            if (!first) appendStringInfoString(&buf, " UNION ALL ");
            first = false;
            
            
            /*
             * Build a single SELECT per table that:
             *  - reads change rows from <table>_cloudsync (t1)
             *  - joins the base table (b) using decoded PK components
             *  - computes col_value in-SQL with a CASE over col_name
             *
             * This avoids calling cloudsync_col_value() (and therefore avoids
             * executing extra SPI queries per row), while still honoring RLS:
             * if the base row is not visible, the LEFT JOIN yields NULL and we
             * return the restricted sentinel value (then filtered out).
             */
            
            char *nsp_lit = quote_literal_cstr(nsp);
            char *base_lit = quote_literal_cstr(base);
            
            /* Collect PK columns (name + SQL type) */
            StringInfoData pkq;
            initStringInfo(&pkq);
            appendStringInfo(&pkq,
                             "SELECT a.attname, format_type(a.atttypid, a.atttypmod) AS typ "
                             "FROM pg_index i "
                             "JOIN pg_class c ON c.oid = i.indrelid "
                             "JOIN pg_namespace n ON n.oid = c.relnamespace "
                             "JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum = ANY(i.indkey) "
                             "WHERE i.indisprimary AND n.nspname = %s AND c.relname = %s "
                             "ORDER BY array_position(i.indkey, a.attnum)",
                             nsp_lit, base_lit
                             );
            int pkrc = SPI_execute(pkq.data, true, 0);
            pfree(pkq.data);
            if (pkrc != SPI_OK_SELECT || (SPI_processed == 0) || (!SPI_tuptable)) {
                if (SPI_tuptable) SPI_freetuptable(SPI_tuptable);
                ereport(ERROR, (errmsg("cloudsync: unable to resolve primary key for %s.%s", nsp, base)));
            }
            uint64 npk = SPI_processed;
            
            StringInfoData joincond;
            initStringInfo(&joincond);
            for (uint64 k = 0; k < npk; k++) {
                HeapTuple pkt = SPI_tuptable->vals[k];
                TupleDesc pkd = SPI_tuptable->tupdesc;
                char *pkname = SPI_getvalue(pkt, pkd, 1);
                char *pktype = SPI_getvalue(pkt, pkd, 2);
                if (!pkname || !pktype) {
                    if (pkname) pfree(pkname);
                    if (pktype) pfree(pktype);
                    pfree(joincond.data);
                    SPI_freetuptable(SPI_tuptable);
                    ereport(ERROR, (errmsg("cloudsync: invalid pk metadata for %s.%s", nsp, base)));
                }
                
                if (k > 0) appendStringInfoString(&joincond, " AND ");
                appendStringInfo(&joincond,
                                 "b.%s = cloudsync_pk_decode(t1.pk, %llu)::%s",
                                 quote_identifier(pkname),
                                 (unsigned long long)(k + 1),
                                 pktype
                                 );
                pfree(pkname);
                pfree(pktype);
            }
            SPI_freetuptable(SPI_tuptable);
            
            // Check if blocks table exists for this table
            char blocks_tbl_name[1024];
            snprintf(blocks_tbl_name, sizeof(blocks_tbl_name), "%s_cloudsync_blocks", base);
            StringInfoData btq;
            initStringInfo(&btq);
            appendStringInfo(&btq,
                "SELECT 1 FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
                "WHERE c.relname = %s AND n.nspname = %s AND c.relkind = 'r'",
                quote_literal_cstr(blocks_tbl_name), nsp_lit);
            int btrc = SPI_execute(btq.data, true, 1);
            bool has_blocks_table = (btrc == SPI_OK_SELECT && SPI_processed > 0);
            if (SPI_tuptable) { SPI_freetuptable(SPI_tuptable); SPI_tuptable = NULL; }
            pfree(btq.data);

            /* Collect all base-table columns to build CASE over t1.col_name */
            StringInfoData colq;
            initStringInfo(&colq);
            appendStringInfo(&colq,
                             "SELECT a.attname "
                             "FROM pg_attribute a "
                             "JOIN pg_class c ON c.oid = a.attrelid "
                             "JOIN pg_namespace n ON n.oid = c.relnamespace "
                             "WHERE a.attnum > 0 AND NOT a.attisdropped "
                             "  AND n.nspname = %s AND c.relname = %s "
                             "ORDER BY a.attnum",
                             nsp_lit, base_lit
                             );
            int colrc = SPI_execute(colq.data, true, 0);
            pfree(colq.data);
            if (colrc != SPI_OK_SELECT || !SPI_tuptable) {
                if (SPI_tuptable) SPI_freetuptable(SPI_tuptable);
                ereport(ERROR, (errmsg("cloudsync: unable to resolve columns for %s.%s", nsp, base)));
            }
            uint64 ncols = SPI_processed;

            StringInfoData caseexpr;
            initStringInfo(&caseexpr);
            appendStringInfoString(&caseexpr,
                                   "CASE "
                                   "WHEN t1.col_name = '" CLOUDSYNC_TOMBSTONE_VALUE "' THEN " CLOUDSYNC_NULL_VALUE_BYTEA " "
                                   "WHEN b.ctid IS NULL THEN " CLOUDSYNC_RLS_RESTRICTED_VALUE_BYTEA " "
                                   );
            if (has_blocks_table) {
                appendStringInfo(&caseexpr,
                    "WHEN t1.col_name LIKE '%%' || chr(31) || '%%' THEN "
                    "(SELECT cloudsync_encode_value(blk.col_value) FROM %s.\"%s_cloudsync_blocks\" blk "
                    "WHERE blk.pk = t1.pk AND blk.col_name = t1.col_name) ",
                    quote_identifier(nsp), base);
            }
            appendStringInfoString(&caseexpr,
                                   "ELSE CASE t1.col_name "
                                   );

            for (uint64 k = 0; k < ncols; k++) {
                HeapTuple ct = SPI_tuptable->vals[k];
                TupleDesc cd = SPI_tuptable->tupdesc;
                char *cname = SPI_getvalue(ct, cd, 1);
                if (!cname) continue;
                
                appendStringInfo(&caseexpr,
                                 "WHEN %s THEN cloudsync_encode_value(b.%s) ",
                                 quote_literal_cstr(cname),
                                 quote_identifier(cname)
                                 );
                pfree(cname);
            }
            SPI_freetuptable(SPI_tuptable);
            
            appendStringInfoString(&caseexpr,
                                   "ELSE " CLOUDSYNC_RLS_RESTRICTED_VALUE_BYTEA " END END"
                                   );
            
            const char *quoted_base_ident = quote_identifier(base);
            
            appendStringInfo(&buf,
                             "SELECT * FROM ("
                             "SELECT %s AS tbl, t1.pk, t1.col_name, "
                             "%s AS col_value, "
                             "t1.col_version, t1.db_version, site_tbl.site_id, "
                             "COALESCE(t2.col_version, 1) AS cl, t1.seq "
                             "FROM %s.%s t1 "
                             "LEFT JOIN cloudsync_site_id site_tbl ON t1.site_id = site_tbl.id "
                             "LEFT JOIN %s.%s t2 "
                             "  ON t1.pk = t2.pk AND t2.col_name = '%s' "
                             "LEFT JOIN %s.%s b ON %s "
                             ") s WHERE s.col_value IS DISTINCT FROM %s",
                             quoted_base,
                             caseexpr.data,
                             quoted_nsp,  quoted_rel,
                             quoted_nsp,  quoted_rel,
                             CLOUDSYNC_TOMBSTONE_VALUE,
                             quoted_nsp,  quoted_base_ident,
                             joincond.data,
                             CLOUDSYNC_RLS_RESTRICTED_VALUE_BYTEA
                             );
            
            // Only free quoted identifiers if they're different from the input
            // (quote_identifier returns input pointer if no quoting needed)
            if (quoted_base_ident != base) pfree((void*)quoted_base_ident);
            pfree(joincond.data);
            pfree(caseexpr.data);

            pfree(base);
            pfree(base_lit);

            pfree(quoted_base);
            pfree(nsp_lit);
            bool nsp_was_quoted = (quoted_nsp != nsp);
            pfree(nsp);
            if (nsp_was_quoted) pfree((void *)quoted_nsp);
            bool rel_was_quoted = (quoted_rel != rel);
            pfree(rel);
            if (rel_was_quoted) pfree((void *)quoted_rel);
        }
        if (nsp_list) pfree(nsp_list);
        if (rel_list) pfree(rel_list);
        
        // Ensure result survives SPI_finish by allocating in the caller context.
        MemoryContext old_ctx = MemoryContextSwitchTo(caller_ctx);
        if (first) {
            result = pstrdup(
                             "SELECT NULL::text AS tbl, NULL::bytea AS pk, NULL::text AS col_name, NULL::bytea AS col_value, "
                             "NULL::bigint AS col_version, NULL::bigint AS db_version, NULL::bytea AS site_id, "
                             "NULL::bigint AS cl, NULL::bigint AS seq WHERE false"
                             );
        } else {
            result = pstrdup(buf.data);
        }
        MemoryContextSwitchTo(old_ctx);
        
        SPI_finish();
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();
    
    return result;
}

PG_FUNCTION_INFO_V1(cloudsync_changes_select);
Datum cloudsync_changes_select(PG_FUNCTION_ARGS) {
    FuncCallContext *funcctx;
    SRFState *st_local = NULL;
    bool spi_connected_local = false;
    
    PG_TRY();
    {
        if (SRF_IS_FIRSTCALL()) {
            funcctx = SRF_FIRSTCALL_INIT();
            MemoryContext oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
            
            int64 min_db_version = PG_GETARG_INT64(0);
            bool site_is_null = PG_ARGISNULL(1);
            bytea *filter_site_id = site_is_null ? NULL : PG_GETARG_BYTEA_PP(1);
            
            char *union_sql = build_union_sql();
            
            StringInfoData q;
            initStringInfo(&q);
            appendStringInfo(&q,
                             "SELECT tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq "
                             "FROM ( %s ) u "
                             "WHERE db_version > $1 "
                             "  AND ($2 IS NULL OR site_id = $2) "
                             "ORDER BY db_version, seq ASC",
                             union_sql
                             );
            
            if (SPI_connect() != SPI_OK_CONNECT) {
                ereport(ERROR, (errmsg("cloudsync: SPI_connect failed in SRF")));
            }
            spi_connected_local = true;
            
            Oid argtypes[2] = {INT8OID, BYTEAOID};
            Datum values[2];
            char nulls[2] = {' ', ' '};
            
            values[0] = Int64GetDatum(min_db_version);
            if (site_is_null) { nulls[1] = 'n'; values[1] = (Datum)0; }
            else values[1] = PointerGetDatum(filter_site_id);
            
            Portal portal = SPI_cursor_open_with_args(NULL, q.data, 2, argtypes, values, nulls, true, 0);
            if (!portal) {
                ereport(ERROR, (errmsg("cloudsync: SPI_cursor_open failed in SRF")));
            }
            
            TupleDesc outdesc;
            if (get_call_result_type(fcinfo, NULL, &outdesc) != TYPEFUNC_COMPOSITE) {
                ereport(ERROR, (errmsg("cloudsync: return type must be composite")));
            }
            outdesc = BlessTupleDesc(outdesc);
            
            SRFState *st = palloc0(sizeof(SRFState));
            st->portal = portal;
            st->outdesc = outdesc;
            st->spi_connected = true;
            funcctx->user_fctx = st;
            st_local = st;
            
            pfree(union_sql);
            pfree(q.data);
            
            MemoryContextSwitchTo(oldcontext);
        }
        
        funcctx = SRF_PERCALL_SETUP();
        SRFState *st = (SRFState *) funcctx->user_fctx;
        st_local = st;
        
        SPI_cursor_fetch(st->portal, true, 1);
        if (SPI_processed == 0) {
            if (SPI_tuptable) {
                SPI_freetuptable(SPI_tuptable);
                SPI_tuptable = NULL;
            }
            SPI_cursor_close(st->portal);
            st->portal = NULL;

            SPI_finish();
            st->spi_connected = false;

            // SPI operations may leave us in multi_call_memory_ctx
            // Must switch to a safe context before SRF_RETURN_DONE deletes it
            MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);

            SRF_RETURN_DONE(funcctx);
        }
        
        HeapTuple tup = SPI_tuptable->vals[0];
        TupleDesc td = SPI_tuptable->tupdesc;
        
        Datum outvals[9];
        bool outnulls[9];
        for (int i = 0; i < 9; i++) {
            outvals[i] = SPI_getbinval(tup, td, i+1, &outnulls[i]);
            if (!outnulls[i]) {
                Form_pg_attribute att = TupleDescAttr(td, i);
                outvals[i] = datumCopy(outvals[i], att->attbyval, att->attlen);
            }
        }
        
        HeapTuple outtup = heap_form_tuple(st->outdesc, outvals, outnulls);
        SPI_freetuptable(SPI_tuptable);
        SPI_tuptable = NULL;
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(outtup));
    }
    PG_CATCH();
    {
        // Switch to function's context (safe, won't be deleted)
        // Avoids assertion if we're currently in multi_call_memory_ctx
        MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);

        if (st_local && st_local->portal) {
            SPI_cursor_close(st_local->portal);
            st_local->portal = NULL;
        }
        
        if (st_local && st_local->spi_connected) {
            SPI_finish();
            st_local->spi_connected = false;
            spi_connected_local = false;
        } else if (spi_connected_local) {
            SPI_finish();
            spi_connected_local = false;
        }

        PG_RE_THROW();
    }
    PG_END_TRY();
}

// Trigger INSERT

PG_FUNCTION_INFO_V1(cloudsync_changes_insert_trigger);
Datum cloudsync_changes_insert_trigger (PG_FUNCTION_ARGS) {
    // sanity check
    bool spi_connected = false;
    TriggerData *trigdata = (TriggerData *) fcinfo->context;
    if (!CALLED_AS_TRIGGER(fcinfo)) ereport(ERROR, (errmsg("cloudsync_changes_insert_trigger must be called as trigger")));
    if (!TRIGGER_FIRED_BY_INSERT(trigdata->tg_event)) ereport(ERROR, (errmsg("Only INSERT allowed on cloudsync_changes")));
    
    HeapTuple newtup = trigdata->tg_trigtuple;
    pgvalue_t *col_value = NULL;
    PG_TRY();
    {
        TupleDesc desc = trigdata->tg_relation->rd_att;
        bool isnull;
        
        char *insert_tbl = text_to_cstring((text*) DatumGetPointer(heap_getattr(newtup, 1, desc, &isnull)));
        if (isnull) ereport(ERROR, (errmsg("tbl cannot be NULL")));
        
        bytea *insert_pk = (bytea*) DatumGetPointer(heap_getattr(newtup, 2, desc, &isnull));
        if (isnull) ereport(ERROR, (errmsg("pk cannot be NULL")));
        int insert_pk_len = (int)(VARSIZE_ANY_EXHDR(insert_pk));
        
        Datum insert_name_datum = heap_getattr(newtup, 3, desc, &isnull);
        char *insert_name = NULL;
        bool insert_name_owned = false;
        if (isnull) {
            insert_name = CLOUDSYNC_TOMBSTONE_VALUE;
        } else {
            insert_name = text_to_cstring((text*) DatumGetPointer(insert_name_datum));
            insert_name_owned = true;
        }
        bool is_tombstone = (strcmp(insert_name, CLOUDSYNC_TOMBSTONE_VALUE) == 0);
        
        // raw_insert_value is declared as bytea in the view (cloudsync-encoded value)
        bytea *insert_value_encoded = (bytea*) DatumGetPointer(heap_getattr(newtup, 4, desc, &isnull));
        
        int64 insert_col_version = DatumGetInt64(heap_getattr(newtup, 5, desc, &isnull));
        if (isnull) ereport(ERROR, (errmsg("col_version cannot be NULL")));
        
        int64 insert_db_version = DatumGetInt64(heap_getattr(newtup, 6, desc, &isnull));
        if (isnull) ereport(ERROR, (errmsg("db_version cannot be NULL")));
        
        bytea *insert_site_id = (bytea*) DatumGetPointer(heap_getattr(newtup, 7, desc, &isnull));
        if (isnull) ereport(ERROR, (errmsg("site_id cannot be NULL")));
        int insert_site_id_len = (int)(VARSIZE_ANY_EXHDR(insert_site_id));
        
        int64 insert_cl = DatumGetInt64(heap_getattr(newtup, 8, desc, &isnull));
        if (isnull) ereport(ERROR, (errmsg("cl cannot be NULL")));
        
        int64 insert_seq = DatumGetInt64(heap_getattr(newtup, 9, desc, &isnull));
        if (isnull) ereport(ERROR, (errmsg("seq cannot be NULL")));
        
        // lookup algo in cloudsync_tables
        cloudsync_context *data = get_cloudsync_context();
        cloudsync_table_context *table = table_lookup(data, insert_tbl);
        if (!table) ereport(ERROR, (errmsg("Unable to find table")));

        if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("cloudsync: SPI_connect failed in trigger")));
        spi_connected = true;

        // Decode value to base type; SQL statement handles type casting via $n::typename.
        // For non-NULL values, we get the decoded base type (INT8OID for integers, TEXTOID for text/UUID, etc).
        // For NULL values, we must use the SAME decoded type that non-NULL values would use.
        // This ensures type consistency across all calls, as SPI caches parameter types on first prepare.
        if (!is_tombstone) {
            bool value_is_null = false;
            col_value = cloudsync_decode_bytea_to_pgvalue(insert_value_encoded, &value_is_null);

            // When value is NULL, create a typed NULL pgvalue with the decoded type.
            // We map the column's actual Oid to the corresponding decoded Oid (e.g., INT4OID → INT8OID).
            if (!col_value && value_is_null) {
                Oid col_oid = get_column_oid(table_schema(table), insert_tbl, insert_name);
                if (OidIsValid(col_oid)) {
                    Oid decoded_oid = map_column_oid_to_decoded_oid(col_oid);
                    col_value = pgvalue_create((Datum)0, decoded_oid, -1, InvalidOid, true);
                }
            }
        }
        
        int rc = DBRES_OK;
        int64_t rowid = 0;
        if (table_algo_isgos(table)) {
            rc = merge_insert_col(data, table, VARDATA_ANY(insert_pk), insert_pk_len, insert_name, col_value, (int64_t)insert_col_version, (int64_t)insert_db_version, VARDATA_ANY(insert_site_id), insert_site_id_len, (int64_t)insert_seq, &rowid);
        } else {
            rc = merge_insert (data, table, VARDATA_ANY(insert_pk), insert_pk_len, insert_cl, insert_name, col_value, insert_col_version, insert_db_version, VARDATA_ANY(insert_site_id), insert_site_id_len, insert_seq, &rowid);
        }
        if (rc != DBRES_OK) {
            ereport(ERROR, (errmsg("Error during merge_insert: %s", database_errmsg(data))));
        }

        pgvalue_free(col_value);
        pfree(insert_tbl);
        if (insert_name_owned) pfree(insert_name);

        SPI_finish();
        spi_connected = false;
    }
    PG_CATCH();
    {
        pgvalue_free(col_value);
        if (spi_connected) {
            SPI_finish();
            spi_connected = false;
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    
    return PointerGetDatum(newtup);
}
