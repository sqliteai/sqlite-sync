//
//  database_postgresql.c
//  cloudsync
//
//  Created by Marco Bambini on 03/12/25.
//

// Define POSIX feature test macros before any includes
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

// PostgreSQL requires postgres.h to be included FIRST
// It sets up the entire environment including platform compatibility
#include "postgres.h"

#include <inttypes.h>
#include <string.h>
#include <stdlib.h>

#include "../cloudsync.h"
#include "../database.h"
#include "../dbutils.h"
#include "../utils.h"
#include "../sql.h"

// PostgreSQL SPI and other headers
#include "executor/spi.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"
#include "utils/memutils.h"
#include "access/xact.h"
#include "utils/snapmgr.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

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
    char *sql;              // Original SQL (converted to $1 style)
    SPIPlanPtr plan;        // NULL until first step (deferred prepare)
    Portal portal;
    int current_row;
    bool prepared;          // True after actual SPI_prepare is called
    bool executed;          // True after first execution
    Datum params[MAX_PARAMS];
    Oid param_types[MAX_PARAMS];
    char nulls[MAX_PARAMS];
    int param_count;
} pg_stmt_wrapper_t;

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

char *sql_build_select_nonpk_by_pk (db_t *db, const char *table_name) {
    char *sql = cloudsync_memory_mprintf(SQL_BUILD_SELECT_NONPK_COLS_BY_PK_PG, table_name);
    if (!sql) return NULL;
    
    char *query = NULL;
    int rc = database_select_text(db, sql, &query);
    cloudsync_memory_free(sql);
    
    return (rc == DBRES_OK) ? query : NULL;
}

// MARK: - HELPER FUNCTIONS -

// Convert SQLite-style ? placeholders to PostgreSQL-style $1, $2, etc.
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
    char *newsql = palloc(newlen);

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

// Map SPI result codes to DBRES
static int map_spi_result(int rc) {
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

// MARK: - PRIVATE -

// Forward declaration
static int set_last_error(int errcode, const char *errmsg);

int database_select1_value (db_t *db, const char *sql, char **ptr_value, int64_t *int_value, DBTYPE expected_type) {
    elog(DEBUG1, "database_select1_value: %s", sql);

    // init values and sanity check expected_type
    if (ptr_value) *ptr_value = NULL;
    *int_value = 0;
    if (expected_type != DBTYPE_INTEGER && expected_type != DBTYPE_TEXT && expected_type != DBTYPE_BLOB) {
        return set_last_error(DBRES_MISUSE, "Invalid expected_type");
    }

    int rc = SPI_execute(sql, true, 0);
    if (rc < 0) {
        return set_last_error(DBRES_ERROR, "SPI_execute failed in database_select1_value");
    }

    // ensure at least one column
    if (!SPI_tuptable || !SPI_tuptable->tupdesc) {
        return set_last_error(DBRES_ERROR, "No result table");
    }
    if (SPI_tuptable->tupdesc->natts < 1) {
        return set_last_error(DBRES_ERROR, "No columns in result");
    }

    // no rows OK
    if (SPI_processed == 0) {
        return set_last_error(DBRES_OK, NULL);
    }

    HeapTuple tuple = SPI_tuptable->vals[0];
    bool isnull;
    Datum datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, &isnull);

    // NULL value is OK
    if (isnull) {
        return set_last_error(DBRES_OK, NULL);
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
                return set_last_error(DBRES_ERROR, "Type mismatch: expected integer");
        }
    } else if (expected_type == DBTYPE_TEXT) {
        text *txt = DatumGetTextP(datum);
        int len = VARSIZE(txt) - VARHDRSZ;
        if (len > 0) {
            char *ptr = cloudsync_memory_alloc(len + 1);
            if (!ptr) {
                return set_last_error(DBRES_NOMEM, "Memory allocation failed");
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
                return set_last_error(DBRES_NOMEM, "Memory allocation failed");
            }
            memcpy(ptr, VARDATA(ba), len);
            *ptr_value = ptr;
            *int_value = len;
        }
    }

    return set_last_error(DBRES_OK, NULL);
}

