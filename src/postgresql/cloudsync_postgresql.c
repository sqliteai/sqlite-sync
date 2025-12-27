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
#include "utils/memutils.h"
#include "utils/array.h"
#include "pgvalue.h"

// CloudSync headers (after PostgreSQL headers)
#include "../cloudsync.h"
#include "../cloudsync_private.h"
#include "../database.h"
#include "../dbutils.h"
#include "../pk.h"

// Note: network.h is not needed for PostgreSQL implementation

PG_MODULE_MAGIC;

// ============================================================================
// Function Declarations
// ============================================================================

// Extension entry points
void _PG_init(void);
void _PG_fini(void);

// Note: PG_FUNCTION_INFO_V1 macros are declared before each function implementation below
// They should NOT be duplicated here to avoid redefinition errors

#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(X) (void)(X)
#endif

// MARK: - Context Management -

// Global context stored per backend
static cloudsync_context *pg_cloudsync_context = NULL;

// Get or create the CloudSync context for this backend
static cloudsync_context *get_cloudsync_context(void) {
    if (pg_cloudsync_context == NULL) {
        // Create context - db_t is not used in PostgreSQL mode
        pg_cloudsync_context = cloudsync_context_create(NULL);
        if (!pg_cloudsync_context) {
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("Not enough memory to create a database context")));
        }
    }
    return pg_cloudsync_context;
}

// MARK: - Extension Entry Points -

void _PG_init(void) {
    // Extension initialization
    // SPI will be connected per-function call
    elog(DEBUG1, "CloudSync extension loading");

    // Initialize memory debugger (NOOP in production)
    cloudsync_memory_init(1);

    // load config, if exists
    cloudsync_context *ctx = get_cloudsync_context();
    if (cloudsync_config_exists(NULL)) {
        if (cloudsync_context_init(ctx, NULL) == NULL) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("An error occurred while trying to initialize context")));

        }
        
        // make sure to update internal version to current version
        dbutils_settings_set_key_value(NULL, ctx, CLOUDSYNC_KEY_LIBVERSION, CLOUDSYNC_VERSION);
    }
}

void _PG_fini(void) {
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
Datum
cloudsync_version(PG_FUNCTION_ARGS)
{
    UNUSED_PARAMETER(fcinfo);
    PG_RETURN_TEXT_P(cstring_to_text(CLOUDSYNC_VERSION));
}

// cloudsync_siteid() - Get site identifier (UUID)
PG_FUNCTION_INFO_V1(pg_cloudsync_siteid);
Datum
pg_cloudsync_siteid(PG_FUNCTION_ARGS)
{
    UNUSED_PARAMETER(fcinfo);

    cloudsync_context *ctx = get_cloudsync_context();
    const void *siteid = cloudsync_siteid(ctx);

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
Datum
cloudsync_uuid(PG_FUNCTION_ARGS)
{
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
Datum
cloudsync_db_version(PG_FUNCTION_ARGS)
{
    UNUSED_PARAMETER(fcinfo);

    cloudsync_context *ctx = get_cloudsync_context();

    // Connect SPI for database operations
    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("SPI_connect failed: %d", spi_rc)));
    }

    PG_TRY();
    {
        int rc = cloudsync_dbversion_check_uptodate(ctx);
        if (rc != DBRES_OK) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("Unable to retrieve db_version (%s)", database_errmsg(NULL))));
        }

        int64_t version = cloudsync_dbversion(ctx);
        SPI_finish();

        PG_RETURN_INT64(version);
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();
}

// cloudsync_db_version_next([merging_version]) - Get next database version
PG_FUNCTION_INFO_V1(cloudsync_db_version_next);
Datum
cloudsync_db_version_next(PG_FUNCTION_ARGS)
{
    cloudsync_context *ctx = get_cloudsync_context();

    int64_t merging_version = CLOUDSYNC_VALUE_NOTSET;
    if (PG_NARGS() == 1 && !PG_ARGISNULL(0)) {
        merging_version = PG_GETARG_INT64(0);
    }

    // Connect SPI for database operations
    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("SPI_connect failed: %d", spi_rc)));
    }

    PG_TRY();
    {
        int64_t next_version = cloudsync_dbversion_next(ctx, merging_version);
        SPI_finish();

        PG_RETURN_INT64(next_version);
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();
}

// MARK: - Table Initialization -

