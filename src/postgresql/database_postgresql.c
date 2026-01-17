//
//  database_postgresql.c
//  cloudsync
//
//  Created by Marco Bambini on 03/12/25.
//

// PostgreSQL requires postgres.h to be included FIRST
// It sets up the entire environment including platform compatibility
#include "postgres.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "../cloudsync.h"
#include "../database.h"
#include "../dbutils.h"
#include "../sql.h"
#include "../utils.h"

// PostgreSQL SPI and other headers
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "pgvalue.h"

// ============================================================================
// SPI CONNECTION REQUIREMENTS
// ============================================================================
//
// IMPORTANT: This implementation requires an active SPI connection to function.
// The Extension Function that calls these functions MUST:
//
// 1. Call SPI_connect() before using any database functions
// 2. Call SPI_finish() before returning from the extension function
//
// ============================================================================

// MARK: - PREPARED STATEMENTS -

// PostgreSQL SPI handles require knowing parameter count and types upfront.
// Solution: Defer actual SPI_prepare until first step(), after all bindings are set.
#define MAX_PARAMS 32

typedef struct {
    // Prepared plan
    SPIPlanPtr      plan;
    bool            plan_is_prepared;

    // Cursor execution
    Portal          portal;             // owned by statement
    bool            portal_open;
    
    // Current fetched batch (we fetch 1 row at a time, but SPI still returns a tuptable)
    SPITupleTable  *last_tuptable;      // must SPI_freetuptable() before next fetch
    HeapTuple       current_tuple;
    TupleDesc       current_tupdesc;

    // Params
    int             nparams;
    Oid             types[MAX_PARAMS];
    Datum           values[MAX_PARAMS];
    char            nulls[MAX_PARAMS];
    bool            executed_nonselect; // non-select executed already

    // Memory
    MemoryContext   stmt_mcxt;          // lifetime = pg_stmt_t
    MemoryContext   bind_mcxt;          // resettable region for parameters (cleared on clear_bindings/reset)
    MemoryContext   row_mcxt;           // per-row scratch (cleared each step after consumer copies)

    // Context
    const char        *sql;
    cloudsync_context *data;
} pg_stmt_t;

// MARK: - SQL -

char *sql_build_drop_table (const char *table_name, char *buffer, int bsize, bool is_meta) {
    // Escape the table name (doubles any embedded quotes)
    char escaped[512];
    sql_escape_name(table_name, escaped, sizeof(escaped));

    // Add the surrounding quotes in the format string
    if (is_meta) {
        snprintf(buffer, bsize, "DROP TABLE IF EXISTS \"%s_cloudsync\";", escaped);
    } else {
        snprintf(buffer, bsize, "DROP TABLE IF EXISTS \"%s\";", escaped);
    }

    return buffer;
}

char *sql_escape_name (const char *name, char *buffer, size_t bsize) {
    // PostgreSQL identifier escaping: double any embedded double quotes
    // Does NOT add surrounding quotes (caller's responsibility)
    // Similar to SQLite's %q behavior for escaping

    if (!name || !buffer || bsize < 1) {
        if (buffer && bsize > 0) buffer[0] = '\0';
        return NULL;
    }

    size_t i = 0, j = 0;

    while (name[i]) {
        if (name[i] == '"') {
            // Need space for 2 chars (escaped quote) + null
            if (j >= bsize - 2) {
                elog(WARNING, "Identifier name too long for buffer, truncated: %s", name);
                break;
            }
            buffer[j++] = '"';
            buffer[j++] = '"';
        } else {
            // Need space for 1 char + null
            if (j >= bsize - 1) {
                elog(WARNING, "Identifier name too long for buffer, truncated: %s", name);
                break;
            }
            buffer[j++] = name[i];
        }
        i++;
    }

    buffer[j] = '\0';
    return buffer;
}

