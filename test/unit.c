//
//  main.c
//  unittest
//
//  Created by Marco Bambini on 31/10/24.
//

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include "sqlite3.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "pk.h"
#include "dbutils.h"
#include "cloudsync.h"
#include "cloudsync_private.h"

// declared only if macro CLOUDSYNC_UNITTEST is defined 
extern char *OUT_OF_MEMORY_BUFFER;
extern bool force_vtab_filter_abort;
extern bool force_uncompressed_blob;

// private prototypes
sqlite3_stmt *stmt_reset (sqlite3_stmt *stmt);
int stmt_count (sqlite3_stmt *stmt, const char *value, size_t len, int type);
int stmt_execute (sqlite3_stmt *stmt, void *data);

sqlite3_int64 dbutils_select (sqlite3 *db, const char *sql, const char **values, int types[], int lens[], int count, int expected_type);
int dbutils_settings_table_load_callback (void *xdata, int ncols, char **values, char **names);
int dbutils_settings_check_version (sqlite3 *db);
bool dbutils_migrate (sqlite3 *db);
const char *opname_from_value (int value);
int colname_is_legal (const char *name);
int binary_comparison (int x, int y);
sqlite3 *do_create_database (void);

static int stdout_backup = -1; // Backup file descriptor for stdout
static int dev_null_fd = -1;   // File descriptor for /dev/null
static int test_counter = 1;

#define TEST_INSERT     (1 << 0) // 0x01
#define TEST_UPDATE     (1 << 1) // 0x02
#define TEST_DELETE     (1 << 2) // 0x04
#define TEST_ALTER      (1 << 3) // 0x08

#define TEST_PRIKEYS    (1 << 0) // 0x01
#define TEST_NOCOLS     (1 << 1) // 0x02
#define TEST_NOPRIKEYS  (1 << 2) // 0x04

#define NINSERT         9
#define NUPDATE         4
#define NDELETE         3

#define MAX_SIMULATED_CLIENTS   128

#define CUSTOMERS_TABLE             "customers 1 test table"
#define CUSTOMERS_NOCOLS_TABLE      "customers nocols"

typedef struct {
    int type;
    union {
        int64_t ivalue;
        double  dvalue;
        struct {
            int plen;
            char *pvalue;
        };
    };
} test_value;

#ifndef CLOUDSYNC_OMIT_PRINT_RESULT
static bool first_time = true;
static const char *query_table = "QUERY_TABLE";
static const char *query_siteid = "QUERY_SITEID";
static const char *query_changes = "QUERY_CHANGES";
#endif

// MARK: -

int64_t random_int64_range (int64_t min, int64_t max) {
    // generate a high-quality pseudo-random number in the full 64-bit space
    uint64_t random_number = 0;
    sqlite3_randomness(sizeof(random_number), &random_number);
    
    // scale result to fit the int64_t range
    uint64_t range = (uint64_t)(max - min + 1);
    
    // map the random number to the specified range
    return (int64_t)(random_number % range) + min;
}

int64_t random_int64 (void) {
    int64_t random_number = 0;
    sqlite3_randomness(sizeof(random_number), &random_number);
    
    return random_number;
}

double random_double (void) {
    int64_t random_number = random_int64();
    return (double)random_number;
}

void random_blob (char buffer[4096], int max_size) {
    assert(max_size <= 4096);
    sqlite3_randomness(sizeof(max_size), buffer);
}

void random_string (char buffer[4096], int max_size) {
    random_blob(buffer, max_size);
    
    for (int i=0; i<max_size; ++i) {
        // map the random value to the printable ASCII range (32 to 126)
        buffer[i] = (buffer[i] % 95) + 32;
    }
}

// MARK: -

void suppress_printf_output (void) {
    // check if already suppressed
    if (stdout_backup != -1) return;

    // open /dev/null for writing
    #ifdef _WIN32
    dev_null_fd = open("nul", O_WRONLY);
    #else
    dev_null_fd = open("/dev/null", O_WRONLY);
    #endif
    if (dev_null_fd == -1) {
        perror("Failed to open /dev/null");
        return;
    }

    // backup the current stdout file descriptor
    stdout_backup = dup(fileno(stdout));
    if (stdout_backup == -1) {
        perror("Failed to duplicate stdout");
        close(dev_null_fd);
        dev_null_fd = -1;
        return;
    }

    // redirect stdout to /dev/null
    if (dup2(dev_null_fd, fileno(stdout)) == -1) {
        perror("Failed to redirect stdout to /dev/null");
        close(dev_null_fd);
        dev_null_fd = -1;
        close(stdout_backup);
        stdout_backup = -1;
        return;
    }
}

void resume_printf_output (void) {
    // check if already resumed
    if (stdout_backup == -1) return;

    // restore the original stdout
    if (dup2(stdout_backup, fileno(stdout)) == -1) {
        perror("Failed to restore stdout");
    }

    // close the backup and /dev/null file descriptors
    close(stdout_backup);
    stdout_backup = -1;

    if (dev_null_fd != -1) {
        close(dev_null_fd);
        dev_null_fd = -1;
    }
}

void generate_pk_test_sql (int nkeys, char buffer[4096]) {
    size_t bsize = 4096;
    int len = snprintf(buffer, bsize, "SELECT cloudsync_pk_encode(");
    
    for (int i=0; i<nkeys; ++i) {
        len += snprintf(buffer+len, bsize-len, "?,");
    }
    
    buffer[len-1] = ')';
    buffer[len] = 0;
}

char *build_long_tablename (void) {
    static char name[4096] = {0};
    if (name[0] == 0) {
        memset(name, 'A', sizeof(name));
        name[sizeof(name)-1] = 0;
    }
    return name;
}

const char *build_huge_table (void) {
    const char *sql = "CREATE TABLE dummy_table ("
                          "c1, c2, c3, c4, c5, "
                          "c6, c7, c8, c9, c10, "
                          "c11, c12, c13, c14, c15, "
                          "c16, c17, c18, c19, c20, "
                          "c21, c22, c23, c24, c25, "
                          "c26, c27, c28, c29, c30, "
                          "c31, c32, c33, c34, c35, "
                          "c36, c37, c38, c39, c40, "
                          "c41, c42, c43, c44, c45, "
                          "c46, c47, c48, c49, c50, "
                          "c51, c52, c53, c54, c55, "
                          "c56, c57, c58, c59, c60, "
                          "c61, c62, c63, c64, c65, "
                          "c66, c67, c68, c69, c70, "
                          "c71, c72, c73, c74, c75, "
                          "c76, c77, c78, c79, c80, "
                          "c81, c82, c83, c84, c85, "
                          "c86, c87, c88, c89, c90, "
                          "c91, c92, c93, c94, c95, "
                          "c96, c97, c98, c99, c100, "
                          "c101, c102, c103, c104, c105, "
                          "c106, c107, c108, c109, c110, "
                          "c111, c112, c113, c114, c115, "
                          "c116, c117, c118, c119, c120, "
                          "c121, c122, c123, c124, c125, "
                          "c126, c127, c128, c129, c130, "
                          "PRIMARY KEY ("
                          "c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, "
                          "c11, c12, c13, c14, c15, c16, c17, c18, c19, c20, "
                          "c21, c22, c23, c24, c25, c26, c27, c28, c29, c30, "
                          "c31, c32, c33, c34, c35, c36, c37, c38, c39, c40, "
                          "c41, c42, c43, c44, c45, c46, c47, c48, c49, c50, "
                          "c51, c52, c53, c54, c55, c56, c57, c58, c59, c60, "
                          "c61, c62, c63, c64, c65, c66, c67, c68, c69, c70, "
                          "c71, c72, c73, c74, c75, c76, c77, c78, c79, c80, "
                          "c81, c82, c83, c84, c85, c86, c87, c88, c89, c90, "
                          "c91, c92, c93, c94, c95, c96, c97, c98, c99, c100, "
                          "c101, c102, c103, c104, c105, c106, c107, c108, c109, c110, "
                          "c111, c112, c113, c114, c115, c116, c117, c118, c119, c120, "
                          "c121, c122, c123, c124, c125, c126, c127, c128, c129, c130"
                          "));";
    return sql;
}

sqlite3 *close_db (sqlite3 *db) {
    if (db) {
        sqlite3_exec(db, "SELECT cloudsync_terminate();", NULL, NULL, NULL);
        dbutils_debug_stmt(db, true);
        sqlite3_close(db);
    }
    return NULL;
}

int close_db_v2 (sqlite3 *db) {
    int counter = 0;
    if (db) {
        sqlite3_exec(db, "SELECT cloudsync_terminate();", NULL, NULL, NULL);
        counter = dbutils_debug_stmt(db, true);
        sqlite3_close(db);
    }
    return counter;
}

bool file_delete (const char *path) {
    #ifdef _WIN32
    if (DeleteFile(path) == 0) return false;
    #else
    if (unlink(path) != 0) return false;
    #endif
    
    return true;
}

// MARK: -

#ifndef UNITTEST_OMIT_RLS_VALIDATION
typedef struct {
    bool    in_savepoint;
    bool    is_approved;
    bool    last_is_delete;
    char    *last_tbl;
    void    *last_pk;
    int64_t last_pk_len;
    int64_t last_db_version;
} unittest_payload_apply_rls_status;

bool unittest_validate_changed_row(sqlite3 *db, cloudsync_context *data, char *tbl_name, void *pk, int64_t pklen) {
    // verify row
    bool ret = false;
    bool vm_persistent;
    sqlite3_stmt *vm = cloudsync_colvalue_stmt(db, data, tbl_name, &vm_persistent);
    if (!vm) goto cleanup;
    
    // bind primary key values (the return code is the pk count)
    int rc = pk_decode_prikey((char *)pk, (size_t)pklen, pk_decode_bind_callback, (void *)vm);
    if (rc < 0) goto cleanup;
    
    // execute vm
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
    } else if (rc == SQLITE_ROW) {
        rc = SQLITE_OK;
        ret = true;
    }
    
cleanup:
    if (vm_persistent) sqlite3_reset(vm);
    else sqlite3_finalize(vm);
    
    return ret;
}

int unittest_payload_apply_reset_transaction(sqlite3 *db, unittest_payload_apply_rls_status *s, bool create_new) {
    int rc = SQLITE_OK;
    
    if (s->in_savepoint == true) {
        if (s->is_approved) rc = sqlite3_exec(db, "RELEASE unittest_payload_apply_transaction", NULL, NULL, NULL);
        else rc = sqlite3_exec(db, "ROLLBACK TO unittest_payload_apply_transaction; RELEASE unittest_payload_apply_transaction", NULL, NULL, NULL);
        if (rc == SQLITE_OK) s->in_savepoint = false;
    }
    if (create_new) {
        rc = sqlite3_exec(db, "SAVEPOINT unittest_payload_apply_transaction", NULL, NULL, NULL);
        if (rc == SQLITE_OK) s->in_savepoint = true;
    }
    return rc;
}

bool unittest_payload_apply_rls_callback(void **xdata, cloudsync_pk_decode_bind_context *d, sqlite3 *db, cloudsync_context *data, int step, int rc) {
    unittest_payload_apply_rls_status *s;
    if (*xdata) {
        s = (unittest_payload_apply_rls_status *)*xdata;
    } else {
        s = cloudsync_memory_zeroalloc(sizeof(unittest_payload_apply_rls_status));
        s->is_approved = true;
        *xdata = s;
    }
    
    // extract context info
    int64_t colname_len = 0;
    char *colname = cloudsync_pk_context_colname(d, &colname_len);
    
    int64_t tbl_len = 0;
    char *tbl = cloudsync_pk_context_tbl(d, &tbl_len);
    
    int64_t pk_len = 0;
    void *pk = cloudsync_pk_context_pk(d, &pk_len);
    
    int64_t cl = cloudsync_pk_context_cl(d);
    int64_t db_version = cloudsync_pk_context_dbversion(d);
    
    switch (step) {
        case CLOUDSYNC_PAYLOAD_APPLY_WILL_APPLY: {
            // if the tbl name or the prikey has changed, then verify if the row is valid
            // must use strncmp because strings in xdata are not zero-terminated
            bool tbl_changed = (s->last_tbl && (strlen(s->last_tbl) != (size_t)tbl_len || strncmp(s->last_tbl, tbl, (size_t)tbl_len) != 0));
            bool pk_changed = (s->last_pk && pk && cloudsync_blob_compare(s->last_pk,  s->last_pk_len, pk, pk_len) != 0);
            if (s->is_approved
                && !s->last_is_delete
                && (tbl_changed || pk_changed)) {
                s->is_approved = unittest_validate_changed_row(db, data, s->last_tbl, s->last_pk, s->last_pk_len);
            }
            
            s->last_is_delete = ((size_t)colname_len == strlen(CLOUDSYNC_TOMBSTONE_VALUE) &&
                                 strncmp(colname, CLOUDSYNC_TOMBSTONE_VALUE, (size_t)colname_len) == 0
                                 ) && cl % 2 == 0;
            
            // update the last_tbl value, if needed
            if (!s->last_tbl ||
                !tbl ||
                (strlen(s->last_tbl) != (size_t)tbl_len) ||
                strncmp(s->last_tbl, tbl, (size_t)tbl_len) != 0) {
                if (s->last_tbl) cloudsync_memory_free(s->last_tbl);
                if (tbl && tbl_len > 0) s->last_tbl = cloudsync_string_ndup(tbl, tbl_len, false);
                else s->last_tbl = NULL;
            }
            
            // update the last_prikey and len values, if needed
            if (!s->last_pk || !pk || cloudsync_blob_compare(s->last_pk, s->last_pk_len, pk, pk_len) != 0) {
                if (s->last_pk) cloudsync_memory_free(s->last_pk);
                if (pk && pk_len > 0) {
                    s->last_pk = cloudsync_memory_alloc(pk_len);
                    memcpy(s->last_pk, pk, pk_len);
                    s->last_pk_len = pk_len;
                } else {
                    s->last_pk = NULL;
                    s->last_pk_len = 0;
                }
            }
            
            // commit the previous transaction, if any
            // begin new transacion, if needed
            if (s->last_db_version != db_version) {
                rc = unittest_payload_apply_reset_transaction(db, s, true);
                if (rc != SQLITE_OK) printf("unittest_payload_apply error in reset_transaction: (%d) %s\n", rc, sqlite3_errmsg(db));
                
                // reset local variables
                s->last_db_version = db_version;
                s->is_approved = true;
            }
            break;
        }
        case CLOUDSYNC_PAYLOAD_APPLY_DID_APPLY:
            break;
        case CLOUDSYNC_PAYLOAD_APPLY_CLEANUP:
            if (s->is_approved && !s->last_is_delete) s->is_approved = unittest_validate_changed_row(db, data, s->last_tbl, s->last_pk, s->last_pk_len);
            rc = unittest_payload_apply_reset_transaction(db, s, false);
            if (s->last_tbl) cloudsync_memory_free(s->last_tbl);
            if (s->last_pk) {
                cloudsync_memory_free(s->last_pk);
                s->last_pk_len = 0;
            }
            
            cloudsync_memory_free(s);
            *xdata = NULL;
            break;
    }
   
    return s->is_approved;
}
#endif

