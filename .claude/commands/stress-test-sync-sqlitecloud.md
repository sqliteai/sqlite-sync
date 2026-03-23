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
4. **Operations per iteration** — how many UPDATE and DELETE operations to perform each iteration:
   - `NUM_UPDATES`: number of UPDATE operations per iteration (default: 1). Each UPDATE runs `UPDATE <table> SET value = value + 1;` affecting all rows.
   - `NUM_DELETES`: number of DELETE operations per iteration (default: 1). Each DELETE runs `DELETE FROM <table> WHERE rowid IN (SELECT rowid FROM <table> ORDER BY RANDOM() LIMIT 10);` removing 10 random rows. Set to 0 to skip deletes entirely.
   - Propose defaults of 1 update and 1 delete. The user can set 0 deletes for update-only tests.
5. **RLS mode** — with RLS (requires user tokens) or without RLS
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
- `NUM_UPDATES` (number of UPDATE operations per iteration, default 1)
- `NUM_DELETES` (number of DELETE operations per iteration, default 1; 0 to skip)

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

### Step 4: Set Up Authentication

Authentication depends on the RLS mode:

**If RLS is disabled:** Use the project `APIKEY` (already extracted from the connection string). After each `cloudsync_network_init`/`cloudsync_network_init_custom` call, authenticate with:
```sql
SELECT cloudsync_network_set_apikey('<APIKEY>');
```
No tokens are needed. Skip token creation entirely.

**If RLS is enabled:** Create tokens for the test users. Create as many users as needed for the number of concurrent databases (assign 2 databases per user, or 1 per user if NUM_DBS <= 2).

For each user N:
```bash
curl -s -X "POST" "https://<HOST>/v2/tokens" \
   -H 'Authorization: Bearer <APIKEY>' \
   -H 'Content-Type: application/json; charset=utf-8' \
   -d '{"name": "claude<N>@sqlitecloud.io", "userId": "018ecfc2-b2b1-7cc3-a9f0-<N_PADDED_12_CHARS>"}'
```

Save each user's `token` and `userId` from the response. After each `cloudsync_network_init`/`cloudsync_network_init_custom` call, authenticate with:
```sql
SELECT cloudsync_network_set_token('<TOKEN>');
```

**IMPORTANT:** Using a token when RLS is disabled will cause the server to silently reject all sent changes (send appears to succeed but data is not persisted remotely). Always use `cloudsync_network_set_apikey` when RLS is off.

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
     a. **UPDATE** — run `UPDATE <table> SET value = value + 1;` repeated `NUM_UPDATES` times (skip if 0)
     b. **DELETE** — run `DELETE FROM <table> WHERE rowid IN (SELECT rowid FROM <table> ORDER BY RANDOM() LIMIT 10);` repeated `NUM_DELETES` times (skip if 0)
     c. **Sync using the 3-step send/check/check pattern:**
        1. `SELECT cloudsync_network_send_changes();` — send local changes to the server
        2. `SELECT cloudsync_network_check_changes();` — ask the server to prepare a payload of remote changes
        3. Sleep 1 second (outside sqlite3, between two separate sqlite3 invocations)
        4. `SELECT cloudsync_network_check_changes();` — download the prepared payload, if any
   - Each sqlite3 session must: `.load` the extension, call `cloudsync_network_init()`/`cloudsync_network_init_custom()`, `cloudsync_network_set_apikey()`/`cloudsync_network_set_token()` (depending on RLS mode), do the work, call `cloudsync_terminate()`
   - **Timing**: Log the wall-clock execution time (in milliseconds) for each `cloudsync_network_send_changes()`, `cloudsync_network_check_changes()` call. Define a `now_ms()` helper function at the top of the script and use it before and after each sqlite3 invocation that calls a network function, computing the delta. On **macOS**, `date` does not support `%3N` (nanoseconds) — use `python3 -c 'import time; print(int(time.time()*1000))'` instead. On **Linux**, `date +%s%3N` works fine. The script should detect the platform and define `now_ms()` accordingly. Log lines like: `[DB<N>][iter <I>] send_changes: 123ms`, `[DB<N>][iter <I>] check_changes_1: 45ms`, `[DB<N>][iter <I>] check_changes_2: 67ms`
   - Include labeled output lines like `[DB<N>][iter <I>] updated count=<C>, deleted count=<D>` for grep-ability

3. **Launches all workers in parallel** using `&` and collects PIDs

4. **Waits for all workers** and captures exit codes

5. **Analyzes logs** for errors:
   - Grep all log files for: `error`, `locked`, `SQLITE_BUSY`, `database is locked`, `500`, `Error`
   - Report per-database: iterations completed, error count, sample error lines
   - Report total errors across all workers

6. **Prints final verdict**: PASS (0 errors) or FAIL (errors detected)

