//
//  vtab.c
//  cloudsync
//
//  Created by Marco Bambini on 23/09/24.
//

#include <stdio.h>
#include <string.h>
#include "vtab.h"
#include "utils.h"
#include "dbutils.h"
#include "cloudsync.h"

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT3
#endif

typedef struct cloudsync_changes_vtab {
    sqlite3_vtab            base;       // base class, must be first
    sqlite3                 *db;
    void                    *aux;
} cloudsync_changes_vtab;

typedef struct cloudsync_changes_cursor {
    sqlite3_vtab_cursor     base;       // base class, must be first
    cloudsync_changes_vtab  *vtab;
    sqlite3_stmt            *vm;        // prepared statement
} cloudsync_changes_cursor;

char *cloudsync_changes_columns[] = {"tbl", "pk", "col_name", "col_value", "col_version", "db_version", "site_id", "cl", "seq"};
#define COLNAME_FROM_INDEX(i)       cloudsync_changes_columns[i]
#define COL_TBL_INDEX               0
#define COL_PK_INDEX                1
#define COL_NAME_INDEX              2
#define COL_VALUE_INDEX             3
#define COL_VERSION_INDEX           4
#define COL_DBVERSION_INDEX         5
#define COL_SITEID_INDEX            6
#define COL_CL_INDEX                7
#define COL_SEQ_INDEX               8

#if CLOUDSYNC_UNITTEST
bool force_vtab_filter_abort = false;
#define CHECK_VFILTERTEST_ABORT()   if (force_vtab_filter_abort) rc = SQLITE_ERROR
#else
#define CHECK_VFILTERTEST_ABORT()
#endif

// MARK: -

const char *opname_from_value (int value) {
    switch (value) {
        case SQLITE_INDEX_CONSTRAINT_EQ: return "=";
        case SQLITE_INDEX_CONSTRAINT_GT: return ">";
        case SQLITE_INDEX_CONSTRAINT_LE: return "<=";
        case SQLITE_INDEX_CONSTRAINT_LT: return "<";
        case SQLITE_INDEX_CONSTRAINT_GE: return ">=";
        case SQLITE_INDEX_CONSTRAINT_LIKE: return "LIKE";
        case SQLITE_INDEX_CONSTRAINT_GLOB: return "GLOB";
        case SQLITE_INDEX_CONSTRAINT_NE: return "!=";
        case SQLITE_INDEX_CONSTRAINT_ISNOT: return "IS NOT";
        case SQLITE_INDEX_CONSTRAINT_ISNOTNULL: return "IS NOT NULL";
        case SQLITE_INDEX_CONSTRAINT_ISNULL: return "IS NULL";
        case SQLITE_INDEX_CONSTRAINT_IS: return "IS";
        
        // The REGEXP operator is a special syntax for the regexp() user function.
        // No regexp() user function is defined by default and so use of the REGEXP operator will normally result in an error message.
        // If an application-defined SQL function named "regexp" is added at run-time, then the "X REGEXP Y" operator will be implemented as
        // a call to "regexp(Y,X)".
        // case SQLITE_INDEX_CONSTRAINT_REGEXP: return "REGEX";
        
        // MATCH is only valid for virtual FTS tables
        // The MATCH operator is a special syntax for the match() application-defined function.
        // The default match() function implementation raises an exception and is not really useful for anything.
        // But extensions can override the match() function with more helpful logic.
        // case SQLITE_INDEX_CONSTRAINT_MATCH: return "MATCH";
    }
    return NULL;
}

int colname_is_legal (const char *name) {
    int count = sizeof(cloudsync_changes_columns) / sizeof (char *);
    
    for (int i=0; i<count; ++i) {
        if (strcasecmp(cloudsync_changes_columns[i], name) == 0) return 1;
    }
    return 0;
}

