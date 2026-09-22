// Exercise the integration wait policy without contacting the cloud.
#define main integration_main
#include "integration.c"
#undef main

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

typedef struct {
    const char *json;
    int ready;
} sync_reply;
typedef struct {
    const sync_reply *replies;
    int count;
    int calls;
} sync_script;

static void mock_sync(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    sync_script *script = sqlite3_user_data(ctx);
    CHECK(argc == 2 && sqlite3_value_int(argv[0]) == 250 && sqlite3_value_int(argv[1]) == 10);
    if (script->calls >= script->count) {
        sqlite3_result_error(ctx, "unexpected extra sync", -1);
        return;
    }
    sync_reply reply = script->replies[script->calls++];
    if (reply.ready) CHECK(sqlite3_exec(sqlite3_context_db_handle(ctx), "UPDATE fixture SET ready=1", NULL, NULL, NULL) == SQLITE_OK);
    if (reply.json) sqlite3_result_text(ctx, reply.json, -1, SQLITE_STATIC);
    else sqlite3_result_error(ctx, "injected SQL error", -1);
}

static void run_case(const sync_reply *replies, int count, int expected_rc, int expected_calls) {
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "CREATE TABLE fixture(ready); INSERT INTO fixture VALUES(0)", NULL, NULL, NULL) == SQLITE_OK);
    sync_script script = {replies, count, 0};
    CHECK(sqlite3_create_function(db, "cloudsync_network_sync", 2, SQLITE_UTF8, &script, mock_sync, NULL, NULL) == SQLITE_OK);
    CHECK(db_sync_await(db, "SELECT ready FROM fixture", count, 0) == expected_rc);
    CHECK(script.calls == expected_calls);
    CHECK(sqlite3_close(db) == SQLITE_OK);
}

int main(void) {
    const char *empty = "{\"send\":{\"status\":\"ok\"},\"receive\":{\"rows\":0}}";
    const char *rows = "{\"send\":{\"status\":\"ok\"},\"receive\":{\"rows\":3}}";
    sync_reply delayed[] = {{empty,0},{empty,0},{rows,1}};
    sync_reply partial[] = {{rows,0},{empty,0},{empty,1}};
    sync_reply never[] = {{empty,0},{empty,0},{empty,0}};
    sync_reply no_data[] = {{rows,0},{rows,0}};
    sync_reply no_rows[] = {{empty,1},{empty,1}};
    sync_reply immediate[] = {{rows,1}};
    for (int i = 0; i < 100; i++) {
        run_case(delayed, 3, SQLITE_OK, 3);
        run_case(partial, 3, SQLITE_OK, 3);
        run_case(immediate, 1, SQLITE_OK, 1);
    }
    run_case(never, 3, SQLITE_ERROR, 3);
    run_case(no_data, 2, SQLITE_ERROR, 2);
    run_case(no_rows, 2, SQLITE_ERROR, 2);
    const char *errors[] = {
        "{\"send\":{\"status\":\"error\"},\"receive\":{\"rows\":3}}",
        "{\"send\":{\"status\":\"ok\",\"lastFailure\":{\"message\":\"denied\"}},\"receive\":{\"rows\":3}}",
        "{\"send\":{\"status\":\"ok\"},\"receive\":{\"rows\":3,\"error\":\"denied\"}}",
        "{\"send\":{\"status\":\"ok\"},\"receive\":{\"rows\":3,\"lastFailure\":{\"message\":\"denied\"}}}",
        "{\"receive\":{\"rows\":3}}", "{}", "null", "{invalid", NULL
    };
    for (unsigned i = 0; i < sizeof(errors)/sizeof(errors[0]); i++) {
        sync_reply error[] = {{errors[i],1},{rows,1}};
        run_case(error, 2, SQLITE_ERROR, 1);
    }
    CHECK(sqlite3_memory_used() == 0);
    printf("Integration bootstrap policy: %d failures\n", failures);
    return failures ? 1 : 0;
}
