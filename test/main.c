//
//  main.c
//  sqlite-sync
//
//  Created by Gioele Cantoni on 05/06/25.
//  Set CONNECTION_STRING, APIKEY and WEBLITE environment variables before running this test.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include "utils.h"
#include "sqlite3.h"

// Define the number of simulated peers, when it's 0 it skips the peer test.
#if defined(__linux__) && !defined(__ANDROID__)
#define PEERS           0
#else
#define PEERS           5
#endif

#ifdef PEERS
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#endif // PEERS

#ifdef CLOUDSYNC_LOAD_FROM_SOURCES
#include "cloudsync.h"
#endif

#define DB_PATH         "health-track.sqlite"
#define EXT_PATH        "./dist/cloudsync"
#define RCHECK          if (rc != SQLITE_OK) goto abort_test;
#define ERROR_MSG       if (rc != SQLITE_OK) printf("Error: %s\n", sqlite3_errmsg(db));
#define TERMINATE       if (db) { db_exec(db, "SELECT cloudsync_terminate();"); }
#define ABORT_TEST      abort_test: ERROR_MSG TERMINATE if (db) sqlite3_close(db); return rc;

typedef enum { PRINT, NOPRINT, INTGR, GT0 } expected_type;

typedef struct {
    expected_type type;
    union {
        int i;
        const char *s; // for future use, if needed
    } value;
} expected_t;

static int callback(void *data, int argc, char **argv, char **names) {
    expected_t *expect = (expected_t *)data;

    switch(expect->type) {
        case NOPRINT: break;
        case PRINT:
            for (int i = 0; i < argc; i++) {
                printf("%s: %s ", names[i], argv[i] ? argv[i] : "NULL");
            }
            printf("\n");
            return SQLITE_OK;

        case INTGR:
            if(argc == 1){
                int res = atoi(argv[0]);

                if(res != expect->value.i){
                    printf("Error: expected from %s: %d, got %d\n", names[0], expect->value.i, res);
                    return SQLITE_ERROR;
                }

            } else goto multiple_columns;
            break;

        case GT0:
            if(argc == 1){
                int res = atoi(argv[0]);

                if(!(res > 0)){
                    printf("Error: expected from %s: to be greater than 0, got %d\n", names[0], res);
                    return SQLITE_ERROR;
                }

            } else goto multiple_columns;
            break;

        default:
            printf("Error: unknown expect type\n");
            return SQLITE_ERROR;
    }
    
    return SQLITE_OK;

multiple_columns:
    printf("Error: expected 1 column, got %d\n", argc);
    return SQLITE_ERROR;
}

int db_exec (sqlite3 *db, const char *sql) {
    expected_t data;
    data.type = NOPRINT;

    int rc = sqlite3_exec(db, sql, callback, &data, NULL);
    if (rc != SQLITE_OK) printf("Error while executing %s: %s\n", sql, sqlite3_errmsg(db));
    return rc;
}

int db_print (sqlite3 *db, const char *sql) {
    expected_t data;
    data.type = PRINT;

    int rc = sqlite3_exec(db, sql, callback, &data, NULL);
    if (rc != SQLITE_OK) printf("Error while executing %s: %s\n", sql, sqlite3_errmsg(db));
    return rc;
}

int db_expect_int (sqlite3 *db, const char *sql, int expect) {
    expected_t data;
    data.type = INTGR;
    data.value.i = expect;

    int rc = sqlite3_exec(db, sql, callback, &data, NULL);
    if (rc != SQLITE_OK) printf("Error while executing %s: %s\n", sql, sqlite3_errmsg(db));
    return rc;
}

int db_expect_gt0 (sqlite3 *db, const char *sql) {
    expected_t data;
    data.type = GT0;

    int rc = sqlite3_exec(db, sql, callback, &data, NULL);
    if (rc != SQLITE_OK) printf("Error while executing %s: %s\n", sql, sqlite3_errmsg(db));
    return rc;
}

