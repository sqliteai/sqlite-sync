//
//  integration.c
//  cloudsync
//
//  Created by Gioele Cantoni on 05/06/25.
//  Set INTEGRATION_TEST_OFFLINE_DATABASE_ID and INTEGRATION_TEST_DATABASE_ID environment variables before running this test.
//  Set INTEGRATION_TEST_CHUNKED_DATABASE_ID to enable the chunked-payload e2e test against an isolated remote database.
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
#include "cloudsync_sqlite.h"
#endif

#define DB_PATH         "health-track.sqlite"
#define EXT_PATH        "./dist/cloudsync"
#define TEST_SKIPPED    100001
#define RCHECK          if (rc != SQLITE_OK) goto abort_test;
#define ERROR_MSG       if (rc != SQLITE_OK) printf("Error: %s\n", sqlite3_errmsg(db));
#define TERMINATE       if (db) { db_exec(db, "SELECT cloudsync_terminate();"); }
#define ABORT_TEST      abort_test: ERROR_MSG TERMINATE if (db) sqlite3_close(db); return rc;

typedef enum { PRINT, NOPRINT, INTGR, GT0, STR } expected_type;

typedef struct {
    expected_type type;
    union {
        int i;
        const char *s; // for future use, if needed
    } value;
} expected_t;

int open_load_ext(const char *db_path, sqlite3 **out_db);

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

        case STR:
            if(argc == 1){
                if(!argv[0] || strcmp(argv[0], expect->value.s) != 0){
                    printf("Error: expected from %s: \"%s\", got \"%s\"\n", names[0], expect->value.s, argv[0] ? argv[0] : "NULL");
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

int db_expect_str (sqlite3 *db, const char *sql, const char *expect) {
    expected_t data;
    data.type = STR;
    data.value.s = expect;

    int rc = sqlite3_exec(db, sql, callback, &data, NULL);
    if (rc != SQLITE_OK) printf("Error while executing %s: %s\n", sql, sqlite3_errmsg(db));
    return rc;
}

int db_select_int (sqlite3 *db, const char *sql, int *out) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        printf("Error while preparing %s: %s\n", sql, sqlite3_errmsg(db));
        return rc;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        printf("Error while executing %s: expected one row, got rc=%d\n", sql, rc);
        sqlite3_finalize(stmt);
        return SQLITE_ERROR;
    }

    *out = sqlite3_column_int(stmt, 0);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        printf("Error while executing %s: expected one row only, got rc=%d\n", sql, rc);
        sqlite3_finalize(stmt);
        return SQLITE_ERROR;
    }

    return sqlite3_finalize(stmt);
}

// Reads the receive probe — chunks (int), complete (int), error (text) — from a
// single row of one network call. error_out is set to "" when receive.error is
// absent (the common success case) and to the message otherwise, so a swallowed
// client-side apply failure surfaces instead of timing out as "not received".
// The call must be a bare/read-only SELECT (e.g. a subquery), never
// CREATE TABLE ... AS SELECT, or the apply path cannot open its savepoint
// ("SQL statements in progress") and the download is silently dropped.
int db_select_receive (sqlite3 *db, const char *sql, int *chunks, int *complete, char *error_out, int error_len) {
    if (error_len > 0) error_out[0] = '\0';

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        printf("Error while preparing %s: %s\n", sql, sqlite3_errmsg(db));
        return rc;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        printf("Error while executing %s: expected one row, got rc=%d\n", sql, rc);
        sqlite3_finalize(stmt);
        return SQLITE_ERROR;
    }

    *chunks = sqlite3_column_int(stmt, 0);
    *complete = sqlite3_column_int(stmt, 1);
    const unsigned char *err = sqlite3_column_text(stmt, 2);
    if (err && error_len > 0) snprintf(error_out, error_len, "%s", (const char *)err);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        printf("Error while executing %s: expected one row only, got rc=%d\n", sql, rc);
        sqlite3_finalize(stmt);
        return SQLITE_ERROR;
    }

    return sqlite3_finalize(stmt);
}

int db_expect_min (sqlite3 *db, const char *sql, int expect_min) {
    int value = 0;
    int rc = db_select_int(db, sql, &value);
    if (rc != SQLITE_OK) return rc;
    if (value < expect_min) {
        printf("Error: expected %s to be >= %d, got %d\n", sql, expect_min, value);
        return SQLITE_ERROR;
    }
    return SQLITE_OK;
}

int integration_network_init(sqlite3 *db, const char *database_id, char *network_init, size_t network_init_len) {
    if (!database_id) {
        fprintf(stderr, "Error: integration database ID not set.\n");
        return SQLITE_ERROR;
    }

    const char* custom_address = getenv("INTEGRATION_TEST_CLOUDSYNC_ADDRESS");
    if (custom_address) {
        snprintf(network_init, network_init_len,
            "SELECT cloudsync_network_init_custom('%s', '%s');", custom_address, database_id);
    } else {
        snprintf(network_init, network_init_len,
            "SELECT cloudsync_network_init('%s');", database_id);
    }

    int rc = db_exec(db, network_init);
    if (rc != SQLITE_OK) return rc;

    const char* apikey = getenv("INTEGRATION_TEST_APIKEY");
    if (apikey) {
        char set_apikey[512];
        snprintf(set_apikey, sizeof(set_apikey),
            "SELECT cloudsync_network_set_apikey('%s');", apikey);
        rc = db_exec(db, set_apikey);
    }

    return rc;
}

int test_chunked_schema_init(sqlite3 *db) {
    int rc = db_exec(db,
        "CREATE TABLE IF NOT EXISTS chunked_payload_items ("
        "id TEXT PRIMARY KEY NOT NULL,"
        "body TEXT NOT NULL DEFAULT ''"
        ");");
    if (rc != SQLITE_OK) return rc;

    return db_exec(db, "SELECT cloudsync_init('chunked_payload_items');");
}

int test_chunked_failure_schema_init(sqlite3 *db) {
    int rc = db_exec(db,
        "CREATE TABLE IF NOT EXISTS chunked_payload_failure_items ("
        "id TEXT PRIMARY KEY NOT NULL,"
        "body TEXT NOT NULL DEFAULT ''"
        ");");
    if (rc != SQLITE_OK) return rc;

    return db_exec(db, "SELECT cloudsync_init('chunked_payload_failure_items');");
}

int test_chunked_sender_open(sqlite3 **sender, char *network_init, size_t network_init_len) {
    const char* test_db_id = getenv("INTEGRATION_TEST_CHUNKED_DATABASE_ID");
    if (!test_db_id || !*test_db_id) {
        return TEST_SKIPPED;
    }

    int rc = open_load_ext(":memory:", sender);
    if (rc != SQLITE_OK) return rc;
    rc = integration_network_init(*sender, test_db_id, network_init, network_init_len);
    if (rc != SQLITE_OK) return rc;

    return rc;
}

int test_chunked_pair_open(sqlite3 **sender, sqlite3 **receiver, char *network_init, size_t network_init_len) {
    const char* test_db_id = getenv("INTEGRATION_TEST_CHUNKED_DATABASE_ID");
    if (!test_db_id || !*test_db_id) {
        return TEST_SKIPPED;
    }

    int rc = open_load_ext(":memory:", sender);
    if (rc != SQLITE_OK) return rc;
    rc = test_chunked_schema_init(*sender);
    if (rc != SQLITE_OK) return rc;
    rc = integration_network_init(*sender, test_db_id, network_init, network_init_len);
    if (rc != SQLITE_OK) return rc;

    rc = open_load_ext(":memory:", receiver);
    if (rc != SQLITE_OK) return rc;
    rc = test_chunked_schema_init(*receiver);
    if (rc != SQLITE_OK) return rc;
    rc = db_exec(*receiver, network_init);
    if (rc != SQLITE_OK) return rc;

    const char* apikey = getenv("INTEGRATION_TEST_APIKEY");
    if (apikey) {
        char set_apikey[512];
        snprintf(set_apikey, sizeof(set_apikey),
            "SELECT cloudsync_network_set_apikey('%s');", apikey);
        rc = db_exec(*receiver, set_apikey);
    }

    return rc;
}

