//
//  integration.c
//  cloudsync
//
//  Created by Gioele Cantoni on 05/06/25.
//  Set INTEGRATION_TEST_OFFLINE_DATABASE_ID and INTEGRATION_TEST_DATABASE_ID environment variables before running this test.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include "utils.h"
#include "sqlite3.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

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
    if (!offline_db_id) {
        printf("Skipping offline error test: INTEGRATION_TEST_OFFLINE_DATABASE_ID not set.\n");
        rc = SQLITE_OK;
        goto abort_test;
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

int version(void){
    sqlite3 *db = NULL;
    int rc = open_load_ext(":memory:", &db);

    rc = db_print(db, "SELECT cloudsync_version();");
    RCHECK

ABORT_TEST
}

#ifndef _WIN32
typedef enum {
    MOCK_INVALID_STATUS,
    MOCK_INVALID_CHECK,
    MOCK_INVALID_UPLOAD,
    MOCK_INVALID_APPLY,
    MOCK_INVALID_SCHEMA_CHECK,
    MOCK_INVALID_SCHEMA_DOWNLOAD,
    MOCK_INVALID_SCHEMA_UPLOAD,
    MOCK_SCHEMA_UPLOAD_AUTH_ERROR,
    MOCK_SCHEMA_CHECK_HTTP_EMPTY_ERROR,
    MOCK_SCHEMA_UPLOAD_MISSING_STATUS,
    MOCK_FIRST_SCHEMA_SYNC
} mock_network_scenario;

typedef struct {
    int listen_fd;
    int port;
    volatile int stop;
    pthread_t thread;
    mock_network_scenario scenario;
    const unsigned char *payload;
    int payload_len;
    const char *migration_json;
} mock_network_server;

static int mock_send_all(int fd, const void *buffer, size_t len) {
    const char *ptr = (const char *)buffer;
    while (len > 0) {
        ssize_t sent = send(fd, ptr, len, 0);
        if (sent <= 0) return SQLITE_ERROR;
        ptr += sent;
        len -= (size_t)sent;
    }
    return SQLITE_OK;
}

static void mock_send_response(int fd, const char *content_type, const void *body, size_t body_len) {
    char header[512];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nContent-Type: %s\r\nConnection: close\r\n\r\n",
             body_len, content_type ? content_type : "application/json");
    mock_send_all(fd, header, strlen(header));
    if (body && body_len > 0) mock_send_all(fd, body, body_len);
}

static void mock_send_status_response(int fd, const char *status, const char *content_type, const void *body, size_t body_len) {
    char header[512];
    snprintf(header, sizeof(header),
             "HTTP/1.1 %s\r\nContent-Length: %zu\r\nContent-Type: %s\r\nConnection: close\r\n\r\n",
             status, body_len, content_type ? content_type : "application/json");
    mock_send_all(fd, header, strlen(header));
    if (body && body_len > 0) mock_send_all(fd, body, body_len);
}

static void mock_send_text(int fd, const char *body) {
    mock_send_response(fd, "application/json", body, strlen(body));
}

static void mock_absorb_request_body(int fd, const char *request, int received) {
    const char *content_length = strstr(request, "Content-Length:");
    if (!content_length) content_length = strstr(request, "content-length:");
    const char *body = strstr(request, "\r\n\r\n");
    if (!content_length || !body) return;
    long expected = strtol(content_length + strlen("Content-Length:"), NULL, 10);
    long have = received - (long)((body + 4) - request);
    char scratch[1024];
    while (have < expected) {
        ssize_t n = recv(fd, scratch, sizeof(scratch), 0);
        if (n <= 0) break;
        have += (long)n;
    }
}