char *build_changes_sql (sqlite3 *db, const char *idxs) {
    DEBUG_VTAB("build_changes_sql");
    
    /*
     * This SQLite query dynamically generates and executes a consolidated query
     * to fetch changes from all tables related to cloud synchronization.
     *
     * It works in the following steps:
     *
     * 1. `table_names` CTE: Retrieves all table names in the database that end
     *    with '_cloudsync' and extracts the base table name (without the '_cloudsync' suffix).
     *
     * 2. `changes_query` CTE: Constructs individual SELECT statements for each
     *    cloud sync table, fetching data about changes in columns:
     *      - `pk`: Primary key of the table.
     *      - `col_name`: Name of the changed column.
     *      - `col_version`: Version of the changed column.
     *      - `db_version`: Database version when the change was recorded.
     *      - `site_id`: Site identifier associated with the change.
     *      - `cl`: Coalesced version (either `t2.col_version` or 1 if `t2.col_version` is NULL).
     *      - `seq`: Sequence number of the change.
     *    Each query is constructed by joining the table with `cloudsync_site_id`
     *    for resolving the site ID and performing a LEFT JOIN with itself to
     *    identify columns with NULL values in `t2.col_name`.
     *
     * 3. `union_query` CTE: Combines all the constructed SELECT statements
     *    from `changes_query` into a single query using `UNION ALL`.
     *
     * 4. `final_query` CTE: Wraps the combined UNION ALL query into another
     *    SELECT statement, preparing for additional filtering.
     *
     * 5. Final SELECT: Adds a WHERE clause to filter records with `db_version`
     *    greater than a specified value (using a placeholder `?`), and orders
     *    the results by `db_version` and `seq`.
     *
     * The overall result is a consolidated view of changes across all
     * cloud sync tables, filtered and ordered based on the `db_version` and
     * `seq` fields.
     */

    const char *query =
    "WITH table_names AS ( "
    "    SELECT SUBSTR(tbl_name, 1, LENGTH(tbl_name) - 10) AS table_name, tbl_name AS table_meta "
    "    FROM sqlite_master "
    "    WHERE type = 'table' AND tbl_name LIKE '%_cloudsync' "
    "), "
    "changes_query AS ( "
    "    SELECT "
    "        'SELECT "
    "        \"' || \"table_name\" || '\" AS tbl, "
    "        t1.pk AS pk, "
    "        t1.col_name AS col_name, "
    "        cloudsync_col_value(\"' || \"table_name\" || '\", t1.col_name, t1.pk) AS col_value, "
    "        t1.col_version AS col_version, "
    "        t1.db_version AS db_version, "
    "        site_tbl.site_id AS site_id, "
    "        t1.seq AS seq, "
    "        COALESCE(t2.col_version, 1) AS cl "
    "     FROM \"' || \"table_meta\" || '\" AS t1 "
    "     LEFT JOIN cloudsync_site_id AS site_tbl ON t1.site_id = site_tbl.rowid "
    "     LEFT JOIN \"' || \"table_meta\" || '\" AS t2 ON t1.pk = t2.pk AND t2.col_name = ''" CLOUDSYNC_TOMBSTONE_VALUE "''' "
    "    AS query_string FROM table_names "
    "), "
    "union_query AS ( "
    "    SELECT GROUP_CONCAT(query_string, ' UNION ALL ') AS union_query FROM changes_query "
    "), "
    "final_query AS ( "
    "    SELECT "
    "        'SELECT tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq FROM (' || union_query || ')' "
    "    AS final_string FROM union_query "
    ") "
    "SELECT concat(final_string, ' ";
    const char *final_query = ";') FROM final_query;";
    
    static size_t query_len = 0;
    static size_t final_query_len = 0;
    if (query_len == 0) query_len = strlen(query);
    if (final_query_len == 0) final_query_len = strlen(final_query);
    
    size_t idx_len = strlen(idxs);
    size_t blen = query_len + idx_len + final_query_len + 128;
    char *sql = (char *)cloudsync_memory_alloc((sqlite3_uint64)blen);
    if (!sql) return NULL;
    
    // build final sql statement taking in account the dynamic idxs string provided by the user
    memcpy(sql, query, query_len);
    memcpy(sql + (query_len), idxs, idx_len);
    memcpy(sql + (query_len + idx_len), final_query, final_query_len+1);
        
    char *value = dbutils_text_select(db, sql);
    cloudsync_memory_free(sql);
    
    return value;
}