char *sql_build_select_nonpk_by_pk (cloudsync_context *data, const char *table_name) {
    char *sql = cloudsync_memory_mprintf(SQL_BUILD_SELECT_NONPK_COLS_BY_PK, table_name);
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

char *sql_build_delete_by_pk (cloudsync_context *data, const char *table_name) {
    char *sql = cloudsync_memory_mprintf(SQL_BUILD_DELETE_ROW_BY_PK, table_name);
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

char *sql_build_insert_pk_ignore (cloudsync_context *data, const char *table_name) {
    char *sql = cloudsync_memory_mprintf(SQL_BUILD_INSERT_PK_IGNORE, table_name);
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

char *sql_build_upsert_pk_and_col (cloudsync_context *data, const char *table_name, const char *colname) {
    char *sql = cloudsync_memory_mprintf(SQL_BUILD_UPSERT_PK_AND_COL, table_name, colname);
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

char *sql_build_select_cols_by_pk (cloudsync_context *data, const char *table_name, const char *colname) {
    char *sql = cloudsync_memory_mprintf(SQL_BUILD_SELECT_COLS_BY_PK_FMT, table_name, colname);
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

char *sql_build_rekey_pk_and_reset_version_except_col (cloudsync_context *data, const char *table_name, const char *except_col) {
    UNUSED_PARAMETER(data);
    char escaped[512];
    sql_escape_name(table_name, escaped, sizeof(escaped));
    
    return cloudsync_memory_mprintf(SQL_CLOUDSYNC_REKEY_PK_AND_RESET_VERSION_EXCEPT_COL, escaped, except_col, escaped, escaped, except_col);
}

// MARK: - HELPER FUNCTIONS -

// TODO: is this really necessary? We now control the SQL statements and so we can use the Postgres style when needed
// Convert SQLite-style ? placeholders to PostgreSQL-style $1, $2, etc.
/*
static char* convert_placeholders(const char *sql) {
    if (!sql) {
        return NULL;
    }

    // Count placeholders
    int count = 0;
    for (const char *p = sql; *p; p++) {
        if (*p == '?') count++;
    }

    // Allocate new string (worst case: $999 for each ? = 4 chars vs 1)
    size_t newlen = strlen(sql) + (count * 3) + 1;
    char *newsql = cloudsync_memory_alloc(newlen);

    // Convert
    char *dst = newsql;
    int param_num = 1;
    for (const char *src = sql; *src; src++) {
        if (*src == '?') {
            dst += sprintf(dst, "$%d", param_num++);
        } else {
            *dst++ = *src;
        }
    }
    *dst = '\0';

    return newsql;
}
 */

// Map SPI result codes to DBRES
static int map_spi_result (int rc) {
    switch (rc) {
        case SPI_OK_SELECT:
        case SPI_OK_INSERT:
        case SPI_OK_UPDATE:
        case SPI_OK_DELETE:
        case SPI_OK_UTILITY:
            return DBRES_OK;
        case SPI_OK_INSERT_RETURNING:
        case SPI_OK_UPDATE_RETURNING:
        case SPI_OK_DELETE_RETURNING:
            return DBRES_ROW;
        default:
            return DBRES_ERROR;
    }
}

static void clear_fetch_batch (pg_stmt_t *stmt) {
    if (!stmt) return;
    if (stmt->last_tuptable) {
        SPI_freetuptable(stmt->last_tuptable);
        stmt->last_tuptable = NULL;
    }
    stmt->current_tuple = NULL;
    stmt->current_tupdesc = NULL;
    if (stmt->row_mcxt) MemoryContextReset(stmt->row_mcxt);
}

static void close_portal (pg_stmt_t *stmt) {
    if (!stmt) return;
    if (stmt->portal) {
        SPI_cursor_close(stmt->portal);
        stmt->portal = NULL;
    }
    stmt->portal_open = false;
}

static inline Datum get_datum (pg_stmt_t *stmt, int col /* 0-based */, bool *isnull, Oid *type) {
    if (!stmt || !stmt->current_tuple || !stmt->current_tupdesc) {
        if (isnull) *isnull = true;
        if (type) *type = 0;
        return (Datum) 0;
    }
    if (type) *type = SPI_gettypeid(stmt->current_tupdesc, col + 1);
    return SPI_getbinval(stmt->current_tuple, stmt->current_tupdesc, col + 1, isnull);
}

// MARK: - PRIVATE -

int database_select1_value (cloudsync_context *data, const char *sql, char **ptr_value, int64_t *int_value, DBTYPE expected_type) {
    cloudsync_reset_error(data);
    
    // init values and sanity check expected_type
    if (ptr_value) *ptr_value = NULL;
    *int_value = 0;
    if (expected_type != DBTYPE_INTEGER && expected_type != DBTYPE_TEXT && expected_type != DBTYPE_BLOB) {
        return cloudsync_set_error(data, "Invalid expected_type", DBRES_MISUSE);
    }

    int rc = SPI_execute(sql, true, 0);
    if (rc < 0) {
        return cloudsync_set_error(data, "SPI_execute failed in database_select1_value", DBRES_ERROR);
    }

    // ensure at least one column
    if (!SPI_tuptable || !SPI_tuptable->tupdesc) {
        return cloudsync_set_error(data, "No result table", DBRES_ERROR);
    }
    if (SPI_tuptable->tupdesc->natts < 1) {
        return cloudsync_set_error(data, "No columns in result", DBRES_ERROR);
    }

    // no rows OK
    if (SPI_processed == 0) {
        return DBRES_OK;
    }

    HeapTuple tuple = SPI_tuptable->vals[0];
    bool isnull;
    Datum datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, &isnull);

    // NULL value is OK
    if (isnull) {
        return DBRES_OK;
    }

    // Get type info
    Oid typeid = SPI_gettypeid(SPI_tuptable->tupdesc, 1);

    if (expected_type == DBTYPE_INTEGER) {
        switch (typeid) {
            case INT2OID:
                *int_value = (int64_t)DatumGetInt16(datum);
                break;
            case INT4OID:
                *int_value = (int64_t)DatumGetInt32(datum);
                break;
            case INT8OID:
                *int_value = DatumGetInt64(datum);
                break;
            default:
                return cloudsync_set_error(data, "Type mismatch: expected integer", DBRES_ERROR);
        }
    } else if (expected_type == DBTYPE_TEXT) {
        text *txt = DatumGetTextP(datum);
        int len = VARSIZE(txt) - VARHDRSZ;
        if (len > 0) {
            char *ptr = cloudsync_memory_alloc(len + 1);
            if (!ptr) {
                return cloudsync_set_error(data, "Memory allocation failed", DBRES_NOMEM);
            }
            memcpy(ptr, VARDATA(txt), len);
            ptr[len] = '\0';
            *ptr_value = ptr;
            *int_value = len;
        }
    } else if (expected_type == DBTYPE_BLOB) {
        bytea *ba = DatumGetByteaP(datum);
        int len = VARSIZE(ba) - VARHDRSZ;
        if (len > 0) {
            char *ptr = cloudsync_memory_alloc(len);
            if (!ptr) {
                return cloudsync_set_error(data, "Memory allocation failed", DBRES_NOMEM);
            }
            memcpy(ptr, VARDATA(ba), len);
            *ptr_value = ptr;
            *int_value = len;
        }
    }

    return DBRES_OK;
}

int database_select3_values (cloudsync_context *data, const char *sql, char **value, int64_t *len, int64_t *value2, int64_t *value3) {
    cloudsync_reset_error(data);
    
    // init values
    *value = NULL;
    *value2 = 0;
    *value3 = 0;
    *len = 0;

    int rc = SPI_execute(sql, true, 0);
    if (rc < 0) return DBRES_ERROR;

    if (!SPI_tuptable || !SPI_tuptable->tupdesc) return DBRES_ERROR;
    if (SPI_tuptable->tupdesc->natts < 3) return DBRES_ERROR;
    if (SPI_processed == 0) return DBRES_OK;

    HeapTuple tuple = SPI_tuptable->vals[0];
    bool isnull;

    // First column - text/blob
    Datum datum1 = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, &isnull);
    if (!isnull) {
        Oid typeid = SPI_gettypeid(SPI_tuptable->tupdesc, 1);
        if (typeid == BYTEAOID) {
            bytea *ba = DatumGetByteaP(datum1);
            int blob_len = VARSIZE(ba) - VARHDRSZ;
            if (blob_len > 0) {
                char *ptr = cloudsync_memory_alloc(blob_len);
                if (!ptr) return DBRES_NOMEM;
                
                memcpy(ptr, VARDATA(ba), blob_len);
                *value = ptr;
                *len = blob_len;
            }
        } else {
            text *txt = DatumGetTextP(datum1);
            int text_len = VARSIZE(txt) - VARHDRSZ;
            if (text_len > 0) {
                char *ptr = cloudsync_memory_alloc(text_len + 1);
                if (!ptr) return DBRES_NOMEM;
                
                memcpy(ptr, VARDATA(txt), text_len);
                ptr[text_len] = '\0';
                *value = ptr;
                *len = text_len;
            }
        }
    }

    // Second column - int
    Datum datum2 = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 2, &isnull);
    if (!isnull) {
        Oid typeid = SPI_gettypeid(SPI_tuptable->tupdesc, 2);
        if (typeid == INT8OID) {
            *value2 = DatumGetInt64(datum2);
        } else if (typeid == INT4OID) {
            *value2 = (int64_t)DatumGetInt32(datum2);
        }
    }

    // Third column - int
    Datum datum3 = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 3, &isnull);
    if (!isnull) {
        Oid typeid = SPI_gettypeid(SPI_tuptable->tupdesc, 3);
        if (typeid == INT8OID) {
            *value3 = DatumGetInt64(datum3);
        } else if (typeid == INT4OID) {
            *value3 = (int64_t)DatumGetInt32(datum3);
        }
    }

    return DBRES_OK;
}

bool database_system_exists (cloudsync_context *data, const char *name, const char *type) {
    if (!name || !type) return false;
    cloudsync_reset_error(data);
 
    char query[512];
    bool exists = false;

    if (strcmp(type, "table") == 0) {
        snprintf(query, sizeof(query), "SELECT 1 FROM pg_tables WHERE schemaname = 'public' AND tablename = '%s'", name);
    } else if (strcmp(type, "trigger") == 0) {
        snprintf(query, sizeof(query), "SELECT 1 FROM pg_trigger WHERE tgname = '%s'", name);
    } else {
        return false;
    }

    PG_TRY();
    {
        int rc = SPI_execute(query, true, 0);
        exists = (rc >= 0 && SPI_processed > 0);
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        cloudsync_set_error(data, edata->message, DBRES_ERROR);
        FreeErrorData(edata);
        FlushErrorState();
        exists = false;
    }
    PG_END_TRY();

    elog(DEBUG1, "database_system_exists %s: %d", name, exists);
    return exists;
}

// MARK: - GENERAL -

int database_exec (cloudsync_context *data, const char *sql) {
    if (!sql) return cloudsync_set_error(data, "SQL statement is NULL", DBRES_ERROR);
    cloudsync_reset_error(data);
    
    int rc;
    bool is_error = false;
    PG_TRY();
    {
        rc = SPI_execute(sql, false, 0);
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        rc = cloudsync_set_error(data, edata->message, DBRES_ERROR);
        FreeErrorData(edata);
        FlushErrorState();
        is_error = true;
    }
    PG_END_TRY();

    if (is_error) return rc;

    // Increment command counter to make changes visible
    if (rc >= 0) {
        CommandCounterIncrement();

        // Refresh snapshot to ensure subsequent reads see the changes
        if (ActiveSnapshotSet()) {
            PopActiveSnapshot();
        }
        PushActiveSnapshot(GetTransactionSnapshot());

        // Clear error on success
        return map_spi_result(rc);
    }

    return cloudsync_set_error(data, "SPI_execute failed", DBRES_ERROR);
}

int database_exec_callback (cloudsync_context *data, const char *sql, int (*callback)(void *xdata, int argc, char **values, char **names), void *xdata) {
    if (!sql) return cloudsync_set_error(data, "SQL statement is NULL", DBRES_ERROR);
    cloudsync_reset_error(data);
    
    int rc;
    bool is_error = false;
    PG_TRY();
    { 
        rc = SPI_execute(sql, true, 0);
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        rc = cloudsync_set_error(data, edata->message, DBRES_ERROR);
        FreeErrorData(edata);
        FlushErrorState();
        is_error = true;
    }
    PG_END_TRY();

    if (is_error) return rc;
    if (rc < 0) return cloudsync_set_error(data, "SPI_execute failed", DBRES_ERROR);

    // Call callback for each row if provided
    if (callback && SPI_tuptable) {
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        int ncols = tupdesc->natts;

        // Allocate arrays for column names and values
        char **names = cloudsync_memory_alloc(ncols * sizeof(char*));
        if (!names) return DBRES_NOMEM;
        char **values = cloudsync_memory_alloc(ncols * sizeof(char*));
        if (!values) {cloudsync_memory_free(names); return DBRES_NOMEM;}

        // Get column names
        for (int i = 0; i < ncols; i++) {
            names[i] = NameStr(TupleDescAttr(tupdesc, i)->attname);
        }

        // Process each row
        for (uint64 row = 0; row < SPI_processed; row++) {
            HeapTuple tuple = SPI_tuptable->vals[row];

            // Get values for this row
            for (int i = 0; i < ncols; i++) {
                bool isnull;
                SPI_getbinval(tuple, tupdesc, i + 1, &isnull);
                values[i] = (isnull) ? NULL : SPI_getvalue(tuple, tupdesc, i + 1);
            }

            // Call user callback
            int cb_rc = callback(xdata, ncols, values, names);

            // Cleanup values
            for (int i = 0; i < ncols; i++) {
                if (values[i]) {
                    pfree(values[i]);
                    values[i] = NULL;
                }
            }
            
            if (cb_rc != 0) {
                cloudsync_memory_free(names);
                cloudsync_memory_free(values);
                char errmsg[1024];
                snprintf(errmsg, sizeof(errmsg), "database_exec_callback aborted %d", cb_rc);
                return cloudsync_set_error(data, errmsg, DBRES_ABORT);
            }
        }

        cloudsync_memory_free(names);
        cloudsync_memory_free(values);
    }

    return DBRES_OK;
}

int database_write (cloudsync_context *data, const char *sql, const char **bind_values, DBTYPE bind_types[], int bind_lens[], int bind_count) {
    if (!sql) return cloudsync_set_error(data, "Invalid parameters to database_write", DBRES_ERROR);
    cloudsync_reset_error(data);
    
    // Prepare statement
    dbvm_t *stmt;
    int rc = databasevm_prepare(data, sql, &stmt, 0);
    if (rc != DBRES_OK) return rc;

    // Bind parameters
    for (int i = 0; i < bind_count; i++) {
        int param_idx = i + 1;

        switch (bind_types[i]) {
            case DBTYPE_NULL:
                rc = databasevm_bind_null(stmt, param_idx);
                break;
            case DBTYPE_INTEGER: {
                int64_t val = strtoll(bind_values[i], NULL, 0);
                rc = databasevm_bind_int(stmt, param_idx, val);
                break;
            }
            case DBTYPE_FLOAT: {
                double val = strtod(bind_values[i], NULL);
                rc = databasevm_bind_double(stmt, param_idx, val);
                break;
            }
            case DBTYPE_TEXT:
                rc = databasevm_bind_text(stmt, param_idx, bind_values[i], bind_lens[i]);
                break;
            case DBTYPE_BLOB:
                rc = databasevm_bind_blob(stmt, param_idx, bind_values[i], bind_lens[i]);
                break;
            default:
                rc = DBRES_ERROR;
                break;
        }

        if (rc != DBRES_OK) {
            databasevm_finalize(stmt);
            return rc;
        }
    }

    // Execute
    rc = databasevm_step(stmt);
    databasevm_finalize(stmt);

    return (rc == DBRES_DONE) ? DBRES_OK : rc;
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

int database_select_blob_2int (cloudsync_context *data, const char *sql, char **value, int64_t *len, int64_t *value2, int64_t *value3) {
    return database_select3_values(data, sql, value, len, value2, value3);
}

// MARK: - STATUS -
int database_errcode (cloudsync_context *data) {
    return cloudsync_errcode(data);
}

const char *database_errmsg (cloudsync_context *data) {
    return cloudsync_errmsg(data);
}

bool database_in_transaction (cloudsync_context *data) {
    // In SPI context, we're always in a transaction
    return IsTransactionState();
}

bool database_table_exists (cloudsync_context *data, const char *name) {
    return database_system_exists(data, name, "table");
}

bool database_trigger_exists (cloudsync_context *data, const char *name) {
    return database_system_exists(data, name, "trigger");
}

// MARK: - SCHEMA INFO -

int database_count_pk (cloudsync_context *data, const char *table_name, bool not_null) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM information_schema.table_constraints tc "
             "JOIN information_schema.key_column_usage kcu ON tc.constraint_name = kcu.constraint_name "
             "WHERE tc.table_name = '%s' AND tc.constraint_type = 'PRIMARY KEY'",
             table_name);

    int64_t count = 0;
    database_select_int(data, sql, &count);
    return (int)count;
}