int database_select3_values (db_t *db, const char *sql, char **value, int64_t *len, int64_t *value2, int64_t *value3) {
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

bool database_system_exists (db_t *db, const char *name, const char *type) {
    if (!name || !type) return false;
 
    char query[512];
    bool exists = false;

    if (strcmp(type, "table") == 0) {
        snprintf(query, sizeof(query),
                 "SELECT 1 FROM pg_tables WHERE schemaname = 'public' AND tablename = '%s'",
                 name);
    } else if (strcmp(type, "trigger") == 0) {
        snprintf(query, sizeof(query),
                 "SELECT 1 FROM pg_trigger WHERE tgname = '%s'",
                 name);
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
        FlushErrorState();
        exists = false;
    }
    PG_END_TRY();

    elog(DEBUG1, "database_system_exists %s: %d", name, exists);
    return exists;
}

// MARK: - GENERAL -

int database_exec (db_t *db, const char *sql) {
    elog(DEBUG1, "database_exec %s", sql);
    if (!sql) return set_last_error(DBRES_ERROR, "SQL statement is NULL");

    int rc;
    PG_TRY();
    {
        rc = SPI_execute(sql, false, 0);
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        int err = set_last_error(DBRES_ERROR, edata->message);
        FlushErrorState();
        FreeErrorData(edata);
        return err;
    }
    PG_END_TRY();

    // Increment command counter to make changes visible
    if (rc >= 0) {
        CommandCounterIncrement();

        // Refresh snapshot to ensure subsequent reads see the changes
        if (ActiveSnapshotSet()) {
            PopActiveSnapshot();
        }
        PushActiveSnapshot(GetTransactionSnapshot());

        // Clear error on success
        elog(DEBUG1, "database_exec %s: OK", sql);
        set_last_error(DBRES_OK, NULL);
        return map_spi_result(rc);
    }

    elog(DEBUG1, "database_exec %s: ERROR", sql);
    return set_last_error(DBRES_ERROR, "SPI_execute failed");
}

int database_exec_callback (db_t *db, const char *sql, int (*callback)(void *xdata, int argc, char **values, char **names), void *xdata) {
    elog(DEBUG1, "database_exec_callback %s", sql);
    if (!sql) return set_last_error(DBRES_ERROR, "SQL statement is NULL");;

    int rc;
    PG_TRY();
    { 
        rc = SPI_execute(sql, true, 0);
      }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        int err = set_last_error(DBRES_ERROR, edata->message);
        FlushErrorState();
        FreeErrorData(edata);
        return err;
    }
    PG_END_TRY();

    if (rc < 0) return set_last_error(DBRES_ERROR, "SPI_execute failed");;

    // Call callback for each row if provided
    if (callback && SPI_tuptable) {
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        int ncols = tupdesc->natts;

        // Allocate arrays for column names and values
        char **names = palloc(ncols * sizeof(char*));
        char **values = palloc(ncols * sizeof(char*));

        // Get column names
        for (int i = 0; i < ncols; i++) {
            names[i] = NameStr(tupdesc->attrs[i].attname);
        }

        // Process each row
        for (uint64 row = 0; row < SPI_processed; row++) {
            HeapTuple tuple = SPI_tuptable->vals[row];

            // Get values for this row
            for (int i = 0; i < ncols; i++) {
                bool isnull;
                Datum datum = SPI_getbinval(tuple, tupdesc, i + 1, &isnull);
                if (isnull) {
                    values[i] = NULL;
                } else {
                    // Convert to text
                    Oid typeid = SPI_gettypeid(tupdesc, i + 1);
                    if (typeid == TEXTOID || typeid == VARCHAROID) {
                        text *txt = DatumGetTextP(datum);
                        values[i] = text_to_cstring(txt);
                    } else {
                        // For non-text types, convert to string representation
                        values[i] = DatumGetCString(DirectFunctionCall1(textout, datum));
                    }
                }
            }

            // Call user callback
            int cb_rc = callback(xdata, ncols, values, names);

            if (cb_rc != 0) {
                pfree(names);
                pfree(values);
                char errmsg[1024];
                snprintf(errmsg, sizeof(errmsg), "database_exec_callback aborted %d", cb_rc);
                return set_last_error(DBRES_ABORT, errmsg);
            }
        }

        pfree(names);
        pfree(values);
    }

    return DBRES_OK;
}