int open_load_ext(const char *db_path, sqlite3 **out_db) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(db_path, &db);
    RCHECK
    
#ifdef CLOUDSYNC_LOAD_FROM_SOURCES
    rc = sqlite3_cloudsync_init(db, NULL, NULL);
#else
    // enable load extension
    rc = sqlite3_enable_load_extension(db, 1);
    RCHECK

    rc = db_exec(db, "SELECT load_extension('"EXT_PATH"');");
    RCHECK
#endif
    
    *out_db = db;
    return rc;

ABORT_TEST
}

// MARK: -

int db_init (sqlite3 *db){

    int rc = db_exec(db, "\
        CREATE TABLE IF NOT EXISTS users (\
            id TEXT PRIMARY KEY NOT NULL,\
            name TEXT UNIQUE NOT NULL DEFAULT ''\
        );\
        CREATE TABLE IF NOT EXISTS activities (\
            id TEXT PRIMARY KEY NOT NULL,\
            user_id TEXT,\
            km REAL,\
            bpm INTEGER,\
            time TEXT,\
            activity_type TEXT NOT NULL DEFAULT 'running',\
            FOREIGN KEY(user_id) REFERENCES users(id)\
        );\
        CREATE TABLE IF NOT EXISTS workouts (\
            id TEXT PRIMARY KEY NOT NULL,\
            assigned_user_id TEXT,\
            day_of_week TEXT,\
            km REAL,\
            max_time TEXT\
        );\
    ");

ERROR_MSG
    return rc;

}

int test_init (const char *db_path, int init) {
    int rc = SQLITE_OK;
    
    sqlite3 *db = NULL;
    rc = open_load_ext(db_path, &db); RCHECK

    if(init){
        rc = db_init(db);
        RCHECK
    }

    rc = db_exec(db, "SELECT cloudsync_init('users');"); RCHECK
    rc = db_exec(db, "SELECT cloudsync_init('activities');"); RCHECK
    rc = db_exec(db, "SELECT cloudsync_init('workouts');"); RCHECK

    // init network with connection string + apikey
    char network_init[512];
    const char* conn_str = getenv("CONNECTION_STRING");
    const char* apikey = getenv("APIKEY");
    if (!conn_str || !apikey) {
        fprintf(stderr, "Error: CONNECTION_STRING or APIKEY not set.\n");
        exit(1);
    }
    snprintf(network_init, sizeof(network_init), "SELECT cloudsync_network_init('%s?apikey=%s');", conn_str, apikey);
    rc = db_exec(db, network_init); RCHECK

    rc = db_expect_int(db, "SELECT COUNT(*) as count FROM activities;", 0); RCHECK
    rc = db_expect_int(db, "SELECT COUNT(*) as count FROM workouts;", 0); RCHECK
    char value[UUID_STR_MAXLEN];
    cloudsync_uuid_v7_string(value, true);
    char sql[256];
    snprintf(sql, sizeof(sql), "INSERT INTO users (id, name) VALUES ('%s', '%s');", value, value);
    rc = db_exec(db, sql); RCHECK
    rc = db_expect_int(db, "SELECT COUNT(*) as count FROM users;", 1); RCHECK
    rc = db_expect_gt0(db, "SELECT cloudsync_network_sync(250,10);"); RCHECK
    rc = db_expect_gt0(db, "SELECT COUNT(*) as count FROM users;"); RCHECK
    rc = db_expect_gt0(db, "SELECT COUNT(*) as count FROM activities;"); RCHECK
    rc = db_expect_int(db, "SELECT COUNT(*) as count FROM workouts;", 0); RCHECK
    rc = db_exec(db, "SELECT cloudsync_terminate();");
    
ABORT_TEST
}

int test_is_enabled(const char *db_path) {
    sqlite3 *db = NULL;
    int rc = open_load_ext(db_path, &db);

    rc = db_expect_int(db, "SELECT cloudsync_is_enabled('users');", 1); RCHECK
    rc = db_expect_int(db, "SELECT cloudsync_is_enabled('activities');", 1); RCHECK
    rc = db_expect_int(db, "SELECT cloudsync_is_enabled('workouts');", 1);

ABORT_TEST
}