int database_count_nonpk (cloudsync_context *data, const char *table_name) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM information_schema.columns c "
             "WHERE c.table_name = '%s' "
             "AND c.column_name NOT IN ("
             "  SELECT kcu.column_name FROM information_schema.table_constraints tc "
             "  JOIN information_schema.key_column_usage kcu ON tc.constraint_name = kcu.constraint_name "
             "  WHERE tc.table_name = '%s' AND tc.constraint_type = 'PRIMARY KEY'"
             ")",
             table_name, table_name);

    int64_t count = 0;
    database_select_int(data, sql, &count);
    return (int)count;
}

int database_count_int_pk (cloudsync_context *data, const char *table_name) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM information_schema.columns c "
             "JOIN information_schema.key_column_usage kcu ON c.column_name = kcu.column_name "
             "JOIN information_schema.table_constraints tc ON kcu.constraint_name = tc.constraint_name "
             "WHERE c.table_name = '%s' AND tc.constraint_type = 'PRIMARY KEY' "
             "AND c.data_type IN ('smallint', 'integer', 'bigint')",
             table_name);

    int64_t count = 0;
    database_select_int(data, sql, &count);
    return (int)count;
}

int database_count_notnull_without_default (cloudsync_context *data, const char *table_name) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM information_schema.columns c "
             "WHERE c.table_name = '%s' "
             "AND c.is_nullable = 'NO' "
             "AND c.column_default IS NULL "
             "AND c.column_name NOT IN ("
             "  SELECT kcu.column_name FROM information_schema.table_constraints tc "
             "  JOIN information_schema.key_column_usage kcu ON tc.constraint_name = kcu.constraint_name "
             "  WHERE tc.table_name = '%s' AND tc.constraint_type = 'PRIMARY KEY'"
             ")",
             table_name, table_name);

    int64_t count = 0;
    database_select_int(data, sql, &count);
    return (int)count;
}

/*
int database_debug (db_t *db, bool print_result) {
    // PostgreSQL debug information
    if (print_result) {
        elog(DEBUG1, "PostgreSQL SPI debug info:");
        elog(DEBUG1, "  SPI_processed: %lu", (unsigned long)SPI_processed);
        elog(DEBUG1, "  In transaction: %d", IsTransactionState());
    }
    return DBRES_OK;
}
 */

// MARK: - METADATA TABLES -

int database_create_metatable (cloudsync_context *data, const char *table_name) {
    char sql[2048];
    int rc;

    // Create the metadata table
    snprintf(sql, sizeof(sql),
             "CREATE TABLE IF NOT EXISTS \"%s_cloudsync\" ("
             "pk BYTEA NOT NULL,"
             "col_name TEXT NOT NULL,"
             "col_version BIGINT,"
             "db_version BIGINT NOT NULL DEFAULT 0,"
             "seq INTEGER NOT NULL DEFAULT 0,"
             "site_id BIGINT NOT NULL DEFAULT 0,"
             "PRIMARY KEY (pk, col_name)"
             ");",
             table_name);

    rc = database_exec(data, sql);
    if (rc != DBRES_OK) return rc;

    // Create indices for performance
    snprintf(sql, sizeof(sql),
             "CREATE INDEX IF NOT EXISTS \"%s_cloudsync_db_version_idx\" "
             "ON \"%s_cloudsync\" (db_version);",
             table_name, table_name);

    rc = database_exec(data, sql);
    return rc;
}

// MARK: - TRIGGERS -

int database_create_insert_trigger (cloudsync_context *data, const char *table_name, char *trigger_when) {
    if (!table_name) return DBRES_MISUSE;

    char trigger_name[1024];
    char func_name[1024];
    snprintf(trigger_name, sizeof(trigger_name), "cloudsync_after_insert_%s", table_name);
    snprintf(func_name, sizeof(func_name), "cloudsync_after_insert_%s_fn", table_name);

    if (database_trigger_exists(data, trigger_name)) return DBRES_OK;

    char sql[2048];
    snprintf(sql, sizeof(sql),
             "SELECT string_agg('NEW.' || quote_ident(kcu.column_name), ',' ORDER BY kcu.ordinal_position) "
             "FROM information_schema.table_constraints tc "
             "JOIN information_schema.key_column_usage kcu "
             "  ON tc.constraint_name = kcu.constraint_name "
             "WHERE tc.table_name = '%s' AND tc.constraint_type = 'PRIMARY KEY';",
             table_name);

    char *pk_list = NULL;
    int rc = database_select_text(data, sql, &pk_list);
    if (rc != DBRES_OK) return rc;
    if (!pk_list || pk_list[0] == '\0') {
        if (pk_list) cloudsync_memory_free(pk_list);
        return cloudsync_set_error(data, "No primary key columns found for table", DBRES_ERROR);
    }

    char *sql2 = cloudsync_memory_mprintf(
        "CREATE OR REPLACE FUNCTION %s() RETURNS trigger AS $$ "
        "BEGIN "
        "  IF cloudsync_is_sync('%s') THEN RETURN NEW; END IF; "
        "  PERFORM cloudsync_insert('%s', VARIADIC ARRAY[%s]); "
        "  RETURN NEW; "
        "END; "
        "$$ LANGUAGE plpgsql;",
        func_name, table_name, table_name, pk_list);
    cloudsync_memory_free(pk_list);
    if (!sql2) return DBRES_NOMEM;

    rc = database_exec(data, sql2);
    cloudsync_memory_free(sql2);
    if (rc != DBRES_OK) return rc;

    sql2 = cloudsync_memory_mprintf(
        "CREATE TRIGGER %s AFTER INSERT ON \"%s\" %s "
        "EXECUTE FUNCTION %s();",
        trigger_name, table_name, trigger_when ? trigger_when : "", func_name);
    if (!sql2) return DBRES_NOMEM;

    rc = database_exec(data, sql2);
    cloudsync_memory_free(sql2);
    return rc;
}

