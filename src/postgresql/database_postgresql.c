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
    Oid             prepared_types[MAX_PARAMS]; // types used when plan was SPI_prepare'd
    int             prepared_nparams;           // nparams at prepare time
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

static int database_refresh_snapshot (void);

// MARK: - SQL -

static char *sql_escape_character (const char *name, char *buffer, size_t bsize, char c) {
    if (!name || !buffer || bsize < 1) {
        if (buffer && bsize > 0) buffer[0] = '\0';
        return NULL;
    }

    size_t i = 0, j = 0;

    while (name[i]) {
        if (name[i] == c) {
            // Need space for 2 chars (escaped c) + null
            if (j >= bsize - 2) {
                elog(WARNING, "Identifier name too long for buffer, truncated: %s", name);
                break;
            }
            buffer[j++] = c;
            buffer[j++] = c;
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

static char *sql_escape_identifier (const char *name, char *buffer, size_t bsize) {
    // PostgreSQL identifier escaping: double any embedded double quotes
    // Does NOT add surrounding quotes (caller's responsibility)
    // Similar to SQLite's %q behavior for escaping
    return sql_escape_character(name, buffer, bsize, '"');
}

static char *sql_escape_literal (const char *name, char *buffer, size_t bsize) {
    // Escapes single quotes for use inside SQL string literals: ' → ''
    // Does NOT add surrounding quotes (caller's responsibility)
    return sql_escape_character(name, buffer, bsize, '\'');
}

char *sql_build_drop_table (const char *table_name, char *buffer, int bsize, bool is_meta) {
    // Escape the table name (doubles any embedded quotes)
    char escaped[512];
    sql_escape_identifier(table_name, escaped, sizeof(escaped));

    // Add the surrounding quotes in the format string
    if (is_meta) {
        snprintf(buffer, bsize, "DROP TABLE IF EXISTS \"%s_cloudsync\";", escaped);
    } else {
        snprintf(buffer, bsize, "DROP TABLE IF EXISTS \"%s\";", escaped);
    }

    return buffer;
}

char *sql_build_select_nonpk_by_pk (cloudsync_context *data, const char *table_name, const char *schema) {
    UNUSED_PARAMETER(data);
    char *qualified = database_build_base_ref(schema, table_name);
    if (!qualified) return NULL;

    char *sql = cloudsync_memory_mprintf(SQL_BUILD_SELECT_NONPK_COLS_BY_PK, qualified);
    cloudsync_memory_free(qualified);
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

char *sql_build_delete_by_pk (cloudsync_context *data, const char *table_name, const char *schema) {
    UNUSED_PARAMETER(data);
    char *qualified = database_build_base_ref(schema, table_name);
    if (!qualified) return NULL;

    char *sql = cloudsync_memory_mprintf(SQL_BUILD_DELETE_ROW_BY_PK, qualified);
    cloudsync_memory_free(qualified);
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

char *sql_build_insert_pk_ignore (cloudsync_context *data, const char *table_name, const char *schema) {
    UNUSED_PARAMETER(data);
    char *qualified = database_build_base_ref(schema, table_name);
    if (!qualified) return NULL;

    char *sql = cloudsync_memory_mprintf(SQL_BUILD_INSERT_PK_IGNORE, qualified);
    cloudsync_memory_free(qualified);
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

char *sql_build_upsert_pk_and_col (cloudsync_context *data, const char *table_name, const char *colname, const char *schema) {
    UNUSED_PARAMETER(data);
    char *qualified = database_build_base_ref(schema, table_name);
    if (!qualified) return NULL;

    char *sql = cloudsync_memory_mprintf(SQL_BUILD_UPSERT_PK_AND_COL, qualified, colname, colname);
    cloudsync_memory_free(qualified);
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

char *sql_build_upsert_pk_and_multi_cols (cloudsync_context *data, const char *table_name, const char **colnames, int ncolnames, const char *schema) {
    if (ncolnames <= 0 || !colnames) return NULL;

    char *qualified = database_build_base_ref(schema, table_name);
    if (!qualified) return NULL;

    // Build VALUES list for column names: ('col_a',1),('col_b',2)
    // Column names are SQL literals here, so escape single quotes
    size_t values_cap = (size_t)ncolnames * 128 + 1;
    char *col_values = cloudsync_memory_alloc(values_cap);
    if (!col_values) { cloudsync_memory_free(qualified); return NULL; }

    size_t vpos = 0;
    for (int i = 0; i < ncolnames; i++) {
        char esc[1024];
        sql_escape_literal(colnames[i], esc, sizeof(esc));
        vpos += snprintf(col_values + vpos, values_cap - vpos, "%s('%s'::text,%d)",
                         i > 0 ? "," : "", esc, i + 1);
    }

    // Build meta-query that generates the final INSERT...ON CONFLICT SQL with proper types
    char *meta_sql = cloudsync_memory_mprintf(
        "WITH tbl AS ("
        "  SELECT to_regclass('%s') AS oid"
        "), "
        "pk AS ("
        "  SELECT a.attname, k.ord, format_type(a.atttypid, a.atttypmod) AS coltype "
        "  FROM pg_index x "
        "  JOIN tbl t ON t.oid = x.indrelid "
        "  JOIN LATERAL unnest(x.indkey) WITH ORDINALITY AS k(attnum, ord) ON true "
        "  JOIN pg_attribute a ON a.attrelid = x.indrelid AND a.attnum = k.attnum "
        "  WHERE x.indisprimary "
        "  ORDER BY k.ord"
        "), "
        "pk_count AS (SELECT count(*) AS n FROM pk), "
        "cols AS ("
        "  SELECT u.colname, format_type(a.atttypid, a.atttypmod) AS coltype, u.ord "
        "  FROM (VALUES %s) AS u(colname, ord) "
        "  JOIN pg_attribute a ON a.attrelid = (SELECT oid FROM tbl) AND a.attname = u.colname "
        "  WHERE a.attnum > 0 AND NOT a.attisdropped"
        ") "
        "SELECT "
        "  'INSERT INTO ' || (SELECT (oid::regclass)::text FROM tbl)"
        "  || ' (' || (SELECT string_agg(format('%%I', attname), ',' ORDER BY ord) FROM pk)"
        "  || ',' || (SELECT string_agg(format('%%I', colname), ',' ORDER BY ord) FROM cols) || ')'"
        "  || ' VALUES (' || (SELECT string_agg(format('$%%s::%%s', ord, coltype), ',' ORDER BY ord) FROM pk)"
        "  || ',' || (SELECT string_agg(format('$%%s::%%s', (SELECT n FROM pk_count) + ord, coltype), ',' ORDER BY ord) FROM cols) || ')'"
        "  || ' ON CONFLICT (' || (SELECT string_agg(format('%%I', attname), ',' ORDER BY ord) FROM pk) || ')'"
        "  || ' DO UPDATE SET ' || (SELECT string_agg(format('%%I=EXCLUDED.%%I', colname, colname), ',' ORDER BY ord) FROM cols)"
        "  || ';';",
        qualified, col_values
    );

    cloudsync_memory_free(qualified);
    cloudsync_memory_free(col_values);
    if (!meta_sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, meta_sql, &query);
    cloudsync_memory_free(meta_sql);

    return (rc == DBRES_OK) ? query : NULL;
}

char *sql_build_update_pk_and_multi_cols (cloudsync_context *data, const char *table_name, const char **colnames, int ncolnames, const char *schema) {
    if (ncolnames <= 0 || !colnames) return NULL;

    char *qualified = database_build_base_ref(schema, table_name);
    if (!qualified) return NULL;

    // Build VALUES list for column names: ('col_a',1),('col_b',2)
    size_t values_cap = (size_t)ncolnames * 128 + 1;
    char *col_values = cloudsync_memory_alloc(values_cap);
    if (!col_values) { cloudsync_memory_free(qualified); return NULL; }

    size_t vpos = 0;
    for (int i = 0; i < ncolnames; i++) {
        char esc[1024];
        sql_escape_literal(colnames[i], esc, sizeof(esc));
        vpos += snprintf(col_values + vpos, values_cap - vpos, "%s('%s'::text,%d)",
                         i > 0 ? "," : "", esc, i + 1);
    }

    // Build meta-query that generates UPDATE ... SET col=$ WHERE pk=$
    char *meta_sql = cloudsync_memory_mprintf(
        "WITH tbl AS ("
        "  SELECT to_regclass('%s') AS oid"
        "), "
        "pk AS ("
        "  SELECT a.attname, k.ord, format_type(a.atttypid, a.atttypmod) AS coltype "
        "  FROM pg_index x "
        "  JOIN tbl t ON t.oid = x.indrelid "
        "  JOIN LATERAL unnest(x.indkey) WITH ORDINALITY AS k(attnum, ord) ON true "
        "  JOIN pg_attribute a ON a.attrelid = x.indrelid AND a.attnum = k.attnum "
        "  WHERE x.indisprimary "
        "  ORDER BY k.ord"
        "), "
        "pk_count AS (SELECT count(*) AS n FROM pk), "
        "cols AS ("
        "  SELECT u.colname, format_type(a.atttypid, a.atttypmod) AS coltype, u.ord "
        "  FROM (VALUES %s) AS u(colname, ord) "
        "  JOIN pg_attribute a ON a.attrelid = (SELECT oid FROM tbl) AND a.attname = u.colname "
        "  WHERE a.attnum > 0 AND NOT a.attisdropped"
        ") "
        "SELECT "
        "  'UPDATE ' || (SELECT (oid::regclass)::text FROM tbl)"
        "  || ' SET ' || (SELECT string_agg(format('%%I=$%%s::%%s', colname, (SELECT n FROM pk_count) + ord, coltype), ',' ORDER BY ord) FROM cols)"
        "  || ' WHERE ' || (SELECT string_agg(format('%%I=$%%s::%%s', attname, ord, coltype), ' AND ' ORDER BY ord) FROM pk)"
        "  || ';';",
        qualified, col_values
    );

    cloudsync_memory_free(qualified);
    cloudsync_memory_free(col_values);
    if (!meta_sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, meta_sql, &query);
    cloudsync_memory_free(meta_sql);

    return (rc == DBRES_OK) ? query : NULL;
}

char *sql_build_select_cols_by_pk (cloudsync_context *data, const char *table_name, const char *colname, const char *schema) {
    UNUSED_PARAMETER(data);
    char *qualified = database_build_base_ref(schema, table_name);
    if (!qualified) return NULL;

    char *sql = cloudsync_memory_mprintf(SQL_BUILD_SELECT_COLS_BY_PK_FMT, qualified, colname);
    cloudsync_memory_free(qualified);
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

char *sql_build_rekey_pk_and_reset_version_except_col (cloudsync_context *data, const char *table_name, const char *except_col) {
    char *meta_ref = database_build_meta_ref(cloudsync_schema(data), table_name);
    if (!meta_ref) return NULL;

    char *result = cloudsync_memory_mprintf(SQL_CLOUDSYNC_REKEY_PK_AND_RESET_VERSION_EXCEPT_COL, meta_ref, except_col, meta_ref, meta_ref, except_col);
    cloudsync_memory_free(meta_ref);
    return result;
}

char *database_table_schema (const char *table_name) {
    if (!table_name) return NULL;

    // Build metadata table name
    char meta_table[256];
    snprintf(meta_table, sizeof(meta_table), "%s_cloudsync", table_name);

    // Query system catalogs to find the schema of the metadata table.
    // Rationale: The metadata table is created in the same schema as the base table,
    // so finding its location tells us which schema the table belongs to.
    const char *query =
        "SELECT n.nspname "
        "FROM pg_class c "
        "JOIN pg_namespace n ON c.relnamespace = n.oid "
        "WHERE c.relname = $1 "
        "AND c.relkind = 'r'";  // 'r' = ordinary table

    char *schema = NULL;

    if (SPI_connect() != SPI_OK_CONNECT) {
        return NULL;
    }

    Oid argtypes[1] = {TEXTOID};
    Datum values[1] = {CStringGetTextDatum(meta_table)};
    char nulls[1] = {' '};

    int rc = SPI_execute_with_args(query, 1, argtypes, values, nulls, true, 1);

    if (rc == SPI_OK_SELECT && SPI_processed > 0) {
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        HeapTuple tuple = SPI_tuptable->vals[0];
        bool isnull;

        Datum datum = SPI_getbinval(tuple, tupdesc, 1, &isnull);
        if (!isnull) {
            // pg_namespace.nspname is type 'name', not 'text'
            Name nspname = DatumGetName(datum);
            schema = cloudsync_string_dup(NameStr(*nspname));
        }
    }

    if (SPI_tuptable) {
        SPI_freetuptable(SPI_tuptable);
    }
    pfree(DatumGetPointer(values[0]));
    SPI_finish();

    // Returns NULL if metadata table doesn't exist yet (during initialization).
    // Caller should fall back to cloudsync_schema() in this case.
    return schema;
}

char *database_build_meta_ref (const char *schema, const char *table_name) {
    char escaped_table[512];
    sql_escape_identifier(table_name, escaped_table, sizeof(escaped_table));
    if (schema) {
        char escaped_schema[512];
        sql_escape_identifier(schema, escaped_schema, sizeof(escaped_schema));
        return cloudsync_memory_mprintf("\"%s\".\"%s_cloudsync\"", escaped_schema, escaped_table);
    }
    return cloudsync_memory_mprintf("\"%s_cloudsync\"", escaped_table);
}

char *database_build_base_ref (const char *schema, const char *table_name) {
    char escaped_table[512];
    sql_escape_identifier(table_name, escaped_table, sizeof(escaped_table));
    if (schema) {
        char escaped_schema[512];
        sql_escape_identifier(schema, escaped_schema, sizeof(escaped_schema));
        return cloudsync_memory_mprintf("\"%s\".\"%s\"", escaped_schema, escaped_table);
    }
    return cloudsync_memory_mprintf("\"%s\"", escaped_table);
}

char *database_build_blocks_ref (const char *schema, const char *table_name) {
    char escaped_table[512];
    sql_escape_identifier(table_name, escaped_table, sizeof(escaped_table));
    if (schema) {
        char escaped_schema[512];
        sql_escape_identifier(schema, escaped_schema, sizeof(escaped_schema));
        return cloudsync_memory_mprintf("\"%s\".\"%s_cloudsync_blocks\"", escaped_schema, escaped_table);
    }
    return cloudsync_memory_mprintf("\"%s_cloudsync_blocks\"", escaped_table);
}

// Schema-aware SQL builder for PostgreSQL: deletes columns not in schema or pkcol.
// Schema parameter: pass empty string to fall back to current_schema() via SQL.
char *sql_build_delete_cols_not_in_schema_query (const char *schema, const char *table_name, const char *meta_ref, const char *pkcol) {
    const char *schema_param = schema ? schema : "";

    char esc_table[1024], esc_schema[1024];
    sql_escape_literal(table_name, esc_table, sizeof(esc_table));
    sql_escape_literal(schema_param, esc_schema, sizeof(esc_schema));

    return cloudsync_memory_mprintf(
        "DELETE FROM %s WHERE col_name NOT IN ("
        "SELECT column_name FROM information_schema.columns WHERE table_name = '%s' "
        "AND table_schema = COALESCE(NULLIF('%s', ''), current_schema()) "
        "UNION SELECT '%s'"
        ");",
        meta_ref, esc_table, esc_schema, pkcol
    );
}

// Builds query to get comma-separated list of primary key column names.
char *sql_build_pk_collist_query (const char *schema, const char *table_name) {
    const char *schema_param = schema ? schema : "";

    char esc_table[1024], esc_schema[1024];
    sql_escape_literal(table_name, esc_table, sizeof(esc_table));
    sql_escape_literal(schema_param, esc_schema, sizeof(esc_schema));

    return cloudsync_memory_mprintf(
        "SELECT string_agg(quote_ident(column_name), ',') "
        "FROM information_schema.key_column_usage "
        "WHERE table_name = '%s' AND table_schema = COALESCE(NULLIF('%s', ''), current_schema()) "
        "AND constraint_name LIKE '%%_pkey';",
        esc_table, esc_schema
    );
}

// Builds query to get SELECT list of decoded primary key columns.
char *sql_build_pk_decode_selectlist_query (const char *schema, const char *table_name) {
    const char *schema_param = schema ? schema : "";

    char esc_table[1024], esc_schema[1024];
    sql_escape_literal(table_name, esc_table, sizeof(esc_table));
    sql_escape_literal(schema_param, esc_schema, sizeof(esc_schema));

    return cloudsync_memory_mprintf(
        "SELECT string_agg("
        "'cloudsync_pk_decode(pk, ' || ordinal_position || ') AS ' || quote_ident(column_name), ',' ORDER BY ordinal_position"
        ") "
        "FROM information_schema.key_column_usage "
        "WHERE table_name = '%s' AND table_schema = COALESCE(NULLIF('%s', ''), current_schema()) "
        "AND constraint_name LIKE '%%_pkey';",
        esc_table, esc_schema
    );
}

// Builds query to get qualified (schema.table.column) primary key column list.
char *sql_build_pk_qualified_collist_query (const char *schema, const char *table_name) {
    const char *schema_param = schema ? schema : "";

    char esc_table[1024], esc_schema[1024];
    sql_escape_literal(table_name, esc_table, sizeof(esc_table));
    sql_escape_literal(schema_param, esc_schema, sizeof(esc_schema));

    return cloudsync_memory_mprintf(
        "SELECT string_agg(quote_ident(column_name), ',' ORDER BY ordinal_position) "
        "FROM information_schema.key_column_usage "
        "WHERE table_name = '%s' AND table_schema = COALESCE(NULLIF('%s', ''), current_schema()) "
        "AND constraint_name LIKE '%%_pkey';", esc_table, esc_schema
    );
}

char *sql_build_insert_missing_pks_query(const char *schema, const char *table_name,
                                         const char *pkvalues_identifiers,
                                         const char *base_ref, const char *meta_ref,
                                         const char *filter) {
    UNUSED_PARAMETER(schema);

    char esc_table[1024];
    sql_escape_literal(table_name, esc_table, sizeof(esc_table));

    // PostgreSQL: Use NOT EXISTS with cloudsync_pk_encode to avoid EXCEPT type mismatch.
    //
    // CRITICAL: Pass PK columns directly to VARIADIC functions (NOT wrapped in ARRAY[]).
    // This preserves each column's actual type (TEXT, INTEGER, etc.) for correct pk_encode.
    // Using ARRAY[] would require all elements to be the same type, causing errors with
    // mixed-type composite PKs (e.g., TEXT + INTEGER).
    //
    // Example: cloudsync_insert('table', col1, col2) where col1=TEXT, col2=INTEGER
    // PostgreSQL's VARIADIC handling preserves each type and matches SQLite's encoding.
    if (filter) {
        return cloudsync_memory_mprintf(
            "SELECT cloudsync_insert('%s', %s) "
            "FROM %s b "
            "WHERE (%s) AND NOT EXISTS ("
            "    SELECT 1 FROM %s m WHERE m.pk = cloudsync_pk_encode(%s)"
            ");",
            esc_table, pkvalues_identifiers, base_ref, filter, meta_ref, pkvalues_identifiers
        );
    }
    return cloudsync_memory_mprintf(
        "SELECT cloudsync_insert('%s', %s) "
        "FROM %s b "
        "WHERE NOT EXISTS ("
        "    SELECT 1 FROM %s m WHERE m.pk = cloudsync_pk_encode(%s)"
        ");",
        esc_table, pkvalues_identifiers, base_ref, meta_ref, pkvalues_identifiers
    );
}

// MARK: - HELPER FUNCTIONS -

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
    
    // Always clear portal_open first to maintain consistent state
    stmt->portal_open = false;
    
    if (!stmt->portal) return;
    
    PG_TRY();
    {
        SPI_cursor_close(stmt->portal);
    }
    PG_CATCH();
    {
        // Log but don't throw - we're cleaning up
        FlushErrorState();
    }
    PG_END_TRY();
    stmt->portal = NULL;
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
    if (int_value) *int_value = 0;
    if (expected_type != DBTYPE_INTEGER && expected_type != DBTYPE_TEXT && expected_type != DBTYPE_BLOB) {
        return cloudsync_set_error(data, "Invalid expected_type", DBRES_MISUSE);
    }

    int rc = SPI_execute(sql, true, 0);
    if (rc < 0) {
        rc = cloudsync_set_error(data, "SPI_execute failed in database_select1_value", DBRES_ERROR);
        goto cleanup;
    }

    // ensure at least one column
    if (!SPI_tuptable || !SPI_tuptable->tupdesc) {
        rc = cloudsync_set_error(data, "No result table", DBRES_ERROR);
        goto cleanup;
    }
    if (SPI_tuptable->tupdesc->natts < 1) {
        rc = cloudsync_set_error(data, "No columns in result", DBRES_ERROR);
        goto cleanup;
    }

    // no rows OK
    if (SPI_processed == 0) {
        rc = DBRES_OK;
        goto cleanup;
    }

    HeapTuple tuple = SPI_tuptable->vals[0];
    bool isnull;
    Datum datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, &isnull);

    // NULL value is OK
    if (isnull) {
        rc = DBRES_OK;
        goto cleanup;
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
                rc = cloudsync_set_error(data, "Type mismatch: expected integer", DBRES_ERROR);
                goto cleanup;
        }
    } else if (expected_type == DBTYPE_TEXT) {
        char *val = SPI_getvalue(tuple, SPI_tuptable->tupdesc, 1);
        if (val) {
            size_t len = strlen(val);
            char *ptr = cloudsync_memory_alloc(len + 1);
            if (!ptr) {
                pfree(val);
                rc = cloudsync_set_error(data, "Memory allocation failed", DBRES_NOMEM);
                goto cleanup;
            }
            memcpy(ptr, val, len);
            ptr[len] = '\0';
            if (ptr_value) *ptr_value = ptr;
            if (int_value) *int_value = (int64_t)len;
            pfree(val);
        }
    } else if (expected_type == DBTYPE_BLOB) {
        bytea *ba = DatumGetByteaP(datum);
        int len = VARSIZE(ba) - VARHDRSZ;
        if (len > 0) {
            char *ptr = cloudsync_memory_alloc(len);
            if (!ptr) {
                rc = cloudsync_set_error(data, "Memory allocation failed", DBRES_NOMEM);
                goto cleanup;
            }
            memcpy(ptr, VARDATA(ba), len);
            if (ptr_value) *ptr_value = ptr;
            if (int_value) *int_value = len;
        }
    }

    rc = DBRES_OK;

cleanup:
    if (SPI_tuptable) SPI_freetuptable(SPI_tuptable);
    return rc;
}

int database_select2_values (cloudsync_context *data, const char *sql, char **value, int64_t *len, int64_t *value2) {
    cloudsync_reset_error(data);
    
    // init values
    *value = NULL;
    *value2 = 0;
    *len = 0;

    int rc = SPI_execute(sql, true, 0);
    if (rc < 0) {
        rc = cloudsync_set_error(data, "SPI_execute failed in database_select2_values", DBRES_ERROR);
        goto cleanup;
    }

    if (!SPI_tuptable || !SPI_tuptable->tupdesc) {
        rc = cloudsync_set_error(data, "No result table in database_select2_values", DBRES_ERROR);
        goto cleanup;
    }
    if (SPI_tuptable->tupdesc->natts < 2) {
        rc = cloudsync_set_error(data, "Result has fewer than 2 columns in database_select2_values", DBRES_ERROR);
        goto cleanup;
    }
    if (SPI_processed == 0) {
        rc = DBRES_OK;
        goto cleanup;
    }

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
                if (!ptr) {
                    rc = DBRES_NOMEM;
                    goto cleanup;
                }
                
                memcpy(ptr, VARDATA(ba), blob_len);
                *value = ptr;
                *len = blob_len;
            }
        } else {
            text *txt = DatumGetTextP(datum1);
            int text_len = VARSIZE(txt) - VARHDRSZ;
            if (text_len > 0) {
                char *ptr = cloudsync_memory_alloc(text_len + 1);
                if (!ptr) {
                    rc = DBRES_NOMEM;
                    goto cleanup;
                }
                
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

    rc = DBRES_OK;

cleanup:
    if (SPI_tuptable) SPI_freetuptable(SPI_tuptable);
    return rc;
}

static bool database_system_exists (cloudsync_context *data, const char *name, const char *type, bool force_public, const char *schema) {
      if (!name || !type) return false;
      cloudsync_reset_error(data);

      bool exists = false;
      const char *query;
      // Schema parameter: pass empty string to fall back to current_schema() via SQL
      const char *schema_param = (schema && schema[0]) ? schema : "";

      if (strcmp(type, "table") == 0) {
          if (force_public) {
              query = "SELECT 1 FROM pg_tables WHERE schemaname = 'public' AND tablename = $1";
          } else {
              query = "SELECT 1 FROM pg_tables WHERE schemaname = COALESCE(NULLIF($2, ''), current_schema()) AND tablename = $1";
          }
      } else if (strcmp(type, "trigger") == 0) {
          query = "SELECT 1 FROM pg_trigger WHERE tgname = $1";
      } else {
          return false;
      }

      bool need_schema_param = !force_public && strcmp(type, "trigger") != 0;
      Datum datum_name = CStringGetTextDatum(name);
      Datum datum_schema = need_schema_param ? CStringGetTextDatum(schema_param) : (Datum)0;

      MemoryContext oldcontext = CurrentMemoryContext;
      PG_TRY();
      {
          if (!need_schema_param) {
              // force_public or trigger: only need table/trigger name parameter
              Oid argtypes[1] = {TEXTOID};
              Datum values[1] = {datum_name};
              char nulls[1] = {' '};
              int rc = SPI_execute_with_args(query, 1, argtypes, values, nulls, true, 0);
              exists = (rc >= 0 && SPI_processed > 0);
              if (SPI_tuptable) SPI_freetuptable(SPI_tuptable);
          } else {
              // table with schema parameter
              Oid argtypes[2] = {TEXTOID, TEXTOID};
              Datum values[2] = {datum_name, datum_schema};
              char nulls[2] = {' ', ' '};
              int rc = SPI_execute_with_args(query, 2, argtypes, values, nulls, true, 0);
              exists = (rc >= 0 && SPI_processed > 0);
              if (SPI_tuptable) SPI_freetuptable(SPI_tuptable);
          }
      }
      PG_CATCH();
      {
          MemoryContextSwitchTo(oldcontext);
          ErrorData *edata = CopyErrorData();
          cloudsync_set_error(data, edata->message, DBRES_ERROR);
          FreeErrorData(edata);
          FlushErrorState();
          exists = false;
      }
      PG_END_TRY();

      pfree(DatumGetPointer(datum_name));
      if (need_schema_param) pfree(DatumGetPointer(datum_schema));

      elog(DEBUG1, "database_system_exists %s: %d", name, exists);
      return exists;
  }

// MARK: - GENERAL -

int database_exec (cloudsync_context *data, const char *sql) {
    if (!sql) return cloudsync_set_error(data, "SQL statement is NULL", DBRES_ERROR);
    cloudsync_reset_error(data);

    int rc;
    bool is_error = false;
    MemoryContext oldcontext = CurrentMemoryContext;
    PG_TRY();
    {
        rc = SPI_execute(sql, false, 0);
        if (SPI_tuptable) {
            SPI_freetuptable(SPI_tuptable);
        }
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(oldcontext);
        ErrorData *edata = CopyErrorData();
        rc = cloudsync_set_error(data, edata->message, DBRES_ERROR);
        FreeErrorData(edata);
        FlushErrorState();
        if (SPI_tuptable) {
            SPI_freetuptable(SPI_tuptable);
        }
        is_error = true;
    }
    PG_END_TRY();

    if (is_error) return rc;

    // Increment command counter to make changes visible
    if (rc >= 0) {
        database_refresh_snapshot();
        return map_spi_result(rc);
    }

    return cloudsync_set_error(data, "SPI_execute failed", DBRES_ERROR);
}

int database_exec_callback (cloudsync_context *data, const char *sql, int (*callback)(void *xdata, int argc, char **values, char **names), void *xdata) {
    if (!sql) return cloudsync_set_error(data, "SQL statement is NULL", DBRES_ERROR);
    cloudsync_reset_error(data);

    int rc;
    bool is_error = false;
    MemoryContext oldcontext = CurrentMemoryContext;
    PG_TRY();
    {
        rc = SPI_execute(sql, true, 0);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(oldcontext);
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
        if (!tupdesc) {
            SPI_freetuptable(SPI_tuptable);
            return cloudsync_set_error(data, "Invalid tuple descriptor", DBRES_ERROR);
        }

        int ncols = tupdesc->natts;
        if (ncols <= 0) {
            SPI_freetuptable(SPI_tuptable);
            return DBRES_OK;
        }

        // IMPORTANT: Save SPI state before any callback can modify it.
        // Callbacks may execute SPI queries which overwrite global SPI_tuptable.
        // We must copy all data we need BEFORE calling any callbacks.
        uint64 nrows = SPI_processed;
        SPITupleTable *saved_tuptable = SPI_tuptable;

        // No rows to process - free tuptable and return success
        if (nrows == 0) {
            SPI_freetuptable(saved_tuptable);
            return DBRES_OK;
        }

        // Allocate array for column names (shared across all rows)
        char **names = cloudsync_memory_alloc(ncols * sizeof(char*));
        if (!names) {
            SPI_freetuptable(saved_tuptable);
            return DBRES_NOMEM;
        }

        // Get column names - make copies to avoid pointing to internal memory
        for (int i = 0; i < ncols; i++) {
            Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
            if (attr) {
                names[i] = cloudsync_string_dup(NameStr(attr->attname));
            } else {
                names[i] = NULL;
            }
        }

        // Pre-extract ALL row values before calling any callbacks.
        // This prevents SPI state corruption when callbacks run queries.
        char ***all_values = cloudsync_memory_alloc(nrows * sizeof(char**));
        if (!all_values) {
            for (int i = 0; i < ncols; i++) {
                if (names[i]) cloudsync_memory_free(names[i]);
            }
            cloudsync_memory_free(names);
            SPI_freetuptable(saved_tuptable);
            return DBRES_NOMEM;
        }

        // Extract values from all tuples
        for (uint64 row = 0; row < nrows; row++) {
            HeapTuple tuple = saved_tuptable->vals[row];
            all_values[row] = cloudsync_memory_alloc(ncols * sizeof(char*));
            if (!all_values[row]) {
                // Cleanup already allocated rows
                for (uint64 r = 0; r < row; r++) {
                    for (int c = 0; c < ncols; c++) {
                        if (all_values[r][c]) pfree(all_values[r][c]);
                    }
                    cloudsync_memory_free(all_values[r]);
                }
                cloudsync_memory_free(all_values);
                for (int i = 0; i < ncols; i++) {
                    if (names[i]) cloudsync_memory_free(names[i]);
                }
                cloudsync_memory_free(names);
                SPI_freetuptable(saved_tuptable);
                return DBRES_NOMEM;
            }

            if (!tuple) {
                for (int i = 0; i < ncols; i++) all_values[row][i] = NULL;
                continue;
            }

            for (int i = 0; i < ncols; i++) {
                bool isnull;
                SPI_getbinval(tuple, tupdesc, i + 1, &isnull);
                all_values[row][i] = (isnull) ? NULL : SPI_getvalue(tuple, tupdesc, i + 1);
            }
        }

        // Free SPI_tuptable BEFORE calling callbacks - we have all data we need
        SPI_freetuptable(saved_tuptable);
        SPI_tuptable = NULL;

        // Now process each row - callbacks can safely run SPI queries
        int result = DBRES_OK;
        for (uint64 row = 0; row < nrows; row++) {
            int cb_rc = callback(xdata, ncols, all_values[row], names);

            if (cb_rc != 0) {
                char errmsg[1024];
                snprintf(errmsg, sizeof(errmsg), "database_exec_callback aborted %d", cb_rc);
                result = cloudsync_set_error(data, errmsg, DBRES_ABORT);
                break;
            }
        }

        // Cleanup all extracted values
        for (uint64 row = 0; row < nrows; row++) {
            for (int i = 0; i < ncols; i++) {
                if (all_values[row][i]) pfree(all_values[row][i]);
            }
            cloudsync_memory_free(all_values[row]);
        }
        cloudsync_memory_free(all_values);

        // Free column names
        for (int i = 0; i < ncols; i++) {
            if (names[i]) cloudsync_memory_free(names[i]);
        }
        cloudsync_memory_free(names);

        return result;
    }

    if (SPI_tuptable) SPI_freetuptable(SPI_tuptable);
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

int database_select_blob_int (cloudsync_context *data, const char *sql, char **value, int64_t *len, int64_t *value2) {
    return database_select2_values(data, sql, value, len, value2);
}

int database_cleanup (cloudsync_context *data) {
    // NOOP
    return DBRES_OK;
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

bool database_table_exists (cloudsync_context *data, const char *name, const char *schema) {
    return database_system_exists(data, name, "table", false, schema);
}

bool database_internal_table_exists (cloudsync_context *data, const char *name) {
    // Internal tables always in public schema
    return database_system_exists(data, name, "table", true, NULL);
}

bool database_trigger_exists (cloudsync_context *data, const char *name) {
    // Triggers: extract table name to get schema
    // Trigger names follow pattern: <table>_cloudsync_<type>
    // For now, pass NULL to use current_schema()
    return database_system_exists(data, name, "trigger", false, NULL);
}

// MARK: - SCHEMA INFO -

static int64_t database_count_bind (cloudsync_context *data, const char *sql, const char *table_name, const char *schema) {
    // Schema parameter: pass empty string to fall back to current_schema() via SQL
    const char *schema_param = (schema && schema[0]) ? schema : "";

    Oid argtypes[2] = {TEXTOID, TEXTOID};
    Datum values[2] = {CStringGetTextDatum(table_name), CStringGetTextDatum(schema_param)};
    char nulls[2] = {' ', ' '};

    int64_t count = 0;
    int rc = SPI_execute_with_args(sql, 2, argtypes, values, nulls, true, 0);
    if (rc >= 0 && SPI_processed > 0 && SPI_tuptable) {
        bool isnull;
        Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull) count = DatumGetInt64(d);
    }

    if (SPI_tuptable) SPI_freetuptable(SPI_tuptable);
    pfree(DatumGetPointer(values[0]));
    pfree(DatumGetPointer(values[1]));
    return count;
}

int database_count_pk (cloudsync_context *data, const char *table_name, bool not_null, const char *schema) {
    const char *sql =
            "SELECT COUNT(*) FROM information_schema.table_constraints tc "
            "JOIN information_schema.key_column_usage kcu ON tc.constraint_name = kcu.constraint_name "
            "  AND tc.table_schema = kcu.table_schema "
            "WHERE tc.table_name = $1 AND tc.table_schema = COALESCE(NULLIF($2, ''), current_schema()) "
            "AND tc.constraint_type = 'PRIMARY KEY'";

    return (int)database_count_bind(data, sql, table_name, schema);
}

int database_count_nonpk (cloudsync_context *data, const char *table_name, const char *schema) {
    const char *sql =
            "SELECT COUNT(*) FROM information_schema.columns c "
            "WHERE c.table_name = $1 "
            "AND c.table_schema = COALESCE(NULLIF($2, ''), current_schema()) "
            "AND c.column_name NOT IN ("
            "  SELECT kcu.column_name FROM information_schema.table_constraints tc "
            "  JOIN information_schema.key_column_usage kcu ON tc.constraint_name = kcu.constraint_name "
            "    AND tc.table_schema = kcu.table_schema "
            "  WHERE tc.table_name = $1 AND tc.table_schema = COALESCE(NULLIF($2, ''), current_schema()) "
            "  AND tc.constraint_type = 'PRIMARY KEY'"
            ")";

    return (int)database_count_bind(data, sql, table_name, schema);
}

int database_count_int_pk (cloudsync_context *data, const char *table_name, const char *schema) {
    const char *sql =
              "SELECT COUNT(*) FROM information_schema.columns c "
              "JOIN information_schema.key_column_usage kcu ON c.column_name = kcu.column_name AND c.table_schema = kcu.table_schema AND c.table_name = kcu.table_name "
              "JOIN information_schema.table_constraints tc ON kcu.constraint_name = tc.constraint_name AND kcu.table_schema = tc.table_schema "
              "WHERE c.table_name = $1 AND c.table_schema = COALESCE(NULLIF($2, ''), current_schema()) "
              "AND tc.constraint_type = 'PRIMARY KEY' "
              "AND c.data_type IN ('smallint', 'integer', 'bigint')";

    return (int)database_count_bind(data, sql, table_name, schema);
}

int database_count_notnull_without_default (cloudsync_context *data, const char *table_name, const char *schema) {
    const char *sql =
              "SELECT COUNT(*) FROM information_schema.columns c "
              "WHERE c.table_name = $1 "
              "AND c.table_schema = COALESCE(NULLIF($2, ''), current_schema()) "
              "AND c.is_nullable = 'NO' "
              "AND c.column_default IS NULL "
              "AND c.column_name NOT IN ("
              "  SELECT kcu.column_name FROM information_schema.table_constraints tc "
              "  JOIN information_schema.key_column_usage kcu ON tc.constraint_name = kcu.constraint_name "
              "    AND tc.table_schema = kcu.table_schema "
              "  WHERE tc.table_name = $1 AND tc.table_schema = COALESCE(NULLIF($2, ''), current_schema()) "
              "  AND tc.constraint_type = 'PRIMARY KEY'"
              ")";

    return (int)database_count_bind(data, sql, table_name, schema);
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
    int rc;
    const char *schema = cloudsync_schema(data);

    char *meta_ref = database_build_meta_ref(schema, table_name);
    if (!meta_ref) return DBRES_NOMEM;

    char *sql2 = cloudsync_memory_mprintf(
             "CREATE TABLE IF NOT EXISTS %s ("
             "pk BYTEA NOT NULL,"
             "col_name TEXT NOT NULL,"
             "col_version BIGINT,"
             "db_version BIGINT NOT NULL DEFAULT 0,"
             "seq INTEGER NOT NULL DEFAULT 0,"
             "site_id BIGINT NOT NULL DEFAULT 0,"
             "PRIMARY KEY (pk, col_name)"
             ");",
             meta_ref);
    if (!sql2) { cloudsync_memory_free(meta_ref); return DBRES_NOMEM; }

    rc = database_exec(data, sql2);
    cloudsync_memory_free(sql2);
    if (rc != DBRES_OK) { cloudsync_memory_free(meta_ref); return rc; }

    // Create indices for performance
    {
        char escaped_tbl[512], escaped_sch[512];
        sql_escape_identifier(table_name, escaped_tbl, sizeof(escaped_tbl));
        if (schema) {
            sql_escape_identifier(schema, escaped_sch, sizeof(escaped_sch));
            sql2 = cloudsync_memory_mprintf(
                     "CREATE INDEX IF NOT EXISTS \"%s_cloudsync_db_version_idx\" "
                     "ON \"%s\".\"%s_cloudsync\" (db_version);",
                     escaped_tbl, escaped_sch, escaped_tbl);
        } else {
            sql2 = cloudsync_memory_mprintf(
                     "CREATE INDEX IF NOT EXISTS \"%s_cloudsync_db_version_idx\" "
                     "ON \"%s_cloudsync\" (db_version);",
                     escaped_tbl, escaped_tbl);
        }
    }
    cloudsync_memory_free(meta_ref);
    if (!sql2) return DBRES_NOMEM;

    rc = database_exec(data, sql2);
    cloudsync_memory_free(sql2);
    return rc;
}

// MARK: - TRIGGERS -

static int database_create_insert_trigger_internal (cloudsync_context *data, const char *table_name, char *trigger_when, const char *schema) {
    if (!table_name) return DBRES_MISUSE;

    const char *schema_param = (schema && schema[0]) ? schema : "";

    char trigger_name[1024];
    char func_name[1024];
    char escaped_tbl[512];
    sql_escape_identifier(table_name, escaped_tbl, sizeof(escaped_tbl));
    snprintf(trigger_name, sizeof(trigger_name), "cloudsync_after_insert_%s", escaped_tbl);
    snprintf(func_name, sizeof(func_name), "cloudsync_after_insert_%s_fn", escaped_tbl);

    if (database_trigger_exists(data, trigger_name)) return DBRES_OK;

    char esc_tbl_literal[1024], esc_schema_literal[1024];
    sql_escape_literal(table_name, esc_tbl_literal, sizeof(esc_tbl_literal));
    sql_escape_literal(schema_param, esc_schema_literal, sizeof(esc_schema_literal));

    char sql[2048];
    snprintf(sql, sizeof(sql),
             "SELECT string_agg('NEW.' || quote_ident(kcu.column_name) || '::text', ',' ORDER BY kcu.ordinal_position) "
             "FROM information_schema.table_constraints tc "
             "JOIN information_schema.key_column_usage kcu "
             "  ON tc.constraint_name = kcu.constraint_name "
             "  AND tc.table_schema = kcu.table_schema "
             "WHERE tc.table_name = '%s' AND tc.table_schema = COALESCE(NULLIF('%s', ''), current_schema()) "
             "AND tc.constraint_type = 'PRIMARY KEY';",
             esc_tbl_literal, esc_schema_literal);

    char *pk_list = NULL;
    int rc = database_select_text(data, sql, &pk_list);
    if (rc != DBRES_OK) return rc;
    if (!pk_list || pk_list[0] == '\0') {
        if (pk_list) cloudsync_memory_free(pk_list);
        return cloudsync_set_error(data, "No primary key columns found for table", DBRES_ERROR);
    }

    char *sql2 = cloudsync_memory_mprintf(
        "CREATE OR REPLACE FUNCTION \"%s\"() RETURNS trigger AS $$ "
        "BEGIN "
        "  IF cloudsync_is_sync('%s') THEN RETURN NEW; END IF; "
        "  PERFORM cloudsync_insert('%s', %s); "
        "  RETURN NEW; "
        "END; "
        "$$ LANGUAGE plpgsql;",
        func_name, esc_tbl_literal, esc_tbl_literal, pk_list);
    cloudsync_memory_free(pk_list);
    if (!sql2) return DBRES_NOMEM;

    rc = database_exec(data, sql2);
    cloudsync_memory_free(sql2);
    if (rc != DBRES_OK) return rc;

    char *base_ref = database_build_base_ref(schema, table_name);
    if (!base_ref) return DBRES_NOMEM;

    sql2 = cloudsync_memory_mprintf(
        "CREATE TRIGGER \"%s\" AFTER INSERT ON %s %s "
        "EXECUTE FUNCTION \"%s\"();",
        trigger_name, base_ref, trigger_when ? trigger_when : "", func_name);
    cloudsync_memory_free(base_ref);
    if (!sql2) return DBRES_NOMEM;

    rc = database_exec(data, sql2);
    cloudsync_memory_free(sql2);
    return rc;
}

static int database_create_update_trigger_gos_internal (cloudsync_context *data, const char *table_name, const char *schema) {
    if (!table_name) return DBRES_MISUSE;

    char trigger_name[1024];
    char func_name[1024];
    char escaped_tbl[512];
    sql_escape_identifier(table_name, escaped_tbl, sizeof(escaped_tbl));
    snprintf(trigger_name, sizeof(trigger_name), "cloudsync_before_update_%s", escaped_tbl);
    snprintf(func_name, sizeof(func_name), "cloudsync_before_update_%s_fn", escaped_tbl);

    if (database_trigger_exists(data, trigger_name)) return DBRES_OK;

    char esc_tbl_literal[1024];
    sql_escape_literal(table_name, esc_tbl_literal, sizeof(esc_tbl_literal));

    char *sql = cloudsync_memory_mprintf(
        "CREATE OR REPLACE FUNCTION \"%s\"() RETURNS trigger AS $$ "
        "BEGIN "
        "  RAISE EXCEPTION 'Error: UPDATE operation is not allowed on table %s.'; "
        "END; "
        "$$ LANGUAGE plpgsql;",
        func_name, esc_tbl_literal);
    if (!sql) return DBRES_NOMEM;

    int rc = database_exec(data, sql);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) return rc;

    char *base_ref = database_build_base_ref(schema, table_name);
    if (!base_ref) return DBRES_NOMEM;

    sql = cloudsync_memory_mprintf(
        "CREATE TRIGGER \"%s\" BEFORE UPDATE ON %s "
        "FOR EACH ROW WHEN (cloudsync_is_enabled('%s') = true) "
        "EXECUTE FUNCTION \"%s\"();",
        trigger_name, base_ref, esc_tbl_literal, func_name);
    cloudsync_memory_free(base_ref);
    if (!sql) return DBRES_NOMEM;

    rc = database_exec(data, sql);
    cloudsync_memory_free(sql);
    return rc;
}

static int database_create_update_trigger_internal (cloudsync_context *data, const char *table_name, const char *trigger_when, const char *schema) {
    if (!table_name) return DBRES_MISUSE;

    const char *schema_param = (schema && schema[0]) ? schema : "";

    char trigger_name[1024];
    char func_name[1024];
    char escaped_tbl[512];
    sql_escape_identifier(table_name, escaped_tbl, sizeof(escaped_tbl));
    snprintf(trigger_name, sizeof(trigger_name), "cloudsync_after_update_%s", escaped_tbl);
    snprintf(func_name, sizeof(func_name), "cloudsync_after_update_%s_fn", escaped_tbl);

    if (database_trigger_exists(data, trigger_name)) return DBRES_OK;

    char esc_tbl_literal[1024], esc_schema_literal[1024];
    sql_escape_literal(table_name, esc_tbl_literal, sizeof(esc_tbl_literal));
    sql_escape_literal(schema_param, esc_schema_literal, sizeof(esc_schema_literal));

    char sql[2048];
    snprintf(sql, sizeof(sql),
           "SELECT string_agg("
           "  '(''%s'', NEW.' || quote_ident(kcu.column_name) || '::text, OLD.' || "
           "quote_ident(kcu.column_name) || '::text)', "
           "  ', ' ORDER BY kcu.ordinal_position"
           ") "
           "FROM information_schema.table_constraints tc "
           "JOIN information_schema.key_column_usage kcu "
           "  ON tc.constraint_name = kcu.constraint_name "
           "  AND tc.table_schema = kcu.table_schema "
           "WHERE tc.table_name = '%s' AND tc.table_schema = COALESCE(NULLIF('%s', ''), current_schema()) "
           "AND tc.constraint_type = 'PRIMARY KEY';",
           esc_tbl_literal, esc_tbl_literal, esc_schema_literal);

    char *pk_values_list = NULL;
    int rc = database_select_text(data, sql, &pk_values_list);
    if (rc != DBRES_OK) return rc;
    if (!pk_values_list || pk_values_list[0] == '\0') {
        if (pk_values_list) cloudsync_memory_free(pk_values_list);
        return cloudsync_set_error(data, "No primary key columns found for table", DBRES_ERROR);
    }

    snprintf(sql, sizeof(sql),
           "SELECT string_agg("
           "  '(''%s'', NEW.' || quote_ident(c.column_name) || '::text, OLD.' || "
           "quote_ident(c.column_name) || '::text)', "
           "  ', ' ORDER BY c.ordinal_position"
           ") "
           "FROM information_schema.columns c "
           "WHERE c.table_name = '%s' "
           "AND c.table_schema = COALESCE(NULLIF('%s', ''), current_schema()) "
           "AND NOT EXISTS ("
           "  SELECT 1 FROM information_schema.table_constraints tc "
           "  JOIN information_schema.key_column_usage kcu "
           "    ON tc.constraint_name = kcu.constraint_name "
           "    AND tc.table_schema = kcu.table_schema "
           "  WHERE tc.table_name = c.table_name "
           "  AND tc.table_schema = c.table_schema "
           "  AND tc.constraint_type = 'PRIMARY KEY' "
           "  AND kcu.column_name = c.column_name"
           ");",
           esc_tbl_literal, esc_tbl_literal, esc_schema_literal);

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
        "CREATE OR REPLACE FUNCTION \"%s\"() RETURNS trigger AS $$ "
        "BEGIN "
        "  IF cloudsync_is_sync('%s') THEN RETURN NEW; END IF; "
        "  PERFORM cloudsync_update(table_name, new_value, old_value) "
        "  FROM (%s) AS v(table_name, new_value, old_value); "
        "  RETURN NEW; "
        "END; "
        "$$ LANGUAGE plpgsql;",
        func_name, esc_tbl_literal, values_query);
    cloudsync_memory_free(values_query);
    if (!sql2) return DBRES_NOMEM;

    rc = database_exec(data, sql2);
    cloudsync_memory_free(sql2);
    if (rc != DBRES_OK) return rc;

    char *base_ref = database_build_base_ref(schema, table_name);
    if (!base_ref) return DBRES_NOMEM;

    sql2 = cloudsync_memory_mprintf(
        "CREATE TRIGGER \"%s\" AFTER UPDATE ON %s %s "
        "EXECUTE FUNCTION \"%s\"();",
        trigger_name, base_ref, trigger_when ? trigger_when : "", func_name);
    cloudsync_memory_free(base_ref);
    if (!sql2) return DBRES_NOMEM;

    rc = database_exec(data, sql2);
    cloudsync_memory_free(sql2);
    return rc;
}

static int database_create_delete_trigger_gos_internal (cloudsync_context *data, const char *table_name, const char *schema) {
    if (!table_name) return DBRES_MISUSE;

    char trigger_name[1024];
    char func_name[1024];
    char escaped_tbl[512];
    sql_escape_identifier(table_name, escaped_tbl, sizeof(escaped_tbl));
    snprintf(trigger_name, sizeof(trigger_name), "cloudsync_before_delete_%s", escaped_tbl);
    snprintf(func_name, sizeof(func_name), "cloudsync_before_delete_%s_fn", escaped_tbl);

    if (database_trigger_exists(data, trigger_name)) return DBRES_OK;

    char esc_tbl_literal[1024];
    sql_escape_literal(table_name, esc_tbl_literal, sizeof(esc_tbl_literal));

    char *sql = cloudsync_memory_mprintf(
        "CREATE OR REPLACE FUNCTION \"%s\"() RETURNS trigger AS $$ "
        "BEGIN "
        "  RAISE EXCEPTION 'Error: DELETE operation is not allowed on table %s.'; "
        "END; "
        "$$ LANGUAGE plpgsql;",
        func_name, esc_tbl_literal);
    if (!sql) return DBRES_NOMEM;

    int rc = database_exec(data, sql);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) return rc;

    char *base_ref = database_build_base_ref(schema, table_name);
    if (!base_ref) return DBRES_NOMEM;

    sql = cloudsync_memory_mprintf(
        "CREATE TRIGGER \"%s\" BEFORE DELETE ON %s "
        "FOR EACH ROW WHEN (cloudsync_is_enabled('%s') = true) "
        "EXECUTE FUNCTION \"%s\"();",
        trigger_name, base_ref, esc_tbl_literal, func_name);
    cloudsync_memory_free(base_ref);
    if (!sql) return DBRES_NOMEM;

    rc = database_exec(data, sql);
    cloudsync_memory_free(sql);
    return rc;
}

static int database_create_delete_trigger_internal (cloudsync_context *data, const char *table_name, const char *trigger_when, const char *schema) {
    if (!table_name) return DBRES_MISUSE;

    const char *schema_param = (schema && schema[0]) ? schema : "";

    char trigger_name[1024];
    char func_name[1024];
    char escaped_tbl[512];
    sql_escape_identifier(table_name, escaped_tbl, sizeof(escaped_tbl));
    snprintf(trigger_name, sizeof(trigger_name), "cloudsync_after_delete_%s", escaped_tbl);
    snprintf(func_name, sizeof(func_name), "cloudsync_after_delete_%s_fn", escaped_tbl);

    if (database_trigger_exists(data, trigger_name)) return DBRES_OK;

    char esc_tbl_literal[1024], esc_schema_literal[1024];
    sql_escape_literal(table_name, esc_tbl_literal, sizeof(esc_tbl_literal));
    sql_escape_literal(schema_param, esc_schema_literal, sizeof(esc_schema_literal));

    char sql[2048];
    snprintf(sql, sizeof(sql),
             "SELECT string_agg('OLD.' || quote_ident(kcu.column_name) || '::text', ',' ORDER BY kcu.ordinal_position) "
             "FROM information_schema.table_constraints tc "
             "JOIN information_schema.key_column_usage kcu "
             "  ON tc.constraint_name = kcu.constraint_name "
             "  AND tc.table_schema = kcu.table_schema "
             "WHERE tc.table_name = '%s' AND tc.table_schema = COALESCE(NULLIF('%s', ''), current_schema()) "
             "AND tc.constraint_type = 'PRIMARY KEY';",
             esc_tbl_literal, esc_schema_literal);

    char *pk_list = NULL;
    int rc = database_select_text(data, sql, &pk_list);
    if (rc != DBRES_OK) return rc;
    if (!pk_list || pk_list[0] == '\0') {
        if (pk_list) cloudsync_memory_free(pk_list);
        return cloudsync_set_error(data, "No primary key columns found for table", DBRES_ERROR);
    }

    char *sql2 = cloudsync_memory_mprintf(
        "CREATE OR REPLACE FUNCTION \"%s\"() RETURNS trigger AS $$ "
        "BEGIN "
        "  IF cloudsync_is_sync('%s') THEN RETURN OLD; END IF; "
        "  PERFORM cloudsync_delete('%s', %s); "
        "  RETURN OLD; "
        "END; "
        "$$ LANGUAGE plpgsql;",
        func_name, esc_tbl_literal, esc_tbl_literal, pk_list);
    cloudsync_memory_free(pk_list);
    if (!sql2) return DBRES_NOMEM;

    rc = database_exec(data, sql2);
    cloudsync_memory_free(sql2);
    if (rc != DBRES_OK) return rc;

    char *base_ref = database_build_base_ref(schema, table_name);
    if (!base_ref) return DBRES_NOMEM;

    sql2 = cloudsync_memory_mprintf(
        "CREATE TRIGGER \"%s\" AFTER DELETE ON %s %s "
        "EXECUTE FUNCTION \"%s\"();",
        trigger_name, base_ref, trigger_when ? trigger_when : "", func_name);
    cloudsync_memory_free(base_ref);
    if (!sql2) return DBRES_NOMEM;

    rc = database_exec(data, sql2);
    cloudsync_memory_free(sql2);
    return rc;
}

// Build trigger WHEN clauses, optionally incorporating a row-level filter.
// INSERT/UPDATE use NEW-prefixed filter, DELETE uses OLD-prefixed filter.
static void database_build_trigger_when(
    cloudsync_context *data, const char *table_name, const char *filter,
    const char *schema,
    char *when_new, size_t when_new_size,
    char *when_old, size_t when_old_size)
{
    char *new_filter_str = NULL;
    char *old_filter_str = NULL;

    if (filter) {
        const char *schema_param = (schema && schema[0]) ? schema : "";
        char esc_tbl[1024], esc_schema[1024];
        sql_escape_literal(table_name, esc_tbl, sizeof(esc_tbl));
        sql_escape_literal(schema_param, esc_schema, sizeof(esc_schema));

        char col_sql[2048];
        snprintf(col_sql, sizeof(col_sql),
            "SELECT column_name::text FROM information_schema.columns "
            "WHERE table_name = '%s' AND table_schema = COALESCE(NULLIF('%s', ''), current_schema()) "
            "ORDER BY ordinal_position;",
            esc_tbl, esc_schema);

        char *col_names[256];
        int ncols = 0;

        dbvm_t *col_vm = NULL;
        int crc = databasevm_prepare(data, col_sql, &col_vm, 0);
        if (crc == DBRES_OK) {
            while (databasevm_step(col_vm) == DBRES_ROW && ncols < 256) {
                const char *name = database_column_text(col_vm, 0);
                if (name) col_names[ncols++] = cloudsync_memory_mprintf("%s", name);
            }
            databasevm_finalize(col_vm);
        }

        if (ncols > 0) {
            new_filter_str = cloudsync_filter_add_row_prefix(filter, "NEW", col_names, ncols);
            old_filter_str = cloudsync_filter_add_row_prefix(filter, "OLD", col_names, ncols);
            for (int i = 0; i < ncols; ++i) cloudsync_memory_free(col_names[i]);
        }
    }

    char esc_tbl[512];
    sql_escape_literal(table_name, esc_tbl, sizeof(esc_tbl));

    if (new_filter_str) {
        snprintf(when_new, when_new_size,
            "FOR EACH ROW WHEN (cloudsync_is_sync('%s') = false AND (%s))",
            esc_tbl, new_filter_str);
    } else {
        snprintf(when_new, when_new_size,
            "FOR EACH ROW WHEN (cloudsync_is_sync('%s') = false)",
            esc_tbl);
    }

    if (old_filter_str) {
        snprintf(when_old, when_old_size,
            "FOR EACH ROW WHEN (cloudsync_is_sync('%s') = false AND (%s))",
            esc_tbl, old_filter_str);
    } else {
        snprintf(when_old, when_old_size,
            "FOR EACH ROW WHEN (cloudsync_is_sync('%s') = false)",
            esc_tbl);
    }

    if (new_filter_str) cloudsync_memory_free(new_filter_str);
    if (old_filter_str) cloudsync_memory_free(old_filter_str);
}

int database_create_triggers (cloudsync_context *data, const char *table_name, table_algo algo, const char *filter) {
    if (!table_name) return DBRES_MISUSE;

    // Detect schema from metadata table if it exists, otherwise use cloudsync_schema()
    // This is called before table_add_to_context(), so we can't rely on table lookup.
    char *detected_schema = database_table_schema(table_name);
    const char *schema = detected_schema ? detected_schema : cloudsync_schema(data);

    char trigger_when_new[4096];
    char trigger_when_old[4096];
    database_build_trigger_when(data, table_name, filter, schema,
        trigger_when_new, sizeof(trigger_when_new),
        trigger_when_old, sizeof(trigger_when_old));

    int rc = database_create_insert_trigger_internal(data, table_name, trigger_when_new, schema);
    if (rc != DBRES_OK) {
        if (detected_schema) cloudsync_memory_free(detected_schema);
        return rc;
    }

    if (algo == table_algo_crdt_gos) {
        rc = database_create_update_trigger_gos_internal(data, table_name, schema);
    } else {
        rc = database_create_update_trigger_internal(data, table_name, trigger_when_new, schema);
    }
    if (rc != DBRES_OK) {
        if (detected_schema) cloudsync_memory_free(detected_schema);
        return rc;
    }

    if (algo == table_algo_crdt_gos) {
        rc = database_create_delete_trigger_gos_internal(data, table_name, schema);
    } else {
        rc = database_create_delete_trigger_internal(data, table_name, trigger_when_old, schema);
    }

    if (detected_schema) cloudsync_memory_free(detected_schema);
    return rc;
}

int database_delete_triggers (cloudsync_context *data, const char *table) {
    char *base_ref = database_build_base_ref(cloudsync_schema(data), table);
    if (!base_ref) return DBRES_NOMEM;

    char escaped_tbl[512];
    sql_escape_identifier(table, escaped_tbl, sizeof(escaped_tbl));

    char *sql = cloudsync_memory_mprintf(
             "DROP TRIGGER IF EXISTS \"cloudsync_after_insert_%s\" ON %s;",
             escaped_tbl, base_ref);
    if (sql) { database_exec(data, sql); cloudsync_memory_free(sql); }

    sql = cloudsync_memory_mprintf(
             "DROP FUNCTION IF EXISTS \"cloudsync_after_insert_%s_fn\"() CASCADE;",
             escaped_tbl);
    if (sql) { database_exec(data, sql); cloudsync_memory_free(sql); }

    sql = cloudsync_memory_mprintf(
             "DROP TRIGGER IF EXISTS \"cloudsync_after_update_%s\" ON %s;",
             escaped_tbl, base_ref);
    if (sql) { database_exec(data, sql); cloudsync_memory_free(sql); }

    sql = cloudsync_memory_mprintf(
             "DROP TRIGGER IF EXISTS \"cloudsync_before_update_%s\" ON %s;",
             escaped_tbl, base_ref);
    if (sql) { database_exec(data, sql); cloudsync_memory_free(sql); }

    sql = cloudsync_memory_mprintf(
             "DROP FUNCTION IF EXISTS \"cloudsync_after_update_%s_fn\"() CASCADE;",
             escaped_tbl);
    if (sql) { database_exec(data, sql); cloudsync_memory_free(sql); }

    sql = cloudsync_memory_mprintf(
             "DROP FUNCTION IF EXISTS \"cloudsync_before_update_%s_fn\"() CASCADE;",
             escaped_tbl);
    if (sql) { database_exec(data, sql); cloudsync_memory_free(sql); }

    sql = cloudsync_memory_mprintf(
             "DROP TRIGGER IF EXISTS \"cloudsync_after_delete_%s\" ON %s;",
             escaped_tbl, base_ref);
    if (sql) { database_exec(data, sql); cloudsync_memory_free(sql); }

    sql = cloudsync_memory_mprintf(
             "DROP TRIGGER IF EXISTS \"cloudsync_before_delete_%s\" ON %s;",
             escaped_tbl, base_ref);
    if (sql) { database_exec(data, sql); cloudsync_memory_free(sql); }

    sql = cloudsync_memory_mprintf(
             "DROP FUNCTION IF EXISTS \"cloudsync_after_delete_%s_fn\"() CASCADE;",
             escaped_tbl);
    if (sql) { database_exec(data, sql); cloudsync_memory_free(sql); }

    sql = cloudsync_memory_mprintf(
             "DROP FUNCTION IF EXISTS \"cloudsync_before_delete_%s_fn\"() CASCADE;",
             escaped_tbl);
    if (sql) { database_exec(data, sql); cloudsync_memory_free(sql); }

    cloudsync_memory_free(base_ref);
    return DBRES_OK;
}

// MARK: - SCHEMA VERSIONING -

int64_t database_schema_version (cloudsync_context *data) {
    int64_t value = 0;
    int rc = database_select_int(data, SQL_SCHEMA_VERSION, &value);
    return (rc == DBRES_OK) ? value : 0;
}

uint64_t database_schema_hash (cloudsync_context *data) {
    int64_t value = 0;
    int rc = database_select_int(data, "SELECT hash FROM cloudsync_schema_versions ORDER BY seq DESC LIMIT 1;", &value);
    return (rc == DBRES_OK) ? (uint64_t)value : 0;
}

bool database_check_schema_hash (cloudsync_context *data, uint64_t hash) {
    char sql[1024];
    snprintf(sql, sizeof(sql), "SELECT 1 FROM cloudsync_schema_versions WHERE hash = %" PRId64, (int64_t)hash);

    int64_t value = 0;
    database_select_int(data, sql, &value);
    return (value == 1);
}

int database_update_schema_hash (cloudsync_context *data, uint64_t *hash) {
    // Build normalized schema string using only: column name (lowercase), type (SQLite affinity), pk flag
    // Format: tablename:colname:affinity:pk,... (ordered by table name, then column ordinal position)
    // This makes the hash resilient to formatting, quoting, case differences and portable across databases
    //
    // PostgreSQL type to SQLite affinity mapping:
    // - integer, smallint, bigint, boolean → 'integer'
    // - bytea → 'blob'
    // - real, double precision → 'real'
    // - numeric, decimal → 'numeric'
    // - Everything else → 'text' (default)
    //   This includes: text, varchar, char, uuid, timestamp, timestamptz, date, time,
    //   interval, json, jsonb, inet, cidr, macaddr, geometric types, xml, enums,
    //   and any custom/extension types. Using 'text' as default ensures compatibility
    //   since most types serialize to text representation and SQLite stores unknown
    //   types as TEXT affinity.

    char *schema = NULL;
    int rc = database_select_text(data,
        "SELECT string_agg("
        "    LOWER(c.table_name) || ':' || LOWER(c.column_name) || ':' || "
        "    CASE "
        // Integer types (including boolean as 0/1)
        "        WHEN c.data_type IN ('integer', 'smallint', 'bigint', 'boolean') THEN 'integer' "
        // Blob type
        "        WHEN c.data_type = 'bytea' THEN 'blob' "
        // Real/float types
        "        WHEN c.data_type IN ('real', 'double precision') THEN 'real' "
        // Numeric types (explicit precision/scale)
        "        WHEN c.data_type IN ('numeric', 'decimal') THEN 'numeric' "
        // Default to text for everything else:
        // - String types: text, character varying, character, name, uuid
        // - Date/time: timestamp, date, time, interval, etc.
        // - JSON: json, jsonb
        // - Network: inet, cidr, macaddr
        // - Geometric: point, line, box, etc.
        // - Custom/extension types
        "        ELSE 'text' "
        "    END || ':' || "
        "    CASE WHEN kcu.column_name IS NOT NULL THEN '1' ELSE '0' END, "
        "    ',' ORDER BY c.table_name, c.ordinal_position"
        ") "
        "FROM information_schema.columns c "
        "JOIN cloudsync_table_settings cts ON LOWER(c.table_name) = LOWER(cts.tbl_name) "
        "LEFT JOIN information_schema.table_constraints tc "
        "    ON tc.table_name = c.table_name "
        "    AND tc.table_schema = c.table_schema "
        "    AND tc.constraint_type = 'PRIMARY KEY' "
        "LEFT JOIN information_schema.key_column_usage kcu "
        "    ON kcu.table_name = c.table_name "
        "    AND kcu.column_name = c.column_name "
        "    AND kcu.table_schema = c.table_schema "
        "    AND kcu.constraint_name = tc.constraint_name "
        "WHERE c.table_schema = COALESCE(cloudsync_schema(), current_schema())",
        &schema);

    if (rc != DBRES_OK || !schema) return cloudsync_set_error(data, "database_update_schema_hash error 1", DBRES_ERROR);

    size_t schema_len = strlen(schema);
    DEBUG_MERGE("database_update_schema_hash len %zu schema %s", schema_len, schema);
    uint64_t h = fnv1a_hash(schema, schema_len);
    cloudsync_memory_free(schema);
    if (hash && *hash == h) return cloudsync_set_error(data, "database_update_schema_hash constraint", DBRES_CONSTRAINT);

    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO cloudsync_schema_versions (hash, seq) "
             "VALUES (%" PRId64 ", COALESCE((SELECT MAX(seq) FROM cloudsync_schema_versions), 0) + 1) "
             "ON CONFLICT(hash) DO UPDATE SET "
             "seq = (SELECT COALESCE(MAX(seq), 0) + 1 FROM cloudsync_schema_versions);",
             (int64_t)h);
    rc = database_exec(data, sql);
    if (rc == DBRES_OK) {
        if (hash) *hash = h;
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

    const char *sql =
            "SELECT kcu.column_name FROM information_schema.table_constraints tc "
            "JOIN information_schema.key_column_usage kcu ON tc.constraint_name = kcu.constraint_name "
            "  AND tc.table_schema = kcu.table_schema "
            "WHERE tc.table_name = $1 AND tc.table_schema = COALESCE(cloudsync_schema(), current_schema()) "
            "AND tc.constraint_type = 'PRIMARY KEY' "
            "ORDER BY kcu.ordinal_position";
    
    Oid argtypes[1] = { TEXTOID };
    Datum values[1] = { CStringGetTextDatum(table_name) };
    char nulls[1] = { ' ' };
    
    int rc = SPI_execute_with_args(sql, 1, argtypes, values, nulls, true, 0);
    pfree(DatumGetPointer(values[0]));
    
    if (rc != SPI_OK_SELECT || SPI_processed == 0) {
        *names = NULL;
        *count = 0;
        if (SPI_tuptable) SPI_freetuptable(SPI_tuptable);
        return DBRES_OK;
    }
    
    uint64_t n = SPI_processed;
    char **pk_names = cloudsync_memory_zeroalloc(n * sizeof(char*));
    if (!pk_names) {
        if (SPI_tuptable) SPI_freetuptable(SPI_tuptable);
        return DBRES_NOMEM;
    }
    
    for (uint64_t i = 0; i < n; i++) {
        HeapTuple tuple = SPI_tuptable->vals[i];
        bool isnull;
        SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull) {
            // SPI_getvalue returns a palloc'd string regardless of column type
            char *name = SPI_getvalue(tuple, SPI_tuptable->tupdesc, 1);
            pk_names[i] = (name) ? cloudsync_string_dup(name) : NULL;
            if (name) pfree(name);
        }
        
        // Cleanup on allocation failure
        if (!isnull && pk_names[i] == NULL) {
            for (uint64_t j = 0; j < i; j++) {
                if (pk_names[j]) cloudsync_memory_free(pk_names[j]);
            }
            cloudsync_memory_free(pk_names);
            if (SPI_tuptable) SPI_freetuptable(SPI_tuptable);
            return DBRES_NOMEM;
        }
    }
    
    *names = pk_names;
    *count = (int)n;
    if (SPI_tuptable) SPI_freetuptable(SPI_tuptable);
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
    MemoryContext oldcontext = CurrentMemoryContext;
    PG_TRY();
    {
        MemoryContext parent = (flags & DBFLAG_PERSISTENT) ? TopMemoryContext : CurrentMemoryContext;
        stmt->stmt_mcxt = AllocSetContextCreate(parent, "cloudsync stmt", ALLOCSET_DEFAULT_SIZES);
        if (!stmt->stmt_mcxt) {
            cloudsync_memory_free(stmt);
            ereport(ERROR, (errmsg("Failed to create statement memory context")));
        }
        stmt->bind_mcxt = AllocSetContextCreate(stmt->stmt_mcxt, "cloudsync binds", ALLOCSET_DEFAULT_SIZES);
        stmt->row_mcxt = AllocSetContextCreate(stmt->stmt_mcxt, "cloudsync row", ALLOCSET_DEFAULT_SIZES);

        MemoryContext old = MemoryContextSwitchTo(stmt->stmt_mcxt);
        stmt->sql = pstrdup(sql);
        MemoryContextSwitchTo(old);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(oldcontext);
        ErrorData *edata = CopyErrorData();
        rc = cloudsync_set_error(data, edata->message, DBRES_ERROR);
        FreeErrorData(edata);
        FlushErrorState();
        if (stmt->stmt_mcxt) MemoryContextDelete(stmt->stmt_mcxt);
        cloudsync_memory_free(stmt);
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
    if (!data) return DBRES_ERROR;

    int rc = DBRES_OK;
    MemoryContext oldcontext = CurrentMemoryContext;

    PG_TRY();
    {
        if (!stmt->sql) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                errmsg("databasevm_step0 invalid sql pointer")));
        }

        stmt->plan = SPI_prepare(stmt->sql, stmt->nparams, stmt->types);
        if (stmt->plan == NULL) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                errmsg("Unable to prepare SQL statement")));
        }

        SPI_keepplan(stmt->plan);
        stmt->plan_is_prepared = true;

        // Save the types used for this plan so we can detect type changes
        memcpy(stmt->prepared_types, stmt->types, sizeof(Oid) * stmt->nparams);
        stmt->prepared_nparams = stmt->nparams;
    }
    PG_CATCH();
    {
        // Switch to safe context for CopyErrorData (can't be ErrorContext)
        MemoryContextSwitchTo(oldcontext);
        ErrorData *edata = CopyErrorData();
        rc = cloudsync_set_error(data, edata->message, DBRES_ERROR);
        FreeErrorData(edata);
        FlushErrorState();

        // Clean up partially prepared plan if needed
        if (stmt->plan != NULL && !stmt->plan_is_prepared) {
            PG_TRY();
            {
                SPI_freeplan(stmt->plan);
            }
            PG_CATCH();
            {
                FlushErrorState();  // Swallow errors during cleanup
            }
            PG_END_TRY();
            stmt->plan = NULL;
        }
    }
    PG_END_TRY();

    return rc;
}

