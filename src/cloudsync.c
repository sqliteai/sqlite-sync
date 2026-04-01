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
#include "lz4.h"
#include "pk.h"
#include "sql.h"
#include "utils.h"
#include "dbutils.h"
#include "block.h"

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

#define CLOUDSYNC_INIT_NTABLES                  64
#define CLOUDSYNC_MIN_DB_VERSION                0

#define CLOUDSYNC_PAYLOAD_MINBUF_SIZE                   (512*1024)
#define CLOUDSYNC_PAYLOAD_SIGNATURE                     0x434C5359  /* 'C','L','S','Y' */
#define CLOUDSYNC_PAYLOAD_VERSION_ORIGNAL               1
#define CLOUDSYNC_PAYLOAD_VERSION_1                     CLOUDSYNC_PAYLOAD_VERSION_ORIGNAL
#define CLOUDSYNC_PAYLOAD_VERSION_2                     2
#define CLOUDSYNC_PAYLOAD_VERSION_LATEST                CLOUDSYNC_PAYLOAD_VERSION_2
#define CLOUDSYNC_PAYLOAD_MIN_VERSION_WITH_CHECKSUM     CLOUDSYNC_PAYLOAD_VERSION_2

#ifndef MAX
#define MAX(a, b)                               (((a)>(b))?(a):(b))
#endif

#define DEBUG_DBERROR(_rc, _fn, _data)   do {if (_rc != DBRES_OK) printf("Error in %s: %s\n", _fn, database_errmsg(_data));} while (0)

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
    DBVM_VALUE_ERROR      = -1,
    DBVM_VALUE_UNCHANGED  = 0,
    DBVM_VALUE_CHANGED    = 1,
} DBVM_VALUE;

#define SYNCBIT_SET(_data)                  _data->insync = 1
#define SYNCBIT_RESET(_data)                _data->insync = 0

// MARK: - Deferred column-batch merge -

typedef struct {
    const char  *col_name;       // pointer into table_context->col_name[idx] (stable)
    dbvalue_t   *col_value;      // duplicated via database_value_dup (owned)
    int64_t     col_version;
    int64_t     db_version;
    uint8_t     site_id[UUID_LEN];
    int         site_id_len;
    int64_t     seq;
} merge_pending_entry;

typedef struct {
    cloudsync_table_context *table;
    char        *pk;             // malloc'd copy, freed on flush
    int         pk_len;
    int64_t     cl;
    bool        sentinel_pending;
    bool        row_exists;       // true when the PK already exists locally
    int         count;
    int         capacity;
    merge_pending_entry *entries;

    // Statement cache — reuse the prepared statement when the column
    // combination and row_exists flag match between consecutive PK flushes.
    dbvm_t      *cached_vm;
    bool        cached_row_exists;
    int         cached_col_count;
    const char  **cached_col_names; // array of pointers into table_context (not owned)
} merge_pending_batch;

// MARK: -

struct cloudsync_pk_decode_bind_context {
    dbvm_t      *vm;
    char        *tbl;
    int64_t     tbl_len;
    const void  *pk;
    int64_t     pk_len;
    char        *col_name;
    int64_t     col_name_len;
    int64_t     col_version;
    int64_t     db_version;
    const void  *site_id;
    int64_t     site_id_len;
    int64_t     cl;
    int64_t     seq;
};

struct cloudsync_context {
    void        *db;
    char        errmsg[1024];
    int         errcode;
    
    char        *libversion;
    uint8_t     site_id[UUID_LEN];
    int         insync;
    int         debug;
    bool        merge_equal_values;
    void        *aux_data;
    
    // stmts and context values
    dbvm_t      *schema_version_stmt;
    dbvm_t      *data_version_stmt;
    dbvm_t      *db_version_stmt;
    dbvm_t      *getset_siteid_stmt;
    int         data_version;
    int         schema_version;
    uint64_t    schema_hash;
    
    // set at transaction start and reset on commit/rollback
    int64_t    db_version;
    // version the DB would have if the transaction committed now
    int64_t    pending_db_version;
    // used to set an order inside each transaction
    int        seq;
    
    // optional schema_name to be set in the cloudsync_table_context
    char       *current_schema;
    
    // augmented tables are stored in-memory so we do not need to retrieve information about
    // col_names and cid from the disk each time a write statement is performed
    // we do also not need to use an hash map here because for few tables the direct
    // in-memory comparison with table name is faster
    cloudsync_table_context **tables;   // dense vector: [0..tables_count-1] are valid
    int tables_count;                   // size
    int tables_cap;                     // capacity

    int skip_decode_idx;                // -1 in sqlite, col_value index in postgresql

    // deferred column-batch merge (active during payload_apply)
    merge_pending_batch *pending_batch;
};

struct cloudsync_table_context {
    table_algo  algo;                           // CRDT algoritm associated to the table
    char        *name;                          // table name
    char        *schema;                        // table schema
    char        *meta_ref;                      // schema-qualified meta table name (e.g. "schema"."name_cloudsync")
    char        *base_ref;                      // schema-qualified base table name (e.g. "schema"."name")
    char        **col_name;                     // array of column names
    dbvm_t      **col_merge_stmt;               // array of merge insert stmt (indexed by col_name)
    dbvm_t      **col_value_stmt;               // array of column value stmt (indexed by col_name)
    int         *col_id;                        // array of column id
    col_algo_t  *col_algo;                      // per-column algorithm (normal or block)
    char        **col_delimiter;                // per-column delimiter for block splitting (NULL = default "\n")
    bool        has_block_cols;                 // quick check: does this table have any block columns?
    dbvm_t      *block_value_read_stmt;         // SELECT col_value FROM blocks table
    dbvm_t      *block_value_write_stmt;        // INSERT OR REPLACE into blocks table
    dbvm_t      *block_value_delete_stmt;       // DELETE from blocks table
    dbvm_t      *block_list_stmt;               // SELECT block entries for materialization
    char        *blocks_ref;                    // schema-qualified blocks table name
    int         ncols;                          // number of non primary key cols
    int         npks;                           // number of primary key cols
    bool        enabled;                        // flag to check if a table is enabled or disabled
    #if !CLOUDSYNC_DISABLE_ROWIDONLY_TABLES
    bool        rowid_only;                     // a table with no primary keys other than the implicit rowid
    #endif
    
    char        **pk_name;                      // array of primary key names

    // precompiled statements
    dbvm_t      *meta_pkexists_stmt;            // check if a primary key already exist in the augmented table
    dbvm_t      *meta_sentinel_update_stmt;     // update a local sentinel row
    dbvm_t      *meta_sentinel_insert_stmt;     // insert a local sentinel row
    dbvm_t      *meta_row_insert_update_stmt;   // insert/update a local row
    dbvm_t      *meta_row_drop_stmt;            // delete rows from meta
    dbvm_t      *meta_update_move_stmt;         // update rows in meta when pk changes
    dbvm_t      *meta_local_cl_stmt;            // compute local cl value
    dbvm_t      *meta_winner_clock_stmt;        // get the rowid of the last inserted/updated row in the meta table
    dbvm_t      *meta_merge_delete_drop;
    dbvm_t      *meta_zero_clock_stmt;
    dbvm_t      *meta_col_version_stmt;
    dbvm_t      *meta_site_id_stmt;
    
    dbvm_t      *real_col_values_stmt;          // retrieve all column values based on pk
    dbvm_t      *real_merge_delete_stmt;
    dbvm_t      *real_merge_sentinel_stmt;
    
    bool        is_altering;                    // flag to track if a table alteration is in progress

    // context
    cloudsync_context *context;
};

struct cloudsync_payload_context {
    char        *buffer;
    size_t      bsize;
    size_t      balloc;
    size_t      bused;
    uint64_t    nrows;
    uint16_t    ncols;
};

#ifdef _MSC_VER
    #pragma pack(push, 1) // For MSVC: pack struct with 1-byte alignment
    #define PACKED
#else
    #define PACKED __attribute__((__packed__))
#endif

typedef struct PACKED {
    uint32_t    signature;          // 'CLSY'
    uint8_t     version;            // protocol version
    uint8_t     libversion[3];      // major.minor.patch
    uint32_t    expanded_size;
    uint16_t    ncols;
    uint32_t    nrows;
    uint64_t    schema_hash;
    uint8_t     checksum[6];        // 48 bits checksum (to ensure struct is 32 bytes)
} cloudsync_payload_header;

#ifdef _MSC_VER
    #pragma pack(pop)
#endif

#if CLOUDSYNC_UNITTEST
bool force_uncompressed_blob = false;
#define CHECK_FORCE_UNCOMPRESSED_BUFFER()   if (force_uncompressed_blob) use_uncompressed_buffer = true
#else
#define CHECK_FORCE_UNCOMPRESSED_BUFFER()
#endif

// Internal prototypes
int local_mark_insert_or_update_meta (cloudsync_table_context *table, const void *pk, size_t pklen, const char *col_name, int64_t db_version, int seq);

// MARK: - CRDT algos -

table_algo cloudsync_algo_from_name (const char *algo_name) {
    if (algo_name == NULL) return table_algo_none;
    
    if ((strcasecmp(algo_name, "CausalLengthSet") == 0) || (strcasecmp(algo_name, "cls") == 0)) return table_algo_crdt_cls;
    if ((strcasecmp(algo_name, "GrowOnlySet") == 0) || (strcasecmp(algo_name, "gos") == 0)) return table_algo_crdt_gos;
    if ((strcasecmp(algo_name, "DeleteWinsSet") == 0) || (strcasecmp(algo_name, "dws") == 0)) return table_algo_crdt_dws;
    if ((strcasecmp(algo_name, "AddWinsSet") == 0) || (strcasecmp(algo_name, "aws") == 0)) return table_algo_crdt_aws;
    
    // if nothing is found
    return table_algo_none;
}

const char *cloudsync_algo_name (table_algo algo) {
    switch (algo) {
        case table_algo_crdt_cls: return "cls";
        case table_algo_crdt_gos: return "gos";
        case table_algo_crdt_dws: return "dws";
        case table_algo_crdt_aws: return "aws";
        case table_algo_none: return NULL;
    }
    return NULL;
}

// MARK: - DBVM Utils -

DBVM_VALUE dbvm_execute (dbvm_t *stmt, cloudsync_context *data) {
    if (!stmt) return DBVM_VALUE_ERROR;

    int rc = databasevm_step(stmt);
    if (rc != DBRES_ROW && rc != DBRES_DONE) {
        if (data) DEBUG_DBERROR(rc, "stmt_execute", data);
        databasevm_reset(stmt);
        return DBVM_VALUE_ERROR;
    }

    DBVM_VALUE result = DBVM_VALUE_CHANGED;
    if (stmt == data->data_version_stmt) {
        int version = (int)database_column_int(stmt, 0);
        if (version != data->data_version) {
            data->data_version = version;
        } else {
            result = DBVM_VALUE_UNCHANGED;
        }
    } else if (stmt == data->schema_version_stmt) {
        int version = (int)database_column_int(stmt, 0);
        if (version > data->schema_version) {
            data->schema_version = version;
        } else {
            result = DBVM_VALUE_UNCHANGED;
        }
        
    } else if (stmt == data->db_version_stmt) {
        data->db_version = (rc == DBRES_DONE) ? CLOUDSYNC_MIN_DB_VERSION : database_column_int(stmt, 0);
    }
    
    databasevm_reset(stmt);
    return result;
}

int dbvm_count (dbvm_t *stmt, const char *value, size_t len, int type) {
    int result = -1;
    int rc = DBRES_OK;
    
    if (value) {
        rc = (type == DBTYPE_TEXT) ? databasevm_bind_text(stmt, 1, value, (int)len) : databasevm_bind_blob(stmt, 1, value, len);
        if (rc != DBRES_OK) goto cleanup;
    }

    rc = databasevm_step(stmt);
    if (rc == DBRES_DONE) {
        result = 0;
        rc = DBRES_OK;
    } else if (rc == DBRES_ROW) {
        result = (int)database_column_int(stmt, 0);
        rc = DBRES_OK;
    }
    
cleanup:
    databasevm_reset(stmt);
    return result;
}

void dbvm_reset (dbvm_t *stmt) {
    if (!stmt) return;
    databasevm_clear_bindings(stmt);
    databasevm_reset(stmt);
}

// MARK: - Database Version -

char *cloudsync_dbversion_build_query (cloudsync_context *data) {
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
    
    char *value = NULL;
    int rc = database_select_text(data, SQL_DBVERSION_BUILD_QUERY, &value);
    return (rc == DBRES_OK) ? value : NULL;
}

int cloudsync_dbversion_rebuild (cloudsync_context *data) {
    if (data->db_version_stmt) {
        databasevm_finalize(data->db_version_stmt);
        data->db_version_stmt = NULL;
    }
    
    int64_t count = dbutils_table_settings_count_tables(data);
    if (count == 0) return DBRES_OK;
    else if (count == -1) return cloudsync_set_dberror(data);
    
    char *sql = cloudsync_dbversion_build_query(data);
    if (!sql) return DBRES_NOMEM;
    DEBUG_SQL("db_version_stmt: %s", sql);
    
    int rc = databasevm_prepare(data, sql, (void **)&data->db_version_stmt, DBFLAG_PERSISTENT);
    DEBUG_STMT("db_version_stmt %p", data->db_version_stmt);
    cloudsync_memory_free(sql);
    return rc;
}

int cloudsync_dbversion_rerun (cloudsync_context *data) {
    DBVM_VALUE schema_changed = dbvm_execute(data->schema_version_stmt, data);
    if (schema_changed == DBVM_VALUE_ERROR) return -1;

    if (schema_changed == DBVM_VALUE_CHANGED) {
        int rc = cloudsync_dbversion_rebuild(data);
        if (rc != DBRES_OK) return -1;
    }

    if (!data->db_version_stmt) {
        data->db_version = CLOUDSYNC_MIN_DB_VERSION;
        return 0;
    }

    DBVM_VALUE rc = dbvm_execute(data->db_version_stmt, data);
    if (rc == DBVM_VALUE_ERROR) return -1;
    return 0;
}

int cloudsync_dbversion_check_uptodate (cloudsync_context *data) {
    // perform a PRAGMA data_version to check if some other process write any data
    DBVM_VALUE rc = dbvm_execute(data->data_version_stmt, data);
    if (rc == DBVM_VALUE_ERROR) return -1;
    
    // db_version is already set and there is no need to update it
    if (data->db_version != CLOUDSYNC_VALUE_NOTSET && rc == DBVM_VALUE_UNCHANGED) return 0;
    
    return cloudsync_dbversion_rerun(data);
}