void test_chunked_pair_close(sqlite3 *sender, sqlite3 *receiver) {
    if (sender) {
        db_exec(sender, "SELECT cloudsync_terminate();");
        sqlite3_close(sender);
    }
    if (receiver) {
        db_exec(receiver, "SELECT cloudsync_terminate();");
        sqlite3_close(receiver);
    }
}

void test_chunked_sender_close(sqlite3 *sender) {
    if (sender) {
        db_exec(sender, "SELECT cloudsync_terminate();");
        sqlite3_close(sender);
    }
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

    // init network
    char network_init[1024];
    const char* test_db_id = getenv("INTEGRATION_TEST_DATABASE_ID");
    if (!test_db_id) {
        fprintf(stderr, "Error: INTEGRATION_TEST_DATABASE_ID not set.\n");
        exit(1);
    }
    const char* custom_address = getenv("INTEGRATION_TEST_CLOUDSYNC_ADDRESS");
    if (custom_address) {
        snprintf(network_init, sizeof(network_init),
            "SELECT cloudsync_network_init_custom('%s', '%s');", custom_address, test_db_id);
    } else {
        snprintf(network_init, sizeof(network_init),
            "SELECT cloudsync_network_init('%s');", test_db_id);
    }
    rc = db_exec(db, network_init); RCHECK

    const char* apikey = getenv("INTEGRATION_TEST_APIKEY");
    if (apikey) {
        char set_apikey[512];
        snprintf(set_apikey, sizeof(set_apikey),
            "SELECT cloudsync_network_set_apikey('%s');", apikey);
        rc = db_exec(db, set_apikey); RCHECK
    }

    rc = db_expect_int(db, "SELECT COUNT(*) as count FROM activities;", 0); RCHECK
    rc = db_expect_int(db, "SELECT COUNT(*) as count FROM workouts;", 0); RCHECK
    char value[UUID_STR_MAXLEN];
    cloudsync_uuid_v7_string(value, true);
    char sql[256];
    snprintf(sql, sizeof(sql), "INSERT INTO users (id, name) VALUES ('%s', '%s');", value, value);
    rc = db_exec(db, sql); RCHECK
    rc = db_expect_int(db, "SELECT COUNT(*) as count FROM users;", 1); RCHECK
    rc = db_expect_gt0(db, "SELECT cloudsync_network_sync(250,10) ->> '$.receive.rows';"); RCHECK
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

    rc = db_exec(db, "SELECT cloudsync_init('users');"); RCHECK
    rc = db_exec(db, "SELECT cloudsync_init('activities');"); RCHECK
    rc = db_exec(db, "SELECT cloudsync_init('workouts');"); RCHECK
    rc = db_exec(db, "SELECT cloudsync_disable('users');"); RCHECK

    snprintf(sql, sizeof(sql), "INSERT INTO users (id, name) VALUES ('%s', '%s');", value, value);
    //rc = db_exec(db, sql); RCHECK

    rc = db_exec(db, "SELECT cloudsync_enable('users');"); RCHECK

    snprintf(sql, sizeof(sql), "INSERT INTO users (id, name) VALUES ('%s-should-sync', '%s-should-sync');", value, value);
    rc = db_exec(db, sql); RCHECK

    // init network
    char network_init[1024];
    const char* test_db_id = getenv("INTEGRATION_TEST_DATABASE_ID");
    if (!test_db_id) {
        fprintf(stderr, "Error: INTEGRATION_TEST_DATABASE_ID not set.\n");
        exit(1);
    }
    const char* custom_address = getenv("INTEGRATION_TEST_CLOUDSYNC_ADDRESS");
    if (custom_address) {
        snprintf(network_init, sizeof(network_init),
            "SELECT cloudsync_network_init_custom('%s', '%s');", custom_address, test_db_id);
    } else {
        snprintf(network_init, sizeof(network_init),
            "SELECT cloudsync_network_init('%s');", test_db_id);
    }
    rc = db_exec(db, network_init); RCHECK

    const char* apikey = getenv("INTEGRATION_TEST_APIKEY");
    if (apikey) {
        char set_apikey[512];
        snprintf(set_apikey, sizeof(set_apikey),
            "SELECT cloudsync_network_set_apikey('%s');", apikey);
        rc = db_exec(db, set_apikey); RCHECK
    }

    rc = db_exec(db, "SELECT cloudsync_network_send_changes();"); RCHECK
    rc = db_exec(db, "SELECT cloudsync_cleanup('users');"); RCHECK
    rc = db_exec(db, "SELECT cloudsync_cleanup('activities');"); RCHECK
    rc = db_exec(db, "SELECT cloudsync_cleanup('workouts');"); RCHECK

    // give the server the time to apply the latest sent changes, it is an async job
    sqlite3_sleep(5000);
    
    sqlite3 *db2 = NULL;
    rc = open_load_ext(":memory:", &db2); RCHECK
    rc = db_init(db2); RCHECK

    rc = db_exec(db2, "SELECT cloudsync_init('users');"); RCHECK
    rc = db_exec(db2, "SELECT cloudsync_init('activities');"); RCHECK
    rc = db_exec(db2, "SELECT cloudsync_init('workouts');"); RCHECK

    // init network with connection string + apikey
    rc = db_exec(db2, network_init); RCHECK

    if (apikey) {
        char set_apikey2[512];
        snprintf(set_apikey2, sizeof(set_apikey2),
            "SELECT cloudsync_network_set_apikey('%s');", apikey);
        rc = db_exec(db2, set_apikey2); RCHECK
    }

    rc = db_expect_gt0(db2, "SELECT cloudsync_network_sync(250,10) ->> '$.receive.rows';"); RCHECK

    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM users WHERE name='%s';", value);
    rc = db_expect_int(db2, sql, 0); RCHECK

    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM users WHERE name='%s-should-sync';", value);
    rc = db_expect_int(db2, sql, 1); RCHECK

    rc = db_exec(db2, "SELECT cloudsync_terminate();"); RCHECK

    sqlite3_close(db2);

ABORT_TEST
}