static void mock_handle_client(mock_network_server *server, int fd) {
    char request[8192];
    int received = 0;
    while (received < (int)sizeof(request) - 1) {
        ssize_t n = recv(fd, request + received, sizeof(request) - 1 - (size_t)received, 0);
        if (n <= 0) return;
        received += (int)n;
        request[received] = '\0';
        if (strstr(request, "\r\n\r\n")) break;
    }

    char method[16] = {0};
    char path[1024] = {0};
    sscanf(request, "%15s %1023s", method, path);
    mock_absorb_request_body(fd, request, received);

    if (server->stop) {
        mock_send_text(fd, "{}");
        return;
    }

    if (strstr(path, "/schema/check")) {
        if (server->scenario == MOCK_INVALID_SCHEMA_CHECK) {
            mock_send_text(fd, "not-json");
        } else if (server->scenario == MOCK_SCHEMA_CHECK_HTTP_EMPTY_ERROR) {
            mock_send_status_response(fd, "503 Service Unavailable", "application/json", NULL, 0);
        } else if (server->scenario == MOCK_FIRST_SCHEMA_SYNC) {
            char *body = sqlite3_mprintf("{\"migration\":%s}", server->migration_json);
            mock_send_text(fd, body ? body : "{}");
            sqlite3_free(body);
        } else {
            mock_send_text(fd, "{\"status\":\"none\"}");
        }
    } else if (strstr(path, "/schema/download")) {
        if (server->scenario == MOCK_INVALID_SCHEMA_DOWNLOAD) mock_send_text(fd, "not-json");
        else mock_send_text(fd, "{\"status\":\"none\"}");
    } else if (strstr(path, "/schema/upload")) {
        if (server->scenario == MOCK_INVALID_SCHEMA_UPLOAD) mock_send_text(fd, "not-json");
        else if (server->scenario == MOCK_SCHEMA_UPLOAD_AUTH_ERROR) {
            const char *body = "{\"error\":\"missing schema api key\"}";
            mock_send_status_response(fd, "403 Forbidden", "application/json", body, strlen(body));
        } else if (server->scenario == MOCK_SCHEMA_UPLOAD_MISSING_STATUS) {
            mock_send_text(fd, "{}");
        }
        else mock_send_text(fd, "{\"status\":\"uploaded\"}");
    } else if (strstr(path, "/blob-upload")) {
        mock_send_text(fd, "");
    } else if (strstr(path, "/download")) {
        mock_send_response(fd, "application/octet-stream", server->payload, server->payload_len);
    } else if (strstr(path, "/status")) {
        if (server->scenario == MOCK_INVALID_STATUS) mock_send_text(fd, "not-json");
        else mock_send_text(fd, "{\"lastOptimisticVersion\":0,\"lastConfirmedVersion\":0,\"gaps\":[]}");
    } else if (strstr(path, "/upload")) {
        if (server->scenario == MOCK_INVALID_UPLOAD) {
            mock_send_text(fd, "not-json");
        } else {
            char body[256];
            snprintf(body, sizeof(body), "{\"url\":\"http://127.0.0.1:%d/blob-upload\"}", server->port);
            mock_send_text(fd, body);
        }
    } else if (strstr(path, "/apply")) {
        if (server->scenario == MOCK_INVALID_APPLY) mock_send_text(fd, "not-json");
        else mock_send_text(fd, "{\"lastOptimisticVersion\":1,\"lastConfirmedVersion\":1,\"gaps\":[]}");
    } else if (strstr(path, "/check")) {
        if (server->scenario == MOCK_INVALID_CHECK) {
            mock_send_text(fd, "not-json");
        } else if (server->scenario == MOCK_FIRST_SCHEMA_SYNC) {
            char body[256];
            snprintf(body, sizeof(body), "{\"url\":\"http://127.0.0.1:%d/download\"}", server->port);
            mock_send_text(fd, body);
        } else {
            mock_send_text(fd, "{\"lastOptimisticVersion\":0,\"lastConfirmedVersion\":0,\"gaps\":[]}");
        }
    } else {
        mock_send_text(fd, "{}");
    }
}

static void *mock_network_worker(void *arg) {
    mock_network_server *server = (mock_network_server *)arg;
    while (!server->stop) {
        int fd = accept(server->listen_fd, NULL, NULL);
        if (fd < 0) continue;
        mock_handle_client(server, fd);
        close(fd);
    }
    return NULL;
}