int database_create_update_trigger_gos (cloudsync_context *data, const char *table_name) {
    if (!table_name) return DBRES_MISUSE;

    char trigger_name[1024];
    char func_name[1024];
    snprintf(trigger_name, sizeof(trigger_name), "cloudsync_before_update_%s", table_name);
    snprintf(func_name, sizeof(func_name), "cloudsync_before_update_%s_fn", table_name);

    if (database_trigger_exists(data, trigger_name)) return DBRES_OK;

    char *sql = cloudsync_memory_mprintf(
        "CREATE OR REPLACE FUNCTION %s() RETURNS trigger AS $$ "
        "BEGIN "
        "  RAISE EXCEPTION 'Error: UPDATE operation is not allowed on table %s.'; "
        "END; "
        "$$ LANGUAGE plpgsql;",
        func_name, table_name);
    if (!sql) return DBRES_NOMEM;

    int rc = database_exec(data, sql);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) return rc;

    sql = cloudsync_memory_mprintf(
        "CREATE TRIGGER %s BEFORE UPDATE ON \"%s\" "
        "FOR EACH ROW WHEN (cloudsync_is_enabled('%s') = true) "
        "EXECUTE FUNCTION %s();",
        trigger_name, table_name, table_name, func_name);
    if (!sql) return DBRES_NOMEM;

    rc = database_exec(data, sql);
    cloudsync_memory_free(sql);
    return rc;
}

int database_create_update_trigger (cloudsync_context *data, const char *table_name, const char *trigger_when) {
    if (!table_name) return DBRES_MISUSE;

    char trigger_name[1024];
    char func_name[1024];
    snprintf(trigger_name, sizeof(trigger_name), "cloudsync_after_update_%s", table_name);
    snprintf(func_name, sizeof(func_name), "cloudsync_after_update_%s_fn", table_name);

    if (database_trigger_exists(data, trigger_name)) return DBRES_OK;

    char sql[2048];
    snprintf(sql, sizeof(sql),
           "SELECT string_agg("
           "  '(''%s'', NEW.' || quote_ident(kcu.column_name) || ', OLD.' || "
           "quote_ident(kcu.column_name) || ')', "
           "  ', ' ORDER BY kcu.ordinal_position"
           ") "
           "FROM information_schema.table_constraints tc "
           "JOIN information_schema.key_column_usage kcu "
           "  ON tc.constraint_name = kcu.constraint_name "
           "WHERE tc.table_name = '%s' AND tc.constraint_type = 'PRIMARY KEY';",
           table_name, table_name);

    char *pk_values_list = NULL;
    int rc = database_select_text(data, sql, &pk_values_list);
    if (rc != DBRES_OK) return rc;
    if (!pk_values_list || pk_values_list[0] == '\0') {
        if (pk_values_list) cloudsync_memory_free(pk_values_list);
        return cloudsync_set_error(data, "No primary key columns found for table", DBRES_ERROR);
    }

    snprintf(sql, sizeof(sql),
           "SELECT string_agg("
           "  '(''%s'', NEW.' || quote_ident(c.column_name) || ', OLD.' || "
           "quote_ident(c.column_name) || ')', "
           "  ', ' ORDER BY c.ordinal_position"
           ") "
           "FROM information_schema.columns c "
           "WHERE c.table_name = '%s' "
           "AND NOT EXISTS ("
           "  SELECT 1 FROM information_schema.table_constraints tc "
           "  JOIN information_schema.key_column_usage kcu "
           "    ON tc.constraint_name = kcu.constraint_name "
           "  WHERE tc.table_name = c.table_name "
           "  AND tc.constraint_type = 'PRIMARY KEY' "
           "  AND kcu.column_name = c.column_name"
           ");",
           table_name, table_name);

    char *col_values_list = NULL;
    rc = database_select_text(data, sql, &col_values_list);
    if (rc != DBRES_OK) {
        if (pk_values_list) cloudsync_memory_free(pk_values_list);
        return rc;
    }

    char *values_query = NULL;
    if (col_values_list && col_values_list[0] != '\0') {
        values_query = cloudsync_memory_mprintf("VALUES %s, %s", pk_values_list, col_values_list);
    } else {
        values_query = cloudsync_memory_mprintf("VALUES %s", pk_values_list);
    }

    if (pk_values_list) cloudsync_memory_free(pk_values_list);
    if (col_values_list) cloudsync_memory_free(col_values_list);
    if (!values_query) return DBRES_NOMEM;

    char *sql2 = cloudsync_memory_mprintf(
        "CREATE OR REPLACE FUNCTION %s() RETURNS trigger AS $$ "
        "BEGIN "
        "  IF cloudsync_is_sync('%s') THEN RETURN NEW; END IF; "
        "  PERFORM cloudsync_update(table_name, new_value, old_value) "
        "  FROM (%s) AS v(table_name, new_value, old_value); "
        "  RETURN NEW; "
        "END; "
        "$$ LANGUAGE plpgsql;",
        func_name, table_name, values_query);
    cloudsync_memory_free(values_query);
    if (!sql2) return DBRES_NOMEM;

    rc = database_exec(data, sql2);
    cloudsync_memory_free(sql2);
    if (rc != DBRES_OK) return rc;

    sql2 = cloudsync_memory_mprintf(
        "CREATE TRIGGER %s AFTER UPDATE ON \"%s\" %s "
        "EXECUTE FUNCTION %s();",
        trigger_name, table_name, trigger_when ? trigger_when : "", func_name);
    if (!sql2) return DBRES_NOMEM;

    rc = database_exec(data, sql2);
    cloudsync_memory_free(sql2);
    return rc;
}

int database_create_delete_trigger_gos (cloudsync_context *data, const char *table_name) {
    if (!table_name) return DBRES_MISUSE;

    char trigger_name[1024];
    char func_name[1024];
    snprintf(trigger_name, sizeof(trigger_name), "cloudsync_before_delete_%s", table_name);
    snprintf(func_name, sizeof(func_name), "cloudsync_before_delete_%s_fn", table_name);

    if (database_trigger_exists(data, trigger_name)) return DBRES_OK;

    char *sql = cloudsync_memory_mprintf(
        "CREATE OR REPLACE FUNCTION %s() RETURNS trigger AS $$ "
        "BEGIN "
        "  RAISE EXCEPTION 'Error: DELETE operation is not allowed on table %s.'; "
        "END; "
        "$$ LANGUAGE plpgsql;",
        func_name, table_name);
    if (!sql) return DBRES_NOMEM;

    int rc = database_exec(data, sql);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) return rc;

    sql = cloudsync_memory_mprintf(
        "CREATE TRIGGER %s BEFORE DELETE ON \"%s\" "
        "FOR EACH ROW WHEN (cloudsync_is_enabled('%s') = true) "
        "EXECUTE FUNCTION %s();",
        trigger_name, table_name, table_name, func_name);
    if (!sql) return DBRES_NOMEM;

    rc = database_exec(data, sql);
    cloudsync_memory_free(sql);
    return rc;
}

int database_create_delete_trigger (cloudsync_context *data, const char *table_name, const char *trigger_when) {
    if (!table_name) return DBRES_MISUSE;

    char trigger_name[1024];
    char func_name[1024];
    snprintf(trigger_name, sizeof(trigger_name), "cloudsync_after_delete_%s", table_name);
    snprintf(func_name, sizeof(func_name), "cloudsync_after_delete_%s_fn", table_name);

    if (database_trigger_exists(data, trigger_name)) return DBRES_OK;

    char sql[2048];
    snprintf(sql, sizeof(sql),
             "SELECT string_agg('OLD.' || quote_ident(kcu.column_name), ',' ORDER BY kcu.ordinal_position) "
             "FROM information_schema.table_constraints tc "
             "JOIN information_schema.key_column_usage kcu "
             "  ON tc.constraint_name = kcu.constraint_name "
             "WHERE tc.table_name = '%s' AND tc.constraint_type = 'PRIMARY KEY';",
             table_name);

    char *pk_list = NULL;
    int rc = database_select_text(data, sql, &pk_list);
    if (rc != DBRES_OK) return rc;
    if (!pk_list || pk_list[0] == '\0') {
        if (pk_list) cloudsync_memory_free(pk_list);
        return cloudsync_set_error(data, "No primary key columns found for table", DBRES_ERROR);
    }

    char *sql2 = cloudsync_memory_mprintf(
        "CREATE OR REPLACE FUNCTION %s() RETURNS trigger AS $$ "
        "BEGIN "
        "  IF cloudsync_is_sync('%s') THEN RETURN OLD; END IF; "
        "  PERFORM cloudsync_delete('%s', VARIADIC ARRAY[%s]); "
        "  RETURN OLD; "
        "END; "
        "$$ LANGUAGE plpgsql;",
        func_name, table_name, table_name, pk_list);
    cloudsync_memory_free(pk_list);
    if (!sql2) return DBRES_NOMEM;

    rc = database_exec(data, sql2);
    cloudsync_memory_free(sql2);
    if (rc != DBRES_OK) return rc;

    sql2 = cloudsync_memory_mprintf(
        "CREATE TRIGGER %s AFTER DELETE ON \"%s\" %s "
        "EXECUTE FUNCTION %s();",
        trigger_name, table_name, trigger_when ? trigger_when : "", func_name);
    if (!sql2) return DBRES_NOMEM;

    rc = database_exec(data, sql2);
    cloudsync_memory_free(sql2);
    return rc;
}