int test_db_version(const char *db_path) {
    sqlite3 *db = NULL;
    int rc = open_load_ext(db_path, &db);
    
    rc = db_expect_gt0(db, "SELECT cloudsync_db_version();"); RCHECK
    rc = db_expect_gt0(db, "SELECT cloudsync_db_version_next();");

ABORT_TEST
}

int test_enable_disable(const char *db_path) {
    sqlite3 *db = NULL;
    int rc = open_load_ext(db_path, &db); RCHECK

    char value[UUID_STR_MAXLEN];
    cloudsync_uuid_v7_string(value, true);
    char sql[256];

    rc = db_exec(db, "SELECT cloudsync_init('*');"); RCHECK
    rc = db_exec(db, "SELECT cloudsync_disable('users');"); RCHECK

    snprintf(sql, sizeof(sql), "INSERT INTO users (id, name) VALUES ('%s', '%s');", value, value);
    rc = db_exec(db, sql); RCHECK

    rc = db_exec(db, "SELECT cloudsync_enable('users');"); RCHECK

    snprintf(sql, sizeof(sql), "INSERT INTO users (id, name) VALUES ('%s-should-sync', '%s-should-sync');", value, value);
    rc = db_exec(db, sql); RCHECK

    // init network with connection string + apikey
    char network_init[512];
    const char* conn_str = getenv("CONNECTION_STRING");
    const char* apikey = getenv("APIKEY");
    if (!conn_str || !apikey) {
        fprintf(stderr, "Error: CONNECTION_STRING or APIKEY not set.\n");
        exit(1);
    }
    snprintf(network_init, sizeof(network_init), "SELECT cloudsync_network_init('%s?apikey=%s');", conn_str, apikey);
    rc = db_exec(db, network_init); RCHECK

    rc = db_exec(db, "SELECT cloudsync_network_send_changes();"); RCHECK
    rc = db_exec(db, "SELECT cloudsync_cleanup('*');");

    // give the server the time to apply the latest sent changes, it is an async job
    sqlite3_sleep(5000);
    
    sqlite3 *db2 = NULL;
    rc = open_load_ext(":memory:", &db2); RCHECK
    rc = db_init(db2); RCHECK

    rc = db_exec(db2, "SELECT cloudsync_init('*');"); RCHECK

    // init network with connection string + apikey
    rc = db_exec(db2, network_init); RCHECK

    rc = db_expect_gt0(db2, "SELECT cloudsync_network_sync(250,10);"); RCHECK

    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM users WHERE name='%s';", value);
    rc = db_expect_int(db2, sql, 0); RCHECK

    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM users WHERE name='%s-should-sync';", value);
    rc = db_expect_int(db2, sql, 1); RCHECK

    rc = db_exec(db2, "SELECT cloudsync_terminate();"); RCHECK
    
    sqlite3_close(db2);

ABORT_TEST
}

int version(void){
    sqlite3 *db = NULL;
    int rc = open_load_ext(":memory:", &db);

    rc = db_print(db, "SELECT cloudsync_version();");
    RCHECK

ABORT_TEST
}

// MARK: -

int test_report(const char *description, int rc){
    printf("%-24s %s\n", description, rc ? "FAILED" : "OK");
    return rc;
}