static int mock_network_start(mock_network_server *server, mock_network_scenario scenario) {
    memset(server, 0, sizeof(*server));
    server->scenario = scenario;
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) return SQLITE_ERROR;

    int reuse = 1;
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) return SQLITE_ERROR;
    if (listen(server->listen_fd, 16) != 0) return SQLITE_ERROR;

    socklen_t len = sizeof(addr);
    if (getsockname(server->listen_fd, (struct sockaddr *)&addr, &len) != 0) return SQLITE_ERROR;
    server->port = ntohs(addr.sin_port);
    if (pthread_create(&server->thread, NULL, mock_network_worker, server) != 0) return SQLITE_ERROR;
    return SQLITE_OK;
}

static void mock_network_stop(mock_network_server *server) {
    if (!server || server->listen_fd <= 0) return;
    server->stop = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((uint16_t)server->port);
        connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        close(fd);
    }
    pthread_join(server->thread, NULL);
    close(server->listen_fd);
    server->listen_fd = -1;
}

static int mock_network_init_db(sqlite3 *db, mock_network_server *server) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT cloudsync_network_init_custom('http://127.0.0.1:%d', 'mockdb');", server->port);
    return db_exec(db, sql);
}

static int expect_sql_error_contains(sqlite3 *db, const char *sql, const char *expected) {
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc == SQLITE_OK) {
        printf("Error: expected SQL failure while executing %s\n", sql);
        return SQLITE_ERROR;
    }
    if (expected && (!errmsg || !strstr(errmsg, expected))) {
        printf("Error: expected message containing \"%s\", got \"%s\"\n", expected, errmsg ? errmsg : "NULL");
        sqlite3_free(errmsg);
        return SQLITE_ERROR;
    }
    sqlite3_free(errmsg);
    return SQLITE_OK;
}

static int mock_prepare_synced_row(sqlite3 *db) {
    int rc = db_exec(db,
        "CREATE TABLE IF NOT EXISTS notes (id TEXT PRIMARY KEY NOT NULL, body TEXT NOT NULL DEFAULT '');"
        "SELECT cloudsync_init('notes');"
        "INSERT INTO notes (id, body) VALUES ('n1', 'hello');");
    return rc;
}

static int mock_prepare_large_synced_row(sqlite3 *db) {
    sqlite3_str *str = sqlite3_str_new(NULL);
    if (!str) return SQLITE_NOMEM;

    sqlite3_str_appendall(str, "CREATE TABLE IF NOT EXISTS notes (id TEXT PRIMARY KEY NOT NULL, body TEXT NOT NULL DEFAULT ''");
    for (int i = 0; i < 90; ++i) {
        sqlite3_str_appendf(str, ", extra_%02d TEXT", i);
    }
    sqlite3_str_appendall(str,
        ");"
        "SELECT cloudsync_init('notes');"
        "INSERT INTO notes (id, body) VALUES ('n1', 'hello');");

    char *sql = sqlite3_str_finish(str);
    if (!sql) return SQLITE_NOMEM;
    int rc = db_exec(db, sql);
    sqlite3_free(sql);
    return rc;
}

static int select_payload_blob(sqlite3 *db, unsigned char **payload, int *payload_len) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) FROM cloudsync_changes;";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) { sqlite3_finalize(stmt); return SQLITE_ERROR; }
    int len = sqlite3_column_bytes(stmt, 0);
    const void *blob = sqlite3_column_blob(stmt, 0);
    if (!blob || len <= 0) { sqlite3_finalize(stmt); return SQLITE_ERROR; }
    unsigned char *copy = sqlite3_malloc(len);
    if (!copy) { sqlite3_finalize(stmt); return SQLITE_NOMEM; }
    memcpy(copy, blob, (size_t)len);
    *payload = copy;
    *payload_len = len;
    sqlite3_finalize(stmt);
    return SQLITE_OK;
}