int database_write (db_t *db, const char *sql, const char **bind_values, DBTYPE bind_types[], int bind_lens[], int bind_count) {
    if (!sql) return set_last_error(DBRES_ERROR, "Invalid parameters to database_write");

    // Prepare statement
    dbvm_t *stmt;
    int rc = database_prepare(db, sql, &stmt, 0);
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

int database_select_int (db_t *db, const char *sql, int64_t *value) {
    return database_select1_value(db, sql, NULL, value, DBTYPE_INTEGER);
}

int database_select_text (db_t *db, const char *sql, char **value) {
    int64_t len = 0;
    return database_select1_value(db, sql, value, &len, DBTYPE_TEXT);
}

int database_select_blob (db_t *db, const char *sql, char **value, int64_t *len) {
    return database_select1_value(db, sql, value, len, DBTYPE_BLOB);
}

int database_select_blob_2int (db_t *db, const char *sql, char **value, int64_t *len, int64_t *value2, int64_t *value3) {
    return database_select3_values(db, sql, value, len, value2, value3);
}

// MARK: - STATUS -

static int last_error_code = DBRES_OK;
static char *last_error_msg = NULL;

// Helper function to record errors and return the error code
// This allows callers to write: return set_last_error(code, msg);
static int set_last_error(int errcode, const char *errmsg) {
    // elog(DEBUG1, "set_last_error: %d %s", errcode, errmsg ? errmsg : "(null)");

    last_error_code = errcode;

    if (last_error_msg) {
        pfree(last_error_msg);
        last_error_msg = NULL;
    }

    if (errmsg) {
        last_error_msg = pstrdup(errmsg);
    }

    return errcode;
}

int database_errcode (db_t *db) {
    return last_error_code;
}

const char *database_errmsg (db_t *db) {
    return last_error_msg ? last_error_msg : "not an error";
}

bool database_in_transaction (db_t *db) {
    // In SPI context, we're always in a transaction
    return IsTransactionState();
}

bool database_table_exists (db_t *db, const char *name) {
    return database_system_exists(db, name, "table");
}

bool database_trigger_exists (db_t *db, const char *name) {
    return database_system_exists(db, name, "trigger");
}

// MARK: - SCHEMA INFO -

int database_count_pk (db_t *db, const char *table_name, bool not_null) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM information_schema.table_constraints tc "
             "JOIN information_schema.key_column_usage kcu ON tc.constraint_name = kcu.constraint_name "
             "WHERE tc.table_name = '%s' AND tc.constraint_type = 'PRIMARY KEY'",
             table_name);

    int64_t count = 0;
    database_select_int(db, sql, &count);
    return (int)count;
}

int database_count_nonpk (db_t *db, const char *table_name) {
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
    database_select_int(db, sql, &count);
    return (int)count;
}

int database_count_int_pk (db_t *db, const char *table_name) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM information_schema.columns c "
             "JOIN information_schema.key_column_usage kcu ON c.column_name = kcu.column_name "
             "JOIN information_schema.table_constraints tc ON kcu.constraint_name = tc.constraint_name "
             "WHERE c.table_name = '%s' AND tc.constraint_type = 'PRIMARY KEY' "
             "AND c.data_type IN ('smallint', 'integer', 'bigint')",
             table_name);

    int64_t count = 0;
    database_select_int(db, sql, &count);
    return (int)count;
}

int database_count_notnull_without_default (db_t *db, const char *table_name) {
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
    database_select_int(db, sql, &count);
    return (int)count;
}

int database_debug (db_t *db, bool print_result) {
    // PostgreSQL debug information
    if (print_result) {
        elog(DEBUG1, "PostgreSQL SPI debug info:");
        elog(DEBUG1, "  SPI_processed: %lu", SPI_processed);
        elog(DEBUG1, "  In transaction: %d", IsTransactionState());
    }
    return DBRES_OK;
}

// MARK: - METADATA TABLES -

int database_create_metatable (db_t *db, const char *table_name) {
    char sql[2048];
    int rc;

    // Create the metadata table
    snprintf(sql, sizeof(sql),
             "CREATE TABLE IF NOT EXISTS \"%s_cloudsync\" ("
             "pk TEXT PRIMARY KEY,"
             "db_version BIGINT NOT NULL DEFAULT 0,"
             "seq INTEGER NOT NULL DEFAULT 0,"
             "site_id TEXT NOT NULL DEFAULT '',"
             "col_version BIGINT,"
             "col_name TEXT,"
             "col_site_id TEXT"
             ");",
             table_name);

    rc = database_exec(db, sql);
    if (rc != DBRES_OK) return rc;

    // Create indices for performance
    snprintf(sql, sizeof(sql),
             "CREATE INDEX IF NOT EXISTS \"%s_cloudsync_db_version_idx\" "
             "ON \"%s_cloudsync\" (db_version);",
             table_name, table_name);

    rc = database_exec(db, sql);
    return rc;
}

// MARK: - TRIGGERS -

// TODO
int database_create_insert_trigger (db_t *db, const char *table_name, char *trigger_when) {
    // PostgreSQL triggers are more complex - placeholder implementation
    // Full implementation would create trigger functions and triggers
    elog(WARNING, "database_create_insert_trigger not yet implemented for PostgreSQL");
    return DBRES_OK;
}

// TODO
int database_create_update_trigger_gos (db_t *db, const char *table_name) {
    elog(WARNING, "database_create_update_trigger_gos not yet implemented for PostgreSQL");
    return DBRES_OK;
}

// TODO
int database_create_update_trigger (db_t *db, const char *table_name, const char *trigger_when) {
    elog(WARNING, "database_create_update_trigger not yet implemented for PostgreSQL");
    return DBRES_OK;
}

// TODO
int database_create_delete_trigger_gos (db_t *db, const char *table_name) {
    elog(WARNING, "database_create_delete_trigger_gos not yet implemented for PostgreSQL");
    return DBRES_OK;
}