// Internal helper for cloudsync_init - replicates dbsync_init logic from SQLite
// Returns site_id as text on success, raises error on failure
static text *cloudsync_init_internal(cloudsync_context *ctx, const char *table, const char *algo, bool skip_int_pk_check)
{
    text *result = NULL;

    // Connect SPI for database operations
    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("SPI_connect failed: %d", spi_rc)));
    }

    PG_TRY();
    {
        // Begin savepoint for transactional init
        int rc = database_begin_savepoint(NULL, "cloudsync_init");
        if (rc != DBRES_OK) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("Unable to create cloudsync_init savepoint: %s", database_errmsg(NULL))));
        }

        // Initialize table for sync
        rc = cloudsync_init_table(ctx, table, algo, skip_int_pk_check);
        ereport(DEBUG1, (errmsg("cloudsync_init_internal cloudsync_init_table %d", rc)));

        if (rc == DBRES_OK) {
            rc = database_commit_savepoint(NULL, "cloudsync_init");
            if (rc != DBRES_OK) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("Unable to release cloudsync_init savepoint: %s", database_errmsg(NULL))));
            }
        } else {
            // In case of error, rollback transaction
            database_rollback_savepoint(NULL, "cloudsync_init");

            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("%s", cloudsync_errmsg(ctx))));
        }

        cloudsync_update_schema_hash(ctx);

        // Build site_id as TEXT to return
        char buffer[UUID_STR_MAXLEN];
        cloudsync_uuid_v7_stringify(cloudsync_siteid(ctx), buffer, false);
        result = cstring_to_text(buffer);
        ereport(DEBUG1, (errmsg("cloudsync_init_internal uuid %s", buffer)));

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
Datum
cloudsync_init(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("table_name cannot be NULL")));
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

    cloudsync_context *ctx = get_cloudsync_context();

    // Call internal helper and return site_id as text
    text *result = cloudsync_init_internal(ctx, table, algo, skip_int_pk_check);
    PG_RETURN_TEXT_P(result);
}

// MARK: - Table Enable/Disable Functions -

// Internal helper for enable/disable
static void cloudsync_enable_disable(const char *table_name, bool value) {
    cloudsync_context *ctx = get_cloudsync_context();
    cloudsync_table_context *table = table_lookup(ctx, table_name);
    if (table) {
        table_set_enabled(table, value);
    }
}

// cloudsync_enable - Enable sync for a table
PG_FUNCTION_INFO_V1(cloudsync_enable);
Datum
cloudsync_enable(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("table_name cannot be NULL")));
    }

    const char *table = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_enable_disable(table, true);
    PG_RETURN_BOOL(true);
}

// cloudsync_disable - Disable sync for a table
PG_FUNCTION_INFO_V1(cloudsync_disable);
Datum
cloudsync_disable(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("table_name cannot be NULL")));
    }

    const char *table = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_enable_disable(table, false);
    PG_RETURN_BOOL(true);
}

// cloudsync_is_enabled - Check if table is sync-enabled
PG_FUNCTION_INFO_V1(cloudsync_is_enabled);
Datum
cloudsync_is_enabled(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("table_name cannot be NULL")));
    }

    cloudsync_context *ctx = get_cloudsync_context();
    const char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_table_context *table = table_lookup(ctx, table_name);

    bool result = (table && table_enabled(table));
    PG_RETURN_BOOL(result);
}

// MARK: - Cleanup and Termination -

// cloudsync_cleanup - Cleanup orphaned metadata for a table
PG_FUNCTION_INFO_V1(pg_cloudsync_cleanup);
Datum
pg_cloudsync_cleanup(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("table_name cannot be NULL")));
    }

    const char *table = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_context *ctx = get_cloudsync_context();

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("SPI_connect failed: %d", spi_rc)));
    }

    PG_TRY();
    {
        int rc = cloudsync_cleanup(ctx, table);
        SPI_finish();

        if (rc != DBRES_OK) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("%s", cloudsync_errmsg(ctx))));
        }

        PG_RETURN_BOOL(true);
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();
}

// cloudsync_terminate - Terminate CloudSync
PG_FUNCTION_INFO_V1(pg_cloudsync_terminate);
Datum
pg_cloudsync_terminate(PG_FUNCTION_ARGS)
{
    UNUSED_PARAMETER(fcinfo);

    cloudsync_context *ctx = get_cloudsync_context();

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("SPI_connect failed: %d", spi_rc)));
    }

    PG_TRY();
    {
        int rc = cloudsync_terminate(ctx);
        SPI_finish();
        PG_RETURN_INT32(rc);
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();
}

// MARK: - Settings Functions -