int databasevm_step (dbvm_t *vm) {
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    if (!stmt) return DBRES_MISUSE;
    
    cloudsync_context *data = stmt->data;
    cloudsync_reset_error(data);
    
    // If plan is prepared but parameter types have changed since preparation,
    // free the old plan and re-prepare with new types. This happens when the same
    // prepared statement is reused with different PK encodings (e.g., integer vs text).
    if (stmt->plan_is_prepared && stmt->plan) {
        bool types_changed = (stmt->nparams != stmt->prepared_nparams);
        if (!types_changed) {
            for (int i = 0; i < stmt->nparams; i++) {
                if (stmt->types[i] != stmt->prepared_types[i]) {
                    types_changed = true;
                    break;
                }
            }
        }
        if (types_changed) {
            SPI_freeplan(stmt->plan);
            stmt->plan = NULL;
            stmt->plan_is_prepared = false;
        }
    }

    if (!stmt->plan_is_prepared) {
        int rc = databasevm_step0(stmt);
        if (rc != DBRES_OK) return rc;
    }
    if (!stmt->plan_is_prepared || !stmt->plan) return DBRES_ERROR;

    int rc = DBRES_DONE;
    MemoryContext oldcontext = CurrentMemoryContext;
    PG_TRY();
    {
        do {
            // if portal is open, we fetch one row
            if (stmt->portal_open) {
                // free prior fetched row batch
                clear_fetch_batch(stmt);
                
                SPI_cursor_fetch(stmt->portal, true, 1);
                
                if (SPI_processed == 0) {
                    clear_fetch_batch(stmt);
                    close_portal(stmt);
                    rc = DBRES_DONE;
                    break;
                }
                
                // null check for SPI_tuptable
                if (!SPI_tuptable || !SPI_tuptable->tupdesc || !SPI_tuptable->vals) {
                    clear_fetch_batch(stmt);
                    close_portal(stmt);
                    rc = cloudsync_set_error(data, "SPI_cursor_fetch returned invalid tuptable", DBRES_ERROR);
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
                        // Don't set portal_open until we successfully fetch first row
                        
                        // fetch first row
                        clear_fetch_batch(stmt);
                        SPI_cursor_fetch(stmt->portal, true, 1);
                        
                        if (SPI_processed == 0) {
                            // No rows - close portal, don't set portal_open
                            clear_fetch_batch(stmt);
                            close_portal(stmt);
                            rc = DBRES_DONE;
                            break;
                        }
                        
                        // null check for SPI_tuptable
                        if (!SPI_tuptable || !SPI_tuptable->tupdesc || !SPI_tuptable->vals) {
                            clear_fetch_batch(stmt);
                            close_portal(stmt);
                            rc = cloudsync_set_error(data, "SPI_cursor_fetch returned invalid tuptable", DBRES_ERROR);
                            break;
                        }
                        
                        MemoryContextReset(stmt->row_mcxt);
                        
                        stmt->last_tuptable = SPI_tuptable;
                        stmt->current_tupdesc = stmt->last_tuptable->tupdesc;
                        stmt->current_tuple = stmt->last_tuptable->vals[0];
                        
                        // Only set portal_open AFTER everything succeeded
                        stmt->portal_open = true;
                        
                        rc = DBRES_ROW;
                        break;
                    }
                }

                // Execute once (non-row-returning or cursor open failed).
                int spi_rc;
                if (stmt->nparams == 0) spi_rc = SPI_execute_plan(stmt->plan, NULL, NULL, false, 0);
                else spi_rc = SPI_execute_plan(stmt->plan, stmt->values, stmt->nulls, false, 0);
                if (spi_rc < 0) {
                    rc = cloudsync_set_error(data, "SPI_execute_plan failed", DBRES_ERROR);
                    break;
                }
                if (SPI_tuptable) {
                    SPI_freetuptable(SPI_tuptable);
                    SPI_tuptable = NULL;
                }

                stmt->executed_nonselect = true;
                rc = DBRES_DONE;
                break;
            }
            
            rc = DBRES_DONE;
        } while (0);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(oldcontext);
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
        if (SPI_tuptable) {
            SPI_freetuptable(SPI_tuptable);
            SPI_tuptable = NULL;
        }
        
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
    pg_stmt_t *stmt = (pg_stmt_t*)vm;

    // Close any open cursor and clear fetched data
    clear_fetch_batch(stmt);
    close_portal(stmt);

    // Clear global SPI tuple table if any
    if (SPI_tuptable) {
        SPI_freetuptable(SPI_tuptable);
        SPI_tuptable = NULL;
    }

    // Reset execution state
    stmt->executed_nonselect = false;

    // Reset parameter values but keep the plan, types, and nparams intact.
    // The prepared plan can be reused with new values of the same types,
    // avoiding the cost of re-planning on every iteration.
    if (stmt->bind_mcxt) MemoryContextReset(stmt->bind_mcxt);
    for (int i = 0; i < stmt->nparams; i++) {
        stmt->values[i] = (Datum) 0;
        stmt->nulls[i] = 'n';
    }
}

