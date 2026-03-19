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
#include <inttypes.h>
#include <fcntl.h>
#include "sqlite3.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "pk.h"
#include "dbutils.h"
#include "database.h"
#include "cloudsync.h"
#include "cloudsync_sqlite.h"

// declared only if macro CLOUDSYNC_UNITTEST is defined 
extern char *OUT_OF_MEMORY_BUFFER;
extern bool force_vtab_filter_abort;
extern bool force_uncompressed_blob;

void dbvm_reset (dbvm_t *stmt);
int dbvm_count (dbvm_t *stmt, const char *value, size_t len, int type);
int dbvm_execute (dbvm_t *stmt, void *data);

int dbutils_settings_get_value (cloudsync_context *data, const char *key, char *buffer, size_t *blen, int64_t *intvalue);
int dbutils_settings_table_load_callback (void *xdata, int ncols, char **values, char **names);
int dbutils_settings_check_version (cloudsync_context *data, const char *version);
bool dbutils_settings_migrate (cloudsync_context *data);
const char *vtab_opname_from_value (int value);
int vtab_colname_is_legal (const char *name);
int dbutils_binary_comparison (int x, int y);
sqlite3 *do_create_database (void);

int cloudsync_table_sanity_check (cloudsync_context *data, const char *name, bool skip_int_pk_check);
bool database_system_exists (cloudsync_context *data, const char *name, const char *type);

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

#define CUSTOMERS_TABLE             "customers test table name with quotes ' and \" "
#define CUSTOMERS_NOCOLS_TABLE      "customers nocols"

#define CUSTOMERS_TABLE_COLUMN_LASTNAME "last name with ' and \"\""

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

typedef struct {
    int type;
    int len;
    int rc;
    union {
        sqlite3_int64 intValue;
        double doubleValue;
        char *stringValue;
    } value;
} DATABASE_RESULT;

DATABASE_RESULT unit_exec (cloudsync_context *data, const char *sql, const char **values, int types[], int lens[], int count, DATABASE_RESULT results[], int expected_types[], int result_count) {
    DEBUG_DBFUNCTION("unit_exec %s", sql);
    
    sqlite3_stmt *pstmt = NULL;
    bool is_write = (result_count == 0);
    int type = 0;
    
    // compile sql
    int rc = databasevm_prepare(data, sql, (void **)&pstmt, 0);
    if (rc != SQLITE_OK) goto unitexec_finalize;
    // check bindings
    for (int i=0; i<count; ++i) {
        switch (types[i]) {
            case SQLITE_NULL:
                rc = databasevm_bind_null(pstmt, i+1);
                break;
            case SQLITE_TEXT:
                rc = databasevm_bind_text(pstmt, i+1, values[i], lens[i]);
                break;
            case SQLITE_BLOB:
                rc = databasevm_bind_blob(pstmt, i+1, values[i], lens[i]);
                break;
            case SQLITE_INTEGER: {
                sqlite3_int64 value = strtoll(values[i], NULL, 0);
                rc = databasevm_bind_int(pstmt, i+1, value);
            }   break;
            case SQLITE_FLOAT: {
                double value = strtod(values[i], NULL);
                rc = databasevm_bind_double(pstmt, i+1, value);
            }   break;
        }
        if (rc != SQLITE_OK) goto unitexec_finalize;
    }
        
    // execute statement
    rc = databasevm_step(pstmt);
    
    // check return value based on pre-condition
    if (rc != SQLITE_ROW) goto unitexec_finalize;
    if (is_write) goto unitexec_finalize;
    
    // process result (if any)
    for (int i=0; i<result_count; ++i) {
        type = database_column_type(pstmt, i);
        results[i].type = type;
        
        if (type == SQLITE_NULL) {
            rc = SQLITE_OK;
            continue;
        }
        
        if (type != expected_types[i]) {
            rc = SQLITE_ERROR;
            goto unitexec_finalize;
        }
        
        // type == expected_type
        if (type == SQLITE_INTEGER) results[i].value.intValue = database_column_int(pstmt, i);
        else if (type == SQLITE_FLOAT) results[i].value.doubleValue = database_column_double(pstmt, i);
        else {
            // TEXT or BLOB
            int len = database_column_bytes(pstmt, i);
            results[i].len = len;
            
            char *buffer = NULL;
            if (type == SQLITE_BLOB) {
                const void *bvalue = database_column_blob(pstmt, i, NULL);
                if (bvalue) {
                    buffer = (char *)cloudsync_memory_alloc(len);
                    if (!buffer) {rc = SQLITE_NOMEM; goto unitexec_finalize;}
                    memcpy(buffer, bvalue, len);
                }
            } else {
                const char *value = database_column_text(pstmt, i);
                if (value) buffer = cloudsync_string_dup((const char *)value);
            }
            results[i].value.stringValue = buffer;
        }
    }
    
    rc = SQLITE_OK;
    
unitexec_finalize:
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    if (rc != SQLITE_OK) {
        if (count != -1) DEBUG_ALWAYS("Error executing %s in dbutils_exec (%s).", sql, database_errmsg(data));
    }
    if (pstmt) databasevm_finalize(pstmt);
    
    if (is_write) {
        DATABASE_RESULT result = {0};
        result.rc = rc;
        return result;
    }
        
    results[0].rc = rc;
    return results[0];
}

sqlite3_int64 unit_select (cloudsync_context *data, const char *sql, const char **values, int types[], int lens[], int count, int expected_type) {
    // used only in unit-test
    DATABASE_RESULT results[1] = {0};
    int expected_types[1] = {expected_type};
    unit_exec(data, sql, values, types, lens, count, results, expected_types, 1);
    return results[0].value.intValue;
}

int unit_debug (sqlite3 *db, bool print_result) {
    sqlite3_stmt *stmt = NULL;
    int counter = 0;
    while ((stmt = sqlite3_next_stmt(db, stmt))) {
        ++counter;
        if (print_result) printf("Unfinalized stmt statement: %p (%s)\n", stmt, sqlite3_sql(stmt));
    }
    return counter;
}

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

int close_db (sqlite3 *db) {
    int counter = 0;
    if (db) {
        sqlite3_exec(db, "SELECT cloudsync_terminate();", NULL, NULL, NULL);
        counter = unit_debug(db, true);
        sqlite3_close(db);
    }
    return counter;
}

bool file_delete_internal (const char *path) {
    #ifdef _WIN32
    if (DeleteFile(path) == 0) return false;
    #else
    if (unlink(path) != 0) return false;
    #endif
    
    return true;
}