// TODO
int database_create_delete_trigger (db_t *db, const char *table_name, const char *trigger_when) {
    elog(WARNING, "database_create_delete_trigger not yet implemented for PostgreSQL");
    return DBRES_OK;
}

// TODO
int database_create_triggers (db_t *db, const char *table_name, table_algo algo) {
    // Placeholder - triggers need to be implemented with PostgreSQL PL/pgSQL
    elog(WARNING, "database_create_triggers not yet implemented for PostgreSQL");
    return DBRES_OK;
}

int database_delete_triggers (db_t *db, const char *table) {
    char sql[1024];

    snprintf(sql, sizeof(sql),
             "DROP TRIGGER IF EXISTS \"%s_insert_trigger\" ON \"%s\";",
             table, table);
    database_exec(db, sql);

    snprintf(sql, sizeof(sql),
             "DROP TRIGGER IF EXISTS \"%s_update_trigger\" ON \"%s\";",
             table, table);
    database_exec(db, sql);

    snprintf(sql, sizeof(sql),
             "DROP TRIGGER IF EXISTS \"%s_delete_trigger\" ON \"%s\";",
             table, table);
    database_exec(db, sql);

    return DBRES_OK;
}

// MARK: - SCHEMA VERSIONING -

int64_t database_schema_version (db_t *db) {
    int64_t value = 0;
    int rc = database_select_int(db, SQL_SCHEMA_VERSION, &value);
    return (rc == DBRES_OK) ? value : 0;
}

uint64_t database_schema_hash (db_t *db) {
    char *schema = NULL;
    database_select_text(db,
        "SELECT string_agg(LOWER(table_name || column_name || data_type), '' ORDER BY table_name, column_name) "
        "FROM information_schema.columns WHERE table_schema = 'public'",
        &schema);

    if (!schema) return 0;

    uint64_t hash = fnv1a_hash(schema, strlen(schema));
    cloudsync_memory_free(schema);
    return hash;
}

bool database_check_schema_hash (db_t *db, uint64_t hash) {
    char sql[1024];
    snprintf(sql, sizeof(sql), "SELECT 1 FROM cloudsync_schema_versions WHERE hash = %" PRId64, hash);

    int64_t value = 0;
    database_select_int(db, sql, &value);
    return (value == 1);
}

int database_update_schema_hash (db_t *db, uint64_t *hash) {
    char *schema = NULL;
    int rc = database_select_text(db,
        "SELECT string_agg(LOWER(table_name || column_name || data_type), '' ORDER BY table_name, column_name) "
        "FROM information_schema.columns WHERE table_schema = 'public'",
        &schema);

    if (rc != DBRES_OK || !schema) return set_last_error(DBRES_ERROR, "database_update_schema_hash error 1");

    uint64_t h = fnv1a_hash(schema, strlen(schema));
    cloudsync_memory_free(schema);
    if (hash && *hash == h) return set_last_error(DBRES_CONSTRAINT, "database_update_schema_hash constraint");

    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO cloudsync_schema_versions (hash, seq) "
             "VALUES (%" PRId64 ", COALESCE((SELECT MAX(seq) FROM cloudsync_schema_versions), 0) + 1) "
             "ON CONFLICT(hash) DO UPDATE SET "
             "seq = (SELECT COALESCE(MAX(seq), 0) + 1 FROM cloudsync_schema_versions);",
             h);
    rc = database_exec(db, sql);
    if (rc == DBRES_OK && hash) {
        *hash = h;
        return rc;
    } 

    return set_last_error(DBRES_ERROR, "database_update_schema_hash error 2");
}

// MARK: - VM -

int database_prepare (db_t *db, const char *sql, dbvm_t **vm, int flags) {
    elog(DEBUG1, "database_prepare: %s", sql);

    if (!sql || !vm) {
        return set_last_error(DBRES_ERROR, "Invalid parameters to database_prepare");
    }

    // Convert ? placeholders to $1, $2, etc.
    char *pg_sql = convert_placeholders(sql);
    if (!pg_sql) {
        return set_last_error(DBRES_ERROR, "Failed to convert SQL placeholders");
    }

    // Create wrapper - defer actual SPI_prepare until first step
    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)palloc0(sizeof(pg_stmt_wrapper_t));
    wrapper->sql = pg_sql;
    wrapper->plan = NULL;
    wrapper->portal = NULL;
    wrapper->current_row = 0;
    wrapper->prepared = false;
    wrapper->executed = false;
    wrapper->param_count = 0;

    // Initialize nulls array (not null by default)
    for (int i = 0; i < MAX_PARAMS; i++) {
        wrapper->nulls[i] = ' ';
    }

    *vm = (dbvm_t*)wrapper;
    return set_last_error(DBRES_OK, NULL);
}