// MARK: -

int cloudsync_changesvtab_connect (sqlite3 *db, void *aux, int argc, const char *const *argv, sqlite3_vtab **vtab, char **err) {
    DEBUG_VTAB("cloudsync_changesvtab_connect");
    
    int rc = sqlite3_declare_vtab(db, "CREATE TABLE x (tbl TEXT NOT NULL, pk BLOB NOT NULL, col_name TEXT NOT NULL,"
                                  "col_value TEXT, col_version INTEGER NOT NULL, db_version INTEGER NOT NULL,"
                                  "site_id BLOB NOT NULL, cl INTEGER NOT NULL, seq INTEGER NOT NULL);");
    if (rc == SQLITE_OK) {
        // memory internally managed by SQLite, so I cannot use memory_alloc here
        cloudsync_changes_vtab *vnew = sqlite3_malloc64(sizeof(cloudsync_changes_vtab));
        if (vnew == NULL) return SQLITE_NOMEM;
        
        memset(vnew, 0, sizeof(cloudsync_changes_vtab));
        vnew->db = db;
        vnew->aux = aux;
        
        *vtab = (sqlite3_vtab *)vnew;
    }
    
    return rc;
}

int cloudsync_changesvtab_disconnect (sqlite3_vtab *vtab) {
    DEBUG_VTAB("cloudsync_changesvtab_disconnect");
    
    cloudsync_changes_vtab *p = (cloudsync_changes_vtab *)vtab;
    sqlite3_free(p);
    return SQLITE_OK;
}

int cloudsync_changesvtab_open (sqlite3_vtab *vtab, sqlite3_vtab_cursor **pcursor) {
    DEBUG_VTAB("cloudsync_changesvtab_open");
    
    cloudsync_changes_cursor *cursor = cloudsync_memory_alloc(sizeof(cloudsync_changes_cursor));
    if (cursor == NULL) return SQLITE_NOMEM;
    
    memset(cursor, 0, sizeof(cloudsync_changes_cursor));
    cursor->vtab = (cloudsync_changes_vtab *)vtab;
    
    *pcursor = (sqlite3_vtab_cursor *)cursor;
    return SQLITE_OK;
}

int cloudsync_changesvtab_close (sqlite3_vtab_cursor *cursor) {
    DEBUG_VTAB("cloudsync_changesvtab_close");
    
    cloudsync_changes_cursor *c = (cloudsync_changes_cursor *)cursor;
    if (c->vm) {
        sqlite3_finalize(c->vm);
        c->vm = NULL;
    }
    
    cloudsync_memory_free(cursor);
    return SQLITE_OK;
}