// Reproduces the spurious-gap bug in the send path: when the local db_version clock
// has been advanced past the site's own changes — as happens when applied remote
// changes bump the clock — the send announces only the change's own db_version range,
// so the skipped versions stay a gap in the server's per-site coverage and
// lastOptimisticVersion can never reach localVersion. cloudsync_db_version_next()
// forces the jump deterministically, no second database required. Expected to FAIL
// until the send announces the covered window [last_sent+1 .. watermark].
int test_send_gap_from_clock_hole(const char *db_path) {
    sqlite3 *db = NULL;
    int rc = open_load_ext(db_path, &db); RCHECK
    rc = db_init(db); RCHECK   // create users/activities/workouts (this db is fresh)

    char value[UUID_STR_MAXLEN];
    cloudsync_uuid_v7_string(value, true);
    char sql[256];

    rc = db_exec(db, "SELECT cloudsync_init('users');"); RCHECK
    rc = db_exec(db, "SELECT cloudsync_init('activities');"); RCHECK
    rc = db_exec(db, "SELECT cloudsync_init('workouts');"); RCHECK

    // Force the next local change to land at db_version 10, leaving 1..9 with no
    // local-site change (the "hole" that merging applied remote changes would create).
    rc = db_exec(db, "SELECT cloudsync_db_version_next(10);"); RCHECK

    snprintf(sql, sizeof(sql), "INSERT INTO users (id, name) VALUES ('%s', '%s');", value, value);
    rc = db_exec(db, sql); RCHECK

    // sanity: the change really landed at db_version 10, so there is a leading hole
    rc = db_expect_int(db, "SELECT cloudsync_db_version();", 10); RCHECK

    // init network
    char network_init[1024];
    const char* test_db_id = getenv("INTEGRATION_TEST_DATABASE_ID");
    if (!test_db_id) {
        fprintf(stderr, "Error: INTEGRATION_TEST_DATABASE_ID not set.\n");
        exit(1);
    }
    const char* custom_address = getenv("INTEGRATION_TEST_CLOUDSYNC_ADDRESS");
    if (custom_address) {
        snprintf(network_init, sizeof(network_init),
            "SELECT cloudsync_network_init_custom('%s', '%s');", custom_address, test_db_id);
    } else {
        snprintf(network_init, sizeof(network_init),
            "SELECT cloudsync_network_init('%s');", test_db_id);
    }
    rc = db_exec(db, network_init); RCHECK

    const char* apikey = getenv("INTEGRATION_TEST_APIKEY");
    if (apikey) {
        char set_apikey[512];
        snprintf(set_apikey, sizeof(set_apikey),
            "SELECT cloudsync_network_set_apikey('%s');", apikey);
        rc = db_exec(db, set_apikey); RCHECK
    }

    // Send once. The server applies the change and computes lastOptimisticVersion
    // (serverVersion) synchronously from its per-site applied ranges. With contiguous
    // coverage it reaches localVersion (10); with the gap bug it stays at 0 because
    // db_versions 1..9 are reported missing.
    rc = db_expect_int(db,
        "SELECT (j ->> '$.send.serverVersion') = (j ->> '$.send.localVersion') "
        "       AND (j ->> '$.send.localVersion') = 10 "
        "FROM (SELECT cloudsync_network_send_changes() AS j);", 1); RCHECK

    rc = db_exec(db, "SELECT cloudsync_cleanup('users');"); RCHECK
    rc = db_exec(db, "SELECT cloudsync_cleanup('activities');"); RCHECK
    rc = db_exec(db, "SELECT cloudsync_cleanup('workouts');"); RCHECK

ABORT_TEST
}

int test_chunked_payload_paths(void) {
    int rc = SQLITE_OK;
    sqlite3 *sender = NULL;
    sqlite3 *receiver = NULL;
    char network_init[1024];
    char row_id[UUID_STR_MAXLEN];
    char sql[1024];
    bool found = false;
    bool cleanup_remote_row = false;

    rc = test_chunked_pair_open(&sender, &receiver, network_init, sizeof(network_init));
    if (rc == TEST_SKIPPED) return TEST_SKIPPED;
    if (rc != SQLITE_OK) goto cleanup;

    cloudsync_uuid_v7_string(row_id, true);
    rc = db_exec(sender, "SELECT cloudsync_set('payload_max_chunk_size', '262144');"); if (rc != SQLITE_OK) goto cleanup;
    snprintf(sql, sizeof(sql),
        "INSERT INTO chunked_payload_items (id, body) "
        "VALUES ('%s', lower(hex(zeroblob(360000))));",
        row_id);
    rc = db_exec(sender, sql); if (rc != SQLITE_OK) goto cleanup;

    rc = db_expect_min(sender, "SELECT COUNT(*) FROM cloudsync_payload_chunks();", 2); if (rc != SQLITE_OK) goto cleanup;
    rc = db_expect_min(sender, "SELECT COUNT(*) FROM cloudsync_payload_chunks() WHERE hex(substr(payload,5,1))='03';", 2); if (rc != SQLITE_OK) goto cleanup;

    rc = db_exec(sender, "SELECT cloudsync_network_send_changes();"); if (rc != SQLITE_OK) goto cleanup;
    cleanup_remote_row = true;

    for (int attempt = 0; attempt < 30; ++attempt) {
        int matches = 0;

        // Exercises the deprecated cloudsync_network_check_changes() alias on purpose
        // (backward-compatibility coverage); cloudsync_network_receive_changes() is the
        // canonical name and is covered by the rowset and capped-drain tests.
        rc = db_exec(receiver, "SELECT cloudsync_network_check_changes();");
        if (rc != SQLITE_OK) goto cleanup;

        snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM chunked_payload_items "
            "WHERE id='%s' "
            "AND length(body)=720000 "
            "AND body=lower(hex(zeroblob(360000)));",
            row_id);
        rc = db_select_int(receiver, sql, &matches);
        if (rc != SQLITE_OK) goto cleanup;
        if (matches == 1) {
            found = true;
            break;
        }

        sqlite3_sleep(500);
    }

    if (!found) {
        printf("Error: chunked e2e row %s was not received.\n", row_id);
        rc = SQLITE_ERROR;
        goto cleanup;
    }

cleanup:
    if (cleanup_remote_row && sender) {
        snprintf(sql, sizeof(sql), "DELETE FROM chunked_payload_items WHERE id='%s';", row_id);
        if (db_exec(sender, sql) == SQLITE_OK) {
            db_exec(sender, "SELECT cloudsync_network_send_changes();");
        }
    }
    test_chunked_pair_close(sender, receiver);
    return rc;
}

int test_chunked_payload_rowset_path(void) {
    int rc = SQLITE_OK;
    sqlite3 *sender = NULL;
    sqlite3 *receiver = NULL;
    char network_init[1024];
    char batch_id[UUID_STR_MAXLEN];
    char sql[1024];
    bool found = false;
    bool cleanup_remote_rows = false;
    const int row_count = 500;
    const int body_bytes = 1600;

    rc = test_chunked_pair_open(&sender, &receiver, network_init, sizeof(network_init));
    if (rc == TEST_SKIPPED) return TEST_SKIPPED;
    if (rc != SQLITE_OK) goto cleanup;

    cloudsync_uuid_v7_string(batch_id, true);
    rc = db_exec(sender, "SELECT cloudsync_set('payload_max_chunk_size', '262144');"); if (rc != SQLITE_OK) goto cleanup;
    snprintf(sql, sizeof(sql),
        "WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i < %d) "
        "INSERT INTO chunked_payload_items (id, body) "
        "SELECT '%s-' || printf('%%03d', i), lower(hex(zeroblob(%d))) FROM c;",
        row_count, batch_id, body_bytes);
    rc = db_exec(sender, sql); if (rc != SQLITE_OK) goto cleanup;

    rc = db_expect_min(sender, "SELECT COUNT(*) FROM cloudsync_payload_chunks();", 2); if (rc != SQLITE_OK) goto cleanup;
    rc = db_expect_int(sender, "SELECT COUNT(*) FROM cloudsync_payload_chunks() WHERE hex(substr(payload,5,1))='03';", 0); if (rc != SQLITE_OK) goto cleanup;

    rc = db_exec(sender, "SELECT cloudsync_network_send_changes();"); if (rc != SQLITE_OK) goto cleanup;
    cleanup_remote_rows = true;

    for (int attempt = 0; attempt < 30; ++attempt) {
        int matches = 0;

        rc = db_exec(receiver, "SELECT cloudsync_network_receive_changes();");
        if (rc != SQLITE_OK) goto cleanup;

        snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM chunked_payload_items "
            "WHERE id LIKE '%s-%%' "
            "AND length(body)=%d "
            "AND body=lower(hex(zeroblob(%d)));",
            batch_id, body_bytes * 2, body_bytes);
        rc = db_select_int(receiver, sql, &matches);
        if (rc != SQLITE_OK) goto cleanup;
        if (matches == row_count) {
            found = true;
            break;
        }

        sqlite3_sleep(500);
    }

    if (!found) {
        printf("Error: chunked rowset e2e batch %s was not received.\n", batch_id);
        rc = SQLITE_ERROR;
        goto cleanup;
    }