static char *mock_build_large_first_schema_migration(void) {
    sqlite3_str *str = sqlite3_str_new(NULL);
    if (!str) return NULL;

    sqlite3_str_appendall(str,
        "{"
        "\"type\":\"cloudsync.schema.migration\","
        "\"formatVersion\":1,"
        "\"migrationId\":\"mock-first-schema-sync-large\","
        "\"ops\":["
        "{\"op\":\"createTable\",\"table\":\"notes\",\"columns\":["
        "{\"name\":\"id\",\"type\":\"text\",\"primaryKey\":true,\"nullable\":false},"
        "{\"name\":\"body\",\"type\":\"text\",\"nullable\":false,\"default\":{\"type\":\"text\",\"value\":\"\"}}");
    for (int i = 0; i < 90; ++i) {
        sqlite3_str_appendf(str, ",{\"name\":\"extra_%02d\",\"type\":\"text\",\"nullable\":true}", i);
    }
    sqlite3_str_appendall(str,
        "]},"
        "{\"op\":\"augmentTable\",\"table\":\"notes\",\"algorithm\":\"CLS\",\"initFlags\":0}"
        "]"
        "}");

    return sqlite3_str_finish(str);
}

static int test_mock_network_json_validation_one(mock_network_scenario scenario, const char *sql, bool expect_error_json) {
    mock_network_server server;
    int rc = mock_network_start(&server, scenario);
    if (rc != SQLITE_OK) return rc;

    sqlite3 *db = NULL;
    rc = open_load_ext(":memory:", &db);
    if (rc != SQLITE_OK) goto cleanup;
    rc = mock_network_init_db(db, &server);
    if (rc != SQLITE_OK) goto cleanup;

    if (scenario == MOCK_INVALID_UPLOAD || scenario == MOCK_INVALID_APPLY || scenario == MOCK_INVALID_CHECK) {
        rc = mock_prepare_synced_row(db);
        if (rc != SQLITE_OK) goto cleanup;
    }

    if (expect_error_json) rc = db_expect_int(db, sql, 1);
    else rc = expect_sql_error_contains(db, sql, "invalid JSON");

cleanup:
    if (db) {
        db_exec(db, "SELECT cloudsync_terminate();");
        sqlite3_close(db);
    }
    mock_network_stop(&server);
    return rc;
}

static int test_mock_network_json_validation(void) {
    int rc = SQLITE_OK;
    rc += test_mock_network_json_validation_one(MOCK_INVALID_STATUS, "SELECT cloudsync_network_status();", false);
    rc += test_mock_network_json_validation_one(MOCK_INVALID_CHECK, "SELECT cloudsync_network_check_changes() LIKE '%invalid JSON%';", true);
    rc += test_mock_network_json_validation_one(MOCK_INVALID_UPLOAD, "SELECT cloudsync_network_send_changes();", false);
    rc += test_mock_network_json_validation_one(MOCK_INVALID_APPLY, "SELECT cloudsync_network_send_changes();", false);
    rc += test_mock_network_json_validation_one(MOCK_INVALID_SCHEMA_CHECK, "SELECT cloudsync_network_migration_check();", false);
    rc += test_mock_network_json_validation_one(MOCK_INVALID_SCHEMA_DOWNLOAD, "SELECT cloudsync_network_migration_download();", false);
    rc += test_mock_network_json_validation_one(MOCK_INVALID_SCHEMA_UPLOAD, "SELECT cloudsync_network_migration_upload('{\"ops\":[]}');", false);
    return rc == SQLITE_OK ? SQLITE_OK : SQLITE_ERROR;
}

static int test_mock_schema_check_empty_error(void) {
    mock_network_server server;
    int rc = mock_network_start(&server, MOCK_SCHEMA_CHECK_HTTP_EMPTY_ERROR);
    if (rc != SQLITE_OK) return rc;

    sqlite3 *db = NULL;
    rc = open_load_ext(":memory:", &db);
    if (rc != SQLITE_OK) goto cleanup;
    rc = mock_network_init_db(db, &server);
    if (rc != SQLITE_OK) goto cleanup;

    rc = expect_sql_error_contains(db, "SELECT cloudsync_network_migration_check();", "CloudSync schema migration endpoint failed");

cleanup:
    if (db) {
        db_exec(db, "SELECT cloudsync_terminate();");
        sqlite3_close(db);
    }
    mock_network_stop(&server);
    return rc;
}