#ifdef PEERS
#ifdef _WIN32
DWORD WINAPI worker(LPVOID arg) {
#else
void* worker(void* arg) {
#endif
    int thread_id = *(int*)arg;
    int result = 0;

    char description[32];
    snprintf(description, sizeof(description), "%d/%d Peer Test", thread_id+1, PEERS);
    result = test_init(":memory:", 1);
    if(test_report(description, result)){
        printf("PEER %d FAIL.\n", thread_id+1);
        // Return error code instead of exiting entire process
        return (void*)(intptr_t)(thread_id+1);
    }

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
#endif // PEERS

int main (void) {
    int rc = SQLITE_OK;
    
    cloudsync_memory_init(1);
    
    printf("\n\nIntegration Test ");
    rc += version();
    printf("===========================================\n");
    test_report("Version Test:", rc);

    sqlite3 *db = NULL;
    rc += open_load_ext(DB_PATH, &db);
    rc += db_init(db);
    if (db) sqlite3_close(db);

    rc += test_report("Init+Sync Test:", test_init(DB_PATH, 0));
    rc += test_report("Is Enabled Test:", test_is_enabled(DB_PATH));
    rc += test_report("DB Version Test:", test_db_version(DB_PATH));
    rc += test_report("Enable Disable Test:", test_enable_disable(DB_PATH));

    remove(DB_PATH); // remove the database file

    #ifdef PEERS
    #ifdef _WIN32
    HANDLE threads[PEERS];
    #else
    pthread_t threads[PEERS];
    #endif
    int thread_ids[PEERS];
    int threads_created = 0;
    int thread_errors = 0;

    // Initialize threads array to invalid values for cleanup
    #ifdef _WIN32
    for (int i = 0; i < PEERS; i++) {
        threads[i] = NULL;
    }
    #else
    memset(threads, 0, sizeof(threads));
    #endif

    // Create threads with proper error handling
    for (int i = 0; i < PEERS; i++) {
        thread_ids[i] = i;
        #ifdef _WIN32
        threads[i] = CreateThread(NULL, 0, worker, &thread_ids[i], 0, NULL);
        if (threads[i] == NULL) {
            fprintf(stderr, "CreateThread failed for thread %d: %lu\n", i, GetLastError());
            thread_errors++;
            break; // Stop creating more threads on failure
        }
        #else
        int pthread_result = pthread_create(&threads[i], NULL, worker, &thread_ids[i]);
        if (pthread_result != 0) {
            fprintf(stderr, "pthread_create failed for thread %d: %s\n", i, strerror(pthread_result));
            threads[i] = 0; // Mark as invalid
            thread_errors++;
            break; // Stop creating more threads on failure
        }
        #endif
        threads_created++;
    }

    // Wait for all successfully created threads to finish and collect results
    #ifdef _WIN32
    if (threads_created > 0) {
        DWORD wait_result = WaitForMultipleObjects(threads_created, threads, TRUE, INFINITE);
        if (wait_result == WAIT_FAILED) {
            fprintf(stderr, "WaitForMultipleObjects failed: %lu\n", GetLastError());
            thread_errors++;
        }
    }
    #endif

    // Join threads and collect exit codes
    for (int i = 0; i < threads_created; i++) {
        #ifdef _WIN32
        if (threads[i] != NULL) {
            DWORD exit_code;
            if (GetExitCodeThread(threads[i], &exit_code) && exit_code != 0) {
                thread_errors++;
                printf("Thread %d failed with exit code %lu\n", i, exit_code);
            }
            CloseHandle(threads[i]);
            threads[i] = NULL;
        }
        #else
        if (threads[i] != 0) {
            void* thread_result = NULL;
            int join_result = pthread_join(threads[i], &thread_result);
            if (join_result != 0) {
                fprintf(stderr, "pthread_join failed for thread %d: %s\n", i, strerror(join_result));
                thread_errors++;
            } else if (thread_result != NULL) {
                int exit_code = (int)(intptr_t)thread_result;
                thread_errors++;
                printf("Thread %d failed with exit code %d\n", i, exit_code);
            }
            threads[i] = 0;
        }
        #endif
    }

    // Update return code if any thread errors occurred
    if (thread_errors > 0) {
        printf("Threading test failed: %d thread(s) had errors\n", thread_errors);
        rc += thread_errors;
    }
    #endif // PEERS
    
    cloudsync_memory_finalize();
    
    printf("\n");
    return rc;
}