cleanup:
    if (cleanup_remote_rows && sender) {
        snprintf(sql, sizeof(sql), "DELETE FROM chunked_payload_items WHERE id LIKE '%s-%%';", batch_id);
        if (db_exec(sender, sql) == SQLITE_OK) {
            db_exec(sender, "SELECT cloudsync_network_send_changes();");
        }
    }
    test_chunked_pair_close(sender, receiver);
    return rc;
}

// Verifies that a single cloudsync_network_sync() call drains an entire multi-chunk
// download (no per-chunk extra sync() calls), and that the new receive.chunks /
// receive.complete fields report the drain. A large batch of incompressible rows is
// sent so the server's check response spans several pages; one sync() on the receiver
// must pull them all and report chunks>1 with complete=true.
int test_chunked_payload_single_sync_drain(void) {
    int rc = SQLITE_OK;
    sqlite3 *sender = NULL;
    sqlite3 *receiver = NULL;
    char network_init[1024];
    char batch_id[UUID_STR_MAXLEN];
    char sql[1024];
    bool found = false;
    bool observed_multi_chunk = false;   // saw chunks>1 && complete=1 in a SINGLE sync() call
    bool cleanup_remote_rows = false;
    // INCOMPRESSIBLE random bodies (distinct per row) so the payload can't compress
    // below the server's page size. Multi-page splitting is driven by the TENANT's
    // payload_max_chunk_size (the server-side check-response page size, distinct from
    // the client upload chunk size set below), which must be small on
    // INTEGRATION_TEST_CHUNKED_DATABASE_ID — set it to 262144 to match the client.
    // ~1 MB of incompressible data over a 256 KB page cap yields several pages
    // regardless of tenant backlog, so one sync() must drain chunks>1. (zeroblob bodies
    // would compress to ~nothing and collapse back to a single page.)
    const int row_count = 500;
    const int body_rand_bytes = 2048;    // 4096-char bodies; ~1 MB random / ~2 MB serialized

    rc = test_chunked_pair_open(&sender, &receiver, network_init, sizeof(network_init));
    if (rc == TEST_SKIPPED) return TEST_SKIPPED;
    if (rc != SQLITE_OK) goto cleanup;

    cloudsync_uuid_v7_string(batch_id, true);
    rc = db_exec(sender, "SELECT cloudsync_set('payload_max_chunk_size', '262144');"); if (rc != SQLITE_OK) goto cleanup;
    snprintf(sql, sizeof(sql),
        "WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i < %d) "
        "INSERT INTO chunked_payload_items (id, body) "
        "SELECT '%s-' || printf('%%03d', i), lower(hex(randomblob(%d))) FROM c;",
        row_count, batch_id, body_rand_bytes);
    rc = db_exec(sender, sql); if (rc != SQLITE_OK) goto cleanup;

    // Sender splits into multiple non-fragment chunks.
    rc = db_expect_min(sender, "SELECT COUNT(*) FROM cloudsync_payload_chunks();", 2); if (rc != SQLITE_OK) goto cleanup;

    rc = db_exec(sender, "SELECT cloudsync_network_send_changes();"); if (rc != SQLITE_OK) goto cleanup;
    cleanup_remote_rows = true;

    for (int attempt = 0; attempt < 40; ++attempt) {
        int chunks = 0, complete = 0, matches = 0;
        char recv_err[512];

        // Run exactly one sync() and read both receive fields from that single call.
        // The call must stay a read-only SELECT (here a subquery): wrapping it in
        // CREATE TABLE ... AS SELECT would keep the outer statement stepping while the
        // apply path tries to open its savepoint, which SQLite rejects ("SQL statements
        // in progress") — silently leaving the download unapplied.
        rc = db_select_receive(receiver,
            "SELECT j ->> '$.receive.chunks', j ->> '$.receive.complete', j ->> '$.receive.error' "
            "FROM (SELECT cloudsync_network_sync(250, 30) AS j);",
            &chunks, &complete, recv_err, sizeof(recv_err));
        if (rc != SQLITE_OK) goto cleanup;
        // A client-side apply error is swallowed into receive.error; fail loudly
        // instead of looping until the "not received" timeout.
        if (recv_err[0]) {
            printf("Error: chunked single-sync drain batch %s reported receive.error: %s\n", batch_id, recv_err);
            rc = SQLITE_ERROR;
            goto cleanup;
        }
        if (chunks > 1 && complete == 1) observed_multi_chunk = true;

        // Random bodies can't be matched by value; assert the full set arrived with
        // each row at its expected length (content correctness is covered elsewhere).
        snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM chunked_payload_items "
            "WHERE id LIKE '%s-%%' "
            "AND length(body)=%d;",
            batch_id, body_rand_bytes * 2);
        rc = db_select_int(receiver, sql, &matches); if (rc != SQLITE_OK) goto cleanup;
        if (matches == row_count) { found = true; break; }

        sqlite3_sleep(500);
    }

    if (!found) {
        printf("Error: chunked single-sync drain batch %s was not received.\n", batch_id);
        rc = SQLITE_ERROR;
        goto cleanup;
    }
    if (!observed_multi_chunk) {
        printf("Error: a single cloudsync_network_sync() did not drain a multi-chunk stream (chunks>1, complete=1) for batch %s.\n", batch_id);
        rc = SQLITE_ERROR;
        goto cleanup;
    }

cleanup:
    if (cleanup_remote_rows && sender) {
        snprintf(sql, sizeof(sql), "DELETE FROM chunked_payload_items WHERE id LIKE '%s-%%';", batch_id);
        if (db_exec(sender, sql) == SQLITE_OK) {
            db_exec(sender, "SELECT cloudsync_network_send_changes();");
        }
    }
    test_chunked_pair_close(sender, receiver);
    return rc;
}

