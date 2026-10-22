//
//  cloudsync_changes_sqlite.c
//  cloudsync
//
//  Created by Marco Bambini on 23/09/24.
//

#include <stdio.h>
#include <string.h>

#include "cloudsync_changes_sqlite.h"
#include "../utils.h"
#include "../dbutils.h"
#include "../pk.h"
#include "../khash.h"

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT3
#endif

typedef struct cloudsync_changes_vtab {
    sqlite3_vtab            base;       // base class, must be first
    sqlite3                 *db;
    cloudsync_context       *data;
} cloudsync_changes_vtab;

typedef struct cloudsync_changes_cursor {
    sqlite3_vtab_cursor     base;       // base class, must be first
    cloudsync_changes_vtab  *vtab;
    sqlite3_stmt            *vm;        // prepared statement
    cloudsync_changes_scan  *scan;      // state of the current scan
} cloudsync_changes_cursor;

// A table's visible-pk set is built once its lookups in one scan have returned this many
// hidden rows: below it the per-row lookups are cheaper than one scan of the base table.
#define CHANGES_VISIBLE_SET_THRESHOLD   1000
// Above this many visible rows the set is dropped and the scan keeps the per-row lookups.
#define CHANGES_VISIBLE_SET_MAX_ROWS    8000000

#define CHANGES_SCAN_PARAM              ":cloudsync_scan"
#define CHANGES_SCAN_MAX_PKS            255

KHASH_SET_INIT_INT(changes_pkset)

typedef enum {
    pkset_none = 0,
    pkset_built,
    pkset_off
} changes_pkset_state_t;

typedef enum {
    pk_affinity_blob = 0,
    pk_affinity_text,
    pk_affinity_integer,
    pk_affinity_other
} changes_pk_affinity_t;

typedef struct {
    cloudsync_table_context *table;
    int64_t                 hidden;     // lookups that found no row
    changes_pkset_state_t   state;
    khash_t(changes_pkset)  *set;       // 32-bit hashes of the visible pks
    uint8_t                 *affinity;  // per pk column
    int                     npks;
    sqlite3_int64           changes;    // sqlite3_total_changes64 when the set was built
} changes_scan_table;

struct cloudsync_changes_scan {
    changes_scan_table      *tables;
    int                     ntables;
    int                     capacity;

    // last visible value, served to the second evaluation of the same row
    cloudsync_table_context *value_table;
    char                    *value_key; // col_name, NUL, pk
    size_t                  value_keylen;
    size_t                  value_keycap;
    sqlite3_value           *value;
};

#if CLOUDSYNC_UNITTEST
int64_t changes_visible_set_threshold = CHANGES_VISIBLE_SET_THRESHOLD;
#define VISIBLE_SET_THRESHOLD       changes_visible_set_threshold
#else
#define VISIBLE_SET_THRESHOLD       CHANGES_VISIBLE_SET_THRESHOLD
#endif

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

int vtab_set_error (sqlite3_vtab *vtab, const char *format, ...) {
    va_list arg;
    va_start (arg, format);
    char *err = sqlite3_vmprintf(format, arg);
    va_end (arg);
    
    if (vtab->zErrMsg) sqlite3_free(vtab->zErrMsg);
    vtab->zErrMsg = err;
    return SQLITE_ERROR;
}