int databasevm_step (dbvm_t *vm) {
    elog(DEBUG1, "databasevm_step: %s", databasevm_sql(vm));
    if (!vm) {
        return set_last_error(DBRES_ERROR, "NULL vm in databasevm_step");
    }

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;

    // First call - prepare and execute
    if (!wrapper->executed) {
        // Deferred prepare: Now that we have all bindings, we can prepare the plan
        if (!wrapper->prepared) {
            PG_TRY();
            {
                wrapper->plan = SPI_prepare(wrapper->sql, wrapper->param_count, wrapper->param_types);
                if (!wrapper->plan) {
                    return set_last_error(DBRES_ERROR, "SPI_prepare returned NULL");
                }
                wrapper->prepared = true;
            }
            PG_CATCH();
            {
                ErrorData *edata = CopyErrorData();
                int err = set_last_error(DBRES_ERROR, edata->message);
                FlushErrorState();
                FreeErrorData(edata);
                return err;
            }
            PG_END_TRY();
        }

        // Execute plan with buffered parameters
        int rc;
        PG_TRY();
        {
            rc = SPI_execute_plan(wrapper->plan, wrapper->params, wrapper->nulls, false, 0);
        }
        PG_CATCH();
        {
            ErrorData *edata = CopyErrorData();
            int err = set_last_error(DBRES_ERROR, edata->message);
            FlushErrorState();
            FreeErrorData(edata);
            wrapper->executed = true;
            return err;
        }
        PG_END_TRY();

        wrapper->executed = true;

        if (rc < 0) {
            return set_last_error(DBRES_ERROR, "SPI_execute_plan returned error code");
        }

        wrapper->current_row = 0;

        // For INSERT/UPDATE/DELETE, return DBRES_DONE regardless of rows affected
        if (rc == SPI_OK_INSERT || rc == SPI_OK_DELETE || rc == SPI_OK_UPDATE) {
            // Increment command counter to make changes visible
            CommandCounterIncrement();

            // Refresh snapshot
            if (ActiveSnapshotSet()) {
                PopActiveSnapshot();
            }
            PushActiveSnapshot(GetTransactionSnapshot());

            return set_last_error(DBRES_DONE, NULL);
        }

        // For SELECT, return DBRES_ROW if we have results, DBRES_DONE if empty
        if (rc == SPI_OK_SELECT || rc == SPI_OK_SELINTO) {
            if (SPI_processed > 0) {
                return set_last_error(DBRES_ROW, NULL);
            }
            return set_last_error(DBRES_DONE, NULL);
        }

        // For other successful operations, return DBRES_DONE
        return set_last_error(DBRES_DONE, NULL);
    }

    // Subsequent calls - fetch next row
    wrapper->current_row++;

    if (wrapper->current_row < (int)SPI_processed) {
        return set_last_error(DBRES_ROW, NULL);
    }

    return set_last_error(DBRES_DONE, NULL);
}

void databasevm_finalize (dbvm_t *vm) {
    elog(DEBUG1, "databasevm_finalize: %s", databasevm_sql(vm));
    if (!vm) return;

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;

    if (wrapper->portal) {
        SPI_cursor_close(wrapper->portal);
    }

    if (wrapper->plan) {
        SPI_freeplan(wrapper->plan);
    }

    if (wrapper->sql) {
        pfree(wrapper->sql);
    }

    pfree(wrapper);
}

void databasevm_reset (dbvm_t *vm) {
    elog(DEBUG1, "databasevm_reset: %s", databasevm_sql(vm));
    if (!vm) return;

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;

    if (wrapper->portal) {
        SPI_cursor_close(wrapper->portal);
        wrapper->portal = NULL;
    }

    wrapper->current_row = 0;
    wrapper->executed = false;
}

void databasevm_clear_bindings (dbvm_t *vm) {
    elog(DEBUG1, "databasevm_clear_bindings: %s", databasevm_sql(vm));
    if (!vm) return;

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;

    // Reset all bindings
    for (int i = 0; i < MAX_PARAMS; i++) {
        wrapper->params[i] = (Datum)0;
        wrapper->nulls[i] = ' ';
    }
    wrapper->param_count = 0;
}

const char *databasevm_sql (dbvm_t *vm) {
    if (!vm) return NULL;

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;
    return wrapper->sql;
}

// MARK: - PRIMARY KEY -

int database_pk_rowid (db_t *db, const char *table_name, char ***names, int *count) {
    // PostgreSQL doesn't have rowid concept like SQLite
    // Use OID or primary key columns instead
    return database_pk_names(db, table_name, names, count);
}