// Verifies the opt-in max_chunks cap on cloudsync_network_receive_changes(): each call
// applies at most one chunk, reports complete=false while more is pending, and resumes
// across calls (the in-memory page cursor persists) until the whole batch is received.
int test_chunked_payload_capped_receive(void) {
    int rc = SQLITE_OK;
    sqlite3 *sender = NULL;
    sqlite3 *receiver = NULL;
    char network_init[1024];
    char batch_id[UUID_STR_MAXLEN];
    char sql[1024];
    bool found = false;
    bool observed_capped_partial = false;  // saw chunks==1 && complete=0 from a receive(1) call
    bool cleanup_remote_rows = false;
    // INCOMPRESSIBLE random bodies (distinct per row) so the payload can't compress
    // below the server's page size. Multi-page splitting is driven by the TENANT's
    // payload_max_chunk_size (the server-side check-response page size, distinct from
    // the client upload chunk size set below), which must be small on
    // INTEGRATION_TEST_CHUNKED_DATABASE_ID — set it to 262144 to match the client — so
    // the batch spans several pages and receive_changes(1) leaves a partial. (zeroblob
    // bodies would compress away and collapse to one page.)
    const int row_count = 500;
    const int body_rand_bytes = 2048;    // 4096-char bodies; ~1 MB random / ~2 MB serialized

    rc = test_chunked_pair_open(&sender, &receiver, network_init, sizeof(network_init));
    if (rc == TEST_SKIPPED) return TEST_SKIPPED;
    if (rc != SQLITE_OK) goto cleanup;

    cloudsync_uuid_v7_string(batch_id, true);
    rc = db_exec(sender, "SELECT cloudsync_set('payload_max_chunk_size', '262144');"); if (rc != SQLITE_OK) goto cleanup;
    snprintf(sql, sizeof(sql),
        "WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i < %d) "
        "INSERT INTO chunked_payload_items (id, body) "
        "SELECT '%s-' || printf('%%03d', i), lower(hex(randomblob(%d))) FROM c;",
        row_count, batch_id, body_rand_bytes);
    rc = db_exec(sender, sql); if (rc != SQLITE_OK) goto cleanup;

    rc = db_expect_min(sender, "SELECT COUNT(*) FROM cloudsync_payload_chunks();", 2); if (rc != SQLITE_OK) goto cleanup;

    rc = db_exec(sender, "SELECT cloudsync_network_send_changes();"); if (rc != SQLITE_OK) goto cleanup;
    cleanup_remote_rows = true;

    for (int attempt = 0; attempt < 80; ++attempt) {
        int chunks = 0, complete = 0, matches = 0;
        char recv_err[512];

        // Cap each call to a single chunk; read both fields from that one call. Must
        // stay a read-only SELECT (subquery): CREATE TABLE ... AS SELECT would block
        // the apply savepoint ("SQL statements in progress") and silently drop the
        // downloaded chunk.
        rc = db_select_receive(receiver,
            "SELECT j ->> '$.receive.chunks', j ->> '$.receive.complete', j ->> '$.receive.error' "
            "FROM (SELECT cloudsync_network_receive_changes(1) AS j);",
            &chunks, &complete, recv_err, sizeof(recv_err));
        if (rc != SQLITE_OK) goto cleanup;
        // Surface a swallowed client-side apply error instead of looping to timeout.
        if (recv_err[0]) {
            printf("Error: capped-receive batch %s reported receive.error: %s\n", batch_id, recv_err);
            rc = SQLITE_ERROR;
            goto cleanup;
        }
        // The cap must never apply more than one chunk per call.
        if (chunks > 1) {
            printf("Error: cloudsync_network_receive_changes(1) applied %d chunks (cap violated) for batch %s.\n", chunks, batch_id);
            rc = SQLITE_ERROR;
            goto cleanup;
        }
        if (chunks == 1 && complete == 0) observed_capped_partial = true;

        // Random bodies can't be matched by value; assert the full set arrived with
        // each row at its expected length (content correctness is covered elsewhere).
        snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM chunked_payload_items "
            "WHERE id LIKE '%s-%%' "
            "AND length(body)=%d;",
            batch_id, body_rand_bytes * 2);
        rc = db_select_int(receiver, sql, &matches); if (rc != SQLITE_OK) goto cleanup;
        if (matches == row_count) { found = true; break; }

        sqlite3_sleep(300);
    }

    if (!found) {
        printf("Error: capped-receive batch %s was not fully received.\n", batch_id);
        rc = SQLITE_ERROR;
        goto cleanup;
    }
    if (!observed_capped_partial) {
        printf("Error: cloudsync_network_receive_changes(1) never reported a capped partial drain (chunks=1, complete=0) for batch %s.\n", batch_id);
        rc = SQLITE_ERROR;
        goto cleanup;
    }

cleanup:
    if (cleanup_remote_rows && sender) {
        snprintf(sql, sizeof(sql), "DELETE FROM chunked_payload_items WHERE id LIKE '%s-%%';", batch_id);
        if (db_exec(sender, sql) == SQLITE_OK) {
            db_exec(sender, "SELECT cloudsync_network_send_changes();");
        }
    }
    test_chunked_pair_close(sender, receiver);
    return rc;
}

// Verifies the batched cursor-spool response shape with an explicit maxChunks cap
// greater than one. The old single-page client parser ignores data.chunks[] and
// would report chunks=0 forever against the batched server response.
int test_chunked_payload_batched_receive(void) {
    int rc = SQLITE_OK;
    sqlite3 *sender = NULL;
    sqlite3 *receiver = NULL;
    char network_init[1024];
    char batch_id[UUID_STR_MAXLEN];
    char sql[1024];
    bool found = false;
    bool observed_batched_partial = false;  // saw chunks==2 && complete=0 from receive(2)
    bool cleanup_remote_rows = false;
    const int row_count = 500;
    const int body_rand_bytes = 2048;

    rc = test_chunked_pair_open(&sender, &receiver, network_init, sizeof(network_init));
    if (rc == TEST_SKIPPED) return TEST_SKIPPED;
    if (rc != SQLITE_OK) goto cleanup;

    cloudsync_uuid_v7_string(batch_id, true);
    rc = db_exec(sender, "SELECT cloudsync_set('payload_max_chunk_size', '262144');"); if (rc != SQLITE_OK) goto cleanup;
    snprintf(sql, sizeof(sql),
        "WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i < %d) "
        "INSERT INTO chunked_payload_items (id, body) "
        "SELECT '%s-' || printf('%%03d', i), lower(hex(randomblob(%d))) FROM c;",
        row_count, batch_id, body_rand_bytes);
    rc = db_exec(sender, sql); if (rc != SQLITE_OK) goto cleanup;

    rc = db_expect_min(sender, "SELECT COUNT(*) FROM cloudsync_payload_chunks();", 3); if (rc != SQLITE_OK) goto cleanup;

    rc = db_exec(sender, "SELECT cloudsync_network_send_changes();"); if (rc != SQLITE_OK) goto cleanup;
    cleanup_remote_rows = true;

    for (int attempt = 0; attempt < 80; ++attempt) {
        int chunks = 0, complete = 0, matches = 0;
        char recv_err[512];

        rc = db_select_receive(receiver,
            "SELECT j ->> '$.receive.chunks', j ->> '$.receive.complete', j ->> '$.receive.error' "
            "FROM (SELECT cloudsync_network_receive_changes(2) AS j);",
            &chunks, &complete, recv_err, sizeof(recv_err));
        if (rc != SQLITE_OK) goto cleanup;
        if (recv_err[0]) {
            printf("Error: batched-receive batch %s reported receive.error: %s\n", batch_id, recv_err);
            rc = SQLITE_ERROR;
            goto cleanup;
        }
        if (chunks > 2) {
            printf("Error: cloudsync_network_receive_changes(2) applied %d chunks (cap violated) for batch %s.\n", chunks, batch_id);
            rc = SQLITE_ERROR;
            goto cleanup;
        }
        if (chunks == 2 && complete == 0) observed_batched_partial = true;

        snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM chunked_payload_items "
            "WHERE id LIKE '%s-%%' "
            "AND length(body)=%d;",
            batch_id, body_rand_bytes * 2);
        rc = db_select_int(receiver, sql, &matches); if (rc != SQLITE_OK) goto cleanup;
        if (matches == row_count) { found = true; break; }

        sqlite3_sleep(300);
    }

    if (!found) {
        printf("Error: batched-receive batch %s was not fully received.\n", batch_id);
        rc = SQLITE_ERROR;
        goto cleanup;
    }
    if (!observed_batched_partial) {
        printf("Error: cloudsync_network_receive_changes(2) never reported a batched partial drain (chunks=2, complete=0) for batch %s.\n", batch_id);
        rc = SQLITE_ERROR;
        goto cleanup;
    }