int database_create_triggers (cloudsync_context *data, const char *table_name, table_algo algo) {
    if (!table_name) return DBRES_MISUSE;

    char trigger_when[1024];
    snprintf(trigger_when, sizeof(trigger_when),
             "FOR EACH ROW WHEN (cloudsync_is_sync('%s') = false)",
             table_name);

    int rc = database_create_insert_trigger(data, table_name, trigger_when);
    if (rc != DBRES_OK) return rc;

    if (algo == table_algo_crdt_gos) {
        rc = database_create_update_trigger_gos(data, table_name);
    } else {
        rc = database_create_update_trigger(data, table_name, trigger_when);
    }
    if (rc != DBRES_OK) return rc;

    if (algo == table_algo_crdt_gos) {
        rc = database_create_delete_trigger_gos(data, table_name);
    } else {
        rc = database_create_delete_trigger(data, table_name, trigger_when);
    }

    return rc;
}

int database_delete_triggers (cloudsync_context *data, const char *table) {
    char sql[1024];

    snprintf(sql, sizeof(sql),
             "DROP TRIGGER IF EXISTS \"cloudsync_after_insert_%s\" ON \"%s\";",
             table, table);
    database_exec(data, sql);

    snprintf(sql, sizeof(sql),
             "DROP FUNCTION IF EXISTS cloudsync_after_insert_%s_fn() CASCADE;",
             table);
    database_exec(data, sql);

    snprintf(sql, sizeof(sql),
             "DROP TRIGGER IF EXISTS \"cloudsync_after_update_%s\" ON \"%s\";",
             table, table);
    database_exec(data, sql);

    snprintf(sql, sizeof(sql),
             "DROP TRIGGER IF EXISTS \"cloudsync_before_update_%s\" ON \"%s\";",
             table, table);
    database_exec(data, sql);

    snprintf(sql, sizeof(sql),
             "DROP FUNCTION IF EXISTS cloudsync_after_update_%s_fn() CASCADE;",
             table);
    database_exec(data, sql);

    snprintf(sql, sizeof(sql),
             "DROP FUNCTION IF EXISTS cloudsync_before_update_%s_fn() CASCADE;",
             table);
    database_exec(data, sql);

    snprintf(sql, sizeof(sql),
             "DROP TRIGGER IF EXISTS \"cloudsync_after_delete_%s\" ON \"%s\";",
             table, table);
    database_exec(data, sql);

    snprintf(sql, sizeof(sql),
             "DROP TRIGGER IF EXISTS \"cloudsync_before_delete_%s\" ON \"%s\";",
             table, table);
    database_exec(data, sql);

    snprintf(sql, sizeof(sql),
             "DROP FUNCTION IF EXISTS cloudsync_after_delete_%s_fn() CASCADE;",
             table);
    database_exec(data, sql);

    snprintf(sql, sizeof(sql),
             "DROP FUNCTION IF EXISTS cloudsync_before_delete_%s_fn() CASCADE;",
             table);
    database_exec(data, sql);

    return DBRES_OK;
}

// MARK: - SCHEMA VERSIONING -

int64_t database_schema_version (cloudsync_context *data) {
    int64_t value = 0;
    int rc = database_select_int(data, SQL_SCHEMA_VERSION, &value);
    return (rc == DBRES_OK) ? value : 0;
}

uint64_t database_schema_hash (cloudsync_context *data) {
    char *schema = NULL;
    database_select_text(data,
        "SELECT string_agg(LOWER(table_name || column_name || data_type), '' ORDER BY table_name, column_name) "
        "FROM information_schema.columns WHERE table_schema = 'public'",
        &schema);

    if (!schema) {
        elog(INFO, "database_schema_hash: schema is NULL");
        return 0;
    }

    size_t schema_len = strlen(schema);
    uint64_t hash = fnv1a_hash(schema, schema_len);
    cloudsync_memory_free(schema);
    return hash;
}

bool database_check_schema_hash (cloudsync_context *data, uint64_t hash) {
    char sql[1024];
    snprintf(sql, sizeof(sql), "SELECT 1 FROM cloudsync_schema_versions WHERE hash = %" PRIu64, hash);

    int64_t value = 0;
    database_select_int(data, sql, &value);
    return (value == 1);
}

int database_update_schema_hash (cloudsync_context *data, uint64_t *hash) {
    char *schema = NULL;
    int rc = database_select_text(data,
        "SELECT string_agg(LOWER(table_name || column_name || data_type), '' ORDER BY table_name, column_name) "
        "FROM information_schema.columns WHERE table_schema = 'public'",
        &schema);

    if (rc != DBRES_OK || !schema) return cloudsync_set_error(data, "database_update_schema_hash error 1", DBRES_ERROR);

    size_t schema_len = strlen(schema);
    DEBUG_ALWAYS("database_update_schema_hash len %zu", schema_len);
    uint64_t h = fnv1a_hash(schema, schema_len);
    cloudsync_memory_free(schema);
    if (hash && *hash == h) return cloudsync_set_error(data, "database_update_schema_hash constraint", DBRES_CONSTRAINT);

    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO cloudsync_schema_versions (hash, seq) "
             "VALUES (%" PRId64 ", COALESCE((SELECT MAX(seq) FROM cloudsync_schema_versions), 0) + 1) "
             "ON CONFLICT(hash) DO UPDATE SET "
             "seq = (SELECT COALESCE(MAX(seq), 0) + 1 FROM cloudsync_schema_versions);",
             h);
    rc = database_exec(data, sql);
    if (rc == DBRES_OK && hash) {
        *hash = h;
        return rc;
    } 

    return cloudsync_set_error(data, "database_update_schema_hash error 2", DBRES_ERROR);
}

// MARK: - PRIMARY KEY -

int database_pk_rowid (cloudsync_context *data, const char *table_name, char ***names, int *count) {
    // PostgreSQL doesn't have rowid concept like SQLite
    // Use OID or primary key columns instead
    return database_pk_names(data, table_name, names, count);
}

int database_pk_names (cloudsync_context *data, const char *table_name, char ***names, int *count) {
    if (!table_name || !names || !count) return DBRES_MISUSE;

    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT kcu.column_name FROM information_schema.table_constraints tc "
             "JOIN information_schema.key_column_usage kcu ON tc.constraint_name = kcu.constraint_name "
             "WHERE tc.table_name = '%s' AND tc.constraint_type = 'PRIMARY KEY' "
             "ORDER BY kcu.ordinal_position",
             table_name);

    int rc = SPI_execute(sql, true, 0);
    if (rc < 0 || SPI_processed == 0) {
        *names = NULL;
        *count = 0;
        return DBRES_OK;
    }

    uint64_t n = SPI_processed;
    char **pk_names = cloudsync_memory_alloc(n * sizeof(char*));
    if (!pk_names) return DBRES_NOMEM;

    for (int i = 0; i < n; i++) {
        HeapTuple tuple = SPI_tuptable->vals[i];
        bool isnull;
        Datum datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull) {
            text *txt = DatumGetTextP(datum);
            char *name = text_to_cstring(txt);
            pk_names[i] = (name) ? cloudsync_string_dup(name) : NULL;
            if (name) pfree(name);
        } else {
            pk_names[i] = NULL;
        }
    }

    *names = pk_names;
    *count = (int)n;
    return DBRES_OK;
}

// MARK: - VM -