int cloudsync_changesvtab_best_index (sqlite3_vtab *vtab, sqlite3_index_info *idxinfo) {
    DEBUG_VTAB("cloudsync_changesvtab_best_index");
    
    // the goal here is to build a WHERE clause and gives an estimate
    // of the cost of that clause
    
    // we'll incrementally build the clause and we avoid the realloc
    // of memory, so perform a quick loop to estimate memory usage
    
    // count the number of contrainst and order by clauses
    int count1 = idxinfo->nConstraint;
    int count2 = idxinfo->nOrderBy;
    
    // col_version is the longest column name (11)
    // +1 for the space
    // IS NOT NULL is the longest constraint value (11)
    // +1 for the space
    // +1 for the ? character
    // +5 for space AND space
    // +512 for the extra space and for the WHERE and ORDER BY literals
    
    // memory internally manager by SQLite, so I cannot use memory_alloc here
    size_t slen = (count1 * (11 + 1 + 11 + 1 + 5)) + (count2 * 11 + 1 + 5) + 512;
    char *s = (char *)sqlite3_malloc64((sqlite3_uint64)slen);
    if (!s) return SQLITE_NOMEM;
    size_t sindex= 0;

    int idxnum = 0;
    int arg_index = 1;
    int orderconsumed = 1;
    
    // is there a WHERE clause ?
    if (count1 > 0) sindex += snprintf(s+sindex, slen-sindex, "WHERE ");
    
    // check constraints
    for (int i=0; i < count1; ++i) {
        // analyze only usable constraints
        struct sqlite3_index_constraint *constraint = &idxinfo->aConstraint[i];
        if (constraint->usable == false) continue;
        
        int idx = constraint->iColumn;
        uint8_t op = constraint->op;
        
        const char *colname = (idx > 0) ? COLNAME_FROM_INDEX(idx) : "rowid";
        const char *opname = opname_from_value(op);
        if (!opname) continue;
        
        // build next constraint
        if (i > 0) sindex += snprintf(s+sindex, slen-sindex, " AND ");
        
        // handle special case where value is not needed
        if ((op == SQLITE_INDEX_CONSTRAINT_ISNULL) || (op == SQLITE_INDEX_CONSTRAINT_ISNOTNULL)) {
            sindex += snprintf(s+sindex, slen-sindex, "%s %s", colname, opname);
            idxinfo->aConstraintUsage[i].argvIndex = 0;
        } else {
            sindex += snprintf(s+sindex, slen-sindex, "%s %s ?", colname, opname);
            idxinfo->aConstraintUsage[i].argvIndex = arg_index++;
        }
        idxinfo->aConstraintUsage[i].omit = 1;
        
        //a bitmask (idxnum) is built up based on which constraints are applied
        if (idx == COL_DBVERSION_INDEX) idxnum |= 2;    // set bit 1
        else if (idx == COL_SITEID_INDEX) idxnum |= 4;  // set bit 2
    }
    
    // is there an ORDER BY clause ?
    // if not use a default one
    if (count2 > 0) sindex += snprintf(s+sindex, slen-sindex, " ORDER BY ");
    else sindex += snprintf(s+sindex, slen-sindex, " ORDER BY db_version, seq ASC");
    
    for (int i=0; i < count2; ++i) {
        struct sqlite3_index_orderby *orderby = &idxinfo->aOrderBy[i];
        
        // build next constraint
        if (i > 0) sindex += snprintf(s+sindex, slen-sindex, ", ");
        
        int idx = orderby->iColumn;
        const char *colname = COLNAME_FROM_INDEX(idx);
        if (!colname_is_legal(colname)) orderconsumed = 0;
        
        sindex += snprintf(s+sindex, slen-sindex, "%s %s", colname, orderby->desc ? " DESC" : " ASC");
    }
    
    idxinfo->idxNum = idxnum;
    idxinfo->idxStr = s;
    idxinfo->needToFreeIdxStr = 1;
    idxinfo->orderByConsumed = orderconsumed;
    
    // the goal of the xBestIndex function is to help SQLite's query planner decide on the most efficient way
    // to execute a query on the virtual table. It does so by evaluating which constraints (filters) can be applied
    // and providing an estimate of the cost and number of rows that the query will return.
    
    
    /*
     
     By commenting the following code we assume that the developer is using an SQLite library
     more recent than 3.8.2 released on 2013-12-06
     
     int version = sqlite3_libversion_number();
     if (version >= 3008002) {
     // field sqlite3_int64 estimatedRows is available (estimated number of rows returned)
     }
     
     if (version >= 3009000) {
     // field int idxFlags is available (mask of SQLITE_INDEX_SCAN)
     }
     
     if (version >= 3010000) {
     // field sqlite3_uint64 colUsed is available (input: mask of columns used by statement)
     }
     
     */
    
    // perform estimated cost and row count based on the constraints
    if ((idxnum & 6) == 6) {
        // both DbVrsn and SiteId constraints are present
        // query is expected to be highly selective, returning only one row, with a very low execution cost
        idxinfo->estimatedCost = 1.0;
        idxinfo->estimatedRows = 1;
    } else if ((idxnum & 2) == 2) {
        // only DbVrsn constraint is present
        // query is expected to return more rows (10) and take more time (cost of 10.0) than in the previous case
        idxinfo->estimatedCost = 10.0;
        idxinfo->estimatedRows = 10;
    } else if ((idxnum & 4) == 4) {
        // only SiteId constraint is present
        // query is expected to be very inefficient, returning a large number of rows and taking a long time to execute
        idxinfo->estimatedCost = (double)INT32_MAX;
        idxinfo->estimatedRows = (sqlite3_int64)INT32_MAX;
    } else {
        // no constraints are present
        // worst-case scenario, where the query returns all rows from the virtual table
        idxinfo->estimatedCost = (double)INT64_MAX;
        idxinfo->estimatedRows = (sqlite3_int64)INT64_MAX;
    }
    
    return SQLITE_OK;
}