// MARK: -

#ifndef CLOUDSYNC_OMIT_PRINT_RESULT
int do_query_cb (void *type, int argc, char **argv, char **azColName) {
    int query_type = 0;
    if (type == query_table) query_type = 1;
    else if (type == query_changes) query_type = 2;
    else if (type == query_siteid) query_type = 1;
    
    if (first_time) {
        for (int i = 0; i < argc; i++) {
            if (query_type == 1 && i == 0) {
                printf("%-40s", azColName[i]);
                continue;
            }
            if (query_type == 2 && (i == 1)) {
                printf("%-50s", azColName[i]);
                continue;
            }
            if (query_type == 2 && (i == 6)) {
                printf("%-40s", azColName[i]);
                continue;
            }
            
            printf("%-12s", azColName[i]);
        }
        printf("\n");
        first_time = false;
    }
    
    for (int i = 0; i < argc; i++) {
        if (query_type == 1 && i == 0) {
            printf("%-40s", argv[i]);
            continue;
        }
        if (query_type == 2 && (i == 1)) {
            printf("%-50s", argv[i]);
            continue;
        }
        if (query_type == 2 && (i == 6)) {
            printf("%-40s", argv[i]);
            continue;
        }
        printf("%-12s", argv[i]);
    }
    printf("\n");
    
    return SQLITE_OK;
}

void do_query (sqlite3 *db, const char *sql, const char *type) {
    first_time = true;
    int rc = sqlite3_exec(db, sql, do_query_cb, (void *)type, NULL);
    if (rc != SQLITE_OK) {
        printf("Error in %s: %s\n", sql, sqlite3_errmsg(db));
    }
}
#else
#define do_query(db, sql, type) 
#endif

// MARK: -

void do_insert (sqlite3 *db, int table_mask, int ninsert, bool print_result) {
    if (table_mask & TEST_PRIKEYS) {
        const char *table_name = CUSTOMERS_TABLE;
        if (print_result) printf("TESTING INSERT on %s\n", table_name);
        
        for (int i=0; i<ninsert; ++i) {
            char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, last_name, age, note, stamp) VALUES ('name%d', 'surname%d', %d, 'note%d', 'stamp%d');", table_name, i+1, i+1, i+1, i+1, i+1);
            if (!sql) exit (-3);
            
            int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
            sqlite3_free(sql);
            
            if (rc != SQLITE_OK) {
                printf("error in do_insert with sql %s (%s)\n", sql, sqlite3_errmsg(db));
                exit(-4);
            }
        }
    }
    
    if (table_mask & TEST_NOCOLS) {
        const char *table_name = CUSTOMERS_NOCOLS_TABLE;
        if (print_result) printf("TESTING INSERT on %s\n", table_name);
        
        for (int i=0; i<ninsert; ++i) {
            char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, last_name) VALUES ('name%d', 'surname%d');", table_name, (i+1)+100000, (i+1)+100000);
            if (!sql) exit (-3);
            
            int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
            sqlite3_free(sql);
            
            if (rc != SQLITE_OK) {
                printf("error in do_insert (TEST_NOCOLS) with sql %s (%s)\n", sql, sqlite3_errmsg(db));
                exit(-4);
            }
        }
    }
    
    if (table_mask & TEST_NOPRIKEYS) {
        const char *table_name = "customers_noprikey";
        if (print_result) printf("TESTING INSERT on %s\n", table_name);
        
        for (int i=0; i<ninsert; ++i) {
            char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, last_name) VALUES ('name%d', 'surname%d');", table_name, (i+1)+200000, (i+1)+200000);
            if (!sql) exit (-3);
            
            int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
            sqlite3_free(sql);
            
            if (rc != SQLITE_OK) {
                printf("error in do_insert with sql %s (%s)\n", sql, sqlite3_errmsg(db));
                exit(-4);
            }
        }
    }
}

void do_insert_val (sqlite3 *db, int table_mask, int val, bool print_result) {
    if (table_mask & TEST_PRIKEYS) {
        const char *table_name = CUSTOMERS_TABLE;
        if (print_result) printf("TESTING INSERT on %s\n", table_name);
        
        char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, last_name, age, note, stamp) VALUES ('name%d', 'surname%d', %d, 'note%d', 'stamp%d');", table_name, val, val, val, val, val);
        if (!sql) exit (-3);
        
        int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        
        if (rc != SQLITE_OK) {
            printf("error in do_insert with sql %s (%s)\n", sql, sqlite3_errmsg(db));
            exit(-4);
        }
    }
    
    if (table_mask & TEST_NOCOLS) {
        const char *table_name = CUSTOMERS_NOCOLS_TABLE;
        if (print_result) printf("TESTING INSERT on %s\n", table_name);
        
        char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, last_name) VALUES ('name%d', 'surname%d');", table_name, (val)+100000, (val)+100000);
        if (!sql) exit (-3);
        
        int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        
        if (rc != SQLITE_OK) {
            printf("error in do_insert (TEST_NOCOLS) with sql %s (%s)\n", sql, sqlite3_errmsg(db));
            exit(-4);
        }
    }
    
    if (table_mask & TEST_NOPRIKEYS) {
        const char *table_name = "customers_noprikey";
        if (print_result) printf("TESTING INSERT on %s\n", table_name);
        
        char *sql = sqlite3_mprintf("INSERT INTO %w (first_name, last_name) VALUES ('name%d', 'surname%d');", table_name, (val)+200000, (val)+200000);
        if (!sql) exit (-3);
        
        int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        
        if (rc != SQLITE_OK) {
            printf("error in do_insert with sql %s (%s)\n", sql, sqlite3_errmsg(db));
            exit(-4);
        }
    }
}

