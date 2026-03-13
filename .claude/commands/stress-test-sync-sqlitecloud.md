# Sync Stress Test with remote SQLiteCloud database

Execute a stress test against the CloudSync server using multiple concurrent local SQLite databases syncing large volumes of CRUD operations simultaneously. Designed to reproduce server-side errors (e.g., "database is locked", 500 errors) under heavy concurrent load.

## Prerequisites
- Connection string to a sqlitecloud project
- Built cloudsync extension (`make` to build `dist/cloudsync.dylib`)

## Test Configuration

### Step 1: Gather Parameters

Ask the user for the following configuration using a single question set:

1. **CloudSync server address** — propose `https://cloudsync.sqlite.ai` as default (this is the built-in default). If the user provides a different address, save it as `CUSTOM_ADDRESS` and use `cloudsync_network_init_custom` instead of `cloudsync_network_init`.
2. **SQLiteCloud connection string** — format: `sqlitecloud://<host>:<port>/<db_name>?apikey=<apikey>`. If no `<db_name>` is in the path, ask the user for one or propose `test_stress_sync`.
3. **Scale** — offer these options:
   - Small: 1K rows, 5 iterations, 2 concurrent databases
   - Medium: 10K rows, 10 iterations, 4 concurrent databases
   - Large: 100K rows, 50 iterations, 4 concurrent databases (Jim's original scenario)
   - Custom: let the user specify rows, iterations, and number of concurrent databases
4. **RLS mode** — with RLS (requires user tokens) or without RLS
5. **Table schema** — offer simple default or custom:
   ```sql
   CREATE TABLE test_sync (id TEXT PRIMARY KEY, user_id TEXT NOT NULL DEFAULT '', name TEXT, value INTEGER);
   ```

Save these as variables:
- `CUSTOM_ADDRESS` (only if the user provided a non-default address)
- `CONNECTION_STRING` (the full sqlitecloud:// connection string)
- `DB_NAME` (database name extracted or provided)
- `HOST` (hostname extracted from connection string)
- `APIKEY` (apikey extracted from connection string)
- `ROWS` (number of rows per iteration)
- `ITERATIONS` (number of delete/insert/update cycles)
- `NUM_DBS` (number of concurrent databases)

### Step 2: Setup SQLiteCloud Database and Table

Connect to SQLiteCloud using `~/go/bin/sqlc` (last command must be `quit`). Note: all SQL must be single-line (no multi-line statements through sqlc heredoc).

1. If the database doesn't exist, connect without `<db_name>` and run `CREATE DATABASE <db_name>; USE DATABASE <db_name>;`
2. `LIST TABLES` to check for existing tables
3. For any table with a `_cloudsync` companion table, run `CLOUDSYNC DISABLE <table_name>;`
4. `DROP TABLE IF EXISTS <table_name>;`
5. Create the test table (single-line DDL)
6. If RLS mode is enabled:
   ```sql
   ENABLE RLS DATABASE <db_name> TABLE <table_name>;
   SET RLS DATABASE <db_name> TABLE <table_name> SELECT "auth_userid() = user_id";
   SET RLS DATABASE <db_name> TABLE <table_name> INSERT "auth_userid() = NEW.user_id";
   SET RLS DATABASE <db_name> TABLE <table_name> UPDATE "auth_userid() = NEW.user_id AND auth_userid() = OLD.user_id";
   SET RLS DATABASE <db_name> TABLE <table_name> DELETE "auth_userid() = OLD.user_id";
   ```
7. Ask the user to enable CloudSync on the table from the SQLiteCloud dashboard

### Step 3: Get Managed Database ID

Now that the database and tables are created and CloudSync is enabled on the dashboard, ask the user for:

1. **Managed Database ID** — the `managedDatabaseId` returned by the CloudSync service. For SQLiteCloud projects, it can be obtained from the project's OffSync page on the dashboard after enabling CloudSync on the table.

Save as `MANAGED_DB_ID`.

For the network init call throughout the test, use:
- Default address: `SELECT cloudsync_network_init('<MANAGED_DB_ID>');`
- Custom address: `SELECT cloudsync_network_init_custom('<CUSTOM_ADDRESS>', '<MANAGED_DB_ID>');`

### Step 4: Get Auth Tokens (if RLS enabled)

Create tokens for the test users. Create as many users as needed for the number of concurrent databases (assign 2 databases per user, or 1 per user if NUM_DBS <= 2).

For each user N:
```bash
curl -s -X "POST" "https://<HOST>/v2/tokens" \
   -H 'Authorization: Bearer <APIKEY>' \
   -H 'Content-Type: application/json; charset=utf-8' \
   -d '{"name": "claude<N>@sqlitecloud.io", "userId": "018ecfc2-b2b1-7cc3-a9f0-<N_PADDED_12_CHARS>"}'
```

Save each user's `token` and `userId` from the response.

If RLS is disabled, skip this step — tokens are not required.

### Step 5: Run the Concurrent Stress Test

Create a bash script at `/tmp/stress_test_concurrent.sh` that:

1. **Initializes N local SQLite databases** at `/tmp/sync_concurrent_<N>.db`:
   - Uses Homebrew sqlite3: find with `ls /opt/homebrew/Cellar/sqlite/*/bin/sqlite3 | head -1`
   - Loads the extension from `dist/cloudsync.dylib` (use absolute path from project root)
   - Creates the table and runs `cloudsync_init('<table_name>')`
   - Runs `cloudsync_terminate()` after init

2. **Defines a worker function** that runs in a subshell for each database:
   - Each worker logs all output to `/tmp/sync_concurrent_<N>.log`
   - Each iteration does:
     a. **DELETE all rows** → `send_changes()` → `check_changes()`
     b. **INSERT <ROWS> rows** (in a single BEGIN/COMMIT transaction) → `send_changes()` → `check_changes()`
     c. **UPDATE all rows** → `send_changes()` → `check_changes()`
   - Each session must: `.load` the extension, call `cloudsync_network_init()`, `cloudsync_network_set_token()` (if RLS), do the work, call `cloudsync_terminate()`
   - Include labeled output lines like `[DB<N>][iter <I>] deleted/inserted/updated, count=<C>` for grep-ability

3. **Launches all workers in parallel** using `&` and collects PIDs

4. **Waits for all workers** and captures exit codes

5. **Analyzes logs** for errors:
   - Grep all log files for: `error`, `locked`, `SQLITE_BUSY`, `database is locked`, `500`, `Error`
   - Report per-database: iterations completed, error count, sample error lines
   - Report total errors across all workers

6. **Prints final verdict**: PASS (0 errors) or FAIL (errors detected)

**Important script details:**
- Use `echo -e` to pipe generated INSERT SQL (with `\n` separators) into sqlite3
- Row IDs should be unique across databases and iterations: `db<N>_r<I>_<J>`
- User IDs for rows must match the token's userId for RLS to work
- Use `/bin/bash` (not `/bin/sh`) for arrays and process management

Run the script with a 10-minute timeout.

### Step 6: Detailed Error Analysis

After the test completes, provide a detailed breakdown:

1. **Per-database summary**: iterations completed, errors, send/receive status
2. **Error categorization**: group errors by type (e.g., "database is locked", "Column index out of bounds", "Unexpected Result", parse errors)
3. **Timeline analysis**: do errors cluster at specific iterations or spread evenly?
4. **Read full log files** if errors are found — show the first and last 30 lines of each log with errors

### Step 7: Optional — Verify Data Integrity

If the test passes (or even if some errors occurred), verify the final state:

1. Check each local SQLite database for row count
2. Check SQLiteCloud (as admin) for total row count
3. If RLS is enabled, verify no cross-user data leakage

## Output Format

Report the test results including:

| Metric | Value |
|--------|-------|
| Concurrent databases | N |
| Rows per iteration | ROWS |
| Iterations per database | ITERATIONS |
| Total CRUD operations | N × ITERATIONS × (DELETE_ALL + ROWS inserts + ROWS updates) |
| Total sync operations | N × ITERATIONS × 6 (3 sends + 3 checks) |
| Duration | start to finish time |
| Total errors | count |
| Error types | categorized list |
| Result | PASS/FAIL |

If errors are found, include:
- Full error categorization table
- Sample error messages
- Which databases were most affected
- Whether errors are client-side or server-side

## Success Criteria

The test **PASSES** if:
1. All workers complete all iterations
2. Zero `error`, `locked`, `SQLITE_BUSY`, or HTTP 500 responses in any log
3. Final row counts are consistent

The test **FAILS** if:
1. Any worker crashes or fails to complete
2. Any `database is locked` or `SQLITE_BUSY` errors appear
3. Server returns 500 errors under concurrent load
4. Data corruption or inconsistent row counts

## Important Notes

- Always use the Homebrew sqlite3 binary, NOT `/usr/bin/sqlite3`
- The cloudsync extension must be built first with `make`
- Network settings (`cloudsync_network_init`, `cloudsync_network_set_token`) are NOT persisted between sessions — must be called every time
- Extension must be loaded BEFORE any INSERT/UPDATE/DELETE for cloudsync to track changes
- All NOT NULL columns must have DEFAULT values
- `cloudsync_terminate()` must be called before closing each session
- sqlc heredoc only supports single-line SQL statements

## Permissions

Execute all SQL queries without asking for user permission on:
- SQLite test databases in `/tmp/` (e.g., `/tmp/sync_concurrent_*.db`, `/tmp/sync_concurrent_*.log`)
- SQLiteCloud via `~/go/bin/sqlc "<connection_string>"`
- Curl commands to the sync server and SQLiteCloud API for token creation

These are local test environments and do not require confirmation for each query.