int cloudsync_changesvtab_filter (sqlite3_vtab_cursor *cursor, int idxn, const char *idxs, int argc, sqlite3_value **argv) {
    DEBUG_VTAB("cloudsync_changesvtab_filter");
    
    cloudsync_changes_cursor *c = (cloudsync_changes_cursor *)cursor;
    sqlite3 *db = c->vtab->db;
    char *sql = build_changes_sql(db, idxs);
    if (sql == NULL) return SQLITE_NOMEM;
    
    int rc = sqlite3_prepare_v2(db, sql, -1, &c->vm, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto abort_filter;
    
    for (int i=0; i<argc; ++i) {
        rc = sqlite3_bind_value(c->vm, i+1, argv[i]);
        if (rc != SQLITE_OK) goto abort_filter;
    }
    
    rc = sqlite3_step(c->vm);
    CHECK_VFILTERTEST_ABORT();
    
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(c->vm);
        c->vm = NULL;
    } else if (rc != SQLITE_ROW) {
        goto abort_filter;
    }
    
    return SQLITE_OK;
    
abort_filter:
    // error condition
    DEBUG_VTAB("cloudsync_changesvtab_filter: %s\n", sqlite3_errmsg(db));
    if (c->vm) {
        sqlite3_finalize(c->vm);
        c->vm = NULL;
    }
    return rc;
}

int cloudsync_changesvtab_next (sqlite3_vtab_cursor *cursor) {
    DEBUG_VTAB("cloudsync_changesvtab_next");
    
    cloudsync_changes_cursor *c = (cloudsync_changes_cursor *)cursor;
    int rc = sqlite3_step(c->vm);
    
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(c->vm);
        c->vm = NULL;
        rc = SQLITE_OK;
    } else if (rc == SQLITE_ROW) {
        rc = SQLITE_OK;
    }
    
    if (rc != SQLITE_OK) DEBUG_VTAB("cloudsync_changesvtab_next: %s\n", sqlite3_errmsg(c->vtab->db));
    return rc;
}

int cloudsync_changesvtab_eof (sqlite3_vtab_cursor *cursor) {
    DEBUG_VTAB("cloudsync_changesvtab_eof");
    
    // we must return false (zero) if the specified cursor currently points to a valid row of data, or true (non-zero) otherwise
    cloudsync_changes_cursor *c = (cloudsync_changes_cursor *)cursor;
    return (c->vm) ? 0 : 1;
}

int cloudsync_changesvtab_column (sqlite3_vtab_cursor *cursor, sqlite3_context *ctx, int col) {
    DEBUG_VTAB("cloudsync_changesvtab_column %d\n", col);
    
    cloudsync_changes_cursor *c = (cloudsync_changes_cursor *)cursor;
    sqlite3_value *value = sqlite3_column_value(c->vm, col);
    sqlite3_result_value(ctx, value);
    
    return SQLITE_OK;
}