const char *vtab_opname_from_value (int value) {
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

int vtab_colname_is_legal (const char *name) {
    int count = sizeof(cloudsync_changes_columns) / sizeof (char *);
    
    for (int i=0; i<count; ++i) {
        if (strcasecmp(cloudsync_changes_columns[i], name) == 0) return 1;
    }
    return 0;
}

char *vtab_build_changes_sql (cloudsync_context *data, const char *idxs) {
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
    "    SELECT format('%q',SUBSTR(tbl_name, 1, LENGTH(tbl_name) - 10)) AS table_name_literal, format('%w',SUBSTR(tbl_name, 1, LENGTH(tbl_name) - 10)) AS table_name_identifier, format('%w',tbl_name) AS table_meta "
    "    FROM sqlite_master "
    "    WHERE type = 'table' AND tbl_name LIKE '%_cloudsync' "
    "), "
    "changes_query AS ( "
    "    SELECT "
    "        'SELECT "
    "        ''' || \"table_name_literal\" || ''' AS tbl, "
    "        t1.pk AS pk, "
    "        t1.col_name AS col_name, "
    "        cloudsync_col_value(''' || \"table_name_literal\" || ''', t1.col_name, t1.pk, " CHANGES_SCAN_PARAM ") AS col_value, "
    "        t1.col_version AS col_version, "
    "        t1.db_version AS db_version, "
    "        site_tbl.site_id AS site_id, "
    "        t1.seq AS seq, "
    "        COALESCE(t2.col_version, 1) AS cl "
    "     FROM \"' || \"table_meta\" || '\" AS t1 "
    "     LEFT JOIN cloudsync_site_id AS site_tbl ON t1.site_id = site_tbl.rowid "
    "     LEFT JOIN \"' || \"table_meta\" || '\" AS t2 ON t1.pk = t2.pk AND t2.col_name = ''" CLOUDSYNC_TOMBSTONE_VALUE "'' "
    "     WHERE col_value IS NOT ''" CLOUDSYNC_RLS_RESTRICTED_VALUE "''' "
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
    "SELECT final_string || ' ";
    const char *final_query = ";' FROM final_query;";
    
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
    
    char *value = NULL;
    int rc = database_select_text(data, sql, &value);
    cloudsync_memory_free(sql);
    
    return (rc == DBRES_OK) ? value : NULL;
}

// MARK: - Scan state -

// Under row-level security a change row is sent only when its pk lookup finds the row.
// Once a scan has seen enough hidden rows of a table, one SELECT of that table's pks
// (under the same RLS rule and read snapshot) replaces the lookups of the pks it doesn't
// return. Pks it returns still go through the lookup, so a hash collision only costs one.

static uint32_t changes_pk_hash (const void *pk, int pklen) {
    const uint8_t *p = (const uint8_t *)pk;
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < pklen; ++i) {h ^= p[i]; h *= 1099511628211ULL;}
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
    return (uint32_t)h;
}

static uint8_t changes_pk_affinity (const char *type) {
    // SQLite's affinity rules for a declared column type, in their order
    if (!type || !type[0]) return pk_affinity_blob;
    if (sqlite3_strlike("%INT%", type, 0) == 0) return pk_affinity_integer;
    if (sqlite3_strlike("%CHAR%", type, 0) == 0 || sqlite3_strlike("%CLOB%", type, 0) == 0 || sqlite3_strlike("%TEXT%", type, 0) == 0) return pk_affinity_text;
    if (sqlite3_strlike("%BLOB%", type, 0) == 0) return pk_affinity_blob;
    if (sqlite3_strlike("%REAL%", type, 0) == 0 || sqlite3_strlike("%FLOA%", type, 0) == 0 || sqlite3_strlike("%DOUB%", type, 0) == 0) return pk_affinity_other;
    return pk_affinity_integer; // NUMERIC stores integers as integers
}

typedef struct {
    changes_scan_table  *t;
    size_t              size;
    bool                canonical;
} changes_pk_check;

static int changes_pk_check_callback (void *xdata, int index, int type, int64_t ival, double dval, char *pval) {
    UNUSED_PARAMETER(dval); UNUSED_PARAMETER(pval);
    changes_pk_check *check = (changes_pk_check *)xdata;
    if (index >= check->t->npks) {check->canonical = false; return DBRES_OK;}
    uint8_t affinity = check->t->affinity[index];
    switch (type) {
        case DBTYPE_INTEGER:
            if (affinity != pk_affinity_integer && affinity != pk_affinity_blob) check->canonical = false;
            break;
        case DBTYPE_TEXT:
            if (affinity != pk_affinity_text && affinity != pk_affinity_blob) check->canonical = false;
            break;
        case DBTYPE_BLOB:
            break;
        default:
            check->canonical = false;
            return DBRES_OK;
    }
    size_t size = pk_encode_raw_size(type, ival);
    if (size == SIZE_MAX) check->canonical = false;
    else check->size += size;
    return DBRES_OK;
}