static int test_mock_migration_upload_error_keeps_pending(void) {
    mock_network_server server;
    int rc = mock_network_start(&server, MOCK_SCHEMA_UPLOAD_AUTH_ERROR);
    if (rc != SQLITE_OK) return rc;

    sqlite3 *db = NULL;
    rc = open_load_ext(":memory:", &db);
    if (rc != SQLITE_OK) goto cleanup;
    rc = mock_network_init_db(db, &server);
    if (rc != SQLITE_OK) goto cleanup;

    rc = db_exec(db,
        "SELECT cloudsync_alter_create_table('upload_notes');"
        "SELECT cloudsync_alter_add_column('upload_notes', 'id', 'text', 0);"
        "SELECT cloudsync_alter_add_primary_key('upload_notes', 'id');"
        "SELECT cloudsync_alter_augment_table('upload_notes');"
        "SELECT cloudsync_alter_apply();");
    if (rc != SQLITE_OK) goto cleanup;

    rc = db_expect_int(db, "SELECT count(*) FROM cloudsync_pending_migration WHERE uploaded_at IS NULL;", 1);
    if (rc != SQLITE_OK) goto cleanup;
    rc = expect_sql_error_contains(db, "SELECT cloudsync_network_migration_upload();", NULL);
    if (rc != SQLITE_OK) goto cleanup;
    rc = db_expect_int(db, "SELECT count(*) FROM cloudsync_pending_migration WHERE uploaded_at IS NULL;", 1);
    if (rc != SQLITE_OK) goto cleanup;
    rc = db_exec(db, "INSERT INTO upload_notes (id) VALUES ('u1');");
    if (rc != SQLITE_OK) goto cleanup;
    rc = expect_sql_error_contains(db, "SELECT cloudsync_network_send_changes();", "pending schema migration");

cleanup:
    if (db) {
        db_exec(db, "SELECT cloudsync_terminate();");
        sqlite3_close(db);
    }
    mock_network_stop(&server);
    return rc;
}

static int test_mock_migration_upload_missing_status_keeps_pending(void) {
    mock_network_server server;
    int rc = mock_network_start(&server, MOCK_SCHEMA_UPLOAD_MISSING_STATUS);
    if (rc != SQLITE_OK) return rc;

    sqlite3 *db = NULL;
    rc = open_load_ext(":memory:", &db);
    if (rc != SQLITE_OK) goto cleanup;
    rc = mock_network_init_db(db, &server);
    if (rc != SQLITE_OK) goto cleanup;

    rc = db_exec(db,
        "SELECT cloudsync_alter_create_table('upload_missing_status_notes');"
        "SELECT cloudsync_alter_add_column('upload_missing_status_notes', 'id', 'text', 0);"
        "SELECT cloudsync_alter_add_primary_key('upload_missing_status_notes', 'id');"
        "SELECT cloudsync_alter_augment_table('upload_missing_status_notes');"
        "SELECT cloudsync_alter_apply();");
    if (rc != SQLITE_OK) goto cleanup;

    rc = db_expect_int(db, "SELECT count(*) FROM cloudsync_pending_migration WHERE uploaded_at IS NULL;", 1);
    if (rc != SQLITE_OK) goto cleanup;
    rc = expect_sql_error_contains(db, "SELECT cloudsync_network_migration_upload();", "accepted status");
    if (rc != SQLITE_OK) goto cleanup;
    rc = db_expect_int(db, "SELECT count(*) FROM cloudsync_pending_migration WHERE uploaded_at IS NULL;", 1);
    if (rc != SQLITE_OK) goto cleanup;
    rc = db_exec(db, "INSERT INTO upload_missing_status_notes (id) VALUES ('u1');");
    if (rc != SQLITE_OK) goto cleanup;
    rc = expect_sql_error_contains(db, "SELECT cloudsync_network_send_changes();", "pending schema migration");

cleanup:
    if (db) {
        db_exec(db, "SELECT cloudsync_terminate();");
        sqlite3_close(db);
    }
    mock_network_stop(&server);
    return rc;
}