void do_update (sqlite3 *db, int table_mask, bool print_result) {
    int rc = SQLITE_OK;
    
    if (table_mask & TEST_PRIKEYS) {
        const char *table_name = CUSTOMERS_TABLE;
        if (print_result) printf("TESTING UPDATE on %s\n", table_name);
        
        char *sql = sqlite3_mprintf("UPDATE \"%w\" SET age = 40 WHERE first_name='name1';", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
        
        sql = sqlite3_mprintf("UPDATE \"%w\" SET age = 2000, note='hello2000' WHERE first_name='name2';", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
        
        sql = sqlite3_mprintf("UPDATE \"%w\" SET age = 6000, note='hello6000' WHERE first_name='name6';", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
        
        // update primary key here
        sql = sqlite3_mprintf("UPDATE \"%w\" SET first_name = 'name1updated' WHERE first_name='name1';", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
        
        sql = sqlite3_mprintf("UPDATE \"%w\" SET first_name = 'name8updated' WHERE first_name='name8';", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
        
        // update two primary keys here
        sql = sqlite3_mprintf("UPDATE \"%w\" SET first_name = 'name9updated', last_name = 'surname9updated' WHERE first_name='name9';", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    if (table_mask & TEST_NOCOLS) {
        const char *table_name = CUSTOMERS_NOCOLS_TABLE;
        if (print_result) printf("TESTING UPDATE on %s\n", table_name);
        
        // update primary key here
        rc = sqlite3_exec(db, "UPDATE \"" CUSTOMERS_NOCOLS_TABLE "\" SET first_name = 'name100001updated' WHERE first_name='name100001';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        rc = sqlite3_exec(db, "UPDATE \"" CUSTOMERS_NOCOLS_TABLE "\" SET first_name = 'name100008updated' WHERE first_name='name100008';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        // update two primary keys here
        rc = sqlite3_exec(db, "UPDATE \"" CUSTOMERS_NOCOLS_TABLE "\" SET first_name = 'name100009updated', last_name = 'surname100009updated' WHERE first_name='name100009';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    if (table_mask & TEST_NOPRIKEYS) {
        const char *table_name = "customers_noprikey";
        if (print_result) printf("TESTING UPDATE on %s\n", table_name);
        
        // update primary key here
        rc = sqlite3_exec(db, "UPDATE customers_noprikey SET first_name = 'name200001updated' WHERE first_name='name200001';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        rc = sqlite3_exec(db, "UPDATE customers_noprikey SET first_name = 'name200008updated' WHERE first_name='name200008';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        // update two columns
        rc = sqlite3_exec(db, "UPDATE customers_noprikey SET first_name = 'name200009updated', last_name = 'surname200009updated' WHERE first_name='name200009';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
finalize:
    if (rc != SQLITE_OK) {
        printf("error in do_update: %s\n", sqlite3_errmsg(db));
        exit(-4);
    }
}

void do_update_random (sqlite3 *db, int table_mask, bool print_result) {
    int rc = SQLITE_OK;
    
    if (table_mask & TEST_PRIKEYS) {
        const char *table_name = CUSTOMERS_TABLE;
        if (print_result) printf("TESTING RANDOM UPDATE on %s\n", table_name);
        
        char *sql = sqlite3_mprintf( "UPDATE \"%w\" SET age = ABS(RANDOM()) WHERE rowid=(ABS(RANDOM() %% 10)+1);", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
        
        sql = sqlite3_mprintf("UPDATE \"%w\" SET age = ABS(RANDOM()), note='hello' || HEX(RANDOMBLOB(2)), stamp='stamp' || ABS(RANDOM() %% 99) WHERE rowid=(ABS(RANDOM() %% 10)+1);", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
        
        sql = sqlite3_mprintf("UPDATE \"%w\" SET age = ABS(RANDOM()), note='hello' || HEX(RANDOMBLOB(2)), stamp='stamp' || ABS(RANDOM() %% 99) WHERE rowid=(ABS(RANDOM() %% 10)+1);", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
        
        // update primary key here
        sql = sqlite3_mprintf("UPDATE \"%w\" SET first_name = 'name' || HEX(RANDOMBLOB(4)) WHERE rowid=(ABS(RANDOM() %% 10)+1);", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
        
        sql = sqlite3_mprintf("UPDATE \"%w\" SET first_name = 'name' || HEX(RANDOMBLOB(4)) WHERE rowid=(ABS(RANDOM() %% 10)+1);", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
        
        // update two primary keys here
        sql = sqlite3_mprintf("UPDATE \"%w\" SET first_name = 'name' || HEX(RANDOMBLOB(4)), last_name = 'surname' || HEX(RANDOMBLOB(4)) WHERE rowid=(ABS(RANDOM() %% 10)+1);", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    if (table_mask & TEST_NOCOLS) {
        const char *table_name = CUSTOMERS_NOCOLS_TABLE;
        if (print_result) printf("TESTING RANDMOM UPDATE on %s\n", table_name);
        
        // update primary key here
        rc = sqlite3_exec(db, "UPDATE \"" CUSTOMERS_NOCOLS_TABLE "\" SET first_name = HEX(RANDOMBLOB(8)) WHERE first_name='name100001';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        rc = sqlite3_exec(db, "UPDATE \"" CUSTOMERS_NOCOLS_TABLE "\" SET first_name = HEX(RANDOMBLOB(8)) WHERE first_name='name100008';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        // update two primary keys here
        rc = sqlite3_exec(db, "UPDATE \"" CUSTOMERS_NOCOLS_TABLE "\" SET first_name = HEX(RANDOMBLOB(8)), last_name = HEX(RANDOMBLOB(8)) WHERE first_name='name100009';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    if (table_mask & TEST_NOPRIKEYS) {
        const char *table_name = "customers_noprikey";
        if (print_result) printf("TESTING RANDOM UPDATE on %s\n", table_name);
        
        // update primary key here
        rc = sqlite3_exec(db, "UPDATE customers_noprikey SET first_name = HEX(RANDOMBLOB(8)) WHERE first_name='name200001';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        rc = sqlite3_exec(db, "UPDATE customers_noprikey SET first_name = HEX(RANDOMBLOB(8)) WHERE first_name='name200008';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        // update two columns
        rc = sqlite3_exec(db, "UPDATE customers_noprikey SET first_name = HEX(RANDOMBLOB(8)), last_name = HEX(RANDOMBLOB(8)) WHERE first_name='name200009';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
finalize:
    if (rc != SQLITE_OK) {
        printf("error in do_update_random: %s\n", sqlite3_errmsg(db));
        exit(-4);
    }
}

void do_delete (sqlite3 *db, int table_mask, bool print_result) {
    int rc = SQLITE_OK;
    
    if (table_mask & TEST_PRIKEYS) {
        const char *table_name = CUSTOMERS_TABLE;
        if (print_result) printf("TESTING DELETE on %s\n", table_name);
        
        char *sql = sqlite3_mprintf("DELETE FROM \"%w\" WHERE first_name='name5';", table_name);
        int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
        
        sql = sqlite3_mprintf("DELETE FROM \"%w\" WHERE first_name='name7';", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    if (table_mask & TEST_NOCOLS) {
        const char *table_name = CUSTOMERS_NOCOLS_TABLE;
        if (print_result) printf("TESTING DELETE on %s\n", table_name);
        
        int rc = sqlite3_exec(db, "DELETE FROM \"" CUSTOMERS_NOCOLS_TABLE "\" WHERE first_name='name100005';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        rc = sqlite3_exec(db, "DELETE FROM \"" CUSTOMERS_NOCOLS_TABLE "\" WHERE first_name='name100007';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    if (table_mask & TEST_NOPRIKEYS) {
        const char *table_name = "customers_noprikey";
        if (print_result) printf("TESTING DELETE on %s\n", table_name);
        
        int rc = sqlite3_exec(db, "DELETE FROM customers_noprikey WHERE first_name='name200005';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        rc = sqlite3_exec(db, "DELETE FROM customers_noprikey WHERE first_name='name200007';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    return;
    
finalize:
    if (rc != SQLITE_OK) {
        printf("error in do_delete: %s\n", sqlite3_errmsg(db));
        exit(-4);
    }
}

bool do_test_vtab2 (void) {
    bool result = false;
    sqlite3_stmt *stmt = NULL;
    int rc = SQLITE_ERROR;
    
    // test in an in-memory database
    sqlite3 *db = do_create_database();
    if (!db) goto finalize;
    
    // create dummy table
    rc = sqlite3_exec(db, "CREATE TABLE foo (name TEXT PRIMARY KEY NOT NULL, age INTEGER);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // augment table
    rc = sqlite3_exec(db, "SELECT cloudsync_init('foo');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // insert 10 rows
    for (int i=0; i<10; ++i) {
        char sql[512];
        snprintf(sql, sizeof(sql), "INSERT INTO foo (name, age) VALUES ('name%d', %d);", i+1, i+1);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // at this point cloudsync_changes contains 10 rows
    
    // trigger cloudsync_changesvtab_close with vm not null
    const char *sql = "SELECT tbl, quote(pk), col_name, col_value, col_version, db_version, quote(site_id), cl, seq FROM cloudsync_changes;";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // step and finalize BEFORE eof
    for (int i=0; i<5; ++i) {
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) goto finalize;
    }
    
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) goto finalize;
    stmt = NULL;
    // end trigger
    
    // trigger error inside cloudsync_changesvtab_filter
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    force_vtab_filter_abort = true;
    sqlite3_step(stmt);
    force_vtab_filter_abort = false;
    
    sqlite3_finalize(stmt);
    rc = SQLITE_OK;
    stmt = NULL;
    // end trigger
    
    // trigger error inside cloudsync_changesvtab_next
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // step and finalize BEFORE eof
    for (int i=0; i<10; ++i) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_INTERRUPT) break;
        if (rc != SQLITE_ROW) goto finalize;
        if (i == 5) sqlite3_interrupt(db);
    }
    
    sqlite3_finalize(stmt);
    rc = SQLITE_OK;
    stmt = NULL;
    // end trigger
    
    result = true;
    
finalize:
    if (rc != SQLITE_OK) printf("do_test_vtab2 error: %s\n", sqlite3_errmsg(db));
    return result;
}

bool do_test_vtab(sqlite3 *db) {
    int rc = SQLITE_OK;
    
    // test a NON insert statement
    rc = sqlite3_exec(db, "UPDATE cloudsync_changes SET seq=666 WHERE db_version>1;", NULL, NULL, NULL);
    if (rc != SQLITE_MISUSE) goto finalize;
    
    // SELECT tbl, quote(pk), col_name, col_value, col_version, db_version, quote(site_id), cl, seq FROM cloudsync_changes
    rc = sqlite3_exec(db, "SELECT tbl FROM cloudsync_changes WHERE db_version>1;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db, "SELECT tbl FROM cloudsync_changes WHERE db_version>1 AND site_id=0;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db, "SELECT tbl FROM cloudsync_changes WHERE site_id=0;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db, "SELECT tbl FROM cloudsync_changes WHERE db_version>=1 AND db_version<2 AND db_version != 3 AND db_version<=5;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db, "SELECT tbl FROM cloudsync_changes WHERE db_version>1 ORDER BY site_id;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db, "SELECT tbl FROM cloudsync_changes WHERE db_version>1 ORDER BY db_version, site_id;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db, "SELECT tbl FROM cloudsync_changes WHERE db_version IS NOT NULL;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db, "SELECT tbl FROM cloudsync_changes WHERE db_version IS NULL;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db, "SELECT tbl FROM cloudsync_changes WHERE db_version IS NOT 1;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db, "SELECT tbl FROM cloudsync_changes WHERE db_version IS 1;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // cannot use a column declared as NOT NULL, otherwise SQLite optimize the contrains and the xBestIndex callback never receives the correct constraint
    rc = sqlite3_exec(db, "SELECT tbl FROM cloudsync_changes WHERE col_value ISNULL;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db, "SELECT tbl FROM cloudsync_changes WHERE col_value NOTNULL;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db, "SELECT tbl FROM cloudsync_changes WHERE non_existing_column = 1;", NULL, NULL, NULL);
    if (rc == SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db, "SELECT tbl FROM cloudsync_changes WHERE db_version GLOB 1;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db, "SELECT tbl FROM cloudsync_changes WHERE db_version LIKE 1;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    const char *name = opname_from_value (666);
    if (name != NULL) goto finalize;
    
    rc = colname_is_legal("db_version");
    if (rc != 1) goto finalize;
    
    rc = colname_is_legal("non_existing_column");
    if (rc != 0) goto finalize;
    
    return do_test_vtab2();
    
finalize:
    printf("do_test_vtab error: %s\n", sqlite3_errmsg(db));
    return false;
}

bool do_test_functions (sqlite3 *db, bool print_results) {
    int size = 0, rc2;
    char *site_id = dbutils_blob_select(db, "SELECT cloudsync_siteid();", &size, NULL, &rc2);
    if (site_id == NULL || size != 16) goto abort_test_functions;
    cloudsync_memory_free(site_id);
    
    char *site_id_str = dbutils_text_select(db, "SELECT quote(cloudsync_siteid());");
    if (site_id_str == NULL) goto abort_test_functions;
    if (print_results) printf("Site ID: %s\n", site_id_str);
    cloudsync_memory_free(site_id_str);
    
    char *version = dbutils_text_select(db, "SELECT cloudsync_version();");
    if (version == NULL) goto abort_test_functions;
    if (print_results) printf("Lib Version: %s\n", version);
    cloudsync_memory_free(version);
    
    sqlite3_int64 db_version = dbutils_int_select(db, "SELECT cloudsync_db_version();");
    if (print_results) printf("DB Version: %lld\n", db_version);
    
    sqlite3_int64 db_version_next = dbutils_int_select(db, "SELECT cloudsync_db_version_next();");
    if (print_results) printf("DB Version Next: %lld\n", db_version_next);

    int rc = sqlite3_exec(db, "CREATE TABLE tbl1 (col1 TEXT PRIMARY KEY NOT NULL, col2);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    rc = sqlite3_exec(db, "CREATE TABLE tbl2 (col1 TEXT PRIMARY KEY NOT NULL, col2);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    rc = sqlite3_exec(db, "DROP TABLE IF EXISTS rowid_table; DROP TABLE IF EXISTS nonnull_prikey_table;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    rc = sqlite3_exec(db, "SELECT cloudsync_init('*');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    rc = sqlite3_exec(db, "SELECT cloudsync_disable('tbl1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    int v1 = (int)dbutils_int_select(db, "SELECT cloudsync_is_enabled('tbl1');");
    if (v1 == 1) goto abort_test_functions;
    
    rc = sqlite3_exec(db, "SELECT cloudsync_disable('*');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    int v2 = (int)dbutils_int_select(db, "SELECT cloudsync_is_enabled('tbl2');");
    if (v2 == 1) goto abort_test_functions;
    
    rc = sqlite3_exec(db, "SELECT cloudsync_enable('tbl1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    int v3 = (int)dbutils_int_select(db, "SELECT cloudsync_is_enabled('tbl1');");
    if (v3 != 1) goto abort_test_functions;
    
    rc = sqlite3_exec(db, "SELECT cloudsync_enable('*');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    int v4 = (int)dbutils_int_select(db, "SELECT cloudsync_is_enabled('tbl2');");
    if (v4 != 1) goto abort_test_functions;
    
    rc = sqlite3_exec(db, "SELECT cloudsync_set('key1', 'value1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    rc = sqlite3_exec(db, "SELECT cloudsync_set_table('tbl1', 'key1', 'value1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    rc = sqlite3_exec(db, "SELECT cloudsync_set_column('tbl1', 'col1', 'key1', 'value1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    rc = sqlite3_exec(db, "SELECT cloudsync_cleanup('*');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    char *uuid = dbutils_text_select(db, "SELECT cloudsync_uuid();");
    if (uuid == NULL) goto abort_test_functions;
    if (print_results) printf("New uuid: %s\n", uuid);
    cloudsync_memory_free(uuid);
    
    return true;
    
abort_test_functions:
    printf("Error in do_test_functions: %s\n", sqlite3_errmsg(db));
    return false;
}

bool do_create_tables (int table_mask, sqlite3 *db) {
    // declare tables
    if (table_mask & TEST_PRIKEYS) {
        // TEST a table with a composite primary key
        char *sql = sqlite3_mprintf("CREATE TABLE IF NOT EXISTS \"%w\" (first_name TEXT NOT NULL, last_name TEXT NOT NULL, age INTEGER, note TEXT, stamp TEXT DEFAULT CURRENT_TIME, PRIMARY KEY(first_name, last_name));", CUSTOMERS_TABLE);
        int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto abort_create_tables;
    }
    
    if (table_mask & TEST_NOCOLS) {
        // TEST a table with no columns other than primary keys
        char *sql = sqlite3_mprintf("CREATE TABLE IF NOT EXISTS \"%w\" (first_name TEXT NOT NULL, last_name TEXT NOT NULL, PRIMARY KEY(first_name, last_name));", CUSTOMERS_NOCOLS_TABLE);
        int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto abort_create_tables;
    }
    
    if (table_mask & TEST_NOPRIKEYS) {
        // TEST a table with implicit rowid primary key
        const char *sql = "CREATE TABLE IF NOT EXISTS customers_noprikey (first_name TEXT NOT NULL, last_name TEXT NOT NULL);";
        if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) goto abort_create_tables;
    }
    
    return true;
    
abort_create_tables:
    printf("Error in do_create_tables: %s\n", sqlite3_errmsg(db));
    return false;
}

bool do_alter_tables (int table_mask, sqlite3 *db, int alter_version) {
    // declare tables
    if (table_mask & TEST_PRIKEYS) {
        // TEST a table with a composite primary key
        const char *sql;
        switch (alter_version) {
            case 1:
                sql = "SELECT cloudsync_begin_alter('" CUSTOMERS_TABLE "'); "
                      "ALTER TABLE \"" CUSTOMERS_TABLE "\" ADD new_column_1 TEXT; "
                      "ALTER TABLE \"" CUSTOMERS_TABLE "\" ADD new_column_2 TEXT DEFAULT 'default value'; "
                      "ALTER TABLE \"" CUSTOMERS_TABLE "\" DROP note; "
                      "SELECT cloudsync_commit_alter('" CUSTOMERS_TABLE "') ";
                break;
            case 2:
                sql = "SELECT cloudsync_begin_alter('" CUSTOMERS_TABLE "'); "
                      "ALTER TABLE \"" CUSTOMERS_TABLE "\" RENAME TO do_alter_tables_temp_customers; "
                      "CREATE TABLE \"" CUSTOMERS_TABLE "\" (first_name TEXT NOT NULL, last_name TEXT NOT NULL, note TEXT, note3 TEXT DEFAULT 'note',  note4 DATETIME DEFAULT(datetime('subsec')), stamp TEXT DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(first_name, last_name)); "
                      "INSERT INTO \"" CUSTOMERS_TABLE "\" (\"first_name\",\"last_name\",\"note\", \"note3\", \"stamp\") SELECT \"first_name\",\"last_name\",\"note\",'a new note',\"stamp\" FROM do_alter_tables_temp_customers; "
                      "DROP TABLE do_alter_tables_temp_customers; "
                      "SELECT cloudsync_commit_alter('" CUSTOMERS_TABLE "') ";
                break;
            case 3:
                sql = "SELECT cloudsync_begin_alter('" CUSTOMERS_TABLE "'); "
                      "ALTER TABLE \"" CUSTOMERS_TABLE "\" RENAME TO do_alter_tables_temp_customers; "
                      "CREATE TABLE \"" CUSTOMERS_TABLE "\" (name TEXT NOT NULL, note TEXT, note3 TEXT DEFAULT 'note',  note4 DATETIME DEFAULT(datetime('subsec')), stamp TEXT DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(name)); "
                      "INSERT INTO \"" CUSTOMERS_TABLE "\" (\"name\",\"note\", \"note3\", \"stamp\") SELECT \"first_name\" ||  \"last_name\" ,\"note\",'a new note',\"stamp\" FROM do_alter_tables_temp_customers; "
                      "DROP TABLE do_alter_tables_temp_customers; "
                      "SELECT cloudsync_commit_alter('" CUSTOMERS_TABLE "') ";
                break;
            case 4: // only add columns, not drop
                sql = "SELECT cloudsync_begin_alter('" CUSTOMERS_TABLE "'); "
                      "ALTER TABLE \"" CUSTOMERS_TABLE "\" ADD new_column_1 TEXT; "
                      "ALTER TABLE \"" CUSTOMERS_TABLE "\" ADD new_column_2 TEXT DEFAULT 'default value'; "
                      "SELECT cloudsync_commit_alter('" CUSTOMERS_TABLE "') ";
                break;
            default:
                break;
        }
        if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) goto abort_alter_tables;
    }
    
    if (table_mask & TEST_NOCOLS) {
        const char *sql;
        switch (alter_version) {
            case 1:
                sql = "SELECT cloudsync_begin_alter('" CUSTOMERS_NOCOLS_TABLE "'); "
                      "ALTER TABLE \"" CUSTOMERS_NOCOLS_TABLE "\" ADD new_column_1 TEXT; "
                      "ALTER TABLE \"" CUSTOMERS_NOCOLS_TABLE "\" ADD new_column_2 TEXT DEFAULT 'default value'; "
                      "SELECT cloudsync_commit_alter('" CUSTOMERS_NOCOLS_TABLE "'); ";
                break;
            case 2:
                sql = "SELECT cloudsync_begin_alter('" CUSTOMERS_NOCOLS_TABLE "'); "
                      "ALTER TABLE \"" CUSTOMERS_NOCOLS_TABLE "\" RENAME TO do_alter_tables_temp_customers_nocols; "
                      "CREATE TABLE \"" CUSTOMERS_NOCOLS_TABLE "\" (first_name TEXT NOT NULL, last_name_2 TEXT NOT NULL, PRIMARY KEY(first_name, last_name_2)); "
                      "INSERT INTO \"" CUSTOMERS_NOCOLS_TABLE "\" (\"first_name\",\"last_name_2\") SELECT \"first_name\",\"last_name\" FROM do_alter_tables_temp_customers_nocols; "
                      "DROP TABLE do_alter_tables_temp_customers_nocols; "
                      "SELECT cloudsync_commit_alter('" CUSTOMERS_NOCOLS_TABLE "'); "
                      "SELECT cloudsync_begin_alter('" CUSTOMERS_NOCOLS_TABLE "'); "
                      "ALTER TABLE \"" CUSTOMERS_NOCOLS_TABLE "\" RENAME TO do_alter_tables_temp_customers_nocols; "
                      "CREATE TABLE \"" CUSTOMERS_NOCOLS_TABLE "\" (first_name TEXT NOT NULL, last_name TEXT NOT NULL, PRIMARY KEY(first_name, last_name)); "
                      "INSERT INTO \"" CUSTOMERS_NOCOLS_TABLE "\" (\"first_name\",\"last_name\") SELECT \"first_name\",\"last_name_2\" FROM do_alter_tables_temp_customers_nocols; "
                      "DROP TABLE do_alter_tables_temp_customers_nocols; "
                      "SELECT cloudsync_commit_alter('" CUSTOMERS_NOCOLS_TABLE "');" ;
                break;
            case 3:
                sql = "SELECT cloudsync_begin_alter('" CUSTOMERS_NOCOLS_TABLE "'); "
                      "ALTER TABLE \"" CUSTOMERS_NOCOLS_TABLE "\" RENAME TO do_alter_tables_temp_customers_nocols; "
                      "CREATE TABLE \"" CUSTOMERS_NOCOLS_TABLE "\" (name TEXT NOT NULL PRIMARY KEY); "
                      "INSERT INTO \"" CUSTOMERS_NOCOLS_TABLE "\" (\"name\") SELECT \"first_name\" || \"last_name\" FROM do_alter_tables_temp_customers_nocols; "
                      "DROP TABLE do_alter_tables_temp_customers_nocols; "
                      "SELECT cloudsync_commit_alter('" CUSTOMERS_NOCOLS_TABLE "'); ";
                break;
            default:
                break;
        }
        if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) goto abort_alter_tables;
    }
    
    if (table_mask & TEST_NOPRIKEYS) {
        // TEST a table with implicit rowid primary key
        const char *sql;
        switch (alter_version) {
            case 1:
                sql = "SELECT cloudsync_begin_alter('customers_noprikey'); "
                      "ALTER TABLE customers_noprikey ADD new_column_1 TEXT, new_column_2 TEXT DEFAULT CURRENT_TIMESTAMP; "
                      "SELECT cloudsync_commit_alter('customers_noprikey') ";
                break;
            case 2:
                sql = "SELECT cloudsync_begin_alter('customers_noprikey'); "
                      "ALTER TABLE \"customers_noprikey\" RENAME TO do_alter_tables_temp_customers_noprikey; "
                      "CREATE TABLE \"customers_noprikey\" (first_name TEXT NOT NULL, last_name TEXT NOT NULL, note TEXT, note3 TEXT DEFAULT 'note', stamp TEXT DEFAULT CURRENT_TIMESTAMP); "
                      "INSERT INTO \"customers_noprikey\" (\"first_name\",\"last_name\",\"note\", \"note3\", \"stamp\") SELECT \"first_name\",\"last_name\",\"note\",'a new note',\"stamp\" FROM do_alter_tables_temp_customers_noprikey; "
                      "DROP TABLE do_alter_tables_temp_customers_noprikey; "
                      "SELECT cloudsync_commit_alter('customers_noprikey') "
                        "SELECT cloudsync_begin_alter('customers_noprikey'); "
                        "ALTER TABLE \"customers_noprikey\" RENAME TO do_alter_tables_temp_customers_noprikey; "
                        "CREATE TABLE customers_noprikey (first_name TEXT NOT NULL, last_name TEXT NOT NULL, PRIMARY KEY(first_name, last_name)); "
                        "INSERT INTO \"customers_noprikey\" (\"first_name\",\"last_name\") SELECT \"first_name\",\"last_name_2\" FROM do_alter_tables_temp_customers_noprikey; "
                        "DROP TABLE do_alter_tables_temp_customers_noprikey; "
                        "SELECT cloudsync_commit_alter('customers_noprikey');" ;
                break;
            case 3:
                sql = "SELECT cloudsync_begin_alter('customers_noprikey'); "
                      "ALTER TABLE \"customers_noprikey\" RENAME TO do_alter_tables_temp_customers_noprikey; "
                      "CREATE TABLE \"customers_noprikey\" (name TEXT NOT NULL, note TEXT, note3 TEXT DEFAULT 'note', stamp TEXT DEFAULT CURRENT_TIMESTAMP); "
                      "INSERT INTO \"customers_noprikey\" (\"first_name\" || \"last_name\",\"note\", \"note3\", \"stamp\") SELECT \"first_name\",\"last_name\",\"note\",'a new note',\"stamp\" FROM do_alter_tables_temp_customers_noprikey; "
                      "DROP TABLE do_alter_tables_temp_customers_noprikey; "
                      "SELECT cloudsync_commit_alter('customers_noprikey') ";
            default:
                break;
        }
        if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) goto abort_alter_tables;
    }
    
    return true;
    
abort_alter_tables:
    printf("Error in do_alter_tables %d: %s\n", alter_version, sqlite3_errmsg(db));
    return false;
}

bool do_augment_tables (int table_mask, sqlite3 *db, table_algo algo) {
    char sql[512];
    
    if (table_mask & TEST_PRIKEYS) {
        snprintf(sql, sizeof(sql), "SELECT cloudsync_init('" CUSTOMERS_TABLE "', '%s');", crdt_algo_name(algo));
        if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) goto abort_augment_tables;
    }
    
    if (table_mask & TEST_NOCOLS) {
        snprintf(sql, sizeof(sql), "SELECT cloudsync_init('" CUSTOMERS_NOCOLS_TABLE "', '%s');", crdt_algo_name(algo));
        if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) goto abort_augment_tables;
    }
    
    if (table_mask & TEST_NOPRIKEYS) {
        snprintf(sql, sizeof(sql), "SELECT cloudsync_init('customers_noprikey', '%s');", crdt_algo_name(algo));
        if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) goto abort_augment_tables;
    }
    
    return true;
    
abort_augment_tables:
    printf("Error in do_augment_tables: %s\n", sqlite3_errmsg(db));
    return false;
}

bool do_test_local (int test_mask, int table_mask, sqlite3 *db, bool print_result) {
    
    if (do_create_tables(table_mask, db) == false) {
        return false;
    }
    
    if (do_augment_tables(table_mask, db, table_algo_crdt_cls) == false) {
        return false;
    }
        
    if (test_mask & TEST_INSERT) {
        do_insert(db, table_mask, NINSERT, print_result);
    }
    
    if (test_mask & TEST_UPDATE) {
        do_update(db, table_mask, print_result);
    }
    
    if (test_mask & TEST_DELETE) {
        do_delete(db, table_mask, print_result);
    }
    
    if ((test_mask & TEST_INSERT) && (test_mask & TEST_DELETE) && (table_mask & TEST_PRIKEYS)) {
        // reinsert a previously deleted row to trigget local_update_sentinel
        // "DELETE FROM customers WHERE first_name='name5';"
        const char *sql = "INSERT INTO \"" CUSTOMERS_TABLE "\" (first_name, last_name, age, note) VALUES ('name5', 'surname5', 55, 'Reinsert a previously delete row');";
        if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
            printf("%s\n", sqlite3_errmsg(db));
            return false;
        }
    }
    
    // print results
    if (print_result) {
        printf("\n-> cloudsync_changes\n");
        do_query(db, "SELECT tbl, quote(pk), col_name, col_value, col_version, db_version, quote(site_id), cl, seq FROM cloudsync_changes WHERE site_id=cloudsync_siteid();", query_changes);
    }
    
    return true;
}

// MARK: -

int do_text_pk_cb (void *xdata, int index, int type, int64_t ival, double dval, char *pval) {
    test_value *pklist = (test_value *)xdata;
    
    // compare type
    if (type != pklist[index].type)
        return 1;
    
    // compare value
    switch (type) {
        case SQLITE_INTEGER:
            if (pklist[index].ivalue != ival)
                return 1;
            break;
        
        case SQLITE_FLOAT:
            if (pklist[index].dvalue != dval)
                return 1;
            break;
            
        case SQLITE_NULL:
            // NULL primary key values cannot exist but we handle the cases for completeness
            if ((dval != 0.0) || (ival != 0) || (pval != NULL)) return 1;
            break;
            
        case SQLITE_TEXT:
        case SQLITE_BLOB:
            // compare size first
            if ((int)ival != pklist[index].plen)
                return 1;
            if (memcmp(pklist[index].pvalue, pval, (size_t)ival) != 0)
                return 1;
            break;
    }
    
    return 0;
}

bool do_test_pk_single_value (sqlite3 *db, int type, int64_t ivalue, double dvalue, char *pvalue, bool print_result) {
    char sql[4096];
    bool result = false;
    test_value pklist[1] = {0};
    
    pklist[0].type = type;
    if (type == SQLITE_INTEGER) {
        snprintf(sql, sizeof(sql), "SELECT cloudsync_pk_encode(%lld);", ivalue);
        pklist[0].ivalue = ivalue;
    } else if (type == SQLITE_FLOAT) {
        snprintf(sql, sizeof(sql), "SELECT cloudsync_pk_encode(%f);", dvalue);
        pklist[0].dvalue = dvalue;
    } else if (type == SQLITE_NULL) {
        snprintf(sql, sizeof(sql), "SELECT cloudsync_pk_encode(NULL);");
    } else if (type == SQLITE_TEXT) {
        snprintf(sql, sizeof(sql), "SELECT cloudsync_pk_encode('%s');", pvalue);
        pklist[0].pvalue = pvalue;
        pklist[0].plen = (int)strlen(pvalue);
    } else if (type == SQLITE_BLOB) {
        snprintf(sql, sizeof(sql), "SELECT cloudsync_pk_encode(?);");
        pklist[0].pvalue = pvalue;
        pklist[0].plen = (int)ivalue;
    }
    
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    if (type == SQLITE_BLOB) {
        // bind value
        rc = sqlite3_bind_blob(stmt, 1, (const void *)pvalue, (int)ivalue, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto finalize;
    
    rc = SQLITE_OK;
    char *value = (char *)sqlite3_column_blob(stmt, 0);
    int vsize = sqlite3_column_bytes(stmt, 0);
    
    // print result (force calling the pk_decode_print_callback for code coverage)
    if (print_result == false) suppress_printf_output();
    pk_decode_prikey(value, vsize, pk_decode_print_callback, NULL);
    if (print_result == false) resume_printf_output();
    
    // compare result
    int n = pk_decode_prikey(value, vsize, do_text_pk_cb, (void *)pklist);
    if (n != 1) goto finalize;
    
    result = true;
finalize:
    if (rc != SQLITE_OK) {
        printf("SQL error: %s\n", sqlite3_errmsg(db));
        exit(-666);
    }
    if (stmt) sqlite3_finalize(stmt);
    dbutils_debug_stmt(db, true);
    
    return result;
}

bool do_test_pkbind_callback (sqlite3 *db) {
    int rc = SQLITE_OK;
    bool result = false;
    sqlite3_stmt *stmt = NULL;
    
    // compile a statement that can cover all 5 SQLITE types
    char *sql = "SELECT cloudsync_pk_encode(?, ?, ?, ?, ?);";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // bind pk values
    rc = sqlite3_bind_int(stmt, 1, 12345);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_bind_null(stmt, 2);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_bind_double(stmt, 3, 3.1415);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_bind_text(stmt, 4, "Hello World", -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto finalize;
    
    char blob[] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    rc = sqlite3_bind_blob(stmt, 5, blob, sizeof(blob), SQLITE_STATIC);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto finalize;
    
    rc = SQLITE_OK;
    char *pk = (char *)sqlite3_column_blob(stmt, 0);
    int pklen = sqlite3_column_bytes(stmt, 0);
    
    // make a copy of the buffer before resetting the vm
    char buffer[1024];
    memcpy(buffer, pk, (size_t)pklen);
    
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    
    int n = pk_decode_prikey((char *)buffer, (size_t)pklen, pk_decode_bind_callback, stmt);
    if (n != 5) goto finalize;
    
    result = true;
    
finalize:
    if (rc != SQLITE_OK) {
        printf("SQL error: %s\n", sqlite3_errmsg(db));
        exit(-666);
    }
    if (stmt) sqlite3_finalize(stmt);
    dbutils_debug_stmt(db, true);
    return result;
}

bool do_test_pk (sqlite3 *db, int ntest, bool print_result) {
    int rc = SQLITE_OK;
    sqlite3_stmt *stmt = NULL;
    bool result = false;
    
    // NOTE:
    // pk_encode function supports up to 255 values but the maximum number of parameters in an SQLite function
    // is 127 and it represents an hard limit that cannot be changed from sqlite3_limit nor from the preprocessor macro SQLITE_MAX_FUNCTION_ARG
    
    // buffer bigger enough to hold SELECT cloudsync_pk_encode (?,?,?,?,?,?,...,?) with maximum 255 bound holders
    char sql[4096];
    
    for (int i=0; i<ntest; ++i) {
        // generate a maximum number of primary keys to test (see NOTE above)
        int nkeys = (int)random_int64_range(1, 127);
        test_value pklist[255];
        
        generate_pk_test_sql(nkeys, sql);
        
        // compile statement
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        // bind values
        for (int j=0; j<nkeys; ++j) {
            // generate random type in the range 1..4 (SQLITE_INTEGER, SQLITE_FLOAT, SQLITE_TEXT, SQLITE_BLOB)
            // excluding SQLITE_NULL (5) because a primary key cannot be NULL
            int type = (int)random_int64_range(1, 4);
            pklist[j].type = type;
            switch (type) {
                case SQLITE_INTEGER: {
                    int64_t value = random_int64();
                    rc = sqlite3_bind_int64(stmt, j+1, (sqlite3_int64)value);
                    if (rc != SQLITE_OK) goto finalize;
                    pklist[j].ivalue = value;
                } break;
                case SQLITE_FLOAT: {
                    double value = random_double();
                    //printf("%d SQLITE_FLOAT: %.5f\n", j, value);
                    rc = sqlite3_bind_double(stmt, j+1, value);
                    if (rc != SQLITE_OK) goto finalize;
                    pklist[j].dvalue = value;
                } break;
                case SQLITE_TEXT:
                case SQLITE_BLOB: {
                    int size = (int)random_int64_range(1, 4096);
                    char *buffer = calloc(1, 4096);
                    if (!buffer) assert(0);
                    //printf("%d %s: %d\n", j, (type == SQLITE_TEXT) ? "SQLITE_TEXT": "SQLITE_BLOB", size);
                    (type == SQLITE_TEXT) ? random_string(buffer, size) : random_blob(buffer, size);
                    rc = (type == SQLITE_TEXT) ? sqlite3_bind_text(stmt, j+1, buffer, size, SQLITE_TRANSIENT) : sqlite3_bind_blob(stmt, j+1, buffer, size, SQLITE_TRANSIENT);
                    if (rc != SQLITE_OK) goto finalize;
                    pklist[j].pvalue = buffer;
                    pklist[j].plen = size;
                } break;
                default: {
                    assert(0);
                } break;
            }
        }
        
        // execute statement
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) goto finalize;
        
        // retrieve result of the cloudsync_pk_encode function
        if (sqlite3_column_type(stmt, 0) != SQLITE_BLOB) {
            // test fails
            rc = SQLITE_OK;
            goto finalize;
        }
        
        rc = SQLITE_OK;
        char *value = (char *)sqlite3_column_blob(stmt, 0);
        int vsize = sqlite3_column_bytes(stmt, 0);
        
        // compare result
        int n = pk_decode_prikey(value, vsize, do_text_pk_cb, (void *)pklist);
        // test fails
        if (n != nkeys) goto finalize;
        
        // cleanup memory
        sqlite3_finalize(stmt);
        stmt = NULL;
        for (int i=0; i<nkeys; ++i) {
            int t = pklist[i].type;
            if ((t == SQLITE_TEXT) || (t == SQLITE_BLOB)) free(pklist[i].pvalue);
        }
    }
    
    // test max/min
    if (do_test_pk_single_value(db, SQLITE_INTEGER, INT64_MAX, 0, NULL, print_result) == false) goto finalize;
    if (do_test_pk_single_value(db, SQLITE_INTEGER, INT64_MIN, 0, NULL, print_result) == false) goto finalize;
    if (do_test_pk_single_value(db, SQLITE_INTEGER, -15592946911031981, 0, NULL, print_result) == false) goto finalize;
    if (do_test_pk_single_value(db, SQLITE_INTEGER, -922337203685477580, 0, NULL, print_result) == false) goto finalize;
    if (do_test_pk_single_value(db, SQLITE_FLOAT, 0, -9223372036854775.808, NULL, print_result) == false) goto finalize;
    if (do_test_pk_single_value(db, SQLITE_NULL, 0, 0, NULL, print_result) == false) goto finalize;
    if (do_test_pk_single_value(db, SQLITE_TEXT, 0, 0, "Hello World", print_result) == false) goto finalize;
    char blob[] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    if (do_test_pk_single_value(db, SQLITE_BLOB, sizeof(blob), 0, blob, print_result) == false) goto finalize;
    
    // test bind callback
    do_test_pkbind_callback(db);
    
    result = true;
finalize:
    if (rc != SQLITE_OK) {
        printf("SQL error: %s\n", sqlite3_errmsg(db));
        exit(-666);
    }
    if (stmt) sqlite3_finalize(stmt);
    dbutils_debug_stmt(db, true);
    return result;
}

// MARK: -

bool do_test_uuid (sqlite3 *db, int ntest, bool print_result) {
    bool result = false;
    uint8_t uuid_first[UUID_LEN];
    uint8_t uuid_last[UUID_LEN];
    
    if (cloudsync_uuid_v7(uuid_first) != 0) goto finalize;
    for (int i=0; i<ntest; ++i) {
        uint8_t uuid1[UUID_LEN];
        uint8_t uuid2[UUID_LEN];
        char uuid1str[UUID_STR_MAXLEN];
        char uuid2str[UUID_STR_MAXLEN];
        
        // generate two UUIDs
        if (cloudsync_uuid_v7(uuid1) != 0) goto finalize;
        if (cloudsync_uuid_v7(uuid2) != 0) goto finalize;
        
        // stringify the newly generated UUIDs
        if (cloudsync_uuid_v7_stringify(uuid1, uuid1str, true) == NULL) goto finalize;
        if (cloudsync_uuid_v7_stringify(uuid2, uuid2str, false) == NULL) goto finalize;
        
        // compare UUIDs (the 2nd value can be greater than the first one, if the timestamp is the same)
        cloudsync_uuid_v7_compare(uuid1, uuid2);
        
        // generate two UUID strings (just to increase code coverage)
        if (cloudsync_uuid_v7_string(uuid1str, true) == NULL) goto finalize;
        if (cloudsync_uuid_v7_string(uuid2str, false) == NULL) goto finalize;
    }
    // to increase code coverage
    if (cloudsync_uuid_v7(uuid_last) != 0) goto finalize;
    cloudsync_uuid_v7_compare(uuid_first, uuid_last);
    
    result = true;
    
finalize:
    return result;
}

// MARK: -

int do_test_compare_values (sqlite3 *db, char *sql1, char *sql2, int *result, bool print_result) {
    int rc = SQLITE_OK;
    sqlite3_stmt *stmt1 = NULL;
    sqlite3_stmt *stmt2 = NULL;
    
    rc = sqlite3_prepare_v2(db, sql1, -1, &stmt1, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_prepare_v2(db, sql2, -1, &stmt2, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_step(stmt1);
    if (rc != SQLITE_ROW) goto finalize;
    
    rc = sqlite3_step(stmt2);
    if (rc != SQLITE_ROW) goto finalize;
    
    sqlite3_value *value1 = sqlite3_column_value(stmt1, 0);
    sqlite3_value *value2 = sqlite3_column_value(stmt2, 0);
    if ((value1 == NULL) || (value2 == NULL)) goto finalize;
    sqlite3_value *values[]= {value1, value2};
    
    // print result (force calling the pk_decode_print_callback for code coverage)
    if (print_result == false) suppress_printf_output();
    dbutils_debug_values(2, values);
    if (print_result == false) resume_printf_output();
    
    *result = dbutils_value_compare(value1, value2);
    rc = SQLITE_OK;
    
finalize:
    if (stmt1) sqlite3_finalize(stmt1);
    if (stmt2) sqlite3_finalize(stmt2);
    return rc;
}

bool do_test_compare (sqlite3 *db, bool print_result) {
    int result;
    
    // INTEGER comparison
    char *sql_int1 = "SELECT 1";
    char *sql_int2 = "SELECT 2";
    
    if (do_test_compare_values(db, sql_int1, sql_int2, &result, print_result) != SQLITE_OK) goto finalize;
    if (result >= 0) goto finalize;
    
    if (do_test_compare_values(db, sql_int2, sql_int1, &result, print_result) != SQLITE_OK) goto finalize;
    if (result <= 0) goto finalize;
    
    if (do_test_compare_values(db, sql_int1, sql_int1, &result, print_result) != SQLITE_OK) goto finalize;
    if (result != 0) goto finalize;
    
    // FLOAT COMPARISON
    char *sql_float1 = "SELECT 3.1415;";
    char *sql_float2 = "SELECT 6.2830;";
    
    if (do_test_compare_values(db, sql_float1, sql_float2, &result, print_result) != SQLITE_OK) goto finalize;
    if (result >= 0) goto finalize;
    
    if (do_test_compare_values(db, sql_float2, sql_float1, &result, print_result) != SQLITE_OK) goto finalize;
    if (result <= 0) goto finalize;
    
    if (do_test_compare_values(db, sql_float1, sql_float1, &result, print_result) != SQLITE_OK) goto finalize;
    if (result != 0) goto finalize;
    
    // TEXT COMPARISON
    char *sql_text1 = "SELECT 'Hello World1';";
    char *sql_text2 = "SELECT 'Hello World2';";
    
    if (do_test_compare_values(db, sql_text1, sql_text2, &result, print_result) != SQLITE_OK) goto finalize;
    if (result >= 0) goto finalize;
    
    if (do_test_compare_values(db, sql_text2, sql_text1, &result, print_result) != SQLITE_OK) goto finalize;
    if (result <= 0) goto finalize;
    
    if (do_test_compare_values(db, sql_text1, sql_text1, &result, print_result) != SQLITE_OK) goto finalize;
    if (result != 0) goto finalize;
    
    // BLOB COMPARISON
    char *sql_blob1 = "SELECT zeroblob(90);";
    char *sql_blob2 = "SELECT zeroblob(100);";
    
    if (do_test_compare_values(db, sql_blob1, sql_blob2, &result, print_result) != SQLITE_OK) goto finalize;
    if (result >= 0) goto finalize;
    
    if (do_test_compare_values(db, sql_blob2, sql_blob1, &result, print_result) != SQLITE_OK) goto finalize;
    if (result <= 0) goto finalize;
    
    if (do_test_compare_values(db, sql_blob1, sql_blob1, &result, print_result) != SQLITE_OK) goto finalize;
    if (result != 0) goto finalize;
    
    // NULL COMPARISON
    char *sql_null1 = "SELECT NULL;";
    char *sql_null2 = "SELECT NULL;";
    
    if (do_test_compare_values(db, sql_null1, sql_null2, &result, print_result) != SQLITE_OK) goto finalize;
    if (result != 0) goto finalize;
    
    // TYPE COMPARISON
    if (do_test_compare_values(db, sql_int1, sql_float1, &result, print_result) != SQLITE_OK) goto finalize;
    if (result == 0) goto finalize;
    
    return true;
    
finalize:
    return false;
}

bool do_test_rowid (int ntest, bool print_result) {
    for (int i=0; i<ntest; ++i) {
        // for an explanation see https://github.com/sqliteai/sqlite-sync/blob/main/docs/RowID.md
        sqlite3_int64 db_version = (sqlite3_int64)random_int64_range(1, 17179869183);
        sqlite3_int64 seq = (sqlite3_int64)random_int64_range(1, 1073741823);
        sqlite3_int64 rowid = (db_version << 30) | seq;
        
        sqlite3_int64 value1;
        sqlite3_int64 value2;
        cloudsync_rowid_decode(rowid, &value1, &value2);
        if (value1 != db_version)  return false;
        if (value2 != seq) return false;
    }
    
    // special case that failed in an old version
    sqlite3_int64 db_version = 14963874252;
    sqlite3_int64 seq = 172784902;
    sqlite3_int64 rowid = (db_version << 30) | seq;
    
    sqlite3_int64 value1;
    sqlite3_int64 value2;
    cloudsync_rowid_decode(rowid, &value1, &value2);
    if (value1 != db_version)  return false;
    if (value2 != seq) return false;
    
    return true;
}

bool do_test_algo_names (void) {
    if (crdt_algo_name(table_algo_none) != NULL) return false;
    if (strcmp(crdt_algo_name(table_algo_crdt_cls), "cls") != 0) return false;
    if (strcmp(crdt_algo_name(table_algo_crdt_gos), "gos") != 0) return false;
    if (strcmp(crdt_algo_name(table_algo_crdt_dws), "dws") != 0) return false;
    if (strcmp(crdt_algo_name(table_algo_crdt_aws), "aws") != 0) return false;
    if (crdt_algo_name(666) != NULL) return false;
    
    return true;
}

bool do_test_dbutils (void) {
    // test in an in-memory database
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) goto finalize;
    
    // manually load extension
    sqlite3_cloudsync_init(db, NULL, NULL);
    cloudsync_set_payload_apply_callback(db, unittest_payload_apply_rls_callback);

    const char *sql = "CREATE TABLE IF NOT EXISTS foo (name TEXT PRIMARY KEY NOT NULL, age INTEGER, note TEXT, stamp TEXT DEFAULT CURRENT_TIME);"
    "CREATE TABLE IF NOT EXISTS bar (name TEXT PRIMARY KEY NOT NULL, age INTEGER, note TEXT, stamp TEXT DEFAULT CURRENT_TIME);"
    "CREATE TABLE IF NOT EXISTS rowid_table (name TEXT, age INTEGER);"
    "CREATE TABLE IF NOT EXISTS nonnull_prikey_table (name TEXT PRIMARY KEY, age INTEGER);"
    "CREATE TABLE IF NOT EXISTS nonnull_nodefault_table (name TEXT PRIMARY KEY NOT NULL, stamp TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS nonnull_default_table (name TEXT PRIMARY KEY NOT NULL, stamp TEXT NOT NULL DEFAULT CURRENT_TIME);"
    "CREATE TABLE IF NOT EXISTS integer_pk (id INTEGER PRIMARY KEY NOT NULL, value);"
    "CREATE TABLE IF NOT EXISTS int_pk (id INT PRIMARY KEY NOT NULL, value);"
    "CREATE TABLE IF NOT EXISTS \"quoted table name 🚀\" (\"pk quoted col 1\" TEXT NOT NULL, \"pk quoted col 2\" TEXT NOT NULL, \"non pk quoted col 1\", \"non pk quoted col 2\", PRIMARY KEY (\"pk quoted col 1\", \"pk quoted col 2\"));";
;
    
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // augment foo with cloudsync
    rc = sqlite3_exec(db, "SELECT cloudsync_init('foo');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // re-augment foo
    rc = sqlite3_exec(db, "SELECT cloudsync_init('foo');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // augment bar with cloudsync
    rc = sqlite3_exec(db, "SELECT cloudsync_init('bar', 'gos');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db, "SELECT cloudsync_init('quoted table name 🚀');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;

    // test dbutils_write
    sql = "INSERT INTO foo (name, age, note) VALUES (?, ?, ?);";
    const char *values[] = {"Test1", "3.1415", NULL};
    int type[] = {SQLITE_TEXT, SQLITE_FLOAT, SQLITE_NULL};
    int len[] = {5, 0, 0};
    rc = dbutils_write(db, NULL, sql, values, type, len, 3);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = "INSERT INTO foo2 (name) VALUES ('Error');";
    rc = dbutils_write(db, NULL, sql, NULL, NULL, NULL, -1);
    if (rc == SQLITE_OK) goto finalize;
    
    // test dbutils_text_select
    sql = "INSERT INTO foo (name) VALUES ('Test2')";
    rc = dbutils_write_simple(db, sql);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = "INSERT INTO \"quoted table name 🚀\" (\"pk quoted col 1\", \"pk quoted col 2\", \"non pk quoted col 1\", \"non pk quoted col 2\") VALUES ('pk1', 'pk2', 'nonpk1', 'nonpk2');";
    rc = dbutils_write(db, NULL, sql, NULL, NULL, NULL, -1);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = "SELECT * FROM cloudsync_changes();";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    sqlite3_int64 i64_value = dbutils_int_select(db, "SELECT NULL;");
    if (i64_value != 0) goto finalize;
    
    rc = dbutils_register_function(db, NULL, NULL, 0, NULL, NULL, NULL);
    if (rc == SQLITE_OK) goto finalize;
    
    rc = dbutils_register_aggregate(db, NULL, NULL, NULL, 0, NULL, NULL, NULL);
    if (rc == SQLITE_OK) goto finalize;
    
    bool b = dbutils_system_exists(db, "non_existing_table", "non_existing_type");
    if (b == true) goto finalize;
    
    // test dbutils_table_sanity_check
    b = dbutils_table_sanity_check(db, NULL, NULL, false);
    if (b == true) goto finalize;
    b = dbutils_table_sanity_check(db, NULL, "rowid_table", false);
    if (b == true) goto finalize;
    b = dbutils_table_sanity_check(db, NULL, "foo2", false);
    if (b == true) goto finalize;
    b = dbutils_table_sanity_check(db, NULL, build_long_tablename(), false);
    if (b == true) goto finalize;
    b = dbutils_table_sanity_check(db, NULL, "nonnull_prikey_table", false);
    if (b == true) goto finalize;
    b = dbutils_table_sanity_check(db, NULL, "nonnull_nodefault_table", false);
    if (b == true) goto finalize;
    b = dbutils_table_sanity_check(db, NULL, "nonnull_default_table", false);
    if (b == false) goto finalize;
    b = dbutils_table_sanity_check(db, NULL, "integer_pk", false);
    if (b == true) goto finalize;
    b = dbutils_table_sanity_check(db, NULL, "integer_pk", true);
    if (b == false) goto finalize;
    b = dbutils_table_sanity_check(db, NULL, "int_pk", false);
    if (b == true) goto finalize;
    b = dbutils_table_sanity_check(db, NULL, "int_pk", true);
    if (b == false) goto finalize;
    b = dbutils_table_sanity_check(db, NULL, "quoted table name 🚀", true);
    if (b == false) goto finalize;
    
    // create huge dummy_table table
    rc = sqlite3_exec(db, build_huge_table(), NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // sanity check the huge dummy_table table
    b = dbutils_table_sanity_check(db, NULL, "dummy_table", false);
    if (b == true) goto finalize;
    
    // de-augment bar with cloudsync
    rc = sqlite3_exec(db, "SELECT cloudsync_cleanup('bar');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // test settings
    dbutils_settings_set_key_value(db, NULL, "key1", "test1");
    dbutils_settings_set_key_value(db, NULL, "key2", "test2");
    dbutils_settings_set_key_value(db, NULL, "key2", NULL);
    
    char *value1 = dbutils_settings_get_value(db, "key1", NULL, 0);
    char *value2 = dbutils_settings_get_value(db, "key2", NULL, 0);
    if (value1 == NULL) goto finalize;
    if (value2 != NULL) goto finalize;
    cloudsync_memory_free(value1);
    
    // test table settings
    rc = dbutils_table_settings_set_key_value(db, NULL, NULL, NULL, NULL, NULL);
    if (rc != SQLITE_ERROR) goto finalize;
    
    rc = dbutils_table_settings_set_key_value(db, NULL, "foo", NULL, "key1", "value1");
    if (rc != SQLITE_OK) goto finalize;
    
    rc = dbutils_table_settings_set_key_value(db, NULL, "foo", NULL, "key2", "value2");
    if (rc != SQLITE_OK) goto finalize;
    
    rc = dbutils_table_settings_set_key_value(db, NULL, "foo", NULL, "key2", NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = SQLITE_ERROR;
    
    value1 = dbutils_table_settings_get_value(db, "foo", NULL, "key1", NULL, 0);
    value2 = dbutils_table_settings_get_value(db, "foo", NULL, "key2", NULL, 0);
    if (value1 == NULL) goto finalize;
    if (value2 != NULL) goto finalize;
    cloudsync_memory_free(value1);
    
    sqlite3_int64 db_version = dbutils_int_select(db, "SELECT cloudsync_db_version();");

    char *site_id_blob;
    int site_id_blob_size;
    sqlite3_int64 dbver1, seq1;
    rc = dbutils_blob_int_int_select(db, "SELECT cloudsync_siteid(),  cloudsync_db_version(),  cloudsync_seq();", &site_id_blob, &site_id_blob_size, &dbver1, &seq1);
    if (rc != SQLITE_OK || site_id_blob == NULL ||dbver1 != db_version) goto finalize;
    cloudsync_memory_free(site_id_blob);
    
    // force out-of-memory test
    value1 = dbutils_settings_get_value(db, "key1", OUT_OF_MEMORY_BUFFER, 0);
    if (value1 != NULL) goto finalize;
    
    value1 = dbutils_table_settings_get_value(db, "foo", NULL, "key1", OUT_OF_MEMORY_BUFFER, 0);
    if (value1 != NULL) goto finalize;

    char *p = NULL;
    dbutils_select(db, "SELECT zeroblob(16);", NULL, NULL, NULL, 0, SQLITE_NOMEM);
    if (p != NULL) goto finalize;
    
    dbutils_settings_set_key_value(db, NULL, CLOUDSYNC_KEY_LIBVERSION, "0.0.0");
    int cmp = dbutils_settings_check_version(db);
    if (cmp == 0) goto finalize;
    
    dbutils_settings_set_key_value(db, NULL, CLOUDSYNC_KEY_LIBVERSION, CLOUDSYNC_VERSION); 
    cmp = dbutils_settings_check_version(db);
    if (cmp != 0) goto finalize;

    //dbutils_settings_table_load_callback(NULL, 0, NULL, NULL);
    dbutils_migrate(NULL);
    
    dbutils_settings_cleanup(db);
    
    int n1 = 1;
    int n2 = 2;
    cmp = binary_comparison(n1, n2);
    if (cmp != -1) goto finalize;
    cmp = binary_comparison(n2, n1);
    if (cmp != 1) goto finalize;
    cmp = binary_comparison(n1, n1);
    if (cmp != 0) goto finalize;
    
    rc = SQLITE_OK;

finalize:
    if (rc != SQLITE_OK) printf("%s\n", sqlite3_errmsg(db));
    db = close_db(db);
    return (rc == SQLITE_OK);
}

bool do_test_others (sqlite3 *db) {
    // test unfinalized statement just to increase code coverage
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db, "SELECT 1;", -1, &stmt, NULL);
    int count = dbutils_debug_stmt(db, false);
    sqlite3_finalize(stmt);
    // to increase code coverage
    dbutils_context_result_error(NULL, "Test is: %s", "Hello World");
    return (count == 1);
}

// test error cases to increase code coverage
bool do_test_error_cases (sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    
    // test cloudsync_init missing table
    sqlite3_prepare_v2(db, "SELECT cloudsync_init('missing_table');", -1, &stmt, NULL);
    int res = stmt_execute(stmt, NULL);
    sqlite3_finalize(stmt);
    if (res != -1) return false;
    
    // test missing algo
    const char *sql = "CREATE TABLE IF NOT EXISTS foo2 (id TEXT PRIMARY KEY NOT NULL, value);"
    "SELECT cloudsync_init('foo2', 'missing_algo');";
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_ERROR) return false;
    
    // test error
    sql = "SELECT cloudsync_begin_alter('foo2');";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_MISUSE) return false;
    
    // test error
    sql = "SELECT cloudsync_commit_alter('foo2');";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_MISUSE) return false;

    return true;
}

bool do_test_internal_functions (void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *vm = NULL;
    bool result = false;
    const char *sql = NULL;
    
    // INIT
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) goto abort_test;
    
    rc = sqlite3_exec(db, "SELECT \"double-quoted string literal misfeature\"", NULL, NULL, NULL);
    if (rc != SQLITE_ERROR) goto abort_test;
    
    sql = "CREATE TABLE foo (name TEXT PRIMARY KEY NOT NULL, age INTEGER UNIQUE);";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test;
    
    // TEST 1 (count returns DONE)
    sql = "INSERT INTO foo (name) VALUES ('TestInternalFunctions')";
    rc = sqlite3_prepare(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto abort_test;
    
    int res = stmt_count(vm, NULL, 0, 0);
    if (res != 0) goto abort_test;
    if (vm) sqlite3_finalize(vm);
    vm = NULL;
    
    // TEST 2 (stmt_execute returns an error)
    sql = "INSERT INTO foo (name, age) VALUES ('Name1', 22)";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test;
    
    rc = sqlite3_prepare(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto abort_test;
    
    // this statement must fail
    res = stmt_execute(vm, NULL);
    if (res != -1) goto abort_test;
    if (vm) sqlite3_finalize(vm);
    vm = NULL;
    
    result = true;
    
abort_test:
    if (vm) sqlite3_finalize(vm);
    if (db) sqlite3_close(db);
    return result;
}

bool do_test_string_replace_prefix(void) {
    char *host = "rejfwkr.sqlite.cloud";
    char *prefix = "sqlitecloud://";
    char *replacement = "https://";
    
    char string[512];
    snprintf(string, sizeof(string), "%s%s", prefix, host);
    char expected[512];
    snprintf(expected, sizeof(expected), "%s%s", replacement, host);
    
    char *replaced = cloudsync_string_replace_prefix(string, prefix, replacement);
    if (string == replaced || strcmp(replaced, expected) != 0) return false;
    if (string != replaced) cloudsync_memory_free(replaced);
    
    replaced = cloudsync_string_replace_prefix(expected, prefix, replacement);
    if (expected != replaced) return false;
    
    return true;
}

// MARK: -

bool do_compare_queries (sqlite3 *db1, const char *sql1, sqlite3 *db2, const char *sql2, int col_to_skip, int col_tombstone, bool display_column) {
    sqlite3_stmt *vm1 = NULL;
    sqlite3_stmt *vm2 = NULL;
    int rc1 = SQLITE_OK;
    int rc2 = SQLITE_OK;
    bool result = false;
    
    // compile vm(s)
    rc1 = sqlite3_prepare_v2(db1, sql1, -1, &vm1, NULL);
    if (rc1 != SQLITE_OK) goto finalize;
    
    rc2 = sqlite3_prepare_v2(db2, sql2, -1, &vm2, NULL);
    if (rc2 != SQLITE_OK) goto finalize;
    
    // compare number of columns
    int col1 = sqlite3_column_count(vm1);
    int col2 = sqlite3_column_count(vm2);
    if (col1 != col2) {
        printf("Columns not equal: %d != %d\n", col1, col2);
        goto finalize;
    }
    
    while (1) {
        rc1 = sqlite3_step(vm1);
        rc2 = sqlite3_step(vm2);
        if (rc1 != rc2) {
            printf("do_compare_queries -> sqlite3_step reports a different result:  %d != %d\n", rc1, rc2);
            goto finalize;
        }
        // rc(s) are equals here
        if (rc1 == SQLITE_DONE) break;
        if (rc1 != SQLITE_ROW) goto finalize;
        
        // we have a ROW here
        for (int i=0; i<col1; ++i) {
            if (i != col_to_skip) {
                sqlite3_value *value1 = sqlite3_column_value(vm1, i);
                sqlite3_value *value2 = sqlite3_column_value(vm2, i);
                if (dbutils_value_compare(value1, value2) != 0) {
                    // handle special case for TOMBSTONE values
                    if ((i == col_tombstone) && (sqlite3_column_type(vm1, i) == SQLITE_TEXT) && (sqlite3_column_type(vm2, i) == SQLITE_TEXT)) {
                        const char *text1 = (const char *)sqlite3_column_text(vm1, i);
                        const char *text2 = (const char *)sqlite3_column_text(vm2, i);
                        if ((strcmp(text1, "__[RIP]__") == 0) && (strcmp(text2, "-1") == 0)) continue;
                    }
                    
                    printf("Values are different!\n");
                    dbutils_debug_value(value1);
                    dbutils_debug_value(value2);
                    rc1 = rc2 = SQLITE_OK;
                    goto finalize;
                }
                else {
                    if (display_column) dbutils_debug_value(value1);
                }
            }
        }
    }
    
    rc1 = rc2 = SQLITE_OK;
    result = true;
    
finalize:
    if (rc1 != SQLITE_OK) printf("Error: %s\n", sqlite3_errmsg(db1));
    if (rc2 != SQLITE_OK) printf("Error: %s\n", sqlite3_errmsg(db2));
    
    if (vm1) sqlite3_finalize(vm1);
    if (vm2) sqlite3_finalize(vm2);
    return result;
}

bool do_merge_values (sqlite3 *srcdb, sqlite3 *destdb, bool only_local) {
    // select changes from src and write changes to dest
    sqlite3_stmt *select_stmt = NULL;
    sqlite3_stmt *insert_stmt = NULL;
    bool result = false;
    
    // select changes
    const char *sql;
    if (only_local) sql = "SELECT tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq FROM cloudsync_changes WHERE site_id=cloudsync_siteid();";
    else sql = "SELECT tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq FROM cloudsync_changes;";
    int rc = sqlite3_prepare_v2(srcdb, sql, -1, &select_stmt, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // write changes
    sql = "INSERT INTO cloudsync_changes(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) VALUES (?,?,?,?,?,?,?,?,?);";
    rc = sqlite3_prepare_v2(destdb, sql, -1, &insert_stmt, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    while (1) {
        rc = sqlite3_step(select_stmt);
        
        if (rc == SQLITE_DONE) {
            break;
        }
        
        if (rc != SQLITE_ROW) {
            goto finalize;
        }
        
        // we have a row, so bind each individual column to the INSERT statement
        int ncols = sqlite3_column_count(select_stmt);
        assert(ncols == 9);
        
        for (int j=0; j<ncols; ++j) {
            rc = sqlite3_bind_value(insert_stmt, j+1, sqlite3_column_value(select_stmt, j));
            if (rc != SQLITE_OK) {
                goto finalize;
            }
        }
        
        // perform the INSERT statement
        rc = sqlite3_step(insert_stmt);
        if (rc != SQLITE_DONE) {
            goto finalize;
        }
        
        stmt_reset(insert_stmt);
    }
    
    rc = SQLITE_OK;
    result = true;
    
finalize:
    if (rc != SQLITE_OK) printf("Error in do_merge_values %s - %s\n", sqlite3_errmsg(srcdb), sqlite3_errmsg(destdb));
    if (select_stmt) sqlite3_finalize(select_stmt);
    if (insert_stmt) sqlite3_finalize(insert_stmt);
    return result;
}

bool do_merge_enc_dec_values (sqlite3 *srcdb, sqlite3 *destdb, bool only_local, bool print_error_msg) {
    // select changes from src and write changes to dest
    sqlite3_stmt *select_stmt = NULL;
    sqlite3_stmt *insert_stmt = NULL;
    bool result = false;
    
    // select changes
    const char *sql;
    if (only_local) sql = "SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) FROM cloudsync_changes WHERE site_id=cloudsync_siteid();";
    else sql = "SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) FROM cloudsync_changes;";
    int rc = sqlite3_prepare_v2(srcdb, sql, -1, &select_stmt, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // write changes
    sql = "SELECT cloudsync_payload_decode(?);";
    rc = sqlite3_prepare_v2(destdb, sql, -1, &insert_stmt, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    while (1) {
        rc = sqlite3_step(select_stmt);
        
        if (rc == SQLITE_DONE) {
            break;
        }
        
        if (rc != SQLITE_ROW) {
            goto finalize;
        }
        
        // we have a row, so bind each individual column to the INSERT statement
        int ncols = sqlite3_column_count(select_stmt);
        assert(ncols == 1);
        
        sqlite3_value *value = sqlite3_column_value(select_stmt, 0);
        if (sqlite3_value_type(value) == SQLITE_NULL) continue;
        
        rc = sqlite3_bind_value(insert_stmt, 1, value);
        if (rc != SQLITE_OK) {
            goto finalize;
        }
        
        // perform the INSERT statement (SELECT cloudsync_payload_decode, it returns a row)
        rc = sqlite3_step(insert_stmt);
        if (rc != SQLITE_ROW) {
            goto finalize;
        }
        
        stmt_reset(insert_stmt);
    }
    
    rc = SQLITE_OK;
    result = true;
    
finalize:
    if (rc != SQLITE_OK && print_error_msg) printf("Error in do_merge_enc_dec_values %s - %s\n", sqlite3_errmsg(srcdb), sqlite3_errmsg(destdb));
    if (select_stmt) rc = sqlite3_finalize(select_stmt);
    if (insert_stmt) rc = sqlite3_finalize(insert_stmt);
    return result;
}

bool do_merge (sqlite3 *db[MAX_SIMULATED_CLIENTS], int nclients, bool only_local) {
    for (int i=0; i<nclients; ++i) {
        int target = i;
        for (int j=0; j<nclients; ++j) {
            if (target == j) continue;
            if (do_merge_enc_dec_values(db[target], db[j], only_local, true) == false) {
                return false;
            }
        }
    }
    return true;
}

sqlite3 *do_create_database (void) {
    sqlite3 *db = NULL;
    
    // open in-memory database
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) {
        printf("Error in do_create_database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    
    // manually load extension
    sqlite3_cloudsync_init(db, NULL, NULL);
    cloudsync_set_payload_apply_callback(db, unittest_payload_apply_rls_callback);

    return db;
}

void do_build_database_path (char buf[256], int i, time_t timestamp, int ntest) {
    #ifdef __ANDROID__
    sprintf(buf, "%s/cloudsync-test-%ld-%d-%d.sqlite", ".", timestamp, ntest, i);
    #else
    sprintf(buf, "%s/cloudsync-test-%ld-%d-%d.sqlite", getenv("HOME"), timestamp, ntest, i);
    #endif
}

sqlite3 *do_create_database_file (int i, time_t timestamp, int ntest) {
    sqlite3 *db = NULL;

    // open database in home dir
    char buf[256];
    do_build_database_path(buf, i, timestamp, ntest);
    int rc = sqlite3_open(buf, &db);
    if (rc != SQLITE_OK) {
        printf("Error in do_create_database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    
    // manually load extension
    sqlite3_cloudsync_init(db, NULL, NULL);
    cloudsync_set_payload_apply_callback(db, unittest_payload_apply_rls_callback);

    return db;
}

bool do_test_merge (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 2) {
        nclients = 2;
        printf("Number of test merge increased to %d clients\n", 2);
    }
    
    // create databases and tables
    int table_mask = TEST_PRIKEYS;
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        if (do_create_tables(table_mask, db[i]) == false) {
            return false;
        }
        
        if (do_augment_tables(table_mask, db[i], table_algo_crdt_cls) == false) {
            return false;
        }
    }
    
    // insert, update and delete some data in all clients
    for (int i=0; i<nclients; ++i) {
        do_insert(db[i], table_mask, NINSERT, print_result);
        if (i==0) do_update(db[i], table_mask, print_result);
        if (i!=nclients-1) do_delete(db[i], table_mask, print_result);
    }
    
    // merge all changes
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // compare results
    for (int i=1; i<nclients; ++i) {
        const char *sql = "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY first_name, last_name;";
        if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> customers\n");
        do_query(db[0], "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY first_name, last_name;", query_table);
    }
    
    result = true;
    rc = SQLITE_OK;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) {
            result = false;
            printf("do_test_merge error: %s\n", sqlite3_errmsg(db[i]));
        }
        
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge error: db %d is in transaction\n", i);
            }
            
            int counter = close_db_v2(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete(buf);
        }
    }
    return result;
}

// Test a sequence of random different concurrent changes in various clients
// with changes to pk columns and non-pk columns.
bool do_test_merge_2 (int nclients, int table_mask, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    int nrows = NINSERT;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 2) {
        nclients = 2;
        printf("Number of test merge increased to %d clients\n", 2);
    }
    
    // create databases and tables
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        if (do_create_tables(table_mask, db[i]) == false) {
            return false;
        }
        
        if (do_augment_tables(table_mask, db[i], table_algo_crdt_cls) == false) {
            return false;
        }
    }
    
    // insert, update and delete some data in some clients
    for (int i=0; i<nclients-1; ++i) {
        do_insert(db[i], table_mask, nrows, print_result);
        if (i%2 == 0) {
            if (nrows == NINSERT) {
                do_update_random(db[i], table_mask, print_result);
            } else {
                rc = sqlite3_exec(db[i], "UPDATE \"" CUSTOMERS_TABLE "\" SET age = ABS(RANDOM()), note='hello' || HEX(RANDOMBLOB(2)), stamp='stamp' || ABS(RANDOM() % 99) WHERE rowid=1;", NULL, NULL, NULL);
                if (rc != SQLITE_OK) goto finalize;
            }
        }
        if (i%2 != 0) do_delete(db[i], table_mask, print_result);
    }
    
    // merge changes from/to only some clients, not the last one
    if (do_merge(db, nclients-1, true) == false) {
        goto finalize;
    }
    
    // insert data in the last customer
    do_insert(db[nclients-1], table_mask, NINSERT, print_result);
    
    // update some random data in all clients except the last one
    for (int i=0; i<nclients; ++i) {
        if (nrows == NINSERT) {
            do_update_random(db[i], table_mask, print_result);
        } else {
            rc = sqlite3_exec(db[i], "UPDATE \"" CUSTOMERS_TABLE "\" SET age = ABS(RANDOM()), note='hello' || HEX(RANDOMBLOB(2)), stamp='stamp' || ABS(RANDOM() % 99) WHERE rowid=1;", NULL, NULL, NULL);
            if (rc != SQLITE_OK) goto finalize;
        }
    }
    
    // deleta data in the first customer
    do_delete(db[0], table_mask, print_result);
        
    // merge all changes
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
            
    // compare results
    for (int i=1; i<nclients; ++i) {
        if (table_mask & TEST_PRIKEYS) {
            const char *sql = "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY first_name, last_name;";
            if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
        }
        if (table_mask & TEST_NOCOLS) {
            const char *sql = "SELECT * FROM \"" CUSTOMERS_NOCOLS_TABLE "\" ORDER BY first_name, last_name;";
            if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
        }
    }
    
    if (print_result) {
        if (table_mask & TEST_PRIKEYS) {
            printf("\n-> " CUSTOMERS_TABLE "\n");
            do_query(db[0], "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY first_name, last_name;", query_table);
        }
        if (table_mask & TEST_NOCOLS) {
            printf("\n-> \"" CUSTOMERS_NOCOLS_TABLE "s\"\n");
            do_query(db[0], "SELECT * FROM \"" CUSTOMERS_NOCOLS_TABLE "\" ORDER BY first_name, last_name;", query_table);
        }
    }
    
    result = true;
    rc = SQLITE_OK;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge error: db %d is in transaction\n", i);
            }
            
            int counter = close_db_v2(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete(buf);
        }
    }
    return result;
}

// Test cloudsync_merge_insert where local col_version is equal to the insert col_version,
// the greater value must win.
bool do_test_merge_4 (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 2) {
        nclients = 2;
        printf("Number of test merge increased to %d clients\n", 2);
    }
    
    // create databases and tables
    int table_mask = TEST_PRIKEYS;
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        if (do_create_tables(table_mask, db[i]) == false) {
            return false;
        }
        
        if (do_augment_tables(table_mask, db[i], table_algo_crdt_cls) == false) {
            return false;
        }
    }
    
    char buf[256];
    
    // insert, update and delete some data in all clients
    for (int i=0; i<nclients; ++i) {
        do_insert(db[i], table_mask, 1, print_result);
        snprintf(buf, sizeof(buf), "UPDATE \"" CUSTOMERS_TABLE "\" SET age=%d, note='note%d', stamp='stamp%d';", i+nclients, i+nclients, 9-i);
        rc = sqlite3_exec(db[i], buf, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // merge all changes
    if (do_merge(db, nclients, true) == false) {
        goto finalize;
    }
    
    // compare results
    for (int i=1; i<nclients; ++i) {
        const char *sql = "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY first_name, last_name;";
        if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
        const char *sql2 = "SELECT 'name1', 'surname1', 3, 'note3', 'stamp9'";
        if (do_compare_queries(db[0], sql, db[i], sql2, -1, -1, print_result) == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> " CUSTOMERS_TABLE "\n");
        do_query(db[0], "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY first_name, last_name;", query_table);
    }
    
    result = true;
    rc = SQLITE_OK;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_3 error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete(buf);
        }
    }
    return result;
}

// Test the following scenario:
// 1. insert of the same row on 2 clients
// 2. update non-pk col ("age") on the first client
// 3. merge this change from the first client to the second client
// 4. update a pk col ("first_name") on the second client. The cloud sync cloudsync_changes vtab on the second cilent now contains:
//   a. pk:<new_pk>, col_name:age,       db_version:2, col_version:2, site_id:1 (remote change)
//   b. pk:<old_pk>, col_name:TOMBSTONE, db_version:3, col_version:2, site_id:0 (local change, delete old pk)
//   c. pk:<new_pk>, col_name:TOMBSTONE, db_version:3, col_version:1, site_id:0 (local change, updated row with new pk)
// 5. merge changes from the second client to the first client:
//   - if the second client sends only local changes, then the change at 4a is not sent so the first client will show NULL
//     at column "age" for the row with the new pk
//   - if the second client sends all the changes (not only local changes), then the change at 4a is sent along with 4b and 4c
//     so the first client will show the correct value for the "age" column
bool do_test_merge_5 (int nclients, bool print_result, bool cleanup_databases, bool only_locals) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 2) {
        nclients = 2;
        printf("Number of test merge increased to %d clients\n", 2);
    }
    
    // create databases and tables
    int table_mask = TEST_PRIKEYS;
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        if (do_create_tables(table_mask, db[i]) == false) {
            return false;
        }
        
        if (do_augment_tables(table_mask, db[i], table_algo_crdt_cls) == false) {
            return false;
        }
    }
    
    char buf[256];
    
    // insert, update and delete some data in all clients
    for (int i=0; i<nclients; ++i) {
        do_insert(db[i], table_mask, 1, print_result);
    }
    
    int n = 99;
    snprintf(buf, sizeof(buf), "UPDATE \"" CUSTOMERS_TABLE "\" SET age=%d;", n);
    rc = sqlite3_exec(db[0], buf, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
        
    if (do_merge_values(db[0], db[1], true) == false) {
        return false;
    }
    
    snprintf(buf, sizeof(buf), "UPDATE \"" CUSTOMERS_TABLE "\" SET first_name='name%d';", n);
    rc = sqlite3_exec(db[1], buf, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;

    // merge all changes
    if (do_merge(db, nclients, only_locals) == false) {
        goto finalize;
    }
    
    // compare results
    for (int i=1; i<nclients; ++i) {
        const char *sql = "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY first_name, last_name;";
        if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> customers\n");
        do_query(db[0], "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY first_name, last_name;", query_table);
    }
    
    result = true;
    rc = SQLITE_OK;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_4: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete(buf);
        }
    }
    return result;
}

bool do_test_merge_alter_schema_1 (int nclients, bool print_result, bool cleanup_databases, bool only_locals) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 2) {
        nclients = 2;
        printf("Number of test merge increased to %d clients\n", 2);
    }
    
    // create databases and tables
    int table_mask = TEST_PRIKEYS | TEST_NOCOLS;
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        if (do_create_tables(table_mask, db[i]) == false) {
            return false;
        }
        
        if (do_augment_tables(TEST_PRIKEYS, db[i], table_algo_crdt_cls) == false) {
            return false;
        }
    }
    
    // augment TEST_NOCOLS only on db0 so the schema hash will differ between the two clients
    if (do_augment_tables(TEST_NOCOLS, db[0], table_algo_crdt_cls) == false) {
        return false;
    }
    
    // insert, update and delete some data in the first client
    do_insert(db[0], TEST_PRIKEYS, NINSERT, print_result);
    
    // merge changes from db0 to db1, it should fail because db0 has a newer schema hash
    if (do_merge_enc_dec_values(db[0], db[1], only_locals, false) == true) {
        return false;
    }
    
    // augment TEST_NOCOLS also on db1
    if (do_augment_tables(TEST_NOCOLS, db[1], table_algo_crdt_cls) == false) {
        return false;
    }
    
    // merge all changes, now it must work fine
    if (do_merge(db, nclients, only_locals) == false) {
        goto finalize;
    }
    
    do_update(db[1], table_mask, print_result);

    // merge all changes, now it must work fine
    if (do_merge(db, nclients, only_locals) == false) {
        goto finalize;
    }
    
    // compare results
    for (int i=1; i<nclients; ++i) {
        const char *sql = "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY first_name, last_name;";
        if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> customers\n");
        do_query(db[0], "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY first_name, last_name;", query_table);
    }
    
    result = true;
    rc = SQLITE_OK;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_alter_schema_1 error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete(buf);
        }
    }
    return result;
}

bool do_test_merge_alter_schema_2 (int nclients, bool print_result, bool cleanup_databases, bool only_locals) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 2) {
        nclients = 2;
        printf("Number of test merge increased to %d clients\n", 2);
    }
    
    // create databases and tables
    int table_mask = TEST_PRIKEYS;
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        if (do_create_tables(table_mask, db[i]) == false) {
            goto finalize;
        }
        
        if (do_augment_tables(TEST_PRIKEYS, db[i], table_algo_crdt_cls) == false) {
            goto finalize;
        }
    }
    
    // insert, update and delete some data in the first client
    do_insert(db[0], TEST_PRIKEYS, NINSERT, print_result);
    
    // alter table also on db0
    if (do_alter_tables(table_mask, db[0], 4) == false) {
        goto finalize;
    }
    
    // merge changes from db0 to db1, it should fail because db0 has a newer schema hash
    if (do_merge_enc_dec_values(db[0], db[1], only_locals, false) == true) {
        goto finalize;
    }
    
    // insert a new value on db1
    do_insert_val(db[1], TEST_PRIKEYS, 123456, print_result);
        
    // merge changes from db1 to db0, it should work if columns are not removed
    if (do_merge_enc_dec_values(db[1], db[0], only_locals, false) == false) {
        goto finalize;
    }
    
    // alter table also on db1
    if (do_alter_tables(table_mask, db[1], 4) == false) {
        goto finalize;
    }
    
    // merge all changes, now it must work fine
    if (do_merge(db, nclients, only_locals) == false) {
        goto finalize;
    }
        
    // compare results
    for (int i=1; i<nclients; ++i) {
        const char *sql = "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY first_name, last_name;";
        if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> customers\n");
        do_query(db[0], "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY first_name, last_name;", query_table);
    }
    
    result = true;
    rc = SQLITE_OK;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_alter_schema_2 error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete(buf);
        }
    }
    return result;
}

bool do_test_prikey (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 2) {
        nclients = 2;
        printf("Number of test merge increased to %d clients\n", 2);
    }
    
    // create databases and tables
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        const char *sql = "CREATE TABLE foo (a INTEGER PRIMARY KEY NOT NULL, b INTEGER);";
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        sql = "SELECT cloudsync_init('foo', 'cls', 1);";
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // write in client1
    const char *sql = "INSERT INTO foo (a,b) VALUES (1,2);";
    rc = sqlite3_exec(db[0], sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // send changes from client1 to all other clients
    for (int i=1; i<nclients; ++i) {
        if (do_merge_values(db[0], db[i], true) == false) {
            goto finalize;
        }
    }
    
    // update primary key in all clients except client1
    sql = "UPDATE foo SET a=666 WHERE a=1;";
    for (int i=1; i<nclients; ++i) {
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> foo (client1)\n");
        do_query(db[0], "SELECT * FROM foo ORDER BY a;", NULL);
    }
    
    // send local changes to client1
    for (int i=1; i<nclients; ++i) {
        if (do_merge_values(db[i], db[0], true) == false) {
            goto finalize;
        }
    }
    
    // compare results
    for (int i=1; i<nclients; ++i) {
        const char *sql = "SELECT * FROM foo ORDER BY a;";
        if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_prikey_null error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete(buf);
        }
    }
    return result;
}

bool do_test_double_init(int nclients, bool cleanup_databases) {
    if (nclients<2) {
        nclients = 2;
        printf("Number of clients for test do_test_double_init increased to %d clients\n", 2);
    }
    
    bool result = false;
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    time_t timestamp = time(NULL);
    db[0] = do_create_database_file(0, timestamp, test_counter);

    // configure cloudsync for a table on the first connection
    int table_mask = TEST_PRIKEYS;
    if (do_create_tables(table_mask, db[0]) == false) goto finalize;
    if (do_augment_tables(table_mask, db[0], table_algo_crdt_cls) == false) goto finalize;
    
    // double load
    sqlite3_cloudsync_init(db[0], NULL, NULL);
    sqlite3_cloudsync_init(db[0], NULL, NULL);
    
    // double cloudsync-init
    if (do_augment_tables(table_mask, db[0], table_algo_crdt_cls) == false) goto finalize;
    if (do_augment_tables(table_mask, db[0], table_algo_crdt_cls) == false) goto finalize;

    // double terminate
    if (sqlite3_exec(db[0], "SELECT cloudsync_terminate();", NULL, NULL, NULL) != SQLITE_OK) goto finalize;
    if (do_augment_tables(table_mask, db[0], table_algo_crdt_cls) == false) goto finalize;
    if (sqlite3_exec(db[0], "SELECT cloudsync_terminate();", NULL, NULL, NULL) != SQLITE_OK) goto finalize;

    db[1] = do_create_database_file(0, timestamp, test_counter);

    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (!result && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_init error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, test_counter);
            file_delete(buf);
        }
    }
    test_counter++;
    return result;
}

// MARK: -

bool do_test_gos (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 2) {
        nclients = 2;
        printf("Number of test merge increased to %d clients\n", 2);
    }
    
    // create databases and tables
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        const char *sql = "CREATE TABLE log (id TEXT PRIMARY KEY NOT NULL, desc TEXT, counter INTEGER, stamp TEXT DEFAULT CURRENT_TIMESTAMP);";
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        sql = "SELECT cloudsync_init('log', 'gos', 0);";
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    const char *sql = "INSERT INTO log (id, desc, counter) VALUES (?, ?, ?);";
    char buffer[UUID_STR_MAXLEN];
    char desc[256];
    int nentries = 10;
    
    // insert unique log in each database
    for (int i=0; i<nclients; ++i) {
        for (int j=0; j<nentries; ++j) {
            char *uuid = cloudsync_uuid_v7_string (buffer, true);
            snprintf(desc, sizeof(desc), "Description %d for client %d", j+1, i+1);
            
            char s[255];
            int counter = ((i+1)*100)+j;
            snprintf(s, sizeof(s), "%d", counter);
            
            const char *values[] = {uuid, desc, s};
            int types[] = {SQLITE_TEXT, SQLITE_TEXT, SQLITE_INTEGER};
            int len[] = {-1, -1, 0};
            
            rc = dbutils_write(db[i], NULL, sql, values, types, len, 3);
            if (rc != SQLITE_OK) goto finalize;
        }
    }
    
    // merge all changes
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // compare results
    for (int i=1; i<nclients; ++i) {
        const char *sql = "SELECT * FROM log ORDER BY id;";
        if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> log\n");
        do_query(db[0], "SELECT * FROM log ORDER BY id;", NULL);
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_gos error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete(buf);
        }
    }
    return result;
}

// MARK: -

bool do_test_network_encode_decode (int nclients, bool print_result, bool cleanup_databases, bool force_uncompressed) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 2) {
        nclients = 2;
        printf("Number of test merge increased to %d clients\n", 2);
    }
    
    // create databases and tables
    int table_mask = TEST_PRIKEYS | TEST_NOCOLS;
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        if (do_create_tables(table_mask, db[i]) == false) {
            return false;
        }
        
        if (do_augment_tables(table_mask, db[i], table_algo_crdt_cls) == false) {
            return false;
        }
    }
    
    // insert, update and delete some data in all clients
    for (int i=0; i<nclients; ++i) {
        do_insert(db[i], table_mask, NINSERT, print_result);
        //if (i==0) do_update(db[i], table_mask, print_result);
        //if (i!=nclients-1) do_delete(db[i], table_mask, print_result);
    }
    
    if (force_uncompressed) force_uncompressed_blob = true;
    
    // merge all changes (loop extracted from do_merge and do_merge_values)
    const char *src_sql = "SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) FROM cloudsync_changes WHERE site_id=cloudsync_siteid();";
    const char *dest_sql = "SELECT cloudsync_payload_decode(?);";
    
    for (int i=0; i<nclients; ++i) {
        int target = i;
        for (int j=0; j<nclients; ++j) {
            if (target == j) continue;
            
            int blob_size = 0, rc;
            char *blob = dbutils_blob_select (db[target], src_sql, &blob_size, NULL, &rc);
            if (!blob) goto finalize;
            
            const char *values[] = {blob};
            int types[] = {SQLITE_BLOB};
            int len[] = {blob_size};

            dbutils_select(db[j], dest_sql, values, types, len, 1, SQLITE_INTEGER);
            cloudsync_memory_free(blob);
        }
    }
    
    if (force_uncompressed) force_uncompressed_blob = false;
    
    // compare results
    for (int i=1; i<nclients; ++i) {
        const char *sql = "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY first_name, last_name;";
        if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> customers\n");
        do_query(db[0], "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY first_name, last_name;", query_table);
    }
    
    result = true;
    rc = SQLITE_OK;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete(buf);
        }
    }
    return result;
}

// MARK: -

bool do_test_fill_initial_data(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 2) {
        nclients = 2;
        printf("Number of test merge increased to %d clients\n", 2);
    }
    
    // create databases and tables
    int table_mask = TEST_PRIKEYS | TEST_NOCOLS;
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        if (do_create_tables(table_mask, db[i]) == false) {
            return false;
        }
        
        do_insert(db[i], table_mask, i, print_result);
        
        if (do_augment_tables(table_mask, db[i], table_algo_crdt_cls) == false) {
            return false;
        }
    }
    
    // merge all changes
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
        
    // compare results
    for (int i=1; i<nclients; ++i) {
        if (table_mask & TEST_PRIKEYS) {
            const char *sql = "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY first_name, last_name;";
            if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
        }
        if (table_mask & TEST_NOCOLS) {
            const char *sql = "SELECT * FROM \"" CUSTOMERS_NOCOLS_TABLE "\" ORDER BY first_name, last_name;";
            if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
        }
        if (table_mask & TEST_NOPRIKEYS) {
            const char *sql = "SELECT * FROM customers_noprikey ORDER BY first_name, last_name;";
            if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
        }
    }
    
    if (print_result) {
        if (table_mask & TEST_PRIKEYS) {
            printf("\n-> " CUSTOMERS_TABLE "\n");
            do_query(db[0], "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY first_name, last_name;", query_table);
        }
        if (table_mask & TEST_NOCOLS) {
            printf("\n-> \"" CUSTOMERS_NOCOLS_TABLE "\"\n");
            do_query(db[0], "SELECT * FROM \"" CUSTOMERS_NOCOLS_TABLE "\" ORDER BY first_name, last_name;", query_table);
        }
        if (table_mask & TEST_NOPRIKEYS) {
            printf("\n-> customers_noprikey\n");
            do_query(db[0], "SELECT * FROM customers_noprikey ORDER BY first_name, last_name;", query_table);
        }
    }
    
    result = true;
    rc = SQLITE_OK;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge error: %s\n", sqlite3_errmsg(db[i]));
        
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge error: db %d is in transaction\n", i);
            }
            
            int counter = close_db_v2(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete(buf);
        }
    }
    return result;
}

bool do_test_alter(int nclients, int alter_version, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 2) {
        nclients = 2;
        printf("Number of test merge increased to %d clients\n", 2);
    }
    
    // create databases and tables
    int table_mask = TEST_PRIKEYS | TEST_NOCOLS;
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        if (do_create_tables(table_mask, db[i]) == false) {
            return false;
        }
        
        do_insert(db[i], table_mask, i, print_result);
        
        if (do_augment_tables(table_mask, db[i], table_algo_crdt_cls) == false) {
            return false;
        }
        
        if (do_alter_tables(table_mask, db[i], alter_version) == false) {
            return false;
        }
    }
    
    // merge all changes
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
        
    // compare results
    for (int i=1; i<nclients; ++i) {
        if (table_mask & TEST_PRIKEYS) {
            const char *sql;
            switch (alter_version) {
                case 3:
                    sql = "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY name;";
                    break;
                default:
                    sql = "SELECT * FROM \"" CUSTOMERS_TABLE "\" ORDER BY first_name, last_name;";
                    break;
            }
            if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
        }
        if (table_mask & TEST_NOCOLS) {
            const char *sql;
            switch (alter_version) {
                case 3:
                    sql = "SELECT * FROM \"" CUSTOMERS_NOCOLS_TABLE "\" ORDER BY name;";
                    break;
                default:
                    sql = "SELECT * FROM \"" CUSTOMERS_NOCOLS_TABLE "\" ORDER BY first_name, last_name;";
                    break;
            }
            if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
        }
        if (table_mask & TEST_NOPRIKEYS) {
            const char *sql;
            switch (alter_version) {
                case 3:
                    sql = "SELECT * FROM customers_noprikey ORDER BY name;";
                    break;
                default:
                    sql = "SELECT * FROM customers_noprikey ORDER BY first_name, last_name;";
                    break;
            }
            if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
        }
    }
    
    if (print_result) {
        if (table_mask & TEST_PRIKEYS) {
            printf("\n-> " CUSTOMERS_TABLE "\n");
            do_query(db[0], "SELECT * FROM " CUSTOMERS_TABLE " ORDER BY first_name, last_name;", query_table);
        }
        if (table_mask & TEST_NOCOLS) {
            printf("\n-> \"" CUSTOMERS_NOCOLS_TABLE "\"\n");
            do_query(db[0], "SELECT * FROM \"" CUSTOMERS_NOCOLS_TABLE "\" ORDER BY first_name, last_name;", query_table);
        }
        if (table_mask & TEST_NOPRIKEYS) {
            printf("\n-> customers_noprikey\n");
            do_query(db[0], "SELECT * FROM customers_noprikey ORDER BY first_name, last_name;", query_table);
        }
    }
    
    result = true;
    rc = SQLITE_OK;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete(buf);
        }
    }
    return result;
}

// MARK: -

int test_report(const char *description, bool result){
    printf("%-24s %s\n", description, (result) ? "OK" : "FAILED");
    return result ? 0 : 1;
}

int main(int argc, const char * argv[]) {
    sqlite3 *db = NULL;
    int result = 0;
    bool print_result = false;
    bool cleanup_databases = true;
    
    // test in an in-memory database
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) goto finalize;
    
    // manually load extension
    sqlite3_cloudsync_init(db, NULL, NULL);
    cloudsync_set_payload_apply_callback(db, unittest_payload_apply_rls_callback);

    printf("Testing CloudSync version %s\n", CLOUDSYNC_VERSION);
    printf("===============================\n");

    result += test_report("PK Test:", do_test_pk(db, 10000, print_result));
    result += test_report("UUID Test:", do_test_uuid(db, 1000, print_result));
    result += test_report("Comparison Test:", do_test_compare(db, print_result));
    result += test_report("RowID Test:", do_test_rowid(50000, print_result));
    result += test_report("Algo Names Test:", do_test_algo_names());
    result += test_report("DBUtils Test:", do_test_dbutils());
    result += test_report("Minor Test:", do_test_others(db));
    result += test_report("Test Error Cases:", do_test_error_cases(db));

    int test_mask = TEST_INSERT | TEST_UPDATE | TEST_DELETE;
    int table_mask = TEST_PRIKEYS | TEST_NOCOLS;
    #if !CLOUDSYNC_DISABLE_ROWIDONLY_TABLES
    table_mask |= TEST_NOPRIKEYS;
    #endif

    // test local changes
    result += test_report("Local Test:", do_test_local(test_mask, table_mask, db, print_result));
    result += test_report("VTab Test: ", do_test_vtab(db));
    result += test_report("Functions Test:", do_test_functions(db, print_result));
    result += test_report("Functions Test (Int):", do_test_internal_functions());
    result += test_report("String Func Test:", do_test_string_replace_prefix());
 
    // close local database
    db = close_db(db);
 
    // simulate remote merge
    result += test_report("Merge Test:", do_test_merge(3, print_result, cleanup_databases));
    result += test_report("Merge Test 2:", do_test_merge_2(3, TEST_PRIKEYS, print_result, cleanup_databases));
    result += test_report("Merge Test 3:", do_test_merge_2(3, TEST_NOCOLS, print_result, cleanup_databases));
    result += test_report("Merge Test 4:", do_test_merge_4(2, print_result, cleanup_databases));
    result += test_report("Merge Test 5:", do_test_merge_5(2, print_result, cleanup_databases, false));
    result += test_report("Merge Alter Schema 1:", do_test_merge_alter_schema_1(2, print_result, cleanup_databases, false));
    result += test_report("Merge Alter Schema 2:", do_test_merge_alter_schema_2(2, print_result, cleanup_databases, false));
    result += test_report("PriKey NULL Test:", do_test_prikey(2, print_result, cleanup_databases));
    result += test_report("Test Double Init:", do_test_double_init(2, cleanup_databases));
    
    // test grow-only set
    result += test_report("Test GrowOnlySet:", do_test_gos(6, print_result, cleanup_databases));
    result += test_report("Test Network Enc/Dec:", do_test_network_encode_decode(2, print_result, cleanup_databases, false));
    result += test_report("Test Network Enc/Dec 2:", do_test_network_encode_decode(2, print_result, cleanup_databases, true));
    result += test_report("Test Fill Initial Data:", do_test_fill_initial_data(3, print_result, cleanup_databases));
    result += test_report("Test Alter Table 1:", do_test_alter(3, 1, print_result, cleanup_databases));
    result += test_report("Test Alter Table 2:", do_test_alter(3, 2, print_result, cleanup_databases));
    result += test_report("Test Alter Table 3:", do_test_alter(3, 3, print_result, cleanup_databases));
    
finalize:
    printf("\n");
    if (rc != SQLITE_OK) printf("%s (%d)\n", (db) ? sqlite3_errmsg(db) : "N/A", rc);
    db = close_db(db);
    
    return result;
}