cleanup:
    if (cleanup_remote_rows && sender) {
        snprintf(sql, sizeof(sql), "DELETE FROM chunked_payload_items WHERE id LIKE '%s-%%';", batch_id);
        if (db_exec(sender, sql) == SQLITE_OK) {
            db_exec(sender, "SELECT cloudsync_network_send_changes();");
        }
    }
    test_chunked_pair_close(sender, receiver);
    return rc;
}

int test_chunked_send_failure_preserves_checkpoint(void) {
    int rc = SQLITE_OK;
    sqlite3 *sender = NULL;
    char network_init[1024];
    char batch_id[UUID_STR_MAXLEN];
    char sql[1024];
    char *errmsg = NULL;
    int send_dbversion_before = 0;
    int send_dbversion_after = -1;
    const int row_count = 500;
    const int body_bytes = 1600;

    rc = test_chunked_sender_open(&sender, network_init, sizeof(network_init));
    if (rc == TEST_SKIPPED) return TEST_SKIPPED;
    if (rc != SQLITE_OK) goto cleanup;

    rc = test_chunked_failure_schema_init(sender); if (rc != SQLITE_OK) goto cleanup;
    rc = db_exec(sender, "SELECT cloudsync_set('payload_max_chunk_size', '262144');"); if (rc != SQLITE_OK) goto cleanup;

    cloudsync_uuid_v7_string(batch_id, true);
    snprintf(sql, sizeof(sql),
        "WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i < %d) "
        "INSERT INTO chunked_payload_failure_items (id, body) "
        "SELECT '%s-' || printf('%%03d', i), lower(hex(zeroblob(%d))) FROM c;",
        row_count, batch_id, body_bytes);
    rc = db_exec(sender, sql); if (rc != SQLITE_OK) goto cleanup;

    rc = db_expect_min(sender, "SELECT COUNT(*) FROM cloudsync_payload_chunks();", 2); if (rc != SQLITE_OK) goto cleanup;
    rc = db_expect_int(sender, "SELECT COUNT(*) FROM cloudsync_payload_chunks() WHERE hex(substr(payload,5,1))='03';", 0); if (rc != SQLITE_OK) goto cleanup;

    rc = db_select_int(sender,
        "SELECT CAST(COALESCE((SELECT value FROM cloudsync_settings WHERE key='send_dbversion'), '0') AS INTEGER);",
        &send_dbversion_before);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_exec(sender, "SELECT cloudsync_network_send_changes();", NULL, NULL, &errmsg);
    if (rc == SQLITE_OK) {
        printf("Error: chunked send failure test expected cloudsync_network_send_changes to fail.\n");
        rc = SQLITE_ERROR;
        goto cleanup;
    }
    if (errmsg) {
        sqlite3_free(errmsg);
        errmsg = NULL;
    }

    rc = db_select_int(sender,
        "SELECT CAST(COALESCE((SELECT value FROM cloudsync_settings WHERE key='send_dbversion'), '0') AS INTEGER);",
        &send_dbversion_after);
    if (rc != SQLITE_OK) goto cleanup;

    if (send_dbversion_after != send_dbversion_before) {
        printf("Error: send_dbversion advanced after failed chunked send (before=%d after=%d).\n",
               send_dbversion_before, send_dbversion_after);
        rc = SQLITE_ERROR;
        goto cleanup;
    }

    rc = SQLITE_OK;

cleanup:
    if (errmsg) sqlite3_free(errmsg);
    test_chunked_sender_close(sender);
    return rc;
}

// Regression test for the chunked-check stale negative-cache bug
// (see cloudsync/docs/chunked-check-negative-cache.md). A receiver that drains
// the tenant to empty must not be permanently pinned to "up to date": an empty
// /check result advances no cursor, so the receiver keeps polling the same
// (dbVersion, seq) key. A server that *caches* that empty chunk at the key would
// keep answering "no changes" even after another client commits changes at the
// same key, hiding them until the artifact TTL (up to 24h) expires.
//
// This drives the exact sequence that exposes the bug end-to-end:
//   1. the receiver drains the whole tenant until it is provably caught up;
//   2. it keeps polling for a few more seconds and asserts it stays at 0 rows
//      (steady empty state — this is where the stale empty chunk would be cached);
//   3. a second client (the sender) inserts a row and sends it at that same key;
//   4. the receiver must observe that row on a subsequent receive. Against a
//      server that caches the negative result this never arrives and the test
//      fails; against the fixed server it is delivered.
//
// "Caught up" cannot be detected from row counts alone: a server that is still
// preparing a page replies 202, which the client surfaces as receive.rows=0,
// complete=1 — indistinguishable from a genuinely empty result. (An earlier
// version of this test mistook three preparation 202s for "drained" and then
// had ~71k backlog rows land during phase 2.) So we anchor on a sentinel: the
// sender commits a marker row *after* any pre-existing backlog, and phase 1 is
// only considered drained once the receiver has both applied that sentinel
// (the whole backlog is therefore behind it) and then seen a 0-row poll.
int test_chunked_negative_cache_invalidation(void) {
    int rc = SQLITE_OK;
    sqlite3 *sender = NULL;
    sqlite3 *receiver = NULL;
    char network_init[1024];
    char sentinel_id[UUID_STR_MAXLEN];
    char row_id[UUID_STR_MAXLEN];
    char sql[1024];
    bool drained = false;
    bool found = false;
    bool cleanup_sentinel = false;
    bool cleanup_remote_row = false;

    rc = test_chunked_pair_open(&sender, &receiver, network_init, sizeof(network_init));
    if (rc == TEST_SKIPPED) return TEST_SKIPPED;
    if (rc != SQLITE_OK) goto cleanup;

    // Commit a sentinel row that sorts after any pre-existing tenant backlog, so
    // observing it on the receiver proves the entire backlog has been drained.
    cloudsync_uuid_v7_string(sentinel_id, true);
    snprintf(sql, sizeof(sql),
        "INSERT INTO chunked_payload_items (id, body) VALUES ('%s', 'negative-cache-sentinel');",
        sentinel_id);
    rc = db_exec(sender, sql); if (rc != SQLITE_OK) goto cleanup;
    rc = db_exec(sender, "SELECT cloudsync_network_send_changes();"); if (rc != SQLITE_OK) goto cleanup;
    cleanup_sentinel = true;

    // Phase 1: drain the receiver until it is provably caught up. Each bare
    // receive_changes() applies everything currently ready and returns at the first
    // 202, so a large backlog is pulled across several iterations. Termination
    // requires the sentinel to be present (backlog fully drained) AND a subsequent
    // 0-row poll, so a mid-preparation 202 can never be mistaken for "caught up".
    bool sentinel_seen = false;
    for (int attempt = 0; attempt < 200 && !drained; ++attempt) {
        int rows = 0, complete = 0;
        char recv_err[512];

        rc = db_select_receive(receiver,
            "SELECT j ->> '$.receive.rows', j ->> '$.receive.complete', j ->> '$.receive.error' "
            "FROM (SELECT cloudsync_network_receive_changes() AS j);",
            &rows, &complete, recv_err, sizeof(recv_err));
        if (rc != SQLITE_OK) goto cleanup;
        if (recv_err[0]) {
            printf("Error: negative-cache drain reported receive.error: %s\n", recv_err);
            rc = SQLITE_ERROR;
            goto cleanup;
        }

        if (!sentinel_seen) {
            int matches = 0;
            snprintf(sql, sizeof(sql),
                "SELECT COUNT(*) FROM chunked_payload_items WHERE id='%s';", sentinel_id);
            rc = db_select_int(receiver, sql, &matches); if (rc != SQLITE_OK) goto cleanup;
            if (matches == 1) sentinel_seen = true;
        }

        // Caught up only once the backlog (including the sentinel) is fully applied
        // and a further poll delivers nothing.
        if (sentinel_seen && rows == 0) drained = true;

        sqlite3_sleep(300);
    }

    if (!drained) {
        printf("Error: negative-cache receiver never caught up to the sentinel row.\n");
        rc = SQLITE_ERROR;
        goto cleanup;
    }

    // Phase 2: keep polling for a few seconds with the tenant idle. The receiver is
    // caught up, so every poll must continue to report 0 rows. (This is the window
    // where a buggy server caches the empty chunk at the receiver's key.)
    for (int i = 0; i < 6; ++i) {
        int rows = 0, complete = 0;
        char recv_err[512];

        rc = db_select_receive(receiver,
            "SELECT j ->> '$.receive.rows', j ->> '$.receive.complete', j ->> '$.receive.error' "
            "FROM (SELECT cloudsync_network_receive_changes() AS j);",
            &rows, &complete, recv_err, sizeof(recv_err));
        if (rc != SQLITE_OK) goto cleanup;
        if (recv_err[0]) {
            printf("Error: negative-cache idle poll reported receive.error: %s\n", recv_err);
            rc = SQLITE_ERROR;
            goto cleanup;
        }
        if (rows != 0) {
            printf("Error: negative-cache receiver applied %d unexpected rows while the tenant was idle.\n", rows);
            rc = SQLITE_ERROR;
            goto cleanup;
        }

        sqlite3_sleep(500);
    }

    // Phase 3: a second client commits a change at the same (dbVersion, seq) key
    // the receiver has been polling against.
    cloudsync_uuid_v7_string(row_id, true);
    snprintf(sql, sizeof(sql),
        "INSERT INTO chunked_payload_items (id, body) VALUES ('%s', 'negative-cache');",
        row_id);
    rc = db_exec(sender, sql); if (rc != SQLITE_OK) goto cleanup;
    rc = db_exec(sender, "SELECT cloudsync_network_send_changes();"); if (rc != SQLITE_OK) goto cleanup;
    cleanup_remote_row = true;

    // Phase 4: the receiver must now pick up the change on a subsequent receive.
    // A stale negative cache would keep answering "no changes" at the unchanged key
    // and this row would never arrive.
    for (int attempt = 0; attempt < 60; ++attempt) {
        int matches = 0;
        char recv_err[512];
        int rows = 0, complete = 0;

        rc = db_select_receive(receiver,
            "SELECT j ->> '$.receive.rows', j ->> '$.receive.complete', j ->> '$.receive.error' "
            "FROM (SELECT cloudsync_network_receive_changes() AS j);",
            &rows, &complete, recv_err, sizeof(recv_err));
        if (rc != SQLITE_OK) goto cleanup;
        if (recv_err[0]) {
            printf("Error: negative-cache post-send poll reported receive.error: %s\n", recv_err);
            rc = SQLITE_ERROR;
            goto cleanup;
        }

        snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM chunked_payload_items WHERE id='%s' AND body='negative-cache';",
            row_id);
        rc = db_select_int(receiver, sql, &matches); if (rc != SQLITE_OK) goto cleanup;
        if (matches == 1) { found = true; break; }

        sqlite3_sleep(500);
    }

    if (!found) {
        printf("Error: stale negative cache — receiver never received row %s after the sender committed it.\n", row_id);
        rc = SQLITE_ERROR;
        goto cleanup;
    }