// cloudsync_set - Set global configuration
PG_FUNCTION_INFO_V1(cloudsync_set);
Datum
cloudsync_set(PG_FUNCTION_ARGS)
{
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

    cloudsync_context *ctx = get_cloudsync_context();

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("SPI_connect failed: %d", spi_rc)));
    }

    PG_TRY();
    {
        dbutils_settings_set_key_value(NULL, ctx, key, value);
        SPI_finish();
        PG_RETURN_BOOL(true);
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();
}

// cloudsync_set_table - Set table-level configuration
PG_FUNCTION_INFO_V1(cloudsync_set_table);
Datum
cloudsync_set_table(PG_FUNCTION_ARGS)
{
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

    cloudsync_context *ctx = get_cloudsync_context();

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("SPI_connect failed: %d", spi_rc)));
    }

    PG_TRY();
    {
        dbutils_table_settings_set_key_value(NULL, ctx, tbl, "*", key, value);
        SPI_finish();
        PG_RETURN_BOOL(true);
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();
}

// cloudsync_set_column - Set column-level configuration
PG_FUNCTION_INFO_V1(cloudsync_set_column);
Datum
cloudsync_set_column(PG_FUNCTION_ARGS)
{
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

    cloudsync_context *ctx = get_cloudsync_context();

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("SPI_connect failed: %d", spi_rc)));
    }

    PG_TRY();
    {
        dbutils_table_settings_set_key_value(NULL, ctx, tbl, col, key, value);
        SPI_finish();
        PG_RETURN_BOOL(true);
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();
}

// MARK: - Schema Alteration -

// cloudsync_begin_alter - Begin schema alteration
PG_FUNCTION_INFO_V1(pg_cloudsync_begin_alter);
Datum
pg_cloudsync_begin_alter(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("table_name cannot be NULL")));
    }

    const char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_context *ctx = get_cloudsync_context();

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("SPI_connect failed: %d", spi_rc)));
    }

    PG_TRY();
    {
        int rc = cloudsync_begin_alter(ctx, table_name);
        SPI_finish();

        if (rc != DBRES_OK) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("%s", cloudsync_errmsg(ctx))));
        }

        PG_RETURN_BOOL(true);
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();
}

// cloudsync_commit_alter - Commit schema alteration
PG_FUNCTION_INFO_V1(pg_cloudsync_commit_alter);
Datum
pg_cloudsync_commit_alter(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("table_name cannot be NULL")));
    }

    const char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_context *ctx = get_cloudsync_context();

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("SPI_connect failed: %d", spi_rc)));
    }

    PG_TRY();
    {
        int rc = cloudsync_commit_alter(ctx, table_name);
        SPI_finish();

        if (rc != DBRES_OK) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("%s", cloudsync_errmsg(ctx))));
        }

        PG_RETURN_BOOL(true);
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();
}

// MARK: - Payload Functions -

// Aggregate function: cloudsync_payload_encode transition function
PG_FUNCTION_INFO_V1(cloudsync_payload_encode_transfn);
Datum
cloudsync_payload_encode_transfn(PG_FUNCTION_ARGS)
{
    MemoryContext aggContext;
    cloudsync_payload_context *payload;

    if (!AggCheckCallContext(fcinfo, &aggContext)) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("cloudsync_payload_encode_transfn called in non-aggregate context")));
    }

    // Get or allocate aggregate state
    if (PG_ARGISNULL(0)) {
        MemoryContext oldContext = MemoryContextSwitchTo(aggContext);
        payload = (cloudsync_payload_context *)palloc(cloudsync_payload_context_size(NULL));
        memset(payload, 0, cloudsync_payload_context_size(NULL));
        MemoryContextSwitchTo(oldContext);
    } else {
        payload = (cloudsync_payload_context *)PG_GETARG_POINTER(0);
    }

    cloudsync_context *ctx = get_cloudsync_context();
    int argc = 0;
    pgvalue_t **argv = pgvalues_from_args(fcinfo, 1, &argc, aggContext);

    // Wrap variadic args into pgvalue_t so pk/payload helpers can read types safely.
    if (argc > 0) {
        int rc = cloudsync_payload_encode_step(payload, ctx, argc, (dbvalue_t **)argv);
        if (rc != DBRES_OK) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("%s", cloudsync_errmsg(ctx))));
        }
    }

    // payload_encode_step does not retain pgvalue_t*, free transient wrappers now
    for (int i = 0; i < argc; i++) {
        database_value_free((dbvalue_t *)argv[i]);
    }
    if (argv) pfree(argv);

    PG_RETURN_POINTER(payload);
}

