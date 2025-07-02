//
//  cloudsync.c
//  cloudsync
//
//  Created by Marco Bambini on 16/05/24.
//

#include <inttypes.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>

#include "cloudsync.h"
#include "cloudsync_private.h"
#include "lz4.h"
#include "pk.h"
#include "vtab.h"
#include "utils.h"
#include "dbutils.h"

#ifndef CLOUDSYNC_OMIT_NETWORK
#include "network.h"
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>                          // for htonl, htons, ntohl, ntohs
#include <netinet/in.h>                         // for struct sockaddr_in, INADDR_ANY, etc. (if needed)
#endif

#ifndef htonll
#if __BIG_ENDIAN__
#define htonll(x)                               (x)
#define ntohll(x)                               (x)
#else
#ifndef htobe64
#define htonll(x)                               ((uint64_t)htonl((x) & 0xFFFFFFFF) << 32 | (uint64_t)htonl((x) >> 32))
#define ntohll(x)                               ((uint64_t)ntohl((x) & 0xFFFFFFFF) << 32 | (uint64_t)ntohl((x) >> 32))
#else
#define htonll(x)                               htobe64(x)
#define ntohll(x)                               be64toh(x)
#endif
#endif
#endif

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT1
#endif

#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(X) (void)(X)
#endif

#ifdef _WIN32
#define APIEXPORT   __declspec(dllexport)
#else
#define APIEXPORT
#endif

#define CLOUDSYNC_DEFAULT_ALGO                  "cls"
#define CLOUDSYNC_INIT_NTABLES                  128
#define CLOUDSYNC_VALUE_NOTSET                  -1
#define CLOUDSYNC_MIN_DB_VERSION                0

#define CLOUDSYNC_PAYLOAD_MINBUF_SIZE           512*1024
#define CLOUDSYNC_PAYLOAD_VERSION               1
#define CLOUDSYNC_PAYLOAD_SIGNATURE             'CLSY'
#define CLOUDSYNC_PAYLOAD_APPLY_CALLBACK_KEY    "cloudsync_payload_apply_callback"

#ifndef MAX
#define MAX(a, b)                               (((a)>(b))?(a):(b))
#endif

#define DEBUG_SQLITE_ERROR(_rc, _fn, _db)   do {if (_rc != SQLITE_OK) printf("Error in %s: %s\n", _fn, sqlite3_errmsg(_db));} while (0)

typedef enum {
    CLOUDSYNC_PK_INDEX_TBL          = 0,
    CLOUDSYNC_PK_INDEX_PK           = 1,
    CLOUDSYNC_PK_INDEX_COLNAME      = 2,
    CLOUDSYNC_PK_INDEX_COLVALUE     = 3,
    CLOUDSYNC_PK_INDEX_COLVERSION   = 4,
    CLOUDSYNC_PK_INDEX_DBVERSION    = 5,
    CLOUDSYNC_PK_INDEX_SITEID       = 6,
    CLOUDSYNC_PK_INDEX_CL           = 7,
    CLOUDSYNC_PK_INDEX_SEQ          = 8
} CLOUDSYNC_PK_INDEX;

typedef enum {
    CLOUDSYNC_STMT_VALUE_ERROR      = -1,
    CLOUDSYNC_STMT_VALUE_UNCHANGED  = 0,
    CLOUDSYNC_STMT_VALUE_CHANGED    = 1,
} CLOUDSYNC_STMT_VALUE;

typedef struct {
    sqlite3_context *context;
    int             index;
} cloudsync_pk_decode_context;

#define SYNCBIT_SET(_data)                  _data->insync = 1
#define SYNCBIT_RESET(_data)                _data->insync = 0
#define BUMP_SEQ(_data)                     ((_data)->seq += 1, (_data)->seq - 1)

// MARK: -

typedef struct {
    table_algo      algo;                           // CRDT algoritm associated to the table
    char            *name;                          // table name
    char            **col_name;                     // array of column names
    sqlite3_stmt    **col_merge_stmt;               // array of merge insert stmt (indexed by col_name)
    sqlite3_stmt    **col_value_stmt;               // array of column value stmt (indexed by col_name)
    int             *col_id;                        // array of column id
    int             ncols;                          // number of non primary key cols
    int             npks;                           // number of primary key cols
    bool            enabled;                        // flag to check if a table is enabled or disabled
    #if !CLOUDSYNC_DISABLE_ROWIDONLY_TABLES
    bool            rowid_only;                     // a table with no primary keys other than the implicit rowid
    #endif
    
    char            **pk_name;                      // array of primary key names
    
    // precompiled statements
    sqlite3_stmt    *meta_pkexists_stmt;            // check if a primary key already exist in the augmented table
    sqlite3_stmt    *meta_sentinel_update_stmt;     // update a local sentinel row
    sqlite3_stmt    *meta_sentinel_insert_stmt;     // insert a local sentinel row
    sqlite3_stmt    *meta_row_insert_update_stmt;   // insert/update a local row
    sqlite3_stmt    *meta_row_drop_stmt;            // delete rows from meta
    sqlite3_stmt    *meta_update_move_stmt;         // update rows in meta when pk changes
    sqlite3_stmt    *meta_local_cl_stmt;            // compute local cl value
    sqlite3_stmt    *meta_winner_clock_stmt;        // get the rowid of the last inserted/updated row in the meta table
    sqlite3_stmt    *meta_merge_delete_drop;
    sqlite3_stmt    *meta_zero_clock_stmt;
    sqlite3_stmt    *meta_col_version_stmt;
    sqlite3_stmt    *meta_site_id_stmt;
    
    sqlite3_stmt    *real_col_values_stmt;          // retrieve all column values based on pk
    sqlite3_stmt    *real_merge_delete_stmt;
    sqlite3_stmt    *real_merge_sentinel_stmt;
    
} cloudsync_table_context;

struct cloudsync_pk_decode_bind_context {
    sqlite3_stmt    *vm;
    char            *tbl;
    int64_t         tbl_len;
    const void      *pk;
    int64_t         pk_len;
    char            *col_name;
    int64_t         col_name_len;
    int64_t         col_version;
    int64_t         db_version;
    const void      *site_id;
    int64_t         site_id_len;
    int64_t         cl;
    int64_t         seq;
};

struct cloudsync_context {
    sqlite3_context *sqlite_ctx;
    
    char            *libversion;
    uint8_t         site_id[UUID_LEN];
    int             insync;
    int             debug;
    bool            merge_equal_values;
    bool            temp_bool;                  // temporary value used in callback
    void            *aux_data;
    
    // stmts and context values
    bool            pragma_checked;             // we need to check PRAGMAs only once per transaction
    sqlite3_stmt    *schema_version_stmt;
    sqlite3_stmt    *data_version_stmt;
    sqlite3_stmt    *db_version_stmt;
    sqlite3_stmt    *getset_siteid_stmt;
    int             data_version;
    int             schema_version;
    uint64_t        schema_hash;
    
    // set at the start of each transaction on the first invocation and
    // re-set on transaction commit or rollback
    sqlite3_int64   db_version;
    // the version that the db will be set to at the end of the transaction
    // if that transaction were to commit at the time this value is checked
    sqlite3_int64   pending_db_version;
    // used to set an order inside each transaction
    int             seq;
    
    // augmented tables are stored in-memory so we do not need to retrieve information about col names and cid
    // from the disk each time a write statement is performed
    // we do also not need to use an hash map here because for few tables the direct in-memory comparison with table name is faster
    cloudsync_table_context **tables;
    int tables_count;
    int tables_alloc;
};

typedef struct {
    char        *buffer;
    size_t      balloc;
    size_t      bused;
    uint64_t    nrows;
    uint16_t    ncols;
} cloudsync_network_payload;

#ifdef _MSC_VER
    #pragma pack(push, 1) // For MSVC: pack struct with 1-byte alignment
    #define PACKED
#else
    #define PACKED __attribute__((__packed__))
#endif

typedef struct PACKED {
    uint32_t    signature;         // 'CLSY'
    uint8_t     version;           // protocol version
    uint8_t     libversion[3];     // major.minor.patch
    uint32_t    expanded_size;
    uint16_t    ncols;
    uint32_t    nrows;
    uint64_t    schema_hash;
    uint8_t     unused[6];        // padding to ensure the struct is exactly 32 bytes
} cloudsync_network_header;

#ifdef _MSC_VER
    #pragma pack(pop)
#endif

#if CLOUDSYNC_UNITTEST
bool force_uncompressed_blob = false;
#define CHECK_FORCE_UNCOMPRESSED_BUFFER()   if (force_uncompressed_blob) use_uncompressed_buffer = true
#else
#define CHECK_FORCE_UNCOMPRESSED_BUFFER()
#endif

int db_version_rebuild_stmt (sqlite3 *db, cloudsync_context *data);
int cloudsync_load_siteid (sqlite3 *db, cloudsync_context *data);
int local_mark_insert_or_update_meta (sqlite3 *db, cloudsync_table_context *table, const char *pk, size_t pklen, const char *col_name, sqlite3_int64 db_version, int seq);

// MARK: - STMT Utils -

CLOUDSYNC_STMT_VALUE stmt_execute (sqlite3_stmt *stmt, cloudsync_context *data) {
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        if (data) DEBUG_SQLITE_ERROR(rc, "stmt_execute", sqlite3_db_handle(stmt));
        sqlite3_reset(stmt);
        return CLOUDSYNC_STMT_VALUE_ERROR;
    }
    
    CLOUDSYNC_STMT_VALUE result = CLOUDSYNC_STMT_VALUE_CHANGED;
    if (stmt == data->data_version_stmt) {
        int version = sqlite3_column_int(stmt, 0);
        if (version != data->data_version) {
            data->data_version = version;
        } else {
            result = CLOUDSYNC_STMT_VALUE_UNCHANGED;
        }
    } else if (stmt == data->schema_version_stmt) {
        int version = sqlite3_column_int(stmt, 0);
        if (version > data->schema_version) {
            data->schema_version = version;
        } else {
            result = CLOUDSYNC_STMT_VALUE_UNCHANGED;
        }
        
    } else if (stmt == data->db_version_stmt) {
        data->db_version = (rc == SQLITE_DONE) ? CLOUDSYNC_MIN_DB_VERSION : sqlite3_column_int64(stmt, 0);
    }
    
    sqlite3_reset(stmt);
    return result;
}

int stmt_count (sqlite3_stmt *stmt, const char *value, size_t len, int type) {
    int result = -1;
    int rc = SQLITE_OK;
    
    if (value) {
        rc = (type == SQLITE_TEXT) ? sqlite3_bind_text(stmt, 1, value, (int)len, SQLITE_STATIC) : sqlite3_bind_blob(stmt, 1, value, (int)len, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto cleanup;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        result = 0;
        rc = SQLITE_OK;
    } else if (rc == SQLITE_ROW) {
        result = sqlite3_column_int(stmt, 0);
        rc = SQLITE_OK;
    }
    
cleanup:
    DEBUG_SQLITE_ERROR(rc, "stmt_count", sqlite3_db_handle(stmt));
    sqlite3_reset(stmt);
    return result;
}

sqlite3_stmt *stmt_reset (sqlite3_stmt *stmt) {
    sqlite3_clear_bindings(stmt);
    sqlite3_reset(stmt);
    return NULL;
}

int stmts_add_tocontext (sqlite3 *db, cloudsync_context *data) {
    DEBUG_DBFUNCTION("cloudsync_add_stmts");
    
    if (data->data_version_stmt == NULL) {
        const char *sql = "PRAGMA data_version;";
        int rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &data->data_version_stmt, NULL);
        DEBUG_STMT("data_version_stmt %p", data->data_version_stmt);
        if (rc != SQLITE_OK) return rc;
        DEBUG_SQL("data_version_stmt: %s", sql);
    }
    
    if (data->schema_version_stmt == NULL) {
        const char *sql = "PRAGMA schema_version;";
        int rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &data->schema_version_stmt, NULL);
        DEBUG_STMT("schema_version_stmt %p", data->schema_version_stmt);
        if (rc != SQLITE_OK) return rc;
        DEBUG_SQL("schema_version_stmt: %s", sql);
    }
    
    if (data->getset_siteid_stmt == NULL) {
        // get and set index of the site_id
        // in SQLite, we can’t directly combine an INSERT and a SELECT to both insert a row and return an identifier (rowid) in a single statement,
        // however, we can use a workaround by leveraging the INSERT statement with ON CONFLICT DO UPDATE and then combining it with RETURNING rowid
        const char *sql = "INSERT INTO cloudsync_site_id (site_id) VALUES (?) ON CONFLICT(site_id) DO UPDATE SET site_id = site_id RETURNING rowid;";
        int rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &data->getset_siteid_stmt, NULL);
        DEBUG_STMT("getset_siteid_stmt %p", data->getset_siteid_stmt);
        if (rc != SQLITE_OK) return rc;
        DEBUG_SQL("getset_siteid_stmt: %s", sql);
    }
    
    return db_version_rebuild_stmt(db, data);
}

// MARK: - Database Version -

char *db_version_build_query (sqlite3 *db) {
    // this function must be manually called each time tables changes
    // because the query plan changes too and it must be re-prepared
    // unfortunately there is no other way
    
    // we need to execute a query like:
    /*
     SELECT max(version) as version FROM (
         SELECT max(db_version) as version FROM "table1_cloudsync"
         UNION ALL
         SELECT max(db_version) as version FROM "table2_cloudsync"
         UNION ALL
         SELECT max(db_version) as version FROM "table3_cloudsync"
         UNION
         SELECT value as version FROM cloudsync_settings WHERE key = 'pre_alter_dbversion'
     )
     */
    
    // the good news is that the query can be computed in SQLite without the need to do any extra computation from the host language
    const char *sql = "WITH table_names AS ("
                      "SELECT format('%w', name) as tbl_name "
                      "FROM sqlite_master "
                      "WHERE type='table' "
                      "AND name LIKE '%_cloudsync'"
                      "), "
                      "query_parts AS ("
                      "SELECT 'SELECT max(db_version) as version FROM \"' || tbl_name || '\"' as part FROM table_names"
                      "), "
                      "combined_query AS ("
                      "SELECT GROUP_CONCAT(part, ' UNION ALL ') || ' UNION SELECT value as version FROM cloudsync_settings WHERE key = ''pre_alter_dbversion''' as full_query FROM query_parts"
                      ") "
                      "SELECT CONCAT('SELECT max(version) as version FROM (', full_query, ');') FROM combined_query;";
    return dbutils_text_select(db, sql);
}

int db_version_rebuild_stmt (sqlite3 *db, cloudsync_context *data) {
    if (data->db_version_stmt) {
        sqlite3_finalize(data->db_version_stmt);
        data->db_version_stmt = NULL;
    }
    
    sqlite3_int64 count = dbutils_table_settings_count_tables(db);
    if (count == 0) return SQLITE_OK;
    else if (count == -1) {
        dbutils_context_result_error(data->sqlite_ctx, "%s", sqlite3_errmsg(db));
        return SQLITE_ERROR;
    }
    
    char *sql = db_version_build_query(db);
    if (!sql) return SQLITE_NOMEM;
    DEBUG_SQL("db_version_stmt: %s", sql);
    
    int rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &data->db_version_stmt, NULL);
    DEBUG_STMT("db_version_stmt %p", data->db_version_stmt);
    cloudsync_memory_free(sql);
    return rc;
}

int db_version_rerun (sqlite3 *db, cloudsync_context *data) {
    CLOUDSYNC_STMT_VALUE schema_changed = stmt_execute(data->schema_version_stmt, data);
    if (schema_changed == CLOUDSYNC_STMT_VALUE_ERROR) return -1;
    
    if (schema_changed == CLOUDSYNC_STMT_VALUE_CHANGED) {
        int rc = db_version_rebuild_stmt(db, data);
        if (rc != SQLITE_OK) return -1;
    }
    
    CLOUDSYNC_STMT_VALUE rc = stmt_execute(data->db_version_stmt, data);
    if (rc == CLOUDSYNC_STMT_VALUE_ERROR) return -1;
    return 0;
}