int databasevm_prepare (cloudsync_context *data, const char *sql, dbvm_t **vm, int flags) {
    if (!sql || !vm) {
        return cloudsync_set_error(data, "Invalid parameters to databasevm_prepare", DBRES_ERROR);
    }
    *vm = NULL;
    cloudsync_reset_error(data);
    
    // sanity check number of parameters
    // int counter = count_params(sql);
    // if (counter > MAX_PARAMS) return cloudsync_set_error(data, "Maximum number of parameters reached", DBRES_MISUSE);
    
    // create PostgreSQL VM statement
    pg_stmt_t *stmt = (pg_stmt_t *)cloudsync_memory_zeroalloc(sizeof(pg_stmt_t));
    if (!stmt) return cloudsync_set_error(data, "Not enough memory to allocate a dbvm_t struct", DBRES_NOMEM);
    stmt->data = data;
    
    int rc = DBRES_OK;
    PG_TRY();
    {
        MemoryContext parent = (flags & DBFLAG_PERSISTENT) ? TopMemoryContext : CurrentMemoryContext;
        stmt->stmt_mcxt = AllocSetContextCreate(parent, "cloudsync stmt", ALLOCSET_DEFAULT_SIZES);
        stmt->bind_mcxt = AllocSetContextCreate(stmt->stmt_mcxt, "cloudsync binds", ALLOCSET_DEFAULT_SIZES);
        stmt->row_mcxt = AllocSetContextCreate(stmt->stmt_mcxt, "cloudsync row", ALLOCSET_DEFAULT_SIZES);
        
        MemoryContext old = MemoryContextSwitchTo(stmt->stmt_mcxt);
        stmt->sql = pstrdup(sql);
        MemoryContextSwitchTo(old);
    }
    PG_CATCH();
    {
        if (stmt->stmt_mcxt) MemoryContextDelete(stmt->stmt_mcxt);
        cloudsync_memory_free(stmt);
        FlushErrorState();
        rc = DBRES_NOMEM;
        stmt = NULL;
    }
    PG_END_TRY();
    
    if (stmt) databasevm_clear_bindings((dbvm_t*)stmt);
    *vm = (dbvm_t*)stmt;
    
    return rc;
}

int databasevm_step0 (pg_stmt_t *stmt) {
    cloudsync_context *data = stmt->data;
    int rc = DBRES_OK;
    
    // prepare plan
    PG_TRY();
    {
        if (!stmt || !stmt->sql) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("databasevm_step0 invalid stmt or sql pointer")));
        }
        
        stmt->plan = SPI_prepare(stmt->sql, stmt->nparams, stmt->types);
        if (stmt->plan == NULL) {
            int err = cloudsync_set_error(data, "Unable to prepare SQL statement", DBRES_ERROR);
            return err;
        }
        SPI_keepplan(stmt->plan);
        stmt->plan_is_prepared = true;
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        int err = cloudsync_set_error(data, edata->message, DBRES_ERROR);
        FreeErrorData(edata);
        FlushErrorState();
        rc = err;
    }
    PG_END_TRY();
    
    return rc;
}

int databasevm_step (dbvm_t *vm) {
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    if (!stmt) return DBRES_MISUSE;
    
    cloudsync_context *data = stmt->data;
    cloudsync_reset_error(data);
    
    if (!stmt->plan_is_prepared) {
        int rc = databasevm_step0(stmt);
        if (rc != DBRES_OK) return rc;
    }
    if (!stmt->plan_is_prepared || !stmt->plan) return DBRES_ERROR;
    
    int rc = DBRES_DONE;
    PG_TRY();
    {
        do {
            // if portal is open, we fetch one row
            if (stmt->portal_open) {
                // free prior fetched row batch
                clear_fetch_batch(stmt);
                
                SPI_cursor_fetch(stmt->portal, true /* forward */, 1);
                
                if (SPI_processed == 0) {
                    // done
                    clear_fetch_batch(stmt);
                    close_portal(stmt);
                    rc = DBRES_DONE;
                    break;
                }
                
                MemoryContextReset(stmt->row_mcxt);
                
                stmt->last_tuptable = SPI_tuptable;
                stmt->current_tupdesc = stmt->last_tuptable->tupdesc;
                stmt->current_tuple = stmt->last_tuptable->vals[0];
                rc = DBRES_ROW;
                break;
            }
            
            // First step: decide whether to use portal.
            // Even for INSERT/UPDATE/DELETE ... RETURNING you WANT a portal.
            // Strategy:
            // - Only open a cursor if the plan supports it (avoid "cannot open INSERT query as cursor").
            // - Otherwise execute once as a non-row-returning statement.
            if (!stmt->executed_nonselect) {
                if (SPI_is_cursor_plan(stmt->plan)) {
                    // try cursor open
                    stmt->portal = NULL;
                    if (stmt->nparams == 0) stmt->portal = SPI_cursor_open(NULL, stmt->plan, NULL, NULL, false);
                    else stmt->portal = SPI_cursor_open(NULL, stmt->plan, stmt->values, stmt->nulls, false);

                    if (stmt->portal != NULL) {
                        stmt->portal_open = true;
                        
                        // fetch first row
                        clear_fetch_batch(stmt);
                        SPI_cursor_fetch(stmt->portal, true, 1);
                        
                        if (SPI_processed == 0) {
                            clear_fetch_batch(stmt);
                            close_portal(stmt);
                            rc = DBRES_DONE;
                            break;
                        }
                        
                        MemoryContextReset(stmt->row_mcxt);
                        
                        stmt->last_tuptable = SPI_tuptable;
                        stmt->current_tupdesc = stmt->last_tuptable->tupdesc;
                        stmt->current_tuple = stmt->last_tuptable->vals[0];
                        rc = DBRES_ROW;
                        break;
                    }
                }

                // Execute once (non-row-returning or cursor open failed).
                if (stmt->nparams == 0) SPI_execute_plan(stmt->plan, NULL, NULL, false, 0);
                else SPI_execute_plan(stmt->plan, stmt->values, stmt->nulls, false, 0);

                stmt->executed_nonselect = true;
                rc = DBRES_DONE;
                break;
            }
            
            rc = DBRES_DONE;
        } while (0);
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        int err = cloudsync_set_error(data, edata->message, DBRES_ERROR);
        FreeErrorData(edata);
        FlushErrorState();
        
        // free resources
        clear_fetch_batch(stmt);
        close_portal(stmt);
        
        rc = err;
    }
    PG_END_TRY();
    return rc;
}

void databasevm_finalize (dbvm_t *vm) {
    if (!vm) return;
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    
    PG_TRY();
    {
        clear_fetch_batch(stmt);
        close_portal(stmt);
        
        if (stmt->plan_is_prepared && stmt->plan) {
            SPI_freeplan(stmt->plan);
            stmt->plan = NULL;
            stmt->plan_is_prepared = false;
        }
    }
    PG_CATCH();
    {
        /* don't throw from finalize; just swallow */
        FlushErrorState();
    }
    PG_END_TRY();

    if (stmt->stmt_mcxt) MemoryContextDelete(stmt->stmt_mcxt);
    cloudsync_memory_free(stmt);
}

void databasevm_reset (dbvm_t *vm) {
    if (!vm) return;
    databasevm_clear_bindings(vm);
}

void databasevm_clear_bindings (dbvm_t *vm) {
    if (!vm) return;
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    
    clear_fetch_batch(stmt);
    close_portal(stmt);
    
    if (stmt->plan_is_prepared && stmt->plan) {
        SPI_freeplan(stmt->plan);
        stmt->plan = NULL;
        stmt->plan_is_prepared = false;
    }
    
    if (stmt->bind_mcxt) MemoryContextReset(stmt->bind_mcxt);
    stmt->nparams = 0;
    stmt->executed_nonselect = false;
    
    // initialize static array of params
    for (int i = 0; i < MAX_PARAMS; i++) {
        stmt->types[i] = UNKNOWNOID;
        stmt->values[i] = (Datum) 0;
        stmt->nulls[i] = 'n';   // default NULL
    }
}

const char *databasevm_sql (dbvm_t *vm) {
    if (!vm) return NULL;

    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    return stmt->sql;
}

// MARK: - BINDING -

int databasevm_bind_blob (dbvm_t *vm, int index, const void *value, uint64_t size) {
    if (!vm || index < 1) return DBRES_ERROR;
    if (!value) return databasevm_bind_null(vm, index);
    
    // validate size fits Size and won't overflow
    if (size > (uint64) (MaxAllocSize - VARHDRSZ)) return DBRES_NOMEM;
    
    int idx = index - 1;
    if (idx >= MAX_PARAMS) return DBRES_ERROR;
    
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    MemoryContext old = MemoryContextSwitchTo(stmt->bind_mcxt);
    
    // Convert binary data to PostgreSQL bytea
    bytea *ba = (bytea*)palloc(size + VARHDRSZ);
    SET_VARSIZE(ba, size + VARHDRSZ);
    memcpy(VARDATA(ba), value, size);
    
    stmt->values[idx] = PointerGetDatum(ba);
    stmt->types[idx] = BYTEAOID;
    stmt->nulls[idx] = ' ';
    
    MemoryContextSwitchTo(old);
    
    if (stmt->nparams < idx + 1) stmt->nparams = idx + 1;
    return DBRES_OK;
}

int databasevm_bind_double (dbvm_t *vm, int index, double value) {
    if (!vm || index < 1) return DBRES_ERROR;

    int idx = index - 1;
    if (idx >= MAX_PARAMS) return DBRES_ERROR;
    
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    stmt->values[idx] = Float8GetDatum(value);
    stmt->types[idx] = FLOAT8OID;
    stmt->nulls[idx] = ' ';
    
    if (stmt->nparams < idx + 1) stmt->nparams = idx + 1;
    return DBRES_OK;
}