void databasevm_clear_bindings (dbvm_t *vm) {
    if (!vm) return;
    pg_stmt_t *stmt = (pg_stmt_t*)vm;

    // Only clear the bound parameter values.
    // Do NOT close portals, free fetch batches, or free the plan —
    // those are execution state, not bindings.
    if (stmt->bind_mcxt) MemoryContextReset(stmt->bind_mcxt);
    stmt->nparams = 0;

    // Reset params array to defaults
    for (int i = 0; i < MAX_PARAMS; i++) {
        stmt->types[i] = UNKNOWNOID;
        stmt->values[i] = (Datum) 0;
        stmt->nulls[i] = 'n';   // default NULL
    }
}

const char *databasevm_sql (dbvm_t *vm) {
    if (!vm) return NULL;

    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    return (char *)stmt->sql;
}

// MARK: - BINDING -

static int databasevm_bind_null_type (dbvm_t *vm, int index, Oid t) {
    int rc = databasevm_bind_null(vm, index);
    if (rc != DBRES_OK) return rc;
    int idx = index - 1;

    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    stmt->types[idx] = t;
    return rc;
}

int databasevm_bind_blob (dbvm_t *vm, int index, const void *value, uint64_t size) {
    if (!vm || index < 1) return DBRES_ERROR;
    if (!value) return databasevm_bind_null_type(vm, index, BYTEAOID);
    
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
    stmt->types[idx] = TEXTOID;  // TEXTOID has casts to most types
    stmt->nulls[idx] = 'n';
    
    if (stmt->nparams < idx + 1) stmt->nparams = idx + 1;
    return DBRES_OK;
}