int64_t cloudsync_dbversion_next (cloudsync_context *data, int64_t merging_version) {
    int rc = cloudsync_dbversion_check_uptodate(data);
    if (rc != DBRES_OK) return -1;
    
    int64_t result = data->db_version + 1;
    if (result < data->pending_db_version) result = data->pending_db_version;
    if (merging_version != CLOUDSYNC_VALUE_NOTSET && result < merging_version) result = merging_version;
    data->pending_db_version = result;
    
    return result;
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

// MARK: - CloudSync Context -

int cloudsync_insync (cloudsync_context *data) {
    return data->insync;
}

void *cloudsync_siteid (cloudsync_context *data) {
    return (void *)data->site_id;
}

void cloudsync_reset_siteid (cloudsync_context *data) {
    memset(data->site_id, 0, sizeof(uint8_t) * UUID_LEN);
}

int cloudsync_load_siteid (cloudsync_context *data) {
    // check if site_id was already loaded
    if (data->site_id[0] != 0) return DBRES_OK;
    
    // load site_id
    char *buffer = NULL;
    int64_t size = 0;
    int rc = database_select_blob(data, SQL_SITEID_SELECT_ROWID0, &buffer, &size);
    if (rc != DBRES_OK) return rc;
    if (!buffer || size != UUID_LEN) {
        if (buffer) cloudsync_memory_free(buffer);
        return cloudsync_set_error(data, "Unable to retrieve siteid", DBRES_MISUSE);
    }
    
    memcpy(data->site_id, buffer, UUID_LEN);
    cloudsync_memory_free(buffer);
    
    return DBRES_OK;
}

int64_t cloudsync_dbversion (cloudsync_context *data) {
    return data->db_version;
}

int cloudsync_bumpseq (cloudsync_context *data) {
    int value = data->seq;
    data->seq += 1;
    return value;
}

void cloudsync_update_schema_hash (cloudsync_context *data) {
    database_update_schema_hash(data, &data->schema_hash);
}

void *cloudsync_db (cloudsync_context *data) {
    return data->db;
}

int cloudsync_add_dbvms (cloudsync_context *data) {
    DEBUG_DBFUNCTION("cloudsync_add_stmts");
    
    if (data->data_version_stmt == NULL) {
        int rc = databasevm_prepare(data, SQL_DATA_VERSION, (void **)&data->data_version_stmt, DBFLAG_PERSISTENT);
        DEBUG_STMT("data_version_stmt %p", data->data_version_stmt);
        if (rc != DBRES_OK) return rc;
        DEBUG_SQL("data_version_stmt: %s", SQL_DATA_VERSION);
    }
    
    if (data->schema_version_stmt == NULL) {
        int rc = databasevm_prepare(data, SQL_SCHEMA_VERSION, (void **)&data->schema_version_stmt, DBFLAG_PERSISTENT);
        DEBUG_STMT("schema_version_stmt %p", data->schema_version_stmt);
        if (rc != DBRES_OK) return rc;
        DEBUG_SQL("schema_version_stmt: %s", SQL_SCHEMA_VERSION);
    }
    
    if (data->getset_siteid_stmt == NULL) {
        // get and set index of the site_id
        // in SQLite, we can’t directly combine an INSERT and a SELECT to both insert a row and return an identifier (rowid) in a single statement,
        // however, we can use a workaround by leveraging the INSERT statement with ON CONFLICT DO UPDATE and then combining it with RETURNING rowid
        int rc = databasevm_prepare(data, SQL_SITEID_GETSET_ROWID_BY_SITEID, (void **)&data->getset_siteid_stmt, DBFLAG_PERSISTENT);
        DEBUG_STMT("getset_siteid_stmt %p", data->getset_siteid_stmt);
        if (rc != DBRES_OK) return rc;
        DEBUG_SQL("getset_siteid_stmt: %s", SQL_SITEID_GETSET_ROWID_BY_SITEID);
    }
    
    return cloudsync_dbversion_rebuild(data);
}

int cloudsync_set_error (cloudsync_context *data, const char *err_user, int err_code) {
    // force err_code to be something different than OK
    if (err_code == DBRES_OK) err_code = database_errcode(data);
    if (err_code == DBRES_OK) err_code = DBRES_ERROR;
    
    // compute a meaningful error message
    if (err_user == NULL) {
        snprintf(data->errmsg, sizeof(data->errmsg), "%s", database_errmsg(data));
    } else {
        const char *db_error = database_errmsg(data);
        char db_error_copy[sizeof(data->errmsg)];
        int rc = database_errcode(data);
        if (rc == DBRES_OK) {
            snprintf(data->errmsg, sizeof(data->errmsg), "%s", err_user);
        } else {
            if (db_error == data->errmsg) {
                snprintf(db_error_copy, sizeof(db_error_copy), "%s", db_error);
                db_error = db_error_copy;
            }
            snprintf(data->errmsg, sizeof(data->errmsg), "%s (%s)", err_user, db_error);
        }
    }
    
    data->errcode = err_code;
    return err_code;
}

int cloudsync_set_dberror (cloudsync_context *data) {
    return cloudsync_set_error(data, NULL, DBRES_OK);
}

const char *cloudsync_errmsg (cloudsync_context *data) {
    return data->errmsg;
}

int cloudsync_errcode (cloudsync_context *data) {
    return data->errcode;
}

void cloudsync_reset_error (cloudsync_context *data) {
    data->errmsg[0] = 0;
    data->errcode = DBRES_OK;
}

void *cloudsync_auxdata (cloudsync_context *data) {
    return data->aux_data;
}

void cloudsync_set_auxdata (cloudsync_context *data, void *xdata) {
    data->aux_data = xdata;
}

void cloudsync_set_schema (cloudsync_context *data, const char *schema) {
    if (data->current_schema && schema && strcmp(data->current_schema, schema) == 0) return;
    if (data->current_schema) cloudsync_memory_free(data->current_schema);
    data->current_schema = NULL;
    if (schema) data->current_schema = cloudsync_string_dup_lowercase(schema);
}

const char *cloudsync_schema (cloudsync_context *data) {
    return data->current_schema;
}

const char *cloudsync_table_schema (cloudsync_context *data, const char *table_name) {
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) return NULL;

    return table->schema;
}

// MARK: - Table Utils -

void table_pknames_free (char **names, int nrows) {
    if (!names) return;
    for (int i = 0; i < nrows; ++i) {cloudsync_memory_free(names[i]);}
    cloudsync_memory_free(names);
}

char *table_build_mergedelete_sql (cloudsync_table_context *table) {
    #if !CLOUDSYNC_DISABLE_ROWIDONLY_TABLES
    if (table->rowid_only) {
        char *sql = memory_mprintf(SQL_DELETE_ROW_BY_ROWID, table->name);
        return sql;
    }
    #endif

    return sql_build_delete_by_pk(table->context, table->name, table->schema);
}

char *table_build_mergeinsert_sql (cloudsync_table_context *table, const char *colname) {
    char *sql = NULL;
    
    #if !CLOUDSYNC_DISABLE_ROWIDONLY_TABLES
    if (table->rowid_only) {
        if (colname == NULL) {
            // INSERT OR IGNORE INTO customers (first_name,last_name) VALUES (?,?);
            sql = memory_mprintf(SQL_INSERT_ROWID_IGNORE, table->name);
        } else {
            // INSERT INTO customers (first_name,last_name,age) VALUES (?,?,?) ON CONFLICT DO UPDATE SET age=?;
            sql = memory_mprintf(SQL_UPSERT_ROWID_AND_COL_BY_ROWID, table->name, colname, colname);
        }
        return sql;
    }
    #endif
    
    if (colname == NULL) {
        // is sentinel insert
        sql = sql_build_insert_pk_ignore(table->context, table->name, table->schema);
    } else {
        sql = sql_build_upsert_pk_and_col(table->context, table->name, colname, table->schema);
    }
    return sql;
}

char *table_build_value_sql (cloudsync_table_context *table, const char *colname) {
    #if !CLOUDSYNC_DISABLE_ROWIDONLY_TABLES
    if (table->rowid_only) {
        char *colnamequote = "\"";
        char *sql = memory_mprintf(SQL_SELECT_COLS_BY_ROWID_FMT, colnamequote, colname, colnamequote, table->name);
        return sql;
    }
    #endif
        
    // SELECT age FROM customers WHERE first_name=? AND last_name=?;
    return sql_build_select_cols_by_pk(table->context, table->name, colname, table->schema);
}
    
cloudsync_table_context *table_create (cloudsync_context *data, const char *name, table_algo algo) {
    DEBUG_DBFUNCTION("table_create %s", name);
    
    cloudsync_table_context *table = (cloudsync_table_context *)cloudsync_memory_zeroalloc(sizeof(cloudsync_table_context));
    if (!table) return NULL;
    
    table->context = data;
    table->algo = algo;
    table->name = cloudsync_string_dup_lowercase(name);

    // Detect schema from metadata table location. If metadata table doesn't
    // exist yet (during initialization), fall back to cloudsync_schema() which
    // returns the explicitly set schema or current_schema().
    table->schema = database_table_schema(name);
    if (!table->schema) {
        const char *fallback_schema = cloudsync_schema(data);
        if (fallback_schema) {
            table->schema = cloudsync_string_dup(fallback_schema);
        }
    }

    if (!table->name) {
        cloudsync_memory_free(table);
        return NULL;
    }
    table->meta_ref = database_build_meta_ref(table->schema, table->name);
    table->base_ref = database_build_base_ref(table->schema, table->name);
    table->enabled = true;
        
    return table;
}

void table_free (cloudsync_table_context *table) {
    DEBUG_DBFUNCTION("table_free %s", (table) ? (table->name) : "NULL");
    if (!table) return;
    
    if (table->col_name) {
        for (int i=0; i<table->ncols; ++i) {
            cloudsync_memory_free(table->col_name[i]);
        }
        cloudsync_memory_free(table->col_name);
    }
    if (table->col_merge_stmt) {
        for (int i=0; i<table->ncols; ++i) {
            databasevm_finalize(table->col_merge_stmt[i]);
        }
        cloudsync_memory_free(table->col_merge_stmt);
    }
    if (table->col_value_stmt) {
        for (int i=0; i<table->ncols; ++i) {
            databasevm_finalize(table->col_value_stmt[i]);
        }
        cloudsync_memory_free(table->col_value_stmt);
    }
    if (table->col_id) {
        cloudsync_memory_free(table->col_id);
    }
    if (table->col_algo) {
        cloudsync_memory_free(table->col_algo);
    }
    if (table->col_delimiter) {
        for (int i=0; i<table->ncols; ++i) {
            if (table->col_delimiter[i]) cloudsync_memory_free(table->col_delimiter[i]);
        }
        cloudsync_memory_free(table->col_delimiter);
    }

    if (table->block_value_read_stmt) databasevm_finalize(table->block_value_read_stmt);
    if (table->block_value_write_stmt) databasevm_finalize(table->block_value_write_stmt);
    if (table->block_value_delete_stmt) databasevm_finalize(table->block_value_delete_stmt);
    if (table->block_list_stmt) databasevm_finalize(table->block_list_stmt);
    if (table->blocks_ref) cloudsync_memory_free(table->blocks_ref);

    if (table->name) cloudsync_memory_free(table->name);
    if (table->schema) cloudsync_memory_free(table->schema);
    if (table->meta_ref) cloudsync_memory_free(table->meta_ref);
    if (table->base_ref) cloudsync_memory_free(table->base_ref);
    if (table->pk_name) table_pknames_free(table->pk_name, table->npks);
    if (table->meta_pkexists_stmt) databasevm_finalize(table->meta_pkexists_stmt);
    if (table->meta_sentinel_update_stmt) databasevm_finalize(table->meta_sentinel_update_stmt);
    if (table->meta_sentinel_insert_stmt) databasevm_finalize(table->meta_sentinel_insert_stmt);
    if (table->meta_row_insert_update_stmt) databasevm_finalize(table->meta_row_insert_update_stmt);
    if (table->meta_row_drop_stmt) databasevm_finalize(table->meta_row_drop_stmt);
    if (table->meta_update_move_stmt) databasevm_finalize(table->meta_update_move_stmt);
    if (table->meta_local_cl_stmt) databasevm_finalize(table->meta_local_cl_stmt);
    if (table->meta_winner_clock_stmt) databasevm_finalize(table->meta_winner_clock_stmt);
    if (table->meta_merge_delete_drop) databasevm_finalize(table->meta_merge_delete_drop);
    if (table->meta_zero_clock_stmt) databasevm_finalize(table->meta_zero_clock_stmt);
    if (table->meta_col_version_stmt) databasevm_finalize(table->meta_col_version_stmt);
    if (table->meta_site_id_stmt) databasevm_finalize(table->meta_site_id_stmt);
    
    if (table->real_col_values_stmt) databasevm_finalize(table->real_col_values_stmt);
    if (table->real_merge_delete_stmt) databasevm_finalize(table->real_merge_delete_stmt);
    if (table->real_merge_sentinel_stmt) databasevm_finalize(table->real_merge_sentinel_stmt);
    
    cloudsync_memory_free(table);
}