int databasevm_bind_int (dbvm_t *vm, int index, int64_t value) {
    if (!vm || index < 1) return DBRES_ERROR;

    int idx = index - 1;
    if (idx >= MAX_PARAMS) return DBRES_ERROR;
    
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    stmt->values[idx] = Int64GetDatum(value);
    stmt->types[idx] = INT8OID;
    stmt->nulls[idx] = ' ';
    
    if (stmt->nparams < idx + 1) stmt->nparams = idx + 1;
    return DBRES_OK;
}

int databasevm_bind_null (dbvm_t *vm, int index) {
    if (!vm || index < 1) return DBRES_ERROR;

    int idx = index - 1;
    if (idx >= MAX_PARAMS) return DBRES_ERROR;
    
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    stmt->values[idx] = (Datum)0;
    stmt->types[idx] = UNKNOWNOID;
    stmt->nulls[idx] = 'n';
    
    if (stmt->nparams < idx + 1) stmt->nparams = idx + 1;
    return DBRES_OK;
}

int databasevm_bind_text (dbvm_t *vm, int index, const char *value, int size) {
    if (!vm || index < 1) return DBRES_ERROR;
    if (!value) return databasevm_bind_null(vm, index);
    
    // validate size fits Size and won't overflow
    if (size < 0) size = (int)strlen(value);
    if (size > (uint64) (MaxAllocSize - VARHDRSZ)) return DBRES_NOMEM;
    
    int idx = index - 1;
    if (idx >= MAX_PARAMS) return DBRES_ERROR;
    
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    MemoryContext old = MemoryContextSwitchTo(stmt->bind_mcxt);
    
    text *t = cstring_to_text_with_len(value, size);
    stmt->values[idx] = PointerGetDatum(t);
    stmt->types[idx] = TEXTOID;
    stmt->nulls[idx] = ' ';
    
    MemoryContextSwitchTo(old);
    
    if (stmt->nparams < idx + 1) stmt->nparams = idx + 1;
    return DBRES_OK;
}

int databasevm_bind_value (dbvm_t *vm, int index, dbvalue_t *value) {
    if (!vm) return DBRES_ERROR;
    if (!value) return databasevm_bind_null(vm, index);

    int idx = index - 1;
    if (idx >= MAX_PARAMS) return DBRES_ERROR;
    
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    pgvalue_t *v = (pgvalue_t *)value;
    if (!v || v->isnull) {
        stmt->values[idx] = (Datum)0;
        stmt->types[idx] = TEXTOID;
        stmt->nulls[idx] = 'n';
    } else {
        int16 typlen;
        bool typbyval;
        
        get_typlenbyval(v->typeid, &typlen, &typbyval);
        MemoryContext old = MemoryContextSwitchTo(stmt->bind_mcxt);
        Datum dcopy = typbyval ? v->datum : datumCopy(v->datum, typbyval, typlen);
        stmt->values[idx] = dcopy;
        MemoryContextSwitchTo(old);
        stmt->types[idx] = OidIsValid(v->typeid) ? v->typeid : TEXTOID;
        stmt->nulls[idx] = ' ';
    }
    
    if (stmt->nparams < idx + 1) stmt->nparams = idx + 1;
    return DBRES_OK;
}

// MARK: - COLUMN -

Datum database_column_datum (dbvm_t *vm, int index) {
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    if (!vm || index >= MAX_PARAMS) return (Datum)0;
    if (!stmt->last_tuptable || !stmt->current_tupdesc) return (Datum)0;
    if (index < 0 || index >= stmt->current_tupdesc->natts) return (Datum)0;
    
    bool isnull = true;
    Datum d = get_datum(stmt, index, &isnull, NULL);
    return (isnull) ? (Datum)0 : d;
}

const void *database_column_blob (dbvm_t *vm, int index) {
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    if (!vm || index >= MAX_PARAMS) return NULL;
    if (!stmt->last_tuptable || !stmt->current_tupdesc) return NULL;
    if (index < 0 || index >= stmt->current_tupdesc->natts) return NULL;

    bool isnull = true;
    Datum d = get_datum(stmt, index, &isnull, NULL);
    if (isnull) return NULL;
    
    MemoryContext old = MemoryContextSwitchTo(stmt->row_mcxt);
    bytea *ba = DatumGetByteaP(d);
    int len = VARSIZE(ba) - VARHDRSZ;
    void *out = palloc(len);
    memcpy(out, VARDATA(ba), len);
    MemoryContextSwitchTo(old);
    
    return out;
}

double database_column_double (dbvm_t *vm, int index) {
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    if (!vm || index >= MAX_PARAMS) return 0.0;
    if (!stmt->last_tuptable || !stmt->current_tupdesc) return 0.0;
    if (index < 0 || index >= stmt->current_tupdesc->natts) return 0.0;

    bool isnull = true;
    Oid type = 0;
    Datum d = get_datum(stmt, index, &isnull, &type);
    if (isnull) return 0.0;
    
    switch (type) {
        case FLOAT4OID: return (double)DatumGetFloat4(d);
        case FLOAT8OID: return (double)DatumGetFloat8(d);
        case INT2OID: return (double)DatumGetInt16(d);
        case INT4OID: return (double)DatumGetInt32(d);
        case INT8OID: return (double)DatumGetInt64(d);
        case BOOLOID: return (double)DatumGetBool(d);
    }
    
    return 0.0;
}

int64_t database_column_int (dbvm_t *vm, int index) {
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    if (!vm || index >= MAX_PARAMS) return 0;
    if (!stmt->last_tuptable || !stmt->current_tupdesc) return 0;
    if (index < 0 || index >= stmt->current_tupdesc->natts) return 0;

    bool isnull = true;
    Oid type = 0;
    Datum d = get_datum(stmt, index, &isnull, &type);
    if (isnull) return 0;
    
    switch (type) {
        case FLOAT4OID: return (int64_t)DatumGetFloat4(d);
        case FLOAT8OID: return (int64_t)DatumGetFloat8(d);
        case INT2OID: return (int64_t)DatumGetInt16(d);
        case INT4OID: return (int64_t)DatumGetInt32(d);
        case INT8OID: return (int64_t)DatumGetInt64(d);
        case BOOLOID: return (int64_t)DatumGetBool(d);
    }
    
    return 0;
}

const char *database_column_text (dbvm_t *vm, int index) {
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    if (!vm || index >= MAX_PARAMS) return NULL;
    if (!stmt->last_tuptable || !stmt->current_tupdesc) return NULL;
    if (index < 0 || index >= stmt->current_tupdesc->natts) return NULL;

    bool isnull = true;
    Oid type = 0;
    Datum d = get_datum(stmt, index, &isnull, &type);
    if (isnull) return NULL;
    
    if (type != TEXTOID && type != VARCHAROID && type != BPCHAROID)
        return NULL; // or convert via output function if you want

    MemoryContext old = MemoryContextSwitchTo(stmt->row_mcxt);
    text *t = DatumGetTextP(d);
    int len = VARSIZE(t) - VARHDRSZ;
    char *out = palloc(len + 1);
    memcpy(out, VARDATA(t), len);
    out[len] = 0;
    MemoryContextSwitchTo(old);
    
    return out;
}

dbvalue_t *database_column_value (dbvm_t *vm, int index) {
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    if (!vm || index >= MAX_PARAMS) return NULL;
    if (!stmt->last_tuptable || !stmt->current_tupdesc) return NULL;
    if (index < 0 || index >= stmt->current_tupdesc->natts) return NULL;
    
    bool isnull = true;
    Oid type = 0;
    Datum d = get_datum(stmt, index, &isnull, &type);
    int32 typmod = TupleDescAttr(stmt->current_tupdesc, index)->atttypmod;
    Oid collation = TupleDescAttr(stmt->current_tupdesc, index)->attcollation;
    
    pgvalue_t *v = pgvalue_create(d, type, typmod, collation, isnull);
    if (v) pgvalue_ensure_detoast(v);
    return (dbvalue_t*)v;
}

int database_column_bytes (dbvm_t *vm, int index) {
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    if (!vm || index >= MAX_PARAMS) return 0;
    if (!stmt->last_tuptable || !stmt->current_tupdesc) return 0;
    if (index < 0 || index >= stmt->current_tupdesc->natts) return 0;

    bool isnull = true;
    Oid type = 0;
    Datum d = get_datum(stmt, index, &isnull, &type);
    if (isnull) return 0;
    
    MemoryContext old = MemoryContextSwitchTo(stmt->row_mcxt);
    
    int bytes = 0;
    if (type == BYTEAOID) {
        // BLOB case
        bytea *ba = DatumGetByteaP(d);
        bytes = (int)(VARSIZE(ba) - VARHDRSZ);
    } else if (type != TEXTOID && type != VARCHAROID && type != BPCHAROID) {
        // any non-TEXT case should be discarded
        bytes = 0;
    } else {
        // for text, return string length
        text *txt = DatumGetTextP(d);
        bytes = (int)(VARSIZE(txt) - VARHDRSZ);
    }
    MemoryContextSwitchTo(old);
    
    return bytes;
}