// Aggregate function: cloudsync_payload_encode finalize function
PG_FUNCTION_INFO_V1(cloudsync_payload_encode_finalfn);
Datum
cloudsync_payload_encode_finalfn(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }

    cloudsync_payload_context *payload = (cloudsync_payload_context *)PG_GETARG_POINTER(0);
    cloudsync_context *ctx = get_cloudsync_context();

    int rc = cloudsync_payload_encode_final(payload, ctx);
    if (rc != DBRES_OK) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("%s", cloudsync_errmsg(ctx))));
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
Datum
cloudsync_payload_decode(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("payload cannot be NULL")));
    }

    bytea *payload_data = PG_GETARG_BYTEA_P(0);
    int blen = VARSIZE(payload_data) - VARHDRSZ;

    // Sanity check payload size
    size_t header_size = 0;
    cloudsync_payload_context_size(&header_size);
    if (blen < (int)header_size) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("Invalid payload size")));
    }

    const char *payload = VARDATA(payload_data);
    cloudsync_context *ctx = get_cloudsync_context();

    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("SPI_connect failed: %d", spi_rc)));
    }

    PG_TRY();
    {
        int nrows = 0;
        int rc = cloudsync_payload_apply(ctx, payload, blen, &nrows);
        SPI_finish();

        if (rc != DBRES_OK) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("%s", cloudsync_errmsg(ctx))));
        }

        PG_RETURN_INT32(nrows);
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();
}

// Alias for payload_decode
PG_FUNCTION_INFO_V1(pg_cloudsync_payload_apply);
Datum
pg_cloudsync_payload_apply(PG_FUNCTION_ARGS)
{
    return cloudsync_payload_decode(fcinfo);
}

// MARK: - Private/Internal Functions -

// cloudsync_is_sync - Check if table has sync metadata
PG_FUNCTION_INFO_V1(cloudsync_is_sync);
Datum
cloudsync_is_sync(PG_FUNCTION_ARGS)
{
    cloudsync_context *ctx = get_cloudsync_context();

    if (cloudsync_insync(ctx)) {
        PG_RETURN_BOOL(true);
    }

    if (PG_ARGISNULL(0)) {
        PG_RETURN_BOOL(false);
    }

    const char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_table_context *table = table_lookup(ctx, table_name);

    bool result = (table && (table_enabled(table) == 0));
    PG_RETURN_BOOL(result);
}

// cloudsync_seq - Get sequence number
PG_FUNCTION_INFO_V1(cloudsync_seq);
Datum
cloudsync_seq(PG_FUNCTION_ARGS)
{
    UNUSED_PARAMETER(fcinfo);

    cloudsync_context *ctx = get_cloudsync_context();
    int seq = cloudsync_bumpseq(ctx);

    PG_RETURN_INT32(seq);
}

// cloudsync_pk_encode - Encode primary key from variadic arguments
PG_FUNCTION_INFO_V1(cloudsync_pk_encode);
Datum
cloudsync_pk_encode(PG_FUNCTION_ARGS)
{
    MemoryContext mcxt = CurrentMemoryContext;
    int argc = 0;
    pgvalue_t **argv = NULL;

    // Signature is VARIADIC anyarray, so arg 0 is an array of PK values.
    if (!PG_ARGISNULL(0)) {
        ArrayType *array = PG_GETARG_ARRAYTYPE_P(0);
        argv = pgvalues_from_array(array, &argc, mcxt);
    }

    size_t pklen = 0;
    char *encoded = pk_encode_prikey((dbvalue_t **)argv, argc, NULL, &pklen);
    if (!encoded) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("cloudsync_pk_encode failed to encode primary key")));
    }

    text *result = cstring_to_text_with_len(encoded, pklen);
    cloudsync_memory_free(encoded);

    for (int i = 0; i < argc; i++) {
        database_value_free((dbvalue_t *)argv[i]);
    }
    if (argv) pfree(argv);

    PG_RETURN_TEXT_P(result);
}

// cloudsync_pk_decode - Decode primary key component at given index
PG_FUNCTION_INFO_V1(cloudsync_pk_decode);
Datum
cloudsync_pk_decode(PG_FUNCTION_ARGS)
{
    // TODO: Implement pk_decode with callback pattern
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("cloudsync_pk_decode not yet implemented - requires callback implementation")));
    PG_RETURN_NULL();
}

