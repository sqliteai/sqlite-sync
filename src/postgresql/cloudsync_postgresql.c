//
//  cloudsync_postgresql.c
//  cloudsync
//
//  Created by Claude Code on 18/12/25.
//

// Define POSIX feature test macros before any includes
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

// PostgreSQL requires postgres.h to be included FIRST
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/uuid.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "access/xact.h"
#include "storage/ipc.h"
#include "utils/memutils.h"
#include "utils/array.h"
#include "pgvalue.h"

// CloudSync headers (after PostgreSQL headers)
#include "../cloudsync.h"
#include "../database.h"
#include "../dbutils.h"
#include "../pk.h"

// Note: network.h is not needed for PostgreSQL implementation

PG_MODULE_MAGIC;

// Note: PG_FUNCTION_INFO_V1 macros are declared before each function implementation below
// They should NOT be duplicated here to avoid redefinition errors

#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(X) (void)(X)
#endif

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

            // make sure to update internal version to current version
            dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_LIBVERSION, CLOUDSYNC_VERSION);
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
        cloudsync_pg_context_init(data);
        pg_cloudsync_context = data;
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

    uint8_t uuid[UUID_LEN];
    cloudsync_uuid_v7(uuid);

    // Return as bytea
    bytea *result = (bytea *)palloc(VARHDRSZ + UUID_LEN);
    SET_VARSIZE(result, VARHDRSZ + UUID_LEN);
    memcpy(VARDATA(result), uuid, UUID_LEN);

    PG_RETURN_BYTEA_P(result);
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
        } else {
            // In case of error, rollback transaction
            char err[1024];
            snprintf(err, sizeof(err), "%s", cloudsync_errmsg(data));
            database_rollback_savepoint(data, "cloudsync_init");
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s", err)));
        }

        cloudsync_update_schema_hash(data);

        // Build site_id as bytea to return
        result = (bytea *)palloc(UUID_LEN + VARHDRSZ);
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
    PG_RETURN_INT32(rc);
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
        dbutils_table_settings_set_key_value(data, tbl, col, key, value);
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
    bool spi_connected = false;

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed: %d", spi_rc)));
    }
    spi_connected = true;

    PG_TRY();
    {
        rc = cloudsync_begin_alter(data, table_name);
    }
    PG_CATCH();
    {
        if (spi_connected) SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    if (spi_connected) SPI_finish();
    if (rc != DBRES_OK) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("%s", cloudsync_errmsg(data))));
    }
    PG_RETURN_BOOL(true);
}

// cloudsync_commit_alter - Commit schema alteration
PG_FUNCTION_INFO_V1(pg_cloudsync_commit_alter);
Datum pg_cloudsync_commit_alter (PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("table_name cannot be NULL")));
    }

    const char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
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
        rc = cloudsync_commit_alter(data, table_name);
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
        payload = (cloudsync_payload_context *)cloudsync_memory_alloc(cloudsync_payload_context_size(NULL));
        memset(payload, 0, cloudsync_payload_context_size(NULL));
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
        database_value_free((dbvalue_t *)argv[i]);
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
        database_value_free((dbvalue_t *)state->argv[i]);
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
    pgvalue_t   *table_name;
    pgvalue_t   **new_values;
    pgvalue_t   **old_values;
    int         count;
    int         capacity;
} cloudsync_update_payload;

static void cloudsync_update_payload_free (cloudsync_update_payload *payload) {
    if (!payload) return;

    for (int i = 0; i < payload->count; i++) {
        database_value_free((dbvalue_t *)payload->new_values[i]);
        database_value_free((dbvalue_t *)payload->old_values[i]);
    }
    if (payload->new_values) cloudsync_memory_free(payload->new_values);
    if (payload->old_values) cloudsync_memory_free(payload->old_values);
    if (payload->table_name) database_value_free((dbvalue_t *)payload->table_name);

    payload->new_values = NULL;
    payload->old_values = NULL;
    payload->table_name = NULL;
    payload->count = 0;
    payload->capacity = 0;
}

static bool cloudsync_update_payload_append (cloudsync_update_payload *payload, pgvalue_t *table_name, pgvalue_t *new_value, pgvalue_t *old_value) {
    if (!payload) return false;

    if (payload->count >= payload->capacity) {
        int newcap = payload->capacity ? payload->capacity * 2 : 128;

        pgvalue_t **new_values_2 = (pgvalue_t **)cloudsync_memory_realloc(payload->new_values, newcap * sizeof(*new_values_2));
        if (!new_values_2) return false;
        payload->new_values = new_values_2;

        pgvalue_t **old_values_2 = (pgvalue_t **)cloudsync_memory_realloc(payload->old_values, newcap * sizeof(*old_values_2));
        if (!old_values_2) return false;
        payload->old_values = old_values_2;

        payload->capacity = newcap;
    }

    int index = payload->count;
    if (payload->table_name == NULL) {
        payload->table_name = table_name;
    } else if (dbutils_value_compare((dbvalue_t *)payload->table_name, (dbvalue_t *)table_name) != 0) {
        return false;
    } else {
        database_value_free((dbvalue_t *)table_name);
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

    // Signature is VARIADIC anyarray, so arg 0 is an array of PK values.
    if (!PG_ARGISNULL(0)) {
        ArrayType *array = PG_GETARG_ARRAYTYPE_P(0);
        argv = pgvalues_from_array(array, &argc);
    }

    size_t pklen = 0;
    char *encoded = pk_encode_prikey((dbvalue_t **)argv, argc, NULL, &pklen);
    if (!encoded) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("cloudsync_pk_encode failed to encode primary key")));
    }

    bytea *result = (bytea *)palloc(pklen + VARHDRSZ);
    SET_VARSIZE(result, pklen + VARHDRSZ);
    memcpy(VARDATA(result), encoded, pklen);
    cloudsync_memory_free(encoded);

    for (int i = 0; i < argc; i++) {
        database_value_free((dbvalue_t *)argv[i]);
    }
    if (argv) cloudsync_memory_free(argv);

    PG_RETURN_BYTEA_P(result);
}