int database_column_type (dbvm_t *vm, int index) {
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    if (!vm || index >= MAX_PARAMS) return DBTYPE_NULL;
    if (!stmt->last_tuptable || !stmt->current_tupdesc) return DBTYPE_NULL;
    if (index < 0 || index >= stmt->current_tupdesc->natts) return DBTYPE_NULL;
    
    bool isnull = true;
    Oid type = 0;
    get_datum(stmt, index, &isnull, &type);
    if (isnull) return DBTYPE_NULL;
    
    switch (type) {
        case INT2OID:
        case INT4OID:
        case INT8OID:
            return DBTYPE_INTEGER;

        case FLOAT4OID:
        case FLOAT8OID:
        case NUMERICOID:
            return DBTYPE_FLOAT;

        case TEXTOID:
        case VARCHAROID:
        case BPCHAROID:
            return DBTYPE_TEXT;

        case BYTEAOID:
            return DBTYPE_BLOB;
    }
    
    return DBTYPE_TEXT;
}

// MARK: - VALUE -

const void *database_value_blob (dbvalue_t *value) {
    pgvalue_t *v = (pgvalue_t *)value;
    if (!v || v->isnull) return NULL;

    // Text types reuse blob accessor (pk encode reads text bytes directly)
    if (pgvalue_is_text_type(v->typeid)) {
        pgvalue_ensure_detoast(v);
        text *txt = (text *)DatumGetPointer(v->datum);
        return VARDATA_ANY(txt);
    }

    if (v->typeid == BYTEAOID) {
        pgvalue_ensure_detoast(v);
        bytea *ba = (bytea *)DatumGetPointer(v->datum);
        return VARDATA_ANY(ba);
    }

    return NULL;
}

double database_value_double (dbvalue_t *value) {
    pgvalue_t *v = (pgvalue_t *)value;
    if (!v || v->isnull) return 0.0;

    switch (v->typeid) {
        case FLOAT4OID:
            return (double)DatumGetFloat4(v->datum);
        case FLOAT8OID:
            return DatumGetFloat8(v->datum);
        case NUMERICOID:
            return DatumGetFloat8(DirectFunctionCall1(numeric_float8_no_overflow, v->datum));
        case INT2OID:
            return (double)DatumGetInt16(v->datum);
        case INT4OID:
            return (double)DatumGetInt32(v->datum);
        case INT8OID:
            return (double)DatumGetInt64(v->datum);
        case BOOLOID:
            return DatumGetBool(v->datum) ? 1.0 : 0.0;
        default:
            return 0.0;
    }
}

int64_t database_value_int (dbvalue_t *value) {
    pgvalue_t *v = (pgvalue_t *)value;
    if (!v || v->isnull) return 0;

    switch (v->typeid) {
        case INT2OID:
            return (int64_t)DatumGetInt16(v->datum);
        case INT4OID:
            return (int64_t)DatumGetInt32(v->datum);
        case INT8OID:
            return DatumGetInt64(v->datum);
        case BOOLOID:
            return DatumGetBool(v->datum) ? 1 : 0;
        default:
            return 0;
    }
}

const char *database_value_text (dbvalue_t *value) {
    pgvalue_t *v = (pgvalue_t *)value;
    if (!v || v->isnull) return "";

    if (!v->cstring) {
        if (pgvalue_is_text_type(v->typeid)) {
            pgvalue_ensure_detoast(v);
            v->cstring = text_to_cstring((text *)DatumGetPointer(v->datum));
        } else {
            // Fallback to type output function for non-text types
            Oid outfunc;
            bool isvarlena;
            getTypeOutputInfo(v->typeid, &outfunc, &isvarlena);
            v->cstring = OidOutputFunctionCall(outfunc, v->datum);
        }
        v->owns_cstring = true;
    }

    return v->cstring;
}

int database_value_bytes (dbvalue_t *value) {
    pgvalue_t *v = (pgvalue_t *)value;
    if (!v || v->isnull) return 0;

    if (pgvalue_is_text_type(v->typeid)) {
        pgvalue_ensure_detoast(v);
        text *txt = (text *)DatumGetPointer(v->datum);
        return VARSIZE_ANY_EXHDR(txt);
    }
    if (v->typeid == BYTEAOID) {
        pgvalue_ensure_detoast(v);
        bytea *ba = (bytea *)DatumGetPointer(v->datum);
        return VARSIZE_ANY_EXHDR(ba);
    }
    if (v->cstring) {
        return (int)strlen(v->cstring);
    }
    return 0;
}

int database_value_type (dbvalue_t *value) {
    return pgvalue_dbtype((pgvalue_t *)value);
}

void database_value_free (dbvalue_t *value) {
    pgvalue_t *v = (pgvalue_t *)value;
    pgvalue_free(v);
}

void *database_value_dup (dbvalue_t *value) {
    pgvalue_t *v = (pgvalue_t *)value;
    if (!v) return NULL;

    pgvalue_t *copy = pgvalue_create(v->datum, v->typeid, v->typmod, v->collation, v->isnull);
    if (v->detoasted && v->owned_detoast) {
        Size len = VARSIZE_ANY(v->owned_detoast);
        copy->owned_detoast = palloc(len);
        memcpy(copy->owned_detoast, v->owned_detoast, len);
        copy->datum = PointerGetDatum(copy->owned_detoast);
        copy->detoasted = true;
    }
    if (v->cstring) {
        copy->cstring = copy->cstring ? pstrdup(v->cstring) : NULL;
        copy->owns_cstring = true;
    }
    return (void*)copy;
}

// MARK: - SAVEPOINTS -

int database_begin_savepoint (cloudsync_context *data, const char *savepoint_name) {
    cloudsync_reset_error(data);
    int rc = DBRES_OK;

    PG_TRY();
    {
        BeginInternalSubTransaction(NULL);
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        rc = cloudsync_set_error(data, edata->message, DBRES_ERROR);
        FreeErrorData(edata);
        FlushErrorState();
    }
    PG_END_TRY();

    return rc;
}

int database_commit_savepoint (cloudsync_context *data, const char *savepoint_name) {
    cloudsync_reset_error(data);
    int rc = DBRES_OK;

    PG_TRY();
    {
        ReleaseCurrentSubTransaction();
        CommandCounterIncrement();

        // Refresh snapshot
        if (ActiveSnapshotSet()) {
            PopActiveSnapshot();
        }
        PushActiveSnapshot(GetTransactionSnapshot());
    }
    PG_CATCH();
    {
        FlushErrorState();
        rc = DBRES_ERROR;
    }
    PG_END_TRY();

    return rc;
}

int database_rollback_savepoint (cloudsync_context *data, const char *savepoint_name) {
    cloudsync_reset_error(data);
    int rc = DBRES_OK;

    PG_TRY();
    {
        RollbackAndReleaseCurrentSubTransaction();

        // Refresh snapshot
        if (ActiveSnapshotSet()) {
            PopActiveSnapshot();
        }
        PushActiveSnapshot(GetTransactionSnapshot());
    }
    PG_CATCH();
    {
        FlushErrorState();
        rc = DBRES_ERROR;
    }
    PG_END_TRY();

    return rc;
}

// MARK: - MEMORY -

void *dbmem_alloc (uint64_t size) {
    return malloc(size);
}

void *dbmem_zeroalloc (uint64_t size) {
    void *ptr = malloc(size);
    if (ptr) {
        memset(ptr, 0, (size_t)size);
    }
    return ptr;
}

void *dbmem_realloc (void *ptr, uint64_t new_size) {
    return realloc(ptr, new_size);
}

char *dbmem_mprintf (const char *format, ...) {
    if (!format) return NULL;

    va_list args;
    va_start(args, format);

    // Calculate required buffer size
    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);

    if (len < 0) {
        va_end(args);
        return NULL;
    }

    // Allocate buffer and format string
    char *result = (char*)malloc(len + 1);
    vsnprintf(result, len + 1, format, args);

    va_end(args);
    return result;
}

char *dbmem_vmprintf (const char *format, va_list list) {
    if (!format) return NULL;

    // Calculate required buffer size
    va_list args_copy;
    va_copy(args_copy, list);
    int len = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);

    if (len < 0) return NULL;

    // Allocate buffer and format string
    char *result = (char*)malloc(len + 1);
    vsnprintf(result, len + 1, format, list);

    return result;
}

void dbmem_free (void *ptr) {
    if (ptr) {
        free(ptr);
    }
}

uint64_t dbmem_size (void *ptr) {
    // PostgreSQL memory alloc doesn't expose allocated size directly
    // Return 0 as a safe default
    return 0;
}

// MARK: - CLOUDSYNC CALLBACK -

static cloudsync_payload_apply_callback_t payload_apply_callback = NULL;

void cloudsync_set_payload_apply_callback(void *db, cloudsync_payload_apply_callback_t callback) {
    payload_apply_callback = callback;
}

cloudsync_payload_apply_callback_t cloudsync_get_payload_apply_callback(void *db) {
    return payload_apply_callback;
}