int db_version_check_uptodate (sqlite3 *db, cloudsync_context *data) {
    // perform a PRAGMA data_version to check if some other process write any data
    CLOUDSYNC_STMT_VALUE rc = stmt_execute(data->data_version_stmt, data);
    if (rc == CLOUDSYNC_STMT_VALUE_ERROR) return -1;
    
    // db_version is already set and there is no need to update it
    if (data->db_version != CLOUDSYNC_VALUE_NOTSET && rc == CLOUDSYNC_STMT_VALUE_UNCHANGED) return 0;
    
    return db_version_rerun(db, data);
}

sqlite3_int64 db_version_next (sqlite3 *db, cloudsync_context *data, sqlite3_int64 merging_version) {
    int rc = db_version_check_uptodate(db, data);
    if (rc != SQLITE_OK) return -1;
    
    sqlite3_int64 result = data->db_version + 1;
    if (result < data->pending_db_version) result = data->pending_db_version;
    if (merging_version != CLOUDSYNC_VALUE_NOTSET && result < merging_version) result = merging_version;
    data->pending_db_version = result;
    
    return result;
}

// MARK: -

void *cloudsync_get_auxdata (sqlite3_context *context) {
    cloudsync_context *data = (context) ? (cloudsync_context *)sqlite3_user_data(context) : NULL;
    return (data) ? data->aux_data : NULL;
}

void cloudsync_set_auxdata (sqlite3_context *context, void *xdata) {
    cloudsync_context *data = (context) ? (cloudsync_context *)sqlite3_user_data(context) : NULL;
    if (data) data->aux_data = xdata;
}

// MARK: - PK Context -

char *cloudsync_pk_context_tbl (cloudsync_pk_decode_bind_context *ctx, int64_t *tbl_len) {
    *tbl_len = ctx->tbl_len;
    return ctx->tbl;
}

void *cloudsync_pk_context_pk (cloudsync_pk_decode_bind_context *ctx, int64_t *pk_len) {
    *pk_len = ctx->pk_len;
    return (void *)ctx->pk;
}

char *cloudsync_pk_context_colname (cloudsync_pk_decode_bind_context *ctx, int64_t *colname_len) {
    *colname_len = ctx->col_name_len;
    return ctx->col_name;
}

int64_t cloudsync_pk_context_cl (cloudsync_pk_decode_bind_context *ctx) {
    return ctx->cl;
}

int64_t cloudsync_pk_context_dbversion (cloudsync_pk_decode_bind_context *ctx) {
    return ctx->db_version;
}

// MARK: - Table Utils -

char *table_build_values_sql (sqlite3 *db, cloudsync_table_context *table) {
    char *sql = NULL;
    
    /*
    This SQL statement dynamically generates a SELECT query for a specified table.
    It uses Common Table Expressions (CTEs) to construct the column names and
    primary key conditions based on the table schema, which is obtained through
    the `pragma_table_info` function.

    1. `col_names` CTE:
       - Retrieves a comma-separated list of non-primary key column names from
         the specified table's schema.

    2. `pk_where` CTE:
       - Retrieves a condition string representing the primary key columns in the
         format: "column1=? AND column2=? AND ...", used to create the WHERE clause
         for selecting rows based on primary key values.

    3. Final SELECT:
       - Constructs the complete SELECT statement as a string, combining:
         - Column names from `col_names`.
         - The target table name.
         - The WHERE clause conditions from `pk_where`.

    The resulting query can be used to select rows from the table based on primary
    key values, and can be executed within the application to retrieve data dynamically.
    */

    // Unfortunately in SQLite column names (or table names) cannot be bound parameters in a SELECT statement
    // otherwise we should have used something like SELECT 'SELECT ? FROM %w WHERE rowid=?';
    
    #if !CLOUDSYNC_DISABLE_ROWIDONLY_TABLES
    if (table->rowid_only) {
        sql = memory_mprintf("WITH col_names AS (SELECT group_concat(name, ',') AS cols FROM pragma_table_info('%w') WHERE pk=0 ORDER BY cid) SELECT 'SELECT ' || (SELECT cols FROM col_names) || ' FROM %w WHERE rowid=?;'", table->name, table->name);
        goto process_process;
    }
    #endif
    
    sql = cloudsync_memory_mprintf("WITH col_names AS (SELECT group_concat(name, ',') AS cols FROM pragma_table_info('%w') WHERE pk=0 ORDER BY cid), pk_where AS (SELECT group_concat(name, '=? AND ') || '=?' AS pk_clause FROM pragma_table_info('%w') WHERE pk>0 ORDER BY pk) SELECT 'SELECT ' || (SELECT cols FROM col_names) || ' FROM %w WHERE ' || (SELECT pk_clause FROM pk_where) || ';'", table->name, table->name, table->name);
 
#if !CLOUDSYNC_DISABLE_ROWIDONLY_TABLES
process_process:
#endif
    if (!sql) return NULL;
    char *query = dbutils_text_select(db, sql);
    cloudsync_memory_free(sql);
    
    return query;
}

char *table_build_mergedelete_sql (sqlite3 *db, cloudsync_table_context *table) {
    #if !CLOUDSYNC_DISABLE_ROWIDONLY_TABLES
    if (table->rowid_only) {
        char *sql = memory_mprintf("DELETE FROM %w WHERE rowid=?;", table->name);
        return sql;
    }
    #endif
    
    char *sql = cloudsync_memory_mprintf("WITH pk_where AS (SELECT group_concat(name, '=? AND ') || '=?' AS pk_clause FROM pragma_table_info('%w') WHERE pk>0 ORDER BY pk) SELECT 'DELETE FROM %w WHERE ' || (SELECT pk_clause FROM pk_where) || ';'", table->name, table->name);
    if (!sql) return NULL;
    
    char *query = dbutils_text_select(db, sql);
    cloudsync_memory_free(sql);
    
    return query;
}

char *table_build_mergeinsert_sql (sqlite3 *db, cloudsync_table_context *table, const char *colname) {
    char *sql = NULL;
    
    #if !CLOUDSYNC_DISABLE_ROWIDONLY_TABLES
    if (table->rowid_only) {
        if (colname == NULL) {
            // INSERT OR IGNORE INTO customers (first_name,last_name) VALUES (?,?);
            sql = memory_mprintf("INSERT OR IGNORE INTO %w (rowid) VALUES (?);", table->name);
        } else {
            // INSERT INTO customers (first_name,last_name,age) VALUES (?,?,?) ON CONFLICT DO UPDATE SET age=?;
            sql = memory_mprintf("INSERT INTO %w (rowid, %w) VALUES (?, ?) ON CONFLICT DO UPDATE SET %w=?;", table->name, colname, colname);
        }
        return sql;
    }
    #endif
    
    if (colname == NULL) {
        // is sentinel insert
        sql = cloudsync_memory_mprintf("WITH pk_where AS (SELECT group_concat(name) AS pk_clause FROM pragma_table_info('%w') WHERE pk>0 ORDER BY pk), pk_bind AS (SELECT group_concat('?') AS pk_binding FROM pragma_table_info('%w') WHERE pk>0 ORDER BY pk) SELECT 'INSERT OR IGNORE INTO %w (' || (SELECT pk_clause FROM pk_where) || ') VALUES ('  || (SELECT pk_binding FROM pk_bind) || ');'", table->name, table->name, table->name);
    } else {
        sql = cloudsync_memory_mprintf("WITH pk_where AS (SELECT group_concat(name) AS pk_clause FROM pragma_table_info('%w') WHERE pk>0 ORDER BY pk), pk_bind AS (SELECT group_concat('?') AS pk_binding FROM pragma_table_info('%w') WHERE pk>0 ORDER BY pk) SELECT 'INSERT INTO %w (' || (SELECT pk_clause FROM pk_where) || ',%w) VALUES ('  || (SELECT pk_binding FROM pk_bind) || ',?) ON CONFLICT DO UPDATE SET %w=?;'", table->name, table->name, table->name, colname, colname);
    }
    if (!sql) return NULL;
    
    char *query = dbutils_text_select(db, sql);
    cloudsync_memory_free(sql);
    
    return query;
}

char *table_build_value_sql (sqlite3 *db, cloudsync_table_context *table, const char *colname) {
    #if !CLOUDSYNC_DISABLE_ROWIDONLY_TABLES
    if (table->rowid_only) {
        char *sql = memory_mprintf("SELECT %w FROM %w WHERE rowid=?;", colname, table->name);
        return sql;
    }
    #endif
    
    // SELECT age FROM customers WHERE first_name=? AND last_name=?;
    char *sql = cloudsync_memory_mprintf("WITH pk_where AS (SELECT group_concat(name, '=? AND ') || '=?' AS pk_clause FROM pragma_table_info('%w') WHERE pk>0 ORDER BY pk) SELECT 'SELECT %w FROM %w WHERE ' || (SELECT pk_clause FROM pk_where) || ';'", table->name, colname, table->name);
    if (!sql) return NULL;
    
    char *query = dbutils_text_select(db, sql);
    cloudsync_memory_free(sql);
    
    return query;
}
    
cloudsync_table_context *table_create (const char *name, table_algo algo) {
    DEBUG_DBFUNCTION("table_create %s", name);
    
    cloudsync_table_context *table = (cloudsync_table_context *)cloudsync_memory_zeroalloc(sizeof(cloudsync_table_context));
    if (!table) return NULL;
    
    table->algo = algo;
    table->name = cloudsync_string_dup(name, true);
    if (!table->name) {
        cloudsync_memory_free(table);
        return NULL;
    }
    table->enabled = true;
        
    return table;
}

void table_free (cloudsync_table_context *table) {
    DEBUG_DBFUNCTION("table_free %s", (table) ? (table->name) : "NULL");
    if (!table) return;
    
    if (table->ncols > 0) {
        if (table->col_name) {
            for (int i=0; i<table->ncols; ++i) {
                cloudsync_memory_free(table->col_name[i]);
            }
            cloudsync_memory_free(table->col_name);
        }
        if (table->col_merge_stmt) {
            for (int i=0; i<table->ncols; ++i) {
                sqlite3_finalize(table->col_merge_stmt[i]);
            }
            cloudsync_memory_free(table->col_merge_stmt);
        }
        if (table->col_value_stmt) {
            for (int i=0; i<table->ncols; ++i) {
                sqlite3_finalize(table->col_value_stmt[i]);
            }
            cloudsync_memory_free(table->col_value_stmt);
        }
        if (table->col_id) {
            cloudsync_memory_free(table->col_id);
        }
    }
    
    if (table->pk_name) sqlite3_free_table(table->pk_name);
    if (table->name) cloudsync_memory_free(table->name);
    if (table->meta_pkexists_stmt) sqlite3_finalize(table->meta_pkexists_stmt);
    if (table->meta_sentinel_update_stmt) sqlite3_finalize(table->meta_sentinel_update_stmt);
    if (table->meta_sentinel_insert_stmt) sqlite3_finalize(table->meta_sentinel_insert_stmt);
    if (table->meta_row_insert_update_stmt) sqlite3_finalize(table->meta_row_insert_update_stmt);
    if (table->meta_row_drop_stmt) sqlite3_finalize(table->meta_row_drop_stmt);
    if (table->meta_update_move_stmt) sqlite3_finalize(table->meta_update_move_stmt);
    if (table->meta_local_cl_stmt) sqlite3_finalize(table->meta_local_cl_stmt);
    if (table->meta_winner_clock_stmt) sqlite3_finalize(table->meta_winner_clock_stmt);
    if (table->meta_merge_delete_drop) sqlite3_finalize(table->meta_merge_delete_drop);
    if (table->meta_zero_clock_stmt) sqlite3_finalize(table->meta_zero_clock_stmt);
    if (table->meta_col_version_stmt) sqlite3_finalize(table->meta_col_version_stmt);
    if (table->meta_site_id_stmt) sqlite3_finalize(table->meta_site_id_stmt);
    
    if (table->real_col_values_stmt) sqlite3_finalize(table->real_col_values_stmt);
    if (table->real_merge_delete_stmt) sqlite3_finalize(table->real_merge_delete_stmt);
    if (table->real_merge_sentinel_stmt) sqlite3_finalize(table->real_merge_sentinel_stmt);
    
    cloudsync_memory_free(table);
}