int database_pk_names (db_t *db, const char *table_name, char ***names, int *count) {
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

    int n = SPI_processed;
    char **pk_names = cloudsync_memory_alloc(n * sizeof(char*));
    if (!pk_names) return DBRES_NOMEM;

    for (int i = 0; i < n; i++) {
        HeapTuple tuple = SPI_tuptable->vals[i];
        bool isnull;
        Datum datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull) {
            text *txt = DatumGetTextP(datum);
            char *name = text_to_cstring(txt);
            pk_names[i] = cloudsync_string_dup(name, false);
        } else {
            pk_names[i] = NULL;
        }
    }

    *names = pk_names;
    *count = n;
    return DBRES_OK;
}

// MARK: - BINDING -

int databasevm_bind_blob (dbvm_t *vm, int index, const void *value, uint64_t size) {
    if (!vm || index < 1 || !value) return DBRES_ERROR;

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;
    int idx = index - 1;

    if (idx >= MAX_PARAMS) return DBRES_ERROR;

    // Convert binary data to PostgreSQL bytea
    bytea *ba = (bytea*)palloc(size + VARHDRSZ);
    SET_VARSIZE(ba, size + VARHDRSZ);
    memcpy(VARDATA(ba), value, size);

    wrapper->params[idx] = PointerGetDatum(ba);
    wrapper->param_types[idx] = BYTEAOID;
    wrapper->nulls[idx] = ' ';

    if (index > wrapper->param_count) {
        wrapper->param_count = index;
    }

    return DBRES_OK;
}

int databasevm_bind_double (dbvm_t *vm, int index, double value) {
    if (!vm || index < 1) return DBRES_ERROR;

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;
    int idx = index - 1;

    if (idx >= MAX_PARAMS) return DBRES_ERROR;

    wrapper->params[idx] = Float8GetDatum(value);
    wrapper->param_types[idx] = FLOAT8OID;
    wrapper->nulls[idx] = ' ';

    if (index > wrapper->param_count) {
        wrapper->param_count = index;
    }

    return DBRES_OK;
}

int databasevm_bind_int (dbvm_t *vm, int index, int64_t value) {
    if (!vm || index < 1) return DBRES_ERROR;

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;
    int idx = index - 1;

    if (idx >= MAX_PARAMS) return DBRES_ERROR;

    wrapper->params[idx] = Int64GetDatum(value);
    wrapper->param_types[idx] = INT8OID;
    wrapper->nulls[idx] = ' ';

    if (index > wrapper->param_count) {
        wrapper->param_count = index;
    }

    return DBRES_OK;
}

int databasevm_bind_null (dbvm_t *vm, int index) {
    if (!vm || index < 1) return DBRES_ERROR;

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;
    int idx = index - 1;

    if (idx >= MAX_PARAMS) return DBRES_ERROR;

    wrapper->params[idx] = (Datum)0;
    wrapper->param_types[idx] = TEXTOID;  // Default type for NULL
    wrapper->nulls[idx] = 'n';  // Mark as NULL

    if (index > wrapper->param_count) {
        wrapper->param_count = index;
    }

    return DBRES_OK;
}

int databasevm_bind_text (dbvm_t *vm, int index, const char *value, int size) {
    if (!vm || index < 1 || !value) return DBRES_ERROR;

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;
    int idx = index - 1;

    if (idx >= MAX_PARAMS) return DBRES_ERROR;

    // Convert C string to PostgreSQL text
    wrapper->params[idx] = CStringGetTextDatum(value);
    wrapper->param_types[idx] = TEXTOID;
    wrapper->nulls[idx] = ' ';

    if (index > wrapper->param_count) {
        wrapper->param_count = index;
    }

    return DBRES_OK;
}

int databasevm_bind_value (dbvm_t *vm, int index, dbvalue_t *value) {
    if (!vm) return DBRES_ERROR;

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;
    int idx = index - 1;

    if (idx >= MAX_PARAMS) return DBRES_ERROR;

    pgvalue_t *v = (pgvalue_t *)value;
    if (!v) {
        wrapper->params[idx] = (Datum)0;
        wrapper->param_types[idx] = TEXTOID;
        wrapper->nulls[idx] = 'n';
    } else {
        wrapper->params[idx] = v->isnull ? (Datum)0 : v->datum;
        wrapper->param_types[idx] = OidIsValid(v->typeid) ? v->typeid : TEXTOID;
        wrapper->nulls[idx] = v->isnull ? 'n' : ' ';
    }

    if (index > wrapper->param_count) {
        wrapper->param_count = index;
    }

    return DBRES_OK;
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
    if (!v) return;

    if (v->owned_detoast) {
        pfree(v->owned_detoast);
    }
    if (v->owns_cstring && v->cstring) {
        pfree(v->cstring);
    }
    pfree(v);
}