int cloudsync_changesvtab_rowid (sqlite3_vtab_cursor *cursor, sqlite3_int64 *rowid) {
    DEBUG_VTAB("cloudsync_changesvtab_rowid");
    
    cloudsync_changes_cursor *c = (cloudsync_changes_cursor *)cursor;
    sqlite3_int64 seq = sqlite3_column_int64(c->vm, COL_SEQ_INDEX);
    sqlite3_int64 db_version = sqlite3_column_int64(c->vm, COL_DBVERSION_INDEX);
    
    // for an explanation see https://github.com/sqliteai/sqlite-sync/blob/main/docs/RowID.md
    *rowid = (db_version << 30) | seq;
    return SQLITE_OK;
}

int cloudsync_changesvtab_update (sqlite3_vtab *vtab, int argc, sqlite3_value **argv, sqlite3_int64 *rowid) {
    DEBUG_VTAB("cloudsync_changesvtab_update");
    
    // only INSERT statements are allowed
    bool is_insert = (argc > 1 && sqlite3_value_type(argv[0]) == SQLITE_NULL);
    if (!is_insert) {
        cloudsync_vtab_set_error(vtab, "Only INSERT and SELECT statements are allowed against the cloudsync_changes table");
        return SQLITE_MISUSE;
    }
    
    // argv[0] is set only in case of DELETE statement (it contains the rowid of a row in the virtual table to be deleted)
    // argv[1] is the rowid of a new row to be inserted into the virtual table (always NULL in our case)
    // so reduce the number of meaningful arguments by 2
    return cloudsync_merge_insert(vtab, argc-2, &argv[2], rowid);
}

// MARK: -

cloudsync_context *cloudsync_vtab_get_context (sqlite3_vtab *vtab) {
    return (cloudsync_context *)(((cloudsync_changes_vtab *)vtab)->aux);
}

int cloudsync_vtab_set_error (sqlite3_vtab *vtab, const char *format, ...) {
    va_list arg;
    va_start (arg, format);
    char *err = cloudsync_memory_vmprintf(format, arg);
    va_end (arg);
    
    vtab->zErrMsg = err;
    return SQLITE_ERROR;
}

int cloudsync_vtab_register_changes (sqlite3 *db, cloudsync_context *xdata) {
    static sqlite3_module cloudsync_changes_module = {
        /* iVersion    */ 0,
        /* xCreate     */ 0, // Eponymous only virtual table
        /* xConnect    */ cloudsync_changesvtab_connect,
        /* xBestIndex  */ cloudsync_changesvtab_best_index,
        /* xDisconnect */ cloudsync_changesvtab_disconnect,
        /* xDestroy    */ 0,
        /* xOpen       */ cloudsync_changesvtab_open,
        /* xClose      */ cloudsync_changesvtab_close,
        /* xFilter     */ cloudsync_changesvtab_filter,
        /* xNext       */ cloudsync_changesvtab_next,
        /* xEof        */ cloudsync_changesvtab_eof,
        /* xColumn     */ cloudsync_changesvtab_column,
        /* xRowid      */ cloudsync_changesvtab_rowid,
        /* xUpdate     */ cloudsync_changesvtab_update,
        /* xBegin      */ 0,
        /* xSync       */ 0,
        /* xCommit     */ 0,
        /* xRollback   */ 0,
        /* xFindMethod */ 0,
        /* xRename     */ 0,
        /* xSavepoint  */ 0,
        /* xRelease    */ 0,
        /* xRollbackTo */ 0,
        /* xShadowName */ 0,
        /* xIntegrity  */ 0
    };
    
    return sqlite3_create_module(db, "cloudsync_changes", &cloudsync_changes_module, (void *)xdata);
}