static int test_mock_first_schema_sync(void) {
    sqlite3 *source = NULL;
    sqlite3 *target = NULL;
    unsigned char *payload = NULL;
    int payload_len = 0;
    char *migration_json = NULL;
    mock_network_server server;
    int rc = open_load_ext(":memory:", &source);
    if (rc != SQLITE_OK) goto cleanup;
    rc = mock_prepare_large_synced_row(source);
    if (rc != SQLITE_OK) goto cleanup;
    rc = select_payload_blob(source, &payload, &payload_len);
    if (rc != SQLITE_OK) goto cleanup;

    rc = mock_network_start(&server, MOCK_FIRST_SCHEMA_SYNC);
    if (rc != SQLITE_OK) goto cleanup;
    server.payload = payload;
    server.payload_len = payload_len;
    migration_json = mock_build_large_first_schema_migration();
    if (!migration_json || strlen(migration_json) < 4096) { rc = SQLITE_ERROR; goto cleanup_server; }
    server.migration_json = migration_json;

    rc = open_load_ext(":memory:", &target);
    if (rc != SQLITE_OK) goto cleanup_server;
    rc = mock_network_init_db(target, &server);
    if (rc != SQLITE_OK) goto cleanup_server;
    rc = db_exec(target, "SELECT cloudsync_network_sync(10, 1);");
    if (rc != SQLITE_OK) goto cleanup_server;
    rc = db_expect_int(target, "SELECT count(*) FROM notes WHERE id='n1' AND body='hello';", 1);
    if (rc != SQLITE_OK) goto cleanup_server;
    rc = db_expect_int(target, "SELECT count(*) FROM cloudsync_migrations WHERE migration_id='mock-first-schema-sync-large';", 1);

cleanup_server:
    mock_network_stop(&server);
cleanup:
    if (source) { db_exec(source, "SELECT cloudsync_terminate();"); sqlite3_close(source); }
    if (target) { db_exec(target, "SELECT cloudsync_terminate();"); sqlite3_close(target); }
    if (payload) sqlite3_free(payload);
    if (migration_json) sqlite3_free(migration_json);
    return rc;
}
#else
static int test_mock_network_json_validation(void) {
    printf("Skipping local mock network JSON test on Windows.\n");
    return SQLITE_OK;
}

static int test_mock_schema_check_empty_error(void) {
    printf("Skipping local mock schema empty error test on Windows.\n");
    return SQLITE_OK;
}

static int test_mock_migration_upload_error_keeps_pending(void) {
    printf("Skipping local mock migration upload auth test on Windows.\n");
    return SQLITE_OK;
}

static int test_mock_migration_upload_missing_status_keeps_pending(void) {
    printf("Skipping local mock migration upload ack test on Windows.\n");
    return SQLITE_OK;
}

static int test_mock_first_schema_sync(void) {
    printf("Skipping local mock first schema sync test on Windows.\n");
    return SQLITE_OK;
}
#endif

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

    rc += test_report("Mock Network JSON Test:", test_mock_network_json_validation());
    rc += test_report("Mock Schema Empty Error:", test_mock_schema_check_empty_error());
    rc += test_report("Mock Migration Upload Auth:", test_mock_migration_upload_error_keeps_pending());
    rc += test_report("Mock Migration Upload Ack:", test_mock_migration_upload_missing_status_keeps_pending());
    rc += test_report("Mock First Schema Sync Test:", test_mock_first_schema_sync());
    rc += test_report("Double Empty Init Test:", test_double_empty_network_init(":memory:"));

    if (!getenv("INTEGRATION_TEST_DATABASE_ID")) {
        printf("Skipping remote integration tests: INTEGRATION_TEST_DATABASE_ID not set.\n");
        remove(DB_PATH);
        cloudsync_memory_finalize();
        printf("\n");
        return rc;
    }

    sqlite3 *db = NULL;
    rc += open_load_ext(DB_PATH, &db);
    rc += db_init(db);
    if (db) sqlite3_close(db);

    rc += test_report("Init+Sync Test:", test_init(DB_PATH, 0));
    rc += test_report("Is Enabled Test:", test_is_enabled(DB_PATH));
    rc += test_report("DB Version Test:", test_db_version(DB_PATH));
    rc += test_report("Enable Disable Test:", test_enable_disable(DB_PATH));
    rc += test_report("Offline Error Test:", test_offline_error(":memory:"));

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