**Important script details:**
- Use `echo -e` to pipe generated SQL (with `\n` separators) into sqlite3
- During database initialization (Step 1), insert `ROWS` initial rows per database in a single transaction so each DB starts with data to update/delete. Row IDs should be unique across databases: `db<N>_r<J>`
- User IDs for rows must match the token's userId for RLS to work
- The sync pattern requires **separate sqlite3 invocations** for send_changes and each check_changes call (with a 1-second sleep between the two check_changes calls), so that timing can be measured per-call from bash
- **stderr capture**: All sqlite3 invocations must redirect both stdout and stderr to the log file. Use `>> "$LOG" 2>&1` (in this order — stdout redirect first, then stderr to stdout). For timed calls that capture output in a variable, redirect stderr to the log file separately: `RESULT=$(echo -e "$SQL" | $SQLITE3 "$DB" 2>> "$LOG")` and then echo `$RESULT` to the log as well. This ensures "Runtime error" messages from sqlite3 are never lost.
- Use `/bin/bash` (not `/bin/sh`) for arrays and process management

Run the script with a 10-minute timeout.

### Step 6: Detailed Error Analysis

After the test completes, provide a detailed breakdown:

1. **Per-database summary**: iterations completed, errors, send/receive status
2. **Error categorization**: group errors by type (e.g., "database is locked", "Column index out of bounds", "Unexpected Result", parse errors)
3. **Timeline analysis**: do errors cluster at specific iterations or spread evenly?
4. **Read full log files** if errors are found — show the first and last 30 lines of each log with errors

### Step 7: Final Sync and Data Integrity Verification

After all workers have terminated, perform a **final sync on every local database** to ensure all databases converge to the same state. Then verify data integrity.

**IMPORTANT — RLS mode changes what "convergence" means:** When RLS is enabled, each user can only see their own rows. Databases belonging to different users will have different row counts and different data — this is correct behavior. All convergence and integrity checks must therefore be scoped **per user group** (i.e., only compare databases that share the same userId/token).

1. **Final sync loop** (max 10 retries): Repeat the following until convergence is achieved within each user group, or the retry limit is reached:
   a. For each local database (sequentially):
      - Load the extension, call `cloudsync_network_init`/`cloudsync_network_init_custom`, authenticate with `cloudsync_network_set_apikey`/`cloudsync_network_set_token`
      - Run `SELECT cloudsync_network_sync(100, 10);` to sync remaining changes
      - Call `cloudsync_terminate()`
   b. After syncing all databases, query `SELECT COUNT(*) FROM <table>` on each database
   c. **If RLS is disabled:** Check that all databases have the same row count. If so, convergence is achieved — break.
   d. **If RLS is enabled:** Group databases by userId. Within each user group, check that all databases have the same row count. Convergence is achieved when every user group is internally consistent — break. Different user groups are expected to have different row counts.
   e. Otherwise, log the round number and the distinct row counts (per group if RLS), then repeat from (a)
   f. If the retry limit is reached without convergence, report it as a failure

2. **Row count verification**:
   - **If RLS is disabled:** Report the final row counts. All databases should have the same number of rows.
   - **If RLS is enabled:** Report row counts grouped by user. All databases within the same user group should have identical row counts. Different user groups may differ. Also verify that each database only contains rows matching its userId.
   - In both cases, also check SQLiteCloud (as admin) for total row count.

3. **Row content verification**:
   - **If RLS is disabled:** Pick one random row ID from the first database. Query that row on every local database. All must return identical values.
   - **If RLS is enabled:** For each user group, pick one random row ID from the first database in that group. Query that row on all databases in the same user group. All databases in the group must return identical values. Do NOT expect databases from other user groups to have this row — they should return empty (RLS blocks cross-user access).

4. **RLS cross-user leak check** (RLS mode only): For a sample of databases (e.g., one per user group), verify that `SELECT COUNT(*) FROM <table> WHERE user_id != '<expected_user_id>'` returns 0. Report any cross-user data leakage as a test failure.

## Output Format

Report the test results including:

| Metric | Value |
|--------|-------|
| Concurrent databases | N |
| Rows per iteration | ROWS |
| Iterations per database | ITERATIONS |
| Total CRUD operations | N × ITERATIONS × (UPDATE_ALL + DELETE_FEW) |
| Total sync operations | N × ITERATIONS × 3 (1 send_changes + 2 check_changes) |
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
3. After the final sync, databases converge:
   - **Without RLS:** all local databases have the same row count
   - **With RLS:** all databases within each user group have the same row count (different user groups may differ)
4. Row content is consistent:
   - **Without RLS:** a randomly selected row has identical content across all local databases
   - **With RLS:** a randomly selected row has identical content across all databases in the same user group; databases from other user groups correctly return empty for that row
5. **With RLS:** no cross-user data leakage (each database contains only rows matching its userId)

The test **FAILS** if:
1. Any worker crashes or fails to complete
2. Any `database is locked` or `SQLITE_BUSY` errors appear
3. Server returns 500 errors under concurrent load
4. Row counts differ within the comparison scope (all DBs without RLS, same-user DBs with RLS) after the final sync loop exhausts all retries
5. Row content differs within the comparison scope (data corruption)
6. **With RLS:** any database contains rows belonging to a different userId (cross-user data leakage)

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