// True when a base row matching this pk under the lookup's `pk = ?` comparison would
// encode to exactly these bytes: no affinity conversion applies and the encoding is minimal.
static bool changes_pk_canonical (changes_scan_table *t, const void *pk, int pklen) {
    changes_pk_check check = {.t = t, .size = 1, .canonical = true};
    int count = pk_decode_prikey((char *)pk, (size_t)pklen, changes_pk_check_callback, &check);
    return (check.canonical && count == t->npks && check.size == (size_t)pklen);
}

static void changes_scan_table_clear (changes_scan_table *t) {
    if (t->set) kh_destroy(changes_pkset, t->set);
    if (t->affinity) cloudsync_memory_free(t->affinity);
    t->set = NULL;
    t->affinity = NULL;
}

static bool changes_scan_build_set (changes_scan_table *t, sqlite3 *db, const char *table_name) {
    sqlite3_stmt *vm = NULL;
    char *cols = NULL;
    bool result = false;
    int capacity = 0;

    // pk columns in the order the metadata pk is encoded in, as the lookup statement uses
    int rc = sqlite3_prepare_v2(db, "SELECT name, type FROM pragma_table_info(?1) WHERE pk>0 ORDER BY pk;", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    sqlite3_bind_text(vm, 1, table_name, -1, SQLITE_STATIC);
    while ((rc = sqlite3_step(vm)) == SQLITE_ROW) {
        if (t->npks == capacity) {
            capacity = capacity ? capacity * 2 : 4;
            uint8_t *affinity = cloudsync_memory_realloc(t->affinity, (uint64_t)capacity);
            if (!affinity) goto cleanup;
            t->affinity = affinity;
        }
        t->affinity[t->npks++] = changes_pk_affinity((const char *)sqlite3_column_text(vm, 1));
        cols = sqlite3_mprintf("%z%s\"%w\"", cols, cols ? "," : "", (const char *)sqlite3_column_text(vm, 0));
        if (!cols) goto cleanup;
    }
    if (rc != SQLITE_DONE || t->npks == 0 || t->npks > CHANGES_SCAN_MAX_PKS) goto cleanup;
    sqlite3_finalize(vm);
    vm = NULL;

    // a non-binary collation lets the lookup match a pk spelled differently from the stored one
    rc = sqlite3_prepare_v2(db, "SELECT 1 FROM pragma_index_list(?1) AS l, pragma_index_xinfo(l.name) AS x "
                                "WHERE l.origin = 'pk' AND x.key = 1 AND upper(x.coll) <> 'BINARY' LIMIT 1;", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    sqlite3_bind_text(vm, 1, table_name, -1, SQLITE_STATIC);
    rc = sqlite3_step(vm);
    if (rc != SQLITE_DONE) goto cleanup;
    sqlite3_finalize(vm);
    vm = NULL;

    char *sql = sqlite3_mprintf("SELECT %s FROM \"%w\";", cols, table_name);
    if (!sql) goto cleanup;
    rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto cleanup;

    t->set = kh_init(changes_pkset);
    if (!t->set) goto cleanup;
    sqlite3_value *values[CHANGES_SCAN_MAX_PKS];
    while ((rc = sqlite3_step(vm)) == SQLITE_ROW) {
        for (int i = 0; i < t->npks; ++i) values[i] = sqlite3_column_value(vm, i);
        char buffer[1024];
        size_t pklen = sizeof(buffer);
        char *pk = pk_encode_prikey((dbvalue_t **)values, t->npks, buffer, &pklen);
        if (!pk) goto cleanup;
        if (pk == PRIKEY_NULL_CONSTRAINT_ERROR) continue;
        int ret = 0;
        kh_put(changes_pkset, t->set, changes_pk_hash(pk, (int)pklen), &ret);
        if (pk != buffer) cloudsync_memory_free(pk);
        if (ret < 0 || kh_size(t->set) > CHANGES_VISIBLE_SET_MAX_ROWS) goto cleanup;
    }
    if (rc != SQLITE_DONE) goto cleanup;
    t->changes = sqlite3_total_changes64(db);
    result = true;

cleanup:
    if (vm) sqlite3_finalize(vm);
    if (cols) sqlite3_free(cols);
    return result;
}

static changes_scan_table *changes_scan_table_get (cloudsync_changes_scan *scan, cloudsync_table_context *table) {
    for (int i = 0; i < scan->ntables; ++i) {
        if (scan->tables[i].table == table) return &scan->tables[i];
    }
    if (scan->ntables == scan->capacity) {
        int capacity = scan->capacity ? scan->capacity * 2 : 4;
        changes_scan_table *tables = cloudsync_memory_realloc(scan->tables, (uint64_t)(sizeof(changes_scan_table) * capacity));
        if (!tables) return NULL;
        scan->tables = tables;
        scan->capacity = capacity;
    }
    changes_scan_table *t = &scan->tables[scan->ntables++];
    memset(t, 0, sizeof(changes_scan_table));
    t->table = table;
    return t;
}

bool changes_scan_pk_hidden (cloudsync_changes_scan *scan, sqlite3 *db, cloudsync_table_context *table, const char *table_name, const void *pk, int pklen) {
    changes_scan_table *t = changes_scan_table_get(scan, table);
    if (!t || t->state == pkset_off) return false;
    if (t->state == pkset_none) {
        if (t->hidden < VISIBLE_SET_THRESHOLD) return false;
        if (!changes_scan_build_set(t, db, table_name)) {
            changes_scan_table_clear(t);
            t->state = pkset_off;
            return false;
        }
        t->state = pkset_built;
    }

    // a write on this connection since the build may have changed what a lookup finds
    if (sqlite3_total_changes64(db) != t->changes) {
        changes_scan_table_clear(t);
        t->state = pkset_off;
        return false;
    }

    if (!changes_pk_canonical(t, pk, pklen)) return false;
    return (kh_get(changes_pkset, t->set, changes_pk_hash(pk, pklen)) == kh_end(t->set));
}

void changes_scan_note_hidden (cloudsync_changes_scan *scan, cloudsync_table_context *table) {
    changes_scan_table *t = changes_scan_table_get(scan, table);
    if (t) t->hidden++;
}

static void changes_scan_drop_value (cloudsync_changes_scan *scan) {
    if (scan->value) sqlite3_value_free(scan->value);
    scan->value = NULL;
    scan->value_table = NULL;
}

// The scan's WHERE drops hidden rows by testing col_value, so SQLite evaluates
// cloudsync_col_value once for the WHERE and again for the result column. The second
// call for the same table, column and pk takes the value the first one read.
bool changes_scan_take_value (cloudsync_changes_scan *scan, cloudsync_table_context *table, const char *col_name, const void *pk, int pklen, sqlite3_context *context) {
    if (!scan->value || scan->value_table != table) return false;
    size_t collen = strlen(col_name) + 1;
    if (scan->value_keylen != collen + (size_t)pklen) return false;
    if (memcmp(scan->value_key, col_name, collen) != 0) return false;
    if (pklen > 0 && memcmp(scan->value_key + collen, pk, (size_t)pklen) != 0) return false;
    sqlite3_result_value(context, scan->value);
    changes_scan_drop_value(scan);
    return true;
}

void changes_scan_keep_value (cloudsync_changes_scan *scan, cloudsync_table_context *table, const char *col_name, const void *pk, int pklen, sqlite3_value *value) {
    changes_scan_drop_value(scan);
    size_t collen = strlen(col_name) + 1;
    size_t keylen = collen + (size_t)pklen;
    if (keylen > scan->value_keycap) {
        char *key = cloudsync_memory_realloc(scan->value_key, (uint64_t)keylen);
        if (!key) return;
        scan->value_key = key;
        scan->value_keycap = keylen;
    }
    memcpy(scan->value_key, col_name, collen);
    if (pklen > 0) memcpy(scan->value_key + collen, pk, (size_t)pklen);
    scan->value_keylen = keylen;
    scan->value = sqlite3_value_dup(value);
    if (scan->value) scan->value_table = table;
}

static void changes_scan_reset (cloudsync_changes_scan *scan) {
    for (int i = 0; i < scan->ntables; ++i) changes_scan_table_clear(&scan->tables[i]);
    scan->ntables = 0;
    changes_scan_drop_value(scan);
}

static void changes_scan_free (cloudsync_changes_scan *scan) {
    if (!scan) return;
    changes_scan_reset(scan);
    if (scan->tables) cloudsync_memory_free(scan->tables);
    if (scan->value_key) cloudsync_memory_free(scan->value_key);
    cloudsync_memory_free(scan);
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
        vnew->data = aux;
        
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
    changes_scan_free(c->scan);
    
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
    size_t slen = ((size_t)count1 * 32) + ((size_t)count2 * 20) + 512;
    char *s = (char *)sqlite3_malloc64((sqlite3_uint64)slen);
    if (!s) return SQLITE_NOMEM;
    size_t sindex= 0;

    int idxnum = 0;
    int arg_index = 1;
    int orderconsumed = 1;
    
    // is there a WHERE clause ?
    int accepted = 0;
    
    // check constraints
    for (int i=0; i < count1; ++i) {
        // analyze only usable constraints
        struct sqlite3_index_constraint *constraint = &idxinfo->aConstraint[i];
        if (constraint->usable == false) continue;
        
        int idx = constraint->iColumn;
        uint8_t op = constraint->op;
        
        const char *colname = (idx >= 0 && idx < CLOUDSYNC_CHANGES_NCOLS) ? COLNAME_FROM_INDEX(idx) : "rowid";
        const char *opname = vtab_opname_from_value(op);
        if (!opname) continue;
        
        // build next constraint
        sindex += snprintf(s+sindex, slen-sindex, accepted++ ? " AND " : "WHERE ");
        
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
        const char *colname = (idx >= 0 && idx < CLOUDSYNC_CHANGES_NCOLS) ? COLNAME_FROM_INDEX(idx) : "rowid";
        if (!vtab_colname_is_legal(colname)) orderconsumed = 0;
        
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
    cloudsync_context *data = c->vtab->data;
    sqlite3 *db = c->vtab->db;
    char *sql = vtab_build_changes_sql(data, idxs);
    if (sql == NULL) {
        // vtab_build_changes_sql returns NULL when no *_cloudsync meta-tables
        // exist (cloudsync_init was never called, or the last configured table
        // was cleaned up): the inner GROUP_CONCAT produces a NULL row and the
        // outer SELECT yields a NULL final string. Distinguish this from a
        // genuine OOM by checking whether cloudsync is configured, so the user
        // gets an actionable message instead of "out of memory".
        if (!cloudsync_config_exists(data) || dbutils_table_settings_count_tables(data) == 0) {
            return vtab_set_error((sqlite3_vtab *)c->vtab,
                "cloudsync has no tables configured for sync. Call "
                "SELECT cloudsync_init('<table_name>') to enable sync on a "
                "table before querying cloudsync_changes.");
        }
        // settings rows exist but no *_cloudsync tables do (e.g. a dump/restore
        // that skipped the meta tables): still not an OOM
        sqlite3_stmt *vm = NULL;
        bool meta_missing = false;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND tbl_name LIKE '%_cloudsync' LIMIT 1;", -1, &vm, NULL) == SQLITE_OK) {
            meta_missing = (sqlite3_step(vm) != SQLITE_ROW);
        }
        if (vm) sqlite3_finalize(vm);
        if (meta_missing) {
            return vtab_set_error((sqlite3_vtab *)c->vtab,
                "cloudsync settings reference tables whose sync metadata is "
                "missing (no *_cloudsync tables found). Re-run "
                "SELECT cloudsync_init('<table_name>') for each configured "
                "table to rebuild it.");
        }
        return SQLITE_NOMEM;
    }
    
    // the xFilter method may be called multiple times on the same sqlite3_vtab_cursor*
    if (c->vm) sqlite3_finalize(c->vm);
    c->vm = NULL;
    
    // each xFilter starts a new scan
    if (c->scan) changes_scan_reset(c->scan);
    else c->scan = (cloudsync_changes_scan *)cloudsync_memory_zeroalloc(sizeof(cloudsync_changes_scan));
    if (!c->scan) {
        cloudsync_memory_free(sql);
        return SQLITE_NOMEM;
    }
    
    int rc = sqlite3_prepare_v2(db, sql, -1, &c->vm, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto abort_filter;
    
    // the constraint values fill the anonymous parameters in order
    int scan_index = sqlite3_bind_parameter_index(c->vm, CHANGES_SCAN_PARAM);
    for (int i=0, index=1; i<argc; ++index) {
        if (index == scan_index) continue;
        rc = sqlite3_bind_value(c->vm, index, argv[i++]);
        if (rc != SQLITE_OK) goto abort_filter;
    }
    if (scan_index > 0) {
        rc = sqlite3_bind_pointer(c->vm, scan_index, c->scan, CLOUDSYNC_CHANGES_SCAN_PTR, NULL);
        if (rc != SQLITE_OK) goto abort_filter;
    }
    
    rc = sqlite3_step(c->vm);
    CHECK_VFILTERTEST_ABORT();
    
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(c->vm);
        c->vm = NULL;
        changes_scan_reset(c->scan);
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
        changes_scan_reset(c->scan);
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

int cloudsync_changesvtab_insert_gos (sqlite3_vtab *vtab, cloudsync_context *data, cloudsync_table_context *table, const char *insert_pk, int insert_pk_len, const char *insert_name, sqlite3_value *insert_value, sqlite3_int64 insert_col_version, sqlite3_int64 insert_db_version, const char *insert_site_id, int insert_site_id_len, sqlite3_int64 insert_seq, int64_t *rowid) {
    DEBUG_VTAB("cloudsync_changesvtab_insert_gos");
    
    // Grow-Only Set (GOS) Algorithm: Only insertions are allowed, deletions and updates are prevented from a trigger.
    int rc = merge_insert_col(data, table, insert_pk, insert_pk_len, insert_name, insert_value, (int64_t)insert_col_version, (int64_t)insert_db_version, insert_site_id, insert_site_id_len, (int64_t)insert_seq, rowid);
    
    if (rc != SQLITE_OK) {
        vtab_set_error(vtab, "%s", cloudsync_errmsg(data));
    }
    
    return rc;
}

int cloudsync_changesvtab_insert (sqlite3_vtab *vtab, int argc, sqlite3_value **argv, sqlite3_int64 *rowid) {
    DEBUG_VTAB("cloudsync_changesvtab_insert");
    
    // this function performs the merging logic for an insert in a cloud-synchronized table. It handles
    // different scenarios including conflicts, causal lengths, delete operations, and resurrecting rows
    // based on the incoming data (from remote nodes or clients) and the local database state

    // this function handles different CRDT algorithms (GOS, DWS, AWS, and CLS).
    // the merging strategy is determined based on the table->algo value.
    
    // meta table declaration:
    // tbl TEXT NOT NULL, pk BLOB NOT NULL, col_name TEXT NOT NULL,"
    // "col_value ANY, col_version INTEGER NOT NULL, db_version INTEGER NOT NULL,"
    // "site_id BLOB NOT NULL, cl INTEGER NOT NULL, seq INTEGER NOT NULL
    
    // meta information to retrieve from arguments:
    // argv[0] -> table name (TEXT)
    // argv[1] -> primary key (BLOB)
    // argv[2] -> column name (TEXT or NULL if sentinel)
    // argv[3] -> column value (ANY)
    // argv[4] -> column version (INTEGER)
    // argv[5] -> database version (INTEGER)
    // argv[6] -> site ID (BLOB, identifies the origin of the update)
    // argv[7] -> causal length (INTEGER, tracks the order of operations)
    // argv[8] -> sequence number (INTEGER, unique per operation)
    
    // extract table name
    const char *insert_tbl = (const char *)sqlite3_value_text(argv[0]);
    
    // lookup table
    cloudsync_context *data = (cloudsync_context *)(((cloudsync_changes_vtab *)vtab)->data);
    cloudsync_table_context *table = table_lookup(data, insert_tbl);
    if (!table) return vtab_set_error(vtab, "Unable to find table %s,", insert_tbl);
    
    // extract the remaining fields from the input values
    const char *insert_pk = (const char *)sqlite3_value_blob(argv[1]);
    int insert_pk_len = sqlite3_value_bytes(argv[1]);
    const char *insert_name = (sqlite3_value_type(argv[2]) == SQLITE_NULL) ? CLOUDSYNC_TOMBSTONE_VALUE : (const char *)sqlite3_value_text(argv[2]);
    sqlite3_value *insert_value = argv[3];
    int64_t insert_col_version = sqlite3_value_int64(argv[4]);
    int64_t insert_db_version = sqlite3_value_int64(argv[5]);
    const char *insert_site_id = (const char *)sqlite3_value_blob(argv[6]);
    int insert_site_id_len = sqlite3_value_bytes(argv[6]);
    int64_t insert_cl = sqlite3_value_int64(argv[7]);
    int64_t insert_seq = sqlite3_value_int64(argv[8]);
    
    // perform different logic for each different table algorithm
    if (table_algo_isgos(table)) return cloudsync_changesvtab_insert_gos(vtab, data, table, insert_pk, insert_pk_len, insert_name, insert_value, insert_col_version, insert_db_version, insert_site_id, insert_site_id_len, insert_seq, (int64_t *)rowid);
    
    int rc = merge_insert (data, table, insert_pk, insert_pk_len, insert_cl, insert_name, insert_value, insert_col_version, insert_db_version, insert_site_id, insert_site_id_len, insert_seq, (int64_t *)rowid);
    if (rc != SQLITE_OK) {
        return vtab_set_error(vtab, "%s", cloudsync_errmsg(data));
    }
    
    return SQLITE_OK;
}

int cloudsync_changesvtab_update (sqlite3_vtab *vtab, int argc, sqlite3_value **argv, sqlite3_int64 *rowid) {
    DEBUG_VTAB("cloudsync_changesvtab_update");
    
    // only INSERT statements are allowed
    bool is_insert = (argc > 1 && sqlite3_value_type(argv[0]) == SQLITE_NULL);
    if (!is_insert) {
        vtab_set_error(vtab, "Only INSERT and SELECT statements are allowed against the cloudsync_changes table");
        return SQLITE_MISUSE;
    }
    
    // argv[0] is set only in case of DELETE statement (it contains the rowid of a row in the virtual table to be deleted)
    // argv[1] is the rowid of a new row to be inserted into the virtual table (always NULL in our case)
    // so reduce the number of meaningful arguments by 2
    return cloudsync_changesvtab_insert(vtab, argc-2, &argv[2], rowid);
}

// MARK: -

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