int databasevm_bind_text (dbvm_t *vm, int index, const char *value, int size) {
    if (!vm || index < 1) return DBRES_ERROR;
    if (!value) return databasevm_bind_null_type(vm, index, TEXTOID);
    
    // validate size fits Size and won't overflow
    if (size < 0) size = (int)strlen(value);
    if ((Size)size > MaxAllocSize - VARHDRSZ) return DBRES_NOMEM;
    
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
    if (!value) return databasevm_bind_null_type(vm, index, TEXTOID);

    // validate index bounds properly (1-based index)
    if (index < 1) return DBRES_ERROR;
    int idx = index - 1;
    if (idx >= MAX_PARAMS) return DBRES_ERROR;
    
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    pgvalue_t *v = (pgvalue_t *)value;
    if (!v || v->isnull) {
        stmt->values[idx] = (Datum)0;
        // Use the actual column type if available, otherwise default to TEXTOID
        stmt->types[idx] = (v && OidIsValid(v->typeid)) ? v->typeid : TEXTOID;
        stmt->nulls[idx] = 'n';
    } else {
        int16 typlen;
        bool typbyval;
        
        get_typlenbyval(v->typeid, &typlen, &typbyval);
        MemoryContext old = MemoryContextSwitchTo(stmt->bind_mcxt);
        
        Datum dcopy;
        if (typbyval) {
            // Pass-by-value: direct copy is safe
            dcopy = v->datum;
        } else {
            // Pass-by-reference: need to copy the actual data
            // Handle variable-length types (typlen == -1) and cstrings (typlen == -2)
            if (typlen == -1) {
                // Variable-length type (varlena): use VARSIZE_ANY to handle
                // both short (1-byte) and regular (4-byte) varlena headers
                Size len = VARSIZE_ANY(DatumGetPointer(v->datum));
                dcopy = PointerGetDatum(palloc(len));
                memcpy(DatumGetPointer(dcopy), DatumGetPointer(v->datum), len);
            } else if (typlen == -2) {
                // Null-terminated cstring
                dcopy = CStringGetDatum(pstrdup(DatumGetCString(v->datum)));
            } else {
                // Fixed-length pass-by-reference
                dcopy = datumCopy(v->datum, false, typlen);
            }
        }
        
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
    if (!vm) return (Datum)0;
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    if (!stmt->last_tuptable || !stmt->current_tupdesc) return (Datum)0;
    if (index < 0 || index >= stmt->current_tupdesc->natts) return (Datum)0;
    
    bool isnull = true;
    Datum d = get_datum(stmt, index, &isnull, NULL);
    return (isnull) ? (Datum)0 : d;
}

const void *database_column_blob (dbvm_t *vm, int index, size_t *len) {
    if (!vm) return NULL;
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    if (!stmt->last_tuptable || !stmt->current_tupdesc) return NULL;
    if (index < 0 || index >= stmt->current_tupdesc->natts) return NULL;

    bool isnull = true;
    Datum d = get_datum(stmt, index, &isnull, NULL);
    if (isnull) return NULL;
    
    MemoryContext old = MemoryContextSwitchTo(stmt->row_mcxt);
    bytea *ba = DatumGetByteaP(d);
    
    // Validate VARSIZE before computing length
    Size varsize = VARSIZE(ba);
    if (varsize < VARHDRSZ) {
        // Corrupt or invalid bytea - VARSIZE should always be >= VARHDRSZ
        MemoryContextSwitchTo(old);
        elog(WARNING, "database_column_blob: invalid bytea VARSIZE %zu", varsize);
        return NULL;
    }
    
    Size blen = VARSIZE(ba) - VARHDRSZ;
    void *out = palloc(blen);
    if (!out) {
        MemoryContextSwitchTo(old);
        return NULL;
    }
    
    memcpy(out, VARDATA(ba), (size_t)blen);
    MemoryContextSwitchTo(old);
    
    if (len) *len = (size_t)blen;
    return out;
}

double database_column_double (dbvm_t *vm, int index) {
    if (!vm) return 0.0;
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    if (!stmt->last_tuptable || !stmt->current_tupdesc) return 0.0;
    if (index < 0 || index >= stmt->current_tupdesc->natts) return 0.0;

    bool isnull = true;
    Oid type = 0;
    Datum d = get_datum(stmt, index, &isnull, &type);
    if (isnull) return 0.0;
    
    switch (type) {
        case FLOAT4OID: return (double)DatumGetFloat4(d);
        case FLOAT8OID: return (double)DatumGetFloat8(d);
        case NUMERICOID: return DatumGetFloat8(DirectFunctionCall1(numeric_float8_no_overflow, d));
        case INT2OID: return (double)DatumGetInt16(d);
        case INT4OID: return (double)DatumGetInt32(d);
        case INT8OID: return (double)DatumGetInt64(d);
        case BOOLOID: return (double)DatumGetBool(d);
    }

    return 0.0;
}

int64_t database_column_int (dbvm_t *vm, int index) {
    if (!vm) return 0;
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
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
    if (!vm) return NULL;
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
    if (!stmt->last_tuptable || !stmt->current_tupdesc) return NULL;
    if (index < 0 || index >= stmt->current_tupdesc->natts) return NULL;

    bool isnull = true;
    Oid type = 0;
    Datum d = get_datum(stmt, index, &isnull, &type);
    if (isnull) return NULL;
    
    MemoryContext old = MemoryContextSwitchTo(stmt->row_mcxt);
    char *out = NULL;

    if (type == BYTEAOID) {
        bytea *b = DatumGetByteaP(d);
        int len = VARSIZE(b) - VARHDRSZ;
        out = palloc(len + 1);
        memcpy(out, VARDATA(b), len);
        out[len] = 0;
    } else if (type == TEXTOID || type == VARCHAROID || type == BPCHAROID) {
        text *t = DatumGetTextP(d);
        int len = VARSIZE(t) - VARHDRSZ;
        out = palloc(len + 1);
        memcpy(out, VARDATA(t), len);
        out[len] = 0;
    } else {
        MemoryContextSwitchTo(old);
        return NULL;
    }

    MemoryContextSwitchTo(old);
    
    return out;
}

dbvalue_t *database_column_value (dbvm_t *vm, int index) {
    if (!vm) return NULL;
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
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
    if (!vm) return 0;
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
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
    if (!vm) return DBTYPE_NULL;
    pg_stmt_t *stmt = (pg_stmt_t*)vm;
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

    // Text types reuse blob accessor (pk encode reads text bytes directly).
    // Exclude JSONB: its internal format is binary, not text —
    // it must go through OidOutputFunctionCall to get the JSON text.
    if (pgvalue_is_text_type(v->typeid) && v->typeid != JSONBOID) {
        pgvalue_ensure_detoast(v);
        text *txt = (text *)DatumGetPointer(v->datum);
        return VARDATA_ANY(txt);
    }

    if (v->typeid == BYTEAOID) {
        pgvalue_ensure_detoast(v);
        bytea *ba = (bytea *)DatumGetPointer(v->datum);
        return VARDATA_ANY(ba);
    }

    // For unmapped types and JSONB (mapped to DBTYPE_TEXT),
    // convert to text representation via the type's output function
    const char *cstr = database_value_text(value);
    if (cstr) return cstr;

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
    if (!v || v->isnull) return NULL;

    if (!v->cstring && !v->owns_cstring) {
        PG_TRY();
        {
            if (pgvalue_is_text_type(v->typeid) && v->typeid != JSONBOID) {
                pgvalue_ensure_detoast(v);
                v->cstring = text_to_cstring((text *)DatumGetPointer(v->datum));
            } else {
                // Type output function for JSONB and other non-text types
                Oid outfunc;
                bool isvarlena;
                getTypeOutputInfo(v->typeid, &outfunc, &isvarlena);
                v->cstring = OidOutputFunctionCall(outfunc, v->datum);
            }
            v->owns_cstring = true;
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(CurrentMemoryContext);
            ErrorData *edata = CopyErrorData();
            elog(WARNING, "database_value_text: conversion failed for type %u: %s", v->typeid, edata->message);
            FreeErrorData(edata);
            FlushErrorState();
            v->cstring = NULL;
            v->owns_cstring = true;  // prevents retry of failed conversion
        }
        PG_END_TRY();
    }

    return v->cstring;
}

int database_value_bytes (dbvalue_t *value) {
    pgvalue_t *v = (pgvalue_t *)value;
    if (!v || v->isnull) return 0;

    // Exclude JSONB: binary internal format, must use OidOutputFunctionCall
    if (pgvalue_is_text_type(v->typeid) && v->typeid != JSONBOID) {
        pgvalue_ensure_detoast(v);
        text *txt = (text *)DatumGetPointer(v->datum);
        return VARSIZE_ANY_EXHDR(txt);
    }
    if (v->typeid == BYTEAOID) {
        pgvalue_ensure_detoast(v);
        bytea *ba = (bytea *)DatumGetPointer(v->datum);
        return VARSIZE_ANY_EXHDR(ba);
    }
    // For unmapped types and JSONB (mapped to DBTYPE_TEXT),
    // ensure the text representation is materialized
    database_value_text(value);
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

    // Deep-copy pass-by-reference (varlena) datum data into TopMemoryContext
    // so the copy survives SPI_finish() which destroys the caller's SPI context.
    bool is_varlena = (v->typeid == BYTEAOID) || pgvalue_is_text_type(v->typeid);
    if (is_varlena && !v->isnull) {
        void *src = v->owned_detoast ? v->owned_detoast : DatumGetPointer(v->datum);
        Size len = VARSIZE_ANY(src);
        MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
        copy->owned_detoast = palloc(len);
        MemoryContextSwitchTo(old);
        memcpy(copy->owned_detoast, src, len);
        copy->datum = PointerGetDatum(copy->owned_detoast);
        copy->detoasted = true;
    }
    if (v->cstring) {
        MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
        copy->cstring = pstrdup(v->cstring);
        MemoryContextSwitchTo(old);
        copy->owns_cstring = true;
    }
    return (void*)copy;
}

// MARK: - SAVEPOINTS -

static int database_refresh_snapshot (void) {
    // Only manipulate snapshots in a valid transaction
    if (!IsTransactionState()) {
        return DBRES_OK;  // Not in transaction, nothing to do
    }

    MemoryContext oldcontext = CurrentMemoryContext;
    PG_TRY();
    {
        CommandCounterIncrement();

        // Pop existing snapshot if any
        if (ActiveSnapshotSet()) {
            PopActiveSnapshot();
        }

        // Push fresh snapshot
        PushActiveSnapshot(GetTransactionSnapshot());
    }
    PG_CATCH();
    {
        // Snapshot refresh failed - log warning but don't fail operation
        MemoryContextSwitchTo(oldcontext);
        ErrorData *edata = CopyErrorData();
        elog(WARNING, "refresh_snapshot_after_command failed: %s", edata->message);
        FreeErrorData(edata);
        FlushErrorState();
        return DBRES_ERROR;
    }
    PG_END_TRY();

    return DBRES_OK;
}

int database_begin_savepoint (cloudsync_context *data, const char *savepoint_name) {
    cloudsync_reset_error(data);
    int rc = DBRES_OK;

    MemoryContext oldcontext = CurrentMemoryContext;
    PG_TRY();
    {
        BeginInternalSubTransaction(NULL);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(oldcontext);
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
    if (GetCurrentTransactionNestLevel() <= 1) return DBRES_OK;
    int rc = DBRES_OK;

    MemoryContext oldcontext = CurrentMemoryContext;
    PG_TRY();
    {
        ReleaseCurrentSubTransaction();
        database_refresh_snapshot();
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(oldcontext);
        ErrorData *edata = CopyErrorData();
        cloudsync_set_error(data, edata->message, DBRES_ERROR);
        FreeErrorData(edata);
        FlushErrorState();
        rc = DBRES_ERROR;
    }
    PG_END_TRY();

    return rc;
}

int database_rollback_savepoint (cloudsync_context *data, const char *savepoint_name) {
    cloudsync_reset_error(data);
    if (GetCurrentTransactionNestLevel() <= 1) return DBRES_OK;
    int rc = DBRES_OK;

    MemoryContext oldcontext = CurrentMemoryContext;
    PG_TRY();
    {
        RollbackAndReleaseCurrentSubTransaction();
        database_refresh_snapshot();
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(oldcontext);
        ErrorData *edata = CopyErrorData();
        cloudsync_set_error(data, edata->message, DBRES_ERROR);
        FreeErrorData(edata);
        FlushErrorState();
        rc = DBRES_ERROR;
    }
    PG_END_TRY();
    
    return rc;
}

// MARK: - MEMORY -
// Use palloc in TopMemoryContext for PostgreSQL memory management integration.
// This provides memory tracking, debugging support, and proper cleanup on connection end.

void *dbmem_alloc (uint64_t size) {
    MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
    void *ptr = palloc(size);
    MemoryContextSwitchTo(old);
    return ptr;
}

void *dbmem_zeroalloc (uint64_t size) {
    MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
    void *ptr = palloc0(size);
    MemoryContextSwitchTo(old);
    return ptr;
}

void *dbmem_realloc (void *ptr, uint64_t new_size) {
    // repalloc doesn't accept NULL, unlike realloc
    if (!ptr) return dbmem_alloc(new_size);
    MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
    void *newptr = repalloc(ptr, new_size);
    MemoryContextSwitchTo(old);
    return newptr;
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

    // Allocate buffer in TopMemoryContext and format string
    char *result = dbmem_alloc(len + 1);
    if (!result) {va_end(args); return NULL;}
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

    // Allocate buffer in TopMemoryContext and format string
    char *result = dbmem_alloc(len + 1);
    if (!result) return NULL;
    vsnprintf(result, len + 1, format, list);

    return result;
}

void dbmem_free (void *ptr) {
    if (ptr) {
        pfree(ptr);
    }
}

uint64_t dbmem_size (void *ptr) {
    // palloc doesn't expose allocated size directly
    // Return 0 as a safe default
    return 0;
}