int table_add_stmts (sqlite3 *db, cloudsync_table_context *table, int ncols) {
    int rc = SQLITE_OK;
    char *sql = NULL;
    
    // META TABLE statements
    
    // CREATE TABLE IF NOT EXISTS \"%w_cloudsync\" (pk BLOB NOT NULL, col_name TEXT NOT NULL, col_version INTEGER, db_version INTEGER, site_id INTEGER DEFAULT 0, seq INTEGER, PRIMARY KEY (pk, col_name));
    
    // precompile the pk exists statement
    // we do not need an index on the pk column because it is already covered by the fact that it is part of the prikeys
    // EXPLAIN QUERY PLAN reports: SEARCH table_name USING PRIMARY KEY (pk=?)
    sql = cloudsync_memory_mprintf("SELECT EXISTS(SELECT 1 FROM \"%w_cloudsync\" WHERE pk = ? LIMIT 1);", table->name);
    if (!sql) {rc = SQLITE_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_pkexists_stmt: %s", sql);
    
    rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &table->meta_pkexists_stmt, NULL);
    
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto cleanup;
    
    // precompile the update local sentinel statement
    sql = cloudsync_memory_mprintf("UPDATE \"%w_cloudsync\" SET col_version = CASE col_version %% 2 WHEN 0 THEN col_version + 1 ELSE col_version + 2 END, db_version = ?, seq = ?, site_id = 0 WHERE pk = ? AND col_name = '%s';", table->name, CLOUDSYNC_TOMBSTONE_VALUE);
    if (!sql) {rc = SQLITE_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_sentinel_update_stmt: %s", sql);
    
    rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &table->meta_sentinel_update_stmt, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto cleanup;
    
    // precompile the insert local sentinel statement
    sql = cloudsync_memory_mprintf("INSERT INTO \"%w_cloudsync\" (pk, col_name, col_version, db_version, seq, site_id) SELECT ?, '%s', 1, ?, ?, 0 WHERE 1 ON CONFLICT DO UPDATE SET col_version = CASE col_version %% 2 WHEN 0 THEN col_version + 1 ELSE col_version + 2 END, db_version = ?, seq = ?, site_id = 0;", table->name, CLOUDSYNC_TOMBSTONE_VALUE);
    if (!sql) {rc = SQLITE_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_sentinel_insert_stmt: %s", sql);
    
    rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &table->meta_sentinel_insert_stmt, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto cleanup;

    // precompile the insert/update local row statement
    sql = cloudsync_memory_mprintf("INSERT INTO \"%w_cloudsync\" (pk, col_name, col_version, db_version, seq, site_id ) SELECT ?, ?, ?, ?, ?, 0 WHERE 1 ON CONFLICT DO UPDATE SET col_version = col_version + 1, db_version = ?, seq = ?, site_id = 0;", table->name);
    if (!sql) {rc = SQLITE_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_row_insert_update_stmt: %s", sql);
    
    rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &table->meta_row_insert_update_stmt, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto cleanup;
    
    // precompile the delete rows from meta
    sql = cloudsync_memory_mprintf("DELETE FROM \"%w_cloudsync\" WHERE pk=? AND col_name!='%s';", table->name, CLOUDSYNC_TOMBSTONE_VALUE);
    if (!sql) {rc = SQLITE_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_row_drop_stmt: %s", sql);
    
    rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &table->meta_row_drop_stmt, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto cleanup;
    
    // precompile the update rows from meta when pk changes
    // see https://github.com/sqliteai/sqlite-sync/blob/main/docs/PriKey.md for more details
    sql = cloudsync_memory_mprintf("UPDATE OR REPLACE \"%w_cloudsync\" SET pk=?, db_version=?, col_version=1, seq=cloudsync_seq(), site_id=0 WHERE (pk=? AND col_name!='%s');", table->name, CLOUDSYNC_TOMBSTONE_VALUE);
    if (!sql) {rc = SQLITE_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_update_move_stmt: %s", sql);
    
    rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &table->meta_update_move_stmt, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto cleanup;
    
    // local cl
    sql = cloudsync_memory_mprintf("SELECT COALESCE((SELECT col_version FROM \"%w_cloudsync\" WHERE pk=? AND col_name='%s'), (SELECT 1 FROM \"%w_cloudsync\" WHERE pk=?));", table->name, CLOUDSYNC_TOMBSTONE_VALUE, table->name);
    if (!sql) {rc = SQLITE_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_local_cl_stmt: %s", sql);
    
    rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &table->meta_local_cl_stmt, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto cleanup;
    
    // rowid of the last inserted/updated row in the meta table
    sql = cloudsync_memory_mprintf("INSERT OR REPLACE INTO \"%w_cloudsync\" (pk, col_name, col_version, db_version, seq, site_id) VALUES (?, ?, ?, cloudsync_db_version_next(?), ?, ?) RETURNING ((db_version << 30) | seq);", table->name);
    if (!sql) {rc = SQLITE_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_winner_clock_stmt: %s", sql);
    
    rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &table->meta_winner_clock_stmt, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto cleanup;
    
    sql = cloudsync_memory_mprintf("DELETE FROM \"%w_cloudsync\" WHERE pk=? AND col_name!='%s';", table->name, CLOUDSYNC_TOMBSTONE_VALUE);
    if (!sql) {rc = SQLITE_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_merge_delete_drop: %s", sql);
    
    rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &table->meta_merge_delete_drop, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto cleanup;
    
    // zero clock
    sql = cloudsync_memory_mprintf("UPDATE \"%w_cloudsync\" SET col_version = 0, db_version = cloudsync_db_version_next(?) WHERE pk=? AND col_name!='%s';", table->name, CLOUDSYNC_TOMBSTONE_VALUE);
    if (!sql) {rc = SQLITE_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_zero_clock_stmt: %s", sql);
    
    rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &table->meta_zero_clock_stmt, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto cleanup;
    
    // col_version
    sql = cloudsync_memory_mprintf("SELECT col_version FROM \"%w_cloudsync\" WHERE pk=? AND col_name=?;", table->name);
    if (!sql) {rc = SQLITE_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_col_version_stmt: %s", sql);
    
    rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &table->meta_col_version_stmt, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto cleanup;
    
    // site_id
    sql = cloudsync_memory_mprintf("SELECT site_id FROM \"%w_cloudsync\" WHERE pk=? AND col_name=?;", table->name);
    if (!sql) {rc = SQLITE_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_site_id_stmt: %s", sql);
    
    rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &table->meta_site_id_stmt, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto cleanup;
    
    // REAL TABLE statements
    
    // precompile the get column value statement
    if (ncols > 0) {
        sql = table_build_values_sql(db, table);
        if (!sql) {rc = SQLITE_NOMEM; goto cleanup;}
        DEBUG_SQL("real_col_values_stmt: %s", sql);
        
        rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &table->real_col_values_stmt, NULL);
        cloudsync_memory_free(sql);
        if (rc != SQLITE_OK) goto cleanup;
    }
    
    sql = table_build_mergedelete_sql(db, table);
    if (!sql) {rc = SQLITE_NOMEM; goto cleanup;}
    DEBUG_SQL("real_merge_delete: %s", sql);
    
    rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &table->real_merge_delete_stmt, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto cleanup;
    
    sql = table_build_mergeinsert_sql(db, table, NULL);
    if (!sql) {rc = SQLITE_NOMEM; goto cleanup;}
    DEBUG_SQL("real_merge_sentinel: %s", sql);
    
    rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &table->real_merge_sentinel_stmt, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto cleanup;
    
cleanup:
    if (rc != SQLITE_OK) printf("table_add_stmts error: %s\n", sqlite3_errmsg(db));
    return rc;
}

cloudsync_table_context *table_lookup (cloudsync_context *data, const char *table_name) {
    DEBUG_DBFUNCTION("table_lookup %s", table_name);
    
    for (int i=0; i<data->tables_count; ++i) {
        const char *name = (data->tables[i]) ? data->tables[i]->name : NULL;
        if ((name) && (strcasecmp(name, table_name) == 0)) {
            return data->tables[i];
        }
    }
    
    return NULL;
}

sqlite3_stmt *table_column_lookup (cloudsync_table_context *table, const char *col_name, bool is_merge, int *index) {
    DEBUG_DBFUNCTION("table_column_lookup %s", col_name);
    
    for (int i=0; i<table->ncols; ++i) {
        if (strcasecmp(table->col_name[i], col_name) == 0) {
            if (index) *index = i;
            return (is_merge) ? table->col_merge_stmt[i] : table->col_value_stmt[i];
        }
    }
    
    if (index) *index = -1;
    return NULL;
}

int table_remove (cloudsync_context *data, const char *table_name) {
    DEBUG_DBFUNCTION("table_remove %s", table_name);
    
    for (int i=0; i<data->tables_count; ++i) {
        const char *name = (data->tables[i]) ? data->tables[i]->name : NULL;
        if ((name) && (strcasecmp(name, table_name) == 0)) {
            data->tables[i] = NULL;
            return i;
        }
    }
    return -1;
}

int table_add_to_context_cb (void *xdata, int ncols, char **values, char **names) {
    cloudsync_table_context *table = (cloudsync_table_context *)xdata;
    
    sqlite3 *db = sqlite3_db_handle(table->meta_pkexists_stmt);
    if (!db) return SQLITE_ERROR;
    
    int index = table->ncols;
    for (int i=0; i<ncols; i+=2) {
        const char *name = values[i];
        int cid = (int)strtol(values[i+1], NULL, 0);
        
        table->col_id[index] = cid;
        table->col_name[index] = cloudsync_string_dup(name, true);
        if (!table->col_name[index]) return 1;
        
        char *sql = table_build_mergeinsert_sql(db, table, name);
        if (!sql) return SQLITE_NOMEM;
        DEBUG_SQL("col_merge_stmt[%d]: %s", index, sql);
        
        int rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &table->col_merge_stmt[index], NULL);
        cloudsync_memory_free(sql);
        if (rc != SQLITE_OK) return rc;
        if (!table->col_merge_stmt[index]) return SQLITE_MISUSE;
        
        sql = table_build_value_sql(db, table, name);
        if (!sql) return SQLITE_NOMEM;
        DEBUG_SQL("col_value_stmt[%d]: %s", index, sql);
        
        rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &table->col_value_stmt[index], NULL);
        cloudsync_memory_free(sql);
        if (rc != SQLITE_OK) return rc;
        if (!table->col_value_stmt[index]) return SQLITE_MISUSE;
    }
    table->ncols += 1;
    
    return 0;
}

bool table_add_to_context (sqlite3 *db, cloudsync_context *data, table_algo algo, const char *table_name) {
    DEBUG_DBFUNCTION("cloudsync_context_add_table %s", table_name);
    
    // check if table is already in the global context and in that case just return
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (table) return true;
    
    // is there any space available?
    if (data->tables_alloc <= data->tables_count + 1) {
        // realloc tables
        cloudsync_table_context **clone = (cloudsync_table_context **)cloudsync_memory_realloc(data->tables, sizeof(cloudsync_table_context) * data->tables_alloc + CLOUDSYNC_INIT_NTABLES);
        if (!clone) goto abort_add_table;
        
        // reset new entries
        for (int i=data->tables_alloc; i<data->tables_alloc + CLOUDSYNC_INIT_NTABLES; ++i) {
            clone[i] = NULL;
        }
        
        // replace old ptr
        data->tables = clone;
        data->tables_alloc += CLOUDSYNC_INIT_NTABLES;
    }
    
    // setup a new table context
    table = table_create(table_name, algo);
    if (!table) return false;
    
    // fill remaining metadata in the table
    char *sql = cloudsync_memory_mprintf("SELECT count(*) FROM pragma_table_info('%q') WHERE pk>0;", table_name);
    if (!sql) goto abort_add_table;
    table->npks = (int)dbutils_int_select(db, sql);
    cloudsync_memory_free(sql);
    if (table->npks == -1) {
        dbutils_context_result_error(data->sqlite_ctx, "%s", sqlite3_errmsg(db));
        goto abort_add_table;
    }
    
    if (table->npks == 0) {
        #if CLOUDSYNC_DISABLE_ROWIDONLY_TABLES
        return false;
        #else
        table->rowid_only = true;
        table->npks = 1; // rowid
        #endif
    }
    
    sql = cloudsync_memory_mprintf("SELECT count(*) FROM pragma_table_info('%q') WHERE pk=0;", table_name);
    if (!sql) goto abort_add_table;
    int64_t ncols = (int64_t)dbutils_int_select(db, sql);
    cloudsync_memory_free(sql);
    if (ncols == -1) {
        dbutils_context_result_error(data->sqlite_ctx, "%s", sqlite3_errmsg(db));
        goto abort_add_table;
    }
    
    int rc = table_add_stmts(db, table, (int)ncols);
    if (rc != SQLITE_OK) goto abort_add_table;
    
    // a table with only pk(s) is totally legal
    if (ncols > 0) {
        table->col_name = (char **)cloudsync_memory_alloc((sqlite3_uint64)(sizeof(char *) * ncols));
        if (!table->col_name) goto abort_add_table;
        
        table->col_id = (int *)cloudsync_memory_alloc((sqlite3_uint64)(sizeof(int) * ncols));
        if (!table->col_id) goto abort_add_table;
        
        table->col_merge_stmt = (sqlite3_stmt **)cloudsync_memory_alloc((sqlite3_uint64)(sizeof(sqlite3_stmt *) * ncols));
        if (!table->col_merge_stmt) goto abort_add_table;
        
        table->col_value_stmt = (sqlite3_stmt **)cloudsync_memory_alloc((sqlite3_uint64)(sizeof(sqlite3_stmt *) * ncols));
        if (!table->col_value_stmt) goto abort_add_table;
        
        sql = cloudsync_memory_mprintf("SELECT name, cid FROM pragma_table_info('%q') WHERE pk=0 ORDER BY cid;", table_name);
        if (!sql) goto abort_add_table;
        int rc = sqlite3_exec(db, sql, table_add_to_context_cb, (void *)table, NULL);
        cloudsync_memory_free(sql);
        if (rc == SQLITE_ABORT) goto abort_add_table;
    }
    
    // lookup the first free slot
    for (int i=0; i<data->tables_alloc; ++i) {
        if (data->tables[i] == NULL) {
            data->tables[i] = table;
            if (i > data->tables_count - 1) ++data->tables_count;
            break;
        }
    }
    
    return true;
    
abort_add_table:
    table_free(table);
    return false;
}

bool table_remove_from_context (cloudsync_context *data, cloudsync_table_context *table) {
    return (table_remove(data, table->name) != -1);
}

sqlite3_stmt *cloudsync_colvalue_stmt (sqlite3 *db, cloudsync_context *data, const char *tbl_name, bool *persistent) {
    sqlite3_stmt *vm = NULL;
    
    cloudsync_table_context *table = table_lookup(data, tbl_name);
    if (table) {
        char *col_name = NULL;
        if (table->ncols > 0) {
            col_name = table->col_name[0];
            // retrieve col_value precompiled statement
            vm = table_column_lookup(table, col_name, false, NULL);
            *persistent = true;
        } else {
            char *sql = table_build_value_sql(db, table, "*");
            sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
            cloudsync_memory_free(sql);
            *persistent = false;
        }
    }
    
    return vm;
}

// MARK: - Merge Insert -

sqlite3_int64 merge_get_local_cl (cloudsync_table_context *table, const char *pk, int pklen, const char **err) {
    sqlite3_stmt *vm = table->meta_local_cl_stmt;
    sqlite3_int64 result = -1;
    
    int rc = sqlite3_bind_blob(vm, 1, (const void *)pk, pklen, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_blob(vm, 2, (const void *)pk, pklen, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_ROW) result = sqlite3_column_int64(vm, 0);
    else if (rc == SQLITE_DONE) result = 0;
    
cleanup:
    if (result == -1) *err = sqlite3_errmsg(sqlite3_db_handle(vm));
    stmt_reset(vm);
    return result;
}

int merge_get_col_version (cloudsync_table_context *table, const char *col_name, const char *pk, int pklen, sqlite3_int64 *version, const char **err) {
    sqlite3_stmt *vm = table->meta_col_version_stmt;
    
    int rc = sqlite3_bind_blob(vm, 1, (const void *)pk, pklen, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_text(vm, 2, col_name, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_ROW) {
        *version = sqlite3_column_int64(vm, 0);
        rc = SQLITE_OK;
    }
    
cleanup:
    if ((rc != SQLITE_OK) && (rc != SQLITE_DONE)) *err = sqlite3_errmsg(sqlite3_db_handle(vm));
    stmt_reset(vm);
    return rc;
}

int merge_set_winner_clock (cloudsync_context *data, cloudsync_table_context *table, const char *pk, int pk_len, const char *colname, sqlite3_int64 col_version, sqlite3_int64 db_version, const char *site_id, int site_len, sqlite3_int64 seq, sqlite3_int64 *rowid, const char **err) {
    
    // get/set site_id
    sqlite3_stmt *vm = data->getset_siteid_stmt;
    int rc = sqlite3_bind_blob(vm, 1, (const void *)site_id, site_len, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup_merge;
    
    rc = sqlite3_step(vm);
    if (rc != SQLITE_ROW) goto cleanup_merge;
    
    int64_t ord = sqlite3_column_int64(vm, 0);
    stmt_reset(vm);
    
    vm = table->meta_winner_clock_stmt;
    rc = sqlite3_bind_blob(vm, 1, (const void *)pk, pk_len, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup_merge;
    
    rc = sqlite3_bind_text(vm, 2, (colname) ? colname : CLOUDSYNC_TOMBSTONE_VALUE, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup_merge;
    
    rc = sqlite3_bind_int64(vm, 3, col_version);
    if (rc != SQLITE_OK) goto cleanup_merge;
    
    rc = sqlite3_bind_int64(vm, 4, db_version);
    if (rc != SQLITE_OK) goto cleanup_merge;
    
    rc = sqlite3_bind_int64(vm, 5, seq);
    if (rc != SQLITE_OK) goto cleanup_merge;
    
    rc = sqlite3_bind_int64(vm, 6, ord);
    if (rc != SQLITE_OK) goto cleanup_merge;
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_ROW) {
        *rowid = sqlite3_column_int64(vm, 0);
        rc = SQLITE_OK;
    }
    
cleanup_merge:
    if (rc != SQLITE_OK) *err = sqlite3_errmsg(sqlite3_db_handle(vm));
    stmt_reset(vm);
    return rc;
}

int merge_insert_col (cloudsync_context *data, cloudsync_table_context *table, const char *pk, int pklen, const char *col_name, sqlite3_value *col_value, sqlite3_int64 col_version, sqlite3_int64 db_version, const char *site_id, int site_len, sqlite3_int64 seq, sqlite3_int64 *rowid, const char **err) {
    int index;
    sqlite3_stmt *vm = table_column_lookup(table, col_name, true, &index);
    if (vm == NULL) {
        *err = "Unable to retrieve column merge precompiled statement in merge_insert_col.";
        return SQLITE_MISUSE;
    }
    
    // INSERT INTO table (pk1, pk2, col_name) VALUES (?, ?, ?) ON CONFLICT DO UPDATE SET col_name=?;"
    
    // bind primary key(s)
    int rc = pk_decode_prikey((char *)pk, (size_t)pklen, pk_decode_bind_callback, vm);
    if (rc < 0) {
        *err = sqlite3_errmsg(sqlite3_db_handle(vm));
        rc = sqlite3_errcode(sqlite3_db_handle(vm));
        stmt_reset(vm);
        return rc;
    }
    
    // bind value
    if (col_value) {
        rc = sqlite3_bind_value(vm, table->npks+1, col_value);
        if (rc == SQLITE_OK) rc = sqlite3_bind_value(vm, table->npks+2, col_value);
        if (rc != SQLITE_OK) {
            *err = sqlite3_errmsg(sqlite3_db_handle(vm));
            stmt_reset(vm);
            return rc;
        }
        
    }
    
    // perform real operation and disable triggers
    
    // in case of GOS we reused the table->col_merge_stmt statement
    // which looks like: INSERT INTO table (pk1, pk2, col_name) VALUES (?, ?, ?) ON CONFLICT DO UPDATE SET col_name=?;"
    // but the UPDATE in the CONFLICT statement would return SQLITE_CONSTRAINT because the trigger raises the error
    // the trick is to disable that trigger before executing the statement
    if (table->algo == table_algo_crdt_gos) table->enabled = 0;
    SYNCBIT_SET(data);
    rc = sqlite3_step(vm);
    DEBUG_MERGE("merge_insert(%02x%02x): %s (%d)", data->site_id[UUID_LEN-2], data->site_id[UUID_LEN-1], sqlite3_expanded_sql(vm), rc);
    stmt_reset(vm);
    SYNCBIT_RESET(data);
    if (table->algo == table_algo_crdt_gos) table->enabled = 1;
    
    if (rc != SQLITE_DONE) {
        *err = sqlite3_errmsg(sqlite3_db_handle(vm));
        return rc;
    }
    
    return merge_set_winner_clock(data, table, pk, pklen, col_name, col_version, db_version, site_id, site_len, seq, rowid, err);
}

int merge_delete (cloudsync_context *data, cloudsync_table_context *table, const char *pk, int pklen, const char *colname, sqlite3_int64 cl, sqlite3_int64 db_version, const char *site_id, int site_len, sqlite3_int64 seq, sqlite3_int64 *rowid, const char **err) {
    int rc = SQLITE_OK;
    
    // reset return value
    *rowid = 0;
    
    // bind pk
    sqlite3_stmt *vm = table->real_merge_delete_stmt;
    rc = pk_decode_prikey((char *)pk, (size_t)pklen, pk_decode_bind_callback, vm);
    if (rc < 0) {
        *err = sqlite3_errmsg(sqlite3_db_handle(vm));
        rc = sqlite3_errcode(sqlite3_db_handle(vm));
        stmt_reset(vm);
        return rc;
    }
    
    // perform real operation and disable triggers
    SYNCBIT_SET(data);
    rc = sqlite3_step(vm);
    DEBUG_MERGE("merge_delete(%02x%02x): %s (%d)", data->site_id[UUID_LEN-2], data->site_id[UUID_LEN-1], sqlite3_expanded_sql(vm), rc);
    stmt_reset(vm);
    SYNCBIT_RESET(data);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    if (rc != SQLITE_OK) {
        *err = sqlite3_errmsg(sqlite3_db_handle(vm));
        return rc;
    }
    
    rc = merge_set_winner_clock(data, table, pk, pklen, colname, cl, db_version, site_id, site_len, seq, rowid, err);
    if (rc != SQLITE_OK) return rc;
    
    // drop clocks _after_ setting the winner clock so we don't lose track of the max db_version!!
    // this must never come before `set_winner_clock`
    vm = table->meta_merge_delete_drop;
    rc = sqlite3_bind_blob(vm, 1, (const void *)pk, pklen, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(vm);
    stmt_reset(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    if (rc != SQLITE_OK) {
        *err = sqlite3_errmsg(sqlite3_db_handle(vm));
    }
    
    return rc;
}

int merge_zeroclock_on_resurrect(cloudsync_table_context *table, sqlite3_int64 db_version, const char *pk, int pklen, const char **err) {
    sqlite3_stmt *vm = table->meta_zero_clock_stmt;
    
    int rc = sqlite3_bind_int64(vm, 1, db_version);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_blob(vm, 2, (const void *)pk, pklen, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    
cleanup:
    if (rc != SQLITE_OK) *err = sqlite3_errmsg(sqlite3_db_handle(vm));
    stmt_reset(vm);
    return rc;
}

// executed only if insert_cl == local_cl
int merge_did_cid_win (cloudsync_context *data, cloudsync_table_context *table, const char *pk, int pklen, sqlite3_value *insert_value, const char *site_id, int site_len, const char *col_name, sqlite3_int64 col_version, bool *didwin_flag, const char **err) {
    
    if (col_name == NULL) col_name = CLOUDSYNC_TOMBSTONE_VALUE;
    
    sqlite3_int64 local_version;
    int rc = merge_get_col_version(table, col_name, pk, pklen, &local_version, err);
    if (rc == SQLITE_DONE) {
        // no rows returned, the incoming change wins if there's nothing there locally
        *didwin_flag = true;
        return SQLITE_OK;
    }
    if (rc != SQLITE_OK) return rc;
    
    // rc == SQLITE_OK, means that a row with a version exists
    if (local_version != col_version) {
        if (col_version > local_version) {*didwin_flag = true; return SQLITE_OK;}
        if (col_version < local_version) {*didwin_flag = false; return SQLITE_OK;}
    }
    
    // rc == SQLITE_ROW and col_version == local_version, need to compare values
    
    // retrieve col_value precompiled statement
    sqlite3_stmt *vm = table_column_lookup(table, col_name, false, NULL);
    if (!vm) {
        *err = "Unable to retrieve column value precompiled statement in merge_did_cid_win.";
        return SQLITE_ERROR;
    }
    
    // bind primary key values
    rc = pk_decode_prikey((char *)pk, (size_t)pklen, pk_decode_bind_callback, (void *)vm);
    if (rc < 0) {
        *err = sqlite3_errmsg(sqlite3_db_handle(vm));
        rc = sqlite3_errcode(sqlite3_db_handle(vm));
        stmt_reset(vm);
        return rc;
    }
        
    // execute vm
    sqlite3_value *local_value;
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) {
        // meta entry exists but the actual value is missing
        // we should allow the value_compare function to make a decision
        // value_compare has been modified to handle the case where lvalue is NULL
        local_value = NULL;
        rc = SQLITE_OK;
    } else if (rc == SQLITE_ROW) {
        local_value = sqlite3_column_value(vm, 0);
        rc = SQLITE_OK;
    } else {
        goto cleanup;
    }
    
    // compare values
    int ret = dbutils_value_compare(insert_value, local_value);
    // reset after compare, otherwise local value would be deallocated
    vm = stmt_reset(vm);
    
    bool compare_site_id = (ret == 0 && data->merge_equal_values == true);
    if (!compare_site_id) {
        *didwin_flag = (ret > 0);
        goto cleanup;
    }
    
    // values are the same and merge_equal_values is true
    vm = table->meta_site_id_stmt;
    rc = sqlite3_bind_blob(vm, 1, (const void *)pk, pklen, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_text(vm, 2, col_name, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_ROW) {
        const void *local_site_id = sqlite3_column_blob(vm, 0);
        ret = memcmp(site_id, local_site_id, site_len);
        *didwin_flag = (ret > 0);
        stmt_reset(vm);
        return SQLITE_OK;
    }
    
    // handle error condition here
    stmt_reset(vm);
    *err = "Unable to find site_id for previous change. The cloudsync table is probably corrupted.";
    return SQLITE_ERROR;
    
cleanup:
    if (rc != SQLITE_OK) *err = sqlite3_errmsg(sqlite3_db_handle(vm));
    if (vm) stmt_reset(vm);
    return rc;
}

int merge_sentinel_only_insert (cloudsync_context *data, cloudsync_table_context *table, const char *pk, int pklen, sqlite3_int64 cl, sqlite3_int64 db_version, const char *site_id, int site_len, sqlite3_int64 seq, sqlite3_int64 *rowid, const char **err) {
    
    // reset return value
    *rowid = 0;
    
    // bind pk
    sqlite3_stmt *vm = table->real_merge_sentinel_stmt;
    int rc = pk_decode_prikey((char *)pk, (size_t)pklen, pk_decode_bind_callback, vm);
    if (rc < 0) {
        *err = sqlite3_errmsg(sqlite3_db_handle(vm));
        rc = sqlite3_errcode(sqlite3_db_handle(vm));
        stmt_reset(vm);
        return rc;
    }
    
    // perform real operation and disable triggers
    SYNCBIT_SET(data);
    rc = sqlite3_step(vm);
    stmt_reset(vm);
    SYNCBIT_RESET(data);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    if (rc != SQLITE_OK) {
        *err = sqlite3_errmsg(sqlite3_db_handle(vm));
        return rc;
    }
    
    rc = merge_zeroclock_on_resurrect(table, db_version, pk, pklen, err);
    if (rc != SQLITE_OK) return rc;
    
    return merge_set_winner_clock(data, table, pk, pklen, NULL, cl, db_version, site_id, site_len, seq, rowid, err);
}

int cloudsync_merge_insert_gos (sqlite3_vtab *vtab, cloudsync_context *data, cloudsync_table_context *table, const char *insert_pk, int insert_pk_len, const char *insert_name, sqlite3_value *insert_value, sqlite3_int64 insert_col_version, sqlite3_int64 insert_db_version, const char *insert_site_id, int insert_site_id_len, sqlite3_int64 insert_seq, sqlite3_int64 *rowid) {
    // Grow-Only Set (GOS) Algorithm: Only insertions are allowed, deletions and updates are prevented from a trigger.
    
    const char *err = NULL;
    int rc = merge_insert_col(data, table, insert_pk, insert_pk_len, insert_name, insert_value, insert_col_version, insert_db_version,
                              insert_site_id, insert_site_id_len, insert_seq, rowid, &err);
    if (rc != SQLITE_OK) {
        cloudsync_vtab_set_error(vtab, "Unable to perform GOS merge_insert_col: %s", err);
    }
    
    return rc;
}

int cloudsync_merge_insert (sqlite3_vtab *vtab, int argc, sqlite3_value **argv, sqlite3_int64 *rowid) {
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
    cloudsync_context *data = cloudsync_vtab_get_context(vtab);
    cloudsync_table_context *table = table_lookup(data, insert_tbl);
    if (!table) return cloudsync_vtab_set_error(vtab, "Unable to find table %s,", insert_tbl);
    
    // extract the remaining fields from the input values
    const char *insert_pk = (const char *)sqlite3_value_blob(argv[1]);
    int insert_pk_len = sqlite3_value_bytes(argv[1]);
    const char *insert_name = (sqlite3_value_type(argv[2]) == SQLITE_NULL) ? CLOUDSYNC_TOMBSTONE_VALUE : (const char *)sqlite3_value_text(argv[2]);
    sqlite3_value *insert_value = argv[3];
    sqlite3_int64 insert_col_version = sqlite3_value_int64(argv[4]);
    sqlite3_int64 insert_db_version = sqlite3_value_int64(argv[5]);
    const char *insert_site_id = (const char *)sqlite3_value_blob(argv[6]);
    int insert_site_id_len = sqlite3_value_bytes(argv[6]);
    sqlite3_int64 insert_cl = sqlite3_value_int64(argv[7]);
    sqlite3_int64 insert_seq = sqlite3_value_int64(argv[8]);
    const char *err = NULL;
    
    // perform different logic for each different table algorithm
    if (table->algo == table_algo_crdt_gos) return cloudsync_merge_insert_gos(vtab, data, table, insert_pk, insert_pk_len, insert_name, insert_value, insert_col_version, insert_db_version, insert_site_id, insert_site_id_len, insert_seq, rowid);
    
    // Handle DWS and AWS algorithms here
    // Delete-Wins Set (DWS): table_algo_crdt_dws
    // Add-Wins Set (AWS): table_algo_crdt_aws
    
    // Causal-Length Set (CLS) Algorithm (default)
    
    // compute the local causal length for the row based on the primary key
    // the causal length is used to determine the order of operations and resolve conflicts.
    sqlite3_int64 local_cl = merge_get_local_cl(table, insert_pk, insert_pk_len, &err);
    if (local_cl < 0) {
        return cloudsync_vtab_set_error(vtab, "Unable to compute local causal length: %s", err);
    }
    
    // if the incoming causal length is older than the local causal length, we can safely ignore it
    // because the local changes are more recent
    if (insert_cl < local_cl) return SQLITE_OK;
    
    // check if the operation is a delete by examining the causal length
    // even causal lengths typically signify delete operations
    bool is_delete = (insert_cl % 2 == 0);
    if (is_delete) {
        // if it's a delete, check if the local state is at the same causal length
        // if it is, no further action is needed
        if (local_cl == insert_cl) return SQLITE_OK;
        
        // perform a delete merge if the causal length is newer than the local one
        int rc = merge_delete(data, table, insert_pk, insert_pk_len, insert_name, insert_col_version,
                              insert_db_version, insert_site_id, insert_site_id_len, insert_seq, rowid, &err);
        if (rc != SQLITE_OK) cloudsync_vtab_set_error(vtab, "Unable to perform merge_delete: %s", err);
        return rc;
    }
    
    // if the operation is a sentinel-only insert (indicating a new row or resurrected row with no column update), handle it separately.
    bool is_sentinel_only = (strcmp(insert_name, CLOUDSYNC_TOMBSTONE_VALUE) == 0);
    if (is_sentinel_only) {
        if (local_cl == insert_cl) return SQLITE_OK;
        
        // perform a sentinel-only insert to track the existence of the row
        int rc = merge_sentinel_only_insert(data, table, insert_pk, insert_pk_len, insert_col_version,
                                            insert_db_version, insert_site_id, insert_site_id_len, insert_seq, rowid, &err);
        if (rc != SQLITE_OK) cloudsync_vtab_set_error(vtab, "Unable to perform merge_sentinel_only_insert: %s", err);
        return rc;
    }
    
    // from this point I can be sure that insert_name is not sentinel
    
    // handle the case where a row is being resurrected (e.g., after a delete, a new insert for the same row)
    // odd causal lengths can "resurrect" rows
    bool needs_resurrect = (insert_cl > local_cl && insert_cl % 2 == 1);
    bool row_exists_locally = local_cl != 0;
   
    // if a resurrection is needed, insert a sentinel to mark the row as alive
    // this handles out-of-order deliveries where the row was deleted and is now being re-inserted
    if (needs_resurrect && (row_exists_locally || (!row_exists_locally && insert_cl > 1))) {
        int rc = merge_sentinel_only_insert(data, table, insert_pk, insert_pk_len, insert_cl,
                                            insert_db_version, insert_site_id, insert_site_id_len, insert_seq, rowid, &err);
        if (rc != SQLITE_OK) {
            cloudsync_vtab_set_error(vtab, "Unable to perform merge_sentinel_only_insert: %s", err);
            return rc;
        }
    }
    
    // at this point, we determine whether the incoming change wins based on causal length
    // this can be due to a resurrection, a non-existent local row, or a conflict resolution
    bool flag = false;
    int rc = merge_did_cid_win(data, table, insert_pk, insert_pk_len, insert_value, insert_site_id, insert_site_id_len, insert_name, insert_col_version, &flag, &err);
    if (rc != SQLITE_OK) {
        cloudsync_vtab_set_error(vtab, "Unable to perform merge_did_cid_win: %s", err);
        return rc;
    }
    
    // check if the incoming change wins and should be applied
    bool does_cid_win = ((needs_resurrect) || (!row_exists_locally) || (flag));
    if (!does_cid_win) return SQLITE_OK;
    
    // perform the final column insert or update if the incoming change wins
    rc = merge_insert_col(data, table, insert_pk, insert_pk_len, insert_name, insert_value, insert_col_version, insert_db_version, insert_site_id, insert_site_id_len, insert_seq, rowid, &err);
    if (rc != SQLITE_OK) cloudsync_vtab_set_error(vtab, "Unable to perform merge_insert_col: %s", err);
    return rc;
}

// MARK: - Private -

bool cloudsync_config_exists (sqlite3 *db) {
    return dbutils_table_exists(db, CLOUDSYNC_SITEID_NAME) == true;
}

void *cloudsync_context_create (void) {
    cloudsync_context *data = (cloudsync_context *)cloudsync_memory_zeroalloc((uint64_t)(sizeof(cloudsync_context)));
    DEBUG_SETTINGS("cloudsync_context_create %p", data);
    
    data->libversion = CLOUDSYNC_VERSION;
    #if CLOUDSYNC_DEBUG
    data->debug = 1;
    #endif
    
    // allocate space for 128 tables (it can grow if needed)
    data->tables = (cloudsync_table_context **)cloudsync_memory_zeroalloc((uint64_t)(CLOUDSYNC_INIT_NTABLES * sizeof(cloudsync_table_context *)));
    if (!data->tables) {
        cloudsync_memory_free(data);
        return NULL;
    }
    data->tables_alloc = CLOUDSYNC_INIT_NTABLES;
    data->tables_count = 0;
        
    return data;
}

void cloudsync_context_free (void *ptr) {
    DEBUG_SETTINGS("cloudsync_context_free %p", ptr);
    if (!ptr) return;
        
    cloudsync_context *data = (cloudsync_context*)ptr;
    cloudsync_memory_free(data->tables);
    cloudsync_memory_free(data);
    cloudsync_memory_finalize();
}

const char *cloudsync_context_init (sqlite3 *db, cloudsync_context *data, sqlite3_context *context) {
    if (!data && context) data = (cloudsync_context *)sqlite3_user_data(context);

    // perform init just the first time, if the site_id field is not set.
    // The data->site_id value could exists while settings tables don't exists if the
    // cloudsync_context_init was previously called in init transaction that was rolled back
    // because of an error during the init process.
    if (data->site_id[0] == 0 || !dbutils_table_exists(db, CLOUDSYNC_SITEID_NAME)) {
        if (dbutils_settings_init(db, data, context) != SQLITE_OK) return NULL;
        if (stmts_add_tocontext(db, data) != SQLITE_OK) return NULL;
        if (cloudsync_load_siteid(db, data) != SQLITE_OK) return NULL;
        
        data->sqlite_ctx = context;
        data->schema_hash = dbutils_schema_hash(db);
    }
    
    return (const char *)data->site_id;
}

void cloudsync_sync_key(cloudsync_context *data, const char *key, const char *value) {
    DEBUG_SETTINGS("cloudsync_sync_key key: %s value: %s", key, value);
    
    // sync data
    if (strcmp(key, CLOUDSYNC_KEY_SCHEMAVERSION) == 0) {
        data->schema_version = (int)strtol(value, NULL, 0);
        return;
    }
    
    if (strcmp(key, CLOUDSYNC_KEY_DEBUG) == 0) {
        data->debug = 0;
        if (value && (value[0] != 0) && (value[0] != '0')) data->debug = 1;
        return;
    }
}

#if 0
void cloudsync_sync_table_key(cloudsync_context *data, const char *table, const char *column, const char *key, const char *value) {
    DEBUG_SETTINGS("cloudsync_sync_table_key table: %s column: %s key: %s value: %s", table, column, key, value);
    // Unused in this version
    return;
}
#endif

int cloudsync_commit_hook (void *ctx) {
    cloudsync_context *data = (cloudsync_context *)ctx;
    
    data->db_version = data->pending_db_version;
    data->pending_db_version = CLOUDSYNC_VALUE_NOTSET;
    data->seq = 0;
    
    return SQLITE_OK;
}

void cloudsync_rollback_hook (void *ctx) {
    cloudsync_context *data = (cloudsync_context *)ctx;
    
    data->pending_db_version = CLOUDSYNC_VALUE_NOTSET;
    data->seq = 0;
}

int cloudsync_finalize_alter (sqlite3_context *context, cloudsync_context *data, cloudsync_table_context *table) {
    int rc = SQLITE_OK;
    sqlite3 *db = sqlite3_context_db_handle(context);

    db_version_check_uptodate(db, data);

    // If primary key columns change (in the schema)
    // We need to drop, re-create and backfill
    // the clock table.
    // A change in pk columns means a change in all identities
    // of all rows.
    // We can determine this by comparing unique index on lookaside table vs
    // pks on source table
    char *errmsg = NULL;
    char **result = NULL;
    int nrows, ncols;
    char *sql = cloudsync_memory_mprintf("SELECT name FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk;", table->name);
    rc = sqlite3_get_table(db, sql, &result, &nrows, &ncols, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) {
        DEBUG_SQLITE_ERROR(rc, "cloudsync_finalize_alter", db);
        goto finalize;
    } else if (errmsg || ncols != 1) {
        rc = SQLITE_MISUSE;
        goto finalize;
    }
    
    bool pk_diff = false;
    if (nrows != table->npks) {
        pk_diff = true;
    } else {
        for (int i=0; i<nrows; ++i) {
            if (strcmp(table->pk_name[i], result[i]) != 0) {
                pk_diff = true;
                break;
            }
        }
    }
    
    if (pk_diff) {
        // drop meta-table, it will be recreated
        char *sql = cloudsync_memory_mprintf("DROP TABLE IF EXISTS \"%w_cloudsync\";", table->name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        cloudsync_memory_free(sql);
        if (rc != SQLITE_OK) {
            DEBUG_SQLITE_ERROR(rc, "cloudsync_finalize_alter", db);
            goto finalize;
        }
    } else {
        // compact meta-table
        // delete entries for removed columns
        char *sql = cloudsync_memory_mprintf("DELETE FROM \"%w_cloudsync\" WHERE \"col_name\" NOT IN ("
                                             "SELECT name FROM pragma_table_info('%w') UNION SELECT '%s'"
                                             ")", table->name, table->name, CLOUDSYNC_TOMBSTONE_VALUE);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        cloudsync_memory_free(sql);
        if (rc != SQLITE_OK) {
            DEBUG_SQLITE_ERROR(rc, "cloudsync_finalize_alter", db);
            goto finalize;
        }
        
        sql = cloudsync_memory_mprintf("SELECT group_concat('%w.' || name, ',') FROM pragma_table_info('%w') WHERE pk>0 ORDER BY pk;", table->name, table->name);
        if (!sql) {
            rc = SQLITE_NOMEM;
            goto finalize;
        }
        char *pkclause = dbutils_text_select(db, sql);
        char *pkvalues = (pkclause) ? pkclause : "rowid";
        cloudsync_memory_free(sql);
        
        // delete entries related to rows that no longer exist in the original table, but preserve tombstone
        sql = cloudsync_memory_mprintf("DELETE FROM \"%w_cloudsync\" WHERE (\"col_name\" != '%s' OR (\"col_name\" = '%s' AND col_version %% 2 != 0)) AND NOT EXISTS (SELECT 1 FROM \"%w\" WHERE \"%w_cloudsync\".pk = cloudsync_pk_encode(%w) LIMIT 1);", table->name, CLOUDSYNC_TOMBSTONE_VALUE, CLOUDSYNC_TOMBSTONE_VALUE, table->name, table->name, pkvalues);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        if (pkclause) cloudsync_memory_free(pkclause);
        cloudsync_memory_free(sql);
        if (rc != SQLITE_OK) {
            DEBUG_SQLITE_ERROR(rc, "cloudsync_finalize_alter", db);
            goto finalize;
        }

    }
    
    char buf[256];
    snprintf(buf, sizeof(buf), "%lld", data->db_version);
    dbutils_settings_set_key_value(db, context, "pre_alter_dbversion", buf);
    
finalize:
    sqlite3_free_table(result);
    sqlite3_free(errmsg);
    
    return rc;
}

int cloudsync_refill_metatable (sqlite3 *db, cloudsync_context *data, const char *table_name) {
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) return SQLITE_INTERNAL;
    
    sqlite3_stmt *vm = NULL;
    sqlite3_int64 db_version = db_version_next(db, data, CLOUDSYNC_VALUE_NOTSET);

    char *sql = cloudsync_memory_mprintf("SELECT group_concat(name, ',') FROM pragma_table_info('%w') WHERE pk>0 ORDER BY pk;", table_name);
    char *pkclause = dbutils_text_select(db, sql);
    char *pkvalues = (pkclause) ? pkclause : "rowid";
    cloudsync_memory_free(sql);
    
    sql = cloudsync_memory_mprintf("SELECT group_concat('cloudsync_pk_decode(pk, ' || pk || ') AS ' || name, ',') FROM pragma_table_info('%w') WHERE pk>0 ORDER BY pk;", table_name);
    char *pkdecode = dbutils_text_select(db, sql);
    char *pkdecodeval = (pkdecode) ? pkdecode : "cloudsync_pk_decode(pk, 1) AS rowid";
    cloudsync_memory_free(sql);
    
    sql = cloudsync_memory_mprintf("SELECT group_concat(name || ' = cloudsync_pk_decode(pk, ' || pk || ')', ' AND ') FROM pragma_table_info('%w') WHERE pk>0 ORDER BY pk;", table_name);
    char *pkonclause = dbutils_text_select(db, sql);
    char *pkonclauseval = (pkonclause) ? pkonclause : "rowid = cloudsync_pk_decode(pk, 1) AS rowid";
    cloudsync_memory_free(sql);
    
    sql = cloudsync_memory_mprintf("SELECT cloudsync_insert('%w', %w) FROM (SELECT %w FROM \"%w\" EXCEPT SELECT %s FROM \"%w_cloudsync\");", table_name, pkvalues, pkvalues, table_name, pkdecodeval, table_name);
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto finalize;

    // fill missing colums
    // for each non-pk column:
    
    sql = cloudsync_memory_mprintf("SELECT cloudsync_pk_encode(%w) FROM \"%w\" LEFT JOIN \"%w_cloudsync\" ON %s AND \"%w_cloudsync\".col_name = ? WHERE \"%w_cloudsync\".db_version IS NULL", pkvalues, table_name, table_name, pkonclauseval, table_name, table_name);
    rc = sqlite3_prepare(db, sql, -1, &vm, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    for (int i=0; i<table->ncols; ++i) {
        char *col_name = table->col_name[i];

        rc = sqlite3_bind_text(vm, 1, col_name, -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto finalize;
        
        while (1) {
            rc = sqlite3_step(vm);
            if (rc == SQLITE_ROW) {
                const char *pk = (const char *)sqlite3_column_text(vm, 0);
                size_t pklen = strlen(pk);
                rc = local_mark_insert_or_update_meta(db, table, pk, pklen, col_name, db_version, BUMP_SEQ(data));
            } else if (rc == SQLITE_DONE) {
                rc = SQLITE_OK;
                break;
            } else {
                break;
            }
        }
        if (rc != SQLITE_OK) goto finalize;

        sqlite3_reset(vm);
    }
    
finalize:
    if (rc != SQLITE_OK) DEBUG_ALWAYS("cloudsync_refill_metatable error: %s", sqlite3_errmsg(db));
    if (pkclause) cloudsync_memory_free(pkclause);
    if (pkdecode) cloudsync_memory_free(pkdecode);
    if (pkonclause) cloudsync_memory_free(pkonclause);
    if (vm) sqlite3_finalize(vm);
    return rc;
}

// MARK: - Local -

int local_update_sentinel (sqlite3 *db, cloudsync_table_context *table, const char *pk, size_t pklen, sqlite3_int64 db_version, int seq) {
    sqlite3_stmt *vm = table->meta_sentinel_update_stmt;
    if (!vm) return -1;
    
    int rc = sqlite3_bind_int64(vm, 1, db_version);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int(vm, 2, seq);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_blob(vm, 3, pk, (int)pklen, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    
cleanup:
    DEBUG_SQLITE_ERROR(rc, "local_update_sentinel", db);
    sqlite3_reset(vm);
    return rc;
}

int local_mark_insert_sentinel_meta (sqlite3 *db, cloudsync_table_context *table, const char *pk, size_t pklen, sqlite3_int64 db_version, int seq) {
    sqlite3_stmt *vm = table->meta_sentinel_insert_stmt;
    if (!vm) return -1;
    
    int rc = sqlite3_bind_blob(vm, 1, pk, (int)pklen, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int64(vm, 2, db_version);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int(vm, 3, seq);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int64(vm, 4, db_version);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int(vm, 5, seq);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    
cleanup:
    DEBUG_SQLITE_ERROR(rc, "local_insert_sentinel", db);
    sqlite3_reset(vm);
    return rc;
}

int local_mark_insert_or_update_meta_impl (sqlite3 *db, cloudsync_table_context *table, const char *pk, size_t pklen, const char *col_name, int col_version, sqlite3_int64 db_version, int seq) {
    
    sqlite3_stmt *vm = table->meta_row_insert_update_stmt;
    if (!vm) return -1;
    
    int rc = sqlite3_bind_blob(vm, 1, pk, (int)pklen, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_text(vm, 2, (col_name) ? col_name : CLOUDSYNC_TOMBSTONE_VALUE, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int(vm, 3, col_version);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int64(vm, 4, db_version);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int(vm, 5, seq);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int64(vm, 6, db_version);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int(vm, 7, seq);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    
cleanup:
    DEBUG_SQLITE_ERROR(rc, "local_insert_or_update", db);
    sqlite3_reset(vm);
    return rc;
}

int local_mark_insert_or_update_meta (sqlite3 *db, cloudsync_table_context *table, const char *pk, size_t pklen, const char *col_name, sqlite3_int64 db_version, int seq) {
    return local_mark_insert_or_update_meta_impl(db, table, pk, pklen, col_name, 1, db_version, seq);
}

int local_mark_delete_meta (sqlite3 *db, cloudsync_table_context *table, const char *pk, size_t pklen, sqlite3_int64 db_version, int seq) {
    return local_mark_insert_or_update_meta_impl(db, table, pk, pklen, NULL, 2, db_version, seq);
}

int local_drop_meta (sqlite3 *db, cloudsync_table_context *table, const char *pk, size_t pklen) {
    sqlite3_stmt *vm = table->meta_row_drop_stmt;
    if (!vm) return -1;
    
    int rc = sqlite3_bind_blob(vm, 1, pk, (int)pklen, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    
cleanup:
    DEBUG_SQLITE_ERROR(rc, "local_drop_meta", db);
    sqlite3_reset(vm);
    return rc;
}

int local_update_move_meta (sqlite3 *db, cloudsync_table_context *table, const char *pk, size_t pklen, const char *pk2, size_t pklen2, sqlite3_int64 db_version) {
    /*
      * This function moves non-sentinel metadata entries from an old primary key (OLD.pk)
      * to a new primary key (NEW.pk) when a primary key change occurs.
      *
      * To ensure consistency and proper conflict resolution in a CRDT (Conflict-free Replicated Data Type) system,
      * each non-sentinel metadata entry involved in the move must have a unique sequence value (seq).
      *
      * The `seq` is crucial for tracking the order of operations and for detecting and resolving conflicts
      * during synchronization between replicas. Without a unique `seq` for each entry, concurrent updates
      * may be applied incorrectly, leading to data inconsistency.
      *
      * When performing the update, a unique `seq` must be assigned to each metadata row. This can be achieved
      * by either incrementing the maximum sequence value in the table or using a function (e.g., `bump_seq(data)`)
      * that generates a unique sequence for each row. The update query should ensure that each row moved
      * from OLD.pk to NEW.pk gets a distinct `seq` to maintain proper versioning and ordering of changes.
     */
    
    // see https://github.com/sqliteai/sqlite-sync/blob/main/docs/PriKey.md for more details
    // pk2 is the old pk
    
    sqlite3_stmt *vm = table->meta_update_move_stmt;
    if (!vm) return -1;
    
    // new primary key
    int rc = sqlite3_bind_blob(vm, 1, pk, (int)pklen, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    // new db_version
    rc = sqlite3_bind_int64(vm, 2, db_version);
    if (rc != SQLITE_OK) goto cleanup;
    
    // old primary key
    rc = sqlite3_bind_blob(vm, 3, pk2, (int)pklen2, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    
cleanup:
    DEBUG_SQLITE_ERROR(rc, "local_update_move_meta", db);
    sqlite3_reset(vm);
    return rc;
}

// MARK: - Payload Encode / Decode -

bool cloudsync_buffer_free (cloudsync_network_payload *payload) {
    if (payload) {
        if (payload->buffer) cloudsync_memory_free(payload->buffer);
        memset(payload, 0, sizeof(cloudsync_network_payload));
    }
        
    return false;
}

bool cloudsync_buffer_check (cloudsync_network_payload *payload, size_t needed) {
    // alloc/resize buffer
    if (payload->bused + needed > payload->balloc) {
        if (needed < CLOUDSYNC_PAYLOAD_MINBUF_SIZE) needed = CLOUDSYNC_PAYLOAD_MINBUF_SIZE;
        size_t balloc = (payload->balloc * 2) + needed;
        
        char *buffer = cloudsync_memory_alloc(balloc);
        if (!buffer) return cloudsync_buffer_free(payload);
        
        if (payload->buffer) cloudsync_memory_free(payload->buffer);
        payload->buffer = buffer;
        payload->balloc = balloc;
        if (payload->nrows == 0) payload->bused = sizeof(cloudsync_network_header);
    }
    
    return true;
}

void cloudsync_network_header_init (cloudsync_network_header *header, uint32_t expanded_size, uint16_t ncols, uint32_t nrows, uint64_t hash) {
    memset(header, 0, sizeof(cloudsync_network_header));
    assert(sizeof(cloudsync_network_header)==32);
    
    int major, minor, patch;
    sscanf(CLOUDSYNC_VERSION, "%d.%d.%d", &major, &minor, &patch);
    
    header->signature = htonl(CLOUDSYNC_PAYLOAD_SIGNATURE);
    header->version = CLOUDSYNC_PAYLOAD_VERSION;
    header->libversion[0] = major;
    header->libversion[1] = minor;
    header->libversion[2] = patch;
    header->expanded_size = htonl(expanded_size);
    header->ncols = htons(ncols);
    header->nrows = htonl(nrows);
    header->schema_hash = htonll(hash);
}

void cloudsync_payload_encode_step (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_payload_encode_step");
    // debug_values(argc, argv);
    
    // allocate/get the session context
    cloudsync_network_payload *payload = (cloudsync_network_payload *)sqlite3_aggregate_context(context, sizeof(cloudsync_network_payload));
    if (!payload) return;
    
    // check if the step function is called for the first time
    if (payload->nrows == 0) payload->ncols = argc;
    
    size_t breq = pk_encode_size(argv, argc, 0);
    if (cloudsync_buffer_check(payload, breq) == false) return;
    
    char *buffer = payload->buffer + payload->bused;
    char *ptr = pk_encode(argv, argc, buffer, false, NULL);
    assert(buffer == ptr);
    
    // update buffer
    payload->bused += breq;
    
    // increment row counter
    ++payload->nrows;
}

void cloudsync_payload_encode_final (sqlite3_context *context) {
    DEBUG_FUNCTION("cloudsync_payload_encode_final");

    // get the session context
    cloudsync_network_payload *payload = (cloudsync_network_payload *)sqlite3_aggregate_context(context, sizeof(cloudsync_network_payload));
    if (!payload) return;
    
    if (payload->nrows == 0) {
        sqlite3_result_null(context);
        return;
    }
    
    // encode payload
    int header_size = (int)sizeof(cloudsync_network_header);
    int real_buffer_size = (int)(payload->bused - header_size);
    int zbound = LZ4_compressBound(real_buffer_size);
    char *buffer = cloudsync_memory_alloc(zbound + header_size);
    if (!buffer) {
        cloudsync_buffer_free(payload);
        sqlite3_result_error_code(context, SQLITE_NOMEM);
        return;
    }
    
    // adjust buffer to compress to skip the reserved header
    char *src_buffer = payload->buffer + sizeof(cloudsync_network_header);
    int zused = LZ4_compress_default(src_buffer, buffer+header_size, real_buffer_size, zbound);
    bool use_uncompressed_buffer = (!zused || zused > real_buffer_size);
    CHECK_FORCE_UNCOMPRESSED_BUFFER();
    
    // setup payload network header
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    cloudsync_network_header header;
    cloudsync_network_header_init(&header, (use_uncompressed_buffer) ? 0 : real_buffer_size, payload->ncols, (uint32_t)payload->nrows, data->schema_hash);
    
    // if compression fails or if compressed size is bigger than original buffer, then use the uncompressed buffer
    if (use_uncompressed_buffer) {
        cloudsync_memory_free(buffer);
        buffer = payload->buffer;
        zused = real_buffer_size;
    }
    
    // copy header and data to SQLite BLOB
    memcpy(buffer, &header, sizeof(cloudsync_network_header));
    int blob_size = zused+sizeof(cloudsync_network_header);
    sqlite3_result_blob(context, buffer, blob_size, SQLITE_TRANSIENT);
    
    // cleanup memory
    cloudsync_buffer_free(payload);
    if (!use_uncompressed_buffer) cloudsync_memory_free(buffer);
}

cloudsync_payload_apply_callback_t cloudsync_get_payload_apply_callback(sqlite3 *db) {
    return (sqlite3_libversion_number() >= 3044000) ? sqlite3_get_clientdata(db, CLOUDSYNC_PAYLOAD_APPLY_CALLBACK_KEY) : NULL;
}

void cloudsync_set_payload_apply_callback(sqlite3 *db, cloudsync_payload_apply_callback_t callback) {
    if (sqlite3_libversion_number() >= 3044000) {
        sqlite3_set_clientdata(db, CLOUDSYNC_PAYLOAD_APPLY_CALLBACK_KEY, (void*)callback, NULL);
    }
}

int cloudsync_pk_decode_bind_callback (void *xdata, int index, int type, int64_t ival, double dval, char *pval) {
    cloudsync_pk_decode_bind_context *decode_context = (cloudsync_pk_decode_bind_context*)xdata;
    int rc = pk_decode_bind_callback(decode_context->vm, index, type, ival, dval, pval);
    
    if (rc == SQLITE_OK) {
        // the dbversion index is smaller than seq index, so it is processed first
        // when processing the dbversion column: save the value to the tmp_dbversion field
        // when processing the seq column: update the dbversion and seq fields only if the current dbversion is greater than the last max value
        switch (index) {
            case CLOUDSYNC_PK_INDEX_TBL:
                if (type == SQLITE_TEXT) {
                    decode_context->tbl = pval;
                    decode_context->tbl_len = ival;
                }
                break;
            case CLOUDSYNC_PK_INDEX_PK:
                if (type == SQLITE_BLOB) {
                    decode_context->pk = pval;
                    decode_context->pk_len = ival;
                }
                break;
            case CLOUDSYNC_PK_INDEX_COLNAME:
                if (type == SQLITE_TEXT) {
                    decode_context->col_name = pval;
                    decode_context->col_name_len = ival;
                }
                break;
            case CLOUDSYNC_PK_INDEX_COLVERSION:
                if (type == SQLITE_INTEGER) decode_context->col_version = ival;
                break;
            case CLOUDSYNC_PK_INDEX_DBVERSION:
                if (type == SQLITE_INTEGER) decode_context->db_version = ival;
                break;
            case CLOUDSYNC_PK_INDEX_SITEID:
                if (type == SQLITE_BLOB) {
                    decode_context->site_id = pval;
                    decode_context->site_id_len = ival;
                }
                break;
            case CLOUDSYNC_PK_INDEX_CL:
                if (type == SQLITE_INTEGER) decode_context->cl = ival;
                break;
            case CLOUDSYNC_PK_INDEX_SEQ:
                if (type == SQLITE_INTEGER) decode_context->seq = ival;
                break;
        }
    }
        
    return rc;
}

// #ifndef CLOUDSYNC_OMIT_RLS_VALIDATION

int cloudsync_payload_apply (sqlite3_context *context, const char *payload, int blen) {
    // decode header
    cloudsync_network_header header;
    memcpy(&header, payload, sizeof(cloudsync_network_header));

    header.signature = ntohl(header.signature);
    header.expanded_size = ntohl(header.expanded_size);
    header.ncols = ntohs(header.ncols);
    header.nrows = ntohl(header.nrows);
    header.schema_hash = ntohll(header.schema_hash);
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    if (!data || header.schema_hash != data->schema_hash) {
        sqlite3 *db = sqlite3_context_db_handle(context);
        if (!dbutils_check_schema_hash(db, header.schema_hash)) {
            dbutils_context_result_error(context, "Cannot apply the received payload because the schema hash is unknown %llu.", header.schema_hash);
            sqlite3_result_error_code(context, SQLITE_MISMATCH);
            return -1;
        }
    }
    
    // sanity check header
    if ((header.signature != CLOUDSYNC_PAYLOAD_SIGNATURE) || (header.ncols == 0)) {
        dbutils_context_result_error(context, "Error on cloudsync_payload_apply: invalid signature or column size.");
        sqlite3_result_error_code(context, SQLITE_MISUSE);
        return -1;
    }
    
    const char *buffer = payload + sizeof(cloudsync_network_header);
    blen -= sizeof(cloudsync_network_header);
    
    // check if payload is compressed
    char *clone = NULL;
    if (header.expanded_size != 0) {
        clone = (char *)cloudsync_memory_alloc(header.expanded_size);
        if (!clone) {sqlite3_result_error_code(context, SQLITE_NOMEM); return -1;}
        
        uint32_t rc = LZ4_decompress_safe(buffer, clone, blen, header.expanded_size);
        if (rc <= 0 || rc != header.expanded_size) {
            dbutils_context_result_error(context, "Error on cloudsync_payload_apply: unable to decompress BLOB (%d).", rc);
            sqlite3_result_error_code(context, SQLITE_MISUSE);
            return -1;
        }
        
        buffer = (const char *)clone;
    }
    
    // precompile the insert statement
    sqlite3_stmt *vm = NULL;
    sqlite3 *db = sqlite3_context_db_handle(context);
    const char *sql = "INSERT INTO cloudsync_changes(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) VALUES (?,?,?,?,?,?,?,?,?);";
    int rc = sqlite3_prepare(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) {
        dbutils_context_result_error(context, "Error on cloudsync_payload_apply: error while compiling SQL statement (%s).", sqlite3_errmsg(db));
        if (clone) cloudsync_memory_free(clone);
        return -1;
    }
    
    // process buffer, one row at a time
    uint16_t ncols = header.ncols;
    uint32_t nrows = header.nrows;
    int dbversion = dbutils_settings_get_int_value(db, CLOUDSYNC_KEY_CHECK_DBVERSION);
    int seq = dbutils_settings_get_int_value(db, CLOUDSYNC_KEY_CHECK_SEQ);
    cloudsync_pk_decode_bind_context decoded_context = {.vm = vm};
    void *payload_apply_xdata = NULL;
    cloudsync_payload_apply_callback_t payload_apply_callback = cloudsync_get_payload_apply_callback(db);
    
    for (uint32_t i=0; i<nrows; ++i) {
        size_t seek = 0;
        pk_decode((char *)buffer, blen, ncols, &seek, cloudsync_pk_decode_bind_callback, &decoded_context);
        // n is the pk_decode return value, I don't think I should assert here because in any case the next sqlite3_step would fail
        // assert(n == ncols);
        
        bool approved = true;
        if (payload_apply_callback) approved = payload_apply_callback(&payload_apply_xdata, &decoded_context, db, data, CLOUDSYNC_PAYLOAD_APPLY_WILL_APPLY, SQLITE_OK);

        if (approved) {
            rc = sqlite3_step(vm);
            if (rc != SQLITE_DONE) {
                // don't "break;", the error can be due to a RLS policy.
                // in case of error we try to apply the following changes
                printf("cloudsync_payload_apply error on db_version %lld/%lld: (%d) %s\n", decoded_context.db_version, decoded_context.seq, rc, sqlite3_errmsg(db));
            }
        }
        
        if (payload_apply_callback) payload_apply_callback(&payload_apply_xdata, &decoded_context, db, data, CLOUDSYNC_PAYLOAD_APPLY_DID_APPLY, rc);
        
        buffer += seek;
        blen -= seek;
        stmt_reset(vm);
    }

    char *lasterr = NULL;
    if (rc != SQLITE_OK && rc != SQLITE_DONE) lasterr = cloudsync_string_dup(sqlite3_errmsg(db), false);
    
    if (payload_apply_callback) payload_apply_callback(&payload_apply_xdata, &decoded_context, db, data, CLOUDSYNC_PAYLOAD_APPLY_CLEANUP, rc);

    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    if (rc == SQLITE_OK) {
        char buf[256];
        if (decoded_context.db_version >= dbversion) {
            snprintf(buf, sizeof(buf), "%lld", decoded_context.db_version);
            dbutils_settings_set_key_value(db, context, CLOUDSYNC_KEY_CHECK_DBVERSION, buf);
            
            if (decoded_context.seq != seq) {
                snprintf(buf, sizeof(buf), "%lld", decoded_context.seq);
                dbutils_settings_set_key_value(db, context, CLOUDSYNC_KEY_CHECK_SEQ, buf);
            }
        }
    }

    // cleanup vm
    if (vm) sqlite3_finalize(vm);
    
    // cleanup memory
    if (clone) cloudsync_memory_free(clone);
    
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, lasterr, -1);
        sqlite3_result_error_code(context, SQLITE_MISUSE);
        cloudsync_memory_free(lasterr);
        return -1;
    }
    
    // return the number of processed rows
    sqlite3_result_int(context, nrows);
    return nrows;
}

void cloudsync_payload_decode (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_payload_decode");
    //debug_values(argc, argv);
    
    // sanity check payload type
    if (sqlite3_value_type(argv[0]) != SQLITE_BLOB) {
        dbutils_context_result_error(context, "Error on cloudsync_payload_decode: value must be a BLOB.");
        sqlite3_result_error_code(context, SQLITE_MISUSE);
        return;
    }
    
    // sanity check payload size
    int blen = sqlite3_value_bytes(argv[0]);
    if (blen < (int)sizeof(cloudsync_network_header)) {
        dbutils_context_result_error(context, "Error on cloudsync_payload_decode: invalid input size.");
        sqlite3_result_error_code(context, SQLITE_MISUSE);
        return;
    }
    
    // obtain payload
    const char *payload = (const char *)sqlite3_value_blob(argv[0]);
    
    // apply changes
    cloudsync_payload_apply(context, payload, blen);
}

// MARK: - Public -

void cloudsync_version (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_version");
    UNUSED_PARAMETER(argc);
    UNUSED_PARAMETER(argv);
    sqlite3_result_text(context, CLOUDSYNC_VERSION, -1, SQLITE_STATIC);
}

void cloudsync_siteid (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_siteid");
    UNUSED_PARAMETER(argc);
    UNUSED_PARAMETER(argv);
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    sqlite3_result_blob(context, data->site_id, UUID_LEN, SQLITE_STATIC);
}

void cloudsync_db_version (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_db_version");
    UNUSED_PARAMETER(argc);
    UNUSED_PARAMETER(argv);
    
    // retrieve context
    sqlite3 *db = sqlite3_context_db_handle(context);
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    int rc = db_version_check_uptodate(db, data);
    if (rc != SQLITE_OK) {
        dbutils_context_result_error(context, "Unable to retrieve db_version (%s).", sqlite3_errmsg(db));
        return;
    }
    
    sqlite3_result_int64(context, data->db_version);
}

void cloudsync_db_version_next (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_db_version_next");
    
    // retrieve context
    sqlite3 *db = sqlite3_context_db_handle(context);
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    sqlite3_int64 merging_version = (argc == 1) ? sqlite3_value_int64(argv[0]) : CLOUDSYNC_VALUE_NOTSET;
    sqlite3_int64 value = db_version_next(db, data, merging_version);
    if (value == -1) {
        dbutils_context_result_error(context, "Unable to retrieve next_db_version (%s).", sqlite3_errmsg(db));
        return;
    }
    
    sqlite3_result_int64(context, value);
}

void cloudsync_seq (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_seq");
    
    // retrieve context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    sqlite3_result_int(context, BUMP_SEQ(data));
}

void cloudsync_uuid (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_uuid");
    
    char value[UUID_STR_MAXLEN];
    char *uuid = cloudsync_uuid_v7_string(value, true);
    sqlite3_result_text(context, uuid, -1, SQLITE_TRANSIENT);
}

// MARK: -

void cloudsync_set (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_set");
    
    // sanity check parameters
    const char *key = (const char *)sqlite3_value_text(argv[0]);
    const char *value = (const char *)sqlite3_value_text(argv[1]);
    
    // silently fails
    if (key == NULL) return;
    
    sqlite3 *db = sqlite3_context_db_handle(context);
    dbutils_settings_set_key_value(db, context, key, value);
}

void cloudsync_set_column (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_set_column");
    
    const char *tbl = (const char *)sqlite3_value_text(argv[0]);
    const char *col = (const char *)sqlite3_value_text(argv[1]);
    const char *key = (const char *)sqlite3_value_text(argv[2]);
    const char *value = (const char *)sqlite3_value_text(argv[3]);
    dbutils_table_settings_set_key_value(NULL, context, tbl, col, key, value);
}

void cloudsync_set_table (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_set_table");
    
    const char *tbl = (const char *)sqlite3_value_text(argv[0]);
    const char *key = (const char *)sqlite3_value_text(argv[1]);
    const char *value = (const char *)sqlite3_value_text(argv[2]);
    dbutils_table_settings_set_key_value(NULL, context, tbl, "*", key, value);
}

void cloudsync_is_sync (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_is_sync");
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    if (data->insync) {
        sqlite3_result_int(context, 1);
        return;
    }
    
    const char *table_name = (const char *)sqlite3_value_text(argv[0]);
    cloudsync_table_context *table = table_lookup(data, table_name);
    sqlite3_result_int(context, (table) ? (table->enabled == 0) : 0);
}

void cloudsync_col_value (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // DEBUG_FUNCTION("cloudsync_col_value");
    
    // argv[0] -> table name
    // argv[1] -> column name
    // argv[2] -> encoded pk
    
    // lookup table
    const char *table_name = (const char *)sqlite3_value_text(argv[0]);
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) {
        dbutils_context_result_error(context, "Unable to retrieve table name %s in clousdsync_colvalue.", table_name);
        return;
    }
    
    // retrieve column name
    const char *col_name = (const char *)sqlite3_value_text(argv[1]);
    
    // check for special tombstone value
    if (strcmp(col_name, CLOUDSYNC_TOMBSTONE_VALUE) == 0) {
        sqlite3_result_null(context);
        return;
    }
    
    // extract the right col_value vm associated to the column name
    sqlite3_stmt *vm = table_column_lookup(table, col_name, false, NULL);
    if (!vm) {
        sqlite3_result_error(context, "Unable to retrieve column value precompiled statement in clousdsync_colvalue.", -1);
        return;
    }
    
    // bind primary key values
    int rc = pk_decode_prikey((char *)sqlite3_value_blob(argv[2]), (size_t)sqlite3_value_bytes(argv[2]), pk_decode_bind_callback, (void *)vm);
    if (rc < 0) goto cleanup;
    
    // execute vm
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
        sqlite3_result_text(context, CLOUDSYNC_RLS_RESTRICTED_VALUE, -1, SQLITE_STATIC);
    } else if (rc == SQLITE_ROW) {
        // store value result
        rc = SQLITE_OK;
        sqlite3_result_value(context, sqlite3_column_value(vm, 0));
    }
    
cleanup:
    if (rc != SQLITE_OK) {
        sqlite3 *db = sqlite3_context_db_handle(context);
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
    }
    sqlite3_reset(vm);
}

void cloudsync_pk_encode (sqlite3_context *context, int argc, sqlite3_value **argv) {
    size_t bsize = 0;
    char *buffer = pk_encode_prikey(argv, argc, NULL, &bsize);
    if (!buffer) {
        sqlite3_result_null(context);
        return;
    }
    sqlite3_result_blob(context, (const void *)buffer, (int)bsize, SQLITE_TRANSIENT);
    cloudsync_memory_free(buffer);
}

int cloudsync_pk_decode_set_result_callback (void *xdata, int index, int type, int64_t ival, double dval, char *pval) {
    cloudsync_pk_decode_context *decode_context = (cloudsync_pk_decode_context *)xdata;
    // decode_context->index is 1 based
    // index is 0 based
    if (decode_context->index != index+1) return SQLITE_OK;
    
    int rc = 0;
    sqlite3_context *context = decode_context->context;
    switch (type) {
        case SQLITE_INTEGER:
            sqlite3_result_int64(context, ival);
            break;

        case SQLITE_FLOAT:
            sqlite3_result_double(context, dval);
            break;

        case SQLITE_NULL:
            sqlite3_result_null(context);
            break;

        case SQLITE_TEXT:
            sqlite3_result_text(context, pval, (int)ival, SQLITE_TRANSIENT);
            break;

        case SQLITE_BLOB:
            sqlite3_result_blob(context, pval, (int)ival, SQLITE_TRANSIENT);
            break;
    }
    
    return rc;
}


void cloudsync_pk_decode (sqlite3_context *context, int argc, sqlite3_value **argv) {
    const char *pk = (const char *)sqlite3_value_text(argv[0]);
    int i = sqlite3_value_int(argv[1]);
    
    cloudsync_pk_decode_context xdata = {.context = context, .index = i};
    pk_decode_prikey((char *)pk, strlen(pk), cloudsync_pk_decode_set_result_callback, &xdata);
}

// MARK: -

void cloudsync_insert (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_insert %s", sqlite3_value_text(argv[0]));
    // debug_values(argc-1, &argv[1]);
    
    // argv[0] is table name
    // argv[1]..[N] is primary key(s)
    
    // table_cloudsync
    // pk               -> encode(argc-1, &argv[1])
    // col_name         -> name
    // col_version      -> 0/1 +1
    // db_version       -> check
    // site_id          0
    // seq              -> sqlite_master
    
    // retrieve context
    sqlite3 *db = sqlite3_context_db_handle(context);
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    // lookup table
    const char *table_name = (const char *)sqlite3_value_text(argv[0]);
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) {
        dbutils_context_result_error(context, "Unable to retrieve table name %s in cloudsync_insert.", table_name);
        return;
    }
    
    // encode the primary key values into a buffer
    char buffer[1024];
    size_t pklen = sizeof(buffer);
    char *pk = pk_encode_prikey(&argv[1], table->npks, buffer, &pklen);
    if (!pk) {
        sqlite3_result_error(context, "Not enough memory to encode the primary key(s).", -1);
        return;
    }
    
    // compute the next database version for tracking changes
    sqlite3_int64 db_version = db_version_next(db, data, CLOUDSYNC_VALUE_NOTSET);
    
    // check if a row with the same primary key already exists
    // if so, this means the row might have been previously deleted (sentinel)
    bool pk_exists = (bool)stmt_count(table->meta_pkexists_stmt, pk, pklen, SQLITE_BLOB);
    int rc = SQLITE_OK;
    
    if (table->ncols == 0) {
        // if there are no columns other than primary keys, insert a sentinel record
        rc = local_mark_insert_sentinel_meta(db, table, pk, pklen, db_version, BUMP_SEQ(data));
        if (rc != SQLITE_OK) goto cleanup;
    } else if (pk_exists){
        // if a row with the same primary key already exists, update the sentinel record
        rc = local_update_sentinel(db, table, pk, pklen, db_version, BUMP_SEQ(data));
        if (rc != SQLITE_OK) goto cleanup;
    }
    
    // process each non-primary key column for insert or update
    for (int i=0; i<table->ncols; ++i) {
        // mark the column as inserted or updated in the metadata
        rc = local_mark_insert_or_update_meta(db, table, pk, pklen, table->col_name[i], db_version, BUMP_SEQ(data));
        if (rc != SQLITE_OK) goto cleanup;
    }
    
cleanup:
    if (rc != SQLITE_OK) sqlite3_result_error(context, sqlite3_errmsg(db), -1);
    // free memory if the primary key was dynamically allocated
    if (pk != buffer) cloudsync_memory_free(pk);
}

void cloudsync_update (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_update %s", sqlite3_value_text(argv[0]));
    // debug_values(argc-1, &argv[1]);
    
    // arguments are:
    // [0]                  table name
    // [1..table->npks]     NEW.prikeys
    // [1+table->npks ..]   OLD.prikeys
    // then                 NEW.value,OLD.value
    
    // retrieve context
    sqlite3 *db = sqlite3_context_db_handle(context);
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    // lookup table
    const char *table_name = (const char *)sqlite3_value_text(argv[0]);
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) {
        dbutils_context_result_error(context, "Unable to retrieve table name %s in cloudsync_update.", table_name);
        return;
    }
    
    // compute the next database version for tracking changes
    sqlite3_int64 db_version = db_version_next(db, data, CLOUDSYNC_VALUE_NOTSET);
    int rc = SQLITE_OK;
    
    // check if the primary key(s) have changed by comparing the NEW and OLD primary key values
    bool prikey_changed = false;
    for (int i=1; i<=table->npks; ++i) {
        if (dbutils_value_compare(argv[i], argv[i+table->npks]) != 0) {
            prikey_changed = true;
            break;
        }
    }
    
    // encode the NEW primary key values into a buffer (used later for indexing)
    char buffer[1024];
    char buffer2[1024];
    size_t pklen = sizeof(buffer);
    size_t oldpklen = sizeof(buffer2);
    char *oldpk = NULL;
    
    char *pk = pk_encode_prikey(&argv[1], table->npks, buffer, &pklen);
    if (!pk) {
        sqlite3_result_error(context, "Not enough memory to encode the primary key(s).", -1);
        return;
    }
    
    if (prikey_changed) {
        // if the primary key has changed, we need to handle the row differently:
        // 1. mark the old row (OLD primary key) as deleted
        // 2. create a new row (NEW primary key)
        
        // encode the OLD primary key into a buffer
        oldpk = pk_encode_prikey(&argv[1+table->npks], table->npks, buffer2, &oldpklen);
        if (!oldpk) {
            if (pk != buffer) cloudsync_memory_free(pk);
            sqlite3_result_error(context, "Not enough memory to encode the primary key(s).", -1);
            return;
        }
        
        // mark the rows with the old primary key as deleted in the metadata (old row handling)
        rc = local_mark_delete_meta(db, table, oldpk, oldpklen, db_version, BUMP_SEQ(data));
        if (rc != SQLITE_OK) goto cleanup;
        
        // move non-sentinel metadata entries from OLD primary key to NEW primary key
        // handles the case where some metadata is retained across primary key change
        // see https://github.com/sqliteai/sqlite-sync/blob/main/docs/PriKey.md for more details
        rc = local_update_move_meta(db, table, pk, pklen, oldpk, oldpklen, db_version);
        if (rc != SQLITE_OK) goto cleanup;
        
        // mark a new sentinel row with the new primary key in the metadata
        rc = local_mark_insert_sentinel_meta(db, table, pk, pklen, db_version, BUMP_SEQ(data));
        if (rc != SQLITE_OK) goto cleanup;
        
        // free memory if the OLD primary key was dynamically allocated
        if (oldpk != buffer2) cloudsync_memory_free(oldpk);
        oldpk = NULL;
    }
    
    // compare NEW and OLD values (excluding primary keys) to handle column updates
    // starting index for column values
    int index = 1 + (table->npks * 2);
    for (int i=0; i<table->ncols; i++) {
        if (dbutils_value_compare(argv[i+index], argv[i+index+1]) != 0) {
            // if a column value has changed, mark it as updated in the metadata
            // columns are in cid order
            rc = local_mark_insert_or_update_meta(db, table, pk, pklen, table->col_name[i], db_version, BUMP_SEQ(data));
            if (rc != SQLITE_OK) goto cleanup;
        }
        ++index;
    }
    
cleanup:
    if (rc != SQLITE_OK) sqlite3_result_error(context, sqlite3_errmsg(db), -1);
    if (pk != buffer) cloudsync_memory_free(pk);
    if (oldpk && (oldpk != buffer2)) cloudsync_memory_free(oldpk);
}

void cloudsync_delete (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_delete %s", sqlite3_value_text(argv[0]));
    // debug_values(argc-1, &argv[1]);
    
    // retrieve context
    sqlite3 *db = sqlite3_context_db_handle(context);
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    // lookup table
    const char *table_name = (const char *)sqlite3_value_text(argv[0]);
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) {
        dbutils_context_result_error(context, "Unable to retrieve table name %s in cloudsync_delete.", table_name);
        return;
    }
    
    // compute the next database version for tracking changes
    sqlite3_int64 db_version = db_version_next(db, data, CLOUDSYNC_VALUE_NOTSET);
    int rc = SQLITE_OK;
    
    // encode the primary key values into a buffer
    char buffer[1024];
    size_t pklen = sizeof(buffer);
    char *pk = pk_encode_prikey(&argv[1], table->npks, buffer, &pklen);
    if (!pk) {
        sqlite3_result_error(context, "Not enough memory to encode the primary key(s).", -1);
        return;
    }
    
    // mark the row as deleted by inserting a delete sentinel into the metadata
    rc = local_mark_delete_meta(db, table, pk, pklen, db_version, BUMP_SEQ(data));
    if (rc != SQLITE_OK) goto cleanup;
    
    // remove any metadata related to the old rows associated with this primary key
    rc = local_drop_meta(db, table, pk, pklen);
    if (rc != SQLITE_OK) goto cleanup;
    
cleanup:
    if (rc != SQLITE_OK) sqlite3_result_error(context, sqlite3_errmsg(db), -1);
    // free memory if the primary key was dynamically allocated
    if (pk != buffer) cloudsync_memory_free(pk);
}

// MARK: -

int cloudsync_cleanup_internal (sqlite3_context *context, const char *table_name) {
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    // get database reference
    sqlite3 *db = sqlite3_context_db_handle(context);
    
    // init cloudsync_settings
    if (cloudsync_context_init(db, data, context) == NULL) return SQLITE_MISUSE;
    
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) return SQLITE_OK;
    
    table_remove_from_context(data, table);
    table_free(table);
        
    // drop meta-table
    char *sql = cloudsync_memory_mprintf("DROP TABLE IF EXISTS \"%w_cloudsync\";", table_name);
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    cloudsync_memory_free(sql);
    if (rc != SQLITE_OK) {
        dbutils_context_result_error(context, "Unable to drop cloudsync table %s_cloudsync in cloudsync_cleanup.", table_name);
        sqlite3_result_error_code(context, rc);
        return rc;
    }
    
    // drop original triggers
    dbutils_delete_triggers(db, table_name);
    if (rc != SQLITE_OK) {
        dbutils_context_result_error(context, "Unable to drop cloudsync table %s_cloudsync in cloudsync_cleanup.", table_name);
        sqlite3_result_error_code(context, rc);
        return rc;
    }
    
    // remove all table related settings
    dbutils_table_settings_set_key_value(db, context, table_name, NULL, NULL, NULL);
    
    return SQLITE_OK;
}

void cloudsync_cleanup_all (sqlite3_context *context) {
    char *sql = "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'cloudsync_%' AND name NOT LIKE '%_cloudsync';";
    
    sqlite3 *db = sqlite3_context_db_handle(context);
    char **result = NULL;
    int nrows, ncols;
    char *errmsg;
    int rc = sqlite3_get_table(db, sql, &result, &nrows, &ncols, &errmsg);
    if (errmsg || ncols != 1) {
        printf("cloudsync_cleanup_all error: %s\n", errmsg ? errmsg : "invalid table");
        goto cleanup;
    }
    
    rc = SQLITE_OK;
    for (int i = ncols; i < nrows+ncols; i+=ncols) {
        int rc2 = cloudsync_cleanup_internal(context, result[i]);
        if (rc2 != SQLITE_OK) rc = rc2;
    }
    
    if (rc == SQLITE_OK) {
        cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
        data->site_id[0] = 0;
        dbutils_settings_cleanup(db);
    }
    
cleanup:
    sqlite3_free_table(result);
    sqlite3_free(errmsg);
}

void cloudsync_cleanup (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_cleanup");
    
    const char *table = (const char *)sqlite3_value_text(argv[0]);
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    sqlite3 *db = sqlite3_context_db_handle(context);
    
    if (dbutils_is_star_table(table)) cloudsync_cleanup_all(context);
    else cloudsync_cleanup_internal(context, table);
    
    if (dbutils_table_exists(db, CLOUDSYNC_TABLE_SETTINGS_NAME) == true) dbutils_update_schema_hash(db, &data->schema_hash);
}

void cloudsync_enable_disable (sqlite3_context *context, const char *table_name, bool value) {
    DEBUG_FUNCTION("cloudsync_enable_disable");
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) return;
    
    table->enabled = value;
}

int cloudsync_enable_disable_all_callback (void *xdata, int ncols, char **values, char **names) {
    sqlite3_context *context = (sqlite3_context *)xdata;
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    bool value = data->temp_bool;
    
    for (int i=0; i<ncols; i++) {
        const char *table_name = values[i];
        cloudsync_table_context *table = table_lookup(data, table_name);
        if (!table) continue;
        table->enabled = value;
    }
    
    return SQLITE_OK;
}

void cloudsync_enable_disable_all (sqlite3_context *context, bool value) {
    DEBUG_FUNCTION("cloudsync_enable_disable_all");
    
    char *sql = "SELECT name FROM sqlite_master WHERE type='table';";
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    data->temp_bool = value;
    sqlite3 *db = sqlite3_context_db_handle(context);
    sqlite3_exec(db, sql, cloudsync_enable_disable_all_callback, context, NULL);
}

void cloudsync_enable (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_enable");
    
    const char *table = (const char *)sqlite3_value_text(argv[0]);
    if (dbutils_is_star_table(table)) cloudsync_enable_disable_all(context, true);
    else cloudsync_enable_disable(context, table, true);
}

void cloudsync_disable (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_disable");
    
    const char *table = (const char *)sqlite3_value_text(argv[0]);
    if (dbutils_is_star_table(table)) cloudsync_enable_disable_all(context, false);
    else cloudsync_enable_disable(context, table, false);
}

void cloudsync_is_enabled (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_is_enabled");
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    const char *table_name = (const char *)sqlite3_value_text(argv[0]);
    cloudsync_table_context *table = table_lookup(data, table_name);
    
    int result = (table && table->enabled) ? 1 : 0;
    sqlite3_result_int(context, result);
}

void cloudsync_terminate (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_terminate");
    
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    for (int i=0; i<data->tables_count; ++i) {
        if (data->tables[i]) table_free(data->tables[i]);
        data->tables[i] = NULL;
    }
    
    if (data->schema_version_stmt) sqlite3_finalize(data->schema_version_stmt);
    if (data->data_version_stmt) sqlite3_finalize(data->data_version_stmt);
    if (data->db_version_stmt) sqlite3_finalize(data->db_version_stmt);
    if (data->getset_siteid_stmt) sqlite3_finalize(data->getset_siteid_stmt);
    
    data->schema_version_stmt = NULL;
    data->data_version_stmt = NULL;
    data->db_version_stmt = NULL;
    data->getset_siteid_stmt = NULL;
    
    // reset the site_id so the cloudsync_context_init will be executed again
    // if any other cloudsync function is called after terminate
    data->site_id[0] = 0;
}

// MARK: -

int cloudsync_load_siteid (sqlite3 *db, cloudsync_context *data) {
    // check if site_id was already loaded
    if (data->site_id[0] != 0) return SQLITE_OK;
    
    // load site_id
    int size, rc;
    char *buffer = dbutils_blob_select(db, "SELECT site_id FROM cloudsync_site_id WHERE rowid=0;", &size, data->sqlite_ctx, &rc);
    if (!buffer) return rc;
    if (size != UUID_LEN) return SQLITE_MISUSE;
    
    memcpy(data->site_id, buffer, UUID_LEN);
    cloudsync_memory_free(buffer);
    
    return SQLITE_OK;
}

int cloudsync_init_internal (sqlite3_context *context, const char *table_name, const char *algo_name, bool skip_int_pk_check) {
    DEBUG_FUNCTION("cloudsync_init_internal");
    
    // get database reference
    sqlite3 *db = sqlite3_context_db_handle(context);
    
    // retrieve global context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    // sanity check table and its primary key(s)
    if (dbutils_table_sanity_check(db, context, table_name, skip_int_pk_check) == false) {
        return SQLITE_MISUSE;
    }
    
    // init cloudsync_settings
    if (cloudsync_context_init(db, data, context) == NULL) return SQLITE_MISUSE;
    
    // sanity check algo name (if exists)
    table_algo algo_new = table_algo_none;
    if (!algo_name) {
        algo_name = CLOUDSYNC_DEFAULT_ALGO;
    }
    
    algo_new = crdt_algo_from_name(algo_name);
    if (algo_new == table_algo_none) {
        dbutils_context_result_error(context, "algo name %s does not exist", crdt_algo_name);
        return SQLITE_MISUSE;
    }
    
    // check if table name was already augmented
    table_algo algo_current = dbutils_table_settings_get_algo(db, table_name);
    
    // sanity check algorithm
    if ((algo_new == algo_current) && (algo_current != table_algo_none)) {
        // if table algorithms and the same and not none, do nothing
    } else if ((algo_new == table_algo_none) && (algo_current == table_algo_none)) {
        // nothing is written into settings because the default table_algo_crdt_cls will be used
        algo_new = algo_current = table_algo_crdt_cls;
    } else if ((algo_new == table_algo_none) && (algo_current != table_algo_none)) {
        // algo is already written into settins so just use it
        algo_new = algo_current;
    } else if ((algo_new != table_algo_none) && (algo_current == table_algo_none)) {
        // write table algo name in settings
        dbutils_table_settings_set_key_value(NULL, context, table_name, "*", "algo", algo_name);
    } else {
        // error condition
        dbutils_context_result_error(context, "%s", "Before changing a table algorithm you must call cloudsync_cleanup(table_name)");
        return SQLITE_MISUSE;
    }
    
    // Run the following function even if table was already augmented.
    // It is safe to call the following function multiple times, if there is nothing to update nothing will be changed.
    // After an alter table, in contrast, all the cloudsync triggers, tables and stmts must be recreated.
    
    // sync algo with table (unused in this version)
    // cloudsync_sync_table_key(data, table_name, "*", CLOUDSYNC_KEY_ALGO, crdt_algo_name(algo_new));
    
    // check triggers
    dbutils_check_triggers(db, table_name, algo_new);
    
    // check meta-table
    dbutils_check_metatable(db, table_name, algo_new);
    
    // add prepared statements
    if (stmts_add_tocontext(db, data) != SQLITE_OK) {
        dbutils_context_result_error(context, "%s", "An error occurred while trying to compile prepared SQL statements.");
        return SQLITE_MISUSE;
    }
    
    // add table to in-memory data context
    if (table_add_to_context(db, data, algo_new, table_name) == false) {
        dbutils_context_result_error(context, "An error occurred while adding %s table information to global context", table_name);
        return SQLITE_MISUSE;
    }
    
    if (cloudsync_refill_metatable(db, data, table_name) != SQLITE_OK) {
        dbutils_context_result_error(context, "%s", "An error occurred while trying to fill the augmented table.");
        return SQLITE_MISUSE;
    }
        
    return SQLITE_OK;
}

int cloudsync_init_all (sqlite3_context *context, const char *algo_name, bool skip_int_pk_check) {
    char sql[1024];
    snprintf(sql, sizeof(sql), "SELECT name, '%s' FROM sqlite_master WHERE type='table' and name NOT LIKE 'sqlite_%%' AND name NOT LIKE 'cloudsync_%%' AND name NOT LIKE '%%_cloudsync';", (algo_name) ? algo_name : CLOUDSYNC_DEFAULT_ALGO);
    
    sqlite3 *db = sqlite3_context_db_handle(context);
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto abort_init_all;
    
    while (1) {
        rc = sqlite3_step(vm);
        if (rc == SQLITE_DONE) break;
        else if (rc != SQLITE_ROW) goto abort_init_all;
        
        const char *table = (const char *)sqlite3_column_text(vm, 0);
        const char *algo = (const char *)sqlite3_column_text(vm, 1);
        rc = cloudsync_init_internal(context, table, algo, skip_int_pk_check);
        if (rc != SQLITE_OK) {cloudsync_cleanup_internal(context, table); goto abort_init_all;}
    }
    rc = SQLITE_OK;
     
abort_init_all:
    if (vm) sqlite3_finalize(vm);
    return rc;
}

void cloudsync_init (sqlite3_context *context, const char *table, const char *algo, bool skip_int_pk_check) {
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    data->sqlite_ctx = context;
    
    sqlite3 *db = sqlite3_context_db_handle(context);
    int rc = sqlite3_exec(db, "SAVEPOINT cloudsync_init;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        dbutils_context_result_error(context, "Unable to create cloudsync_init savepoint. %s", sqlite3_errmsg(db));
        sqlite3_result_error_code(context, rc);
        return;
    }
    
    if (dbutils_is_star_table(table)) rc = cloudsync_init_all(context, algo, skip_int_pk_check);
    else rc = cloudsync_init_internal(context, table, algo, skip_int_pk_check);
    
    if (rc == SQLITE_OK) {
        rc = sqlite3_exec(db, "RELEASE cloudsync_init", NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            dbutils_context_result_error(context, "Unable to release cloudsync_init savepoint. %s", sqlite3_errmsg(db));
            sqlite3_result_error_code(context, rc);
        }
    }
    
    if (rc == SQLITE_OK) dbutils_update_schema_hash(db, &data->schema_hash);
    else sqlite3_exec(db, "ROLLBACK TO cloudsync_init; RELEASE cloudsync_init", NULL, NULL, NULL);
}

void cloudsync_init3 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_init2");
    
    const char *table = (const char *)sqlite3_value_text(argv[0]);
    const char *algo = (const char *)sqlite3_value_text(argv[1]);
    bool skip_int_pk_check = (bool)sqlite3_value_int(argv[2]);

    cloudsync_init(context, table, algo, skip_int_pk_check);
}

void cloudsync_init2 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_init2");
    
    const char *table = (const char *)sqlite3_value_text(argv[0]);
    const char *algo = (const char *)sqlite3_value_text(argv[1]);
    
    cloudsync_init(context, table, algo, false);
}

void cloudsync_init1 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_init1");
    
    const char *table = (const char *)sqlite3_value_text(argv[0]);
    
    cloudsync_init(context, table, NULL, false);
}

// MARK: -

void cloudsync_begin_alter (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_begin_alter");
    char *errmsg = NULL;
    char **result = NULL;
    
    const char *table_name = (const char *)sqlite3_value_text(argv[0]);
    
    // get database reference
    sqlite3 *db = sqlite3_context_db_handle(context);
    
    // retrieve global context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    // init cloudsync_settings
    if (cloudsync_context_init(db, data, context) == NULL) {
        sqlite3_result_error(context, "Unable to init the cloudsync context.", -1);
        sqlite3_result_error_code(context, SQLITE_MISUSE);
        return;
    }
    
    // create a savepoint to manage the alter operations as a transaction
    int rc = sqlite3_exec(db, "SAVEPOINT cloudsync_alter", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, "Unable to create cloudsync_alter savepoint.", -1);
        sqlite3_result_error_code(context, rc);
        goto rollback_begin_alter;
    }
    
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) {
        dbutils_context_result_error(context, "Unable to find table %s", table_name);
        sqlite3_result_error_code(context, SQLITE_MISUSE);
        goto rollback_begin_alter;
    }
    
    int nrows, ncols;
    char *sql = cloudsync_memory_mprintf("SELECT name FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk;", table_name);
    rc = sqlite3_get_table(db, sql, &result, &nrows, &ncols, &errmsg);
    cloudsync_memory_free(sql);
    if (errmsg || ncols != 1 || nrows != table->npks) {
        dbutils_context_result_error(context, "Unable to get primary keys for table %s (%s)", table_name, errmsg);
        sqlite3_result_error_code(context, SQLITE_MISUSE);
        goto rollback_begin_alter;
    }
    
    // drop original triggers
    dbutils_delete_triggers(db, table_name);
    if (rc != SQLITE_OK) {
        dbutils_context_result_error(context, "Unable to delete triggers for table %s in cloudsync_begin_alter.", table_name);
        sqlite3_result_error_code(context, rc);
        goto rollback_begin_alter;
    }
    
    if (table->pk_name) sqlite3_free_table(table->pk_name);
    table->pk_name = result;
    return;
    
rollback_begin_alter:
    sqlite3_exec(db, "ROLLBACK TO cloudsync_alter; RELEASE cloudsync_alter;", NULL, NULL, NULL);

cleanup_begin_alter:
    sqlite3_free_table(result);
    sqlite3_free(errmsg);
}

void cloudsync_commit_alter (sqlite3_context *context, int argc, sqlite3_value **argv) {
    DEBUG_FUNCTION("cloudsync_commit_alter");
    
    const char *table_name = (const char *)sqlite3_value_text(argv[0]);
    cloudsync_table_context *table = NULL;
    
    // get database reference
    sqlite3 *db = sqlite3_context_db_handle(context);
    
    // retrieve global context
    cloudsync_context *data = (cloudsync_context *)sqlite3_user_data(context);
    
    // init cloudsync_settings
    if (cloudsync_context_init(db, data, context) == NULL) {
        dbutils_context_result_error(context, "Unable to init the cloudsync context.");
        sqlite3_result_error_code(context, SQLITE_MISUSE);
        goto rollback_finalize_alter;
    }
    
    table = table_lookup(data, table_name);
    if (!table || !table->pk_name) {
        dbutils_context_result_error(context, "Unable to find table context.");
        sqlite3_result_error_code(context, SQLITE_MISUSE);
        goto rollback_finalize_alter;
    }
    
    int rc = cloudsync_finalize_alter(context, data, table);
    if (rc != SQLITE_OK) goto rollback_finalize_alter;
    
    // the table is outdated, delete it and it will be reloaded in the cloudsync_init_internal
    table_remove(data, table_name);
    table_free(table);
    table = NULL;
        
    // init again cloudsync for the table
    table_algo algo_current = dbutils_table_settings_get_algo(db, table_name);
    if (algo_current == table_algo_none) algo_current = dbutils_table_settings_get_algo(db, "*");
    rc = cloudsync_init_internal(context, table_name, crdt_algo_name(algo_current), true);
    if (rc != SQLITE_OK) goto rollback_finalize_alter;

    // release savepoint
    rc = sqlite3_exec(db, "RELEASE cloudsync_alter", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        dbutils_context_result_error(context, sqlite3_errmsg(db));
        sqlite3_result_error_code(context, rc);
        goto rollback_finalize_alter;
    }
    
    dbutils_update_schema_hash(db, &data->schema_hash);
    
    return;
    
rollback_finalize_alter:
    sqlite3_exec(db, "ROLLBACK TO cloudsync_alter; RELEASE cloudsync_alter;", NULL, NULL, NULL);
    if (table) {
        sqlite3_free_table(table->pk_name);
        table->pk_name = NULL;
    }
}
    
// MARK: - Main Entrypoint -

APIEXPORT int sqlite3_cloudsync_init (sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
    DEBUG_FUNCTION("sqlite3_cloudsync_init");
    
    #ifndef SQLITE_CORE
    SQLITE_EXTENSION_INIT2(pApi);
    #endif
    int rc = SQLITE_OK;
    
    // there's no built-in way to verify if sqlite3_cloudsync_init has already been called
    // for this specific database connection, we use a workaround: we attempt to retrieve the
    // cloudsync_version and check for an error, an error indicates that initialization has not been performed
    if (sqlite3_exec(db, "SELECT cloudsync_version();", NULL, NULL, NULL) == SQLITE_OK) return SQLITE_OK;
    
    // init memory debugger (NOOP in production)
    cloudsync_memory_init(1);
    
    // init context
    void *ctx = cloudsync_context_create();
    if (!ctx) {
        if (pzErrMsg) *pzErrMsg = "Not enought memory to create a database context";
        return SQLITE_NOMEM;
    }
    
    // register functions
    
    // PUBLIC functions
    rc = dbutils_register_function(db, "cloudsync_version", cloudsync_version, 0, pzErrMsg, ctx, cloudsync_context_free);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_init", cloudsync_init1, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_init", cloudsync_init2, 2, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_init", cloudsync_init3, 3, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;

    
    rc = dbutils_register_function(db, "cloudsync_enable", cloudsync_enable, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_disable", cloudsync_disable, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_is_enabled", cloudsync_is_enabled, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_cleanup", cloudsync_cleanup, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_terminate", cloudsync_terminate, 0, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_set", cloudsync_set, 2, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_set_table", cloudsync_set_table, 3, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_set_column", cloudsync_set_column, 4, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_siteid", cloudsync_siteid, 0, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_db_version", cloudsync_db_version, 0, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_db_version_next", cloudsync_db_version_next, 0, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_db_version_next", cloudsync_db_version_next, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_begin_alter", cloudsync_begin_alter, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_commit_alter", cloudsync_commit_alter, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_uuid", cloudsync_uuid, 0, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    // PAYLOAD
    rc = dbutils_register_aggregate(db, "cloudsync_payload_encode", cloudsync_payload_encode_step, cloudsync_payload_encode_final, -1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_payload_decode", cloudsync_payload_decode, -1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    // PRIVATE functions
    rc = dbutils_register_function(db, "cloudsync_is_sync", cloudsync_is_sync, 1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_insert", cloudsync_insert, -1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_update", cloudsync_update, -1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_delete", cloudsync_delete, -1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_col_value", cloudsync_col_value, 3, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_pk_encode", cloudsync_pk_encode, -1, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_pk_decode", cloudsync_pk_decode, 2, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = dbutils_register_function(db, "cloudsync_seq", cloudsync_seq, 0, pzErrMsg, ctx, NULL);
    if (rc != SQLITE_OK) return rc;

    // NETWORK LAYER
    #ifndef CLOUDSYNC_OMIT_NETWORK
    rc = cloudsync_network_register(db, pzErrMsg, ctx);
    if (rc != SQLITE_OK) return rc;
    #endif
    
    cloudsync_context *data = (cloudsync_context *)ctx;
    sqlite3_commit_hook(db, cloudsync_commit_hook, ctx);
    sqlite3_rollback_hook(db, cloudsync_rollback_hook, ctx);
    
    // register eponymous only changes virtual table
    rc = cloudsync_vtab_register_changes (db, data);
    if (rc != SQLITE_OK) return rc;
    
    // load config, if exists
    if (cloudsync_config_exists(db)) {
        cloudsync_context_init(db, ctx, NULL);
    }
    
    return SQLITE_OK;
}