cleanup:
    if ((cleanup_sentinel || cleanup_remote_row) && sender) {
        if (cleanup_remote_row) {
            snprintf(sql, sizeof(sql), "DELETE FROM chunked_payload_items WHERE id='%s';", row_id);
            db_exec(sender, sql);
        }
        if (cleanup_sentinel) {
            snprintf(sql, sizeof(sql), "DELETE FROM chunked_payload_items WHERE id='%s';", sentinel_id);
            db_exec(sender, sql);
        }
        db_exec(sender, "SELECT cloudsync_network_send_changes();");
    }
    test_chunked_pair_close(sender, receiver);
    return rc;
}

int test_offline_error(const char *db_path) {
    sqlite3 *db = NULL;
    int rc = open_load_ext(db_path, &db);
    RCHECK

    rc = db_exec(db, "CREATE TABLE IF NOT EXISTS test_table (id TEXT PRIMARY KEY NOT NULL, value TEXT NOT NULL DEFAULT '', created_at TEXT DEFAULT CURRENT_TIMESTAMP);");
    RCHECK

    rc = db_exec(db, "SELECT cloudsync_init('test_table');");
    RCHECK

    rc = db_exec(db, "INSERT INTO test_table (id, value) VALUES (cloudsync_uuid(), 'test1'), (cloudsync_uuid(), 'test2');");
    RCHECK

    // Initialize network with offline database ID
    const char* offline_db_id = getenv("INTEGRATION_TEST_OFFLINE_DATABASE_ID");
    if (!offline_db_id || !*offline_db_id) {
        // Clean up the db opened above and return TEST_SKIPPED directly (going through
        // abort_test would trip ERROR_MSG on the non-zero rc and print a spurious error).
        if (db) { db_exec(db, "SELECT cloudsync_terminate();"); sqlite3_close(db); }
        return TEST_SKIPPED;
    }

    char network_init[512];
    const char* custom_address = getenv("INTEGRATION_TEST_CLOUDSYNC_ADDRESS");
    if (custom_address) {
        snprintf(network_init, sizeof(network_init),
            "SELECT cloudsync_network_init_custom('%s', '%s');", custom_address, offline_db_id);
    } else {
        snprintf(network_init, sizeof(network_init),
            "SELECT cloudsync_network_init('%s');", offline_db_id);
    }
    rc = db_exec(db, network_init);
    RCHECK

    const char* apikey = getenv("INTEGRATION_TEST_APIKEY");
    if (apikey) {
        char set_apikey[512];
        snprintf(set_apikey, sizeof(set_apikey),
            "SELECT cloudsync_network_set_apikey('%s');", apikey);
        rc = db_exec(db, set_apikey);
        RCHECK
    }

    // Try to sync - this should fail with the expected error
    char *errmsg = NULL;
    rc = sqlite3_exec(db, "SELECT cloudsync_network_sync();", NULL, NULL, &errmsg);

    if (rc == SQLITE_OK) {
        printf("Error: Expected network sync to fail, but it succeeded.\n");
        rc = SQLITE_ERROR;
        goto abort_test;
    }

    // Verify the error JSON contains expected fields using SQLite JSON extraction
    if (!errmsg) {
        printf("Error: Expected an error message, but got NULL\n");
        rc = SQLITE_ERROR;
        goto abort_test;
    }

    char verify_sql[1024];
    snprintf(verify_sql, sizeof(verify_sql),
        "SELECT json_extract('%s', '$.errors[0].status');", errmsg);
    rc = db_expect_str(db, verify_sql, "503");
    if (rc != SQLITE_OK) { printf("Offline error: unexpected status in: %s\n", errmsg); sqlite3_free(errmsg); goto abort_test; }

    snprintf(verify_sql, sizeof(verify_sql),
        "SELECT json_extract('%s', '$.errors[0].code');", errmsg);
    rc = db_expect_str(db, verify_sql, "database_paused");
    if (rc != SQLITE_OK) { printf("Offline error: unexpected code in: %s\n", errmsg); sqlite3_free(errmsg); goto abort_test; }

    snprintf(verify_sql, sizeof(verify_sql),
        "SELECT json_extract('%s', '$.errors[0].title');", errmsg);
    rc = db_expect_str(db, verify_sql, "Database paused");
    if (rc != SQLITE_OK) { printf("Offline error: unexpected title in: %s\n", errmsg); sqlite3_free(errmsg); goto abort_test; }

    sqlite3_free(errmsg);
    rc = SQLITE_OK;

ABORT_TEST
}

