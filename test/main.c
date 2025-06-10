//
//  main.c
//  sqlite-sync
//
//  Created by Gioele Cantoni on 05/06/25.
//  Set CONNECTION_STRING, APIKEY and WEBLITE environment variables before running this test.
//

#include <stdio.h>
#include <stdlib.h>
#include "sqlite3.h"

#define DB_PATH         "health-track.sqlite"
#define EXT_PATH       "./dist/cloudsync"
#define ABORT_TEST abort_test: if (rc != SQLITE_OK) printf("Error: %s\n", sqlite3_errmsg(db)); if (db) sqlite3_close(db); return rc;

typedef enum { PRINT, NOPRINT, INT, GT0 } expected_type;

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

        case INT:
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
    data.type = INT;
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
    if (rc != SQLITE_OK) goto abort_test;
    
    // enable load extension
    rc = sqlite3_enable_load_extension(db, 1);
    if (rc != SQLITE_OK) goto abort_test;

    rc = db_exec(db, "SELECT load_extension('"EXT_PATH"');");
    if (rc != SQLITE_OK) goto abort_test;

    *out_db = db;
    return rc;

ABORT_TEST
}

// MARK: -

int db_init (const char *db_path){
    sqlite3 *db = NULL;
    int rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) goto abort_test;

    rc = db_exec(db, "\
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
);");
    if (rc != SQLITE_OK) goto abort_test;

ABORT_TEST
}

int test_init (const char *db_path) {
    sqlite3 *db = NULL;
    int rc = open_load_ext(db_path, &db);
    
    rc = db_exec(db, "SELECT cloudsync_init('users');"); if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "SELECT cloudsync_init('activities');"); if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "SELECT cloudsync_init('workouts');"); if (rc != SQLITE_OK) goto abort_test;

    // init network with connection string + apikey
    char network_init[512];
    snprintf(network_init, sizeof(network_init), "SELECT cloudsync_network_init('%s?apikey=%s');", getenv("CONNECTION_STRING"), getenv("APIKEY"));
    rc = db_exec(db, network_init); if (rc != SQLITE_OK) goto abort_test;

    rc = db_expect_int(db, "SELECT COUNT(*) as count FROM users;", 0); if (rc != SQLITE_OK) goto abort_test;
    rc = db_expect_int(db, "SELECT COUNT(*) as count FROM activities;", 0); if (rc != SQLITE_OK) goto abort_test;
    rc = db_expect_int(db, "SELECT COUNT(*) as count FROM workouts;", 0); if (rc != SQLITE_OK) goto abort_test;
    rc = db_expect_gt0(db, "SELECT cloudsync_network_sync();"); if (rc != SQLITE_OK) goto abort_test;
    rc = db_expect_gt0(db, "SELECT COUNT(*) as count FROM users;"); if (rc != SQLITE_OK) goto abort_test;
    rc = db_expect_gt0(db, "SELECT COUNT(*) as count FROM activities;"); if (rc != SQLITE_OK) goto abort_test;
    rc = db_expect_int(db, "SELECT COUNT(*) as count FROM workouts;", 0); if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "SELECT cloudsync_terminate();");
    
ABORT_TEST
}

int test_is_enabled(const char *db_path) {
    sqlite3 *db = NULL;
    int rc = open_load_ext(db_path, &db);

    rc = db_expect_int(db, "SELECT cloudsync_is_enabled('users');", 1); if (rc != SQLITE_OK) goto abort_test;
    rc = db_expect_int(db, "SELECT cloudsync_is_enabled('activities');", 1); if (rc != SQLITE_OK) goto abort_test;
    rc = db_expect_int(db, "SELECT cloudsync_is_enabled('workouts');", 1);

ABORT_TEST
}

int test_db_version(const char *db_path) {
    sqlite3 *db = NULL;
    int rc = open_load_ext(db_path, &db);
    
    rc = db_expect_gt0(db, "SELECT cloudsync_db_version();"); if (rc != SQLITE_OK) goto abort_test;
    rc = db_expect_gt0(db, "SELECT cloudsync_db_version_next();");

ABORT_TEST
}

int test_enable_disable(const char *db_path) {
    sqlite3 *db = NULL;
    int rc = open_load_ext(db_path, &db);

    rc = db_exec(db, "SELECT cloudsync_init('*');"); if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "SELECT cloudsync_disable('users');"); if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "INSERT INTO users (id, name) VALUES ('12afb', 'provaCmeaakbefa');"); if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "SELECT cloudsync_enable('users');"); if (rc != SQLITE_OK) goto abort_test;

    // init network with connection string + apikey
    char network_init[512];
    snprintf(network_init, sizeof(network_init), "SELECT cloudsync_network_init('%s?apikey=%s');", getenv("CONNECTION_STRING"), getenv("APIKEY"));
    rc = db_exec(db, network_init); if (rc != SQLITE_OK) goto abort_test;

    rc = db_exec(db, "SELECT cloudsync_network_sync();"); if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "SELECT cloudsync_cleanup('*');");

ABORT_TEST
}

int version(){
    sqlite3 *db = NULL;
    int rc = open_load_ext(":memory:", &db);

    rc = db_print(db, "SELECT cloudsync_version();");
    if (rc != SQLITE_OK) goto abort_test;

ABORT_TEST
}

// MARK: -

int test_report(const char *description, int rc){
    printf("%-24s %s\n", description, rc ? "FAILED" : "OK");
    return rc;
}

int main (void) {
    int rc = SQLITE_OK;
    
    printf("\n\nIntegration Test ");
    rc += version();
    printf("===========================================\n");
    test_report("Version Test:", rc);

    //rc += db_init(DB_PATH); // fix first the schema hash comparison
    rc += test_report("Init+Sync Test:", test_init(DB_PATH));
    rc += test_report("Is Enabled Test:", test_is_enabled(DB_PATH));
    rc += test_report("DB Version Test:", test_db_version(DB_PATH));
    rc += test_report("Enable Disable Test:", test_enable_disable(DB_PATH));

    remove(DB_PATH); // clean up the database file
    printf("\n");
    return rc;
}