void *database_value_dup (dbvalue_t *value) {
    pgvalue_t *v = (pgvalue_t *)value;
    if (!v) return NULL;

    pgvalue_t *copy = pgvalue_create(v->datum, v->typeid, v->typmod, v->collation, v->isnull, CurrentMemoryContext);
    if (v->detoasted && v->owned_detoast) {
        Size len = VARSIZE_ANY(v->owned_detoast);
        copy->owned_detoast = palloc(len);
        memcpy(copy->owned_detoast, v->owned_detoast, len);
        copy->datum = PointerGetDatum(copy->owned_detoast);
        copy->detoasted = true;
    }
    if (v->cstring) {
        copy->cstring = pstrdup(v->cstring);
        copy->owns_cstring = true;
    }
    return (void*)copy;
}

// MARK: - COLUMN -

const void *database_column_blob (dbvm_t *vm, int index) {
    if (!vm) return NULL;

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;

    if (!SPI_tuptable || wrapper->current_row >= SPI_processed) return NULL;

    HeapTuple tuple = SPI_tuptable->vals[wrapper->current_row];
    bool isnull;
    Datum datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, index + 1, &isnull);

    if (isnull) return NULL;

    bytea *ba = DatumGetByteaP(datum);
    return VARDATA(ba);
}

double database_column_double (dbvm_t *vm, int index) {
    if (!vm) return 0.0;

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;

    if (!SPI_tuptable || wrapper->current_row >= SPI_processed) return 0.0;

    HeapTuple tuple = SPI_tuptable->vals[wrapper->current_row];
    bool isnull;
    Datum datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, index + 1, &isnull);

    if (isnull) return 0.0;

    Oid typeid = SPI_gettypeid(SPI_tuptable->tupdesc, index + 1);
    switch (typeid) {
        case FLOAT4OID:
            return (double)DatumGetFloat4(datum);
        case FLOAT8OID:
            return DatumGetFloat8(datum);
        default:
            return 0.0;
    }
}

int64_t database_column_int (dbvm_t *vm, int index) {
    if (!vm) return 0;

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;

    if (!SPI_tuptable || wrapper->current_row >= SPI_processed) return 0;

    HeapTuple tuple = SPI_tuptable->vals[wrapper->current_row];
    bool isnull;
    Datum datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, index + 1, &isnull);

    if (isnull) return 0;

    Oid typeid = SPI_gettypeid(SPI_tuptable->tupdesc, index + 1);
    switch (typeid) {
        case INT2OID:
            return (int64_t)DatumGetInt16(datum);
        case INT4OID:
            return (int64_t)DatumGetInt32(datum);
        case INT8OID:
            return DatumGetInt64(datum);
        default:
            return 0;
    }
}

const char *database_column_text (dbvm_t *vm, int index) {
    if (!vm) return "";

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;

    if (!SPI_tuptable || wrapper->current_row >= SPI_processed) return "";

    HeapTuple tuple = SPI_tuptable->vals[wrapper->current_row];
    bool isnull;
    Datum datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, index + 1, &isnull);

    if (isnull) return "";

    text *txt = DatumGetTextP(datum);
    return text_to_cstring(txt);
}

dbvalue_t *database_column_value (dbvm_t *vm, int index) {
    if (!vm) return NULL;

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;

    if (!SPI_tuptable || wrapper->current_row >= SPI_processed) return NULL;

    HeapTuple tuple = SPI_tuptable->vals[wrapper->current_row];
    bool isnull;
    Datum datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, index + 1, &isnull);
    Oid typeid = SPI_gettypeid(SPI_tuptable->tupdesc, index + 1);
    int32 typmod = TupleDescAttr(SPI_tuptable->tupdesc, index + 1)->atttypmod;
    Oid collation = TupleDescAttr(SPI_tuptable->tupdesc, index + 1)->attcollation;

    pgvalue_t *v = pgvalue_create(datum, typeid, typmod, collation, isnull, CurrentMemoryContext);
    return (dbvalue_t*)v;
}

int database_column_bytes (dbvm_t *vm, int index) {
    if (!vm) return 0;

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;

    if (!SPI_tuptable || wrapper->current_row >= SPI_processed) return 0;

    HeapTuple tuple = SPI_tuptable->vals[wrapper->current_row];
    bool isnull;
    Datum datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, index + 1, &isnull);

    if (isnull) return 0;

    Oid typeid = SPI_gettypeid(SPI_tuptable->tupdesc, index + 1);
    if (typeid == BYTEAOID) {
        bytea *ba = DatumGetByteaP(datum);
        return VARSIZE(ba) - VARHDRSZ;
    }

    // For text, return string length
    text *txt = DatumGetTextP(datum);
    return VARSIZE(txt) - VARHDRSZ;
}