// cloudsync_pk_decode - Decode primary key component at given index
PG_FUNCTION_INFO_V1(cloudsync_pk_decode);
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
            if (!database_table_exists(data, meta_name)) {
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

        // Extract PK values from VARIADIC anyarray (arg 1)
        if (!PG_ARGISNULL(1)) {
            ArrayType *pk_array = PG_GETARG_ARRAYTYPE_P(1);
            cleanup.argv = pgvalues_from_array(pk_array, &cleanup.argc);
        }

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
                rc = local_mark_insert_or_update_meta(table, cleanup.pk, pklen, table_colname(table, i), db_version, cloudsync_bumpseq(data));
                if (rc != DBRES_OK) break;
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
            if (!database_table_exists(data, meta_name)) {
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

        if (!PG_ARGISNULL(1)) {
            ArrayType *pk_array = PG_GETARG_ARRAYTYPE_P(1);
            cleanup.argv = pgvalues_from_array(pk_array, &cleanup.argc);
        }

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

// Aggregate function: cloudsync_update (not implemented - complex)
PG_FUNCTION_INFO_V1(cloudsync_update);
Datum cloudsync_update (PG_FUNCTION_ARGS) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cloudsync_update not yet implemented - aggregate function")));
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(cloudsync_update_transfn);
Datum cloudsync_update_transfn (PG_FUNCTION_ARGS) {
    MemoryContext aggContext;
    cloudsync_update_payload *payload = NULL;

    if (!AggCheckCallContext(fcinfo, &aggContext)) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("cloudsync_update_transfn called in non-aggregate context")));
    }

    if (PG_ARGISNULL(0)) {
        MemoryContext old = MemoryContextSwitchTo(aggContext);
        payload = (cloudsync_update_payload *)palloc0(sizeof(cloudsync_update_payload));
        MemoryContextSwitchTo(old);
    } else {
        payload = (cloudsync_update_payload *)PG_GETARG_POINTER(0);
    }

    Oid table_type = get_fn_expr_argtype(fcinfo->flinfo, 1);
    bool table_null = PG_ARGISNULL(1);
    Datum table_datum = table_null ? (Datum)0 : PG_GETARG_DATUM(1);
    Oid new_type = get_fn_expr_argtype(fcinfo->flinfo, 2);
    bool new_null = PG_ARGISNULL(2);
    Datum new_datum = new_null ? (Datum)0 : PG_GETARG_DATUM(2);
    Oid old_type = get_fn_expr_argtype(fcinfo->flinfo, 3);
    bool old_null = PG_ARGISNULL(3);
    Datum old_datum = old_null ? (Datum)0 : PG_GETARG_DATUM(3);

    MemoryContext old_ctx = MemoryContextSwitchTo(aggContext);
    pgvalue_t *table_name = pgvalue_create(table_datum, table_type, -1, fcinfo->fncollation, table_null);
    pgvalue_t *new_value = pgvalue_create(new_datum, new_type, -1, fcinfo->fncollation, new_null);
    pgvalue_t *old_value = pgvalue_create(old_datum, old_type, -1, fcinfo->fncollation, old_null);
    if (table_name) pgvalue_ensure_detoast(table_name);
    if (new_value) pgvalue_ensure_detoast(new_value);
    if (old_value) pgvalue_ensure_detoast(old_value);
    MemoryContextSwitchTo(old_ctx);

    if (!table_name || !new_value || !old_value) {
        if (table_name) database_value_free((dbvalue_t *)table_name);
        if (new_value) database_value_free((dbvalue_t *)new_value);
        if (old_value) database_value_free((dbvalue_t *)old_value);
        ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("cloudsync_update_transfn failed to allocate values")));
    }

    if (!cloudsync_update_payload_append(payload, table_name, new_value, old_value)) {
        if (table_name && payload->table_name != table_name) database_value_free((dbvalue_t *)table_name);
        if (new_value) database_value_free((dbvalue_t *)new_value);
        if (old_value) database_value_free((dbvalue_t *)old_value);
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
            if (!database_table_exists(data, meta_name)) {
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
                rc = local_mark_insert_or_update_meta(table, pk, pklen, table_colname(table, i), db_version, cloudsync_bumpseq(data));
                if (rc != DBRES_OK) goto cleanup;
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