// cloudsync_insert - Internal insert handler
// Signature: cloudsync_insert(table_name text, VARIADIC pk_values anyarray)
PG_FUNCTION_INFO_V1(cloudsync_insert);
Datum
cloudsync_insert(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("table_name cannot be NULL")));
    }

    const char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    cloudsync_context *ctx = get_cloudsync_context();

    // Lookup table
    cloudsync_table_context *table = table_lookup(ctx, table_name);
    if (!table) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("Unable to retrieve table name %s in cloudsync_insert", table_name)));
    }

    // Extract PK values from VARIADIC anyarray (arg 1)
    int argc = 0;
    pgvalue_t **argv = NULL;
    MemoryContext mcxt = CurrentMemoryContext;

    if (!PG_ARGISNULL(1)) {
        ArrayType *pk_array = PG_GETARG_ARRAYTYPE_P(1);
        argv = pgvalues_from_array(pk_array, &argc, mcxt);
    }

    // Verify we have the correct number of PK columns
    int expected_pks = table_count_pks(table);
    if (argc != expected_pks) {
        // Cleanup before error
        for (int i = 0; i < argc; i++) {
            database_value_free((dbvalue_t *)argv[i]);
        }
        if (argv) pfree(argv);

        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("Expected %d primary key values, got %d", expected_pks, argc)));
    }

    // Connect SPI for database operations
    int spi_rc = SPI_connect();
    if (spi_rc != SPI_OK_CONNECT) {
        // Cleanup before error
        for (int i = 0; i < argc; i++) {
            database_value_free((dbvalue_t *)argv[i]);
        }
        if (argv) pfree(argv);

        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("SPI_connect failed: %d", spi_rc)));
    }

    PG_TRY();
    {
        // Encode the primary key values into a buffer
        char buffer[1024];
        size_t pklen = sizeof(buffer);
        char *pk = pk_encode_prikey((dbvalue_t **)argv, argc, buffer, &pklen);

        if (!pk) {
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("Not enough memory to encode the primary key(s)")));
        }

        // Compute the next database version for tracking changes
        int64_t db_version = cloudsync_dbversion_next(ctx, CLOUDSYNC_VALUE_NOTSET);

        // Check if a row with the same primary key already exists
        // (if so, this might be a previously deleted sentinel)
        bool pk_exists = table_pk_exists(table, pk, pklen);
        int rc = DBRES_OK;

        if (table_count_cols(table) == 0) {
            // If there are no columns other than primary keys, insert a sentinel record
            rc = local_mark_insert_sentinel_meta(table, pk, pklen, db_version, cloudsync_bumpseq(ctx));
            if (rc != DBRES_OK) goto cleanup;
        } else if (pk_exists) {
            // If a row with the same primary key already exists, update the sentinel record
            rc = local_update_sentinel(table, pk, pklen, db_version, cloudsync_bumpseq(ctx));
            if (rc != DBRES_OK) goto cleanup;
        }

        // Process each non-primary key column for insert or update
        for (int i = 0; i < table_count_cols(table); i++) {
            rc = local_mark_insert_or_update_meta(table, pk, pklen, table_colname(table, i),
                                                   db_version, cloudsync_bumpseq(ctx));
            if (rc != DBRES_OK) goto cleanup;
        }

    cleanup:
        // Free memory if the primary key was dynamically allocated
        if (pk != buffer) cloudsync_memory_free(pk);

        // Free pgvalue_t wrappers
        for (int i = 0; i < argc; i++) {
            database_value_free((dbvalue_t *)argv[i]);
        }
        if (argv) pfree(argv);

        SPI_finish();

        if (rc != DBRES_OK) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("%s", database_errmsg(NULL))));
        }

        PG_RETURN_BOOL(true);
    }
    PG_CATCH();
    {
        // Cleanup on exception
        for (int i = 0; i < argc; i++) {
            database_value_free((dbvalue_t *)argv[i]);
        }
        if (argv) pfree(argv);

        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();
}

// Aggregate function: cloudsync_update (not implemented - complex)
PG_FUNCTION_INFO_V1(cloudsync_update);
Datum
cloudsync_update(PG_FUNCTION_ARGS)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("cloudsync_update not yet implemented - aggregate function")));
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(cloudsync_update_transfn);
Datum
cloudsync_update_transfn(PG_FUNCTION_ARGS)
{
    // TODO: Implement update aggregate transition function
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("cloudsync_update_transfn not yet implemented")));
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(cloudsync_update_finalfn);
Datum
cloudsync_update_finalfn(PG_FUNCTION_ARGS)
{
    // TODO: Implement update aggregate finalize function
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("cloudsync_update_finalfn not yet implemented")));
    PG_RETURN_NULL();
}

// Placeholder - not implemented yet
PG_FUNCTION_INFO_V1(cloudsync_payload_encode);
Datum
cloudsync_payload_encode(PG_FUNCTION_ARGS)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("cloudsync_payload_encode should not be called directly - use aggregate version")));
    PG_RETURN_NULL();
}