int database_column_type (dbvm_t *vm, int index) {
    if (!vm || !SPI_tuptable || !SPI_tuptable->tupdesc) return DBTYPE_NULL;

    pg_stmt_wrapper_t *wrapper = (pg_stmt_wrapper_t*)vm;

    if (index >= SPI_tuptable->tupdesc->natts) return DBTYPE_NULL;

    if (wrapper->current_row < 0 || wrapper->current_row >= (int)SPI_processed) {
        elog(DEBUG1,  "databasevm_step no rows current_row=%d processed=%lu", wrapper->current_row, SPI_processed);
        return DBTYPE_NULL;
    }

    // Check if the value is NULL
    bool isnull;
    HeapTuple tuple = SPI_tuptable->vals[wrapper->current_row];
    SPI_getbinval(tuple, SPI_tuptable->tupdesc, index + 1, &isnull);

    if (isnull) return DBTYPE_NULL;

    // Value is not NULL, return type based on column definition
    Oid typeid = SPI_gettypeid(SPI_tuptable->tupdesc, index + 1);

    switch (typeid) {
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
        default:
            return DBTYPE_TEXT;  // Default to text
    }
}

// MARK: - RESULT -

void database_result_blob (dbcontext_t *context, const void *value, uint64_t size, void(*destructor)(void*)) {
    // For PostgreSQL extension functions
    // This would need proper implementation in the extension context
    elog(WARNING, "database_result_blob not implemented");
}

void database_result_double (dbcontext_t *context, double value) {
    elog(WARNING, "database_result_double not implemented");
}

void database_result_int (dbcontext_t *context, int64_t value) {
    elog(WARNING, "database_result_int not implemented");
}

void database_result_null (dbcontext_t *context) {
    elog(WARNING, "database_result_null not implemented");
}

void database_result_text (dbcontext_t *context, const char *value, int size, void(*destructor)(void*)) {
    elog(WARNING, "database_result_text not implemented");
}

void database_result_value (dbcontext_t *context, dbvalue_t *value) {
    elog(WARNING, "database_result_value not implemented");
}

// MARK: - SAVEPOINTS -

int database_begin_savepoint (db_t *db, const char *savepoint_name) {
    elog(DEBUG1, "database_begin_savepoint: %s", savepoint_name);

    PG_TRY();
    {
        BeginInternalSubTransaction(NULL);
        MemoryContextSwitchTo(CurTransactionContext);
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        int err = set_last_error(DBRES_ERROR, edata ? edata->message : "Failed to begin savepoint");
        FlushErrorState();
        if (edata) FreeErrorData(edata);
        return err;
    }
    PG_END_TRY();

    return set_last_error(DBRES_OK, NULL);
}

int database_commit_savepoint (db_t *db, const char *savepoint_name) {
    elog(DEBUG1, "database_commit_savepoint: %s", savepoint_name);

    PG_TRY();
    {
        ReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(CurTransactionContext);
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
        return DBRES_ERROR;
    }
    PG_END_TRY();

    return DBRES_OK;
}

int database_rollback_savepoint (db_t *db, const char *savepoint_name) {
    elog(DEBUG1, "database_rollback_savepoint: %s", savepoint_name);

    PG_TRY();
    {
        RollbackAndReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(CurTransactionContext);

        // Refresh snapshot
        if (ActiveSnapshotSet()) {
            PopActiveSnapshot();
        }
        PushActiveSnapshot(GetTransactionSnapshot());
    }
    PG_CATCH();
    {
        FlushErrorState();
        return DBRES_ERROR;
    }
    PG_END_TRY();

    return DBRES_OK;
}

// MARK: - MEMORY -

void *dbmem_alloc (uint64_t size) {
    return palloc(size);
}

void *dbmem_zeroalloc (uint64_t size) {
    void *ptr = palloc(size);
    if (ptr) {
        memset(ptr, 0, (size_t)size);
    }
    return ptr;
}

void *dbmem_realloc (void *ptr, uint64_t new_size) {
    return repalloc(ptr, new_size);
}

char *dbmem_mprintf(const char *format, ...) {
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
    char *result = (char*)palloc(len + 1);
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
    char *result = (char*)palloc(len + 1);
    vsnprintf(result, len + 1, format, list);

    return result;
}

void dbmem_free (void *ptr) {
    if (ptr) {
        pfree(ptr);
    }
}

uint64_t dbmem_size (void *ptr) {
    // PostgreSQL palloc doesn't expose allocated size directly
    // Return 0 as a safe default
    return 0;
}

// MARK: - CLOUDSYNC CALLBACK -

static cloudsync_payload_apply_callback_t payload_apply_callback = NULL;

void cloudsync_set_payload_apply_callback(db_t *db, cloudsync_payload_apply_callback_t callback) {
    payload_apply_callback = callback;
}

cloudsync_payload_apply_callback_t cloudsync_get_payload_apply_callback(db_t *db) {
    return payload_apply_callback;
}