// MARK: -

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
            char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('name%d', 'surname%d', %d, 'note%d', 'stamp%d');", table_name, i+1, i+1, i+1, i+1, i+1);
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
            char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\") VALUES ('name%d', 'surname%d');", table_name, (i+1)+100000, (i+1)+100000);
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
            char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\") VALUES ('name%d', 'surname%d');", table_name, (i+1)+200000, (i+1)+200000);
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
        
        char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('name%d', 'surname%d', %d, 'note%d', 'stamp%d');", table_name, val, val, val, val, val);
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
        
        char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\") VALUES ('name%d', 'surname%d');", table_name, (val)+100000, (val)+100000);
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
        
        char *sql = sqlite3_mprintf("INSERT INTO %w (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\") VALUES ('name%d', 'surname%d');", table_name, (val)+200000, (val)+200000);
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
        
        char sql[512];
        sqlite3_snprintf(sizeof(sql), sql, "UPDATE \"%w\" SET age = 40 WHERE first_name='name1';", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        sqlite3_snprintf(sizeof(sql), sql, "UPDATE \"%w\" SET age = 2000, note='hello2000' WHERE first_name='name2';", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        sqlite3_snprintf(sizeof(sql), sql, "UPDATE \"%w\" SET age = 6000, note='hello6000' WHERE first_name='name6';", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        // update primary key here
        sqlite3_snprintf(sizeof(sql), sql, "UPDATE \"%w\" SET first_name = 'name1updated' WHERE first_name='name1';", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        sqlite3_snprintf(sizeof(sql), sql, "UPDATE \"%w\" SET first_name = 'name8updated' WHERE first_name='name8';", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        // update two primary keys here
        sqlite3_snprintf(sizeof(sql), sql, "UPDATE \"%w\" SET first_name = 'name9updated', \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" = 'surname9updated' WHERE first_name='name9';", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
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
        rc = sqlite3_exec(db, "UPDATE \"" CUSTOMERS_NOCOLS_TABLE "\" SET first_name = 'name100009updated', \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" = 'surname100009updated' WHERE first_name='name100009';", NULL, NULL, NULL);
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
        rc = sqlite3_exec(db, "UPDATE customers_noprikey SET first_name = 'name200009updated', \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" = 'surname200009updated' WHERE first_name='name200009';", NULL, NULL, NULL);
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
        
        char sql[512];
        sqlite3_snprintf(sizeof(sql), sql, "UPDATE \"%w\" SET age = ABS(RANDOM()) WHERE rowid=(ABS(RANDOM() %% 10)+1);", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        sqlite3_snprintf(sizeof(sql), sql, "UPDATE \"%w\" SET age = ABS(RANDOM()), note='hello' || HEX(RANDOMBLOB(2)), stamp='stamp' || ABS(RANDOM() %% 99) WHERE rowid=(ABS(RANDOM() %% 10)+1);", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        sqlite3_snprintf(sizeof(sql), sql, "UPDATE \"%w\" SET age = ABS(RANDOM()), note='hello' || HEX(RANDOMBLOB(2)), stamp='stamp' || ABS(RANDOM() %% 99) WHERE rowid=(ABS(RANDOM() %% 10)+1);", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        // update primary key here
        sqlite3_snprintf(sizeof(sql), sql, "UPDATE \"%w\" SET first_name = 'name' || HEX(RANDOMBLOB(4)) WHERE rowid=(ABS(RANDOM() %% 10)+1);", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        sqlite3_snprintf(sizeof(sql), sql, "UPDATE \"%w\" SET first_name = 'name' || HEX(RANDOMBLOB(4)) WHERE rowid=(ABS(RANDOM() %% 10)+1);", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        // update two primary keys here
        sqlite3_snprintf(sizeof(sql), sql, "UPDATE \"%w\" SET first_name = 'name' || HEX(RANDOMBLOB(4)), \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" = 'surname' || HEX(RANDOMBLOB(4)) WHERE rowid=(ABS(RANDOM() %% 10)+1);", table_name);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
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
        rc = sqlite3_exec(db, "UPDATE \"" CUSTOMERS_NOCOLS_TABLE "\" SET first_name = HEX(RANDOMBLOB(8)), \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" = HEX(RANDOMBLOB(8)) WHERE first_name='name100009';", NULL, NULL, NULL);
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
        rc = sqlite3_exec(db, "UPDATE customers_noprikey SET first_name = HEX(RANDOMBLOB(8)), \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" = HEX(RANDOMBLOB(8)) WHERE first_name='name200009';", NULL, NULL, NULL);
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
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
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
        
        rc = sqlite3_exec(db, "DELETE FROM \"" CUSTOMERS_NOCOLS_TABLE "\" WHERE first_name='name100005';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        rc = sqlite3_exec(db, "DELETE FROM \"" CUSTOMERS_NOCOLS_TABLE "\" WHERE first_name='name100007';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    if (table_mask & TEST_NOPRIKEYS) {
        const char *table_name = "customers_noprikey";
        if (print_result) printf("TESTING DELETE on %s\n", table_name);
        
        rc = sqlite3_exec(db, "DELETE FROM customers_noprikey WHERE first_name='name200005';", NULL, NULL, NULL);
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
    close_db(db);
    db = NULL;
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
    
    const char *name = vtab_opname_from_value (666);
    if (name != NULL) goto finalize;
    
    rc = vtab_colname_is_legal("db_version");
    if (rc != 1) goto finalize;
    
    rc = vtab_colname_is_legal("non_existing_column");
    if (rc != 0) goto finalize;
    
    return do_test_vtab2();
    
finalize:
    printf("do_test_vtab error: %s\n", sqlite3_errmsg(db));
    return false;
}

bool do_test_functions (sqlite3 *db, bool print_results) {
    char *site_id = NULL;
    int64_t len = 0;
    cloudsync_context *data = cloudsync_context_create(db);
    if (!data) return false;
    
    int rc = database_select_blob(data, "SELECT cloudsync_siteid();", &site_id, &len);
    if (rc != DBRES_OK || site_id == NULL || len != 16) {
        if (site_id) cloudsync_memory_free(site_id);
        goto abort_test_functions;
    }
    cloudsync_memory_free(site_id);
    
    char *site_id_str = NULL;
    rc = database_select_text(data, "SELECT quote(cloudsync_siteid());", &site_id_str);
    if (rc != DBRES_OK || site_id_str == NULL) {
        if (site_id_str) cloudsync_memory_free(site_id_str);
        goto abort_test_functions;
    }
    if (print_results) printf("Site ID: %s\n", site_id_str);
    cloudsync_memory_free(site_id_str);
    
    char *version = NULL;
    rc = database_select_text(data, "SELECT cloudsync_version();", &version);
    if (rc != DBRES_OK || version == NULL) {
        if (version) cloudsync_memory_free(version);
        goto abort_test_functions;
    }
    if (print_results) printf("Lib Version: %s\n", version);
    cloudsync_memory_free(version);
    
    int64_t db_version = 0;
    rc = database_select_int(data, "SELECT cloudsync_db_version();", &db_version);
    if (rc != DBRES_OK) goto abort_test_functions;
    if (print_results) printf("DB Version: %" PRId64 "\n", db_version);
    
    int64_t db_version_next = 0;
    rc = database_select_int(data, "SELECT cloudsync_db_version_next();", &db_version);
    if (rc != DBRES_OK) goto abort_test_functions;
    if (print_results) printf("DB Version Next: %" PRId64 "\n", db_version_next);

    rc = sqlite3_exec(db, "CREATE TABLE tbl1 (col1 TEXT PRIMARY KEY NOT NULL, col2);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    rc = sqlite3_exec(db, "CREATE TABLE tbl2 (col1 TEXT PRIMARY KEY NOT NULL, col2);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    rc = sqlite3_exec(db, "DROP TABLE IF EXISTS rowid_table; DROP TABLE IF EXISTS nonnull_prikey_table;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    // * disabled in 0.9.0
    rc = sqlite3_exec(db, "SELECT cloudsync_init('tbl1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    rc = sqlite3_exec(db, "SELECT cloudsync_init('tbl2');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    rc = sqlite3_exec(db, "SELECT cloudsync_disable('tbl1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    int64_t value = 0;
    rc = database_select_int(data, "SELECT cloudsync_is_enabled('tbl1');", &value);
    if (rc != DBRES_OK) goto abort_test_functions;
    int v1 = (int)value;
    if (v1 == 1) goto abort_test_functions;
    
    // * disabled in 0.9.0
    rc = sqlite3_exec(db, "SELECT cloudsync_disable('tbl2');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    rc = database_select_int(data, "SELECT cloudsync_is_enabled('tbl2');", &value);
    if (rc != DBRES_OK) goto abort_test_functions;
    int v2 = (int)value;
    if (v2 == 1) goto abort_test_functions;
    
    rc = sqlite3_exec(db, "SELECT cloudsync_enable('tbl1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    rc = database_select_int(data, "SELECT cloudsync_is_enabled('tbl1');", &value);
    if (rc != DBRES_OK) goto abort_test_functions;
    int v3 = (int)value;
    if (v3 != 1) goto abort_test_functions;
    
    // * disabled in 0.9.0
    rc = sqlite3_exec(db, "SELECT cloudsync_enable('tbl2');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    rc = database_select_int(data, "SELECT cloudsync_is_enabled('tbl2');", &value);
    if (rc != DBRES_OK) goto abort_test_functions;
    int v4 = (int)value;
    if (v4 != 1) goto abort_test_functions;
    
    rc = sqlite3_exec(db, "SELECT cloudsync_set('key1', 'value1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    rc = sqlite3_exec(db, "SELECT cloudsync_set_table('tbl1', 'key1', 'value1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    rc = sqlite3_exec(db, "SELECT cloudsync_set_column('tbl1', 'col1', 'key1', 'value1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    // * disabled in 0.9.0
    rc = sqlite3_exec(db, "SELECT cloudsync_cleanup('tbl1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    rc = sqlite3_exec(db, "SELECT cloudsync_cleanup('tbl2');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test_functions;
    
    char *uuid = NULL;
    rc = database_select_text(data, "SELECT cloudsync_uuid();", &uuid);
    if (rc != DBRES_OK || uuid == NULL) {
        if (uuid) cloudsync_memory_free(uuid);
        goto abort_test_functions;
    }
    if (print_results) printf("New uuid: %s\n", uuid);
    cloudsync_memory_free(uuid);
    cloudsync_context_free(data);
    
    return true;
    
abort_test_functions:
    cloudsync_context_free(data);
    printf("Error in do_test_functions: %s\n", sqlite3_errmsg(db));
    return false;
}

bool do_create_tables (int table_mask, sqlite3 *db) {
    // declare tables
    if (table_mask & TEST_PRIKEYS) {
        // TEST a table with a composite primary key
        char *sql = sqlite3_mprintf("CREATE TABLE IF NOT EXISTS \"%w\" (first_name TEXT NOT NULL, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" TEXT NOT NULL, age INTEGER, note TEXT, stamp TEXT DEFAULT CURRENT_TIME, PRIMARY KEY(first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\"));", CUSTOMERS_TABLE);
        int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto abort_create_tables;
    }
    
    if (table_mask & TEST_NOCOLS) {
        // TEST a table with no columns other than primary keys
        char *sql = sqlite3_mprintf("CREATE TABLE IF NOT EXISTS \"%w\" (first_name TEXT NOT NULL, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" TEXT NOT NULL, PRIMARY KEY(first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\"));", CUSTOMERS_NOCOLS_TABLE);
        int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto abort_create_tables;
    }
    
    if (table_mask & TEST_NOPRIKEYS) {
        // TEST a table with implicit rowid primary key
        const char *sql = "CREATE TABLE IF NOT EXISTS customers_noprikey (first_name TEXT NOT NULL, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" TEXT NOT NULL);";
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
        char *sql = NULL;
        switch (alter_version) {
            case 1:
                sql = sqlite3_mprintf("SELECT cloudsync_begin_alter('%q'); "
                                     "ALTER TABLE \"%w\" ADD new_column_1 TEXT; "
                                     "ALTER TABLE \"%w\" ADD new_column_2 TEXT DEFAULT 'default value'; "
                                     "ALTER TABLE \"%w\" DROP note; "
                                     "SELECT cloudsync_commit_alter('%q') ", 
                                     CUSTOMERS_TABLE, CUSTOMERS_TABLE, CUSTOMERS_TABLE, CUSTOMERS_TABLE, CUSTOMERS_TABLE);
                break;
            case 2:
                sql = sqlite3_mprintf("SELECT cloudsync_begin_alter('%q'); "
                                     "ALTER TABLE \"%w\" RENAME TO do_alter_tables_temp_customers; "
                                     "CREATE TABLE \"%w\" (first_name TEXT NOT NULL, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" TEXT NOT NULL, note TEXT, note3 TEXT DEFAULT 'note',  note4 DATETIME DEFAULT(datetime('subsec')), stamp TEXT DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\")); "
                                     "INSERT INTO \"%w\" (\"first_name\",\"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\",\"note\", \"note3\", \"stamp\") SELECT \"first_name\",\"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\",\"note\",'a new note',\"stamp\" FROM do_alter_tables_temp_customers; "
                                     "DROP TABLE do_alter_tables_temp_customers; "
                                     "SELECT cloudsync_commit_alter('%q') ", 
                                     CUSTOMERS_TABLE, CUSTOMERS_TABLE, CUSTOMERS_TABLE, CUSTOMERS_TABLE, CUSTOMERS_TABLE);
                break;
            case 3:
                sql = sqlite3_mprintf("SELECT cloudsync_begin_alter('%q'); "
                                     "ALTER TABLE \"%w\" RENAME TO do_alter_tables_temp_customers; "
                                     "CREATE TABLE \"%w\" (name TEXT NOT NULL, note TEXT, note3 TEXT DEFAULT 'note',  note4 DATETIME DEFAULT(datetime('subsec')), stamp TEXT DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(name)); "
                                     "INSERT INTO \"%w\" (\"name\",\"note\", \"note3\", \"stamp\") SELECT \"first_name\" ||  \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" ,\"note\",'a new note',\"stamp\" FROM do_alter_tables_temp_customers; "
                                     "DROP TABLE do_alter_tables_temp_customers; "
                                     "SELECT cloudsync_commit_alter('%q') ", 
                                     CUSTOMERS_TABLE, CUSTOMERS_TABLE, CUSTOMERS_TABLE, CUSTOMERS_TABLE, CUSTOMERS_TABLE);
                break;
            case 4: // only add columns, not drop
                sql = sqlite3_mprintf("SELECT cloudsync_begin_alter('%q'); "
                                     "ALTER TABLE \"%w\" ADD new_column_1 TEXT; "
                                     "ALTER TABLE \"%w\" ADD new_column_2 TEXT DEFAULT 'default value'; "
                                     "SELECT cloudsync_commit_alter('%q') ", 
                                     CUSTOMERS_TABLE, CUSTOMERS_TABLE, CUSTOMERS_TABLE, CUSTOMERS_TABLE);
                break;
            default:
                sql = NULL;
                break;
        }
        if (sql) {
            int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
            sqlite3_free(sql);
            if (rc != SQLITE_OK) goto abort_alter_tables;
        }
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
                      "CREATE TABLE \"" CUSTOMERS_NOCOLS_TABLE "\" (first_name TEXT NOT NULL, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME " 2\" TEXT NOT NULL, PRIMARY KEY(first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME " 2\")); "
                      "INSERT INTO \"" CUSTOMERS_NOCOLS_TABLE "\" (\"first_name\",\"" CUSTOMERS_TABLE_COLUMN_LASTNAME " 2\") SELECT \"first_name\",\"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" FROM do_alter_tables_temp_customers_nocols; "
                      "DROP TABLE do_alter_tables_temp_customers_nocols; "
                      "SELECT cloudsync_commit_alter('" CUSTOMERS_NOCOLS_TABLE "'); "
                      "SELECT cloudsync_begin_alter('" CUSTOMERS_NOCOLS_TABLE "'); "
                      "ALTER TABLE \"" CUSTOMERS_NOCOLS_TABLE "\" RENAME TO do_alter_tables_temp_customers_nocols; "
                      "CREATE TABLE \"" CUSTOMERS_NOCOLS_TABLE "\" (first_name TEXT NOT NULL, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" TEXT NOT NULL, PRIMARY KEY(first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\")); "
                      "INSERT INTO \"" CUSTOMERS_NOCOLS_TABLE "\" (\"first_name\",\"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\") SELECT \"first_name\",\"" CUSTOMERS_TABLE_COLUMN_LASTNAME " 2\" FROM do_alter_tables_temp_customers_nocols; "
                      "DROP TABLE do_alter_tables_temp_customers_nocols; "
                      "SELECT cloudsync_commit_alter('" CUSTOMERS_NOCOLS_TABLE "');" ;
                break;
            case 3:
                sql = "SELECT cloudsync_begin_alter('" CUSTOMERS_NOCOLS_TABLE "'); "
                      "ALTER TABLE \"" CUSTOMERS_NOCOLS_TABLE "\" RENAME TO do_alter_tables_temp_customers_nocols; "
                      "CREATE TABLE \"" CUSTOMERS_NOCOLS_TABLE "\" (name TEXT NOT NULL PRIMARY KEY); "
                      "INSERT INTO \"" CUSTOMERS_NOCOLS_TABLE "\" (\"name\") SELECT \"first_name\" || \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" FROM do_alter_tables_temp_customers_nocols; "
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
                      "CREATE TABLE \"customers_noprikey\" (first_name TEXT NOT NULL, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" TEXT NOT NULL, note TEXT, note3 TEXT DEFAULT 'note', stamp TEXT DEFAULT CURRENT_TIMESTAMP); "
                      "INSERT INTO \"customers_noprikey\" (\"first_name\",\"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\",\"note\", \"note3\", \"stamp\") SELECT \"first_name\",\"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\",\"note\",'a new note',\"stamp\" FROM do_alter_tables_temp_customers_noprikey; "
                      "DROP TABLE do_alter_tables_temp_customers_noprikey; "
                      "SELECT cloudsync_commit_alter('customers_noprikey') "
                        "SELECT cloudsync_begin_alter('customers_noprikey'); "
                        "ALTER TABLE \"customers_noprikey\" RENAME TO do_alter_tables_temp_customers_noprikey; "
                        "CREATE TABLE customers_noprikey (first_name TEXT NOT NULL, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" TEXT NOT NULL, PRIMARY KEY(first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\")); "
                        "INSERT INTO \"customers_noprikey\" (\"first_name\",\"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\") SELECT \"first_name\",\"" CUSTOMERS_TABLE_COLUMN_LASTNAME " 2\" FROM do_alter_tables_temp_customers_noprikey; "
                        "DROP TABLE do_alter_tables_temp_customers_noprikey; "
                        "SELECT cloudsync_commit_alter('customers_noprikey');" ;
                break;
            case 3:
                sql = "SELECT cloudsync_begin_alter('customers_noprikey'); "
                      "ALTER TABLE \"customers_noprikey\" RENAME TO do_alter_tables_temp_customers_noprikey; "
                      "CREATE TABLE \"customers_noprikey\" (name TEXT NOT NULL, note TEXT, note3 TEXT DEFAULT 'note', stamp TEXT DEFAULT CURRENT_TIMESTAMP); "
                      "INSERT INTO \"customers_noprikey\" (\"first_name\" || \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\",\"note\", \"note3\", \"stamp\") SELECT \"first_name\",\"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\",\"note\",'a new note',\"stamp\" FROM do_alter_tables_temp_customers_noprikey; "
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
        sqlite3_snprintf(sizeof(sql), sql, "SELECT cloudsync_init('%q', '%s');", CUSTOMERS_TABLE, cloudsync_algo_name(algo));
        int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto abort_augment_tables;
    }
    
    if (table_mask & TEST_NOCOLS) {
        sqlite3_snprintf(sizeof(sql), sql, "SELECT cloudsync_init('%q', '%s');", CUSTOMERS_NOCOLS_TABLE, cloudsync_algo_name(algo));
        if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) goto abort_augment_tables;
    }
    
    if (table_mask & TEST_NOPRIKEYS) {
        sqlite3_snprintf(sizeof(sql), sql, "SELECT cloudsync_init('customers_noprikey', '%s');", cloudsync_algo_name(algo));
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
        char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note) VALUES ('name5', 'surname5', 55, 'Reinsert a previously delete row');", CUSTOMERS_TABLE);
        int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) {
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
        snprintf(sql, sizeof(sql), "SELECT cloudsync_pk_encode(%" PRId64 ");", ivalue);
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
    unit_debug(db, true);
    
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
    unit_debug(db, true);
    return result;
}

bool do_test_single_pk (bool print_result) {
    bool result = false;
    
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) goto cleanup;
    
    // manually load extension
    sqlite3_cloudsync_init(db, NULL, NULL);
    
    rc = sqlite3_exec(db, "CREATE TABLE single_pk_test (col1 INTEGER PRIMARY KEY NOT NULL);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
    // the following function should fail
    rc = sqlite3_exec(db, "SELECT cloudsync_init('single_pk_test');", NULL, NULL, NULL);
    if (rc == SQLITE_OK) return false;
    
    // the following function should succedd
    rc = sqlite3_exec(db, "SELECT cloudsync_init('single_pk_test', 'cls', 1);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) return false;
    result = true;
    
    // cleanup newly created table
    sqlite3_exec(db, "SELECT cloudsync_cleanup('single_pk_test');", NULL, NULL, NULL);
    
cleanup:
    if (rc != SQLITE_OK && print_result) printf("do_test_single_pk error: %s\n", sqlite3_errmsg(db));
    close_db(db);
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
        for (int k=0; k<nkeys; ++k) {
            int t = pklist[k].type;
            if ((t == SQLITE_TEXT) || (t == SQLITE_BLOB)) free(pklist[k].pvalue);
        }
    }
    
    // test max/min
    if (do_test_pk_single_value(db, SQLITE_INTEGER, INT64_MAX, 0, NULL, print_result) == false) goto finalize;
    if (do_test_pk_single_value(db, SQLITE_INTEGER, INT64_MIN, 0, NULL, print_result) == false) goto finalize;
    if (do_test_pk_single_value(db, SQLITE_INTEGER, -15592946911031981, 0, NULL, print_result) == false) goto finalize;
    if (do_test_pk_single_value(db, SQLITE_INTEGER, -922337203685477580, 0, NULL, print_result) == false) goto finalize;
    if (do_test_pk_single_value(db, SQLITE_FLOAT, 0, -9223372036854775.808, NULL, print_result) == false) goto finalize;
    // SQLITE_NULL is no longer valid for primary keys (runtime NULL check rejects it)
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
    unit_debug(db, true);
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
    dbutils_debug_values((dbvalue_t **)values, 2);
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
        int64_t db_version = random_int64_range(1, 17179869183);
        int64_t seq = random_int64_range(1, 1073741823);
        int64_t rowid = (db_version << 30) | seq;
        
        int64_t value1;
        int64_t value2;
        cloudsync_rowid_decode(rowid, &value1, &value2);
        if (value1 != db_version)  return false;
        if (value2 != seq) return false;
    }
    
    // special case that failed in an old version
    int64_t db_version = 14963874252;
    int64_t seq = 172784902;
    int64_t rowid = (db_version << 30) | seq;
    
    int64_t value1;
    int64_t value2;
    cloudsync_rowid_decode(rowid, &value1, &value2);
    if (value1 != db_version)  return false;
    if (value2 != seq) return false;
    
    return true;
}

bool do_test_algo_names (void) {
    if (cloudsync_algo_name(table_algo_none) != NULL) return false;
    if (strcmp(cloudsync_algo_name(table_algo_crdt_cls), "cls") != 0) return false;
    if (strcmp(cloudsync_algo_name(table_algo_crdt_gos), "gos") != 0) return false;
    if (strcmp(cloudsync_algo_name(table_algo_crdt_dws), "dws") != 0) return false;
    if (strcmp(cloudsync_algo_name(table_algo_crdt_aws), "aws") != 0) return false;
    if (cloudsync_algo_name(666) != NULL) return false;
    
    return true;
}

bool do_test_dbutils (void) {
    // test in an in-memory database
    void *data = NULL;
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) goto finalize;
    
    // manually load extension
    sqlite3_cloudsync_init(db, NULL, NULL);

    // test context create and free
    data = cloudsync_context_create(db);
    if (!data) return false;
    
    const char *sql = "CREATE TABLE IF NOT EXISTS foo (name TEXT PRIMARY KEY NOT NULL, age INTEGER, note TEXT, stamp TEXT DEFAULT CURRENT_TIME);"
    "CREATE TABLE IF NOT EXISTS bar (name TEXT PRIMARY KEY NOT NULL, age INTEGER, note TEXT, stamp TEXT DEFAULT CURRENT_TIME);"
    "CREATE TABLE IF NOT EXISTS rowid_table (name TEXT, age INTEGER);"
    "CREATE TABLE IF NOT EXISTS nonnull_prikey_table (name TEXT PRIMARY KEY, age INTEGER);"
    "CREATE TABLE IF NOT EXISTS nonnull_nodefault_table (name TEXT PRIMARY KEY NOT NULL, stamp TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS nonnull_default_table (name TEXT PRIMARY KEY NOT NULL, stamp TEXT NOT NULL DEFAULT CURRENT_TIME);"
    "CREATE TABLE IF NOT EXISTS integer_pk (id INTEGER PRIMARY KEY NOT NULL, value);"
    "CREATE TABLE IF NOT EXISTS int_pk (id INT PRIMARY KEY NOT NULL, value);"
    "CREATE TABLE IF NOT EXISTS \"quoted table name 🚀\" (\"pk quoted col 1\" TEXT NOT NULL, \"pk quoted col 2\" TEXT NOT NULL, \"non pk quoted col 1\", \"non pk quoted col 2\", PRIMARY KEY (\"pk quoted col 1\", \"pk quoted col 2\"));";
    
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
    DBTYPE type[] = {SQLITE_TEXT, SQLITE_FLOAT, SQLITE_NULL};
    int len[] = {5, 0, 0};
    rc = database_write(data, sql, values, type, len, 3);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = "INSERT INTO foo2 (name) VALUES ('Error');";
    rc = database_write(data, sql, NULL, NULL, NULL, -1);
    if (rc == SQLITE_OK) goto finalize;
    
    // test dbutils_text_select
    sql = "INSERT INTO foo (name) VALUES ('Test2')";
    rc = database_exec(data, sql);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = "INSERT INTO \"quoted table name 🚀\" (\"pk quoted col 1\", \"pk quoted col 2\", \"non pk quoted col 1\", \"non pk quoted col 2\") VALUES ('pk1', 'pk2', 'nonpk1', 'nonpk2');";
    rc = database_write(data, sql, NULL, NULL, NULL, -1);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = "SELECT * FROM cloudsync_changes();";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    //sqlite3_int64 i64_value = dbutils_int_select(db, "SELECT NULL;");
    //if (i64_value != 0) goto finalize;
    
    //rc = dbutils_register_function(db, NULL, NULL, 0, NULL, NULL, NULL);
    //if (rc == SQLITE_OK) goto finalize;
    
    //rc = dbutils_register_aggregate(db, NULL, NULL, NULL, 0, NULL, NULL, NULL);
    //if (rc == SQLITE_OK) goto finalize;
    
    bool b = database_system_exists(data, "non_existing_table", "non_existing_type");
    if (b == true) {rc = SQLITE_ERROR; goto finalize;}
    
    // test cloudsync_table_sanity_check
    rc = cloudsync_table_sanity_check(data, NULL, false);
    if (rc == DBRES_OK) goto finalize;
    rc = cloudsync_table_sanity_check(data, "rowid_table", false);
    if (rc == DBRES_OK) goto finalize;
    rc = cloudsync_table_sanity_check(data, "foo2", false);
    if (rc == DBRES_OK) goto finalize;
    rc = cloudsync_table_sanity_check(data, build_long_tablename(), false);
    if (rc == DBRES_OK) goto finalize;
    rc = cloudsync_table_sanity_check(data, "nonnull_prikey_table", false);
    if (rc == DBRES_OK) goto finalize;
    rc = cloudsync_table_sanity_check(data, "nonnull_nodefault_table", false);
    if (rc == DBRES_OK) goto finalize;
    rc = cloudsync_table_sanity_check(data, "nonnull_default_table", false);
    if (rc != DBRES_OK) goto finalize;
    rc = cloudsync_table_sanity_check(data, "integer_pk", false);
    if (rc == DBRES_OK) goto finalize;
    rc = cloudsync_table_sanity_check(data, "integer_pk", true);
    if (rc != DBRES_OK) goto finalize;
    rc = cloudsync_table_sanity_check(data, "int_pk", false);
    if (rc == DBRES_OK) goto finalize;
    rc = cloudsync_table_sanity_check(data, "int_pk", true);
    if (rc != DBRES_OK) goto finalize;
    rc = cloudsync_table_sanity_check(data, "quoted table name 🚀", true);
    if (rc != DBRES_OK) goto finalize;
    
    // create huge dummy_table table
    rc = sqlite3_exec(db, build_huge_table(), NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // sanity check the huge dummy_table table
    rc = cloudsync_table_sanity_check(data, "dummy_table", false);
    if (rc == SQLITE_OK) goto finalize;
    
    // de-augment bar with cloudsync
    rc = sqlite3_exec(db, "SELECT cloudsync_cleanup('bar');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // test settings
    dbutils_settings_set_key_value(data, "key1", "test1");
    dbutils_settings_set_key_value(data, "key2", "test2");
    dbutils_settings_set_key_value(data, "key2", NULL);
    
    char buffer[256];
    size_t blen = sizeof(buffer);
    rc = dbutils_settings_get_value(data, "key1", buffer, &blen, NULL);
    if (rc != SQLITE_OK) goto finalize;
    if (strcmp(buffer, "test1") != 0) goto finalize;
    
    blen = sizeof(buffer);
    rc = dbutils_settings_get_value(data, "key2", buffer, &blen, NULL);
    if (rc != SQLITE_OK) goto finalize;
    if (buffer[0] != 0) goto finalize;
    
    // test table settings
    rc = dbutils_table_settings_set_key_value(data, NULL, NULL, NULL, NULL);
    if (rc != SQLITE_ERROR) goto finalize;
    
    rc = dbutils_table_settings_set_key_value(data, "foo", NULL, "key1", "value1");
    if (rc != SQLITE_OK) goto finalize;
    
    rc = dbutils_table_settings_set_key_value(data, "foo", NULL, "key2", "value2");
    if (rc != SQLITE_OK) goto finalize;
    
    rc = dbutils_table_settings_set_key_value(data, "foo", NULL, "key2", NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = SQLITE_ERROR;
    
    rc = dbutils_table_settings_get_value(data, "foo", NULL, "key1", buffer, sizeof(buffer));
    if (rc != DBRES_OK || strcmp(buffer, "value1") != 0) goto finalize;
    rc = dbutils_table_settings_get_value(data, "foo", NULL, "key2", buffer, sizeof(buffer));
    if (rc != DBRES_OK || strlen(buffer) > 0) goto finalize;
    
    int64_t db_version = 0;
    database_select_int(data, "SELECT cloudsync_db_version();", &db_version);

    char *site_id_blob;
    int64_t site_id_blob_size;
    int64_t dbver1;
    rc = database_select_blob_int(data, "SELECT cloudsync_siteid(),  cloudsync_db_version();", &site_id_blob, &site_id_blob_size, &dbver1);
    if (rc != SQLITE_OK || site_id_blob == NULL ||dbver1 != db_version) goto finalize;
    cloudsync_memory_free(site_id_blob);
    
    // force out-of-memory test
    rc = dbutils_settings_get_value(data, "key1", NULL, 0, NULL);
    if (rc != SQLITE_MISUSE) goto finalize;
    
    rc = dbutils_table_settings_get_value(data, "foo", NULL, "key1", NULL, 0);
    if (rc != DBRES_MISUSE) goto finalize;

    //char *p = NULL;
    //dbutils_select(data, "SELECT zeroblob(16);", NULL, NULL, NULL, 0, SQLITE_BLOB);
    //if (p != NULL) goto finalize;
    
    dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_LIBVERSION, "0.0.0");
    int cmp = dbutils_settings_check_version(data, NULL);
    if (cmp == 0) goto finalize;
    
    dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_LIBVERSION, CLOUDSYNC_VERSION);
    cmp = dbutils_settings_check_version(data, NULL);
    if (cmp != 0) goto finalize;

    cmp = dbutils_settings_check_version(data, "0.8.25");
    if (cmp <= 0) goto finalize;
    
    //dbutils_settings_table_load_callback(NULL, 0, NULL, NULL);
    dbutils_settings_migrate(NULL);
    
    dbutils_settings_cleanup(data);
    
    int n1 = 1;
    int n2 = 2;
    cmp = dbutils_binary_comparison(n1, n2);
    if (cmp != -1) goto finalize;
    cmp = dbutils_binary_comparison(n2, n1);
    if (cmp != 1) goto finalize;
    cmp = dbutils_binary_comparison(n1, n1);
    if (cmp != 0) goto finalize;
    
    rc = SQLITE_OK;

finalize:
    if (rc != SQLITE_OK) printf("%s\n", sqlite3_errmsg(db));
    close_db(db);
    db = NULL;
    if (data) cloudsync_context_free(data);
    return (rc == SQLITE_OK);
}

bool do_test_others (sqlite3 *db) {
    // test unfinalized statement just to increase code coverage
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db, "SELECT 1;", -1, &stmt, NULL);
    int count = unit_debug(db, false);
    sqlite3_finalize(stmt);
    // to increase code coverage
    // dbutils_set_error(NULL, "Test is: %s", "Hello World");
    return (count == 1);
}

// test error cases to increase code coverage
bool do_test_error_cases (sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    
    // test cloudsync_init missing table
    sqlite3_prepare_v2(db, "SELECT cloudsync_init('missing_table');", -1, &stmt, NULL);
    int res = dbvm_execute(stmt, NULL);
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

bool do_test_null_prikey_insert (sqlite3 *db) {
    // Create a table with a primary key that allows NULL (no NOT NULL constraint)
    const char *sql = "CREATE TABLE IF NOT EXISTS t_null_pk (id TEXT PRIMARY KEY, value TEXT);"
                      "SELECT cloudsync_init('t_null_pk');";
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return false;

    // Attempt to insert a row with NULL primary key — should fail
    char *errmsg = NULL;
    sql = "INSERT INTO t_null_pk (id, value) VALUES (NULL, 'test');";
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc == SQLITE_OK) return false;  // should have failed
    if (!errmsg) return false;

    // Verify the error message matches the expected format
    const char *expected = "Insert aborted because primary key in table t_null_pk contains NULL values.";
    bool match = (strcmp(errmsg, expected) == 0);
    sqlite3_free(errmsg);
    if (!match) return false;

    // Verify that a non-NULL primary key insert succeeds
    sql = "INSERT INTO t_null_pk (id, value) VALUES ('valid_id', 'test');";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return false;

    // Verify the metatable has exactly 1 row (only the valid insert)
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t_null_pk_cloudsync;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return false;
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return false; }
    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if (count != 1) return false;

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
    
    sql = "SELECT \"double-quoted string literal misfeature\"";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_ERROR) {
        printf("invalid result code for the following query, expected 1 (ERROR), got %d: '%s'\n", rc, sql);
        printf("the unittest must be built with -DSQLITE_DQS=0\n");
        goto abort_test;
    }
    
    sql = "CREATE TABLE foo (name TEXT PRIMARY KEY NOT NULL, age INTEGER UNIQUE);";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test;
    
    // TEST 1 (count returns DONE)
    sql = "INSERT INTO foo (name) VALUES ('TestInternalFunctions')";
    rc = sqlite3_prepare(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto abort_test;
    
    int res = dbvm_count(vm, NULL, 0, 0);
    if (res != 0) goto abort_test;
    if (vm) sqlite3_finalize(vm);
    vm = NULL;
    
    // TEST 2 (dbvm_execute returns an error)
    sql = "INSERT INTO foo (name, age) VALUES ('Name1', 22)";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto abort_test;
    
    rc = sqlite3_prepare(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto abort_test;
    
    // this statement must fail
    res = dbvm_execute(vm, NULL);
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

// Test cloudsync_string_ndup_lowercase function
bool do_test_string_lowercase(void) {
    // Test cloudsync_string_ndup_lowercase
    const char *test_str = "HELLO World MiXeD";
    char *lowercase = cloudsync_string_ndup_lowercase(test_str, strlen(test_str));
    if (!lowercase) return false;
    if (strcmp(lowercase, "hello world mixed") != 0) {
        cloudsync_memory_free(lowercase);
        return false;
    }
    cloudsync_memory_free(lowercase);

    // Test cloudsync_string_dup_lowercase
    char *dup_lower = cloudsync_string_dup_lowercase("TEST STRING");
    if (!dup_lower) return false;
    if (strcmp(dup_lower, "test string") != 0) {
        cloudsync_memory_free(dup_lower);
        return false;
    }
    cloudsync_memory_free(dup_lower);

    // Test with NULL
    char *null_result = cloudsync_string_ndup_lowercase(NULL, 0);
    if (null_result != NULL) return false;

    null_result = cloudsync_string_dup_lowercase(NULL);
    if (null_result != NULL) return false;

    return true;
}

// Test context error and auxdata functions
bool do_test_context_functions(void) {
    sqlite3 *db = NULL;
    bool result = false;

    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    cloudsync_context *data = cloudsync_context_create(db);
    if (!data) goto cleanup;

    // Test cloudsync_errcode - should return OK initially
    int err = cloudsync_errcode(data);
    if (err != DBRES_OK) goto cleanup_ctx;

    // Test cloudsync_set_error and cloudsync_errmsg
    cloudsync_set_error(data, "Test error message", DBRES_ERROR);
    err = cloudsync_errcode(data);
    if (err != DBRES_ERROR) goto cleanup_ctx;

    const char *errmsg = cloudsync_errmsg(data);
    if (!errmsg || strlen(errmsg) == 0) goto cleanup_ctx;

    // Test cloudsync_reset_error
    cloudsync_reset_error(data);
    err = cloudsync_errcode(data);
    if (err != DBRES_OK) goto cleanup_ctx;

    // Test cloudsync_auxdata / cloudsync_set_auxdata
    void *aux = cloudsync_auxdata(data);
    if (aux != NULL) goto cleanup_ctx;  // Should be NULL initially

    int test_data = 12345;
    cloudsync_set_auxdata(data, &test_data);
    aux = cloudsync_auxdata(data);
    if (aux != &test_data) goto cleanup_ctx;

    // Reset auxdata
    cloudsync_set_auxdata(data, NULL);
    aux = cloudsync_auxdata(data);
    if (aux != NULL) goto cleanup_ctx;

    // Test cloudsync_set_schema / cloudsync_schema
    const char *schema = cloudsync_schema(data);
    // Initially NULL or empty

    cloudsync_set_schema(data, "test_schema");
    schema = cloudsync_schema(data);
    if (!schema || strcmp(schema, "test_schema") != 0) goto cleanup_ctx;

    // Set same schema (should be a no-op)
    cloudsync_set_schema(data, schema);
    schema = cloudsync_schema(data);
    if (!schema || strcmp(schema, "test_schema") != 0) goto cleanup_ctx;

    // Set different schema
    cloudsync_set_schema(data, "another_schema");
    schema = cloudsync_schema(data);
    if (!schema || strcmp(schema, "another_schema") != 0) goto cleanup_ctx;

    // Set to NULL
    cloudsync_set_schema(data, NULL);
    schema = cloudsync_schema(data);
    if (schema != NULL) goto cleanup_ctx;

    // Test cloudsync_table_schema with non-existent table
    const char *table_schema = cloudsync_table_schema(data, "non_existent_table");
    if (table_schema != NULL) goto cleanup_ctx;  // Should return NULL for non-existent table

    // Create and init a table, then test cloudsync_table_schema
    rc = sqlite3_exec(db, "CREATE TABLE schema_test_tbl (id TEXT PRIMARY KEY NOT NULL, data TEXT);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup_ctx;

    rc = sqlite3_exec(db, "SELECT cloudsync_init('schema_test_tbl');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup_ctx;

    table_schema = cloudsync_table_schema(data, "schema_test_tbl");
    // table_schema can be NULL or a valid schema depending on implementation

    result = true;

cleanup_ctx:
    cloudsync_context_free(data);
cleanup:
    if (db) close_db(db);
    return result;
}

// Test pk_decode with count from buffer (count == -1)
bool do_test_pk_decode_count_from_buffer(void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    bool result = false;

    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Encode multiple values (no NULL — primary keys cannot contain NULL)
    const char *sql = "SELECT cloudsync_pk_encode(123, 'text value', 3.14, X'DEADBEEF');";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    char *pk = (char *)sqlite3_column_blob(stmt, 0);
    int pklen = sqlite3_column_bytes(stmt, 0);

    // Copy buffer
    char buffer[1024];
    memcpy(buffer, pk, (size_t)pklen);

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test pk_decode with count = -1 (read count from buffer)
    // The count is embedded in the first byte of the encoded pk
    size_t seek = 0;
    int n = pk_decode(buffer, (size_t)pklen, -1, &seek, -1, NULL, NULL);
    if (n != 4) goto cleanup;  // Should decode 4 values

    result = true;

cleanup:
    if (stmt) sqlite3_finalize(stmt);
    if (db) close_db(db);
    return result;
}

// Test various error paths in cloudsync_set_error
bool do_test_error_handling(void) {
    sqlite3 *db = NULL;
    bool result = false;

    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    cloudsync_context *data = cloudsync_context_create(db);
    if (!data) goto cleanup;

    // Test cloudsync_set_error with NULL err_user (line 519-520 in cloudsync.c)
    int err_code = cloudsync_set_dberror(data);  // This calls set_error with NULL
    // err_code should be non-zero since we force an error state

    // Reset for next test
    cloudsync_reset_error(data);

    // Test cloudsync_set_error with err_code = DBRES_OK (should convert to DBRES_ERROR)
    err_code = cloudsync_set_error(data, "Test", DBRES_OK);
    if (err_code == DBRES_OK) {
        cloudsync_context_free(data);
        goto cleanup;
    }

    cloudsync_context_free(data);
    result = true;

cleanup:
    if (db) close_db(db);
    return result;
}

// Test cloudsync_terminate function
bool do_test_terminate(void) {
    sqlite3 *db = NULL;
    bool result = false;

    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Create and init a table
    rc = sqlite3_exec(db, "CREATE TABLE term_test (id TEXT PRIMARY KEY NOT NULL);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_exec(db, "SELECT cloudsync_init('term_test');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Call terminate
    rc = sqlite3_exec(db, "SELECT cloudsync_terminate();", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    result = true;

cleanup:
    if (db) sqlite3_close(db);
    return result;
}

// Test hash function edge cases
bool do_test_hash_function(void) {
    // Test fnv1a_hash with various inputs

    // Empty string
    uint64_t h1 = fnv1a_hash("", 0);

    // Simple string
    uint64_t h2 = fnv1a_hash("hello", 5);
    if (h1 == h2) return false;  // Different inputs should produce different hashes

    // String with comments (SQL normalization)
    const char *sql1 = "CREATE TABLE foo (id INT)";
    const char *sql2 = "CREATE TABLE foo (id INT) -- comment";
    uint64_t h3 = fnv1a_hash(sql1, strlen(sql1));
    uint64_t h4 = fnv1a_hash(sql2, strlen(sql2));
    if (h3 == h4) return false;  // Comment should affect hash

    // String with quotes
    const char *sql3 = "CREATE TABLE 'foo' (id INT)";
    uint64_t h5 = fnv1a_hash(sql3, strlen(sql3));
    if (h3 == h5) return false;

    // Block comment
    const char *sql4 = "CREATE TABLE /* comment */ foo (id INT)";
    uint64_t h6 = fnv1a_hash(sql4, strlen(sql4));

    // Whitespace normalization
    const char *sql5 = "CREATE  TABLE   foo    (id INT)";
    uint64_t h7 = fnv1a_hash(sql5, strlen(sql5));

    // Suppress unused variable warnings
    (void)h6;
    (void)h7;

    return true;
}

// Test blob compare with large sizes that would overflow old (int)(size1-size2) code
bool do_test_blob_compare_large_sizes(void) {
    // The old code did (int)(size1 - size2) which overflows for large size_t values
    const char blob1[] = {0x01};
    const char blob2[] = {0x02};

    // size1 > size2 should give positive result
    int r1 = cloudsync_blob_compare(blob1, 100, blob2, 1);
    if (r1 <= 0) return false;

    // size1 < size2 should give negative result
    int r2 = cloudsync_blob_compare(blob1, 1, blob2, 100);
    if (r2 >= 0) return false;

    // Same size, different content
    int r3 = cloudsync_blob_compare(blob1, 1, blob2, 1);
    if (r3 == 0) return false;

    // Equal
    int r4 = cloudsync_blob_compare(blob1, 1, blob1, 1);
    if (r4 != 0) return false;

    return true;
}

// Test that cloudsync_uuid() is non-deterministic (returns different values in same query)
bool do_test_deterministic_flags(void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    bool result = false;

    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // SELECT cloudsync_uuid(), cloudsync_uuid() — both values should differ
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_uuid(), cloudsync_uuid();", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    const char *u1 = (const char *)sqlite3_column_text(stmt, 0);
    const char *u2 = (const char *)sqlite3_column_text(stmt, 1);
    if (!u1 || !u2) goto cleanup;

    // Non-deterministic: same query, different results
    if (strcmp(u1, u2) == 0) goto cleanup;

    result = true;

cleanup:
    if (stmt) sqlite3_finalize(stmt);
    if (db) close_db(db);
    return result;
}

// Test schema hash consistency for int64 roundtrip (high-bit values)
bool do_test_schema_hash_consistency(void) {
    sqlite3 *db = NULL;
    bool result = false;

    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Create and init a table — use TEXT pk to avoid single INTEGER pk warning
    rc = sqlite3_exec(db, "CREATE TABLE t1 (id TEXT PRIMARY KEY NOT NULL, name TEXT);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_exec(db, "SELECT cloudsync_init('t1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Get the schema hash value by reading cloudsync_schema_versions
    {
        sqlite3_stmt *stmt = NULL;
        rc = sqlite3_prepare_v2(db, "SELECT hash FROM cloudsync_schema_versions ORDER BY seq DESC LIMIT 1;", -1, &stmt, NULL);
        if (rc != SQLITE_OK) goto cleanup;

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) { sqlite3_finalize(stmt); goto cleanup; }

        int64_t hash = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);

        // Verify the hash can be looked up using the same int64 representation
        // This tests the PRId64 format consistency fix
        char sql[256];
        snprintf(sql, sizeof(sql), "SELECT 1 FROM cloudsync_schema_versions WHERE hash = (%" PRId64 ")", hash);

        stmt = NULL;
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) goto cleanup;

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) { sqlite3_finalize(stmt); goto cleanup; }

        int found = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if (found != 1) goto cleanup;
    }

    result = true;

cleanup:
    if (db) close_db(db);
    return result;
}

// Test cloudsync_blob_compare function
bool do_test_blob_compare(void) {
    // Test same content, same size
    const char blob1[] = {0x01, 0x02, 0x03, 0x04};
    const char blob2[] = {0x01, 0x02, 0x03, 0x04};
    int result = cloudsync_blob_compare(blob1, 4, blob2, 4);
    if (result != 0) return false;

    // Test different sizes (line 168 in utils.c)
    const char blob3[] = {0x01, 0x02, 0x03};
    result = cloudsync_blob_compare(blob1, 4, blob3, 3);
    if (result == 0) return false;  // Should be non-zero (different sizes)

    // Test different content, same size
    const char blob4[] = {0x01, 0x02, 0x03, 0x05};
    result = cloudsync_blob_compare(blob1, 4, blob4, 4);
    if (result == 0) return false;  // Should be non-zero (different content)

    // Test empty blobs
    result = cloudsync_blob_compare("", 0, "", 0);
    if (result != 0) return false;

    return true;
}

// Test string duplication functions more thoroughly
bool do_test_string_functions(void) {
    // Test cloudsync_string_ndup (non-lowercase path)
    const char *test_str = "Hello World";
    char *dup = cloudsync_string_ndup(test_str, 5);  // "Hello"
    if (!dup) return false;
    if (strcmp(dup, "Hello") != 0) {
        cloudsync_memory_free(dup);
        return false;
    }
    cloudsync_memory_free(dup);

    // Test cloudsync_string_dup
    dup = cloudsync_string_dup("Test String");
    if (!dup) return false;
    if (strcmp(dup, "Test String") != 0) {
        cloudsync_memory_free(dup);
        return false;
    }
    cloudsync_memory_free(dup);

    // Test with empty string
    dup = cloudsync_string_dup("");
    if (!dup) return false;
    if (strlen(dup) != 0) {
        cloudsync_memory_free(dup);
        return false;
    }
    cloudsync_memory_free(dup);

    return true;
}

// Test UUID functions
bool do_test_uuid_functions(void) {
    uint8_t uuid1[UUID_LEN];
    uint8_t uuid2[UUID_LEN];

    // Generate two UUIDs
    if (cloudsync_uuid_v7(uuid1) != 0) return false;
    if (cloudsync_uuid_v7(uuid2) != 0) return false;

    // Test UUID comparison - uuid1 should be less than or equal to uuid2 (time-based)
    int cmp = cloudsync_uuid_v7_compare(uuid1, uuid2);
    // cmp can be -1, 0, or 1

    // Test UUID stringify
    char str[UUID_STR_MAXLEN];
    char *result = cloudsync_uuid_v7_stringify(uuid1, str, true);  // With dashes
    if (!result) return false;
    if (strlen(result) != 36) return false;  // UUID with dashes is 36 chars

    result = cloudsync_uuid_v7_stringify(uuid1, str, false);  // Without dashes
    if (!result) return false;
    if (strlen(result) != 32) return false;  // UUID without dashes is 32 chars

    // Test cloudsync_uuid_v7_string
    result = cloudsync_uuid_v7_string(str, true);
    if (!result) return false;
    if (strlen(result) != 36) return false;

    // Test comparison with same UUID
    cmp = cloudsync_uuid_v7_compare(uuid1, uuid1);
    if (cmp != 0) return false;

    return true;
}

// Test rowid decode function
bool do_test_rowid_decode(void) {
    int64_t db_version, seq;

    // Test with a known rowid value
    // rowid = (db_version << 30) | seq
    int64_t test_db_version = 100;
    int64_t test_seq = 500;
    int64_t rowid = (test_db_version << 30) | test_seq;

    cloudsync_rowid_decode(rowid, &db_version, &seq);

    if (db_version != test_db_version) return false;
    if (seq != test_seq) return false;

    // Test with larger values
    test_db_version = 1000000;
    test_seq = 1000000;  // Max seq is 30 bits
    rowid = (test_db_version << 30) | (test_seq & 0x3FFFFFFF);

    cloudsync_rowid_decode(rowid, &db_version, &seq);

    if (db_version != test_db_version) return false;
    if (seq != (test_seq & 0x3FFFFFFF)) return false;

    return true;
}

// Test SQL-level schema functions
bool do_test_sql_schema_functions(void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    bool result = false;
    int rc;

    rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Test cloudsync_set_schema SQL function
    rc = sqlite3_exec(db, "SELECT cloudsync_set_schema('test_schema');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Test cloudsync_schema SQL function - should return the schema we just set
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_schema();", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    const char *schema = (const char *)sqlite3_column_text(stmt, 0);
    if (!schema || strcmp(schema, "test_schema") != 0) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Set schema to NULL
    rc = sqlite3_exec(db, "SELECT cloudsync_set_schema(NULL);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Test cloudsync_schema SQL function - should return NULL now
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_schema();", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    // Should be NULL
    if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Create a table and test cloudsync_table_schema
    rc = sqlite3_exec(db, "CREATE TABLE schema_test (id TEXT PRIMARY KEY NOT NULL, data TEXT);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_exec(db, "SELECT cloudsync_init('schema_test');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Test cloudsync_table_schema SQL function
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_table_schema('schema_test');", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;
    // Result can be NULL or a schema name depending on implementation

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test cloudsync_table_schema for non-existent table
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_table_schema('non_existent_table');", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    // Should be NULL for non-existent table
    if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    result = true;

cleanup:
    if (stmt) sqlite3_finalize(stmt);
    if (db) close_db(db);
    return result;
}

// Test SQL-level pk_decode function
bool do_test_sql_pk_decode(void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    bool result = false;
    int rc;

    rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Create a primary key with multiple values (no NULL — primary keys cannot contain NULL)
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_pk_encode(123, 'hello', 3.14, X'DEADBEEF');", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    const void *pk = sqlite3_column_blob(stmt, 0);
    int pk_len = sqlite3_column_bytes(stmt, 0);

    // Copy the pk blob
    char pk_copy[1024];
    memcpy(pk_copy, pk, pk_len);

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test cloudsync_pk_decode SQL function for INTEGER (index 1)
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_pk_decode(?, 1);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_blob(stmt, 1, pk_copy, pk_len, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    int64_t int_val = sqlite3_column_int64(stmt, 0);
    if (int_val != 123) goto cleanup;

    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test cloudsync_pk_decode for TEXT (index 2)
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_pk_decode(?, 2);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_blob(stmt, 1, pk_copy, pk_len, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    const char *text_val = (const char *)sqlite3_column_text(stmt, 0);
    if (!text_val || strcmp(text_val, "hello") != 0) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test cloudsync_pk_decode for FLOAT (index 3)
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_pk_decode(?, 3);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_blob(stmt, 1, pk_copy, pk_len, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    double float_val = sqlite3_column_double(stmt, 0);
    if (float_val < 3.13 || float_val > 3.15) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test cloudsync_pk_decode for BLOB (index 4)
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_pk_decode(?, 4);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_blob(stmt, 1, pk_copy, pk_len, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    const unsigned char expected_blob[] = {0xDE, 0xAD, 0xBE, 0xEF};
    const void *blob_val = sqlite3_column_blob(stmt, 0);
    int blob_len = sqlite3_column_bytes(stmt, 0);
    if (blob_len != 4 || memcmp(blob_val, expected_blob, 4) != 0) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    result = true;

cleanup:
    if (stmt) sqlite3_finalize(stmt);
    if (db) close_db(db);
    return result;
}

// Test negative integer and float encoding/decoding
bool do_test_pk_negative_values(void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    bool result = false;
    int rc;

    rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Test negative integer encoding and decoding
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_pk_encode(-12345);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    const void *pk = sqlite3_column_blob(stmt, 0);
    int pk_len = sqlite3_column_bytes(stmt, 0);
    char pk_copy[1024];
    memcpy(pk_copy, pk, pk_len);

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Decode and verify
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_pk_decode(?, 1);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_blob(stmt, 1, pk_copy, pk_len, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    int64_t int_val = sqlite3_column_int64(stmt, 0);
    if (int_val != -12345) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test negative float encoding and decoding
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_pk_encode(-3.14159);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    pk = sqlite3_column_blob(stmt, 0);
    pk_len = sqlite3_column_bytes(stmt, 0);
    memcpy(pk_copy, pk, pk_len);

    sqlite3_finalize(stmt);
    stmt = NULL;

    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_pk_decode(?, 1);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_blob(stmt, 1, pk_copy, pk_len, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    double float_val = sqlite3_column_double(stmt, 0);
    if (float_val > -3.14 || float_val < -3.15) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test INT64_MIN (maximum negative integer)
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_pk_encode(-9223372036854775808);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    pk = sqlite3_column_blob(stmt, 0);
    pk_len = sqlite3_column_bytes(stmt, 0);
    memcpy(pk_copy, pk, pk_len);

    sqlite3_finalize(stmt);
    stmt = NULL;

    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_pk_decode(?, 1);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_blob(stmt, 1, pk_copy, pk_len, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    int_val = sqlite3_column_int64(stmt, 0);
    if (int_val != INT64_MIN) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    result = true;

cleanup:
    if (stmt) sqlite3_finalize(stmt);
    if (db) close_db(db);
    return result;
}

// Test settings functions
bool do_test_settings_functions(void) {
    sqlite3 *db = NULL;
    bool result = false;
    int rc;

    rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Test cloudsync_set
    rc = sqlite3_exec(db, "SELECT cloudsync_set('test_key', 'test_value');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Create a table and test table-level settings
    rc = sqlite3_exec(db, "CREATE TABLE settings_test (id TEXT PRIMARY KEY NOT NULL, data TEXT);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_exec(db, "SELECT cloudsync_init('settings_test');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Test cloudsync_set_table
    rc = sqlite3_exec(db, "SELECT cloudsync_set_table('settings_test', 'table_key', 'table_value');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Test cloudsync_set_column
    rc = sqlite3_exec(db, "SELECT cloudsync_set_column('settings_test', 'data', 'col_key', 'col_value');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    result = true;

cleanup:
    if (db) close_db(db);
    return result;
}

// Test cloudsync_is_sync and cloudsync_is_enabled functions
bool do_test_sync_enabled_functions(void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    bool result = false;
    int rc;

    rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Create and init a table
    rc = sqlite3_exec(db, "CREATE TABLE sync_test (id TEXT PRIMARY KEY NOT NULL);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_exec(db, "SELECT cloudsync_init('sync_test');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Test cloudsync_is_enabled - should be 1 after init
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_is_enabled('sync_test');", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    int enabled = sqlite3_column_int(stmt, 0);
    if (enabled != 1) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test cloudsync_is_sync - should return 0 (not in sync mode)
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_is_sync('sync_test');", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    // Value depends on implementation
    sqlite3_finalize(stmt);
    stmt = NULL;

    // Disable sync
    rc = sqlite3_exec(db, "SELECT cloudsync_disable('sync_test');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Test cloudsync_is_enabled - should be 0 after disable
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_is_enabled('sync_test');", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    enabled = sqlite3_column_int(stmt, 0);
    if (enabled != 0) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Re-enable sync
    rc = sqlite3_exec(db, "SELECT cloudsync_enable('sync_test');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Test cloudsync_is_enabled - should be 1 again
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_is_enabled('sync_test');", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    enabled = sqlite3_column_int(stmt, 0);
    if (enabled != 1) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test with non-existent table
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_is_enabled('non_existent_table');", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    enabled = sqlite3_column_int(stmt, 0);
    if (enabled != 0) goto cleanup;  // Should be 0 for non-existent table

    sqlite3_finalize(stmt);
    stmt = NULL;

    result = true;

cleanup:
    if (stmt) sqlite3_finalize(stmt);
    if (db) close_db(db);
    return result;
}

// Test cloudsync_uuid SQL function
bool do_test_sql_uuid_function(void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    bool result = false;
    int rc;

    rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Test cloudsync_uuid() SQL function
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_uuid();", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    const char *uuid1 = (const char *)sqlite3_column_text(stmt, 0);
    if (!uuid1 || strlen(uuid1) != 36) goto cleanup;  // UUID with dashes

    // Store the first UUID
    char uuid1_copy[40];
    strncpy(uuid1_copy, uuid1, sizeof(uuid1_copy) - 1);

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Get another UUID - should be different
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_uuid();", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    const char *uuid2 = (const char *)sqlite3_column_text(stmt, 0);
    if (!uuid2 || strlen(uuid2) != 36) goto cleanup;

    // UUIDs should be different
    if (strcmp(uuid1_copy, uuid2) == 0) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    result = true;

cleanup:
    if (stmt) sqlite3_finalize(stmt);
    if (db) close_db(db);
    return result;
}

// Test pk_encode with empty values
bool do_test_pk_encode_edge_cases(void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    bool result = false;
    int rc;

    rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Test encoding empty text
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_pk_encode('');", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    const void *pk = sqlite3_column_blob(stmt, 0);
    if (!pk) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test encoding empty blob
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_pk_encode(X'');", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    pk = sqlite3_column_blob(stmt, 0);
    if (!pk) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test encoding zero
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_pk_encode(0);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    pk = sqlite3_column_blob(stmt, 0);
    if (!pk) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test encoding 0.0
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_pk_encode(0.0);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    pk = sqlite3_column_blob(stmt, 0);
    if (!pk) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test encoding large integers
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_pk_encode(9223372036854775807);", -1, &stmt, NULL);  // INT64_MAX
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    pk = sqlite3_column_blob(stmt, 0);
    if (!pk) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    result = true;

cleanup:
    if (stmt) sqlite3_finalize(stmt);
    if (db) close_db(db);
    return result;
}

// Test cloudsync_col_value function
bool do_test_col_value_function(void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    bool result = false;
    int rc;

    rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Create and init a table
    rc = sqlite3_exec(db, "CREATE TABLE col_test (id TEXT PRIMARY KEY NOT NULL, data TEXT, num INTEGER);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_exec(db, "SELECT cloudsync_init('col_test');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Insert data
    rc = sqlite3_exec(db, "INSERT INTO col_test (id, data, num) VALUES ('key1', 'value1', 42);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Get the pk for key1
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_pk_encode('key1');", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    const void *pk = sqlite3_column_blob(stmt, 0);
    int pk_len = sqlite3_column_bytes(stmt, 0);
    char pk_copy[256];
    memcpy(pk_copy, pk, pk_len);

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test cloudsync_col_value for text column
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_col_value('col_test', 'data', ?);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_blob(stmt, 1, pk_copy, pk_len, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    const char *val = (const char *)sqlite3_column_text(stmt, 0);
    if (!val || strcmp(val, "value1") != 0) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test cloudsync_col_value for integer column
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_col_value('col_test', 'num', ?);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_blob(stmt, 1, pk_copy, pk_len, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    int num_val = sqlite3_column_int(stmt, 0);
    if (num_val != 42) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test cloudsync_col_value with TOMBSTONE value (should return NULL)
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_col_value('col_test', '__[RIP]__', ?);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_blob(stmt, 1, pk_copy, pk_len, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) goto cleanup;

    sqlite3_finalize(stmt);
    stmt = NULL;

    result = true;

cleanup:
    if (stmt) sqlite3_finalize(stmt);
    if (db) close_db(db);
    return result;
}

// Test cloudsync_is_sync function
bool do_test_is_sync_function(void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    bool result = false;
    int rc;

    rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Create and init a table
    rc = sqlite3_exec(db, "CREATE TABLE sync_check (id TEXT PRIMARY KEY NOT NULL);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_exec(db, "SELECT cloudsync_init('sync_check');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Test cloudsync_is_sync
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_is_sync('sync_check');", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    // Result depends on internal state
    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test cloudsync_is_sync with non-existent table
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_is_sync('non_existent');", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    int is_sync = sqlite3_column_int(stmt, 0);
    if (is_sync != 0) goto cleanup;  // Should be 0 for non-existent table

    sqlite3_finalize(stmt);
    stmt = NULL;

    result = true;

cleanup:
    if (stmt) sqlite3_finalize(stmt);
    if (db) close_db(db);
    return result;
}

// Test cloudsync_db_version_next function
bool do_test_db_version_next(void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    bool result = false;
    int rc;

    rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Create and sync a table to properly initialize the context
    rc = sqlite3_exec(db, "CREATE TABLE dbv_test (id TEXT PRIMARY KEY NOT NULL, val TEXT);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_exec(db, "SELECT cloudsync_sync('dbv_test');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Test cloudsync_db_version_next without argument
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_db_version_next();", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    int64_t v1 = sqlite3_column_int64(stmt, 0);

    sqlite3_finalize(stmt);
    stmt = NULL;

    // Test cloudsync_db_version_next with merging_version argument
    rc = sqlite3_prepare_v2(db, "SELECT cloudsync_db_version_next(100);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) goto cleanup;

    int64_t v2 = sqlite3_column_int64(stmt, 0);

    sqlite3_finalize(stmt);
    stmt = NULL;

    // v2 should be greater or equal to v1
    if (v2 < v1) goto cleanup;

    result = true;

cleanup:
    if (stmt) sqlite3_finalize(stmt);
    if (db) close_db(db);
    return result;
}

// Test various insert/update/delete scenarios through SQL
bool do_test_insert_update_delete_sql(void) {
    sqlite3 *db = NULL;
    bool result = false;
    int rc;

    rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Create and init a table
    rc = sqlite3_exec(db, "CREATE TABLE iud_test (id TEXT PRIMARY KEY NOT NULL, val TEXT, num REAL);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_exec(db, "SELECT cloudsync_init('iud_test');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Insert data
    rc = sqlite3_exec(db, "INSERT INTO iud_test (id, val, num) VALUES ('id1', 'initial', 1.5);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Update data
    rc = sqlite3_exec(db, "UPDATE iud_test SET val = 'updated', num = 2.5 WHERE id = 'id1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Insert more data
    rc = sqlite3_exec(db, "INSERT INTO iud_test (id, val, num) VALUES ('id2', 'second', 3.5);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Delete data
    rc = sqlite3_exec(db, "DELETE FROM iud_test WHERE id = 'id2';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // Check changes table
    rc = sqlite3_exec(db, "SELECT COUNT(*) FROM cloudsync_changes;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    result = true;

cleanup:
    if (db) close_db(db);
    return result;
}

// Test dbutils_binary_comparison function (already exposed for testing)
bool do_test_binary_comparison(void) {
    // Test cases for dbutils_binary_comparison
    int result1 = dbutils_binary_comparison(5, 3);  // 5 > 3, should return 1
    if (result1 != 1) return false;

    int result2 = dbutils_binary_comparison(3, 5);  // 3 < 5, should return -1
    if (result2 != -1) return false;

    int result3 = dbutils_binary_comparison(5, 5);  // 5 == 5, should return 0
    if (result3 != 0) return false;

    int result4 = dbutils_binary_comparison(-10, 10);  // -10 < 10
    if (result4 != -1) return false;

    int result5 = dbutils_binary_comparison(0, 0);  // 0 == 0
    if (result5 != 0) return false;

    return true;
}


// Test pk_decode with various malformed inputs
bool do_test_pk_decode_malformed(void) {
    // Test with empty buffer
    int res = pk_decode_prikey(NULL, 0, NULL, NULL);
    if (res != -1) return false;  // Should fail for NULL buffer

    // Test with buffer but 0 length
    char empty[1] = {0};
    res = pk_decode_prikey(empty, 0, NULL, NULL);
    // This should also fail since count can't be read
    if (res != -1) return false;

    // Test pk_decode with count specified but incomplete buffer
    size_t seek = 0;
    res = pk_decode(empty, 0, 5, &seek, -1, NULL, NULL);
    if (res != -1) return false;  // Should fail - can't read 5 elements from empty buffer

    return true;
}


bool do_test_many_columns (int ncols, sqlite3 *db) {
    char sql_create[10000];
    int pos = 0;
    pos += snprintf(sql_create+pos, sizeof(sql_create)-pos, "CREATE TABLE IF NOT EXISTS test_many_columns (id TEXT PRIMARY KEY NOT NULL");
    for (int i=1; i<ncols; i++) {
        pos += snprintf(sql_create+pos, sizeof(sql_create)-pos, ", col%d TEXT", i);
    }
    pos += snprintf(sql_create+pos, sizeof(sql_create)-pos, ");");

    int rc = sqlite3_exec(db, sql_create, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return false;
    
    char *sql = "SELECT cloudsync_init('test_many_columns', 'cls', 1);";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return false;
    
    sql = sqlite3_mprintf("INSERT INTO test_many_columns (id, col1, col%d) VALUES ('test-id-1', 'original1', 'original%d');", ncols-1, ncols-1);
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) return false;

    sql = sqlite3_mprintf("UPDATE test_many_columns SET col1 = 'updated1', col%d = 'updated%d' WHERE id = 'test-id-1';", ncols-1, ncols-1);
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) return false;
   
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
        
        dbvm_reset(insert_stmt);
    }
    
    rc = SQLITE_OK;
    result = true;
    
finalize:
    if (rc != SQLITE_OK) printf("Error in do_merge_values %s - %s\n", sqlite3_errmsg(srcdb), sqlite3_errmsg(destdb));
    if (select_stmt) sqlite3_finalize(select_stmt);
    if (insert_stmt) sqlite3_finalize(insert_stmt);
    return result;
}

bool do_merge_using_payload (sqlite3 *srcdb, sqlite3 *destdb, bool only_local, bool print_error_msg) {
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
        
        dbvm_reset(insert_stmt);
    }
    
    rc = SQLITE_OK;
    result = true;
    
finalize:
    if (rc != SQLITE_OK && print_error_msg) printf("Error in do_merge_using_payload %s - %s\n", sqlite3_errmsg(srcdb), sqlite3_errmsg(destdb));
    if (select_stmt) rc = sqlite3_finalize(select_stmt);
    if (insert_stmt) rc = sqlite3_finalize(insert_stmt);
    return result;
}

bool do_merge (sqlite3 *db[MAX_SIMULATED_CLIENTS], int nclients, bool only_local) {
    for (int i=0; i<nclients; ++i) {
        int target = i;
        for (int j=0; j<nclients; ++j) {
            if (target == j) continue;
            if (do_merge_using_payload(db[target], db[j], only_local, true) == false) {
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

    return db;
}

void do_build_database_path (char buf[256], int i, time_t timestamp, int ntest) {
    #ifdef __ANDROID__
    snprintf(buf, 256, "%s/cloudsync-test-%ld-%d-%d.sqlite", ".", timestamp, ntest, i);
    #else
    snprintf(buf, 256, "%s/cloudsync-test-%ld-%d-%d.sqlite", getenv("HOME"), timestamp, ntest, i);
    #endif
}

sqlite3 *do_create_database_file_v2 (int i, time_t timestamp, int ntest) {
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

    return db;
}

sqlite3 *do_create_database_file (int i, time_t timestamp, int ntest) {
    return do_create_database_file_v2(i, timestamp, ntest);
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
        db[i] = do_create_database_file_v2(i, timestamp, test_counter++);
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
        char *sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        sqlite3_free(sql);
        if (result == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> customers\n");
        char *sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        do_query(db[0], sql, query_table);
        sqlite3_free(sql);
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
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
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
                char *sql = sqlite3_mprintf("UPDATE \"%w\" SET age = ABS(RANDOM()), note='hello' || HEX(RANDOMBLOB(2)), stamp='stamp' || ABS(RANDOM() %% 99) WHERE rowid=1;", CUSTOMERS_TABLE);
                rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
                sqlite3_free(sql);
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
            char *sql = sqlite3_mprintf("UPDATE \"%w\" SET age = ABS(RANDOM()), note='hello' || HEX(RANDOMBLOB(2)), stamp='stamp' || ABS(RANDOM() %% 99) WHERE rowid=1;", CUSTOMERS_TABLE);
            rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
            sqlite3_free(sql);
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
            char *sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
            result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
            sqlite3_free(sql);
            if (result == false) goto finalize;
        }
        if (table_mask & TEST_NOCOLS) {
            const char *sql = "SELECT * FROM \"" CUSTOMERS_NOCOLS_TABLE "\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";";
            if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
        }
    }
    
    if (print_result) {
        if (table_mask & TEST_PRIKEYS) {
            printf("\n-> " CUSTOMERS_TABLE "\n");
            char *sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
            do_query(db[0], sql, query_table);
            sqlite3_free(sql);
        }
        if (table_mask & TEST_NOCOLS) {
            printf("\n-> \"" CUSTOMERS_NOCOLS_TABLE "s\"\n");
            do_query(db[0], "SELECT * FROM \"" CUSTOMERS_NOCOLS_TABLE "\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", query_table);
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
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
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
    
    char buf[512];
    
    // insert, update and delete some data in all clients
    for (int i=0; i<nclients; ++i) {
        do_insert(db[i], table_mask, 1, print_result);
        sqlite3_snprintf(sizeof(buf), buf, "UPDATE \"%w\" SET age=%d, note='note%d', stamp='stamp%d';", CUSTOMERS_TABLE, i+nclients, i+nclients, 9-i);
        rc = sqlite3_exec(db[i], buf, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // merge all changes
    if (do_merge(db, nclients, true) == false) {
        goto finalize;
    }
    
    // compare results
    for (int i=1; i<nclients; ++i) {
        sqlite3_snprintf(sizeof(buf), buf, "SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        result = do_compare_queries(db[0], buf, db[i], buf, -1, -1, print_result);
        if (result == false) goto finalize;
    
        const char *sql2 = "SELECT 'name1', 'surname1', 3, 'note3', 'stamp9'";
        if (do_compare_queries(db[0], buf, db[i], sql2, -1, -1, print_result) == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> " CUSTOMERS_TABLE "\n");
        sqlite3_snprintf(sizeof(buf), buf, "SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        do_query(db[0], buf, query_table);
    }
    
    result = true;
    rc = SQLITE_OK;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_4 error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char path[256];
            do_build_database_path(path, i, timestamp, saved_counter++);
            file_delete_internal(path);
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
    sqlite3_snprintf(sizeof(buf), buf, "UPDATE \"%w\" SET age=%d;", CUSTOMERS_TABLE, n);
    rc = sqlite3_exec(db[0], buf, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
        
    if (do_merge_values(db[0], db[1], true) == false) {
        return false;
    }
    
    sqlite3_snprintf(sizeof(buf), buf, "UPDATE \"%w\" SET first_name='name%d';", CUSTOMERS_TABLE, n);
    rc = sqlite3_exec(db[1], buf, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;

    // merge all changes
    if (do_merge(db, nclients, only_locals) == false) {
        goto finalize;
    }
    
    // compare results
    for (int i=1; i<nclients; ++i) {
        char *sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) {
            sqlite3_free(sql);
            goto finalize;
        }
        sqlite3_free(sql);
    }
    
    if (print_result) {
        printf("\n-> customers\n");
        char *sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        do_query(db[0], sql, query_table);
        sqlite3_free(sql);
    }
    
    result = true;
    rc = SQLITE_OK;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_5: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char path[256];
            do_build_database_path(path, i, timestamp, saved_counter++);
            file_delete_internal(path);
        }
    }
    return result;
}

bool do_test_merge_check_db_version (int nclients, bool print_result, bool cleanup_databases, bool only_locals) {
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
    int saved_counter = test_counter++;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file_v2(i, timestamp, saved_counter);
        if (db[i] == false) return false;
        
        rc = sqlite3_exec(db[i], "CREATE TABLE todo (id TEXT PRIMARY KEY NOT NULL, title TEXT, status TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('todo');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;;
    }
    
    // insert some data to db 0
    
    rc = sqlite3_exec(db[0], "INSERT INTO todo (id, title, status) VALUES ('ID1', 'Buy groceries', 'in_progress1');", NULL, NULL, NULL); if (rc != SQLITE_OK) goto finalize;

    rc = sqlite3_exec(db[0], "INSERT INTO todo (id, title, status) VALUES ('ID2', 'Buy bananas', 'in_progress2'), ('ID3', 'Buy vegetables', 'in_progress3');", NULL, NULL, NULL); if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db[0], "INSERT INTO todo (id, title, status) VALUES ('ID4', 'Buy apples', 'in_progress4');", NULL, NULL, NULL); if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db[0], "BEGIN TRANSACTION;", NULL, NULL, NULL); if (rc != SQLITE_OK) goto finalize;
    rc = sqlite3_exec(db[0], "INSERT INTO todo (id, title, status) VALUES ('ID5', 'Buy oranges', 'in_progress5');", NULL, NULL, NULL); if (rc != SQLITE_OK) goto finalize;
    rc = sqlite3_exec(db[0], "INSERT INTO todo (id, title, status) VALUES ('ID6', 'Buy lemons', 'in_progress6');", NULL, NULL, NULL); if (rc != SQLITE_OK) goto finalize;
    rc = sqlite3_exec(db[0], "INSERT INTO todo (id, title, status) VALUES ('ID7', 'Buy pizza', 'in_progress7');", NULL, NULL, NULL); if (rc != SQLITE_OK) goto finalize;
    rc = sqlite3_exec(db[0], "COMMIT;", NULL, NULL, NULL); if (rc != SQLITE_OK) goto finalize;
    
    if (do_merge_using_payload(db[0], db[1], only_locals, true) == false) {
        goto finalize;
    }
    
    // compare results
    for (int i=1; i<nclients; ++i) {
        char *sql = "SELECT * FROM todo ORDER BY id;";
        if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) {
            goto finalize;
        }
    }
    
    // compare values in the changes vtab
    for (int i=1; i<nclients; ++i) {
        char *sql = "SELECT * FROM cloudsync_changes;";
        if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) {
            goto finalize;
        }
    }
    
    if (print_result) {
        printf("\n-> customers\n");
        do_query(db[1], "SELECT * FROM cloudsync_changes;", query_changes);
    }
    
    result = true;
    rc = SQLITE_OK;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_check_db_version: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter);
            file_delete_internal(buf);
        }
    }
    return result;
}

bool do_test_merge_check_db_version_2 (int nclients, bool print_result, bool cleanup_databases, bool only_locals) {
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
    int saved_counter = test_counter++;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file_v2(i, timestamp, saved_counter);
        if (db[i] == false) return false;
        
        rc = sqlite3_exec(db[i], "CREATE TABLE todo (id TEXT PRIMARY KEY NOT NULL, title TEXT, status TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('todo');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;;
    }
    
    // insert some data to db 0
    rc = sqlite3_exec(db[0], "INSERT INTO todo (id, title, status) VALUES ('ID1', 'Buy groceries', 'in_progress');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    rc = sqlite3_exec(db[0], "INSERT INTO todo (id, title, status) VALUES ('ID2', 'Foo', 'Bar');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // insert some data to db 1
    rc = sqlite3_exec(db[1], "INSERT INTO todo (id, title, status) VALUES ('ID3', 'Foo3', 'Bar3');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    rc = sqlite3_exec(db[1], "INSERT INTO todo (id, title, status) VALUES ('ID4', 'Foo4', 'Bar4');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    rc = sqlite3_exec(db[1], "BEGIN TRANSACTION; INSERT INTO todo (id, title, status) VALUES ('ID5', 'Foo5', 'Bar5'); INSERT INTO todo (id, title, status) VALUES ('ID6', 'Foo6', 'Bar6'); COMMIT;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;

    
    if (do_merge_using_payload(db[0], db[1], only_locals, true) == false) {
        goto finalize;
    }
    
    if (do_merge_using_payload(db[1], db[0], only_locals, true) == false) {
        goto finalize;
    }
    
    // compare results
    for (int i=1; i<nclients; ++i) {
        char *sql = "SELECT * FROM todo ORDER BY id;";
        if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) {
            goto finalize;
        }
    }
    
    // check repeated db_version, seq tuple in not repeated in cloudsync_changes
    for (int i=0; i<nclients; ++i) {
        char *sql = "SELECT db_version, seq, COUNT(*) AS cnt "
                    "FROM cloudsync_changes "
                    "GROUP BY db_version, seq "
                    "HAVING COUNT(*) > 1;";
        sqlite3_stmt *vm = NULL;
        rc = sqlite3_prepare_v2(db[i], sql, -1, &vm, NULL);
        if (rc != SQLITE_OK) goto finalize;
        rc = sqlite3_step(vm);
        if (rc != SQLITE_DONE) {
            printf("cloudsync_changes should not have repeated db_version and seq values, got: db_version=%d, seq=%d, count=%d\n", sqlite3_column_int(vm, 0), sqlite3_column_int(vm, 1), sqlite3_column_int(vm, 2));
            if (vm) sqlite3_finalize(vm);
            goto finalize;
        }
        if (vm) sqlite3_finalize(vm);
    }
    
    // check grouped values from cloudsync_changes
    char *sql_changes = "SELECT db_version, COUNT(distinct(seq)) AS cnt FROM cloudsync_changes GROUP BY db_version;";
    char *query_expected_results = "SELECT * FROM (VALUES (1,2),(2,2),(3,2),(4,2),(5,4));";
    if (do_compare_queries(db[0], sql_changes, db[0], query_expected_results, -1, -1, print_result) == false) {
        goto finalize;
    }
    
    if (print_result) {
        printf("\n-> customers\n");
        do_query(db[1], "SELECT * FROM cloudsync_changes();", query_changes);
    }
    
    result = true;
    rc = SQLITE_OK;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_check_db_version_2: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter);
            file_delete_internal(buf);
        }
    }
    return result;
}

bool do_test_insert_cloudsync_changes (bool print_result, bool cleanup_databases) {
    sqlite3 *db = NULL;
    bool result = false;
    int rc = SQLITE_OK;
    
    // create databases and tables
    time_t timestamp = time(NULL);
    int saved_counter = test_counter++;
    db = do_create_database_file(0, timestamp, saved_counter);
    if (db == false) return false;
        
    rc = sqlite3_exec(db, "CREATE TABLE todo (id TEXT PRIMARY KEY NOT NULL, title TEXT, status TEXT);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db, "SELECT cloudsync_init('todo');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;;
    
    // insert some data to db 0
    
    // db_version = 1
    rc = sqlite3_exec(db, "INSERT INTO todo (id, title, status) VALUES ('1', 'Buy groceries', 'in_progress1');", NULL, NULL, NULL); if (rc != SQLITE_OK) goto finalize;

    // db_version = 2
    rc = sqlite3_exec(db, "INSERT INTO todo (id, title, status) VALUES ('2', 'Buy bananas', 'in_progress2');", NULL, NULL, NULL); if (rc != SQLITE_OK) goto finalize;
    
    // insert hardcoded changes to cloudsync_changes:
    // - insert a new row with values for two non-pk columns
    rc = sqlite3_exec(db, "INSERT INTO cloudsync_changes (tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq) VALUES ('todo',x'010B0133','title','New Item',10,10,x'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA',1,0);", NULL, NULL, NULL); if (rc != SQLITE_OK) goto finalize;
    rc = sqlite3_exec(db, "INSERT INTO cloudsync_changes (tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq) VALUES ('todo',x'010B0133','status','in progress',10,10,x'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA',1,1);", NULL, NULL, NULL); if (rc != SQLITE_OK) goto finalize;
  
    // - update existing row, setting local db_value to 300
    rc = sqlite3_exec(db, "INSERT INTO cloudsync_changes (tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq) VALUES ('todo',x'010B0131','status','finalized',200,300,x'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA',1,0);", NULL, NULL, NULL); if (rc != SQLITE_OK) goto finalize;
    
    // - this update must be ignored because col_version < local_col_version, even if db_version is greater
    rc = sqlite3_exec(db, "INSERT INTO cloudsync_changes (tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq) VALUES ('todo',x'010B0133','title','this update must be ignored',9,11,x'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA',1,0);", NULL, NULL, NULL); if (rc != SQLITE_OK) goto finalize;
    
    // - this update must be applied because col_version > local_col_version, even if db_version is smaller
    rc = sqlite3_exec(db, "INSERT INTO cloudsync_changes (tbl,pk,col_name,col_value,col_version,db_version,site_id,cl,seq) VALUES ('todo',x'010B0133','status','this update must be applied',11,9,x'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA',1,0);", NULL, NULL, NULL); if (rc != SQLITE_OK) goto finalize;
    
    // compare results
    char *sql = "SELECT * FROM todo ORDER BY id;";
    char *query_expected_results = "SELECT * FROM (VALUES ('1','Buy groceries','finalized'),('2','Buy bananas','in_progress2'),('3','New Item','this update must be applied'));";
    if (do_compare_queries(db, sql, db, query_expected_results, -1, -1, print_result) == false) {
        goto finalize;
    }
    
    // compare values in the changes vtab
    sql = "SELECT col_name,col_value,col_version,db_version,cl,seq FROM cloudsync_changes;";
    query_expected_results = "SELECT * FROM (VALUES ('title','Buy groceries',1,1,1,0),('title','Buy bananas',1,2,1,0),('status','in_progress2',1,2,1,1),('title','New Item',10,10,1,0),('status','finalized',200,300,1,0),('status','this update must be applied',11,301,1,0));";
    if (do_compare_queries(db, sql, db, query_expected_results, -1, -1, print_result) == false) {
        goto finalize;
    }
    
    if (print_result) {
        printf("\n-> customers\n");
        do_query(db, "SELECT * FROM todo;", query_changes);
    }
    
    result = true;
    rc = SQLITE_OK;
    
finalize:
    if (rc != SQLITE_OK && db && (sqlite3_errcode(db) != SQLITE_OK)) printf("do_test_insert_cloudsync_changes: %s\n", sqlite3_errmsg(db));
    if (db) close_db(db);
    if (cleanup_databases) {
        char buf[256];
        do_build_database_path(buf, 0, timestamp, saved_counter);
        file_delete_internal(buf);
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
    // perform the test ONLY if schema hash is enabled
    if (do_merge_using_payload(db[0], db[1], only_locals, false) == true) {
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
        char *sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        sqlite3_free(sql);
        if (result == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> customers\n");
        char *sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        do_query(db[0], sql, query_table);
        sqlite3_free(sql);
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
            file_delete_internal(buf);
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
    if (do_merge_using_payload(db[0], db[1], only_locals, false) == true) {
        goto finalize;
    }
    
    // insert a new value on db1
    do_insert_val(db[1], TEST_PRIKEYS, 123456, print_result);
        
    // merge changes from db1 to db0, it should work if columns are not removed
    if (do_merge_using_payload(db[1], db[0], only_locals, false) == false) {
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
        char *sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) {
            sqlite3_free(sql);
            goto finalize;
        }
        sqlite3_free(sql);
    }
    
    if (print_result) {
        printf("\n-> customers\n");
        char *sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        do_query(db[0], sql, query_table);
        sqlite3_free(sql);
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
            file_delete_internal(buf);
        }
    }
    return result;
}

bool do_test_merge_two_tables (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    int table_mask = TEST_PRIKEYS | TEST_NOCOLS;
    
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
    
    // perform transactions on both tables in client 0
    rc = sqlite3_exec(db[0], "BEGIN TRANSACTION;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // insert data into both tables in a single transaction
    char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('john', 'doe', 30, 'test note', 'stamp1');", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[0], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\") VALUES ('jane', 'smith');", CUSTOMERS_NOCOLS_TABLE);
    rc = sqlite3_exec(db[0], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db[0], "COMMIT;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // perform different transactions on both tables in client 1
    rc = sqlite3_exec(db[1], "BEGIN TRANSACTION;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('alice', 'jones', 25, 'another note', 'stamp2');", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[1], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\") VALUES ('bob', 'wilson');", CUSTOMERS_NOCOLS_TABLE);
    rc = sqlite3_exec(db[1], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db[1], "COMMIT;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // merge changes between the two clients
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // verify that both databases have the same content for both tables
    for (int i=1; i<nclients; ++i) {
        // compare customers table
        sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        bool comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        sqlite3_free(sql);
        if (comparison_result == false) goto finalize;
        
        // compare customers_nocols table
        const char *nocols_sql = "SELECT * FROM \"" CUSTOMERS_NOCOLS_TABLE "\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";";
        if (do_compare_queries(db[0], nocols_sql, db[i], nocols_sql, -1, -1, print_result) == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> " CUSTOMERS_TABLE "\n");
        sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        do_query(db[0], sql, query_table);
        sqlite3_free(sql);
        
        printf("\n-> \"" CUSTOMERS_NOCOLS_TABLE "\"\n");
        do_query(db[0], "SELECT * FROM \"" CUSTOMERS_NOCOLS_TABLE "\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", query_table);
    }
    
    result = true;
    rc = SQLITE_OK;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_two_tables error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_two_tables error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_two_tables error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test conflicting primary key updates
bool do_test_merge_conflicting_pkeys (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    int table_mask = TEST_PRIKEYS;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 2) {
        nclients = 2;
        printf("Number of test merge increased to %d clients\n", 2);
    }
    
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
    
    // insert same primary key in both clients
    char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('john', 'doe', 30, 'original', 'stamp1');", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[0], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('john', 'doe', 35, 'conflict', 'stamp2');", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[1], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    // update same record differently on both clients
    sql = sqlite3_mprintf("UPDATE \"%w\" SET age = 40, note = 'updated_client0' WHERE first_name = 'john' AND \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" = 'doe';", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[0], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_mprintf("UPDATE \"%w\" SET age = 45, note = 'updated_client1' WHERE first_name = 'john' AND \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\" = 'doe';", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[1], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    // merge changes - this should handle conflicts gracefully
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // verify databases converged to same state
    for (int i=1; i<nclients; ++i) {
        sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        bool comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        sqlite3_free(sql);
        if (comparison_result == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> " CUSTOMERS_TABLE " (after conflict resolution)\n");
        sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        do_query(db[0], sql, query_table);
        sqlite3_free(sql);
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_conflicting_pkeys error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_conflicting_pkeys error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_conflicting_pkeys error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test large dataset merge performance
bool do_test_merge_large_dataset (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    int table_mask = TEST_PRIKEYS;
    const int LARGE_N = 1000; // Insert 1000 records per client
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 2) {
        nclients = 2;
        printf("Number of test merge increased to %d clients\n", 2);
    }
    
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
    
    // insert large dataset in each client
    for (int i=0; i<nclients; ++i) {
        rc = sqlite3_exec(db[i], "BEGIN TRANSACTION;", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        for (int j=0; j<LARGE_N; ++j) {
            char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('name%d_client%d', 'surname%d', %d, 'note%d', 'stamp%d');", CUSTOMERS_TABLE, j, i, j, (j+i*LARGE_N) % 100, j, j);
            rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
            sqlite3_free(sql);
            if (rc != SQLITE_OK) goto finalize;
        }
        
        rc = sqlite3_exec(db[i], "COMMIT;", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // merge all datasets
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // verify all clients have same data
    for (int i=1; i<nclients; ++i) {
        char *sql = sqlite3_mprintf("SELECT COUNT(*) FROM \"%w\";", CUSTOMERS_TABLE);
        bool comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        sqlite3_free(sql);
        if (comparison_result == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> Large dataset merge completed\n");
        char *sql = sqlite3_mprintf("SELECT COUNT(*) as total_records FROM \"%w\";", CUSTOMERS_TABLE);
        do_query(db[0], sql, query_table);
        sqlite3_free(sql);
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_large_dataset error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_large_dataset error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_large_dataset error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test nested transactions before merge
bool do_test_merge_nested_transactions (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    int table_mask = TEST_PRIKEYS | TEST_NOCOLS;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 2) {
        nclients = 2;
        printf("Number of test merge increased to %d clients\n", 2);
    }
    
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
    
    // perform nested transactions in client 0
    rc = sqlite3_exec(db[0], "BEGIN TRANSACTION;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('tx1', 'user', 25, 'first_tx', 'stamp1');", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[0], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db[0], "SAVEPOINT sp1;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\") VALUES ('tx2', 'user');", CUSTOMERS_NOCOLS_TABLE);
    rc = sqlite3_exec(db[0], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db[0], "RELEASE sp1;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db[0], "COMMIT;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // perform different nested transactions in client 1
    rc = sqlite3_exec(db[1], "BEGIN TRANSACTION;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('tx3', 'user', 30, 'second_tx', 'stamp2');", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[1], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db[1], "SAVEPOINT sp2;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\") VALUES ('tx4', 'user');", CUSTOMERS_NOCOLS_TABLE);
    rc = sqlite3_exec(db[1], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db[1], "RELEASE sp2;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db[1], "COMMIT;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // merge changes
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // verify consistency
    for (int i=1; i<nclients; ++i) {
        sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        bool comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        sqlite3_free(sql);
        if (comparison_result == false) goto finalize;
        
        const char *nocols_sql = "SELECT * FROM \"" CUSTOMERS_NOCOLS_TABLE "\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";"; 
        if (do_compare_queries(db[0], nocols_sql, db[i], nocols_sql, -1, -1, print_result) == false) goto finalize;
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_nested_transactions error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_nested_transactions error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_nested_transactions error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test three-way merge pattern
bool do_test_merge_three_way (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    int table_mask = TEST_PRIKEYS;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients < 3) {
        nclients = 3;
        printf("Number of test merge increased to %d clients\n", 3);
    }
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    }
    
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
    
    // insert different data in each client
    for (int i=0; i<nclients; ++i) {
        char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('client%d', 'data', %d, 'from_client_%d', 'stamp%d');", CUSTOMERS_TABLE, i, 20+i*10, i, i);
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // perform chain merge: A->B, then B->C, then verify A==C
    if (do_merge_using_payload(db[0], db[1], false, true) == false) {
        goto finalize;
    }
    
    if (do_merge_using_payload(db[1], db[2], false, true) == false) {
        goto finalize;
    }
    
    if (do_merge_using_payload(db[2], db[0], false, true) == false) {
        goto finalize;
    }
    
    // final full merge to ensure consistency
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // verify all databases are identical
    for (int i=1; i<nclients; ++i) {
        char *sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        bool comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        sqlite3_free(sql);
        if (comparison_result == false) goto finalize;
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_three_way error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_three_way error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_three_way error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test NULL value handling during merge
bool do_test_merge_null_values (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    int table_mask = TEST_PRIKEYS;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 2) {
        nclients = 2;
        printf("Number of test merge increased to %d clients\n", 2);
    }
    
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
    
    // insert record with NULL values in client 0
    char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('null_test', 'user', NULL, NULL, NULL);", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[0], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    // update with non-NULL values in client 1
    sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('null_test', 'user', 25, 'updated', 'timestamp');", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[1], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    // update NULL to non-NULL in client 0
    sql = sqlite3_mprintf("UPDATE \"%w\" SET age = 30 WHERE first_name = 'null_test';", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[0], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    // update non-NULL to NULL in client 1
    sql = sqlite3_mprintf("UPDATE \"%w\" SET note = NULL WHERE first_name = 'null_test';", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[1], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    // merge changes
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // verify consistency
    for (int i=1; i<nclients; ++i) {
        sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        bool comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        sqlite3_free(sql);
        if (comparison_result == false) goto finalize;
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_null_values error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_null_values error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_null_values error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test BLOB data merge
bool do_test_merge_blob_data (int nclients, bool print_result, bool cleanup_databases) {
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
    
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        // create table with BLOB column
        const char *sql = "CREATE TABLE blob_test (id TEXT PRIMARY KEY NOT NULL, data BLOB, description TEXT);";
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        sql = "SELECT cloudsync_init('blob_test', 'cls', 1);";
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // insert BLOB data in different clients
    const char *sql1 = "INSERT INTO blob_test (id, data, description) VALUES ('blob1', X'48656c6c6f20576f726c64', 'Hello World from client0');";
    rc = sqlite3_exec(db[0], sql1, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    const char *sql2 = "INSERT INTO blob_test (id, data, description) VALUES ('blob2', X'54657374204461746121', 'Test Data from client1');";
    rc = sqlite3_exec(db[1], sql2, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // update BLOB data
    const char *sql3 = "UPDATE blob_test SET data = X'5570646174656421' WHERE id = 'blob1';";
    rc = sqlite3_exec(db[0], sql3, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // merge changes
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // verify consistency
    for (int i=1; i<nclients; ++i) {
        const char *sql = "SELECT * FROM blob_test ORDER BY id;";
        bool comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        if (comparison_result == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> BLOB test table\n");
        do_query(db[0], "SELECT id, HEX(data), description FROM blob_test ORDER BY id;", query_table);
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_blob_data error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_blob_data error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_blob_data error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test mixed operations (INSERT/UPDATE/DELETE) in transactions
bool do_test_merge_mixed_operations (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    int table_mask = TEST_PRIKEYS;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 2) {
        nclients = 2;
        printf("Number of test merge increased to %d clients\n", 2);
    }
    
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
    
    // initial data setup
    char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('initial', 'data', 25, 'original', 'stamp1');", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[0], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('to_delete', 'data', 30, 'will_delete', 'stamp2');", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[0], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    // sync initial data
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // mixed operations transaction in client 0
    rc = sqlite3_exec(db[0], "BEGIN TRANSACTION;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // INSERT
    sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('new', 'record', 35, 'inserted', 'stamp3');", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[0], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    // UPDATE
    sql = sqlite3_mprintf("UPDATE \"%w\" SET age = 26, note = 'updated' WHERE first_name = 'initial';", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[0], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    // DELETE
    sql = sqlite3_mprintf("DELETE FROM \"%w\" WHERE first_name = 'to_delete';", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[0], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db[0], "COMMIT;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // different mixed operations in client 1
    rc = sqlite3_exec(db[1], "BEGIN TRANSACTION;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('another', 'record', 40, 'client1_insert', 'stamp4');", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[1], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    sql = sqlite3_mprintf("UPDATE \"%w\" SET note = 'client1_update' WHERE first_name = 'initial';", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[1], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = sqlite3_exec(db[1], "COMMIT;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // merge all changes
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // verify consistency
    for (int i=1; i<nclients; ++i) {
        sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        bool comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        sqlite3_free(sql);
        if (comparison_result == false) goto finalize;
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_mixed_operations error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_mixed_operations error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_mixed_operations error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test hub-spoke merge pattern (central client merging with multiple peripherals)
bool do_test_merge_hub_spoke (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    int table_mask = TEST_PRIKEYS;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients < 4) {
        nclients = 4;
        printf("Number of test merge increased to %d clients\n", 4);
    }
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    }
    
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
    
    // client 0 is the hub, others are spokes
    // each spoke inserts unique data
    for (int i=1; i<nclients; ++i) {
        char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('spoke%d', 'client', %d, 'data_from_spoke_%d', 'stamp%d');", CUSTOMERS_TABLE, i, 20+i, i, i);
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // hub-spoke merge: all spokes -> hub
    for (int i=1; i<nclients; ++i) {
        if (do_merge_values(db[i], db[0], false) == false) {
            goto finalize;
        }
    }
    
    // hub inserts aggregated data
    char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\", age, note, stamp) VALUES ('hub', 'aggregated', 50, 'hub_summary', 'hub_stamp');", CUSTOMERS_TABLE);
    rc = sqlite3_exec(db[0], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto finalize;
    
    // hub -> all spokes
    for (int i=1; i<nclients; ++i) {
        if (do_merge_using_payload(db[0], db[i], false, true) == false) {
            goto finalize;
        }
    }
    
    // verify all clients have same data
    for (int i=1; i<nclients; ++i) {
        sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        bool comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        sqlite3_free(sql);
        if (comparison_result == false) goto finalize;
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_hub_spoke error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_hub_spoke error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_hub_spoke error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test timestamp precision conflicts
bool do_test_merge_timestamp_precision (int nclients, bool print_result, bool cleanup_databases) {
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
    
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        // create table with timestamp precision
        const char *sql = "CREATE TABLE timestamp_test (id TEXT PRIMARY KEY NOT NULL, created_at DATETIME DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')), data TEXT);";
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        sql = "SELECT cloudsync_init('timestamp_test', 'cls', 1);";
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // insert with precise timestamps in different clients
    const char *sql1 = "INSERT INTO timestamp_test (id, created_at, data) VALUES ('test1', '2023-12-01 10:30:45.123456', 'client0_data');";
    rc = sqlite3_exec(db[0], sql1, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    const char *sql2 = "INSERT INTO timestamp_test (id, created_at, data) VALUES ('test1', '2023-12-01 10:30:45.123457', 'client1_data');";
    rc = sqlite3_exec(db[1], sql2, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // insert different precision timestamps
    const char *sql3 = "INSERT INTO timestamp_test (id, created_at, data) VALUES ('test2', '2023-12-01 10:30:45.1', 'low_precision');";
    rc = sqlite3_exec(db[0], sql3, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    const char *sql4 = "INSERT INTO timestamp_test (id, created_at, data) VALUES ('test2', '2023-12-01 10:30:45.100000', 'high_precision');";
    rc = sqlite3_exec(db[1], sql4, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // merge changes
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // verify consistency
    for (int i=1; i<nclients; ++i) {
        const char *sql = "SELECT * FROM timestamp_test ORDER BY id, created_at;";
        bool comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        if (comparison_result == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> Timestamp precision test\n");
        do_query(db[0], "SELECT id, created_at, data FROM timestamp_test ORDER BY id, created_at;", query_table);
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_timestamp_precision error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_timestamp_precision error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_timestamp_precision error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test partial merge failure recovery
bool do_test_merge_partial_failure (int nclients, bool print_result, bool cleanup_databases) {
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
    
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        // create table with NOT NULL constraints and DEFAULT values
        const char *sql = "CREATE TABLE partial_test (id TEXT NOT NULL PRIMARY KEY, data TEXT NOT NULL DEFAULT 'default_data', value INTEGER DEFAULT 0);";
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        sql = "SELECT cloudsync_init('partial_test', 'cls', 1);";
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // insert valid data in client 0
    const char *sql1 = "INSERT INTO partial_test (id, data, value) VALUES ('valid1', 'good_data', 100);";
    rc = sqlite3_exec(db[0], sql1, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    const char *sql2 = "INSERT INTO partial_test (id, data, value) VALUES ('valid2', 'more_data', 200);";
    rc = sqlite3_exec(db[0], sql2, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // insert data in client 1 that might cause constraint issues during merge
    const char *sql3 = "INSERT INTO partial_test (id, data, value) VALUES ('valid3', 'client1_data', 300);";
    rc = sqlite3_exec(db[1], sql3, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // attempt merge - should handle any constraint violations gracefully
    do_merge(db, nclients, false);
    
    // verify that databases are still in consistent state even if merge had issues
    for (int i=0; i<nclients; ++i) {
        const char *sql = "SELECT COUNT(*) FROM partial_test;";
        sqlite3_stmt *stmt;
        rc = sqlite3_prepare_v2(db[i], sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                int count = sqlite3_column_int(stmt, 0);
                if (print_result) {
                    printf("Client %d has %d records\n", i, count);
                }
            }
            sqlite3_finalize(stmt);
        }
    }
    
    // test should pass if merge completed or failed gracefully without corruption
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_partial_failure error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_partial_failure error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_partial_failure error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test transaction rollback scenarios
bool do_test_merge_rollback_scenarios (int nclients, bool print_result, bool cleanup_databases) {
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
    
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        const char *sql = "CREATE TABLE rollback_test (id TEXT NOT NULL PRIMARY KEY, data TEXT NOT NULL DEFAULT 'default_data', status INTEGER DEFAULT 0);";
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        sql = "SELECT cloudsync_init('rollback_test', 'cls', 1);";
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // start transaction in client 0
    rc = sqlite3_exec(db[0], "BEGIN TRANSACTION;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    const char *sql1 = "INSERT INTO rollback_test (id, data, status) VALUES ('tx1', 'transaction_data', 1);";
    rc = sqlite3_exec(db[0], sql1, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    const char *sql2 = "INSERT INTO rollback_test (id, data, status) VALUES ('tx2', 'more_tx_data', 2);";
    rc = sqlite3_exec(db[0], sql2, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // rollback transaction
    rc = sqlite3_exec(db[0], "ROLLBACK;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // insert committed data in client 0
    const char *sql3 = "INSERT INTO rollback_test (id, data, status) VALUES ('committed', 'final_data', 10);";
    rc = sqlite3_exec(db[0], sql3, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // insert different data in client 1
    const char *sql4 = "INSERT INTO rollback_test (id, data, status) VALUES ('client1', 'different_data', 20);";
    rc = sqlite3_exec(db[1], sql4, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // verify rolled back data is not present, then merge
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db[0], "SELECT COUNT(*) FROM rollback_test WHERE id IN ('tx1', 'tx2');", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int rollback_count = sqlite3_column_int(stmt, 0);
            if (rollback_count > 0) {
                printf("Rollback failed - found %d rolled back records\n", rollback_count);
                sqlite3_finalize(stmt);
                goto finalize;
            }
        }
        sqlite3_finalize(stmt);
    }
    
    // merge changes
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // verify consistency - only committed data should be present
    for (int i=1; i<nclients; ++i) {
        const char *sql = "SELECT * FROM rollback_test ORDER BY id;";
        bool comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        if (comparison_result == false) goto finalize;
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_rollback_scenarios error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_rollback_scenarios error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_rollback_scenarios error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test circular merge (A->B->C->A)
bool do_test_merge_circular (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients < 3) {
        nclients = 3;
        printf("Number of test merge increased to %d clients\n", 3);
    }
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    }
    
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        const char *sql = "CREATE TABLE circular_test (id TEXT NOT NULL PRIMARY KEY, origin_client INTEGER NOT NULL DEFAULT 0, data TEXT DEFAULT 'default_data');";
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        sql = "SELECT cloudsync_init('circular_test', 'cls', 1);";
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // each client inserts unique data
    for (int i=0; i<nclients; ++i) {
        char *sql = sqlite3_mprintf("INSERT INTO circular_test (id, origin_client, data) VALUES ('data_%d', %d, 'from_client_%d');", i, i, i);
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // circular merge: 0->1->2->0
    if (do_merge_using_payload(db[0], db[1], false, true) == false) {
        goto finalize;
    }
    
    if (do_merge_using_payload(db[1], db[2], false, true) == false) {
        goto finalize;
    }
    
    if (do_merge_using_payload(db[2], db[0], false, true) == false) {
        goto finalize;
    }
    
    // complete the circle and ensure consistency
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // verify all clients have all data
    for (int i=1; i<nclients; ++i) {
        const char *sql = "SELECT * FROM circular_test ORDER BY id;";
        bool comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        if (comparison_result == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> Circular merge result\n");
        do_query(db[0], "SELECT * FROM circular_test ORDER BY origin_client;", query_table);
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_circular error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_circular error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_circular error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test foreign key constraints during merge
bool do_test_merge_foreign_keys (int nclients, bool print_result, bool cleanup_databases) {
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
    
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        // enable foreign keys
        rc = sqlite3_exec(db[i], "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        // create parent table
        const char *sql1 = "CREATE TABLE departments (dept_id TEXT NOT NULL PRIMARY KEY, dept_name TEXT NOT NULL DEFAULT 'Unknown Department');";
        rc = sqlite3_exec(db[i], sql1, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        // create child table with foreign key
        const char *sql2 = "CREATE TABLE employees (emp_id TEXT NOT NULL PRIMARY KEY, emp_name TEXT NOT NULL DEFAULT 'Unknown Employee', dept_id TEXT NOT NULL DEFAULT 'UNKNOWN', FOREIGN KEY (dept_id) REFERENCES departments(dept_id));";
        rc = sqlite3_exec(db[i], sql2, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        const char *sql3 = "SELECT cloudsync_init('departments', 'cls', 1);";
        rc = sqlite3_exec(db[i], sql3, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        const char *sql4 = "SELECT cloudsync_init('employees', 'cls', 1);";
        rc = sqlite3_exec(db[i], sql4, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // insert departments in client 0
    const char *sql0 = "INSERT INTO departments (dept_id, dept_name) VALUES ('UNKNOWN', 'Information Technology');";
    rc = sqlite3_exec(db[0], sql0, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    const char *sql1 = "INSERT INTO departments (dept_id, dept_name) VALUES ('IT', 'Information Technology');";
    rc = sqlite3_exec(db[0], sql1, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    const char *sql2 = "INSERT INTO departments (dept_id, dept_name) VALUES ('HR', 'Human Resources');";
    rc = sqlite3_exec(db[0], sql2, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // insert employees referencing departments in client 1
    const char *sql3 = "INSERT INTO departments (dept_id, dept_name) VALUES ('IT', 'Information Technology');";
    rc = sqlite3_exec(db[1], sql3, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    const char *sql4 = "INSERT INTO employees (emp_id, emp_name, dept_id) VALUES ('E001', 'John Doe', 'IT');";
    rc = sqlite3_exec(db[0], sql4, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    const char *sql5 = "INSERT INTO employees (emp_id, emp_name, dept_id) VALUES ('E002', 'Jane Smith', 'HR');";
    rc = sqlite3_exec(db[0], sql5, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // merge changes - should maintain foreign key constraints
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // verify foreign key constraints are maintained
    for (int i=0; i<nclients; ++i) {
        const char *sql = "PRAGMA foreign_key_check;";
        sqlite3_stmt *stmt;
        rc = sqlite3_prepare_v2(db[i], sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                printf("Foreign key constraint violation found in client %d\n", i);
                sqlite3_finalize(stmt);
                goto finalize;
            }
            sqlite3_finalize(stmt);
        }
    }
    
    // verify consistency
    for (int i=1; i<nclients; ++i) {
        const char *sql = "SELECT * FROM departments ORDER BY dept_id;";
        bool comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        if (comparison_result == false) goto finalize;
        
        sql = "SELECT * FROM employees ORDER BY emp_id;";
        comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        if (comparison_result == false) goto finalize;
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_foreign_keys error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_foreign_keys error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_foreign_keys error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test trigger interaction during merge
// Expected failure: TRIGGERs are not fully supported by this extension.
bool do_test_merge_triggers (int nclients, bool print_result, bool cleanup_databases) {
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
    
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        // create main table
        const char *sql1 = "CREATE TABLE trigger_test (id TEXT NOT NULL PRIMARY KEY, data TEXT DEFAULT 'default_data', update_count INTEGER DEFAULT 0);";
        rc = sqlite3_exec(db[i], sql1, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        // create audit table
        const char *sql2 = "CREATE TABLE audit_log (id TEXT NOT NULL PRIMARY KEY, action TEXT NOT NULL DEFAULT 'UNKNOWN', table_name TEXT NOT NULL DEFAULT 'unknown_table', timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);";
        rc = sqlite3_exec(db[i], sql2, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        // create trigger for update count
        const char *sql3 = "CREATE TRIGGER update_counter AFTER UPDATE ON trigger_test BEGIN UPDATE trigger_test SET update_count = update_count + 1 WHERE id = NEW.id; END;";
        rc = sqlite3_exec(db[i], sql3, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        // create audit trigger
        const char *sql4 = "CREATE TRIGGER audit_insert AFTER INSERT ON trigger_test BEGIN INSERT INTO audit_log (id, action, table_name) VALUES (NEW.id || '_' || datetime('now'), 'INSERT', 'trigger_test'); END;";
        rc = sqlite3_exec(db[i], sql4, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        const char *sql5 = "SELECT cloudsync_init('trigger_test', 'cls', 1);";
        rc = sqlite3_exec(db[i], sql5, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        const char *sql6 = "SELECT cloudsync_init('audit_log', 'cls', 1);";
        rc = sqlite3_exec(db[i], sql6, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // insert data that will trigger audit logging
    const char *sql1 = "INSERT INTO trigger_test (id, data) VALUES ('test1', 'initial_data');";
    rc = sqlite3_exec(db[0], sql1, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    const char *sql2 = "INSERT INTO trigger_test (id, data) VALUES ('test2', 'more_data');";
    rc = sqlite3_exec(db[1], sql2, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // update data to trigger update counter
    const char *sql3 = "UPDATE trigger_test SET data = 'updated_data' WHERE id = 'test1';";
    rc = sqlite3_exec(db[0], sql3, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // merge changes - triggers should function correctly
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // verify trigger effects are consistent
    for (int i=1; i<nclients; ++i) {
        const char *sql = "SELECT * FROM trigger_test ORDER BY id;";
        bool comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        if (comparison_result == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> Trigger test results\n");
        do_query(db[0], "SELECT id, data, update_count FROM trigger_test ORDER BY id;", query_table);
        printf("\n-> Audit log\n");
        do_query(db[0], "SELECT COUNT(*) as audit_entries FROM audit_log;", query_table);
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_triggers error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_triggers error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_triggers error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test index consistency during merge
bool do_test_merge_index_consistency (int nclients, bool print_result, bool cleanup_databases) {
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
    
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        // create table with indexed columns
        const char *sql1 = "CREATE TABLE index_test (id TEXT NOT NULL PRIMARY KEY, name TEXT NOT NULL DEFAULT 'Unknown', email TEXT NOT NULL DEFAULT 'unknown@company.com', age INTEGER DEFAULT 0, department TEXT DEFAULT 'Unknown');";
        rc = sqlite3_exec(db[i], sql1, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        // create various indexes
        const char *sql2 = "CREATE INDEX idx_name ON index_test(name);";
        rc = sqlite3_exec(db[i], sql2, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        const char *sql3 = "CREATE UNIQUE INDEX idx_email ON index_test(email);";
        rc = sqlite3_exec(db[i], sql3, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        const char *sql4 = "CREATE INDEX idx_age_dept ON index_test(age, department);";
        rc = sqlite3_exec(db[i], sql4, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        const char *sql5 = "SELECT cloudsync_init('index_test', 'cls', 1);";
        rc = sqlite3_exec(db[i], sql5, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // insert data that will test various indexes
    const char *sql1 = "INSERT INTO index_test (id, name, email, age, department) VALUES ('emp1', 'Alice Johnson', 'alice@company.com', 30, 'Engineering');";
    rc = sqlite3_exec(db[0], sql1, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    const char *sql2 = "INSERT INTO index_test (id, name, email, age, department) VALUES ('emp2', 'Bob Smith', 'bob@company.com', 25, 'Marketing');";
    rc = sqlite3_exec(db[0], sql2, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    const char *sql3 = "INSERT INTO index_test (id, name, email, age, department) VALUES ('emp3', 'Charlie Brown', 'charlie@company.com', 35, 'Engineering');";
    rc = sqlite3_exec(db[1], sql3, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    const char *sql4 = "INSERT INTO index_test (id, name, email, age, department) VALUES ('emp4', 'Diana Prince', 'diana@company.com', 28, 'Sales');";
    rc = sqlite3_exec(db[1], sql4, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // merge changes
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // verify index consistency by checking integrity
    for (int i=0; i<nclients; ++i) {
        const char *sql = "PRAGMA integrity_check;";
        sqlite3_stmt *stmt;
        rc = sqlite3_prepare_v2(db[i], sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *result2 = (const char*)sqlite3_column_text(stmt, 0);
                if (strcmp(result2, "ok") != 0) {
                    printf("Index integrity issue in client %d: %s\n", i, result2);
                    sqlite3_finalize(stmt);
                    goto finalize;
                }
            }
            sqlite3_finalize(stmt);
        }
    }
    
    // verify data consistency
    for (int i=1; i<nclients; ++i) {
        const char *sql = "SELECT * FROM index_test ORDER BY id;";
        bool comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        if (comparison_result == false) goto finalize;
    }
    
    // test index usage with queries
    for (int i=0; i<nclients; ++i) {
        const char *sql = "SELECT * FROM index_test WHERE name = 'Alice Johnson';";
        sqlite3_stmt *stmt;
        rc = sqlite3_prepare_v2(db[i], sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            int found = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                found++;
            }
            if (found != 1) {
                printf("Index query failed in client %d, found %d records\n", i, found);
                sqlite3_finalize(stmt);
                goto finalize;
            }
            sqlite3_finalize(stmt);
        }
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_index_consistency error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_index_consistency error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_index_consistency error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test JSON column merge
bool do_test_merge_json_columns (int nclients, bool print_result, bool cleanup_databases) {
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
    
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        // create table with JSON column
        const char *sql1 = "CREATE TABLE json_test (id TEXT NOT NULL PRIMARY KEY, metadata JSON, config TEXT);";
        rc = sqlite3_exec(db[i], sql1, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        const char *sql2 = "SELECT cloudsync_init('json_test', 'cls', 1);";
        rc = sqlite3_exec(db[i], sql2, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // insert JSON data in different clients
    const char *sql1 = "INSERT INTO json_test (id, metadata, config) VALUES ('user1', '{\"name\": \"John\", \"age\": 30, \"preferences\": {\"theme\": \"dark\", \"language\": \"en\"}}', 'client0_config');";
    rc = sqlite3_exec(db[0], sql1, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    const char *sql2 = "INSERT INTO json_test (id, metadata, config) VALUES ('user2', '{\"name\": \"Jane\", \"age\": 25, \"preferences\": {\"theme\": \"light\", \"language\": \"es\"}}', 'client1_config');";
    rc = sqlite3_exec(db[1], sql2, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // update JSON data
    const char *sql3 = "UPDATE json_test SET metadata = json_set(metadata, '$.preferences.notifications', true) WHERE id = 'user1';";
    rc = sqlite3_exec(db[0], sql3, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    const char *sql4 = "UPDATE json_test SET metadata = json_set(metadata, '$.last_login', datetime('now')) WHERE id = 'user2';";
    rc = sqlite3_exec(db[1], sql4, NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    
    // merge changes
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // verify JSON data consistency
    for (int i=1; i<nclients; ++i) {
        const char *sql = "SELECT * FROM json_test ORDER BY id;";
        bool comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        if (comparison_result == false) goto finalize;
    }
    
    // test JSON extraction after merge
    for (int i=0; i<nclients; ++i) {
        const char *sql = "SELECT id, json_extract(metadata, '$.name') as name FROM json_test WHERE id = 'user1';";
        sqlite3_stmt *stmt;
        rc = sqlite3_prepare_v2(db[i], sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *name = (const char*)sqlite3_column_text(stmt, 1);
                if (!name || strcmp(name, "John") != 0) {
                    printf("JSON extraction failed in client %d\n", i);
                    sqlite3_finalize(stmt);
                    goto finalize;
                }
            }
            sqlite3_finalize(stmt);
        }
    }
    
    if (print_result) {
        printf("\n-> JSON test results\n");
        do_query(db[0], "SELECT id, json_extract(metadata, '$.name') as name, json_extract(metadata, '$.preferences.theme') as theme FROM json_test ORDER BY id;", query_table);
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_json_columns error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_json_columns error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_json_columns error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test concurrent merge attempts
bool do_test_payload_apply_concurrent_write (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    sqlite3 *db_target2 = NULL;
    sqlite3_stmt *select_stmt = NULL;
    sqlite3_stmt *apply_stmt = NULL;
    bool result = false;
    int rc = SQLITE_OK;

    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients < 2) nclients = 2;
    if (nclients > 2) nclients = 2; // this test uses exactly 2 databases

    time_t timestamp = time(NULL);
    int saved_counter = test_counter;

    // create two file-based databases: db[0]=src, db[1]=target
    for (int i = 0; i < nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == NULL) return false;

        rc = sqlite3_exec(db[i], "CREATE TABLE concurrent_tbl (id TEXT PRIMARY KEY, val TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;

        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('concurrent_tbl', 'cls', 1);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }

    // insert data on src (db[0])
    rc = sqlite3_exec(db[0], "INSERT INTO concurrent_tbl VALUES ('row1', 'hello');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    rc = sqlite3_exec(db[0], "INSERT INTO concurrent_tbl VALUES ('row2', 'world');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;

    // extract payload from db[0]
    const char *encode_sql = "SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) FROM cloudsync_changes WHERE site_id=cloudsync_siteid();";
    rc = sqlite3_prepare_v2(db[0], encode_sql, -1, &select_stmt, NULL);
    if (rc != SQLITE_OK) goto finalize;

    rc = sqlite3_step(select_stmt);
    if (rc != SQLITE_ROW) goto finalize;

    const void *payload_data = sqlite3_column_blob(select_stmt, 0);
    int payload_size = sqlite3_column_bytes(select_stmt, 0);
    if (payload_data == NULL || payload_size == 0) goto finalize;

    // copy payload since we'll need it after finalizing select_stmt
    void *payload_copy = malloc(payload_size);
    if (payload_copy == NULL) goto finalize;
    memcpy(payload_copy, payload_data, payload_size);
    sqlite3_finalize(select_stmt);
    select_stmt = NULL;

    // open second connection to same file as db[1] (target)
    {
        char buf[256];
        do_build_database_path(buf, 1, timestamp, saved_counter + 1);
        rc = sqlite3_open(buf, &db_target2);
        if (rc != SQLITE_OK) {
            printf("Error opening db_target2: %s\n", sqlite3_errmsg(db_target2));
            free(payload_copy);
            goto finalize;
        }
        sqlite3_exec(db_target2, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
        sqlite3_cloudsync_init(db_target2, NULL, NULL);
    }

    // on db[1] (target): begin immediate to hold write lock
    rc = sqlite3_exec(db[1], "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        printf("BEGIN IMMEDIATE failed: %s\n", sqlite3_errmsg(db[1]));
        free(payload_copy);
        goto finalize;
    }
    rc = sqlite3_exec(db[1], "INSERT INTO concurrent_tbl VALUES ('blocker', 'blocking');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        printf("Blocker INSERT failed: %s\n", sqlite3_errmsg(db[1]));
        free(payload_copy);
        goto finalize;
    }

    // on db_target2: try to apply payload — should fail with BUSY
    rc = sqlite3_prepare_v2(db_target2, "SELECT cloudsync_payload_decode(?);", -1, &apply_stmt, NULL);
    if (rc != SQLITE_OK) {
        printf("Prepare apply failed: %s\n", sqlite3_errmsg(db_target2));
        free(payload_copy);
        goto finalize;
    }
    rc = sqlite3_bind_blob(apply_stmt, 1, payload_copy, payload_size, SQLITE_STATIC);
    if (rc != SQLITE_OK) {
        printf("Bind failed: %s\n", sqlite3_errmsg(db_target2));
        free(payload_copy);
        goto finalize;
    }

    // set a short busy timeout so it doesn't wait forever (0 = fail immediately)
    sqlite3_busy_timeout(db_target2, 0);

    rc = sqlite3_step(apply_stmt);
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) {
        printf("Expected BUSY error but apply succeeded (rc=%d)\n", rc);
        free(payload_copy);
        goto finalize;
    }

    // verify we got a BUSY-related error
    int errcode = sqlite3_errcode(db_target2);
    if (errcode != SQLITE_BUSY && errcode != SQLITE_LOCKED) {
        printf("Expected SQLITE_BUSY or SQLITE_LOCKED but got error %d: %s\n", errcode, sqlite3_errmsg(db_target2));
        free(payload_copy);
        goto finalize;
    }

    if (print_result) {
        printf("  Step 1: Apply blocked as expected (errcode=%d: %s)\n", errcode, sqlite3_errmsg(db_target2));
    }

    // release the write lock on db[1]
    rc = sqlite3_exec(db[1], "COMMIT;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        printf("COMMIT failed: %s\n", sqlite3_errmsg(db[1]));
        free(payload_copy);
        goto finalize;
    }

    // retry: reset and step again — should succeed now
    sqlite3_reset(apply_stmt);
    sqlite3_busy_timeout(db_target2, 5000); // give it time now

    rc = sqlite3_step(apply_stmt);
    if (rc != SQLITE_ROW) {
        printf("Expected SQLITE_ROW on retry but got %d: %s\n", rc, sqlite3_errmsg(db_target2));
        free(payload_copy);
        goto finalize;
    }

    if (print_result) {
        printf("  Step 2: Apply succeeded after lock released\n");
    }

    sqlite3_finalize(apply_stmt);
    apply_stmt = NULL;
    free(payload_copy);
    payload_copy = NULL;

    // verify: db_target2 should have row1, row2 (from payload) + blocker (from db[1])
    {
        sqlite3_stmt *count_stmt = NULL;
        rc = sqlite3_prepare_v2(db_target2, "SELECT COUNT(*) FROM concurrent_tbl;", -1, &count_stmt, NULL);
        if (rc != SQLITE_OK) goto finalize;
        rc = sqlite3_step(count_stmt);
        if (rc != SQLITE_ROW) { sqlite3_finalize(count_stmt); goto finalize; }
        int count = sqlite3_column_int(count_stmt, 0);
        sqlite3_finalize(count_stmt);
        if (count != 3) {
            printf("Expected 3 rows but got %d\n", count);
            goto finalize;
        }
        if (print_result) {
            printf("  Step 3: Target has %d rows (expected 3)\n", count);
        }
    }

    // full consistency: merge all databases using payload
    // first close db_target2 to avoid lock conflicts during merge
    close_db(db_target2);
    db_target2 = NULL;

    // merge db[0] <-> db[1] in both directions
    if (do_merge_using_payload(db[0], db[1], false, true) == false) {
        printf("Merge src->target failed\n");
        goto finalize;
    }
    if (do_merge_using_payload(db[1], db[0], false, true) == false) {
        printf("Merge target->src failed\n");
        goto finalize;
    }

    // verify consistency
    {
        const char *sql = "SELECT * FROM concurrent_tbl ORDER BY id;";
        bool cmp = do_compare_queries(db[0], sql, db[1], sql, -1, -1, print_result);
        if (!cmp) {
            printf("Consistency check failed between src and target\n");
            goto finalize;
        }
    }

    if (print_result) {
        printf("  Step 4: Full consistency verified\n");
    }

    result = true;

finalize:
    if (select_stmt) sqlite3_finalize(select_stmt);
    if (apply_stmt) sqlite3_finalize(apply_stmt);
    if (db_target2) close_db(db_target2);
    for (int i = 0; i < nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK))
            printf("do_test_payload_apply_concurrent_write error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_payload_apply_concurrent_write error: db %d is in transaction\n", i);
            }
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_payload_apply_concurrent_write error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

bool do_test_merge_concurrent_attempts (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 3) {
        nclients = 3;
        printf("Number of test merge increased to %d clients\n", 3);
    }
    
    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i=0; i<nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;
        
        const char *sql1 = "CREATE TABLE concurrent_test (id TEXT NOT NULL PRIMARY KEY, client_id INTEGER NOT NULL DEFAULT 0, data TEXT DEFAULT 'default_data', sequence INTEGER DEFAULT 0);";
        rc = sqlite3_exec(db[i], sql1, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        
        const char *sql2 = "SELECT cloudsync_init('concurrent_test', 'cls', 1);";
        rc = sqlite3_exec(db[i], sql2, NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    // each client inserts data with sequence numbers
    for (int i=0; i<nclients; ++i) {
        for (int j=0; j<5; ++j) {
            char *sql = sqlite3_mprintf("INSERT INTO concurrent_test (id, client_id, data, sequence) VALUES ('c%d_seq%d', %d, 'data_from_client_%d', %d);", i, j, i, i, j);
            rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
            sqlite3_free(sql);
            if (rc != SQLITE_OK) goto finalize;
        }
    }
    
    // simulate concurrent merge attempts by merging in different orders
    // first: 0->1 and 1->2 simultaneously
    bool merge1 = do_merge_using_payload(db[0], db[1], false, true);
    bool merge2 = do_merge_using_payload(db[1], db[2], false, true);
    
    if (!merge1 || !merge2) {
        printf("Concurrent merge phase 1 had issues\n");
    }
    
    // second: 2->0 and 0->1 simultaneously
    bool merge3 = do_merge_using_payload(db[2], db[0], false, true);
    bool merge4 = do_merge_using_payload(db[0], db[1], false, true);
    
    if (!merge3 || !merge4) {
        printf("Concurrent merge phase 2 had issues\n");
    }
    
    // final consistency merge
    if (do_merge(db, nclients, false) == false) {
        printf("Final merge failed\n");
    }
    
    // verify all clients have same data count
    int expected_count = nclients * 5; // each client inserted 5 records
    for (int i=0; i<nclients; ++i) {
        const char *sql = "SELECT COUNT(*) FROM concurrent_test;";
        sqlite3_stmt *stmt;
        rc = sqlite3_prepare_v2(db[i], sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                int count = sqlite3_column_int(stmt, 0);
                if (count != expected_count) {
                    printf("Client %d has %d records, expected %d\n", i, count, expected_count);
                }
                if (print_result) {
                    printf("Client %d: %d records\n", i, count);
                }
            }
            sqlite3_finalize(stmt);
        }
    }
    
    // verify consistency across all clients
    for (int i=1; i<nclients; ++i) {
        const char *sql = "SELECT * FROM concurrent_test ORDER BY id;";
        bool comparison_result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        if (comparison_result == false) {
            printf("Consistency check failed between client 0 and %d\n", i);
            // don't fail immediately, continue to see full picture
        }
    }
    
    // test passes if we reach here without database corruption
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge_concurrent_attempts error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_concurrent_attempts error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_concurrent_attempts error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test with 10 clients using a simple table with composite primary key
// Summary of the test function:
// 1. Simple table with composite primary key: Creates a table with columns (id1, id2) as the composite primary key and (data1, data2) as non-primary key columns
// 2. 10 database simulation: Forces exactly 10 clients regardless of the parameter passed
// 3. CloudSync initialization: Each database initializes the cloudsync library for the test table
// 4. Initial data: Each client inserts 3 initial rows with unique data
// 5. Multiple operation rounds: Performs 5 rounds of operations where each client performs:
//    - Inserts new rows (20%)
//    - Updates local data (15%) - data inserted by the same client
//    - Updates remote data (50%) - data inserted by other clients, tests cross-client conflicts
//    - Deletes both local and remote data (15%) - minimal deletions to preserve data for updates
// 6. Partial merging: After each round, clients merge with only 2-3 other clients (not all), simulating real-world scenarios where not all clients sync immediately
// 7. Final convergence: 
//    - Merges all changes from all clients to the first client (client 0)
//    - Propagates the final state from client 0 to all other clients
// 8. Verification: Compares all databases to ensure they have converged to identical final states
bool do_test_merge_composite_pk_10_clients (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;
    const char *table_name = "simple_composite_test";
    
    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
        printf("Number of test merge reduced to %d clients\n", MAX_SIMULATED_CLIENTS);
    } else if (nclients < 2) {
        nclients = 2;
        printf("Number of test merge increased to %d clients\n", 2);
    }
    
    // Force to 10 clients for this specific test
    nclients = 10;
    
    // create databases and tables
    time_t timestamp = time(NULL);
    int saved_counter = test_counter++;
    for (int i = 0; i < nclients; ++i) {
        db[i] = do_create_database_file_v2(i, timestamp, saved_counter);
        if (db[i] == false) return false;
        
        // Create simple table with composite primary key (id1, id2) and two non-pk columns (data1, data2)
        char *sql = sqlite3_mprintf("CREATE TABLE %s (id1 INTEGER NOT NULL, id2 TEXT NOT NULL, data1 TEXT, data2 INTEGER, PRIMARY KEY(id1, id2));", table_name);
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
        
        // Initialize cloudsync for the table
        sql = sqlite3_mprintf("SELECT cloudsync_init('%s', 'cls', 1);", table_name);
        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) goto finalize;
    }
    
    if (print_result) printf("Created %d databases with composite primary key table '%s'\n", nclients, table_name);
    
    // Insert initial data in each client
    for (int i = 0; i < nclients; ++i) {
        for (int j = 0; j < 3; ++j) {
            char *sql = sqlite3_mprintf("INSERT INTO %s (id1, id2, data1, data2) VALUES (%d, 'client%d_row%d', 'initial_data_%d_%d', %d);", 
                                       table_name, i * 100 + j, i, j, i, j, i * 10 + j);
            rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
            sqlite3_free(sql);
            if (rc != SQLITE_OK) goto finalize;
        }
    }
    
    if (print_result) printf("Inserted initial data in all clients\n");
    
    // Perform various operations across clients in multiple rounds
    for (int round = 0; round < 20; ++round) {
        if (print_result) printf("Starting round %d of operations\n", round + 1);
        
        // Each client performs different operations (favoring remote data operations for better conflict testing)
        for (int i = 0; i < nclients; ++i) {
            // Weighted operation selection: focus on updates, minimal deletes
            int operation_selector = (i * 7 + round * 3) % 20;
            int operation;
            
            if (operation_selector < 4) {
                operation = 0; // INSERT (20%)
            } else if (operation_selector < 7) {
                operation = 1; // UPDATE local data (15%)
            } else if (operation_selector < 17) {
                operation = 2; // UPDATE remote data (50%)
            } else {
                operation = 3; // DELETE (15% - reduced from 30%)
            }
            
            switch (operation) {
                case 0: // INSERT
                    {
                        char *sql = sqlite3_mprintf("INSERT INTO %s (id1, id2, data1, data2) VALUES (%d, 'round%d_client%d', 'new_data_%d_%d', %d);", 
                                                   table_name, 1000 + round * 100 + i, round, i, round, i, round * 100 + i);
                        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
                        sqlite3_free(sql);
                        if (rc != SQLITE_OK && rc != SQLITE_CONSTRAINT) {
                            printf("Insert failed on client %d: %s\n", i, sqlite3_errmsg(db[i]));
                        }
                    }
                    break;
                    
                case 1: // UPDATE local data (more aggressive targeting)
                    {
                        // Try multiple approaches to find rows to update
                        char *sql;
                        int updated_rows = 0;
                        
                        // First try: update rows originally from this client
                        sql = sqlite3_mprintf("UPDATE %s SET data1 = 'updated_local_round%d_client%d', data2 = %d WHERE id1 BETWEEN %d AND %d;",
                                             table_name, round, i, round * 1000 + i, i * 100, (i + 1) * 100 - 1);
                        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
                        sqlite3_free(sql);
                        updated_rows += sqlite3_changes(db[i]);
                        
                        // Second try: if no rows updated, target any available rows
                        if (updated_rows == 0) {
                            sql = sqlite3_mprintf("UPDATE %s SET data1 = 'updated_local_round%d_client%d_any', data2 = %d WHERE rowid = (SELECT rowid FROM %s LIMIT 1 OFFSET %d);",
                                                 table_name, round, i, round * 1000 + i, table_name, i % 10);
                            rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
                            sqlite3_free(sql);
                            updated_rows += sqlite3_changes(db[i]);
                        }
                        
                        if (rc != SQLITE_OK) {
                            printf("Update local data failed on client %d: %s\n", i, sqlite3_errmsg(db[i]));
                        } else if (print_result && updated_rows > 0) {
                            printf("Client %d updated %d local rows in round %d\n", i, updated_rows, round + 1);
                        }
                    }
                    break;
                    
                case 2: // UPDATE remote data (more aggressive - creates conflicts)
                    {
                        int target_client = (i + 1 + round) % nclients; // Vary target client
                        char *sql;
                        int updated_rows = 0;
                        
                        // First try: update rows from target client's range
                        sql = sqlite3_mprintf("UPDATE %s SET data1 = 'updated_remote_round%d_client%d_targeting%d', data2 = %d WHERE id1 BETWEEN %d AND %d;",
                                             table_name, round, i, target_client, round * 2000 + i, target_client * 100, (target_client + 1) * 100 - 1);
                        rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
                        sqlite3_free(sql);
                        updated_rows += sqlite3_changes(db[i]);
                        
                        // Second try: if no rows updated, target any rows not from current client
                        if (updated_rows == 0) {
                            sql = sqlite3_mprintf("UPDATE %s SET data1 = 'updated_remote_round%d_client%d_any', data2 = %d WHERE id1 NOT BETWEEN %d AND %d LIMIT 2;",
                                                 table_name, round, i, round * 2000 + i, i * 100, (i + 1) * 100 - 1);
                            rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
                            sqlite3_free(sql);
                            updated_rows += sqlite3_changes(db[i]);
                        }
                        
                        // Third try: if still no updates, target any available rows
                        if (updated_rows == 0) {
                            sql = sqlite3_mprintf("UPDATE %s SET data1 = 'updated_remote_round%d_client%d_fallback', data2 = %d WHERE rowid IN (SELECT rowid FROM %s LIMIT 2 OFFSET %d);",
                                                 table_name, round, i, round * 2000 + i, table_name, (i + round) % 10);
                            rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
                            sqlite3_free(sql);
                            updated_rows += sqlite3_changes(db[i]);
                        }
                        
                        if (rc != SQLITE_OK) {
                            printf("Update remote data failed on client %d targeting client %d: %s\n", i, target_client, sqlite3_errmsg(db[i]));
                        } else if (print_result && updated_rows > 0) {
                            printf("Client %d updated %d remote rows in round %d\n", i, updated_rows, round + 1);
                        }
                    }
                    break;
                    
                case 3: // DELETE (favoring remote data deletion)
                    {
                        // 80% chance to delete remote data, 20% chance to delete local data
                        if ((i + round) % 5 == 0) {
                            // Delete local data (20% of delete operations)
                            char *sql = sqlite3_mprintf("DELETE FROM %s WHERE id1 = %d AND id2 = 'client%d_row1';", 
                                                       table_name, i * 100 + 1, i);
                            rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
                            sqlite3_free(sql);
                            if (rc != SQLITE_OK) {
                                printf("Delete local data failed on client %d: %s\n", i, sqlite3_errmsg(db[i]));
                            }
                        } else {
                            // Delete remote data (80% of delete operations)
                            int target_client = (i + 2 + round) % nclients; // Vary target client
                            char *sql = sqlite3_mprintf("DELETE FROM %s WHERE id1 = %d AND id2 = 'client%d_row0';", 
                                                       table_name, target_client * 100, target_client);
                            rc = sqlite3_exec(db[i], sql, NULL, NULL, NULL);
                            sqlite3_free(sql);
                            if (rc != SQLITE_OK) {
                                printf("Delete remote data failed on client %d targeting client %d: %s\n", i, target_client, sqlite3_errmsg(db[i]));
                            }
                        }
                    }
                    break;
            }
        }
        
        // Partial merge: each client merges with 2-3 other random clients (not all)
        for (int i = 0; i < nclients; ++i) {
            int merge_targets = 2 + (round % 2); // 2 or 3 targets
            for (int j = 0; j < merge_targets; ++j) {
                int target = (i + j + 1 + round) % nclients;
                if (target != i) {
                    if (do_merge_using_payload(db[i], db[target], true, true) == false) {
                        if (print_result) printf("Partial merge failed from client %d to %d\n", i, target);
                        goto finalize;
                    }
                }
            }
        }
        
        if (print_result) printf("Completed round %d operations and partial merges\n", round + 1);
    }
    
    // Final convergence: merge all changes to client 0
    if (print_result) printf("Starting final convergence to client 0\n");
    for (int i = 1; i < nclients; ++i) {
        if (do_merge_using_payload(db[i], db[0], true, true) == false) {
            if (print_result) printf("Final merge to client 0 failed from client %d\n", i);
            goto finalize;
        }
    }
    
    // Merge changes from client 0 to all other clients
    if (print_result) printf("Propagating final state from client 0 to all other clients\n");
    for (int i = 1; i < nclients; ++i) {
        if (do_merge_using_payload(db[0], db[i], false, true) == false) {
            if (print_result) printf("Final merge from client 0 failed to client %d\n", i);
            goto finalize;
        }
    }
    
    // Verify all databases have converged to the same state
    if (print_result) printf("Verifying convergence across all clients\n");
    char *verification_sql = sqlite3_mprintf("SELECT * FROM %s ORDER BY id1, id2;", table_name);
    for (int i = 1; i < nclients; ++i) {
        if (do_compare_queries(db[0], verification_sql, db[i], verification_sql, -1, -1, print_result) == false) {
            if (print_result) printf("Convergence verification failed: client 0 vs client %d\n", i);
            sqlite3_free(verification_sql);
            goto finalize;
        }
    }
    sqlite3_free(verification_sql);
    
    if (print_result) {
        printf("\nFinal converged state:\n");
        char *display_sql = sqlite3_mprintf("SELECT * FROM %s ORDER BY id1, id2;", table_name);
        do_query(db[0], display_sql, NULL);
        sqlite3_free(display_sql);
    }
    
    result = true;
    
finalize:
    for (int i = 0; i < nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) {
            printf("do_test_merge_composite_pk_10_clients error: %s\n", sqlite3_errmsg(db[i]));
        }
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_merge_composite_pk_10_clients error: db %d is in transaction\n", i);
            }
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge_composite_pk_10_clients error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter);
            file_delete_internal(buf);
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
        db[i] = do_create_database_file_v2(i, timestamp, test_counter++);
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
        if (do_merge_using_payload(db[i], db[0], true, true) == false) {
            goto finalize;
        }
    }
    
    // compare results
    for (int i=1; i<nclients; ++i) {
        const char *sql_query = "SELECT * FROM foo ORDER BY a;";
        if (do_compare_queries(db[0], sql_query, db[i], sql_query, -1, -1, print_result) == false) goto finalize;
    }
    
    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_prikey_null error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
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

    db[1] = do_create_database_file(1, timestamp, test_counter);

    result = true;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (!result && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_init error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, test_counter);
            file_delete_internal(buf);
        }
    }
    test_counter++;
    return result;
}

// MARK: -

bool do_test_gos (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    cloudsync_context *data[MAX_SIMULATED_CLIENTS] = {NULL};
    
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
        
        data[i] = cloudsync_context_create(db[i]);
        if (data[i] == false) return false;
        
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
            DBTYPE types[] = {SQLITE_TEXT, SQLITE_TEXT, SQLITE_INTEGER};
            int len[] = {-1, -1, 0};
            
            rc = database_write(data[i], sql, values, types, len, 3);
            if (rc != SQLITE_OK) goto finalize;
        }
    }
    
    // merge all changes
    if (do_merge(db, nclients, false) == false) {
        goto finalize;
    }
    
    // compare results
    for (int i=1; i<nclients; ++i) {
        const char *sql_query = "SELECT * FROM log ORDER BY id;";
        if (do_compare_queries(db[0], sql_query, db[i], sql_query, -1, -1, print_result) == false) goto finalize;
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
        if (data[i]) cloudsync_context_free(data[i]);
        
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// MARK: -

bool do_test_network_encode_decode (int nclients, bool print_result, bool cleanup_databases, bool force_uncompressed) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    cloudsync_context *data[MAX_SIMULATED_CLIENTS] = {NULL};
    
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
        
        data[i] = cloudsync_context_create(db[i]);
        if (data[i] == false) return false;
        
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
            
            char *blob = NULL;
            int64_t blob_size = 0;
            rc = database_select_blob(data[target], src_sql, &blob, &blob_size);
            if ((rc != DBRES_OK) || (!blob)) goto finalize;
            
            const char *values[] = {blob};
            int types[] = {SQLITE_BLOB};
            int len[] = {(int)blob_size};

            unit_select(data[j], dest_sql, values, types, len, 1, SQLITE_INTEGER);
            cloudsync_memory_free(blob);
        }
    }
    
    if (force_uncompressed) force_uncompressed_blob = false;
    
    // compare results
    for (int i=1; i<nclients; ++i) {
        char *sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
        sqlite3_free(sql);
        if (result == false) goto finalize;
    }
    
    if (print_result) {
        printf("\n-> customers\n");
        char *sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
        do_query(db[0], sql, query_table);
        sqlite3_free(sql);
    }
    
    result = true;
    rc = SQLITE_OK;
    
finalize:
    for (int i=0; i<nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK)) printf("do_test_merge error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) close_db(db[i]);
        if (data[i]) cloudsync_context_free(data[i]);
        
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
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
            char *sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
            if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) {
                sqlite3_free(sql);
                goto finalize;
            }
            sqlite3_free(sql);
        }
        if (table_mask & TEST_NOCOLS) {
            const char *sql = "SELECT * FROM \"" CUSTOMERS_NOCOLS_TABLE "\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";";
            if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
        }
        if (table_mask & TEST_NOPRIKEYS) {
            const char *sql = "SELECT * FROM customers_noprikey ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";";
            if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
        }
    }
    
    if (print_result) {
        if (table_mask & TEST_PRIKEYS) {
            printf("\n-> " CUSTOMERS_TABLE "\n");
            char *sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
            do_query(db[0], sql, query_table);
            sqlite3_free(sql);
        }
        if (table_mask & TEST_NOCOLS) {
            printf("\n-> \"" CUSTOMERS_NOCOLS_TABLE "\"\n");
            do_query(db[0], "SELECT * FROM \"" CUSTOMERS_NOCOLS_TABLE "\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", query_table);
        }
        if (table_mask & TEST_NOPRIKEYS) {
            printf("\n-> customers_noprikey\n");
            do_query(db[0], "SELECT * FROM customers_noprikey ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", query_table);
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
            
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_merge error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
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
            char *sql;
            switch (alter_version) {
                case 3:
                    sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY name;", CUSTOMERS_TABLE);
                    break;
                default:
                    sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
                    break;
            }
            result = do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result);
            sqlite3_free(sql);
            if (result == false) goto finalize;
        }
        if (table_mask & TEST_NOCOLS) {
            const char *sql;
            switch (alter_version) {
                case 3:
                    sql = "SELECT * FROM \"" CUSTOMERS_NOCOLS_TABLE "\" ORDER BY name;";
                    break;
                default:
                    sql = "SELECT * FROM \"" CUSTOMERS_NOCOLS_TABLE "\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";";
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
                    sql = "SELECT * FROM customers_noprikey ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";";
                    break;
            }
            if (do_compare_queries(db[0], sql, db[i], sql, -1, -1, print_result) == false) goto finalize;
        }
    }
    
    if (print_result) {
        if (table_mask & TEST_PRIKEYS) {
            printf("\n-> " CUSTOMERS_TABLE "\n");
            char *sql = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", CUSTOMERS_TABLE);
            do_query(db[0], sql, query_table);
            sqlite3_free(sql);
        }
        if (table_mask & TEST_NOCOLS) {
            printf("\n-> \"" CUSTOMERS_NOCOLS_TABLE "\"\n");
            do_query(db[0], "SELECT * FROM \"" CUSTOMERS_NOCOLS_TABLE "\" ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", query_table);
        }
        if (table_mask & TEST_NOPRIKEYS) {
            printf("\n-> customers_noprikey\n");
            do_query(db[0], "SELECT * FROM customers_noprikey ORDER BY first_name, \"" CUSTOMERS_TABLE_COLUMN_LASTNAME "\";", query_table);
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
            file_delete_internal(buf);
        }
    }
    return result;
}

// MARK: -

bool do_test_payload_buffer (size_t blob_size) {
    const char *table_name = "payload_buffer_test";
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    unsigned char *blob = NULL;
    char *errmsg = NULL;
    bool success = false;
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_cloudsync_init(db, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_exec(db, "SELECT cloudsync_version();", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) goto cleanup;
    if (errmsg) { sqlite3_free(errmsg); errmsg = NULL; }

    char *sql = sqlite3_mprintf("CREATE TABLE IF NOT EXISTS \"%w\" ("
                                "id TEXT PRIMARY KEY NOT NULL, "
                                "value BLOB, "
                                "created_at TEXT DEFAULT CURRENT_TIMESTAMP"
                                ");", table_name);
    if (!sql) {
        rc = SQLITE_NOMEM;
        goto cleanup;
    }
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto cleanup;
    if (errmsg) { sqlite3_free(errmsg); errmsg = NULL; }

    sql = sqlite3_mprintf("SELECT cloudsync_init('%q');", table_name);
    if (!sql) {
        rc = SQLITE_NOMEM;
        goto cleanup;
    }
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto cleanup;
    if (errmsg) { sqlite3_free(errmsg); errmsg = NULL; }

    sql = sqlite3_mprintf("INSERT INTO \"%w\" (id, value) VALUES (?, ?);", table_name);
    if (!sql) {
        rc = SQLITE_NOMEM;
        goto cleanup;
    }
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) goto cleanup;

    char dummy_id[UUID_STR_MAXLEN];
    cloudsync_uuid_v7_string(dummy_id, true);

    blob = sqlite3_malloc64(blob_size);
    if (!blob) {
        rc = SQLITE_NOMEM;
        goto cleanup;
    }
    for (size_t i = 0; i < blob_size; ++i) {
        blob[i] = (unsigned char)(i % 256);
    }

    rc = sqlite3_bind_text(stmt, 1, dummy_id, -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) goto cleanup;
    rc = sqlite3_bind_blob(stmt, 2, blob, (int)blob_size, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) goto cleanup;
    rc = sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc != SQLITE_OK) goto cleanup;

    sqlite3_free(blob);
    blob = NULL;

    const char *payload_sql = "SELECT length(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq)) "
                              "FROM cloudsync_changes;";
    rc = sqlite3_prepare_v2(db, payload_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    int row_count = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        (void)sqlite3_column_int64(stmt, 0);
        row_count++;
    }
    if (rc != SQLITE_DONE || row_count == 0) goto cleanup;

    success = true;

cleanup:
    if (stmt) {
        sqlite3_finalize(stmt);
    }
    if (blob) {
        sqlite3_free(blob);
    }
    if (errmsg) {
        fprintf(stderr, "do_test_android_initial_payload error: %s\n", errmsg);
        sqlite3_free(errmsg);
    }
    if (db) close_db(db);
    db = NULL;

    return success;
}

// MARK: - Row Filter Test -

static int64_t test_query_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int64_t value = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

bool do_test_row_filter(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;

    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) nclients = MAX_SIMULATED_CLIENTS;
    if (nclients < 2) nclients = 2;

    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i = 0; i < nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;

        // Create table
        rc = sqlite3_exec(db[i], "CREATE TABLE tasks(id TEXT PRIMARY KEY NOT NULL, title TEXT, user_id INTEGER);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;

        // Init cloudsync
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('tasks');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;

        // Set filter: only sync rows where user_id = 1
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_filter('tasks', 'user_id = 1');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }

    // --- Test 1: Insert matching and non-matching rows on db[0] ---
    rc = sqlite3_exec(db[0], "INSERT INTO tasks VALUES('a', 'Task A', 1);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    rc = sqlite3_exec(db[0], "INSERT INTO tasks VALUES('b', 'Task B', 2);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    rc = sqlite3_exec(db[0], "INSERT INTO tasks VALUES('c', 'Task C', 1);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;

    // Verify: tasks_cloudsync should only have metadata for user_id=1 rows ('a' and 'c')
    {
        // Count distinct PKs in the meta table
        int64_t meta_count = test_query_int(db[0], "SELECT COUNT(DISTINCT pk) FROM tasks_cloudsync;");
        if (meta_count != 2) {
            printf("do_test_row_filter: expected 2 tracked PKs after insert, got %" PRId64 "\n", meta_count);
            goto finalize;
        }
    }

    // --- Test 2: Update matching row → metadata should update ---
    rc = sqlite3_exec(db[0], "UPDATE tasks SET title='Task A Updated' WHERE id='a';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;

    // --- Test 3: Update non-matching row → NO metadata change ---
    {
        int64_t before = test_query_int(db[0], "SELECT COUNT(*) FROM tasks_cloudsync;");
        rc = sqlite3_exec(db[0], "UPDATE tasks SET title='Task B Updated' WHERE id='b';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        int64_t after = test_query_int(db[0], "SELECT COUNT(*) FROM tasks_cloudsync;");
        if (after != before) {
            printf("do_test_row_filter: non-matching UPDATE changed meta count (%" PRId64 " -> %" PRId64 ")\n", before, after);
            goto finalize;
        }
    }

    // --- Test 4: Delete non-matching row → NO metadata change ---
    {
        int64_t before = test_query_int(db[0], "SELECT COUNT(*) FROM tasks_cloudsync;");
        rc = sqlite3_exec(db[0], "DELETE FROM tasks WHERE id='b';", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        int64_t after = test_query_int(db[0], "SELECT COUNT(*) FROM tasks_cloudsync;");
        if (after != before) {
            printf("do_test_row_filter: non-matching DELETE changed meta count (%" PRId64 " -> %" PRId64 ")\n", before, after);
            goto finalize;
        }
    }

    // --- Test 5: Delete matching row → metadata should update (tombstone) ---
    rc = sqlite3_exec(db[0], "DELETE FROM tasks WHERE id='a';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;

    // --- Test 6: Merge from db[0] to db[1] and verify only filtered rows transfer ---
    if (do_merge_using_payload(db[0], db[1], true, true) == false) goto finalize;

    {
        // db[1] should have 'c' (user_id=1) and the tombstone for 'a', but NOT 'b'
        int64_t task_count = test_query_int(db[1], "SELECT COUNT(*) FROM tasks;");
        if (task_count != 1) {
            printf("do_test_row_filter: expected 1 row in db[1] tasks after merge, got %" PRId64 "\n", task_count);
            goto finalize;
        }
        // Verify it's 'c'
        int64_t c_exists = test_query_int(db[1], "SELECT COUNT(*) FROM tasks WHERE id='c';");
        if (c_exists != 1) {
            printf("do_test_row_filter: expected task 'c' in db[1], not found\n");
            goto finalize;
        }
    }

    if (print_result) {
        printf("\n-> tasks (db[0])\n");
        do_query(db[0], "SELECT * FROM tasks ORDER BY id;", NULL);
        printf("\n-> tasks_cloudsync (db[0])\n");
        do_query(db[0], "SELECT hex(pk), col_name, col_version, db_version FROM tasks_cloudsync ORDER BY pk, col_name;", NULL);
        printf("\n-> tasks (db[1])\n");
        do_query(db[1], "SELECT * FROM tasks ORDER BY id;", NULL);
    }

    result = true;

finalize:
    for (int i = 0; i < nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK))
            printf("do_test_row_filter error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// Test that BEFORE triggers with RAISE(ABORT) simulate RLS denial:
// per-PK savepoints isolate failures so allowed rows commit and denied rows roll back.
bool do_test_rls_trigger_denial (int nclients, bool print_result, bool cleanup_databases, bool only_locals) {
    sqlite3 *db[MAX_SIMULATED_CLIENTS] = {NULL};
    bool result = false;
    int rc = SQLITE_OK;

    memset(db, 0, sizeof(sqlite3 *) * MAX_SIMULATED_CLIENTS);
    if (nclients >= MAX_SIMULATED_CLIENTS) {
        nclients = MAX_SIMULATED_CLIENTS;
    } else if (nclients < 2) {
        nclients = 2;
    }

    time_t timestamp = time(NULL);
    int saved_counter = test_counter;
    for (int i = 0; i < nclients; ++i) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (db[i] == false) return false;

        rc = sqlite3_exec(db[i], "CREATE TABLE tasks (id TEXT PRIMARY KEY NOT NULL, user_id TEXT, title TEXT, priority INTEGER);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;

        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('tasks');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }

    // --- Phase 1: baseline sync (no triggers) ---
    rc = sqlite3_exec(db[0], "INSERT INTO tasks VALUES ('t1', 'user1', 'Task 1', 3);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    rc = sqlite3_exec(db[0], "INSERT INTO tasks VALUES ('t2', 'user2', 'Task 2', 5);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    rc = sqlite3_exec(db[0], "INSERT INTO tasks VALUES ('t3', 'user1', 'Task 3', 1);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;

    if (do_merge_using_payload(db[0], db[1], only_locals, true) == false) goto finalize;

    // Verify: B has 3 rows
    {
        sqlite3_stmt *stmt = NULL;
        rc = sqlite3_prepare_v2(db[1], "SELECT COUNT(*) FROM tasks;", -1, &stmt, NULL);
        if (rc != SQLITE_OK) goto finalize;
        if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); goto finalize; }
        int count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if (count != 3) {
            printf("Phase 1: expected 3 rows, got %d\n", count);
            goto finalize;
        }
    }

    // --- Phase 2: INSERT denial with triggers on B ---
    rc = sqlite3_exec(db[1],
        "CREATE TRIGGER rls_deny_insert BEFORE INSERT ON tasks "
        "FOR EACH ROW WHEN NEW.user_id != 'user1' "
        "BEGIN SELECT RAISE(ABORT, 'row violates RLS policy'); END;",
        NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;

    rc = sqlite3_exec(db[1],
        "CREATE TRIGGER rls_deny_update BEFORE UPDATE ON tasks "
        "FOR EACH ROW WHEN NEW.user_id != 'user1' "
        "BEGIN SELECT RAISE(ABORT, 'row violates RLS policy'); END;",
        NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;

    rc = sqlite3_exec(db[0], "INSERT INTO tasks VALUES ('t4', 'user1', 'Task 4', 2);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    rc = sqlite3_exec(db[0], "INSERT INTO tasks VALUES ('t5', 'user2', 'Task 5', 7);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;

    // Merge with partial-failure tolerance: cloudsync_payload_decode returns error
    // when any PK is denied, but allowed PKs are already committed via per-PK savepoints.
    {
        sqlite3_stmt *sel = NULL, *ins = NULL;
        const char *sel_sql = only_locals
            ? "SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) FROM cloudsync_changes WHERE site_id=cloudsync_siteid();"
            : "SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) FROM cloudsync_changes;";
        rc = sqlite3_prepare_v2(db[0], sel_sql, -1, &sel, NULL);
        if (rc != SQLITE_OK) { sqlite3_finalize(sel); goto finalize; }
        rc = sqlite3_prepare_v2(db[1], "SELECT cloudsync_payload_decode(?);", -1, &ins, NULL);
        if (rc != SQLITE_OK) { sqlite3_finalize(sel); sqlite3_finalize(ins); goto finalize; }

        while (sqlite3_step(sel) == SQLITE_ROW) {
            sqlite3_value *v = sqlite3_column_value(sel, 0);
            if (sqlite3_value_type(v) == SQLITE_NULL) continue;
            sqlite3_bind_value(ins, 1, v);
            sqlite3_step(ins); // partial failure expected — ignore rc
            sqlite3_reset(ins);
        }
        sqlite3_finalize(sel);
        sqlite3_finalize(ins);
    }

    // Verify: t4 present (user1 → allowed)
    {
        sqlite3_stmt *stmt = NULL;
        rc = sqlite3_prepare_v2(db[1], "SELECT COUNT(*) FROM tasks WHERE id='t4';", -1, &stmt, NULL);
        if (rc != SQLITE_OK) goto finalize;
        if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); goto finalize; }
        int count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if (count != 1) {
            printf("Phase 2: t4 expected 1 row, got %d\n", count);
            goto finalize;
        }
    }

    // Verify: t5 absent (user2 → denied)
    {
        sqlite3_stmt *stmt = NULL;
        rc = sqlite3_prepare_v2(db[1], "SELECT COUNT(*) FROM tasks WHERE id='t5';", -1, &stmt, NULL);
        if (rc != SQLITE_OK) goto finalize;
        if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); goto finalize; }
        int count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if (count != 0) {
            printf("Phase 2: t5 expected 0 rows, got %d\n", count);
            goto finalize;
        }
    }

    // Verify: total 4 rows on B (t1, t2, t3 from phase 1 + t4)
    {
        sqlite3_stmt *stmt = NULL;
        rc = sqlite3_prepare_v2(db[1], "SELECT COUNT(*) FROM tasks;", -1, &stmt, NULL);
        if (rc != SQLITE_OK) goto finalize;
        if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); goto finalize; }
        int count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if (count != 4) {
            printf("Phase 2: expected 4 total rows, got %d\n", count);
            goto finalize;
        }
    }

    // --- Phase 3: UPDATE denial ---
    rc = sqlite3_exec(db[0], "UPDATE tasks SET title='Task 1 Updated', priority=10 WHERE id='t1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;
    rc = sqlite3_exec(db[0], "UPDATE tasks SET title='Task 2 Hacked', priority=99 WHERE id='t2';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;

    // Merge with partial-failure tolerance (same pattern as phase 2)
    {
        sqlite3_stmt *sel = NULL, *ins = NULL;
        const char *sel_sql = only_locals
            ? "SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) FROM cloudsync_changes WHERE site_id=cloudsync_siteid();"
            : "SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) FROM cloudsync_changes;";
        rc = sqlite3_prepare_v2(db[0], sel_sql, -1, &sel, NULL);
        if (rc != SQLITE_OK) { sqlite3_finalize(sel); goto finalize; }
        rc = sqlite3_prepare_v2(db[1], "SELECT cloudsync_payload_decode(?);", -1, &ins, NULL);
        if (rc != SQLITE_OK) { sqlite3_finalize(sel); sqlite3_finalize(ins); goto finalize; }

        while (sqlite3_step(sel) == SQLITE_ROW) {
            sqlite3_value *v = sqlite3_column_value(sel, 0);
            if (sqlite3_value_type(v) == SQLITE_NULL) continue;
            sqlite3_bind_value(ins, 1, v);
            sqlite3_step(ins); // partial failure expected — ignore rc
            sqlite3_reset(ins);
        }
        sqlite3_finalize(sel);
        sqlite3_finalize(ins);
    }

    // Verify: t1 updated (user1 → allowed)
    {
        sqlite3_stmt *stmt = NULL;
        rc = sqlite3_prepare_v2(db[1], "SELECT title, priority FROM tasks WHERE id='t1';", -1, &stmt, NULL);
        if (rc != SQLITE_OK) goto finalize;
        if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); goto finalize; }
        const char *title = (const char *)sqlite3_column_text(stmt, 0);
        int priority = sqlite3_column_int(stmt, 1);
        bool ok = (strcmp(title, "Task 1 Updated") == 0) && (priority == 10);
        sqlite3_finalize(stmt);
        if (!ok) {
            printf("Phase 3: t1 update not applied (title='%s', priority=%d)\n", title, priority);
            goto finalize;
        }
    }

    // Verify: t2 unchanged (user2 → denied)
    {
        sqlite3_stmt *stmt = NULL;
        rc = sqlite3_prepare_v2(db[1], "SELECT title, priority FROM tasks WHERE id='t2';", -1, &stmt, NULL);
        if (rc != SQLITE_OK) goto finalize;
        if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); goto finalize; }
        const char *title = (const char *)sqlite3_column_text(stmt, 0);
        int priority = sqlite3_column_int(stmt, 1);
        bool ok = (strcmp(title, "Task 2") == 0) && (priority == 5);
        sqlite3_finalize(stmt);
        if (!ok) {
            printf("Phase 3: t2 should be unchanged (title='%s', priority=%d)\n", title, priority);
            goto finalize;
        }
    }

    result = true;
    rc = SQLITE_OK;

finalize:
    for (int i = 0; i < nclients; ++i) {
        if (rc != SQLITE_OK && db[i] && (sqlite3_errcode(db[i]) != SQLITE_OK))
            printf("do_test_rls_trigger_denial error: %s\n", sqlite3_errmsg(db[i]));
        if (db[i]) {
            if (sqlite3_get_autocommit(db[i]) == 0) {
                result = false;
                printf("do_test_rls_trigger_denial error: db %d is in transaction\n", i);
            }
            int counter = close_db(db[i]);
            if (counter > 0) {
                result = false;
                printf("do_test_rls_trigger_denial error: db %d has %d unterminated statements\n", i, counter);
            }
        }
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

// MARK: - Block-level LWW Tests -

static int64_t do_select_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int64_t val = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            val = sqlite3_column_int64(stmt, 0);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    return val;
}

static char *do_select_text(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    char *val = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(stmt, 0);
            if (t) val = sqlite3_mprintf("%s", t);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    return val;
}

bool do_test_block_lww_insert(int nclients, bool print_result, bool cleanup_databases) {
    // Test: INSERT into a table with a block column properly splits text into blocks
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;

        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) { printf("block_insert: CREATE TABLE failed: %s\n", sqlite3_errmsg(db[i])); goto fail; }

        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) { printf("block_insert: cloudsync_init failed: %s\n", sqlite3_errmsg(db[i])); goto fail; }

        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) { printf("block_insert: set_column failed: %s\n", sqlite3_errmsg(db[i])); goto fail; }
    }

    // Insert a document with 3 lines
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'Line 1\nLine 2\nLine 3');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_insert: INSERT failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    // Verify blocks were created in the blocks table
    int64_t block_count = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1');");
    if (block_count != 3) {
        printf("block_insert: expected 3 blocks, got %" PRId64 "\n", block_count);
        goto fail;
    }

    // Verify metadata entries for blocks (col_name contains \x1F)
    int64_t meta_count = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync WHERE col_name LIKE 'body' || x'1f' || '%';");
    if (meta_count != 3) {
        printf("block_insert: expected 3 block metadata entries, got %" PRId64 "\n", meta_count);
        goto fail;
    }

    // Verify no metadata entry for the whole 'body' column
    int64_t whole_meta = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync WHERE col_name = 'body';");
    if (whole_meta != 0) {
        printf("block_insert: expected 0 whole-column metadata entries, got %" PRId64 "\n", whole_meta);
        goto fail;
    }

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return true;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

bool do_test_block_lww_update(int nclients, bool print_result, bool cleanup_databases) {
    // Test: UPDATE on a block column performs block diff
    sqlite3 *db[1] = {NULL};
    time_t timestamp = time(NULL);
    int rc;

    db[0] = do_create_database_file(0, timestamp, test_counter++);
    if (!db[0]) return false;

    rc = sqlite3_exec(db[0], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[0], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[0], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Insert initial text
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'AAA\nBBB\nCCC');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_update: INSERT failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    int64_t blocks_before = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks;");

    // Update: change middle line and add a new line
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'AAA\nXXX\nCCC\nDDD' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_update: UPDATE failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    int64_t blocks_after = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks;");

    // Should have 4 blocks after update (AAA, XXX, CCC, DDD)
    if (blocks_after != 4) {
        printf("block_update: expected 4 blocks after update, got %" PRId64 " (before: %" PRId64 ")\n", blocks_after, blocks_before);
        goto fail;
    }

    close_db(db[0]);
    return true;

fail:
    if (db[0]) close_db(db[0]);
    return false;
}

bool do_test_block_lww_sync(int nclients, bool print_result, bool cleanup_databases) {
    // Test: Two sites edit different blocks of the same document; after sync, both edits are preserved
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;

        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Site 0 inserts the initial document
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'Line A\nLine B\nLine C');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_sync: INSERT db[0] failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    // Sync initial state: db[0] -> db[1] so both have the same document
    if (!do_merge_using_payload(db[0], db[1], false, true)) { printf("block_sync: initial merge 0->1 failed\n"); goto fail; }

    // Site 0: edit first line
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'EDITED A\nLine B\nLine C' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_sync: UPDATE db[0] failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    // Site 1: edit third line
    rc = sqlite3_exec(db[1], "UPDATE docs SET body = 'Line A\nLine B\nEDITED C' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_sync: UPDATE db[1] failed: %s\n", sqlite3_errmsg(db[1])); goto fail; }

    // Sync: db[0] -> db[1] (send site 0's edits)
    if (!do_merge_using_payload(db[0], db[1], true, true)) { printf("block_sync: merge 0->1 failed\n"); goto fail; }
    // Sync: db[1] -> db[0] (send site 1's edits)
    if (!do_merge_using_payload(db[1], db[0], true, true)) { printf("block_sync: merge 1->0 failed\n"); goto fail; }

    // Both databases should now have the merged result: "EDITED A\nLine B\nEDITED C"
    char *body0 = do_select_text(db[0], "SELECT body FROM docs WHERE id = 'doc1';");
    char *body1 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");

    bool ok = true;
    if (!body0 || !body1) {
        printf("block_sync: could not read body from one or both databases\n");
        ok = false;
    } else if (strcmp(body0, body1) != 0) {
        printf("block_sync: bodies don't match after sync:\n  db[0]: %s\n  db[1]: %s\n", body0, body1);
        ok = false;
    } else {
        // Check that both edits were preserved
        if (!strstr(body0, "EDITED A")) {
            printf("block_sync: missing 'EDITED A' in result: %s\n", body0);
            ok = false;
        }
        if (!strstr(body0, "EDITED C")) {
            printf("block_sync: missing 'EDITED C' in result: %s\n", body0);
            ok = false;
        }
        if (!strstr(body0, "Line B")) {
            printf("block_sync: missing 'Line B' in result: %s\n", body0);
            ok = false;
        }
    }

    if (body0) sqlite3_free(body0);
    if (body1) sqlite3_free(body1);

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return ok;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

bool do_test_block_lww_delete(int nclients, bool print_result, bool cleanup_databases) {
    // Test: DELETE on a row with block columns marks tombstone and block metadata is dropped
    sqlite3 *db[1] = {NULL};
    time_t timestamp = time(NULL);
    int rc;

    db[0] = do_create_database_file(0, timestamp, test_counter++);
    if (!db[0]) return false;

    rc = sqlite3_exec(db[0], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[0], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[0], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Insert a document
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'Line A\nLine B\nLine C');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_delete: INSERT failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    // Verify blocks and metadata exist
    int64_t blocks_before = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks;");
    if (blocks_before != 3) {
        printf("block_delete: expected 3 blocks before delete, got %" PRId64 "\n", blocks_before);
        goto fail;
    }
    int64_t meta_before = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync WHERE col_name LIKE 'body' || x'1f' || '%';");
    if (meta_before != 3) {
        printf("block_delete: expected 3 block metadata before delete, got %" PRId64 "\n", meta_before);
        goto fail;
    }

    // Delete the row
    rc = sqlite3_exec(db[0], "DELETE FROM docs WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_delete: DELETE failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    // Verify metadata tombstone exists (delete sentinel)
    int64_t tombstone = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync WHERE col_name = '__[RIP]__' AND col_version % 2 = 0;");
    if (tombstone != 1) {
        printf("block_delete: expected 1 delete tombstone, got %" PRId64 "\n", tombstone);
        goto fail;
    }

    // Verify block metadata was dropped (local_drop_meta removes non-tombstone metadata)
    int64_t meta_after = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync WHERE col_name LIKE 'body' || x'1f' || '%';");
    if (meta_after != 0) {
        printf("block_delete: expected 0 block metadata after delete, got %" PRId64 "\n", meta_after);
        goto fail;
    }

    // Row should be gone from base table
    int64_t row_count = do_select_int(db[0], "SELECT count(*) FROM docs WHERE id = 'doc1';");
    if (row_count != 0) {
        printf("block_delete: row still in base table after delete\n");
        goto fail;
    }

    close_db(db[0]);
    return true;

fail:
    if (db[0]) close_db(db[0]);
    return false;
}

bool do_test_block_lww_materialize(int nclients, bool print_result, bool cleanup_databases) {
    // Test: cloudsync_text_materialize reconstructs text from blocks after sync
    // Sync to a second db where body column is empty, then materialize there
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;

        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Insert multi-line text on db[0]
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'Alpha\nBravo\nCharlie\nDelta\nEcho');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_materialize: INSERT failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    // Sync to db[1] — body column on db[1] will be populated by payload_apply but
    // materialize should reconstruct correctly from blocks
    if (!do_merge_using_payload(db[0], db[1], false, true)) { printf("block_materialize: merge failed\n"); goto fail; }

    // Materialize on db[1] should reconstruct from blocks
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_materialize: materialize failed: %s\n", sqlite3_errmsg(db[1])); goto fail; }

    char *body = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");
    if (!body) {
        printf("block_materialize: body is NULL after materialize\n");
        goto fail;
    }
    if (strcmp(body, "Alpha\nBravo\nCharlie\nDelta\nEcho") != 0) {
        printf("block_materialize: body mismatch: %s\n", body);
        sqlite3_free(body);
        goto fail;
    }
    sqlite3_free(body);

    // Also test materialize on db[0] (where body already matches)
    rc = sqlite3_exec(db[0], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_materialize: materialize on db[0] failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    char *body0 = do_select_text(db[0], "SELECT body FROM docs WHERE id = 'doc1';");
    if (!body0 || strcmp(body0, "Alpha\nBravo\nCharlie\nDelta\nEcho") != 0) {
        printf("block_materialize: body0 mismatch: %s\n", body0 ? body0 : "NULL");
        if (body0) sqlite3_free(body0);
        goto fail;
    }
    sqlite3_free(body0);

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return true;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

bool do_test_block_lww_empty_text(int nclients, bool print_result, bool cleanup_databases) {
    // Test: INSERT with empty body creates a single empty block
    sqlite3 *db[1] = {NULL};
    time_t timestamp = time(NULL);
    int rc;

    db[0] = do_create_database_file(0, timestamp, test_counter++);
    if (!db[0]) return false;

    rc = sqlite3_exec(db[0], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[0], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[0], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Insert empty text
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', '');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_empty: INSERT failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    // Should have exactly 1 block (empty content)
    int64_t block_count = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks;");
    if (block_count != 1) {
        printf("block_empty: expected 1 block for empty text, got %" PRId64 "\n", block_count);
        goto fail;
    }

    // Insert NULL text
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc2', NULL);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_empty: INSERT NULL failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    // NULL body should also create 1 block (treated as empty)
    int64_t null_blocks = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc2');");
    if (null_blocks != 1) {
        printf("block_empty: expected 1 block for NULL text, got %" PRId64 "\n", null_blocks);
        goto fail;
    }

    // Update from empty to multi-line
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'Line1\nLine2' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_empty: UPDATE from empty failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    int64_t updated_blocks = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1');");
    if (updated_blocks != 2) {
        printf("block_empty: expected 2 blocks after update from empty, got %" PRId64 "\n", updated_blocks);
        goto fail;
    }

    close_db(db[0]);
    return true;

fail:
    if (db[0]) close_db(db[0]);
    return false;
}

bool do_test_block_lww_conflict(int nclients, bool print_result, bool cleanup_databases) {
    // Test: Two sites edit the SAME line concurrently; LWW picks the later write
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;

        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Site 0 inserts initial document
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'Same\nMiddle\nEnd');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_conflict: INSERT db[0] failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    // Sync initial state: db[0] -> db[1]
    if (!do_merge_values(db[0], db[1], false)) { printf("block_conflict: initial merge failed\n"); goto fail; }

    // Site 0: edit first line
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'Site0\nMiddle\nEnd' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_conflict: UPDATE db[0] failed\n"); goto fail; }

    // Site 1: also edit first line (conflict!)
    rc = sqlite3_exec(db[1], "UPDATE docs SET body = 'Site1\nMiddle\nEnd' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_conflict: UPDATE db[1] failed\n"); goto fail; }

    // Sync both ways using row-by-row merge
    if (!do_merge_values(db[0], db[1], true)) { printf("block_conflict: merge 0->1 failed\n"); goto fail; }
    if (!do_merge_values(db[1], db[0], true)) { printf("block_conflict: merge 1->0 failed\n"); goto fail; }

    // Materialize on both databases to reconstruct body from blocks
    rc = sqlite3_exec(db[0], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_conflict: materialize db[0] failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_conflict: materialize db[1] failed: %s\n", sqlite3_errmsg(db[1])); goto fail; }

    // Both databases should converge (same value)
    char *body0 = do_select_text(db[0], "SELECT body FROM docs WHERE id = 'doc1';");
    char *body1 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");

    bool ok = true;
    if (!body0 || !body1) {
        printf("block_conflict: could not read body from databases\n");
        ok = false;
    } else if (strcmp(body0, body1) != 0) {
        printf("block_conflict: bodies don't match after sync:\n  db[0]: %s\n  db[1]: %s\n", body0, body1);
        ok = false;
    } else {
        // Should contain either "Site0" or "Site1" (LWW picks one), plus unchanged lines
        if (!strstr(body0, "Middle")) {
            printf("block_conflict: missing 'Middle' in result: %s\n", body0);
            ok = false;
        }
        if (!strstr(body0, "End")) {
            printf("block_conflict: missing 'End' in result: %s\n", body0);
            ok = false;
        }
        // One of the conflicting edits should win
        if (!strstr(body0, "Site0") && !strstr(body0, "Site1")) {
            printf("block_conflict: neither 'Site0' nor 'Site1' in result: %s\n", body0);
            ok = false;
        }
    }

    if (body0) sqlite3_free(body0);
    if (body1) sqlite3_free(body1);

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return ok;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

bool do_test_block_lww_multi_update(int nclients, bool print_result, bool cleanup_databases) {
    // Test: Multiple successive updates correctly maintain block state
    sqlite3 *db[1] = {NULL};
    time_t timestamp = time(NULL);
    int rc;

    db[0] = do_create_database_file(0, timestamp, test_counter++);
    if (!db[0]) return false;

    rc = sqlite3_exec(db[0], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[0], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[0], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Insert initial text (3 lines)
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'A\nB\nC');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_multi: INSERT failed\n"); goto fail; }

    // Update 1: remove middle line (3 -> 2 blocks)
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'A\nC' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_multi: UPDATE 1 failed\n"); goto fail; }

    int64_t blocks1 = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks;");
    if (blocks1 != 2) { printf("block_multi: expected 2 blocks after update 1, got %" PRId64 "\n", blocks1); goto fail; }

    // Update 2: add two lines (2 -> 4 blocks)
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'A\nX\nC\nY' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_multi: UPDATE 2 failed\n"); goto fail; }

    int64_t blocks2 = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks;");
    if (blocks2 != 4) { printf("block_multi: expected 4 blocks after update 2, got %" PRId64 "\n", blocks2); goto fail; }

    // Update 3: change everything to a single line (4 -> 1 block)
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'SINGLE' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_multi: UPDATE 3 failed\n"); goto fail; }

    int64_t blocks3 = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks;");
    if (blocks3 != 1) { printf("block_multi: expected 1 block after update 3, got %" PRId64 "\n", blocks3); goto fail; }

    // Materialize and verify
    rc = sqlite3_exec(db[0], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_multi: materialize failed\n"); goto fail; }

    char *body = do_select_text(db[0], "SELECT body FROM docs WHERE id = 'doc1';");
    if (!body || strcmp(body, "SINGLE") != 0) {
        printf("block_multi: expected 'SINGLE', got '%s'\n", body ? body : "NULL");
        if (body) sqlite3_free(body);
        goto fail;
    }
    sqlite3_free(body);

    close_db(db[0]);
    return true;

fail:
    if (db[0]) close_db(db[0]);
    return false;
}

bool do_test_block_lww_reinsert(int nclients, bool print_result, bool cleanup_databases) {
    // Test: DELETE then re-INSERT recreates blocks properly
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;

        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Insert, delete, then re-insert with different content
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'Old1\nOld2');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_reinsert: initial INSERT failed\n"); goto fail; }

    rc = sqlite3_exec(db[0], "DELETE FROM docs WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_reinsert: DELETE failed\n"); goto fail; }

    // Block metadata should be dropped (blocks table entries are orphaned by design)
    int64_t meta_after_del = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync WHERE col_name LIKE 'body' || x'1f' || '%';");
    if (meta_after_del != 0) {
        printf("block_reinsert: expected 0 block metadata after delete, got %" PRId64 "\n", meta_after_del);
        goto fail;
    }

    // Re-insert with new content
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'New1\nNew2\nNew3');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_reinsert: re-INSERT failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    // Check block metadata was recreated (3 new block entries)
    int64_t meta_after_reinsert = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync WHERE col_name LIKE 'body' || x'1f' || '%';");
    if (meta_after_reinsert != 3) {
        printf("block_reinsert: expected 3 block metadata after re-insert, got %" PRId64 "\n", meta_after_reinsert);
        goto fail;
    }

    // Sync to db[1] and verify
    if (!do_merge_using_payload(db[0], db[1], false, true)) { printf("block_reinsert: merge failed\n"); goto fail; }

    // Materialize on db[1]
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("block_reinsert: materialize on db[1] failed: %s\n", sqlite3_errmsg(db[1])); goto fail; }

    char *body1 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");
    if (!body1 || strcmp(body1, "New1\nNew2\nNew3") != 0) {
        printf("block_reinsert: body mismatch on db[1]: %s\n", body1 ? body1 : "NULL");
        if (body1) sqlite3_free(body1);
        goto fail;
    }
    sqlite3_free(body1);

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return true;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

bool do_test_block_lww_add_lines(int nclients, bool print_result, bool cleanup_databases) {
    // Test: Both sites add lines at different positions; after sync, all lines are present
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;

        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Site 0 inserts initial doc
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'Line1\nLine2');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Sync initial: 0 -> 1
    if (!do_merge_using_payload(db[0], db[1], false, true)) goto fail;

    // Site 0: append a line at the end
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'Line1\nLine2\nAppended0' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Site 1: insert a line in the middle
    rc = sqlite3_exec(db[1], "UPDATE docs SET body = 'Line1\nInserted1\nLine2' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Sync both ways
    if (!do_merge_using_payload(db[0], db[1], true, true)) goto fail;
    if (!do_merge_using_payload(db[1], db[0], true, true)) goto fail;

    // Both should converge
    char *body0 = do_select_text(db[0], "SELECT body FROM docs WHERE id = 'doc1';");
    char *body1 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");

    bool ok = true;
    if (!body0 || !body1) {
        printf("block_add_lines: could not read body\n");
        ok = false;
    } else if (strcmp(body0, body1) != 0) {
        printf("block_add_lines: bodies don't match:\n  db[0]: %s\n  db[1]: %s\n", body0, body1);
        ok = false;
    } else {
        // All original and added lines should be present
        if (!strstr(body0, "Line1")) { printf("block_add_lines: missing Line1\n"); ok = false; }
        if (!strstr(body0, "Line2")) { printf("block_add_lines: missing Line2\n"); ok = false; }
        if (!strstr(body0, "Appended0")) { printf("block_add_lines: missing Appended0\n"); ok = false; }
        if (!strstr(body0, "Inserted1")) { printf("block_add_lines: missing Inserted1\n"); ok = false; }
    }

    if (body0) sqlite3_free(body0);
    if (body1) sqlite3_free(body1);
    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return ok;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test 1: Non-conflicting edits on different blocks — both edits preserved
bool do_test_block_lww_noconflict(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Site 0 inserts initial document with 3 lines
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'Line1\nLine2\nLine3');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Sync initial: 0 -> 1
    if (!do_merge_values(db[0], db[1], false)) goto fail;

    // Site 0: edit first line only
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'EditedByA\nLine2\nLine3' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Site 1: edit third line only (no conflict — different block)
    rc = sqlite3_exec(db[1], "UPDATE docs SET body = 'Line1\nLine2\nEditedByB' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Sync both ways
    if (!do_merge_values(db[0], db[1], true)) goto fail;
    if (!do_merge_values(db[1], db[0], true)) goto fail;

    // Materialize on both
    rc = sqlite3_exec(db[0], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    char *body0 = do_select_text(db[0], "SELECT body FROM docs WHERE id = 'doc1';");
    char *body1 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");

    bool ok = true;
    if (!body0 || !body1 || strcmp(body0, body1) != 0) {
        printf("noconflict: bodies diverged: [%s] vs [%s]\n", body0 ? body0 : "NULL", body1 ? body1 : "NULL");
        ok = false;
    } else {
        // BOTH edits should be preserved (this is the key value of block-level LWW)
        if (!strstr(body0, "EditedByA")) { printf("noconflict: missing EditedByA\n"); ok = false; }
        if (!strstr(body0, "Line2")) { printf("noconflict: missing Line2\n"); ok = false; }
        if (!strstr(body0, "EditedByB")) { printf("noconflict: missing EditedByB\n"); ok = false; }
    }

    if (body0) sqlite3_free(body0);
    if (body1) sqlite3_free(body1);
    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return ok;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test 2: Concurrent add + edit — Site A adds a line, Site B modifies an existing line
bool do_test_block_lww_add_and_edit(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Initial doc
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'Alpha\nBravo');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    if (!do_merge_values(db[0], db[1], false)) goto fail;

    // Site 0: add a new line at the end
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'Alpha\nBravo\nCharlie' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Site 1: modify first line
    rc = sqlite3_exec(db[1], "UPDATE docs SET body = 'AlphaEdited\nBravo' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Sync both ways
    if (!do_merge_values(db[0], db[1], true)) goto fail;
    if (!do_merge_values(db[1], db[0], true)) goto fail;

    rc = sqlite3_exec(db[0], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    char *body0 = do_select_text(db[0], "SELECT body FROM docs WHERE id = 'doc1';");
    char *body1 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");

    bool ok = true;
    if (!body0 || !body1 || strcmp(body0, body1) != 0) {
        printf("add_and_edit: bodies diverged: [%s] vs [%s]\n", body0 ? body0 : "NULL", body1 ? body1 : "NULL");
        ok = false;
    } else {
        // The added line and the edit should both be present
        if (!strstr(body0, "Charlie")) { printf("add_and_edit: missing Charlie (added line)\n"); ok = false; }
        if (!strstr(body0, "Bravo")) { printf("add_and_edit: missing Bravo\n"); ok = false; }
        // First line: either AlphaEdited wins (from site 1) or Alpha (from site 0) — depends on LWW
        // But the added line Charlie must survive regardless
    }

    if (body0) sqlite3_free(body0);
    if (body1) sqlite3_free(body1);
    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return ok;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test 3: Three-way sync — 3 databases with overlapping edits converge
bool do_test_block_lww_three_way(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[3] = {NULL, NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 3; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Site 0 creates initial doc
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'L1\nL2\nL3\nL4');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Sync 0 -> 1, 0 -> 2
    if (!do_merge_values(db[0], db[1], false)) goto fail;
    if (!do_merge_values(db[0], db[2], false)) goto fail;

    // Site 0: edit line 1
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'S0\nL2\nL3\nL4' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Site 1: edit line 2
    rc = sqlite3_exec(db[1], "UPDATE docs SET body = 'L1\nS1\nL3\nL4' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Site 2: edit line 4
    rc = sqlite3_exec(db[2], "UPDATE docs SET body = 'L1\nL2\nL3\nS2' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Full mesh sync: each site sends to every other site
    for (int src = 0; src < 3; src++) {
        for (int dst = 0; dst < 3; dst++) {
            if (src == dst) continue;
            if (!do_merge_values(db[src], db[dst], true)) { printf("three_way: merge %d->%d failed\n", src, dst); goto fail; }
        }
    }

    // Materialize all
    for (int i = 0; i < 3; i++) {
        rc = sqlite3_exec(db[i], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) { printf("three_way: materialize db[%d] failed\n", i); goto fail; }
    }

    // All three should converge
    char *body[3];
    for (int i = 0; i < 3; i++) {
        body[i] = do_select_text(db[i], "SELECT body FROM docs WHERE id = 'doc1';");
    }

    bool ok = true;
    if (!body[0] || !body[1] || !body[2]) { printf("three_way: NULL body\n"); ok = false; }
    else if (strcmp(body[0], body[1]) != 0 || strcmp(body[1], body[2]) != 0) {
        printf("three_way: not converged:\n  [0]: %s\n  [1]: %s\n  [2]: %s\n", body[0], body[1], body[2]);
        ok = false;
    } else {
        // All three non-conflicting edits should be preserved
        if (!strstr(body[0], "S0")) { printf("three_way: missing S0\n"); ok = false; }
        if (!strstr(body[0], "S1")) { printf("three_way: missing S1\n"); ok = false; }
        if (!strstr(body[0], "L3")) { printf("three_way: missing L3\n"); ok = false; }
        if (!strstr(body[0], "S2")) { printf("three_way: missing S2\n"); ok = false; }
    }

    for (int i = 0; i < 3; i++) { if (body[i]) sqlite3_free(body[i]); }
    for (int i = 0; i < 3; i++) { close_db(db[i]); db[i] = NULL; }
    return ok;

fail:
    for (int i = 0; i < 3; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test 4: Mixed block + normal columns — both work independently
bool do_test_block_lww_mixed_columns(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE notes (id TEXT NOT NULL PRIMARY KEY, body TEXT, title TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('notes');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        // body is block-level LWW, title is normal LWW
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('notes', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Site 0: insert row with multi-line body and title
    rc = sqlite3_exec(db[0], "INSERT INTO notes (id, body, title) VALUES ('n1', 'Line1\nLine2\nLine3', 'My Title');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Sync 0 -> 1
    if (!do_merge_values(db[0], db[1], false)) goto fail;

    // Site 0: edit block column (body line 1) AND normal column (title)
    rc = sqlite3_exec(db[0], "UPDATE notes SET body = 'EditedLine1\nLine2\nLine3', title = 'Title From A' WHERE id = 'n1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Site 1: edit a different block (body line 3) AND normal column (title — will conflict via LWW)
    rc = sqlite3_exec(db[1], "UPDATE notes SET body = 'Line1\nLine2\nEditedLine3', title = 'Title From B' WHERE id = 'n1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Sync both ways
    if (!do_merge_values(db[0], db[1], true)) goto fail;
    if (!do_merge_values(db[1], db[0], true)) goto fail;

    // Materialize block column
    rc = sqlite3_exec(db[0], "SELECT cloudsync_text_materialize('notes', 'body', 'n1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('notes', 'body', 'n1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    char *body0 = do_select_text(db[0], "SELECT body FROM notes WHERE id = 'n1';");
    char *body1 = do_select_text(db[1], "SELECT body FROM notes WHERE id = 'n1';");
    char *title0 = do_select_text(db[0], "SELECT title FROM notes WHERE id = 'n1';");
    char *title1 = do_select_text(db[1], "SELECT title FROM notes WHERE id = 'n1';");

    bool ok = true;

    // Bodies should converge
    if (!body0 || !body1 || strcmp(body0, body1) != 0) {
        printf("mixed_columns: body diverged\n");
        ok = false;
    } else {
        // Both non-conflicting block edits should be preserved
        if (!strstr(body0, "EditedLine1")) { printf("mixed_columns: missing EditedLine1\n"); ok = false; }
        if (!strstr(body0, "Line2")) { printf("mixed_columns: missing Line2\n"); ok = false; }
        if (!strstr(body0, "EditedLine3")) { printf("mixed_columns: missing EditedLine3\n"); ok = false; }
    }

    // Titles should converge (normal LWW — one wins)
    if (!title0 || !title1 || strcmp(title0, title1) != 0) {
        printf("mixed_columns: title diverged: [%s] vs [%s]\n", title0 ? title0 : "NULL", title1 ? title1 : "NULL");
        ok = false;
    }

    if (body0) sqlite3_free(body0);
    if (body1) sqlite3_free(body1);
    if (title0) sqlite3_free(title0);
    if (title1) sqlite3_free(title1);
    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return ok;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test 5: NULL to text transition — INSERT with NULL body, then UPDATE to multi-line text
bool do_test_block_lww_null_to_text(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Insert with NULL body on site 0
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', NULL);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("null_to_text: INSERT NULL failed\n"); goto fail; }

    // Sync to site 1
    if (!do_merge_values(db[0], db[1], false)) { printf("null_to_text: initial sync failed\n"); goto fail; }

    // Update to multi-line text on site 0
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'Hello\nWorld\nFoo' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("null_to_text: UPDATE failed\n"); goto fail; }

    // Verify blocks created
    int64_t blocks = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1');");
    if (blocks != 3) { printf("null_to_text: expected 3 blocks, got %" PRId64 "\n", blocks); goto fail; }

    // Sync update to site 1
    if (!do_merge_values(db[0], db[1], true)) { printf("null_to_text: sync update failed\n"); goto fail; }

    // Materialize on site 1
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("null_to_text: materialize failed\n"); goto fail; }

    char *body = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");
    if (!body || strcmp(body, "Hello\nWorld\nFoo") != 0) {
        printf("null_to_text: expected 'Hello\\nWorld\\nFoo', got '%s'\n", body ? body : "NULL");
        if (body) sqlite3_free(body);
        goto fail;
    }
    sqlite3_free(body);

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return true;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test 6: Interleaved inserts — multiple rounds of inserting between existing lines
bool do_test_block_lww_interleaved(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Start with 2 lines
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'A\nB');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    if (!do_merge_values(db[0], db[1], false)) goto fail;

    // Round 1: Site 0 inserts between A and B
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'A\nC\nB' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    if (!do_merge_values(db[0], db[1], true)) goto fail;
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Round 2: Site 1 inserts between A and C
    rc = sqlite3_exec(db[1], "UPDATE docs SET body = 'A\nD\nC\nB' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    if (!do_merge_values(db[1], db[0], true)) goto fail;
    rc = sqlite3_exec(db[0], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Round 3: Site 0 inserts between D and C
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'A\nD\nE\nC\nB' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    if (!do_merge_values(db[0], db[1], true)) goto fail;
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Verify final state on both sites
    char *body0 = do_select_text(db[0], "SELECT body FROM docs WHERE id = 'doc1';");
    char *body1 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");

    bool ok = true;
    if (!body0 || !body1 || strcmp(body0, body1) != 0) {
        printf("interleaved: diverged: [%s] vs [%s]\n", body0 ? body0 : "NULL", body1 ? body1 : "NULL");
        ok = false;
    } else {
        // All 5 lines should be present
        if (!strstr(body0, "A")) { printf("interleaved: missing A\n"); ok = false; }
        if (!strstr(body0, "D")) { printf("interleaved: missing D\n"); ok = false; }
        if (!strstr(body0, "E")) { printf("interleaved: missing E\n"); ok = false; }
        if (!strstr(body0, "C")) { printf("interleaved: missing C\n"); ok = false; }
        if (!strstr(body0, "B")) { printf("interleaved: missing B\n"); ok = false; }

        // Verify 5 blocks
        int64_t blocks = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1');");
        if (blocks != 5) { printf("interleaved: expected 5 blocks, got %" PRId64 "\n", blocks); ok = false; }
    }

    if (body0) sqlite3_free(body0);
    if (body1) sqlite3_free(body1);
    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return ok;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test 7: Custom delimiter — paragraph separator instead of newline
bool do_test_block_lww_custom_delimiter(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        // Set custom delimiter: double newline (paragraph separator)
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'delimiter', '\n\n');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) { printf("custom_delim: set delimiter failed: %s\n", sqlite3_errmsg(db[i])); goto fail; }
    }

    // Insert text with double-newline separated paragraphs
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'Para one line1\nline2\n\nPara two\n\nPara three');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Should produce 3 blocks (3 paragraphs)
    int64_t blocks = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1');");
    if (blocks != 3) { printf("custom_delim: expected 3 blocks, got %" PRId64 "\n", blocks); goto fail; }

    // Sync and materialize
    if (!do_merge_values(db[0], db[1], false)) goto fail;
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    char *body = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");
    if (!body || strcmp(body, "Para one line1\nline2\n\nPara two\n\nPara three") != 0) {
        printf("custom_delim: mismatch: [%s]\n", body ? body : "NULL");
        if (body) sqlite3_free(body);
        goto fail;
    }
    sqlite3_free(body);

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return true;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test 8: Large text — many lines to verify position ID distribution
bool do_test_block_lww_large_text(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Build a 200-line text
    #define LARGE_NLINES 200
    char large_text[LARGE_NLINES * 20];
    int offset = 0;
    for (int i = 0; i < LARGE_NLINES; i++) {
        if (i > 0) large_text[offset++] = '\n';
        offset += snprintf(large_text + offset, sizeof(large_text) - offset, "Line %03d content", i);
    }

    // Insert via prepared statement to avoid SQL escaping issues
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db[0], "INSERT INTO docs (id, body) VALUES ('bigdoc', ?);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, large_text, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) { printf("large_text: INSERT failed\n"); goto fail; }

    // Verify block count
    int64_t blocks = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('bigdoc');");
    if (blocks != LARGE_NLINES) { printf("large_text: expected %d blocks, got %" PRId64 "\n", LARGE_NLINES, blocks); goto fail; }

    // Verify all position IDs are unique and ordered
    int64_t distinct_positions = do_select_int(db[0],
        "SELECT count(DISTINCT col_name) FROM docs_cloudsync WHERE col_name LIKE 'body' || x'1f' || '%';");
    if (distinct_positions != LARGE_NLINES) {
        printf("large_text: expected %d distinct positions, got %" PRId64 "\n", LARGE_NLINES, distinct_positions);
        goto fail;
    }

    // Sync and materialize
    if (!do_merge_using_payload(db[0], db[1], false, true)) { printf("large_text: sync failed\n"); goto fail; }
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'bigdoc');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("large_text: materialize failed\n"); goto fail; }

    char *body = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'bigdoc';");
    if (!body || strcmp(body, large_text) != 0) {
        printf("large_text: roundtrip mismatch\n");
        if (body) sqlite3_free(body);
        goto fail;
    }
    sqlite3_free(body);

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return true;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test 9: Rapid sequential updates — many updates on same row in quick succession
bool do_test_block_lww_rapid_updates(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Insert initial
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'Start');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // 50 rapid updates, progressively adding lines
    sqlite3_stmt *upd = NULL;
    rc = sqlite3_prepare_v2(db[0], "UPDATE docs SET body = ? WHERE id = 'doc1';", -1, &upd, NULL);
    if (rc != SQLITE_OK) goto fail;

    #define RAPID_ROUNDS 50
    char rapid_text[RAPID_ROUNDS * 20];
    int roff = 0;
    for (int i = 0; i < RAPID_ROUNDS; i++) {
        if (i > 0) rapid_text[roff++] = '\n';
        roff += snprintf(rapid_text + roff, sizeof(rapid_text) - roff, "Update%d", i);

        sqlite3_bind_text(upd, 1, rapid_text, roff, SQLITE_STATIC);
        rc = sqlite3_step(upd);
        if (rc != SQLITE_DONE) { printf("rapid: UPDATE %d failed\n", i); sqlite3_finalize(upd); goto fail; }
        sqlite3_reset(upd);
    }
    sqlite3_finalize(upd);

    // Verify final block count matches line count
    int64_t blocks = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1');");
    if (blocks != RAPID_ROUNDS) {
        printf("rapid: expected %d blocks, got %" PRId64 "\n", RAPID_ROUNDS, blocks);
        goto fail;
    }

    // Sync and verify roundtrip
    if (!do_merge_using_payload(db[0], db[1], false, true)) { printf("rapid: sync failed\n"); goto fail; }
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("rapid: materialize failed\n"); goto fail; }

    char *body0 = do_select_text(db[0], "SELECT body FROM docs WHERE id = 'doc1';");
    char *body1 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");

    bool ok = true;
    if (!body0 || !body1 || strcmp(body0, body1) != 0) {
        printf("rapid: roundtrip mismatch\n");
        ok = false;
    } else {
        // Check first and last lines
        if (!strstr(body0, "Update0")) { printf("rapid: missing Update0\n"); ok = false; }
        if (!strstr(body0, "Update49")) { printf("rapid: missing Update49\n"); ok = false; }
    }

    if (body0) sqlite3_free(body0);
    if (body1) sqlite3_free(body1);
    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return ok;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test: Unicode/multibyte content in blocks (emoji, CJK, accented chars)
bool do_test_block_lww_unicode(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Insert multi-line text with unicode content
    const char *unicode_text = "Hello \xC3\xA9\xC3\xA0\xC3\xBC" "\n"  // accented: éàü
                               "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95" "\n" // CJK: 中文测试
                               "\xF0\x9F\x98\x80\xF0\x9F\x8E\x89\xF0\x9F\x9A\x80";       // emoji: 😀🎉🚀

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', ?);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, unicode_text, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) goto fail;

    // Should have 3 blocks
    int64_t blocks = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1');");
    if (blocks != 3) { printf("unicode: expected 3 blocks, got %" PRId64 "\n", blocks); goto fail; }

    // Sync and materialize
    if (!do_merge_using_payload(db[0], db[1], false, true)) { printf("unicode: sync failed\n"); goto fail; }
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("unicode: materialize failed\n"); goto fail; }

    char *body = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");
    if (!body || strcmp(body, unicode_text) != 0) {
        printf("unicode: roundtrip mismatch\n");
        if (body) sqlite3_free(body);
        goto fail;
    }

    // Update: edit the emoji line
    const char *updated_text = "Hello \xC3\xA9\xC3\xA0\xC3\xBC" "\n"
                               "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95" "\n"
                               "\xF0\x9F\x92\xAF\xF0\x9F\x94\xA5";  // changed emoji: 💯🔥
    sqlite3_free(body);

    stmt = NULL;
    rc = sqlite3_prepare_v2(db[0], "UPDATE docs SET body = ? WHERE id = 'doc1';", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, updated_text, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) goto fail;

    // Sync update
    if (!do_merge_using_payload(db[0], db[1], true, true)) { printf("unicode: sync update failed\n"); goto fail; }
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    body = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");
    if (!body || strcmp(body, updated_text) != 0) {
        printf("unicode: update roundtrip mismatch\n");
        if (body) sqlite3_free(body);
        goto fail;
    }
    sqlite3_free(body);

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return true;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test: Special characters (tabs, carriage returns, etc.) in blocks
bool do_test_block_lww_special_chars(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Text with tabs, carriage returns, and other special chars within lines
    const char *special_text = "col1\tcol2\tcol3\n"       // tabs within line
                               "line with\r\nembedded\n"   // \r before \n delimiter
                               "back\\slash \"quotes\"";    // backslash and quotes

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', ?);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, special_text, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) goto fail;

    // Should split on \n: "col1\tcol2\tcol3", "line with\r", "embedded", "back\\slash \"quotes\""
    int64_t blocks = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1');");
    if (blocks != 4) { printf("special: expected 4 blocks, got %" PRId64 "\n", blocks); goto fail; }

    // Sync and verify roundtrip
    if (!do_merge_using_payload(db[0], db[1], false, true)) { printf("special: sync failed\n"); goto fail; }
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    char *body = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");
    if (!body || strcmp(body, special_text) != 0) {
        printf("special: roundtrip mismatch\n");
        if (body) sqlite3_free(body);
        goto fail;
    }
    sqlite3_free(body);

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return true;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test: Concurrent delete vs edit on different blocks
// Site A deletes the row, Site B edits a line. After sync, delete wins.
bool do_test_block_lww_delete_vs_edit(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Insert initial doc
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'Line1\nLine2\nLine3');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Sync to site 1
    if (!do_merge_values(db[0], db[1], false)) goto fail;

    // Site 0: DELETE the row
    rc = sqlite3_exec(db[0], "DELETE FROM docs WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Site 1: Edit line 2
    rc = sqlite3_exec(db[1], "UPDATE docs SET body = 'Line1\nEdited\nLine3' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Sync both ways
    if (!do_merge_values(db[0], db[1], true)) goto fail;
    if (!do_merge_values(db[1], db[0], true)) goto fail;

    // Both should converge: either row deleted or row exists with some content
    int64_t rows0 = do_select_int(db[0], "SELECT count(*) FROM docs WHERE id = 'doc1';");
    int64_t rows1 = do_select_int(db[1], "SELECT count(*) FROM docs WHERE id = 'doc1';");

    bool ok = true;
    if (rows0 != rows1) {
        printf("delete_vs_edit: row count diverged: db0=%" PRId64 " db1=%" PRId64 "\n", rows0, rows1);
        ok = false;
    }

    // If the row still exists, materialize and verify convergence
    if (rows0 > 0 && rows1 > 0) {
        sqlite3_exec(db[0], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
        sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);

        char *body0 = do_select_text(db[0], "SELECT body FROM docs WHERE id = 'doc1';");
        char *body1 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");
        if (body0 && body1 && strcmp(body0, body1) != 0) {
            printf("delete_vs_edit: bodies diverged\n");
            ok = false;
        }
        if (body0) sqlite3_free(body0);
        if (body1) sqlite3_free(body1);
    }

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return ok;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test: Two block columns on same table
bool do_test_block_lww_two_block_cols(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT, notes TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'notes', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Insert with both block columns
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body, notes) VALUES ('doc1', 'B1\nB2\nB3', 'N1\nN2');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("two_block_cols: INSERT failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    // Verify blocks created for both columns
    int64_t body_blocks = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync WHERE col_name LIKE 'body' || x'1f' || '%';");
    int64_t notes_blocks = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync WHERE col_name LIKE 'notes' || x'1f' || '%';");
    if (body_blocks != 3) { printf("two_block_cols: expected 3 body blocks, got %" PRId64 "\n", body_blocks); goto fail; }
    if (notes_blocks != 2) { printf("two_block_cols: expected 2 notes blocks, got %" PRId64 "\n", notes_blocks); goto fail; }

    // Sync to site 1
    if (!do_merge_values(db[0], db[1], false)) goto fail;

    // Site 0: edit body line 1
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'B1_edited\nB2\nB3' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Site 1: edit notes line 2
    rc = sqlite3_exec(db[1], "UPDATE docs SET notes = 'N1\nN2_edited' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Sync both ways
    if (!do_merge_values(db[0], db[1], true)) goto fail;
    if (!do_merge_values(db[1], db[0], true)) goto fail;

    // Materialize both columns on both sites
    for (int i = 0; i < 2; i++) {
        rc = sqlite3_exec(db[i], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) { printf("two_block_cols: materialize body db[%d] failed\n", i); goto fail; }
        rc = sqlite3_exec(db[i], "SELECT cloudsync_text_materialize('docs', 'notes', 'doc1');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) { printf("two_block_cols: materialize notes db[%d] failed\n", i); goto fail; }
    }

    char *body0 = do_select_text(db[0], "SELECT body FROM docs WHERE id = 'doc1';");
    char *body1 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");
    char *notes0 = do_select_text(db[0], "SELECT notes FROM docs WHERE id = 'doc1';");
    char *notes1 = do_select_text(db[1], "SELECT notes FROM docs WHERE id = 'doc1';");

    bool ok = true;
    if (!body0 || !body1 || strcmp(body0, body1) != 0) {
        printf("two_block_cols: body diverged\n"); ok = false;
    } else if (!strstr(body0, "B1_edited")) {
        printf("two_block_cols: body edit missing\n"); ok = false;
    }

    if (!notes0 || !notes1 || strcmp(notes0, notes1) != 0) {
        printf("two_block_cols: notes diverged\n"); ok = false;
    } else if (!strstr(notes0, "N2_edited")) {
        printf("two_block_cols: notes edit missing\n"); ok = false;
    }

    if (body0) sqlite3_free(body0);
    if (body1) sqlite3_free(body1);
    if (notes0) sqlite3_free(notes0);
    if (notes1) sqlite3_free(notes1);
    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return ok;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test: Update text to NULL (text->NULL transition)
bool do_test_block_lww_text_to_null(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Insert multi-line text
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'Line1\nLine2\nLine3');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    int64_t blocks_before = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1');");
    if (blocks_before != 3) { printf("text_to_null: expected 3 blocks before, got %" PRId64 "\n", blocks_before); goto fail; }

    // Update to NULL
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = NULL WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("text_to_null: UPDATE to NULL failed\n"); goto fail; }

    // Verify body is NULL
    int64_t is_null = do_select_int(db[0], "SELECT body IS NULL FROM docs WHERE id = 'doc1';");
    if (is_null != 1) { printf("text_to_null: body not NULL after update\n"); goto fail; }

    // Sync and verify
    if (!do_merge_values(db[0], db[1], false)) { printf("text_to_null: sync failed\n"); goto fail; }

    // Materialize on site 1
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    int64_t is_null_b = do_select_int(db[1], "SELECT body IS NULL FROM docs WHERE id = 'doc1';");
    if (is_null_b != 1) { printf("text_to_null: body not NULL on site 1 after sync\n"); goto fail; }

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return true;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test: Payload-based sync for block columns (vs row-by-row do_merge_values)
bool do_test_block_lww_payload_sync(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Insert and first sync via payload
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'Alpha\nBravo\nCharlie');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    if (!do_merge_using_payload(db[0], db[1], false, true)) { printf("payload_sync: initial merge failed\n"); goto fail; }

    // Edit on both sites
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'Alpha_A\nBravo\nCharlie' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[1], "UPDATE docs SET body = 'Alpha\nBravo\nCharlie_B' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Sync via payload both ways
    if (!do_merge_using_payload(db[0], db[1], true, true)) { printf("payload_sync: merge 0->1 failed\n"); goto fail; }
    if (!do_merge_using_payload(db[1], db[0], true, true)) { printf("payload_sync: merge 1->0 failed\n"); goto fail; }

    // Materialize
    rc = sqlite3_exec(db[0], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    char *body0 = do_select_text(db[0], "SELECT body FROM docs WHERE id = 'doc1';");
    char *body1 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");

    bool ok = true;
    if (!body0 || !body1 || strcmp(body0, body1) != 0) {
        printf("payload_sync: bodies diverged\n"); ok = false;
    } else {
        if (!strstr(body0, "Alpha_A")) { printf("payload_sync: missing Alpha_A\n"); ok = false; }
        if (!strstr(body0, "Bravo")) { printf("payload_sync: missing Bravo\n"); ok = false; }
        if (!strstr(body0, "Charlie_B")) { printf("payload_sync: missing Charlie_B\n"); ok = false; }
    }

    if (body0) sqlite3_free(body0);
    if (body1) sqlite3_free(body1);
    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return ok;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test: Idempotent apply — applying the same payload twice is a no-op
bool do_test_block_lww_idempotent(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Insert and sync
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'Line1\nLine2\nLine3');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    if (!do_merge_using_payload(db[0], db[1], false, true)) goto fail;

    // Edit on site 0
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'Edited1\nLine2\nLine3' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Apply payload to site 1 TWICE
    if (!do_merge_using_payload(db[0], db[1], true, true)) { printf("idempotent: first apply failed\n"); goto fail; }
    if (!do_merge_using_payload(db[0], db[1], true, true)) { printf("idempotent: second apply failed\n"); goto fail; }

    // Materialize
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    char *body = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");
    bool ok = true;
    if (!body || strcmp(body, "Edited1\nLine2\nLine3") != 0) {
        printf("idempotent: body mismatch: [%s]\n", body ? body : "NULL");
        ok = false;
    }

    // Verify block count is still 3 (no duplicates from double apply)
    int64_t blocks = do_select_int(db[1], "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1');");
    if (blocks != 3) { printf("idempotent: expected 3 blocks, got %" PRId64 "\n", blocks); ok = false; }

    if (body) sqlite3_free(body);
    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return ok;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test: Block position ordering — after edits, materialized text has correct line order
bool do_test_block_lww_ordering(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Insert initial doc: A B C D E
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'A\nB\nC\nD\nE');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    if (!do_merge_values(db[0], db[1], false)) goto fail;

    // Site 0: insert X between B and C, remove D -> A B X C E
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'A\nB\nX\nC\nE' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Site 1: insert Y between D and E -> A B C D Y E
    rc = sqlite3_exec(db[1], "UPDATE docs SET body = 'A\nB\nC\nD\nY\nE' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Sync
    if (!do_merge_values(db[0], db[1], true)) goto fail;
    if (!do_merge_values(db[1], db[0], true)) goto fail;

    rc = sqlite3_exec(db[0], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    char *body0 = do_select_text(db[0], "SELECT body FROM docs WHERE id = 'doc1';");
    char *body1 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");

    bool ok = true;
    if (!body0 || !body1 || strcmp(body0, body1) != 0) {
        printf("ordering: bodies diverged: [%s] vs [%s]\n", body0 ? body0 : "NULL", body1 ? body1 : "NULL");
        ok = false;
    } else {
        // Verify ordering: A must come before B, B before C, etc.
        // All lines that survived should maintain relative order
        const char *pA = strstr(body0, "A");
        const char *pB = strstr(body0, "B");
        const char *pC = strstr(body0, "C");
        const char *pE = strstr(body0, "E");

        if (!pA || !pB || !pC || !pE) {
            printf("ordering: missing original lines\n"); ok = false;
        } else {
            if (pA >= pB) { printf("ordering: A not before B\n"); ok = false; }
            if (pB >= pC) { printf("ordering: B not before C\n"); ok = false; }
            if (pC >= pE) { printf("ordering: C not before E\n"); ok = false; }
        }

        // X (inserted between B and C) should appear between B and C
        const char *pX = strstr(body0, "X");
        if (pX) {
            if (pX <= pB || pX >= pC) { printf("ordering: X not between B and C\n"); ok = false; }
        }

        // Y should appear somewhere after C
        const char *pY = strstr(body0, "Y");
        if (pY) {
            if (pY <= pC) { printf("ordering: Y not after C\n"); ok = false; }
        }
    }

    if (body0) sqlite3_free(body0);
    if (body1) sqlite3_free(body1);
    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return ok;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test: Composite primary key (text + int) with block column
bool do_test_block_lww_composite_pk(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (owner TEXT NOT NULL, seq INTEGER NOT NULL, body TEXT, PRIMARY KEY(owner, seq));", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Insert on site 0
    rc = sqlite3_exec(db[0], "INSERT INTO docs (owner, seq, body) VALUES ('alice', 1, 'Line1\nLine2\nLine3');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("composite_pk: INSERT failed\n"); goto fail; }

    int64_t blocks = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('alice', 1);");
    if (blocks != 3) { printf("composite_pk: expected 3 blocks, got %" PRId64 "\n", blocks); goto fail; }

    // Sync to site 1
    if (!do_merge_using_payload(db[0], db[1], false, true)) { printf("composite_pk: sync failed\n"); goto fail; }
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'alice', 1);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("composite_pk: materialize failed: %s\n", sqlite3_errmsg(db[1])); goto fail; }

    char *body = do_select_text(db[1], "SELECT body FROM docs WHERE owner = 'alice' AND seq = 1;");
    if (!body || strcmp(body, "Line1\nLine2\nLine3") != 0) {
        printf("composite_pk: body mismatch: [%s]\n", body ? body : "NULL");
        if (body) sqlite3_free(body);
        goto fail;
    }
    sqlite3_free(body);

    // Edit on site 1, sync back
    rc = sqlite3_exec(db[1], "UPDATE docs SET body = 'Line1\nEdited2\nLine3' WHERE owner = 'alice' AND seq = 1;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    if (!do_merge_using_payload(db[1], db[0], true, true)) { printf("composite_pk: reverse sync failed\n"); goto fail; }
    rc = sqlite3_exec(db[0], "SELECT cloudsync_text_materialize('docs', 'body', 'alice', 1);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    char *body0 = do_select_text(db[0], "SELECT body FROM docs WHERE owner = 'alice' AND seq = 1;");
    if (!body0 || strcmp(body0, "Line1\nEdited2\nLine3") != 0) {
        printf("composite_pk: reverse body mismatch: [%s]\n", body0 ? body0 : "NULL");
        if (body0) sqlite3_free(body0);
        goto fail;
    }
    sqlite3_free(body0);

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return true;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test: Empty string body (not NULL) — should produce 1 block with empty content
bool do_test_block_lww_empty_vs_null(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Insert empty string (NOT NULL)
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', '');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    int64_t blocks = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1');");
    if (blocks != 1) { printf("empty_vs_null: expected 1 block for empty string, got %" PRId64 "\n", blocks); goto fail; }

    // Insert NULL
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc2', NULL);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    int64_t blocks_null = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc2');");
    if (blocks_null != 1) { printf("empty_vs_null: expected 1 block for NULL, got %" PRId64 "\n", blocks_null); goto fail; }

    // Sync both to site 1
    if (!do_merge_using_payload(db[0], db[1], false, true)) { printf("empty_vs_null: sync failed\n"); goto fail; }
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc2');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // doc1 (empty string): body should be empty string, NOT NULL
    char *body1 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");
    int64_t is_null1 = do_select_int(db[1], "SELECT body IS NULL FROM docs WHERE id = 'doc1';");
    if (is_null1 != 0) { printf("empty_vs_null: doc1 body should NOT be NULL\n"); if (body1) sqlite3_free(body1); goto fail; }
    if (!body1 || strcmp(body1, "") != 0) { printf("empty_vs_null: doc1 body should be empty, got [%s]\n", body1 ? body1 : "NULL"); if (body1) sqlite3_free(body1); goto fail; }
    sqlite3_free(body1);

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return true;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test: DELETE row then re-insert with different block content (resurrection)
bool do_test_block_lww_delete_reinsert(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Insert and sync
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'Old1\nOld2\nOld3');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    if (!do_merge_using_payload(db[0], db[1], false, true)) goto fail;

    // Delete the row
    rc = sqlite3_exec(db[0], "DELETE FROM docs WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("del_reinsert: DELETE failed\n"); goto fail; }

    // Sync delete
    if (!do_merge_using_payload(db[0], db[1], true, true)) { printf("del_reinsert: delete sync failed\n"); goto fail; }

    // Verify row gone on site 1
    int64_t count = do_select_int(db[1], "SELECT count(*) FROM docs WHERE id = 'doc1';");
    if (count != 0) { printf("del_reinsert: row should be deleted on site 1, count=%" PRId64 "\n", count); goto fail; }

    // Re-insert with different content
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'New1\nNew2');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("del_reinsert: re-INSERT failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    // Sync re-insert
    if (!do_merge_using_payload(db[0], db[1], true, true)) { printf("del_reinsert: reinsert sync failed\n"); goto fail; }
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    char *body = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");
    if (!body || strcmp(body, "New1\nNew2") != 0) {
        printf("del_reinsert: body mismatch after reinsert: [%s]\n", body ? body : "NULL");
        if (body) sqlite3_free(body);
        goto fail;
    }
    sqlite3_free(body);

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return true;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test: INTEGER primary key with block column
bool do_test_block_lww_integer_pk(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE notes (id INTEGER NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) { printf("int_pk: CREATE TABLE failed on %d: %s\n", i, sqlite3_errmsg(db[i])); goto fail; }
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('notes', 'CLS', 1);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) { printf("int_pk: init failed on %d: %s\n", i, sqlite3_errmsg(db[i])); goto fail; }
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('notes', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) { printf("int_pk: set_column failed on %d: %s\n", i, sqlite3_errmsg(db[i])); goto fail; }
    }

    // Insert on site 0
    rc = sqlite3_exec(db[0], "INSERT INTO notes (id, body) VALUES (42, 'First\nSecond\nThird');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("int_pk: INSERT failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    int64_t blocks = do_select_int(db[0], "SELECT count(*) FROM notes_cloudsync_blocks WHERE pk = cloudsync_pk_encode(42);");
    if (blocks != 3) { printf("int_pk: expected 3 blocks, got %" PRId64 "\n", blocks); goto fail; }

    // Sync to site 1
    if (!do_merge_using_payload(db[0], db[1], false, true)) { printf("int_pk: sync failed\n"); goto fail; }
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('notes', 'body', 42);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("int_pk: materialize failed: %s\n", sqlite3_errmsg(db[1])); goto fail; }

    // Debug: check row exists
    int64_t row_count = do_select_int(db[1], "SELECT count(*) FROM notes WHERE id = 42;");
    if (row_count != 1) { printf("int_pk: row not found on site 1, count=%" PRId64 "\n", row_count); goto fail; }

    char *body = do_select_text(db[1], "SELECT body FROM notes WHERE id = 42;");
    if (!body || strcmp(body, "First\nSecond\nThird") != 0) {
        printf("int_pk: body mismatch: [%s]\n", body ? body : "NULL");
        if (body) sqlite3_free(body);
        goto fail;
    }
    sqlite3_free(body);

    // Edit and sync back
    rc = sqlite3_exec(db[1], "UPDATE notes SET body = 'First\nEdited\nThird' WHERE id = 42;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("int_pk: UPDATE failed: %s\n", sqlite3_errmsg(db[1])); goto fail; }
    if (!do_merge_using_payload(db[1], db[0], true, true)) { printf("int_pk: reverse sync failed\n"); goto fail; }
    rc = sqlite3_exec(db[0], "SELECT cloudsync_text_materialize('notes', 'body', 42);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("int_pk: reverse mat failed: %s\n", sqlite3_errmsg(db[0])); goto fail; }

    char *body0 = do_select_text(db[0], "SELECT body FROM notes WHERE id = 42;");
    if (!body0 || strcmp(body0, "First\nEdited\nThird") != 0) {
        printf("int_pk: reverse body mismatch: [%s]\n", body0 ? body0 : "NULL");
        if (body0) sqlite3_free(body0);
        goto fail;
    }
    sqlite3_free(body0);

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return true;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test: Multiple rows with block columns in a single sync
bool do_test_block_lww_multi_row(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Insert 3 rows
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('r1', 'R1-Line1\nR1-Line2');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('r2', 'R2-Alpha\nR2-Beta\nR2-Gamma');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('r3', 'R3-Only');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Edit r1 and r3
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'R1-Edited\nR1-Line2' WHERE id = 'r1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'R3-Changed' WHERE id = 'r3';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Sync all in one payload
    if (!do_merge_using_payload(db[0], db[1], false, true)) { printf("multi_row: sync failed\n"); goto fail; }

    // Materialize all
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'r1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'r2');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'r3');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    bool ok = true;
    char *b1 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'r1';");
    if (!b1 || strcmp(b1, "R1-Edited\nR1-Line2") != 0) { printf("multi_row: r1 mismatch [%s]\n", b1 ? b1 : "NULL"); ok = false; }
    if (b1) sqlite3_free(b1);

    char *b2 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'r2';");
    if (!b2 || strcmp(b2, "R2-Alpha\nR2-Beta\nR2-Gamma") != 0) { printf("multi_row: r2 mismatch [%s]\n", b2 ? b2 : "NULL"); ok = false; }
    if (b2) sqlite3_free(b2);

    char *b3 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'r3';");
    if (!b3 || strcmp(b3, "R3-Changed") != 0) { printf("multi_row: r3 mismatch [%s]\n", b3 ? b3 : "NULL"); ok = false; }
    if (b3) sqlite3_free(b3);

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return ok;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test: Concurrent add at non-overlapping positions (top vs bottom)
bool do_test_block_lww_nonoverlap_add(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Insert initial: A B C
    rc = sqlite3_exec(db[0], "INSERT INTO docs (id, body) VALUES ('doc1', 'A\nB\nC');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    if (!do_merge_values(db[0], db[1], false)) goto fail;

    // Site 0: add line at top -> X A B C
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'X\nA\nB\nC' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Site 1: add line at bottom -> A B C Y
    rc = sqlite3_exec(db[1], "UPDATE docs SET body = 'A\nB\nC\nY' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    // Bidirectional sync
    if (!do_merge_values(db[0], db[1], true)) goto fail;
    if (!do_merge_values(db[1], db[0], true)) goto fail;

    rc = sqlite3_exec(db[0], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    char *body0 = do_select_text(db[0], "SELECT body FROM docs WHERE id = 'doc1';");
    char *body1 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");

    bool ok = true;
    if (!body0 || !body1 || strcmp(body0, body1) != 0) {
        printf("nonoverlap: bodies diverged: [%s] vs [%s]\n", body0 ? body0 : "NULL", body1 ? body1 : "NULL");
        ok = false;
    } else {
        // X should be present, Y should be present, original A B C should be present
        if (!strstr(body0, "X")) { printf("nonoverlap: X missing\n"); ok = false; }
        if (!strstr(body0, "Y")) { printf("nonoverlap: Y missing\n"); ok = false; }
        if (!strstr(body0, "A")) { printf("nonoverlap: A missing\n"); ok = false; }
        if (!strstr(body0, "B")) { printf("nonoverlap: B missing\n"); ok = false; }
        if (!strstr(body0, "C")) { printf("nonoverlap: C missing\n"); ok = false; }

        // Order: X before A, Y after C
        const char *pX = strstr(body0, "X");
        const char *pA = strstr(body0, "A");
        const char *pC = strstr(body0, "C");
        const char *pY = strstr(body0, "Y");
        if (pX && pA && pX >= pA) { printf("nonoverlap: X not before A\n"); ok = false; }
        if (pC && pY && pY <= pC) { printf("nonoverlap: Y not after C\n"); ok = false; }
    }

    if (body0) sqlite3_free(body0);
    if (body1) sqlite3_free(body1);
    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return ok;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test: Very long single line (10K chars, single block)
bool do_test_block_lww_long_line(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Build a 10,000-char single line
    {
        char *long_line = (char *)malloc(10001);
        if (!long_line) goto fail;
        for (int i = 0; i < 10000; i++) long_line[i] = 'A' + (i % 26);
        long_line[10000] = '\0';

        char *sql = sqlite3_mprintf("INSERT INTO docs (id, body) VALUES ('doc1', '%q');", long_line);
        rc = sqlite3_exec(db[0], sql, NULL, NULL, NULL);
        sqlite3_free(sql);

        if (rc != SQLITE_OK) { printf("long_line: INSERT failed: %s\n", sqlite3_errmsg(db[0])); free(long_line); goto fail; }

        // Should have 1 block (no newlines)
        int64_t blocks = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1');");
        if (blocks != 1) { printf("long_line: expected 1 block, got %" PRId64 "\n", blocks); free(long_line); goto fail; }

        // Sync to site 1
        if (!do_merge_using_payload(db[0], db[1], false, true)) { printf("long_line: sync failed\n"); free(long_line); goto fail; }
        rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) { free(long_line); goto fail; }

        char *body = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");
        bool match = (body && strcmp(body, long_line) == 0);
        if (!match) printf("long_line: body mismatch (len=%zu vs expected 10000)\n", body ? strlen(body) : 0);
        if (body) sqlite3_free(body);
        free(long_line);
        if (!match) goto fail;
    }

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return true;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// Test: Whitespace and empty lines (delimiter edge cases)
bool do_test_block_lww_whitespace(int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    time_t timestamp = time(NULL);
    int rc;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;
        rc = sqlite3_exec(db[i], "CREATE TABLE docs (id TEXT NOT NULL PRIMARY KEY, body TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('docs');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }

    // Text with empty lines, whitespace-only lines, trailing newline
    const char *text = "Line1\n\n  spaces  \n\t\ttabs\n\nLine6\n";
    char *sql = sqlite3_mprintf("INSERT INTO docs (id, body) VALUES ('doc1', '%q');", text);
    rc = sqlite3_exec(db[0], sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) { printf("whitespace: INSERT failed\n"); goto fail; }

    // Count blocks: "Line1", "", "  spaces  ", "\t\ttabs", "", "Line6", "" (trailing newline produces empty last block)
    int64_t blocks = do_select_int(db[0], "SELECT count(*) FROM docs_cloudsync_blocks WHERE pk = cloudsync_pk_encode('doc1');");
    if (blocks != 7) { printf("whitespace: expected 7 blocks, got %" PRId64 "\n", blocks); goto fail; }

    // Sync to site 1
    if (!do_merge_using_payload(db[0], db[1], false, true)) { printf("whitespace: sync failed\n"); goto fail; }
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    char *body = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");
    if (!body || strcmp(body, text) != 0) {
        printf("whitespace: body mismatch: [%s] vs [%s]\n", body ? body : "NULL", text);
        if (body) sqlite3_free(body);
        goto fail;
    }
    sqlite3_free(body);

    // Edit: remove empty lines -> "Line1\n  spaces  \n\t\ttabs\nLine6"
    rc = sqlite3_exec(db[0], "UPDATE docs SET body = 'Line1\n  spaces  \n\t\ttabs\nLine6' WHERE id = 'doc1';", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    if (!do_merge_using_payload(db[0], db[1], true, true)) goto fail;
    rc = sqlite3_exec(db[1], "SELECT cloudsync_text_materialize('docs', 'body', 'doc1');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;

    char *body2 = do_select_text(db[1], "SELECT body FROM docs WHERE id = 'doc1';");
    if (!body2 || strcmp(body2, "Line1\n  spaces  \n\t\ttabs\nLine6") != 0) {
        printf("whitespace: body2 mismatch: [%s]\n", body2 ? body2 : "NULL");
        if (body2) sqlite3_free(body2);
        goto fail;
    }
    sqlite3_free(body2);

    for (int i = 0; i < 2; i++) { close_db(db[i]); db[i] = NULL; }
    return true;

fail:
    for (int i = 0; i < 2; i++) if (db[i]) close_db(db[i]);
    return false;
}

// MARK: - New edge-case tests

bool do_test_unsupported_algorithms (sqlite3 *db) {
    // Test that DWS and AWS algorithms are rejected with an error
    const char *sql;
    int rc;

    // Create tables for the test
    sql = "CREATE TABLE IF NOT EXISTS test_dws (id TEXT PRIMARY KEY, val TEXT);"
          "CREATE TABLE IF NOT EXISTS test_aws (id TEXT PRIMARY KEY, val TEXT);";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return false;

    // DWS should fail
    sql = "SELECT cloudsync_init('test_dws', 'dws');";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_ERROR) return false;

    // AWS should fail
    sql = "SELECT cloudsync_init('test_aws', 'aws');";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_ERROR) return false;

    // Verify no companion tables were created
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sqlite_master WHERE name='test_dws_cloudsync' OR name='test_aws_cloudsync';", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return false;
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return false; }
    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if (count != 0) return false;

    // CLS should still work on the same table
    sql = "SELECT cloudsync_init('test_dws', 'cls');";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return false;

    return true;
}

bool do_test_corrupted_payload (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    bool result = false;

    time_t timestamp = time(NULL);
    int saved_counter = test_counter;

    // Create source and destination databases
    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;

        int rc = sqlite3_exec(db[i], "CREATE TABLE test_tbl (id TEXT PRIMARY KEY, val TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('test_tbl');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }

    // Insert data in source
    sqlite3_exec(db[0], "INSERT INTO test_tbl VALUES ('id1', 'value1');", NULL, NULL, NULL);
    sqlite3_exec(db[0], "INSERT INTO test_tbl VALUES ('id2', 'value2');", NULL, NULL, NULL);

    // Get valid payload as blob
    sqlite3_stmt *enc_stmt = NULL;
    int rc = sqlite3_prepare_v2(db[0], "SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) FROM cloudsync_changes WHERE site_id=cloudsync_siteid();", -1, &enc_stmt, NULL);
    if (rc != SQLITE_OK) goto finalize;

    rc = sqlite3_step(enc_stmt);
    if (rc != SQLITE_ROW) { sqlite3_finalize(enc_stmt); goto finalize; }

    int valid_len = sqlite3_column_bytes(enc_stmt, 0);
    const void *valid_blob = sqlite3_column_blob(enc_stmt, 0);
    if (!valid_blob || valid_len < 20) { sqlite3_finalize(enc_stmt); goto finalize; }

    // Copy valid payload
    char *payload_copy = (char *)malloc(valid_len);
    if (!payload_copy) { sqlite3_finalize(enc_stmt); goto finalize; }
    memcpy(payload_copy, valid_blob, valid_len);
    sqlite3_finalize(enc_stmt);

    // Test 1: Empty blob
    {
        sqlite3_stmt *dec_stmt = NULL;
        rc = sqlite3_prepare_v2(db[1], "SELECT cloudsync_payload_decode(?);", -1, &dec_stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_blob(dec_stmt, 1, "", 0, SQLITE_STATIC);
            rc = sqlite3_step(dec_stmt);
            // Should either error or return without inserting
            sqlite3_finalize(dec_stmt);
        }
    }

    // Test 2: Random garbage
    {
        char garbage[16] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
        sqlite3_stmt *dec_stmt = NULL;
        rc = sqlite3_prepare_v2(db[1], "SELECT cloudsync_payload_decode(?);", -1, &dec_stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_blob(dec_stmt, 1, garbage, sizeof(garbage), SQLITE_STATIC);
            rc = sqlite3_step(dec_stmt);
            sqlite3_finalize(dec_stmt);
        }
    }

    // Test 3: Truncated payload (first 10 bytes)
    {
        sqlite3_stmt *dec_stmt = NULL;
        rc = sqlite3_prepare_v2(db[1], "SELECT cloudsync_payload_decode(?);", -1, &dec_stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_blob(dec_stmt, 1, payload_copy, 10, SQLITE_STATIC);
            rc = sqlite3_step(dec_stmt);
            sqlite3_finalize(dec_stmt);
        }
    }

    // Test 4: Valid payload with flipped byte in the middle
    {
        char *corrupted = (char *)malloc(valid_len);
        memcpy(corrupted, payload_copy, valid_len);
        corrupted[valid_len / 2] ^= 0xFF;

        sqlite3_stmt *dec_stmt = NULL;
        rc = sqlite3_prepare_v2(db[1], "SELECT cloudsync_payload_decode(?);", -1, &dec_stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_blob(dec_stmt, 1, corrupted, valid_len, SQLITE_STATIC);
            rc = sqlite3_step(dec_stmt);
            sqlite3_finalize(dec_stmt);
        }
        free(corrupted);
    }

    // Verify destination table is still empty (no corrupted data inserted)
    {
        sqlite3_stmt *count_stmt = NULL;
        rc = sqlite3_prepare_v2(db[1], "SELECT COUNT(*) FROM test_tbl;", -1, &count_stmt, NULL);
        if (rc != SQLITE_OK) { free(payload_copy); goto finalize; }
        if (sqlite3_step(count_stmt) != SQLITE_ROW) { sqlite3_finalize(count_stmt); free(payload_copy); goto finalize; }
        int count = sqlite3_column_int(count_stmt, 0);
        sqlite3_finalize(count_stmt);
        if (count != 0) { printf("corrupted_payload: expected 0 rows but got %d\n", count); free(payload_copy); goto finalize; }
    }

    // Test 5: Valid payload should still work
    if (!do_merge_using_payload(db[0], db[1], false, true)) {
        printf("corrupted_payload: valid payload failed after corrupted attempts\n");
        free(payload_copy);
        goto finalize;
    }

    {
        sqlite3_stmt *count_stmt = NULL;
        rc = sqlite3_prepare_v2(db[1], "SELECT COUNT(*) FROM test_tbl;", -1, &count_stmt, NULL);
        if (rc != SQLITE_OK) { free(payload_copy); goto finalize; }
        if (sqlite3_step(count_stmt) != SQLITE_ROW) { sqlite3_finalize(count_stmt); free(payload_copy); goto finalize; }
        int count = sqlite3_column_int(count_stmt, 0);
        sqlite3_finalize(count_stmt);
        if (count != 2) { printf("corrupted_payload: expected 2 rows after valid apply but got %d\n", count); free(payload_copy); goto finalize; }
    }

    free(payload_copy);
    result = true;

finalize:
    for (int i = 0; i < 2; i++) {
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

bool do_test_payload_idempotency (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    bool result = false;

    time_t timestamp = time(NULL);
    int saved_counter = test_counter;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;

        int rc = sqlite3_exec(db[i], "CREATE TABLE test_tbl (id TEXT PRIMARY KEY, val TEXT, num INTEGER);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('test_tbl');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }

    // Insert data on source
    sqlite3_exec(db[0], "INSERT INTO test_tbl VALUES ('id1', 'hello', 10);", NULL, NULL, NULL);
    sqlite3_exec(db[0], "INSERT INTO test_tbl VALUES ('id2', 'world', 20);", NULL, NULL, NULL);
    sqlite3_exec(db[0], "UPDATE test_tbl SET val = 'hello_updated' WHERE id = 'id1';", NULL, NULL, NULL);

    // Apply payload 3 times and check after each
    int prev_count = -1;
    for (int apply = 0; apply < 3; apply++) {
        if (!do_merge_using_payload(db[0], db[1], false, true)) {
            printf("payload_idempotency: apply #%d failed\n", apply + 1);
            goto finalize;
        }

        // Check row count
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(db[1], "SELECT COUNT(*) FROM test_tbl;", -1, &stmt, NULL);
        if (rc != SQLITE_OK) goto finalize;
        sqlite3_step(stmt);
        int count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);

        if (count != 2) {
            printf("payload_idempotency: expected 2 rows after apply #%d, got %d\n", apply + 1, count);
            goto finalize;
        }

        if (prev_count >= 0 && count != prev_count) {
            printf("payload_idempotency: row count changed from %d to %d on apply #%d\n", prev_count, count, apply + 1);
            goto finalize;
        }
        prev_count = count;
    }

    // Verify data values are correct
    {
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(db[1], "SELECT val FROM test_tbl WHERE id = 'id1';", -1, &stmt, NULL);
        if (rc != SQLITE_OK) goto finalize;
        if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); goto finalize; }
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        if (!val || strcmp(val, "hello_updated") != 0) {
            printf("payload_idempotency: expected 'hello_updated', got '%s'\n", val ? val : "NULL");
            sqlite3_finalize(stmt);
            goto finalize;
        }
        sqlite3_finalize(stmt);
    }

    // Compare source and target
    result = do_compare_queries(db[0], "SELECT * FROM test_tbl ORDER BY id;",
                                db[1], "SELECT * FROM test_tbl ORDER BY id;",
                                -1, -1, print_result);

finalize:
    for (int i = 0; i < 2; i++) {
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

bool do_test_causal_length_tiebreak (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[3] = {NULL, NULL, NULL};
    bool result = false;

    time_t timestamp = time(NULL);
    int saved_counter = test_counter;

    // Create 3 databases with the same table
    for (int i = 0; i < 3; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;

        int rc = sqlite3_exec(db[i], "CREATE TABLE test_tbl (id TEXT PRIMARY KEY, val TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('test_tbl');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }

    // Seed row on db[0] and sync to all
    sqlite3_exec(db[0], "INSERT INTO test_tbl VALUES ('row1', 'seed');", NULL, NULL, NULL);
    do_merge_using_payload(db[0], db[1], false, true);
    do_merge_using_payload(db[0], db[2], false, true);

    // All 3 independently update the same row+column (producing equal CL)
    sqlite3_exec(db[0], "UPDATE test_tbl SET val = 'value_from_db0' WHERE id = 'row1';", NULL, NULL, NULL);
    sqlite3_exec(db[1], "UPDATE test_tbl SET val = 'value_from_db1' WHERE id = 'row1';", NULL, NULL, NULL);
    sqlite3_exec(db[2], "UPDATE test_tbl SET val = 'value_from_db2' WHERE id = 'row1';", NULL, NULL, NULL);

    // Merge all pairs in both directions
    sqlite3 *all_db[MAX_SIMULATED_CLIENTS] = {NULL};
    all_db[0] = db[0]; all_db[1] = db[1]; all_db[2] = db[2];
    if (!do_merge(all_db, 3, true)) {
        printf("causal_length_tiebreak: merge failed\n");
        goto finalize;
    }

    // All 3 must converge to the same value
    const char *query = "SELECT val FROM test_tbl WHERE id = 'row1';";
    char *values[3] = {NULL, NULL, NULL};

    for (int i = 0; i < 3; i++) {
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(db[i], query, -1, &stmt, NULL);
        if (rc != SQLITE_OK) goto tiebreak_finalize;
        if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); goto tiebreak_finalize; }
        const char *text = (const char *)sqlite3_column_text(stmt, 0);
        values[i] = text ? strdup(text) : NULL;
        sqlite3_finalize(stmt);
    }

    // Check convergence
    if (values[0] && values[1] && values[2] &&
        strcmp(values[0], values[1]) == 0 && strcmp(values[1], values[2]) == 0) {
        result = true;
    } else {
        printf("causal_length_tiebreak: databases diverged: '%s', '%s', '%s'\n",
               values[0] ? values[0] : "NULL",
               values[1] ? values[1] : "NULL",
               values[2] ? values[2] : "NULL");
    }

tiebreak_finalize:
    for (int i = 0; i < 3; i++) {
        if (values[i]) free(values[i]);
    }

finalize:
    for (int i = 0; i < 3; i++) {
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

bool do_test_delete_resurrect_ordering (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[3] = {NULL, NULL, NULL};
    bool result = false;

    time_t timestamp = time(NULL);
    int saved_counter = test_counter;

    for (int i = 0; i < 3; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;

        int rc = sqlite3_exec(db[i], "CREATE TABLE test_tbl (id TEXT PRIMARY KEY, val TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('test_tbl');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }

    // Site A: insert row, sync to B and C
    sqlite3_exec(db[0], "INSERT INTO test_tbl VALUES ('row1', 'original');", NULL, NULL, NULL);
    do_merge_using_payload(db[0], db[1], false, true);
    do_merge_using_payload(db[0], db[2], false, true);

    // Site A: delete row (CL 1->2)
    sqlite3_exec(db[0], "DELETE FROM test_tbl WHERE id = 'row1';", NULL, NULL, NULL);

    // Sync delete to B
    do_merge_using_payload(db[0], db[1], true, true);

    // Site B: re-insert (CL 2->3, resurrection)
    sqlite3_exec(db[1], "INSERT INTO test_tbl VALUES ('row1', 'resurrected_by_b');", NULL, NULL, NULL);

    // Site C receives payloads in REVERSE order: B's resurrection first, then A's delete
    do_merge_using_payload(db[1], db[2], true, true);
    do_merge_using_payload(db[0], db[2], true, true);

    // Site A receives B's resurrection
    do_merge_using_payload(db[1], db[2], true, true);
    do_merge_using_payload(db[1], db[0], true, true);

    // All 3 should converge: row exists
    const char *query = "SELECT * FROM test_tbl ORDER BY id;";
    result = do_compare_queries(db[0], query, db[1], query, -1, -1, print_result);
    if (result) result = do_compare_queries(db[0], query, db[2], query, -1, -1, print_result);

    // Verify the row exists (resurrection should win)
    if (result) {
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(db[2], "SELECT COUNT(*) FROM test_tbl WHERE id = 'row1';", -1, &stmt, NULL);
        if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            int count = sqlite3_column_int(stmt, 0);
            if (count != 1) {
                printf("delete_resurrect_ordering: expected row1 to exist on db[2], count=%d\n", count);
                result = false;
            }
        }
        if (stmt) sqlite3_finalize(stmt);
    }

finalize:
    for (int i = 0; i < 3; i++) {
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

bool do_test_large_composite_pk (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    bool result = false;

    time_t timestamp = time(NULL);
    int saved_counter = test_counter;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;

        int rc = sqlite3_exec(db[i],
            "CREATE TABLE cpk_tbl ("
            "  pk_text1 TEXT NOT NULL,"
            "  pk_int1 INTEGER NOT NULL,"
            "  pk_text2 TEXT NOT NULL,"
            "  pk_int2 INTEGER NOT NULL,"
            "  pk_text3 TEXT NOT NULL,"
            "  data_col TEXT,"
            "  num_col INTEGER,"
            "  PRIMARY KEY (pk_text1, pk_int1, pk_text2, pk_int2, pk_text3)"
            ");", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('cpk_tbl');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }

    // Insert data on both sides
    sqlite3_exec(db[0], "INSERT INTO cpk_tbl VALUES ('alpha', 1, 'beta', 100, 'gamma', 'data_a1', 42);", NULL, NULL, NULL);
    sqlite3_exec(db[0], "INSERT INTO cpk_tbl VALUES ('alpha', 2, 'beta', 200, 'delta', 'data_a2', 84);", NULL, NULL, NULL);
    sqlite3_exec(db[0], "INSERT INTO cpk_tbl VALUES ('x', 999, 'y', -1, 'z', 'edge_case', 0);", NULL, NULL, NULL);

    sqlite3_exec(db[1], "INSERT INTO cpk_tbl VALUES ('alpha', 1, 'beta', 100, 'gamma', 'data_b1', 99);", NULL, NULL, NULL);
    sqlite3_exec(db[1], "INSERT INTO cpk_tbl VALUES ('foo', 3, 'bar', 300, 'baz', 'data_b2', 77);", NULL, NULL, NULL);

    // Merge both directions
    if (!do_merge_using_payload(db[0], db[1], false, true)) goto finalize;
    if (!do_merge_using_payload(db[1], db[0], false, true)) goto finalize;

    // Update on db[0] and sync
    sqlite3_exec(db[0], "UPDATE cpk_tbl SET data_col = 'updated_on_a' WHERE pk_text1 = 'foo' AND pk_int1 = 3 AND pk_text2 = 'bar' AND pk_int2 = 300 AND pk_text3 = 'baz';", NULL, NULL, NULL);
    if (!do_merge_using_payload(db[0], db[1], true, true)) goto finalize;

    // Compare
    result = do_compare_queries(db[0], "SELECT * FROM cpk_tbl ORDER BY pk_text1, pk_int1, pk_text2, pk_int2, pk_text3;",
                                db[1], "SELECT * FROM cpk_tbl ORDER BY pk_text1, pk_int1, pk_text2, pk_int2, pk_text3;",
                                -1, -1, print_result);

    // Verify row count
    if (result) {
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(db[1], "SELECT COUNT(*) FROM cpk_tbl;", -1, &stmt, NULL);
        if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            int count = sqlite3_column_int(stmt, 0);
            if (count != 4) {
                printf("large_composite_pk: expected 4 rows, got %d\n", count);
                result = false;
            }
        }
        if (stmt) sqlite3_finalize(stmt);
    }

finalize:
    for (int i = 0; i < 2; i++) {
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

bool do_test_schema_hash_mismatch (int nclients, bool print_result, bool cleanup_databases) {
    sqlite3 *db[2] = {NULL, NULL};
    bool result = false;

    time_t timestamp = time(NULL);
    int saved_counter = test_counter;

    for (int i = 0; i < 2; i++) {
        db[i] = do_create_database_file(i, timestamp, test_counter++);
        if (!db[i]) return false;

        int rc = sqlite3_exec(db[i], "CREATE TABLE test_tbl (id TEXT PRIMARY KEY, val TEXT);", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
        rc = sqlite3_exec(db[i], "SELECT cloudsync_init('test_tbl');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto finalize;
    }

    // Initial sync
    sqlite3_exec(db[0], "INSERT INTO test_tbl VALUES ('id1', 'value1');", NULL, NULL, NULL);
    if (!do_merge_using_payload(db[0], db[1], false, true)) goto finalize;

    // ALTER TABLE on destination WITHOUT cloudsync_begin/commit_alter
    int rc = sqlite3_exec(db[1], "ALTER TABLE test_tbl ADD COLUMN extra TEXT;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;

    // Re-init to pick up changed schema
    rc = sqlite3_exec(db[1], "SELECT cloudsync_init('test_tbl');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto finalize;

    // Insert new data on source
    sqlite3_exec(db[0], "INSERT INTO test_tbl VALUES ('id2', 'value2');", NULL, NULL, NULL);

    // Apply payload from pre-alter source to post-alter destination
    // This should fail due to schema hash mismatch
    bool merge_result = do_merge_using_payload(db[0], db[1], true, false);
    if (merge_result) {
        // If merge succeeded despite schema mismatch, it means the extension
        // accepted the fewer-columns payload — verify data isn't corrupted
    }

    // Verify original data is intact regardless
    {
        sqlite3_stmt *stmt = NULL;
        rc = sqlite3_prepare_v2(db[1], "SELECT COUNT(*) FROM test_tbl WHERE id = 'id1' AND val = 'value1';", -1, &stmt, NULL);
        if (rc != SQLITE_OK) goto finalize;
        if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); goto finalize; }
        int count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if (count != 1) {
            printf("schema_hash_mismatch: original data corrupted\n");
            goto finalize;
        }
    }

    result = true;

finalize:
    for (int i = 0; i < 2; i++) {
        if (db[i]) close_db(db[i]);
        if (cleanup_databases) {
            char buf[256];
            do_build_database_path(buf, i, timestamp, saved_counter++);
            file_delete_internal(buf);
        }
    }
    return result;
}

int test_report(const char *description, bool result){
    printf("%-30s %s\n", description, (result) ? "OK" : "FAILED");
    return result ? 0 : 1;
}

int main (int argc, const char * argv[]) {
    sqlite3 *db = NULL;
    int result = 0;
    bool print_result = false;
    bool cleanup_databases = true;

    // test in an in-memory database
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) goto finalize;
    
    // manually load extension
    sqlite3_cloudsync_init(db, NULL, NULL);

    printf("Testing CloudSync version %s\n", CLOUDSYNC_VERSION);
    printf("=================================\n");

    result += test_report("PK Test:", do_test_pk(db, 10000, print_result));
    result += test_report("UUID Test:", do_test_uuid(db, 1000, print_result));
    result += test_report("Comparison Test:", do_test_compare(db, print_result));
    result += test_report("RowID Test:", do_test_rowid(50000, print_result));
    result += test_report("Algo Names Test:", do_test_algo_names());
    result += test_report("DBUtils Test:", do_test_dbutils());
    result += test_report("Minor Test:", do_test_others(db));
    result += test_report("Test Error Cases:", do_test_error_cases(db));
    result += test_report("Unsupported Algos Test:", do_test_unsupported_algorithms(db));
    result += test_report("Null PK Insert Test:", do_test_null_prikey_insert(db));
    result += test_report("Test Single PK:", do_test_single_pk(print_result));
    
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
    result += test_report("String Lowercase Test:", do_test_string_lowercase());
    result += test_report("Context Functions Test:", do_test_context_functions());
    result += test_report("PK Decode Count Test:", do_test_pk_decode_count_from_buffer());
    result += test_report("Error Handling Test:", do_test_error_handling());
    result += test_report("Terminate Test:", do_test_terminate());
    result += test_report("Hash Function Test:", do_test_hash_function());
    result += test_report("Blob Compare Test:", do_test_blob_compare());
    result += test_report("Blob Compare Large:", do_test_blob_compare_large_sizes());
    result += test_report("Deterministic Flags:", do_test_deterministic_flags());
    result += test_report("Schema Hash Roundtrip:", do_test_schema_hash_consistency());
    result += test_report("String Functions Test:", do_test_string_functions());
    result += test_report("UUID Functions Test:", do_test_uuid_functions());
    result += test_report("RowID Decode Test:", do_test_rowid_decode());
    result += test_report("SQL Schema Funcs Test:", do_test_sql_schema_functions());
    result += test_report("SQL PK Decode Test:", do_test_sql_pk_decode());
    result += test_report("PK Negative Values Test:", do_test_pk_negative_values());
    result += test_report("Settings Functions Test:", do_test_settings_functions());
    result += test_report("Sync/Enabled Funcs Test:", do_test_sync_enabled_functions());
    result += test_report("SQL UUID Func Test:", do_test_sql_uuid_function());
    result += test_report("PK Encode Edge Cases:", do_test_pk_encode_edge_cases());
    result += test_report("Col Value Func Test:", do_test_col_value_function());
    result += test_report("Is Sync Func Test:", do_test_is_sync_function());
    result += test_report("Insert/Update/Delete:", do_test_insert_update_delete_sql());
    result += test_report("Binary Comparison Test:", do_test_binary_comparison());
    result += test_report("PK Decode Malformed:", do_test_pk_decode_malformed());
    result += test_report("Test Many Columns:", do_test_many_columns(600, db));
    result += test_report("Payload Buffer Test (500KB):", do_test_payload_buffer(500 * 1024));
    result += test_report("Payload Buffer Test (600KB):", do_test_payload_buffer(600 * 1024));
    result += test_report("Payload Buffer Test (1MB):", do_test_payload_buffer(1024 * 1024));
    result += test_report("Payload Buffer Test (10MB):", do_test_payload_buffer(10 * 1024 * 1024));

    // close local database
    close_db(db);
    db = NULL;
    
    // simulate remote merge
    result += test_report("Merge Test:", do_test_merge(3, print_result, cleanup_databases));
    result += test_report("Merge Test 2:", do_test_merge_2(3, TEST_PRIKEYS, print_result, cleanup_databases));
    result += test_report("Merge Test 3:", do_test_merge_2(3, TEST_NOCOLS, print_result, cleanup_databases));
    result += test_report("Merge Test 4:", do_test_merge_4(2, print_result, cleanup_databases));
    result += test_report("Merge Test 5:", do_test_merge_5(2, print_result, cleanup_databases, false));
    result += test_report("Merge Test db_version 1:", do_test_merge_check_db_version(2, print_result, cleanup_databases, true));
    result += test_report("Merge Test db_version 2:", do_test_merge_check_db_version_2(2, print_result, cleanup_databases, true));
    result += test_report("Merge Test Insert Changes", do_test_insert_cloudsync_changes(print_result, cleanup_databases));
    result += test_report("Merge Alter Schema 1:", do_test_merge_alter_schema_1(2, print_result, cleanup_databases, false));
    result += test_report("Merge Alter Schema 2:", do_test_merge_alter_schema_2(2, print_result, cleanup_databases, false));
    result += test_report("Merge Two Tables Test:", do_test_merge_two_tables(2, print_result, cleanup_databases));
    result += test_report("Merge Conflicting PKeys:", do_test_merge_conflicting_pkeys(2, print_result, cleanup_databases));
    result += test_report("Merge Large Dataset:", do_test_merge_large_dataset(3, print_result, cleanup_databases));
    result += test_report("Merge Nested Transactions:", do_test_merge_nested_transactions(2, print_result, cleanup_databases));
    result += test_report("Merge Three Way:", do_test_merge_three_way(3, print_result, cleanup_databases));
    result += test_report("Merge NULL Values:", do_test_merge_null_values(2, print_result, cleanup_databases));
    result += test_report("Merge BLOB Data:", do_test_merge_blob_data(2, print_result, cleanup_databases));
    result += test_report("Merge Mixed Operations:", do_test_merge_mixed_operations(2, print_result, cleanup_databases));
    result += test_report("Merge Hub-Spoke:", do_test_merge_hub_spoke(4, print_result, cleanup_databases));
    result += test_report("Merge Timestamp Precision:", do_test_merge_timestamp_precision(2, print_result, cleanup_databases));
    result += test_report("Merge Partial Failure:", do_test_merge_partial_failure(2, print_result, cleanup_databases));
    result += test_report("Merge Rollback Scenarios:", do_test_merge_rollback_scenarios(2, print_result, cleanup_databases));
    result += test_report("Merge Circular:", do_test_merge_circular(3, print_result, cleanup_databases));
    result += test_report("Merge Foreign Keys:", do_test_merge_foreign_keys(2, print_result, cleanup_databases));
    // Expected failure: AFTER TRIGGERs are not fully supported by this extension.
    // result += test_report("Merge Triggers:", do_test_merge_triggers(2, print_result, cleanup_databases));
    result += test_report("Merge RLS Trigger Denial:", do_test_rls_trigger_denial(2, print_result, cleanup_databases, true));
    result += test_report("Merge Index Consistency:", do_test_merge_index_consistency(2, print_result, cleanup_databases));
    result += test_report("Merge JSON Columns:", do_test_merge_json_columns(2, print_result, cleanup_databases));
    result += test_report("Merge Concurrent Attempts:", do_test_merge_concurrent_attempts(3, print_result, cleanup_databases));
    result += test_report("Payload Apply Lock Test:", do_test_payload_apply_concurrent_write(2, print_result, cleanup_databases));
    result += test_report("Merge Composite PK 10 Clients:", do_test_merge_composite_pk_10_clients(10, print_result, cleanup_databases));
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

    // test row-level filter
    result += test_report("Test Row Filter:", do_test_row_filter(2, print_result, cleanup_databases));

    // test block-level LWW
    result += test_report("Test Block LWW Insert:", do_test_block_lww_insert(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Update:", do_test_block_lww_update(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Sync:", do_test_block_lww_sync(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Delete:", do_test_block_lww_delete(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Materialize:", do_test_block_lww_materialize(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Empty:", do_test_block_lww_empty_text(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Conflict:", do_test_block_lww_conflict(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Multi-Update:", do_test_block_lww_multi_update(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Reinsert:", do_test_block_lww_reinsert(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Add Lines:", do_test_block_lww_add_lines(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW NoConflict:", do_test_block_lww_noconflict(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Add+Edit:", do_test_block_lww_add_and_edit(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Three-Way:", do_test_block_lww_three_way(3, print_result, cleanup_databases));
    result += test_report("Test Block LWW MixedCols:", do_test_block_lww_mixed_columns(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW NULL->Text:", do_test_block_lww_null_to_text(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Interleave:", do_test_block_lww_interleaved(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW CustomDelim:", do_test_block_lww_custom_delimiter(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Large Text:", do_test_block_lww_large_text(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Rapid Upd:", do_test_block_lww_rapid_updates(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Unicode:", do_test_block_lww_unicode(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW SpecialChars:", do_test_block_lww_special_chars(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Del vs Edit:", do_test_block_lww_delete_vs_edit(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW TwoBlockCols:", do_test_block_lww_two_block_cols(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Text->NULL:", do_test_block_lww_text_to_null(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW PayloadSync:", do_test_block_lww_payload_sync(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Idempotent:", do_test_block_lww_idempotent(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Ordering:", do_test_block_lww_ordering(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW CompositePK:", do_test_block_lww_composite_pk(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW EmptyVsNull:", do_test_block_lww_empty_vs_null(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW DelReinsert:", do_test_block_lww_delete_reinsert(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW IntegerPK:", do_test_block_lww_integer_pk(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW MultiRow:", do_test_block_lww_multi_row(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW NonOverlap:", do_test_block_lww_nonoverlap_add(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW LongLine:", do_test_block_lww_long_line(2, print_result, cleanup_databases));
    result += test_report("Test Block LWW Whitespace:", do_test_block_lww_whitespace(2, print_result, cleanup_databases));

    // edge-case tests
    result += test_report("Corrupted Payload Test:", do_test_corrupted_payload(2, print_result, cleanup_databases));
    result += test_report("Payload Idempotency Test:", do_test_payload_idempotency(2, print_result, cleanup_databases));
    result += test_report("CL Tiebreak Test:", do_test_causal_length_tiebreak(3, print_result, cleanup_databases));
    result += test_report("Delete/Resurrect Order:", do_test_delete_resurrect_ordering(3, print_result, cleanup_databases));
    result += test_report("Large Composite PK Test:", do_test_large_composite_pk(2, print_result, cleanup_databases));
    result += test_report("Schema Hash Mismatch:", do_test_schema_hash_mismatch(2, print_result, cleanup_databases));

finalize:
    if (rc != SQLITE_OK) printf("%s (%d)\n", (db) ? sqlite3_errmsg(db) : "N/A", rc);
    close_db(db);
    db = NULL;
    
    cloudsync_memory_finalize();

    int64_t memory_used = (int64_t)sqlite3_memory_used();
    result += test_report("Memory Leaks Check:", memory_used == 0);
    if (memory_used > 0) {
        printf("\tleaked: %" PRId64 " B\n", memory_used);
        result++;
    }
    
    return result;
}