int test_double_empty_network_init(const char *db_path) {
    sqlite3 *db = NULL;
    int rc = open_load_ext(db_path, &db);
    RCHECK

    // First call with empty string - should return error
    char *errmsg1 = NULL;
    rc = sqlite3_exec(db, "SELECT cloudsync_network_init('');", NULL, NULL, &errmsg1);
    if (rc == SQLITE_OK) {
        printf("Error: First cloudsync_network_init('') should have failed but succeeded\n");
        if (errmsg1) sqlite3_free(errmsg1);
        rc = SQLITE_ERROR;
        goto abort_test;
    }
    if (errmsg1) sqlite3_free(errmsg1);

    // Second call with empty string - should also return error (not segfault)
    char *errmsg2 = NULL;
    rc = sqlite3_exec(db, "SELECT cloudsync_network_init('');", NULL, NULL, &errmsg2);
    if (rc == SQLITE_OK) {
        printf("Error: Second cloudsync_network_init('') should have failed but succeeded\n");
        if (errmsg2) sqlite3_free(errmsg2);
        rc = SQLITE_ERROR;
        goto abort_test;
    }
    if (errmsg2) sqlite3_free(errmsg2);

    rc = SQLITE_OK;

ABORT_TEST
}

// Failure-path integration test.
//
// Targets a cloudsync database (INTEGRATION_TEST_FAILURE_DATABASE_ID)
// configured server-side to fail apply and check jobs. Verifies that the
// new failures.{apply,check} response shape is correctly parsed and emitted as
// send.lastFailure (cloudsync_network_send_changes) and receive.lastFailure
// (cloudsync_network_receive_changes), and that cloudsync_network_sync surfaces
// at least one of them.
//
// First invocation primes the server (sends data, queues a check) — server-side
// async jobs may not have failed yet. After a sleep, the second invocation must
// see lastFailure populated.
int test_failure_path (const char *db_path) {
    int rc = SQLITE_OK;
    sqlite3 *db = NULL;

    const char *test_db_id = getenv("INTEGRATION_TEST_FAILURE_DATABASE_ID");
    if (!test_db_id || !*test_db_id) {
        return TEST_SKIPPED;
    }
    const char *custom_address = getenv("INTEGRATION_TEST_CLOUDSYNC_ADDRESS");
    if (!custom_address || !*custom_address) {
        return TEST_SKIPPED;
    }

    rc = open_load_ext(db_path, &db); RCHECK

    rc = db_exec(db, "CREATE TABLE IF NOT EXISTS failure_users (id TEXT PRIMARY KEY NOT NULL, name TEXT NOT NULL DEFAULT '', value BLOB);"); RCHECK
    rc = db_exec(db, "SELECT cloudsync_init('failure_users');"); RCHECK

    char network_init[1024];
    snprintf(network_init, sizeof(network_init),
        "SELECT cloudsync_network_init_custom('%s', '%s');", custom_address, test_db_id);
    rc = db_exec(db, network_init); RCHECK

    const char *apikey = getenv("INTEGRATION_TEST_APIKEY");
    if (apikey) {
        char set_apikey[512];
        snprintf(set_apikey, sizeof(set_apikey),
            "SELECT cloudsync_network_set_apikey('%s');", apikey);
        rc = db_exec(db, set_apikey); RCHECK
    }

    // Insert a row so cloudsync_network_send_changes has a payload to upload.
    // Insert a 1MB value to skip the fast-lane and force using the normal s3 path with async job, 
    // otherwise the error would be immediately returned by the apply endpoint.
    char value[UUID_STR_MAXLEN];
    cloudsync_uuid_v7_string(value, true);
    char sql[256];
    snprintf(sql, sizeof(sql), "INSERT INTO failure_users (id, name, value) VALUES ('%s', '%s', randomblob(1048576));", value, value);
    rc = db_exec(db, sql); RCHECK

    // First invocation — primes the server. Failures may not yet be reported.
    rc = db_exec(db, "SELECT cloudsync_network_send_changes();"); RCHECK
    rc = db_exec(db, "SELECT cloudsync_network_receive_changes();"); RCHECK
    rc = db_exec(db, "SELECT cloudsync_network_sync(250, 1);"); RCHECK

    // Give the server time to process and fail the queued apply/check jobs.
    sqlite3_sleep(5000);

    // Second invocation — failures must surface now.
    // jobId is always > 0 when failure object is present, so ->> + GT0 doubles as
    // an existence check (NULL → atoi returns 0 → fails GT0).
    rc = db_expect_gt0(db,
        "SELECT cloudsync_network_send_changes() ->> '$.send.lastFailure.jobId';"); RCHECK
    rc = db_expect_gt0(db,
        "SELECT cloudsync_network_receive_changes() ->> '$.receive.lastFailure.jobId';"); RCHECK
    // sync must surface at least one of the two; instr() catches either path.
    rc = db_expect_gt0(db,
        "SELECT instr(cloudsync_network_sync(250, 1), '\"lastFailure\":');"); RCHECK

    rc = db_exec(db, "SELECT cloudsync_terminate();");

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
    const char *result = (rc == TEST_SKIPPED) ? "SKIPPED" : (rc ? "FAILED" : "OK");
    printf("%-32s %s\n", description, result);
    return (rc == TEST_SKIPPED) ? 0 : rc;   // a skipped test is not a failure
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
#ifdef _WIN32
        return (DWORD)(intptr_t)(thread_id+1);
#else
        return (void*)(intptr_t)(thread_id+1);
#endif
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
    remove(DB_PATH); // remove the database file if it exists
    
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
    rc += test_report("Send Gap From Clock Hole Test:", test_send_gap_from_clock_hole(":memory:"));

    // Chunked payload tests run only when INTEGRATION_TEST_CHUNKED_DATABASE_ID points at a
    // tenant with a small payload_max_chunk_size; state the skip reason once for the group.
    const char *chunked_id = getenv("INTEGRATION_TEST_CHUNKED_DATABASE_ID");
    printf("\n-- Chunked Payload Tests%s --\n",
           (chunked_id && *chunked_id) ? "" : " (INTEGRATION_TEST_CHUNKED_DATABASE_ID not set, skipping)");
    rc += test_report("Chunked Paths Test:", test_chunked_payload_paths());
    rc += test_report("Chunked Rowset Test:", test_chunked_payload_rowset_path());
    rc += test_report("Chunked Single-Sync Drain Test:", test_chunked_payload_single_sync_drain());
    rc += test_report("Chunked Capped Receive Test:", test_chunked_payload_capped_receive());
    rc += test_report("Chunked Batched Receive Test:", test_chunked_payload_batched_receive());
    rc += test_report("Chunked Failure Test:", test_chunked_send_failure_preserves_checkpoint());
    rc += test_report("Chunked Negative Cache Test:", test_chunked_negative_cache_invalidation());
    printf("\n");

    rc += test_report("Offline Error Test:", test_offline_error(":memory:"));
    rc += test_report("Double Empty Init Test:", test_double_empty_network_init(":memory:"));
    rc += test_report("Failure Path Test:", test_failure_path(":memory:"));

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