int table_add_stmts (cloudsync_table_context *table, int ncols) {
    int rc = DBRES_OK;
    char *sql = NULL;
    cloudsync_context *data = table->context;
    
    // META TABLE statements
    
    // CREATE TABLE IF NOT EXISTS \"%w_cloudsync\" (pk BLOB NOT NULL, col_name TEXT NOT NULL, col_version INTEGER, db_version INTEGER, site_id INTEGER DEFAULT 0, seq INTEGER, PRIMARY KEY (pk, col_name));
    
    // precompile the pk exists statement
    // we do not need an index on the pk column because it is already covered by the fact that it is part of the prikeys
    // EXPLAIN QUERY PLAN reports: SEARCH table_name USING PRIMARY KEY (pk=?)
    sql = cloudsync_memory_mprintf(SQL_CLOUDSYNC_ROW_EXISTS_BY_PK, table->meta_ref);
    if (!sql) {rc = DBRES_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_pkexists_stmt: %s", sql);

    rc = databasevm_prepare(data, sql, (void **)&table->meta_pkexists_stmt, DBFLAG_PERSISTENT);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto cleanup;

    // precompile the update local sentinel statement
    sql = cloudsync_memory_mprintf(SQL_CLOUDSYNC_UPDATE_COL_BUMP_VERSION, table->meta_ref, CLOUDSYNC_TOMBSTONE_VALUE);
    if (!sql) {rc = DBRES_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_sentinel_update_stmt: %s", sql);
    
    rc = databasevm_prepare(data, sql, (void **)&table->meta_sentinel_update_stmt, DBFLAG_PERSISTENT);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto cleanup;
    
    // precompile the insert local sentinel statement
    sql = cloudsync_memory_mprintf(SQL_CLOUDSYNC_UPSERT_COL_INIT_OR_BUMP_VERSION, table->meta_ref, CLOUDSYNC_TOMBSTONE_VALUE, table->meta_ref, table->meta_ref, table->meta_ref);
    if (!sql) {rc = DBRES_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_sentinel_insert_stmt: %s", sql);
    
    rc = databasevm_prepare(data, sql, (void **)&table->meta_sentinel_insert_stmt, DBFLAG_PERSISTENT);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto cleanup;

    // precompile the insert/update local row statement
    sql = cloudsync_memory_mprintf(SQL_CLOUDSYNC_UPSERT_RAW_COLVERSION, table->meta_ref, table->meta_ref);
    if (!sql) {rc = DBRES_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_row_insert_update_stmt: %s", sql);
    
    rc = databasevm_prepare(data, sql, (void **)&table->meta_row_insert_update_stmt, DBFLAG_PERSISTENT);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto cleanup;
    
    // precompile the delete rows from meta
    sql = cloudsync_memory_mprintf(SQL_CLOUDSYNC_DELETE_PK_EXCEPT_COL, table->meta_ref, CLOUDSYNC_TOMBSTONE_VALUE);
    if (!sql) {rc = DBRES_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_row_drop_stmt: %s", sql);

    rc = databasevm_prepare(data, sql, (void **)&table->meta_row_drop_stmt, DBFLAG_PERSISTENT);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto cleanup;
    
    // precompile the update rows from meta when pk changes
    // see https://github.com/sqliteai/sqlite-sync/blob/main/docs/PriKey.md for more details
    sql = sql_build_rekey_pk_and_reset_version_except_col(data, table->name, CLOUDSYNC_TOMBSTONE_VALUE);
    if (!sql) {rc = DBRES_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_update_move_stmt: %s", sql);
    
    rc = databasevm_prepare(data, sql, (void **)&table->meta_update_move_stmt, DBFLAG_PERSISTENT);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto cleanup;
    
    // local cl
    sql = cloudsync_memory_mprintf(SQL_CLOUDSYNC_GET_COL_VERSION_OR_ROW_EXISTS, table->meta_ref, CLOUDSYNC_TOMBSTONE_VALUE, table->meta_ref);
    if (!sql) {rc = DBRES_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_local_cl_stmt: %s", sql);
    
    rc = databasevm_prepare(data, sql, (void **)&table->meta_local_cl_stmt, DBFLAG_PERSISTENT);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto cleanup;
    
    // rowid of the last inserted/updated row in the meta table
    sql = cloudsync_memory_mprintf(SQL_CLOUDSYNC_INSERT_RETURN_CHANGE_ID, table->meta_ref);
    if (!sql) {rc = DBRES_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_winner_clock_stmt: %s", sql);
    
    rc = databasevm_prepare(data, sql, (void **)&table->meta_winner_clock_stmt, DBFLAG_PERSISTENT);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto cleanup;
    
    sql = cloudsync_memory_mprintf(SQL_CLOUDSYNC_DELETE_PK_EXCEPT_COL, table->meta_ref, CLOUDSYNC_TOMBSTONE_VALUE);
    if (!sql) {rc = DBRES_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_merge_delete_drop: %s", sql);

    rc = databasevm_prepare(data, sql, (void **)&table->meta_merge_delete_drop, DBFLAG_PERSISTENT);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto cleanup;
    
    // zero clock
    sql = cloudsync_memory_mprintf(SQL_CLOUDSYNC_TOMBSTONE_PK_EXCEPT_COL, table->meta_ref, CLOUDSYNC_TOMBSTONE_VALUE);
    if (!sql) {rc = DBRES_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_zero_clock_stmt: %s", sql);
    
    rc = databasevm_prepare(data, sql, (void **)&table->meta_zero_clock_stmt, DBFLAG_PERSISTENT);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto cleanup;
    
    // col_version
    sql = cloudsync_memory_mprintf(SQL_CLOUDSYNC_SELECT_COL_VERSION_BY_PK_COL, table->meta_ref);
    if (!sql) {rc = DBRES_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_col_version_stmt: %s", sql);
    
    rc = databasevm_prepare(data, sql, (void **)&table->meta_col_version_stmt, DBFLAG_PERSISTENT);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto cleanup;
    
    // site_id
    sql = cloudsync_memory_mprintf(SQL_CLOUDSYNC_SELECT_SITE_ID_BY_PK_COL, table->meta_ref);
    if (!sql) {rc = DBRES_NOMEM; goto cleanup;}
    DEBUG_SQL("meta_site_id_stmt: %s", sql);
    
    rc = databasevm_prepare(data, sql, (void **)&table->meta_site_id_stmt, DBFLAG_PERSISTENT);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto cleanup;
    
    // REAL TABLE statements

    // precompile the get column value statement
    if (ncols > 0) {
        sql = sql_build_select_nonpk_by_pk(data, table->name, table->schema);
        if (!sql) {rc = DBRES_NOMEM; goto cleanup;}
        DEBUG_SQL("real_col_values_stmt: %s", sql);
        
        rc = databasevm_prepare(data, sql, (void **)&table->real_col_values_stmt, DBFLAG_PERSISTENT);
        cloudsync_memory_free(sql);
        if (rc != DBRES_OK) goto cleanup;
    }
    
    sql = table_build_mergedelete_sql(table);
    if (!sql) {rc = DBRES_NOMEM; goto cleanup;}
    DEBUG_SQL("real_merge_delete: %s", sql);
    
    rc = databasevm_prepare(data, sql, (void **)&table->real_merge_delete_stmt, DBFLAG_PERSISTENT);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto cleanup;
    
    sql = table_build_mergeinsert_sql(table, NULL);
    if (!sql) {rc = DBRES_NOMEM; goto cleanup;}
    DEBUG_SQL("real_merge_sentinel: %s", sql);
    
    rc = databasevm_prepare(data, sql, (void **)&table->real_merge_sentinel_stmt, DBFLAG_PERSISTENT);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto cleanup;
    
cleanup:
    if (rc != DBRES_OK) DEBUG_ALWAYS("table_add_stmts error: %d %s\n", rc, database_errmsg(data));
    return rc;
}

cloudsync_table_context *table_lookup (cloudsync_context *data, const char *table_name) {
    DEBUG_DBFUNCTION("table_lookup %s", table_name);
    
    if (table_name) {
        for (int i=0; i<data->tables_count; ++i) {
            if ((strcasecmp(data->tables[i]->name, table_name) == 0)) return data->tables[i];
        }
    }
    
    return NULL;
}

void *table_column_lookup (cloudsync_table_context *table, const char *col_name, bool is_merge, int *index) {
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

int table_remove (cloudsync_context *data, cloudsync_table_context *table) {
    const char *table_name = table->name;
    DEBUG_DBFUNCTION("table_remove %s", table_name);
    
    for (int i = 0; i < data->tables_count; ++i) {
        cloudsync_table_context *t = data->tables[i];
        
        // pointer compare is fastest but fallback to strcasecmp if not same pointer
        if ((t == table) || ((strcasecmp(t->name, table_name) == 0))) {
            int last = data->tables_count - 1;
            data->tables[i] = data->tables[last];   // move last into the hole (keeps array dense)
            data->tables[last] = NULL;              // NULLify tail (as an extra security measure)
            data->tables_count--;
            return data->tables_count;
        }
    }
    
    return -1;
}

int table_add_to_context_cb (void *xdata, int ncols, char **values, char **names) {
    cloudsync_table_context *table = (cloudsync_table_context *)xdata;
    cloudsync_context *data = table->context;

    int index = table->ncols;
    for (int i=0; i<ncols; i+=2) {
        const char *name = values[i];
        int cid = (int)strtol(values[i+1], NULL, 0);

        table->col_id[index] = cid;
        table->col_name[index] = cloudsync_string_dup_lowercase(name);
        if (!table->col_name[index]) goto error;

        char *sql = table_build_mergeinsert_sql(table, name);
        if (!sql) goto error;
        DEBUG_SQL("col_merge_stmt[%d]: %s", index, sql);

        int rc = databasevm_prepare(data, sql, (void **)&table->col_merge_stmt[index], DBFLAG_PERSISTENT);
        cloudsync_memory_free(sql);
        if (rc != DBRES_OK) goto error;
        if (!table->col_merge_stmt[index]) goto error;

        sql = table_build_value_sql(table, name);
        if (!sql) goto error;
        DEBUG_SQL("col_value_stmt[%d]: %s", index, sql);

        rc = databasevm_prepare(data, sql, (void **)&table->col_value_stmt[index], DBFLAG_PERSISTENT);
        cloudsync_memory_free(sql);
        if (rc != DBRES_OK) goto error;
        if (!table->col_value_stmt[index]) goto error;
    }
    table->ncols += 1;

    return 0;

error:
    // clean up partially-initialized entry at index
    if (table->col_name[index]) {cloudsync_memory_free(table->col_name[index]); table->col_name[index] = NULL;}
    if (table->col_merge_stmt[index]) {databasevm_finalize(table->col_merge_stmt[index]); table->col_merge_stmt[index] = NULL;}
    if (table->col_value_stmt[index]) {databasevm_finalize(table->col_value_stmt[index]); table->col_value_stmt[index] = NULL;}
    return 1;
}

bool table_ensure_capacity (cloudsync_context *data) {
    if (data->tables_count < data->tables_cap) return true;
    
    int new_cap = data->tables_cap ? data->tables_cap * 2 : CLOUDSYNC_INIT_NTABLES;
    size_t bytes = (size_t)new_cap * sizeof(*data->tables);
    void *p = cloudsync_memory_realloc(data->tables, bytes);
    if (!p) return false;
    
    data->tables = (cloudsync_table_context **)p;
    data->tables_cap = new_cap;
    return true;
}

bool table_add_to_context (cloudsync_context *data, table_algo algo, const char *table_name) {
    DEBUG_DBFUNCTION("cloudsync_context_add_table %s", table_name);

    // Check if table already initialized in this connection's context.
    // Note: This prevents same-connection duplicate initialization.
    // SQLite clients cannot distinguish schemas, so having 'public.users'
    // and 'auth.users' would cause sync ambiguity. Users should avoid
    // initializing tables with the same name in different schemas.
    // If two concurrent connections initialize tables with the same name
    // in different schemas, the behavior is undefined.
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (table) return true;
    
    // check for space availability
    if (!table_ensure_capacity(data)) return false;
    
    // setup a new table
    table = table_create(data, table_name, algo);
    if (!table) return false;
    
    // fill remaining metadata in the table
    int count = database_count_pk(data, table_name, false, table->schema);
    if (count < 0) {cloudsync_set_dberror(data); goto abort_add_table;}
    table->npks = count;
    if (table->npks == 0) {
        #if CLOUDSYNC_DISABLE_ROWIDONLY_TABLES
        goto abort_add_table;
        #else
        table->rowid_only = true;
        table->npks = 1; // rowid
        #endif
    }
    
    int ncols = database_count_nonpk(data, table_name, table->schema);
    if (ncols < 0) {cloudsync_set_dberror(data); goto abort_add_table;}
    int rc = table_add_stmts(table, ncols);
    if (rc != DBRES_OK) goto abort_add_table;
    
    // a table with only pk(s) is totally legal
    if (ncols > 0) {
        table->col_name = (char **)cloudsync_memory_zeroalloc((uint64_t)(sizeof(char *) * ncols));
        if (!table->col_name) goto abort_add_table;

        table->col_id = (int *)cloudsync_memory_zeroalloc((uint64_t)(sizeof(int) * ncols));
        if (!table->col_id) goto abort_add_table;

        table->col_merge_stmt = (dbvm_t **)cloudsync_memory_zeroalloc((uint64_t)(sizeof(void *) * ncols));
        if (!table->col_merge_stmt) goto abort_add_table;

        table->col_value_stmt = (dbvm_t **)cloudsync_memory_zeroalloc((uint64_t)(sizeof(void *) * ncols));
        if (!table->col_value_stmt) goto abort_add_table;

        table->col_algo = (col_algo_t *)cloudsync_memory_zeroalloc((uint64_t)(sizeof(col_algo_t) * ncols));
        if (!table->col_algo) goto abort_add_table;

        table->col_delimiter = (char **)cloudsync_memory_zeroalloc((uint64_t)(sizeof(char *) * ncols));
        if (!table->col_delimiter) goto abort_add_table;

        // Pass empty string when schema is NULL; SQL will fall back to current_schema()
        const char *schema = table->schema ? table->schema : "";
        char *sql = cloudsync_memory_mprintf(SQL_PRAGMA_TABLEINFO_LIST_NONPK_NAME_CID,
                                              table_name, schema, table_name, schema);
        if (!sql) goto abort_add_table;
        rc = database_exec_callback(data, sql, table_add_to_context_cb, (void *)table);
        cloudsync_memory_free(sql);
        if (rc == DBRES_ABORT) goto abort_add_table;
    }
    
    // append newly created table
    data->tables[data->tables_count++] = table;
    return true;
    
abort_add_table:
    table_free(table);
    return false;
}

dbvm_t *cloudsync_colvalue_stmt (cloudsync_context *data, const char *tbl_name, bool *persistent) {
    dbvm_t *vm = NULL;
    *persistent = false;

    cloudsync_table_context *table = table_lookup(data, tbl_name);
    if (table) {
        char *col_name = NULL;
        if (table->ncols > 0) {
            col_name = table->col_name[0];
            // retrieve col_value precompiled statement
            vm = table_column_lookup(table, col_name, false, NULL);
            *persistent = true;
        } else {
            char *sql = table_build_value_sql(table, "*");
            databasevm_prepare(data, sql, (void **)&vm, 0);
            cloudsync_memory_free(sql);
            *persistent = false;
        }
    }
    
    return vm;
}

bool table_enabled (cloudsync_table_context *table) {
    return table->enabled;
}

void table_set_enabled (cloudsync_table_context *table, bool value) {
    table->enabled = value;
}

int table_count_cols (cloudsync_table_context *table) {
    return table->ncols;
}

int table_count_pks (cloudsync_table_context *table) {
    return table->npks;
}

const char *table_colname (cloudsync_table_context *table, int index) {
    return table->col_name[index];
}

bool table_pk_exists (cloudsync_table_context *table, const char *value, size_t len) {
    // check if a row with the same primary key already exists
    // if so, this means the row might have been previously deleted (sentinel)
    return (dbvm_count(table->meta_pkexists_stmt, value, len, DBTYPE_BLOB) > 0);
}

char **table_pknames (cloudsync_table_context *table) {
    return table->pk_name;
}

void table_set_pknames (cloudsync_table_context *table, char **pknames) {
    table_pknames_free(table->pk_name, table->npks);
    table->pk_name = pknames;
}

bool table_algo_isgos (cloudsync_table_context *table) {
    return (table->algo == table_algo_crdt_gos);
}

const char *table_schema (cloudsync_table_context *table) {
    return table->schema;
}

// MARK: - Merge Insert -

int64_t merge_get_local_cl (cloudsync_table_context *table, const char *pk, int pklen) {
    dbvm_t *vm = table->meta_local_cl_stmt;
    int64_t result = -1;
    
    int rc = databasevm_bind_blob(vm, 1, (const void *)pk, pklen);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_bind_blob(vm, 2, (const void *)pk, pklen);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_step(vm);
    if (rc == DBRES_ROW) result = database_column_int(vm, 0);
    else if (rc == DBRES_DONE) result = 0;
    
cleanup:
    if (result == -1) cloudsync_set_dberror(table->context);
    dbvm_reset(vm);
    return result;
}

int merge_get_col_version (cloudsync_table_context *table, const char *col_name, const char *pk, int pklen, int64_t *version) {
    dbvm_t *vm = table->meta_col_version_stmt;
    
    int rc = databasevm_bind_blob(vm, 1, (const void *)pk, pklen);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_bind_text(vm, 2, col_name, -1);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_step(vm);
    if (rc == DBRES_ROW) {
        *version = database_column_int(vm, 0);
        rc = DBRES_OK;
    }
    
cleanup:
    if ((rc != DBRES_OK) && (rc != DBRES_DONE)) cloudsync_set_dberror(table->context);
    dbvm_reset(vm);
    return rc;
}

int merge_set_winner_clock (cloudsync_context *data, cloudsync_table_context *table, const char *pk, int pk_len, const char *colname, int64_t col_version, int64_t db_version, const char *site_id, int site_len, int64_t seq, int64_t *rowid) {
    
    // get/set site_id
    dbvm_t *vm = data->getset_siteid_stmt;
    int rc = databasevm_bind_blob(vm, 1, (const void *)site_id, site_len);
    if (rc != DBRES_OK) goto cleanup_merge;
    
    rc = databasevm_step(vm);
    if (rc != DBRES_ROW) goto cleanup_merge;
    
    int64_t ord = database_column_int(vm, 0);
    dbvm_reset(vm);
    
    vm = table->meta_winner_clock_stmt;
    rc = databasevm_bind_blob(vm, 1, (const void *)pk, pk_len);
    if (rc != DBRES_OK) goto cleanup_merge;
    
    rc = databasevm_bind_text(vm, 2, (colname) ? colname : CLOUDSYNC_TOMBSTONE_VALUE, -1);
    if (rc != DBRES_OK) goto cleanup_merge;
    
    rc = databasevm_bind_int(vm, 3, col_version);
    if (rc != DBRES_OK) goto cleanup_merge;
    
    rc = databasevm_bind_int(vm, 4, db_version);
    if (rc != DBRES_OK) goto cleanup_merge;
    
    rc = databasevm_bind_int(vm, 5, seq);
    if (rc != DBRES_OK) goto cleanup_merge;
    
    rc = databasevm_bind_int(vm, 6, ord);
    if (rc != DBRES_OK) goto cleanup_merge;
    
    rc = databasevm_step(vm);
    if (rc == DBRES_ROW) {
        *rowid = database_column_int(vm, 0);
        rc = DBRES_OK;
    }
    
cleanup_merge:
    if (rc != DBRES_OK) cloudsync_set_dberror(data);
    dbvm_reset(vm);
    return rc;
}

// MARK: - Deferred column-batch merge functions -

static int merge_pending_add (cloudsync_context *data, cloudsync_table_context *table, const char *pk, int pklen, const char *col_name, dbvalue_t *col_value, int64_t col_version, int64_t db_version, const char *site_id, int site_len, int64_t seq) {
    merge_pending_batch *batch = data->pending_batch;

    // Store table and PK on first entry
    if (batch->table == NULL) {
        batch->table = table;
        batch->pk = (char *)cloudsync_memory_alloc(pklen);
        if (!batch->pk) return cloudsync_set_error(data, "merge_pending_add: out of memory for pk", DBRES_NOMEM);
        memcpy(batch->pk, pk, pklen);
        batch->pk_len = pklen;
    }

    // Ensure capacity
    if (batch->count >= batch->capacity) {
        int new_cap = batch->capacity ? batch->capacity * 2 : 8;
        merge_pending_entry *new_entries = (merge_pending_entry *)cloudsync_memory_realloc(batch->entries, new_cap * sizeof(merge_pending_entry));
        if (!new_entries) return cloudsync_set_error(data, "merge_pending_add: out of memory for entries", DBRES_NOMEM);
        batch->entries = new_entries;
        batch->capacity = new_cap;
    }

    // Resolve col_name to a stable pointer from the table context
    // (the incoming col_name may point to VM-owned memory that gets freed on reset)
    int col_idx = -1;
    table_column_lookup(table, col_name, true, &col_idx);
    const char *stable_col_name = (col_idx >= 0) ? table_colname(table, col_idx) : NULL;
    if (!stable_col_name) return cloudsync_set_error(data, "merge_pending_add: column not found in table context", DBRES_ERROR);

    merge_pending_entry *e = &batch->entries[batch->count];
    e->col_name = stable_col_name;
    e->col_value = col_value ? (dbvalue_t *)database_value_dup(col_value) : NULL;
    e->col_version = col_version;
    e->db_version = db_version;
    e->site_id_len = (site_len <= (int)sizeof(e->site_id)) ? site_len : (int)sizeof(e->site_id);
    memcpy(e->site_id, site_id, e->site_id_len);
    e->seq = seq;

    batch->count++;
    return DBRES_OK;
}

static void merge_pending_free_entries (merge_pending_batch *batch) {
    if (batch->entries) {
        for (int i = 0; i < batch->count; i++) {
            if (batch->entries[i].col_value) {
                database_value_free(batch->entries[i].col_value);
                batch->entries[i].col_value = NULL;
            }
        }
    }
    if (batch->pk) {
        cloudsync_memory_free(batch->pk);
        batch->pk = NULL;
    }
    batch->table = NULL;
    batch->pk_len = 0;
    batch->cl = 0;
    batch->sentinel_pending = false;
    batch->row_exists = false;
    batch->count = 0;
}

static int merge_flush_pending (cloudsync_context *data) {
    merge_pending_batch *batch = data->pending_batch;
    if (!batch) return DBRES_OK;

    int rc = DBRES_OK;
    bool flush_savepoint = false;

    // Nothing to write — handle sentinel-only case or skip
    if (batch->count == 0 && !(batch->sentinel_pending && batch->table)) {
        goto cleanup;
    }

    // Wrap database operations in a savepoint so that on failure (e.g. RLS
    // denial) the rollback properly releases all executor resources (open
    // relations, snapshots, plan cache) acquired during the failed statement.
    flush_savepoint = (database_begin_savepoint(data, "merge_flush") == DBRES_OK);

    if (batch->count == 0) {
        // Sentinel with no winning columns (PK-only row)
        dbvm_t *vm = batch->table->real_merge_sentinel_stmt;
        rc = pk_decode_prikey(batch->pk, (size_t)batch->pk_len, pk_decode_bind_callback, vm);
        if (rc < 0) {
            cloudsync_set_dberror(data);
            dbvm_reset(vm);
            goto cleanup;
        }
        SYNCBIT_SET(data);
        rc = databasevm_step(vm);
        dbvm_reset(vm);
        SYNCBIT_RESET(data);
        if (rc == DBRES_DONE) rc = DBRES_OK;
        if (rc != DBRES_OK) {
            cloudsync_set_dberror(data);
            goto cleanup;
        }
        goto cleanup;
    }

    // Check if cached prepared statement can be reused
    cloudsync_table_context *table = batch->table;
    dbvm_t *vm = NULL;
    bool cache_hit = false;

    if (batch->cached_vm &&
        batch->cached_row_exists == batch->row_exists &&
        batch->cached_col_count == batch->count) {
        cache_hit = true;
        for (int i = 0; i < batch->count; i++) {
            if (batch->cached_col_names[i] != batch->entries[i].col_name) {
                cache_hit = false;
                break;
            }
        }
    }

    if (cache_hit) {
        vm = batch->cached_vm;
        dbvm_reset(vm);
    } else {
        // Invalidate old cache
        if (batch->cached_vm) {
            databasevm_finalize(batch->cached_vm);
            batch->cached_vm = NULL;
        }

        // Build multi-column SQL
        const char **colnames = (const char **)cloudsync_memory_alloc(batch->count * sizeof(const char *));
        if (!colnames) {
            rc = cloudsync_set_error(data, "merge_flush_pending: out of memory", DBRES_NOMEM);
            goto cleanup;
        }
        for (int i = 0; i < batch->count; i++) {
            colnames[i] = batch->entries[i].col_name;
        }

        char *sql = batch->row_exists
            ? sql_build_update_pk_and_multi_cols(data, table->name, colnames, batch->count, table->schema)
            : sql_build_upsert_pk_and_multi_cols(data, table->name, colnames, batch->count, table->schema);
        cloudsync_memory_free(colnames);

        if (!sql) {
            rc = cloudsync_set_error(data, "merge_flush_pending: unable to build multi-column upsert SQL", DBRES_ERROR);
            goto cleanup;
        }

        rc = databasevm_prepare(data, sql, &vm, 0);
        cloudsync_memory_free(sql);
        if (rc != DBRES_OK) {
            rc = cloudsync_set_error(data, "merge_flush_pending: unable to prepare statement", rc);
            goto cleanup;
        }

        // Update cache
        batch->cached_vm = vm;
        batch->cached_row_exists = batch->row_exists;
        batch->cached_col_count = batch->count;
        // Reallocate cached_col_names if needed
        if (batch->cached_col_count > 0) {
            const char **new_names = (const char **)cloudsync_memory_realloc(
                batch->cached_col_names, batch->count * sizeof(const char *));
            if (new_names) {
                for (int i = 0; i < batch->count; i++) {
                    new_names[i] = batch->entries[i].col_name;
                }
                batch->cached_col_names = new_names;
            }
        }
    }

    // Bind PKs (positions 1..npks)
    int npks = pk_decode_prikey(batch->pk, (size_t)batch->pk_len, pk_decode_bind_callback, vm);
    if (npks < 0) {
        cloudsync_set_dberror(data);
        dbvm_reset(vm);
        rc = DBRES_ERROR;
        goto cleanup;
    }

    // Bind column values (positions npks+1..npks+count)
    for (int i = 0; i < batch->count; i++) {
        merge_pending_entry *e = &batch->entries[i];
        int bind_idx = npks + 1 + i;
        if (e->col_value) {
            rc = databasevm_bind_value(vm, bind_idx, e->col_value);
        } else {
            rc = databasevm_bind_null(vm, bind_idx);
        }
        if (rc != DBRES_OK) {
            cloudsync_set_dberror(data);
            dbvm_reset(vm);
            goto cleanup;
        }
    }

    // Execute with SYNCBIT and GOS handling
    if (table->algo == table_algo_crdt_gos) table->enabled = 0;
    SYNCBIT_SET(data);
    rc = databasevm_step(vm);
    dbvm_reset(vm);
    SYNCBIT_RESET(data);
    if (table->algo == table_algo_crdt_gos) table->enabled = 1;

    if (rc != DBRES_DONE) {
        cloudsync_set_dberror(data);
        goto cleanup;
    }
    rc = DBRES_OK;

    // Call merge_set_winner_clock for each buffered entry
    int64_t rowid = 0;
    for (int i = 0; i < batch->count; i++) {
        merge_pending_entry *e = &batch->entries[i];
        int clock_rc = merge_set_winner_clock(data, table, batch->pk, batch->pk_len,
                                               e->col_name, e->col_version, e->db_version,
                                               (const char *)e->site_id, e->site_id_len,
                                               e->seq, &rowid);
        if (clock_rc != DBRES_OK) {
            rc = clock_rc;
            goto cleanup;
        }
    }

cleanup:
    merge_pending_free_entries(batch);
    if (flush_savepoint) {
        if (rc == DBRES_OK) database_commit_savepoint(data, "merge_flush");
        else database_rollback_savepoint(data, "merge_flush");
    }
    return rc;
}

int merge_insert_col (cloudsync_context *data, cloudsync_table_context *table, const void *pk, int pklen, const char *col_name, dbvalue_t *col_value, int64_t col_version, int64_t db_version, const char *site_id, int site_len, int64_t seq, int64_t *rowid) {
    int index;
    dbvm_t *vm = table_column_lookup(table, col_name, true, &index);
    if (vm == NULL) return cloudsync_set_error(data, "Unable to retrieve column merge precompiled statement in merge_insert_col", DBRES_MISUSE);
    
    // INSERT INTO table (pk1, pk2, col_name) VALUES (?, ?, ?) ON CONFLICT DO UPDATE SET col_name=?;"
    
    // bind primary key(s)
    int rc = pk_decode_prikey((char *)pk, (size_t)pklen, pk_decode_bind_callback, vm);
    if (rc < 0) {
        cloudsync_set_dberror(data);
        dbvm_reset(vm);
        return rc;
    }
    
    // bind value (always bind all expected parameters for correct prepared statement handling)
    if (col_value) {
        rc = databasevm_bind_value(vm, table->npks+1, col_value);
        if (rc == DBRES_OK) rc = databasevm_bind_value(vm, table->npks+2, col_value);
    } else {
        rc = databasevm_bind_null(vm, table->npks+1);
        if (rc == DBRES_OK) rc = databasevm_bind_null(vm, table->npks+2);
    }
    if (rc != DBRES_OK) {
        cloudsync_set_dberror(data);
        dbvm_reset(vm);
        return rc;
    }

    // perform real operation and disable triggers
    
    // in case of GOS we reused the table->col_merge_stmt statement
    // which looks like: INSERT INTO table (pk1, pk2, col_name) VALUES (?, ?, ?) ON CONFLICT DO UPDATE SET col_name=?;"
    // but the UPDATE in the CONFLICT statement would return SQLITE_CONSTRAINT because the trigger raises the error
    // the trick is to disable that trigger before executing the statement
    if (table->algo == table_algo_crdt_gos) table->enabled = 0;
    SYNCBIT_SET(data);
    rc = databasevm_step(vm);
    DEBUG_MERGE("merge_insert(%02x%02x): %s (%d)", data->site_id[UUID_LEN-2], data->site_id[UUID_LEN-1], databasevm_sql(vm), rc);
    dbvm_reset(vm);
    SYNCBIT_RESET(data);
    if (table->algo == table_algo_crdt_gos) table->enabled = 1;
    
    if (rc != DBRES_DONE) {
        cloudsync_set_dberror(data);
        return rc;
    }
    
    return merge_set_winner_clock(data, table, pk, pklen, col_name, col_version, db_version, site_id, site_len, seq, rowid);
}

int merge_delete (cloudsync_context *data, cloudsync_table_context *table, const char *pk, int pklen, const char *colname, int64_t cl, int64_t db_version, const char *site_id, int site_len, int64_t seq, int64_t *rowid) {
    int rc = DBRES_OK;
    
    // reset return value
    *rowid = 0;
    
    // bind pk
    dbvm_t *vm = table->real_merge_delete_stmt;
    rc = pk_decode_prikey((char *)pk, (size_t)pklen, pk_decode_bind_callback, vm);
    if (rc < 0) {
        rc = cloudsync_set_dberror(data);
        dbvm_reset(vm);
        return rc;
    }
    
    // perform real operation and disable triggers
    SYNCBIT_SET(data);
    rc = databasevm_step(vm);
    DEBUG_MERGE("merge_delete(%02x%02x): %s (%d)", data->site_id[UUID_LEN-2], data->site_id[UUID_LEN-1], databasevm_sql(vm), rc);
    dbvm_reset(vm);
    SYNCBIT_RESET(data);
    if (rc == DBRES_DONE) rc = DBRES_OK;
    if (rc != DBRES_OK) {
        cloudsync_set_dberror(data);
        return rc;
    }
    
    rc = merge_set_winner_clock(data, table, pk, pklen, colname, cl, db_version, site_id, site_len, seq, rowid);
    if (rc != DBRES_OK) return rc;
    
    // drop clocks _after_ setting the winner clock so we don't lose track of the max db_version!!
    // this must never come before `set_winner_clock`
    vm = table->meta_merge_delete_drop;
    rc = databasevm_bind_blob(vm, 1, (const void *)pk, pklen);
    if (rc == DBRES_OK) rc = databasevm_step(vm);
    dbvm_reset(vm);
    
    if (rc == DBRES_DONE) rc = DBRES_OK;
    if (rc != DBRES_OK) cloudsync_set_dberror(data);
    return rc;
}

int merge_zeroclock_on_resurrect(cloudsync_table_context *table, int64_t db_version, const char *pk, int pklen) {
    dbvm_t *vm = table->meta_zero_clock_stmt;
    
    int rc = databasevm_bind_int(vm, 1, db_version);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_bind_blob(vm, 2, (const void *)pk, pklen);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_step(vm);
    if (rc == DBRES_DONE) rc = DBRES_OK;
    
cleanup:
    if (rc != DBRES_OK) cloudsync_set_dberror(table->context);
    dbvm_reset(vm);
    return rc;
}

// executed only if insert_cl == local_cl
int merge_did_cid_win (cloudsync_context *data, cloudsync_table_context *table, const char *pk, int pklen, dbvalue_t *insert_value, const char *site_id, int site_len, const char *col_name, int64_t col_version, bool *didwin_flag) {
    
    if (col_name == NULL) col_name = CLOUDSYNC_TOMBSTONE_VALUE;
    
    int64_t local_version;
    int rc = merge_get_col_version(table, col_name, pk, pklen, &local_version);
    if (rc == DBRES_DONE) {
        // no rows returned, the incoming change wins if there's nothing there locally
        *didwin_flag = true;
        return DBRES_OK;
    }
    if (rc != DBRES_OK) return rc;
    
    // rc == DBRES_OK, means that a row with a version exists
    if (local_version != col_version) {
        if (col_version > local_version) {*didwin_flag = true; return DBRES_OK;}
        if (col_version < local_version) {*didwin_flag = false; return DBRES_OK;}
    }
    
    // rc == DBRES_ROW and col_version == local_version, need to compare values

    // retrieve col_value precompiled statement
    bool is_block_col = block_is_block_colname(col_name) && table_has_block_cols(table);
    dbvm_t *vm;
    if (is_block_col) {
        // Block column: read value from blocks table (pk + col_name bindings)
        vm = table_block_value_read_stmt(table);
        if (!vm) return cloudsync_set_error(data, "Unable to retrieve block value read statement in merge_did_cid_win", DBRES_ERROR);
        rc = databasevm_bind_blob(vm, 1, (const void *)pk, pklen);
        if (rc != DBRES_OK) { dbvm_reset(vm); return cloudsync_set_dberror(data); }
        rc = databasevm_bind_text(vm, 2, col_name, -1);
        if (rc != DBRES_OK) { dbvm_reset(vm); return cloudsync_set_dberror(data); }
    } else {
        vm = table_column_lookup(table, col_name, false, NULL);
        if (!vm) return cloudsync_set_error(data, "Unable to retrieve column value precompiled statement in merge_did_cid_win", DBRES_ERROR);

        // bind primary key values
        rc = pk_decode_prikey((char *)pk, (size_t)pklen, pk_decode_bind_callback, (void *)vm);
        if (rc < 0) {
            rc = cloudsync_set_dberror(data);
            dbvm_reset(vm);
            return rc;
        }
    }
        
    // execute vm
    dbvalue_t *local_value;
    rc = databasevm_step(vm);
    if (rc == DBRES_DONE) {
        // meta entry exists but the actual value is missing
        // we should allow the value_compare function to make a decision
        // value_compare has been modified to handle the case where lvalue is NULL
        local_value = NULL;
        rc = DBRES_OK;
    } else if (rc == DBRES_ROW) {
        local_value = database_column_value(vm, 0);
        rc = DBRES_OK;
    } else {
        goto cleanup;
    }
    
    // compare values
    int ret = dbutils_value_compare(insert_value, local_value);
    // reset after compare, otherwise local value would be deallocated
    dbvm_reset(vm);
    vm = NULL;
    
    bool compare_site_id = (ret == 0 && data->merge_equal_values == true);
    if (!compare_site_id) {
        *didwin_flag = (ret > 0);
        goto cleanup;
    }
    
    // values are the same and merge_equal_values is true
    vm = table->meta_site_id_stmt;
    rc = databasevm_bind_blob(vm, 1, (const void *)pk, pklen);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_bind_text(vm, 2, col_name, -1);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_step(vm);
    if (rc == DBRES_ROW) {
        const void *local_site_id = database_column_blob(vm, 0, NULL);
        if (!local_site_id) {
            dbvm_reset(vm);
            return cloudsync_set_error(data, "NULL site_id in cloudsync table, table is probably corrupted", DBRES_ERROR);
        }
        ret = memcmp(site_id, local_site_id, site_len);
        *didwin_flag = (ret > 0);
        dbvm_reset(vm);
        return DBRES_OK;
    }
    
    // handle error condition here
    dbvm_reset(vm);
    return cloudsync_set_error(data, "Unable to find site_id for previous change, cloudsync table is probably corrupted", DBRES_ERROR);
    
cleanup:
    if (rc != DBRES_OK) cloudsync_set_dberror(data);
    dbvm_reset(vm);
    return rc;
}

int merge_sentinel_only_insert (cloudsync_context *data, cloudsync_table_context *table, const char *pk, int pklen, int64_t cl, int64_t db_version, const char *site_id, int site_len, int64_t seq, int64_t *rowid) {

    // reset return value
    *rowid = 0;

    if (data->pending_batch == NULL) {
        // Immediate mode: execute base table INSERT
        dbvm_t *vm = table->real_merge_sentinel_stmt;
        int rc = pk_decode_prikey((char *)pk, (size_t)pklen, pk_decode_bind_callback, vm);
        if (rc < 0) {
            rc = cloudsync_set_dberror(data);
            dbvm_reset(vm);
            return rc;
        }

        SYNCBIT_SET(data);
        rc = databasevm_step(vm);
        dbvm_reset(vm);
        SYNCBIT_RESET(data);
        if (rc == DBRES_DONE) rc = DBRES_OK;
        if (rc != DBRES_OK) {
            cloudsync_set_dberror(data);
            return rc;
        }
    } else {
        // Batch mode: skip base table INSERT, the batch flush will create the row
        merge_pending_batch *batch = data->pending_batch;
        batch->sentinel_pending = true;
        if (batch->table == NULL) {
            batch->table = table;
            batch->pk = (char *)cloudsync_memory_alloc(pklen);
            if (!batch->pk) return cloudsync_set_error(data, "merge_sentinel_only_insert: out of memory for pk", DBRES_NOMEM);
            memcpy(batch->pk, pk, pklen);
            batch->pk_len = pklen;
        }
    }

    // Metadata operations always execute regardless of batch mode
    int rc = merge_zeroclock_on_resurrect(table, db_version, pk, pklen);
    if (rc != DBRES_OK) return rc;

    return merge_set_winner_clock(data, table, pk, pklen, NULL, cl, db_version, site_id, site_len, seq, rowid);
}

// MARK: - Block-level merge helpers -

// Store a block value in the blocks table
static int block_store_value (cloudsync_context *data, cloudsync_table_context *table, const void *pk, int pklen, const char *block_colname, dbvalue_t *col_value) {
    dbvm_t *vm = table->block_value_write_stmt;
    if (!vm) return cloudsync_set_error(data, "block_store_value: blocks table not initialized", DBRES_MISUSE);

    int rc = databasevm_bind_blob(vm, 1, pk, pklen);
    if (rc != DBRES_OK) goto cleanup;
    rc = databasevm_bind_text(vm, 2, block_colname, -1);
    if (rc != DBRES_OK) goto cleanup;
    if (col_value) {
        rc = databasevm_bind_value(vm, 3, col_value);
    } else {
        rc = databasevm_bind_null(vm, 3);
    }
    if (rc != DBRES_OK) goto cleanup;

    rc = databasevm_step(vm);
    if (rc == DBRES_DONE) rc = DBRES_OK;

cleanup:
    if (rc != DBRES_OK) cloudsync_set_dberror(data);
    databasevm_reset(vm);
    return rc;
}

// Delete a block value from the blocks table
static int block_delete_value (cloudsync_context *data, cloudsync_table_context *table, const void *pk, int pklen, const char *block_colname) {
    dbvm_t *vm = table->block_value_delete_stmt;
    if (!vm) return cloudsync_set_error(data, "block_delete_value: blocks table not initialized", DBRES_MISUSE);

    int rc = databasevm_bind_blob(vm, 1, pk, pklen);
    if (rc != DBRES_OK) goto cleanup;
    rc = databasevm_bind_text(vm, 2, block_colname, -1);
    if (rc != DBRES_OK) goto cleanup;

    rc = databasevm_step(vm);
    if (rc == DBRES_DONE) rc = DBRES_OK;

cleanup:
    if (rc != DBRES_OK) cloudsync_set_dberror(data);
    databasevm_reset(vm);
    return rc;
}

// Materialize all alive blocks for a base column into the base table
int block_materialize_column (cloudsync_context *data, cloudsync_table_context *table, const void *pk, int pklen, const char *base_col_name) {
    if (!table->block_list_stmt) return cloudsync_set_error(data, "block_materialize_column: blocks table not initialized", DBRES_MISUSE);

    // Find column index and delimiter
    int col_idx = -1;
    for (int i = 0; i < table->ncols; i++) {
        if (strcasecmp(table->col_name[i], base_col_name) == 0) {
            col_idx = i;
            break;
        }
    }
    if (col_idx < 0) return cloudsync_set_error(data, "block_materialize_column: column not found", DBRES_ERROR);
    const char *delimiter = table->col_delimiter[col_idx] ? table->col_delimiter[col_idx] : BLOCK_DEFAULT_DELIMITER;

    // Build the LIKE pattern for block col_names: "base_col\x1F%"
    char *like_pattern = block_build_colname(base_col_name, "%");
    if (!like_pattern) return DBRES_NOMEM;

    // Query alive blocks from blocks table joined with metadata
    // block_list_stmt: SELECT b.col_value FROM blocks b JOIN meta m
    //   ON b.pk = m.pk AND b.col_name = m.col_name
    //   WHERE b.pk = ? AND b.col_name LIKE ? AND m.col_version % 2 = 1
    //   ORDER BY b.col_name
    dbvm_t *vm = table->block_list_stmt;
    int rc = databasevm_bind_blob(vm, 1, pk, pklen);
    if (rc != DBRES_OK) { cloudsync_memory_free(like_pattern); databasevm_reset(vm); return rc; }
    rc = databasevm_bind_text(vm, 2, like_pattern, -1);
    if (rc != DBRES_OK) { cloudsync_memory_free(like_pattern); databasevm_reset(vm); return rc; }
    // Bind pk again for the join condition (parameter 3)
    rc = databasevm_bind_blob(vm, 3, pk, pklen);
    if (rc != DBRES_OK) { cloudsync_memory_free(like_pattern); databasevm_reset(vm); return rc; }
    rc = databasevm_bind_text(vm, 4, like_pattern, -1);
    if (rc != DBRES_OK) { cloudsync_memory_free(like_pattern); databasevm_reset(vm); return rc; }

    // Collect block values
    const char **block_values = NULL;
    int block_count = 0;
    int block_cap = 0;

    while ((rc = databasevm_step(vm)) == DBRES_ROW) {
        const char *value = database_column_text(vm, 0);
        if (block_count >= block_cap) {
            int new_cap = block_cap ? block_cap * 2 : 16;
            const char **new_arr = (const char **)cloudsync_memory_realloc((void *)block_values, (uint64_t)(new_cap * sizeof(char *)));
            if (!new_arr) { rc = DBRES_NOMEM; break; }
            block_values = new_arr;
            block_cap = new_cap;
        }
        block_values[block_count] = value ? cloudsync_string_dup(value) : cloudsync_string_dup("");
        block_count++;
    }
    databasevm_reset(vm);
    cloudsync_memory_free(like_pattern);

    if (rc != DBRES_DONE && rc != DBRES_OK && rc != DBRES_ROW) {
        // Free collected values
        for (int i = 0; i < block_count; i++) cloudsync_memory_free((void *)block_values[i]);
        if (block_values) cloudsync_memory_free((void *)block_values);
        return cloudsync_set_dberror(data);
    }

    // Materialize text (NULL when no alive blocks)
    char *text = (block_count > 0) ? block_materialize_text(block_values, block_count, delimiter) : NULL;
    for (int i = 0; i < block_count; i++) cloudsync_memory_free((void *)block_values[i]);
    if (block_values) cloudsync_memory_free((void *)block_values);
    if (block_count > 0 && !text) return DBRES_NOMEM;

    // Update the base table column via the col_merge_stmt (with triggers disabled)
    dbvm_t *merge_vm = table->col_merge_stmt[col_idx];
    if (!merge_vm) { cloudsync_memory_free(text); return DBRES_ERROR; }

    // Bind PKs
    rc = pk_decode_prikey((char *)pk, (size_t)pklen, pk_decode_bind_callback, merge_vm);
    if (rc < 0) { cloudsync_memory_free(text); databasevm_reset(merge_vm); return DBRES_ERROR; }

    // Bind the text value twice (INSERT value + ON CONFLICT UPDATE value)
    int npks = table->npks;
    if (text) {
        rc = databasevm_bind_text(merge_vm, npks + 1, text, -1);
        if (rc != DBRES_OK) { cloudsync_memory_free(text); databasevm_reset(merge_vm); return rc; }
        rc = databasevm_bind_text(merge_vm, npks + 2, text, -1);
        if (rc != DBRES_OK) { cloudsync_memory_free(text); databasevm_reset(merge_vm); return rc; }
    } else {
        rc = databasevm_bind_null(merge_vm, npks + 1);
        if (rc != DBRES_OK) { databasevm_reset(merge_vm); return rc; }
        rc = databasevm_bind_null(merge_vm, npks + 2);
        if (rc != DBRES_OK) { databasevm_reset(merge_vm); return rc; }
    }

    // Execute with triggers disabled
    table->enabled = 0;
    SYNCBIT_SET(data);
    rc = databasevm_step(merge_vm);
    databasevm_reset(merge_vm);
    SYNCBIT_RESET(data);
    table->enabled = 1;

    cloudsync_memory_free(text);

    if (rc == DBRES_DONE) rc = DBRES_OK;
    if (rc != DBRES_OK) return cloudsync_set_dberror(data);
    return DBRES_OK;
}

// Accessor for has_block_cols flag
bool table_has_block_cols (cloudsync_table_context *table) {
    return table && table->has_block_cols;
}

// Get block column algo for a given column index
col_algo_t table_col_algo (cloudsync_table_context *table, int index) {
    if (!table || !table->col_algo || index < 0 || index >= table->ncols) return col_algo_normal;
    return table->col_algo[index];
}

// Get block delimiter for a given column index
const char *table_col_delimiter (cloudsync_table_context *table, int index) {
    if (!table || !table->col_delimiter || index < 0 || index >= table->ncols) return BLOCK_DEFAULT_DELIMITER;
    return table->col_delimiter[index] ? table->col_delimiter[index] : BLOCK_DEFAULT_DELIMITER;
}

// Block column struct accessors (for use outside cloudsync.c where struct is opaque)
dbvm_t *table_block_value_read_stmt (cloudsync_table_context *table) { return table ? table->block_value_read_stmt : NULL; }
dbvm_t *table_block_value_write_stmt (cloudsync_table_context *table) { return table ? table->block_value_write_stmt : NULL; }
dbvm_t *table_block_list_stmt (cloudsync_table_context *table) { return table ? table->block_list_stmt : NULL; }
const char *table_blocks_ref (cloudsync_table_context *table) { return table ? table->blocks_ref : NULL; }

void table_set_col_delimiter (cloudsync_table_context *table, int col_idx, const char *delimiter) {
    if (!table || !table->col_delimiter || col_idx < 0 || col_idx >= table->ncols) return;
    if (table->col_delimiter[col_idx]) cloudsync_memory_free(table->col_delimiter[col_idx]);
    table->col_delimiter[col_idx] = delimiter ? cloudsync_string_dup(delimiter) : NULL;
}

// Find column index by name
int table_col_index (cloudsync_table_context *table, const char *col_name) {
    if (!table || !col_name) return -1;
    for (int i = 0; i < table->ncols; i++) {
        if (strcasecmp(table->col_name[i], col_name) == 0) return i;
    }
    return -1;
}

int merge_insert (cloudsync_context *data, cloudsync_table_context *table, const char *insert_pk, int insert_pk_len, int64_t insert_cl, const char *insert_name, dbvalue_t *insert_value, int64_t insert_col_version, int64_t insert_db_version, const char *insert_site_id, int insert_site_id_len, int64_t insert_seq, int64_t *rowid) {
    // Handle DWS and AWS algorithms here
    // Delete-Wins Set (DWS): table_algo_crdt_dws
    // Add-Wins Set (AWS): table_algo_crdt_aws
    
    // Causal-Length Set (CLS) Algorithm (default)
    
    // compute the local causal length for the row based on the primary key
    // the causal length is used to determine the order of operations and resolve conflicts.
    int64_t local_cl = merge_get_local_cl(table, insert_pk, insert_pk_len);
    if (local_cl < 0) return cloudsync_set_error(data, "Unable to compute local causal length", DBRES_ERROR);
    
    // if the incoming causal length is older than the local causal length, we can safely ignore it
    // because the local changes are more recent
    if (insert_cl < local_cl) return DBRES_OK;
    
    // check if the operation is a delete by examining the causal length
    // even causal lengths typically signify delete operations
    bool is_delete = (insert_cl % 2 == 0);
    if (is_delete) {
        // if it's a delete, check if the local state is at the same causal length
        // if it is, no further action is needed
        if (local_cl == insert_cl) return DBRES_OK;
        
        // perform a delete merge if the causal length is newer than the local one
        int rc = merge_delete(data, table, insert_pk, insert_pk_len, insert_name, insert_col_version,
                              insert_db_version, insert_site_id, insert_site_id_len, insert_seq, rowid);
        if (rc != DBRES_OK) cloudsync_set_error(data, "Unable to perform merge_delete", rc);
        return rc;
    }
    
    // if the operation is a sentinel-only insert (indicating a new row or resurrected row with no column update), handle it separately.
    bool is_sentinel_only = (strcmp(insert_name, CLOUDSYNC_TOMBSTONE_VALUE) == 0);
    if (is_sentinel_only) {
        if (local_cl == insert_cl) return DBRES_OK;
        
        // perform a sentinel-only insert to track the existence of the row
        int rc = merge_sentinel_only_insert(data, table, insert_pk, insert_pk_len, insert_col_version,
                                            insert_db_version, insert_site_id, insert_site_id_len, insert_seq, rowid);
        if (rc != DBRES_OK) cloudsync_set_error(data, "Unable to perform merge_sentinel_only_insert", rc);
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
                                            insert_db_version, insert_site_id, insert_site_id_len, insert_seq, rowid);
        if (rc != DBRES_OK) return cloudsync_set_error(data, "Unable to perform merge_sentinel_only_insert", rc);
    }
    
    // at this point, we determine whether the incoming change wins based on causal length
    // this can be due to a resurrection, a non-existent local row, or a conflict resolution
    bool flag = false;
    int rc = merge_did_cid_win(data, table, insert_pk, insert_pk_len, insert_value, insert_site_id, insert_site_id_len, insert_name, insert_col_version, &flag);
    if (rc != DBRES_OK) return cloudsync_set_error(data, "Unable to perform merge_did_cid_win", rc);
    
    // check if the incoming change wins and should be applied
    bool does_cid_win = ((needs_resurrect) || (!row_exists_locally) || (flag));
    if (!does_cid_win) return DBRES_OK;

    // Block-level LWW: if the incoming col_name is a block entry (contains \x1F),
    // bypass the normal base-table write and instead store the value in the blocks table.
    // The base table column will be materialized from all alive blocks.
    if (block_is_block_colname(insert_name) && table->has_block_cols) {
        // Store or delete block value in blocks table depending on tombstone status
        if (insert_col_version % 2 == 0) {
            // Tombstone: remove from blocks table
            rc = block_delete_value(data, table, insert_pk, insert_pk_len, insert_name);
        } else {
            rc = block_store_value(data, table, insert_pk, insert_pk_len, insert_name, insert_value);
        }
        if (rc != DBRES_OK) return cloudsync_set_error(data, "Unable to store/delete block value", rc);

        // Set winner clock in metadata
        rc = merge_set_winner_clock(data, table, insert_pk, insert_pk_len, insert_name,
                                    insert_col_version, insert_db_version,
                                    insert_site_id, insert_site_id_len, insert_seq, rowid);
        if (rc != DBRES_OK) return cloudsync_set_error(data, "Unable to set winner clock for block", rc);

        // Materialize the full column from blocks into the base table
        char *base_col = block_extract_base_colname(insert_name);
        if (base_col) {
            rc = block_materialize_column(data, table, insert_pk, insert_pk_len, base_col);
            cloudsync_memory_free(base_col);
            if (rc != DBRES_OK) return cloudsync_set_error(data, "Unable to materialize block column", rc);
        }

        return DBRES_OK;
    }

    // perform the final column insert or update if the incoming change wins
    if (data->pending_batch) {
        // Propagate row_exists_locally to the batch on the first winning column.
        // This lets merge_flush_pending choose UPDATE vs INSERT ON CONFLICT,
        // which matters when RLS policies reference columns not in the payload.
        if (data->pending_batch->table == NULL) {
            data->pending_batch->row_exists = row_exists_locally;
        }
        rc = merge_pending_add(data, table, insert_pk, insert_pk_len, insert_name, insert_value, insert_col_version, insert_db_version, insert_site_id, insert_site_id_len, insert_seq);
        if (rc != DBRES_OK) cloudsync_set_error(data, "Unable to perform merge_pending_add", rc);
    } else {
        rc = merge_insert_col(data, table, insert_pk, insert_pk_len, insert_name, insert_value, insert_col_version, insert_db_version, insert_site_id, insert_site_id_len, insert_seq, rowid);
        if (rc != DBRES_OK) cloudsync_set_error(data, "Unable to perform merge_insert_col", rc);
    }

    return rc;
}

// MARK: - Block column setup -

int cloudsync_setup_block_column (cloudsync_context *data, const char *table_name, const char *col_name, const char *delimiter) {
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) return cloudsync_set_error(data, "cloudsync_setup_block_column: table not found", DBRES_ERROR);

    // Find column index
    int col_idx = table_col_index(table, col_name);
    if (col_idx < 0) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "cloudsync_setup_block_column: column '%s' not found in table '%s'", col_name, table_name);
        return cloudsync_set_error(data, buf, DBRES_ERROR);
    }

    // Set column algo
    table->col_algo[col_idx] = col_algo_block;
    table->has_block_cols = true;

    // Set delimiter (can be NULL for default)
    if (table->col_delimiter[col_idx]) {
        cloudsync_memory_free(table->col_delimiter[col_idx]);
        table->col_delimiter[col_idx] = NULL;
    }
    if (delimiter) {
        table->col_delimiter[col_idx] = cloudsync_string_dup(delimiter);
    }

    // Create blocks table if not already done
    if (!table->blocks_ref) {
        table->blocks_ref = database_build_blocks_ref(table->schema, table->name);
        if (!table->blocks_ref) return DBRES_NOMEM;

        // CREATE TABLE IF NOT EXISTS
        char *sql = cloudsync_memory_mprintf(SQL_BLOCKS_CREATE_TABLE, table->blocks_ref);
        if (!sql) return DBRES_NOMEM;

        int rc = database_exec(data, sql);
        cloudsync_memory_free(sql);
        if (rc != DBRES_OK) return cloudsync_set_error(data, "Unable to create blocks table", rc);

        // Prepare block statements
        // Write: upsert into blocks (pk, col_name, col_value)
        sql = cloudsync_memory_mprintf(SQL_BLOCKS_UPSERT, table->blocks_ref);
        if (!sql) return DBRES_NOMEM;
        rc = databasevm_prepare(data, sql, (void **)&table->block_value_write_stmt, DBFLAG_PERSISTENT);
        cloudsync_memory_free(sql);
        if (rc != DBRES_OK) return rc;

        // Read: SELECT col_value FROM blocks WHERE pk = ? AND col_name = ?
        sql = cloudsync_memory_mprintf(SQL_BLOCKS_SELECT, table->blocks_ref);
        if (!sql) return DBRES_NOMEM;
        rc = databasevm_prepare(data, sql, (void **)&table->block_value_read_stmt, DBFLAG_PERSISTENT);
        cloudsync_memory_free(sql);
        if (rc != DBRES_OK) return rc;

        // Delete: DELETE FROM blocks WHERE pk = ? AND col_name = ?
        sql = cloudsync_memory_mprintf(SQL_BLOCKS_DELETE, table->blocks_ref);
        if (!sql) return DBRES_NOMEM;
        rc = databasevm_prepare(data, sql, (void **)&table->block_value_delete_stmt, DBFLAG_PERSISTENT);
        cloudsync_memory_free(sql);
        if (rc != DBRES_OK) return rc;

        // List alive blocks for materialization
        sql = cloudsync_memory_mprintf(SQL_BLOCKS_LIST_ALIVE, table->blocks_ref, table->meta_ref);
        if (!sql) return DBRES_NOMEM;
        rc = databasevm_prepare(data, sql, (void **)&table->block_list_stmt, DBFLAG_PERSISTENT);
        cloudsync_memory_free(sql);
        if (rc != DBRES_OK) return rc;
    }

    // Persist settings
    int rc = dbutils_table_settings_set_key_value(data, table_name, col_name, "algo", "block");
    if (rc != DBRES_OK) return rc;

    if (delimiter) {
        rc = dbutils_table_settings_set_key_value(data, table_name, col_name, "delimiter", delimiter);
        if (rc != DBRES_OK) return rc;
    }

    return DBRES_OK;
}

// MARK: - Private -

bool cloudsync_config_exists (cloudsync_context *data) {
    return database_internal_table_exists(data, CLOUDSYNC_SITEID_NAME) == true;
}

cloudsync_context *cloudsync_context_create (void *db) {
    cloudsync_context *data = (cloudsync_context *)cloudsync_memory_zeroalloc((uint64_t)(sizeof(cloudsync_context)));
    if (!data) return NULL;
    DEBUG_SETTINGS("cloudsync_context_create %p", data);
    
    data->libversion = CLOUDSYNC_VERSION;
    data->pending_db_version = CLOUDSYNC_VALUE_NOTSET;
    #if CLOUDSYNC_DEBUG
    data->debug = 1;
    #endif
    
    // allocate space for 64 tables (it can grow if needed)
    uint64_t mem_needed = (uint64_t)(CLOUDSYNC_INIT_NTABLES * sizeof(cloudsync_table_context *));
    data->tables = (cloudsync_table_context **)cloudsync_memory_zeroalloc(mem_needed);
    if (!data->tables) {cloudsync_memory_free(data); return NULL;}
    
    data->tables_cap = CLOUDSYNC_INIT_NTABLES;
    data->tables_count = 0;
    data->db = db;
    
    // SQLite exposes col_value as ANY, but other databases require a concrete type.
    // In PostgreSQL we expose col_value as bytea, which holds the pk-encoded value bytes (type + data).
    // Because col_value is already encoded, we skip decoding this field and pass it through as bytea.
    // It is decoded to the target column type just before applying changes to the base table.
    data->skip_decode_idx = (db == NULL) ? CLOUDSYNC_PK_INDEX_COLVALUE : -1;

    return data;
}

void cloudsync_context_free (void *ctx) {
    cloudsync_context *data = (cloudsync_context *)ctx;
    DEBUG_SETTINGS("cloudsync_context_free %p", data);
    if (!data) return;

    // free all table contexts and prepared statements
    cloudsync_terminate(data);

    cloudsync_memory_free(data->tables);
    cloudsync_memory_free(data);
}

const char *cloudsync_context_init (cloudsync_context *data) {
    if (!data) return NULL;
    
    // perform init just the first time, if the site_id field is not set.
    // The data->site_id value could exists while settings tables don't exists if the
    // cloudsync_context_init was previously called in init transaction that was rolled back
    // because of an error during the init process.
    if (data->site_id[0] == 0 || !database_internal_table_exists(data, CLOUDSYNC_SITEID_NAME)) {
        if (dbutils_settings_init(data) != DBRES_OK) return NULL;
        if (cloudsync_add_dbvms(data) != DBRES_OK) return NULL;
        if (cloudsync_load_siteid(data) != DBRES_OK) return NULL;
        data->schema_hash = database_schema_hash(data);
    }
    
    return (const char *)data->site_id;
}

void cloudsync_sync_key (cloudsync_context *data, const char *key, const char *value) {
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

    if (strcmp(key, CLOUDSYNC_KEY_SCHEMA) == 0) {
        cloudsync_set_schema(data, value);
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
    
    return DBRES_OK;
}

void cloudsync_rollback_hook (void *ctx) {
    cloudsync_context *data = (cloudsync_context *)ctx;
    
    data->pending_db_version = CLOUDSYNC_VALUE_NOTSET;
    data->seq = 0;
}

int cloudsync_begin_alter (cloudsync_context *data, const char *table_name) {
    // init cloudsync_settings
    if (cloudsync_context_init(data) == NULL) {
        return DBRES_MISUSE;
    }

    // lookup table
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) {
        char buffer[1024];
        snprintf(buffer, sizeof(buffer), "Unable to find table %s", table_name);
        return cloudsync_set_error(data, buffer, DBRES_MISUSE);
    }

    // idempotent: if already altering, return OK
    if (table->is_altering) return DBRES_OK;

    // retrieve primary key(s)
    char **names = NULL;
    int nrows = 0;
    int rc = database_pk_names(data, table_name, &names, &nrows);
    if (rc != DBRES_OK) {
        char buffer[1024];
        snprintf(buffer, sizeof(buffer), "Unable to get primary keys for table %s", table_name);
        cloudsync_set_error(data, buffer, DBRES_MISUSE);
        goto rollback_begin_alter;
    }

    // sanity check the number of primary keys
    if (nrows != table_count_pks(table)) {
        char buffer[1024];
        snprintf(buffer, sizeof(buffer), "Number of primary keys for table %s changed before ALTER", table_name);
        cloudsync_set_error(data, buffer, DBRES_MISUSE);
        goto rollback_begin_alter;
    }

    // drop original triggers
    rc = database_delete_triggers(data, table_name);
    if (rc != DBRES_OK) {
        char buffer[1024];
        snprintf(buffer, sizeof(buffer), "Unable to delete triggers for table %s in cloudsync_begin_alter.", table_name);
        cloudsync_set_error(data, buffer, DBRES_ERROR);
        goto rollback_begin_alter;
    }

    table_set_pknames(table, names);
    table->is_altering = true;
    return DBRES_OK;

rollback_begin_alter:
    if (names) table_pknames_free(names, nrows);
    return rc;
}

int cloudsync_finalize_alter (cloudsync_context *data, cloudsync_table_context *table) {
    // check if dbversion needed to be updated
    cloudsync_dbversion_check_uptodate(data);

    // if primary-key columns change, all row identities change.
    // In that case, the clock table must be dropped, recreated,
    // and backfilled. We detect this by comparing the unique index
    // in the lookaside table with the source table's PKs.
    
    // retrieve primary keys (to check is they changed)
    char **result = NULL;
    int nrows = 0;
    int rc = database_pk_names (data, table->name, &result, &nrows);
    if (rc != DBRES_OK || nrows == 0) {
        if (nrows == 0) rc = DBRES_MISUSE;
        goto finalize;
    }
    
    // check if there are differences
    bool pk_diff = (nrows != table->npks);
    if (!pk_diff) {
        for (int i = 0; i < nrows; ++i) {
            if (strcmp(table->pk_name[i], result[i]) != 0) {
                pk_diff = true;
                break;
            }
        }
    }
    
    if (pk_diff) {
        // drop meta-table, it will be recreated
        char *sql = cloudsync_memory_mprintf(SQL_DROP_CLOUDSYNC_TABLE, table->meta_ref);
        rc = database_exec(data, sql);
        cloudsync_memory_free(sql);
        if (rc != DBRES_OK) {
            DEBUG_DBERROR(rc, "cloudsync_finalize_alter", data);
            goto finalize;
        }
    } else {
        // compact meta-table
        // delete entries for removed columns
        const char *schema = table->schema ? table->schema : "";
        char *sql = sql_build_delete_cols_not_in_schema_query(schema, table->name, table->meta_ref, CLOUDSYNC_TOMBSTONE_VALUE);
        rc = database_exec(data, sql);
        cloudsync_memory_free(sql);
        if (rc != DBRES_OK) {
            DEBUG_DBERROR(rc, "cloudsync_finalize_alter", data);
            goto finalize;
        }
        
        sql = sql_build_pk_qualified_collist_query(schema, table->name);
        if (!sql) {rc = DBRES_NOMEM; goto finalize;}
        
        char *pkclause = NULL;
        rc = database_select_text(data, sql, &pkclause);
        cloudsync_memory_free(sql);
        if (rc != DBRES_OK) goto finalize;
        char *pkvalues = (pkclause) ? pkclause : "rowid";
        
        // delete entries related to rows that no longer exist in the original table, but preserve tombstone
        sql = cloudsync_memory_mprintf(SQL_CLOUDSYNC_GC_DELETE_ORPHANED_PK, table->meta_ref, CLOUDSYNC_TOMBSTONE_VALUE, CLOUDSYNC_TOMBSTONE_VALUE, table->base_ref, table->meta_ref, pkvalues);
        rc = database_exec(data, sql);
        if (pkclause) cloudsync_memory_free(pkclause);
        cloudsync_memory_free(sql);
        if (rc != DBRES_OK) {
            DEBUG_DBERROR(rc, "cloudsync_finalize_alter", data);
            goto finalize;
        }

    }
    
    // update key to be later used in cloudsync_dbversion_rebuild
    char buf[256];
    snprintf(buf, sizeof(buf), "%" PRId64, data->db_version);
    dbutils_settings_set_key_value(data, "pre_alter_dbversion", buf);
    
finalize:
    table_pknames_free(result, nrows);
    return rc;
}

int cloudsync_commit_alter (cloudsync_context *data, const char *table_name) {
    int rc = DBRES_MISUSE;
    cloudsync_table_context *table = NULL;

    // init cloudsync_settings
    if (cloudsync_context_init(data) == NULL) {
        cloudsync_set_error(data, "Unable to initialize cloudsync context", DBRES_MISUSE);
        goto rollback_finalize_alter;
    }

    // lookup table
    table = table_lookup(data, table_name);
    if (!table) {
        char buffer[1024];
        snprintf(buffer, sizeof(buffer), "Unable to find table %s", table_name);
        cloudsync_set_error(data, buffer, DBRES_MISUSE);
        goto rollback_finalize_alter;
    }

    // idempotent: if not altering, return OK
    if (!table->is_altering) return DBRES_OK;

    rc = cloudsync_finalize_alter(data, table);
    if (rc != DBRES_OK) goto rollback_finalize_alter;

    // the table is outdated, delete it and it will be reloaded in the cloudsync_init_internal
    // is_altering is reset implicitly because table_free + cloudsync_init_table
    // will reallocate the table context with zero-initialized memory
    table_remove(data, table);
    table_free(table);
    table = NULL;

    // init again cloudsync for the table
    table_algo algo_current = dbutils_table_settings_get_algo(data, table_name);
    if (algo_current == table_algo_none) algo_current = dbutils_table_settings_get_algo(data, "*");
    rc = cloudsync_init_table(data, table_name, cloudsync_algo_name(algo_current), true);
    if (rc != DBRES_OK) goto rollback_finalize_alter;

    return DBRES_OK;

rollback_finalize_alter:
    if (table) {
        table_set_pknames(table, NULL);
        table->is_altering = false;
    }
    return rc;
}

// MARK: - Filter Rewrite -

// Replace bare column names in a filter expression with prefix-qualified names.
// E.g., filter="user_id = 42", prefix="NEW", columns=["user_id","id"] → "NEW.\"user_id\" = 42"
// Columns must be sorted by length descending by the caller to avoid partial matches.
// Skips content inside single-quoted string literals.
// Returns a newly allocated string (caller must free with cloudsync_memory_free), or NULL on error.
// Helper: check if an identifier token matches a column name.
static bool filter_is_column (const char *token, size_t token_len, char **columns, int ncols) {
    for (int i = 0; i < ncols; ++i) {
        if (strlen(columns[i]) == token_len && strncmp(token, columns[i], token_len) == 0)
            return true;
    }
    return false;
}

// Helper: check if character is part of a SQL identifier.
static bool filter_is_ident_char (char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

char *cloudsync_filter_add_row_prefix (const char *filter, const char *prefix, char **columns, int ncols) {
    if (!filter || !prefix || !columns || ncols <= 0) return NULL;

    size_t filter_len = strlen(filter);
    size_t prefix_len = strlen(prefix);

    // Each identifier match grows by at most (prefix_len + 3) bytes.
    // Worst case: the entire filter is one repeated column reference separated by
    // single characters, so up to (filter_len / 2) matches.  Use a safe upper bound.
    size_t max_growth = (filter_len / 2 + 1) * (prefix_len + 3);
    size_t cap = filter_len + max_growth + 64;
    char *result = (char *)cloudsync_memory_alloc(cap);
    if (!result) return NULL;
    size_t out = 0;

    // Single pass: tokenize into identifiers, quoted strings, and everything else.
    size_t i = 0;
    while (i < filter_len) {
        // Skip single-quoted string literals verbatim (handle '' escape)
        if (filter[i] == '\'') {
            result[out++] = filter[i++];
            while (i < filter_len) {
                if (filter[i] == '\'') {
                    result[out++] = filter[i++];
                    // '' is an escaped quote — keep going
                    if (i < filter_len && filter[i] == '\'') {
                        result[out++] = filter[i++];
                        continue;
                    }
                    break; // single ' ends the literal
                }
                result[out++] = filter[i++];
            }
            continue;
        }

        // Extract identifier token
        if (filter_is_ident_char(filter[i])) {
            size_t start = i;
            while (i < filter_len && filter_is_ident_char(filter[i])) ++i;
            size_t token_len = i - start;

            if (filter_is_column(&filter[start], token_len, columns, ncols)) {
                // Emit PREFIX."column_name"
                memcpy(&result[out], prefix, prefix_len); out += prefix_len;
                result[out++] = '.';
                result[out++] = '"';
                memcpy(&result[out], &filter[start], token_len); out += token_len;
                result[out++] = '"';
            } else {
                // Not a column — copy as-is
                memcpy(&result[out], &filter[start], token_len); out += token_len;
            }
            continue;
        }

        // Any other character — copy as-is
        result[out++] = filter[i++];
    }

    result[out] = '\0';
    return result;
}

int cloudsync_refill_metatable (cloudsync_context *data, const char *table_name) {
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) return DBRES_ERROR;

    dbvm_t *vm = NULL;
    int64_t db_version = cloudsync_dbversion_next(data, CLOUDSYNC_VALUE_NOTSET);

    // Read row-level filter from settings (if any)
    char filter_buf[2048];
    int frc = dbutils_table_settings_get_value(data, table_name, "*", "filter", filter_buf, sizeof(filter_buf));
    const char *filter = (frc == DBRES_OK && filter_buf[0]) ? filter_buf : NULL;

    const char *schema = table->schema ? table->schema : "";
    char *sql = sql_build_pk_collist_query(schema, table_name);
    char *pkclause_identifiers = NULL;
    int rc = database_select_text(data, sql, &pkclause_identifiers);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto finalize;
    char *pkvalues_identifiers = (pkclause_identifiers) ? pkclause_identifiers : "rowid";

    // Use database-specific query builder to handle type differences in composite PKs
    sql = sql_build_insert_missing_pks_query(schema, table_name, pkvalues_identifiers, table->base_ref, table->meta_ref, filter);
    if (!sql) {rc = DBRES_NOMEM; goto finalize;}
    rc = database_exec(data, sql);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto finalize;

    // fill missing colums
    // for each non-pk column:
    // The new query does 1 encode per source row and one indexed NOT-EXISTS probe.
    // The old plan does many decodes per candidate and can't use an index to rule out matches quickly—so it burns CPU and I/O.

    if (filter) {
        sql = cloudsync_memory_mprintf(SQL_CLOUDSYNC_SELECT_PKS_NOT_IN_SYNC_FOR_COL_FILTERED, pkvalues_identifiers, table->base_ref, filter, table->meta_ref);
    } else {
        sql = cloudsync_memory_mprintf(SQL_CLOUDSYNC_SELECT_PKS_NOT_IN_SYNC_FOR_COL, pkvalues_identifiers, table->base_ref, table->meta_ref);
    }
    rc = databasevm_prepare(data, sql, (void **)&vm, DBFLAG_PERSISTENT);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto finalize;
     
    for (int i=0; i<table->ncols; ++i) {
        char *col_name = table->col_name[i];

        rc = databasevm_bind_text(vm, 1, col_name, -1);
        if (rc != DBRES_OK) goto finalize;

        while (1) {
            rc = databasevm_step(vm);
            if (rc == DBRES_ROW) {
                size_t pklen = 0;
                const void *pk = (const char *)database_column_blob(vm, 0, &pklen);
                if (!pk) { rc = DBRES_ERROR; break; }
                rc = local_mark_insert_or_update_meta(table, pk, pklen, col_name, db_version, cloudsync_bumpseq(data));
            } else if (rc == DBRES_DONE) {
                rc = DBRES_OK;
                break;
            } else {
                break;
            }
        }
        if (rc != DBRES_OK) goto finalize;

        databasevm_reset(vm);
    }
    
finalize:
    if (rc != DBRES_OK) {DEBUG_ALWAYS("cloudsync_refill_metatable error: %s", database_errmsg(data));}
    if (pkclause_identifiers) cloudsync_memory_free(pkclause_identifiers);
    if (vm) databasevm_finalize(vm);
    return rc;
}

// MARK: - Local -

int local_update_sentinel (cloudsync_table_context *table, const void *pk, size_t pklen, int64_t db_version, int seq) {
    dbvm_t *vm = table->meta_sentinel_update_stmt;
    if (!vm) return -1;
    
    int rc = databasevm_bind_int(vm, 1, db_version);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_bind_int(vm, 2, seq);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_bind_blob(vm, 3, pk, (int)pklen);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_step(vm);
    if (rc == DBRES_DONE) rc = DBRES_OK;
    
cleanup:
    DEBUG_DBERROR(rc, "local_update_sentinel", table->context);
    databasevm_reset(vm);
    return rc;
}

int local_mark_insert_sentinel_meta (cloudsync_table_context *table, const void *pk, size_t pklen, int64_t db_version, int seq) {
    dbvm_t *vm = table->meta_sentinel_insert_stmt;
    if (!vm) return -1;
    
    int rc = databasevm_bind_blob(vm, 1, pk, (int)pklen);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_bind_int(vm, 2, db_version);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_bind_int(vm, 3, seq);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_bind_int(vm, 4, db_version);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_bind_int(vm, 5, seq);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_step(vm);
    if (rc == DBRES_DONE) rc = DBRES_OK;
    
cleanup:
    DEBUG_DBERROR(rc, "local_insert_sentinel", table->context);
    databasevm_reset(vm);
    return rc;
}

int local_mark_insert_or_update_meta_impl (cloudsync_table_context *table, const void *pk, size_t pklen, const char *col_name, int col_version, int64_t db_version, int seq) {
    
    dbvm_t *vm = table->meta_row_insert_update_stmt;
    if (!vm) return -1;
    
    int rc = databasevm_bind_blob(vm, 1, pk, pklen);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_bind_text(vm, 2, (col_name) ? col_name : CLOUDSYNC_TOMBSTONE_VALUE, -1);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_bind_int(vm, 3, col_version);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_bind_int(vm, 4, db_version);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_bind_int(vm, 5, seq);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_bind_int(vm, 6, db_version);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_bind_int(vm, 7, seq);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_step(vm);
    if (rc == DBRES_DONE) rc = DBRES_OK;
    
cleanup:
    DEBUG_DBERROR(rc, "local_insert_or_update", table->context);
    databasevm_reset(vm);
    return rc;
}

int local_mark_insert_or_update_meta (cloudsync_table_context *table, const void *pk, size_t pklen, const char *col_name, int64_t db_version, int seq) {
    return local_mark_insert_or_update_meta_impl(table, pk, pklen, col_name, 1, db_version, seq);
}

int local_mark_delete_block_meta (cloudsync_table_context *table, const void *pk, size_t pklen, const char *block_colname, int64_t db_version, int seq) {
    // Mark a block as deleted by setting col_version = 2 (even = deleted)
    return local_mark_insert_or_update_meta_impl(table, pk, pklen, block_colname, 2, db_version, seq);
}

int block_delete_value_external (cloudsync_context *data, cloudsync_table_context *table, const void *pk, size_t pklen, const char *block_colname) {
    return block_delete_value(data, table, pk, (int)pklen, block_colname);
}

int local_mark_delete_meta (cloudsync_table_context *table, const void *pk, size_t pklen, int64_t db_version, int seq) {
    return local_mark_insert_or_update_meta_impl(table, pk, pklen, NULL, 2, db_version, seq);
}

int local_drop_meta (cloudsync_table_context *table, const void *pk, size_t pklen) {
    dbvm_t *vm = table->meta_row_drop_stmt;
    if (!vm) return -1;
    
    int rc = databasevm_bind_blob(vm, 1, pk, pklen);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_step(vm);
    if (rc == DBRES_DONE) rc = DBRES_OK;
    
cleanup:
    DEBUG_DBERROR(rc, "local_drop_meta", table->context);
    databasevm_reset(vm);
    return rc;
}

int local_update_move_meta (cloudsync_table_context *table, const void *pk, size_t pklen, const void *pk2, size_t pklen2, int64_t db_version) {
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
      * by either incrementing the maximum sequence value in the table or using a function (e.g., cloudsync_bumpseq(data))
      * that generates a unique sequence for each row. The update query should ensure that each row moved
      * from OLD.pk to NEW.pk gets a distinct `seq` to maintain proper versioning and ordering of changes.
     */
    
    // see https://github.com/sqliteai/sqlite-sync/blob/main/docs/PriKey.md for more details
    // pk2 is the old pk
    
    dbvm_t *vm = table->meta_update_move_stmt;
    if (!vm) return -1;
    
    // new primary key
    int rc = databasevm_bind_blob(vm, 1, pk, pklen);
    if (rc != DBRES_OK) goto cleanup;
    
    // new db_version
    rc = databasevm_bind_int(vm, 2, db_version);
    if (rc != DBRES_OK) goto cleanup;
    
    // old primary key
    rc = databasevm_bind_blob(vm, 3, pk2, pklen2);
    if (rc != DBRES_OK) goto cleanup;
    
    rc = databasevm_step(vm);
    if (rc == DBRES_DONE) rc = DBRES_OK;
    
cleanup:
    DEBUG_DBERROR(rc, "local_update_move_meta", table->context);
    databasevm_reset(vm);
    return rc;
}

// MARK: - Payload Encode / Decode -

static void cloudsync_payload_checksum_store (cloudsync_payload_header *header, uint64_t checksum) {
    uint64_t h = checksum & 0xFFFFFFFFFFFFULL; // keep 48 bits
    header->checksum[0] = (uint8_t)(h >> 40);
    header->checksum[1] = (uint8_t)(h >> 32);
    header->checksum[2] = (uint8_t)(h >> 24);
    header->checksum[3] = (uint8_t)(h >> 16);
    header->checksum[4] = (uint8_t)(h >>  8);
    header->checksum[5] = (uint8_t)(h >>  0);
}

static uint64_t cloudsync_payload_checksum_load (cloudsync_payload_header *header) {
    return ((uint64_t)header->checksum[0] << 40) |
           ((uint64_t)header->checksum[1] << 32) |
           ((uint64_t)header->checksum[2] << 24) |
           ((uint64_t)header->checksum[3] << 16) |
           ((uint64_t)header->checksum[4] <<  8) |
           ((uint64_t)header->checksum[5] <<  0);
}

static bool cloudsync_payload_checksum_verify (cloudsync_payload_header *header, uint64_t checksum) {
    uint64_t checksum1 = cloudsync_payload_checksum_load(header);
    uint64_t checksum2 = checksum & 0xFFFFFFFFFFFFULL;
    return (checksum1 == checksum2);
}

static bool cloudsync_payload_encode_check (cloudsync_payload_context *payload, size_t needed) {
    if (payload->nrows == 0) needed += sizeof(cloudsync_payload_header);
    
    // alloc/resize buffer
    if (payload->bused + needed > payload->balloc) {
        if (needed < CLOUDSYNC_PAYLOAD_MINBUF_SIZE) needed = CLOUDSYNC_PAYLOAD_MINBUF_SIZE;
        size_t balloc = payload->balloc + needed;
        
        char *buffer = cloudsync_memory_realloc(payload->buffer, balloc);
        if (!buffer) {
            if (payload->buffer) cloudsync_memory_free(payload->buffer);
            memset(payload, 0, sizeof(cloudsync_payload_context));
            return false;
        }
        
        payload->buffer = buffer;
        payload->balloc = balloc;
        if (payload->nrows == 0) payload->bused = sizeof(cloudsync_payload_header);
    }
    
    return true;
}

size_t cloudsync_payload_context_size (size_t *header_size) {
    if (header_size) *header_size = sizeof(cloudsync_payload_header);
    return sizeof(cloudsync_payload_context);
}

void cloudsync_payload_header_init (cloudsync_payload_header *header, uint32_t expanded_size, uint16_t ncols, uint32_t nrows, uint64_t hash) {
    memset(header, 0, sizeof(cloudsync_payload_header));
    assert(sizeof(cloudsync_payload_header)==32);
    
    int major, minor, patch;
    sscanf(CLOUDSYNC_VERSION, "%d.%d.%d", &major, &minor, &patch);
    
    header->signature = htonl(CLOUDSYNC_PAYLOAD_SIGNATURE);
    header->version = CLOUDSYNC_PAYLOAD_VERSION_2;
    header->libversion[0] = (uint8_t)major;
    header->libversion[1] = (uint8_t)minor;
    header->libversion[2] = (uint8_t)patch;
    header->expanded_size = htonl(expanded_size);
    header->ncols = htons(ncols);
    header->nrows = htonl(nrows);
    header->schema_hash = htonll(hash);
}

int cloudsync_payload_encode_step (cloudsync_payload_context *payload, cloudsync_context *data, int argc, dbvalue_t **argv) {
    DEBUG_FUNCTION("cloudsync_payload_encode_step");
    // debug_values(argc, argv);
    
    // check if the step function is called for the first time
    if (payload->nrows == 0) payload->ncols = (uint16_t)argc;
    
    size_t breq = pk_encode_size((dbvalue_t **)argv, argc, 0, data->skip_decode_idx);
    if (cloudsync_payload_encode_check(payload, breq) == false) {
        return cloudsync_set_error(data, "Not enough memory to resize payload internal buffer", DBRES_NOMEM);
    }
    
    char *buffer = payload->buffer + payload->bused;
    size_t bsize = payload->balloc - payload->bused;
    char *p = pk_encode((dbvalue_t **)argv, argc, buffer, false, &bsize, data->skip_decode_idx);
    if (!p) return cloudsync_set_error(data, "An error occurred while encoding payload", DBRES_ERROR);
    
    // update buffer
    payload->bused += breq;
    
    // increment row counter
    ++payload->nrows;
    
    return DBRES_OK;
}

int cloudsync_payload_encode_final (cloudsync_payload_context *payload, cloudsync_context *data) {
    DEBUG_FUNCTION("cloudsync_payload_encode_final");
    
    if (payload->nrows == 0) {
        if (payload->buffer) cloudsync_memory_free(payload->buffer);
        payload->buffer = NULL;
        payload->bsize = 0;
        return DBRES_OK;
    }
    
    if (payload->nrows > UINT32_MAX) {
        if (payload->buffer) cloudsync_memory_free(payload->buffer);
        payload->buffer = NULL;
        payload->bsize = 0;
        cloudsync_set_error(data, "Maximum number of payload rows reached", DBRES_ERROR);
        return DBRES_ERROR;
    }
    
    // sanity check about buffer size
    int header_size = (int)sizeof(cloudsync_payload_header);
    int64_t buffer_size = (int64_t)payload->bused - (int64_t)header_size;
    if (buffer_size < 0) {
        if (payload->buffer) cloudsync_memory_free(payload->buffer);
        payload->buffer = NULL;
        payload->bsize = 0;
        cloudsync_set_error(data, "cloudsync_encode: internal size underflow", DBRES_ERROR);
        return DBRES_ERROR;
    }
    if (buffer_size > INT_MAX) {
        if (payload->buffer) cloudsync_memory_free(payload->buffer);
        payload->buffer = NULL;
        payload->bsize = 0;
        cloudsync_set_error(data, "cloudsync_encode: payload too large to compress (INT_MAX limit)", DBRES_ERROR);
        return DBRES_ERROR;
    }
    // try to allocate buffer used for compressed data
    int real_buffer_size = (int)buffer_size;
    int zbound = LZ4_compressBound(real_buffer_size);
    char *zbuffer = cloudsync_memory_alloc(zbound + header_size); // if for some reasons allocation fails then just skip compression
    
    // skip the reserved header from the buffer to compress
    char *src_buffer = payload->buffer + sizeof(cloudsync_payload_header);
    int zused = (zbuffer) ? LZ4_compress_default(src_buffer, zbuffer+header_size, real_buffer_size, zbound) : 0;
    bool use_uncompressed_buffer = (!zused || zused > real_buffer_size);
    CHECK_FORCE_UNCOMPRESSED_BUFFER();
    
    // setup payload header
    cloudsync_payload_header header = {0};
    uint32_t expanded_size = (use_uncompressed_buffer) ? 0 : real_buffer_size;
    cloudsync_payload_header_init(&header, expanded_size, payload->ncols, (uint32_t)payload->nrows, data->schema_hash);
    
    // if compression fails or if compressed size is bigger than original buffer, then use the uncompressed buffer
    if (use_uncompressed_buffer) {
        if (zbuffer) cloudsync_memory_free(zbuffer);
        zbuffer = payload->buffer;
        zused = real_buffer_size;
    }
    
    // compute checksum of the buffer
    uint64_t checksum = pk_checksum(zbuffer + header_size, zused);
    cloudsync_payload_checksum_store(&header, checksum);
    
    // copy header and data to SQLite BLOB
    memcpy(zbuffer, &header, sizeof(cloudsync_payload_header));
    int blob_size = zused + sizeof(cloudsync_payload_header);
    payload->bsize = blob_size;
    
    // cleanup memory
    if (zbuffer != payload->buffer) {
        cloudsync_memory_free (payload->buffer);
        payload->buffer = zbuffer;
    }
    
    return DBRES_OK;
}

char *cloudsync_payload_blob (cloudsync_payload_context *payload, int64_t *blob_size, int64_t *nrows) {
    DEBUG_FUNCTION("cloudsync_payload_blob");
    
    if (blob_size) *blob_size = (int64_t)payload->bsize;
    if (nrows) *nrows = (int64_t)payload->nrows;
    return payload->buffer;
}

static int cloudsync_payload_decode_callback (void *xdata, int index, int type, int64_t ival, double dval, char *pval) {
    cloudsync_pk_decode_bind_context *decode_context = (cloudsync_pk_decode_bind_context*)xdata;
    int rc = pk_decode_bind_callback(decode_context->vm, index, type, ival, dval, pval);
    
    if (rc == DBRES_OK) {
        // the dbversion index is smaller than seq index, so it is processed first
        // when processing the dbversion column: save the value to the tmp_dbversion field
        // when processing the seq column: update the dbversion and seq fields only if the current dbversion is greater than the last max value
        switch (index) {
            case CLOUDSYNC_PK_INDEX_TBL:
                if (type == DBTYPE_TEXT) {
                    decode_context->tbl = pval;
                    decode_context->tbl_len = ival;
                }
                break;
            case CLOUDSYNC_PK_INDEX_PK:
                if (type == DBTYPE_BLOB) {
                    decode_context->pk = pval;
                    decode_context->pk_len = ival;
                }
                break;
            case CLOUDSYNC_PK_INDEX_COLNAME:
                if (type == DBTYPE_TEXT) {
                    decode_context->col_name = pval;
                    decode_context->col_name_len = ival;
                }
                break;
            case CLOUDSYNC_PK_INDEX_COLVERSION:
                if (type == DBTYPE_INTEGER) decode_context->col_version = ival;
                break;
            case CLOUDSYNC_PK_INDEX_DBVERSION:
                if (type == DBTYPE_INTEGER) decode_context->db_version = ival;
                break;
            case CLOUDSYNC_PK_INDEX_SITEID:
                if (type == DBTYPE_BLOB) {
                    decode_context->site_id = pval;
                    decode_context->site_id_len = ival;
                }
                break;
            case CLOUDSYNC_PK_INDEX_CL:
                if (type == DBTYPE_INTEGER) decode_context->cl = ival;
                break;
            case CLOUDSYNC_PK_INDEX_SEQ:
                if (type == DBTYPE_INTEGER) decode_context->seq = ival;
                break;
        }
    }
        
    return rc;
}

// #ifndef CLOUDSYNC_OMIT_RLS_VALIDATION

int cloudsync_payload_apply (cloudsync_context *data, const char *payload, int blen, int *pnrows) {
    // sanity check
    if (blen < (int)sizeof(cloudsync_payload_header)) return cloudsync_set_error(data, "Error on cloudsync_payload_apply: invalid payload length", DBRES_MISUSE);
    
    // decode header
    cloudsync_payload_header header;
    memcpy(&header, payload, sizeof(cloudsync_payload_header));
    
    header.signature = ntohl(header.signature);
    header.expanded_size = ntohl(header.expanded_size);
    header.ncols = ntohs(header.ncols);
    header.nrows = ntohl(header.nrows);
    header.schema_hash = ntohll(header.schema_hash);
    
    // compare schema_hash only if not disabled and if the received payload was created with the current header version
    // to avoid schema hash mismatch when processed by a peer with a different extension version during software updates.
    if (dbutils_settings_get_int64_value(data, CLOUDSYNC_KEY_SKIP_SCHEMA_HASH_CHECK) == 0 && header.version == CLOUDSYNC_PAYLOAD_VERSION_LATEST ) {
        if (header.schema_hash != data->schema_hash) {
            if (!database_check_schema_hash(data, header.schema_hash)) {
                char buffer[1024];
                snprintf(buffer, sizeof(buffer), "Cannot apply the received payload because the schema hash is unknown %llu.", header.schema_hash);
                return cloudsync_set_error(data, buffer, DBRES_MISUSE);
            }
        }
    }
    
    // sanity check header
    if ((header.signature != CLOUDSYNC_PAYLOAD_SIGNATURE) || (header.ncols == 0)) {
        return cloudsync_set_error(data, "Error on cloudsync_payload_apply: invalid signature or column size", DBRES_MISUSE);
    }
    
    const char *buffer = payload + sizeof(cloudsync_payload_header);
    size_t buf_len = (size_t)blen - sizeof(cloudsync_payload_header);

    // sanity check checksum (only if version is >= 2)
    if (header.version >= CLOUDSYNC_PAYLOAD_MIN_VERSION_WITH_CHECKSUM) {
        uint64_t checksum = pk_checksum(buffer, buf_len);
        if (cloudsync_payload_checksum_verify(&header, checksum) == false) {
            return cloudsync_set_error(data, "Error on cloudsync_payload_apply: invalid checksum", DBRES_MISUSE);
        }
    }

    // check if payload is compressed
    char *clone = NULL;
    if (header.expanded_size != 0) {
        clone = (char *)cloudsync_memory_alloc(header.expanded_size);
        if (!clone) return cloudsync_set_error(data, "Unable to allocate memory to uncompress payload", DBRES_NOMEM);

        int lz4_rc = LZ4_decompress_safe(buffer, clone, (int)buf_len, (int)header.expanded_size);
        if (lz4_rc <= 0 || (uint32_t)lz4_rc != header.expanded_size) {
            if (clone) cloudsync_memory_free(clone);
            return cloudsync_set_error(data, "Error on cloudsync_payload_apply: unable to decompress BLOB", DBRES_MISUSE);
        }

        buffer = (const char *)clone;
        buf_len = (size_t)header.expanded_size;
    }
    
    // precompile the insert statement
    dbvm_t *vm = NULL;
    int rc = databasevm_prepare(data, SQL_CHANGES_INSERT_ROW, &vm, 0);
    if (rc != DBRES_OK) {
        if (clone) cloudsync_memory_free(clone);
        return cloudsync_set_error(data, "Error on cloudsync_payload_apply: error while compiling SQL statement", rc);
    }
    
    // process buffer, one row at a time
    uint16_t ncols = header.ncols;
    uint32_t nrows = header.nrows;
    int64_t last_payload_db_version = -1;
    int dbversion = dbutils_settings_get_int_value(data, CLOUDSYNC_KEY_CHECK_DBVERSION);
    int seq = dbutils_settings_get_int_value(data, CLOUDSYNC_KEY_CHECK_SEQ);
    cloudsync_pk_decode_bind_context decoded_context = {.vm = vm};

    // Initialize deferred column-batch merge
    merge_pending_batch batch = {0};
    data->pending_batch = &batch;
    bool in_savepoint = false;
    const void *last_pk = NULL;
    int64_t last_pk_len = 0;
    const char *last_tbl = NULL;
    int64_t last_tbl_len = 0;

    for (uint32_t i=0; i<nrows; ++i) {
        size_t seek = 0;
        int res = pk_decode((char *)buffer, buf_len, ncols, &seek, data->skip_decode_idx, cloudsync_payload_decode_callback, &decoded_context);
        if (res == -1) {
            merge_flush_pending(data);
            data->pending_batch = NULL;
            if (batch.cached_vm) { databasevm_finalize(batch.cached_vm); batch.cached_vm = NULL; }
            if (batch.cached_col_names) { cloudsync_memory_free(batch.cached_col_names); batch.cached_col_names = NULL; }
            if (batch.entries) { cloudsync_memory_free(batch.entries); batch.entries = NULL; }
            if (in_savepoint) database_rollback_savepoint(data, "cloudsync_payload_apply");
            rc = DBRES_ERROR;
            goto cleanup;
        }

        // Detect PK/table/db_version boundary to flush pending batch
        bool pk_changed = (last_pk != NULL &&
                           (last_pk_len != decoded_context.pk_len ||
                            memcmp(last_pk, decoded_context.pk, last_pk_len) != 0));
        bool tbl_changed = (last_tbl != NULL &&
                            (last_tbl_len != decoded_context.tbl_len ||
                             memcmp(last_tbl, decoded_context.tbl, last_tbl_len) != 0));
        bool db_version_changed = (last_payload_db_version != decoded_context.db_version);

        // Flush pending batch before any boundary change
        if (pk_changed || tbl_changed || db_version_changed) {
            int flush_rc = merge_flush_pending(data);
            if (flush_rc != DBRES_OK) {
                rc = flush_rc;
                // continue processing remaining rows
            }
        }

        // Per-db_version savepoints group rows with the same source db_version
        // into one transaction. In SQLite autocommit mode, the RELEASE triggers
        // the commit hook which bumps data->db_version and resets seq, ensuring
        // unique (db_version, seq) tuples across groups. In PostgreSQL SPI,
        // database_in_transaction() is always true so this block is inactive —
        // the inner per-PK savepoint in merge_flush_pending handles RLS instead.
        if (in_savepoint && db_version_changed) {
            rc = database_commit_savepoint(data, "cloudsync_payload_apply");
            if (rc != DBRES_OK) {
                merge_pending_free_entries(&batch);
                data->pending_batch = NULL;
                cloudsync_set_error(data, "Error on cloudsync_payload_apply: unable to release a savepoint", rc);
                goto cleanup;
            }
            in_savepoint = false;
        }

        if (!in_savepoint && db_version_changed && !database_in_transaction(data)) {
            rc = database_begin_savepoint(data, "cloudsync_payload_apply");
            if (rc != DBRES_OK) {
                merge_pending_free_entries(&batch);
                data->pending_batch = NULL;
                cloudsync_set_error(data, "Error on cloudsync_payload_apply: unable to start a transaction", rc);
                goto cleanup;
            }
            in_savepoint = true;
        }

        // Track db_version for batch-flush boundary detection
        if (db_version_changed) {
            last_payload_db_version = decoded_context.db_version;
        }

        // Update PK/table tracking
        last_pk = decoded_context.pk;
        last_pk_len = decoded_context.pk_len;
        last_tbl = decoded_context.tbl;
        last_tbl_len = decoded_context.tbl_len;

        rc = databasevm_step(vm);
        if (rc != DBRES_DONE) {
            // don't "break;", the error can be due to a RLS policy.
            // in case of error we try to apply the following changes
        }

        buffer += seek;
        buf_len -= seek;
        dbvm_reset(vm);
    }

    // Final flush after loop
    {
        int flush_rc = merge_flush_pending(data);
        if (flush_rc != DBRES_OK && rc == DBRES_OK) rc = flush_rc;
    }
    data->pending_batch = NULL;

    if (in_savepoint) {
        int rc1 = database_commit_savepoint(data, "cloudsync_payload_apply");
        if (rc1 != DBRES_OK) rc = rc1;
    }

    // save last error (unused if function returns OK)
    if (rc != DBRES_OK && rc != DBRES_DONE) {
        cloudsync_set_dberror(data);
    }

    if (rc == DBRES_DONE) rc = DBRES_OK;
    if (rc == DBRES_OK) {
        char buf[256];
        if (decoded_context.db_version >= dbversion) {
            snprintf(buf, sizeof(buf), "%" PRId64, decoded_context.db_version);
            dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_CHECK_DBVERSION, buf);
            
            if (decoded_context.seq != seq) {
                snprintf(buf, sizeof(buf), "%" PRId64, decoded_context.seq);
                dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_CHECK_SEQ, buf);
            }
        }
    }

cleanup:
    // cleanup merge_pending_batch
    if (batch.cached_vm) { databasevm_finalize(batch.cached_vm); batch.cached_vm = NULL; }
    if (batch.cached_col_names) { cloudsync_memory_free(batch.cached_col_names); batch.cached_col_names = NULL; }
    if (batch.entries) { cloudsync_memory_free(batch.entries); batch.entries = NULL; }  

    // cleanup vm
    if (vm) databasevm_finalize(vm);

    // cleanup memory
    if (clone) cloudsync_memory_free(clone);

    // error already saved in (save last error)
    if (rc != DBRES_OK) return rc;

    // return the number of processed rows
    if (pnrows) *pnrows = nrows;
    return DBRES_OK;
}

// MARK: - Payload load/store -

int cloudsync_payload_get (cloudsync_context *data, char **blob, int *blob_size, int *db_version, int64_t *new_db_version) {
    // retrieve current db_version and seq
    *db_version = dbutils_settings_get_int_value(data, CLOUDSYNC_KEY_SEND_DBVERSION);
    if (*db_version < 0) return DBRES_ERROR;
    
    // retrieve BLOB
    char sql[1024];
    snprintf(sql, sizeof(sql), "WITH max_db_version AS (SELECT MAX(db_version) AS max_db_version FROM cloudsync_changes WHERE site_id=cloudsync_siteid()) "
                               "SELECT * FROM (SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload, max_db_version AS max_db_version FROM cloudsync_changes, max_db_version WHERE site_id=cloudsync_siteid() AND db_version>%d) WHERE payload IS NOT NULL", *db_version);
    
    int64_t len = 0;
    int rc = database_select_blob_int(data, sql, blob, &len, new_db_version);
    *blob_size = (int)len;
    if (rc != DBRES_OK) return rc;
    
    // exit if there is no data to send
    if (*blob == NULL || *blob_size == 0) return DBRES_OK;
    return rc;
}

#ifdef CLOUDSYNC_DESKTOP_OS
int cloudsync_payload_save (cloudsync_context *data, const char *payload_path, int *size) {
    DEBUG_FUNCTION("cloudsync_payload_save");
    
    // silently delete any other payload with the same name
    cloudsync_file_delete(payload_path);
    
    // retrieve payload
    char *blob = NULL;
    int blob_size = 0, db_version = 0;
    int64_t new_db_version = 0;
    int rc = cloudsync_payload_get(data, &blob, &blob_size, &db_version, &new_db_version);
    if (rc != DBRES_OK) {
        if (db_version < 0) return cloudsync_set_error(data, "Unable to retrieve db_version", rc);
        return cloudsync_set_error(data, "Unable to retrieve changes in cloudsync_payload_save", rc);
    }
    
    // exit if there is no data to save
    if (blob == NULL || blob_size == 0) {
        if (size) *size = 0;
        return DBRES_OK;
    }
    
    // write payload to file
    bool res = cloudsync_file_write(payload_path, blob, (size_t)blob_size);
    cloudsync_memory_free(blob);
    if (res == false) {
        return cloudsync_set_error(data, "Unable to write payload to file path", DBRES_IOERR);
    }
    
    // returns blob size
    if (size) *size = blob_size;
    return DBRES_OK;
}
#endif

// MARK: - Core -

int cloudsync_table_sanity_check (cloudsync_context *data, const char *name, bool skip_int_pk_check) {
    DEBUG_DBFUNCTION("cloudsync_table_sanity_check %s", name);
    char buffer[2048];
    
    // sanity check table name
    if (name == NULL) {
        return cloudsync_set_error(data, "cloudsync_init requires a non-null table parameter", DBRES_ERROR);
    }
    
    // avoid allocating heap memory for SQL statements by setting a maximum length of 512 characters
    // for table names. This limit is reasonable and helps prevent memory management issues.
    const size_t maxlen = CLOUDSYNC_MAX_TABLENAME_LEN;
    if (strlen(name) > maxlen) {
        snprintf(buffer, sizeof(buffer), "Table name cannot be longer than %d characters", (int)maxlen);
        return cloudsync_set_error(data, buffer, DBRES_ERROR);
    }
    
    // check if already initialized
    cloudsync_table_context *table = table_lookup(data, name);
    if (table) return DBRES_OK;
    
    // check if table exists
    if (database_table_exists(data, name, cloudsync_schema(data)) == false) {
        snprintf(buffer, sizeof(buffer), "Table %s does not exist", name);
        return cloudsync_set_error(data, buffer, DBRES_ERROR);
    }
    
    // no more than 128 columns can be used as a composite primary key (SQLite hard limit)
    int npri_keys = database_count_pk(data, name, false, cloudsync_schema(data));
    if (npri_keys < 0) return cloudsync_set_dberror(data);
    if (npri_keys > 128) return cloudsync_set_error(data, "No more than 128 columns can be used to form a composite primary key", DBRES_ERROR);
    
    #if CLOUDSYNC_DISABLE_ROWIDONLY_TABLES
    // if count == 0 means that rowid will be used as primary key (BTW: very bad choice for the user)
    if (npri_keys == 0) {
        snprintf(buffer, sizeof(buffer), "Rowid only tables are not supported, all primary keys must be explicitly set and declared as NOT NULL (table %s)", name);
        return cloudsync_set_error(data, buffer, DBRES_ERROR);
    }
    #endif
        
    if (!skip_int_pk_check) {
        if (npri_keys == 1) {
            // the affinity of a column is determined by the declared type of the column,
            // according to the following rules in the order shown:
            // 1. If the declared type contains the string "INT" then it is assigned INTEGER affinity.
            int npri_keys_int = database_count_int_pk(data, name, cloudsync_schema(data));
            if (npri_keys_int < 0) return cloudsync_set_dberror(data);
            if (npri_keys == npri_keys_int) {
                snprintf(buffer, sizeof(buffer), "Table %s uses a single-column INTEGER primary key. For CRDT replication, primary keys must be globally unique. Consider using a TEXT primary key with UUIDs or ULID to avoid conflicts across nodes. If you understand the risk and still want to use this INTEGER primary key, set the third argument of the cloudsync_init function to 1 to skip this check.", name);
                return cloudsync_set_error(data, buffer, DBRES_ERROR);
            }
            
        }
    }
        
    // if user declared explicit primary key(s) then make sure they are all declared as NOT NULL
    #if CLOUDSYNC_CHECK_NOTNULL_PRIKEYS
    if (npri_keys > 0) {
        int npri_keys_notnull = database_count_pk(data, name, true, cloudsync_schema(data));
        if (npri_keys_notnull < 0) return cloudsync_set_dberror(data);
        if (npri_keys != npri_keys_notnull) {
            snprintf(buffer, sizeof(buffer), "All primary keys must be explicitly declared as NOT NULL (table %s)", name);
            return cloudsync_set_error(data, buffer, DBRES_ERROR);
        }
    }
    #endif
    
    // check for columns declared as NOT NULL without a DEFAULT value.
    // Otherwise, col_merge_stmt would fail if changes to other columns are inserted first.
    int n_notnull_nodefault = database_count_notnull_without_default(data, name, cloudsync_schema(data));
    if (n_notnull_nodefault < 0) return cloudsync_set_dberror(data);
    if (n_notnull_nodefault > 0) {
        snprintf(buffer, sizeof(buffer), "All non-primary key columns declared as NOT NULL must have a DEFAULT value. (table %s)", name);
        return cloudsync_set_error(data, buffer, DBRES_ERROR);
    }
    
    return DBRES_OK;
}

int cloudsync_cleanup_internal (cloudsync_context *data, cloudsync_table_context *table) {
    if (cloudsync_context_init(data) == NULL) return DBRES_MISUSE;
    
    // drop meta-table
    const char *table_name = table->name;
    char *sql = cloudsync_memory_mprintf(SQL_DROP_CLOUDSYNC_TABLE, table->meta_ref);
    int rc = database_exec(data, sql);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) {
        char buffer[1024];
        snprintf(buffer, sizeof(buffer), "Unable to drop cloudsync table %s_cloudsync in cloudsync_cleanup", table_name);
        return cloudsync_set_error(data, buffer, rc);
    }
    
    // drop original triggers
    rc = database_delete_triggers(data, table_name);
    if (rc != DBRES_OK) {
        char buffer[1024];
        snprintf(buffer, sizeof(buffer), "Unable to delete triggers for table %s", table_name);
        return cloudsync_set_error(data, buffer, rc);
    }
    
    // remove all table related settings
    dbutils_table_settings_set_key_value(data, table_name, NULL, NULL, NULL);
    return DBRES_OK;
}

int cloudsync_cleanup (cloudsync_context *data, const char *table_name) {
    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) return DBRES_OK;
    
    // TODO: check what happen if cloudsync_cleanup_internal failes (not eveything dropped) and the table is still in memory?
    
    int rc = cloudsync_cleanup_internal(data, table);
    if (rc != DBRES_OK) return rc;
    
    int counter = table_remove(data, table);
    table_free(table);
    
    if (counter == 0) {
        // cleanup database on last table
        cloudsync_reset_siteid(data);
        dbutils_settings_cleanup(data);
    } else {
        if (database_internal_table_exists(data, CLOUDSYNC_TABLE_SETTINGS_NAME) == true) {
            cloudsync_update_schema_hash(data);
        }
    }
    
    return DBRES_OK;
}

int cloudsync_cleanup_all (cloudsync_context *data) {
    return database_cleanup(data);
}

int cloudsync_terminate (cloudsync_context *data) {
    // can't use for/loop here because data->tables_count is changed by table_remove
    while (data->tables_count > 0) {
        cloudsync_table_context *t = data->tables[data->tables_count - 1];
        table_remove(data, t);
        table_free(t);
    }
    
    if (data->schema_version_stmt) databasevm_finalize(data->schema_version_stmt);
    if (data->data_version_stmt) databasevm_finalize(data->data_version_stmt);
    if (data->db_version_stmt) databasevm_finalize(data->db_version_stmt);
    if (data->getset_siteid_stmt) databasevm_finalize(data->getset_siteid_stmt);
    if (data->current_schema) cloudsync_memory_free(data->current_schema);
    
    data->schema_version_stmt = NULL;
    data->data_version_stmt = NULL;
    data->db_version_stmt = NULL;
    data->getset_siteid_stmt = NULL;
    data->current_schema = NULL;
    
    // reset the site_id so the cloudsync_context_init will be executed again
    // if any other cloudsync function is called after terminate
    data->site_id[0] = 0;
    
    return 1;
}

int cloudsync_init_table (cloudsync_context *data, const char *table_name, const char *algo_name, bool skip_int_pk_check) {
    // sanity check table and its primary key(s)
    int rc = cloudsync_table_sanity_check(data, table_name, skip_int_pk_check);
    if (rc != DBRES_OK) return rc;
    
    // init cloudsync_settings
    if (cloudsync_context_init(data) == NULL) {
        return cloudsync_set_error(data, "Unable to initialize cloudsync context", DBRES_MISUSE);
    }
    
    // sanity check algo name (if exists)
    table_algo algo_new = table_algo_none;
    if (!algo_name) algo_name = CLOUDSYNC_DEFAULT_ALGO;
    
    algo_new = cloudsync_algo_from_name(algo_name);
    if (algo_new == table_algo_none) {
        char buffer[1024];
        snprintf(buffer, sizeof(buffer), "Unknown CRDT algorithm name %s", algo_name);
        return cloudsync_set_error(data, buffer, DBRES_ERROR);
    }

    // DWS and AWS algorithms are not yet implemented in the merge logic
    if (algo_new == table_algo_crdt_dws || algo_new == table_algo_crdt_aws) {
        char buffer[1024];
        snprintf(buffer, sizeof(buffer), "CRDT algorithm %s is not yet supported", algo_name);
        return cloudsync_set_error(data, buffer, DBRES_ERROR);
    }
    
    // check if table name was already augmented
    table_algo algo_current = dbutils_table_settings_get_algo(data, table_name);
    
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
        dbutils_table_settings_set_key_value(data, table_name, "*", "algo", algo_name);
    } else {
        // error condition
        return cloudsync_set_error(data, "The function cloudsync_cleanup(table) must be called before changing a table algorithm", DBRES_MISUSE);
    }
    
    // Run the following function even if table was already augmented.
    // It is safe to call the following function multiple times, if there is nothing to update nothing will be changed.
    // After an alter table, in contrast, all the cloudsync triggers, tables and stmts must be recreated.
    
    // sync algo with table (unused in this version)
    // cloudsync_sync_table_key(data, table_name, "*", CLOUDSYNC_KEY_ALGO, crdt_algo_name(algo_new));
    
    // read row-level filter from settings (if any)
    char init_filter_buf[2048];
    int init_frc = dbutils_table_settings_get_value(data, table_name, "*", "filter", init_filter_buf, sizeof(init_filter_buf));
    const char *init_filter = (init_frc == DBRES_OK && init_filter_buf[0]) ? init_filter_buf : NULL;

    // check triggers
    rc = database_create_triggers(data, table_name, algo_new, init_filter);
    if (rc != DBRES_OK) return cloudsync_set_error(data, "An error occurred while creating triggers", DBRES_MISUSE);
    
    // check meta-table
    rc = database_create_metatable(data, table_name);
    if (rc != DBRES_OK) return cloudsync_set_error(data, "An error occurred while creating metatable", DBRES_MISUSE);
    
    // add prepared statements
    if (cloudsync_add_dbvms(data) != DBRES_OK) {
        return cloudsync_set_error(data, "An error occurred while trying to compile prepared SQL statements", DBRES_MISUSE);
    }
    
    // add table to in-memory data context
    if (table_add_to_context(data, algo_new, table_name) == false) {
        char buffer[1024];
        snprintf(buffer, sizeof(buffer), "An error occurred while adding %s table information to global context", table_name);
        return cloudsync_set_error(data, buffer, DBRES_MISUSE);
    }
    
    if (cloudsync_refill_metatable(data, table_name) != DBRES_OK) {
        return cloudsync_set_error(data, "An error occurred while trying to fill the augmented table", DBRES_MISUSE);
    }
        
    return DBRES_OK;
}
