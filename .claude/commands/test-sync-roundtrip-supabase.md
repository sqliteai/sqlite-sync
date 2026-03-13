# Sync Roundtrip Test with local Postgres database

Execute a full roundtrip sync test between a local SQLite database and the local Supabase Docker PostgreSQL instance.

## Prerequisites
- Supabase instance running (local Docker or remote)
- Built cloudsync extension (`make` to build `dist/cloudsync.dylib`)

## Test Procedure

### Step 1: Get Connection Parameters

Ask the user for the following parameters:

1. **CloudSync server address** — propose `https://cloudsync.sqlite.ai` as default (this is the built-in default). If the user provides a different address, save it as `CUSTOM_ADDRESS` and use `cloudsync_network_init_custom` instead of `cloudsync_network_init`.

2. **PostgreSQL connection string**: Propose `postgresql://supabase_admin:postgres@127.0.0.1:54322/postgres` as default. Save as `PG_CONN`. Use this for all `psql` connections throughout the test.

3. **Supabase API key** (used for JWT token generation): Propose `sb_secret_N7UND0UgjKTVK-Uodkm0Hg_xSvEMPvz` as default. Save as `SUPABASE_APIKEY`.

Derive `AUTH_URL` from the PostgreSQL connection string by extracting the host and using port `54321` (Supabase GoTrue). For example, if `PG_CONN` is `postgresql://user:pass@10.0.0.5:54322/postgres`, then `AUTH_URL` is `http://10.0.0.5:54321`. For `127.0.0.1`, use `http://127.0.0.1:54321`.


### Step 2: Get DDL from User

Ask the user to provide a DDL query for the table(s) to test. It can be in PostgreSQL or SQLite format. Offer the following options:

**Option 1: Simple TEXT primary key**
```sql
CREATE TABLE test_sync (
    id TEXT PRIMARY KEY,
    name TEXT,
    value INTEGER
);
```

**Option 2: UUID primary key**
```sql
CREATE TABLE test_uuid (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name TEXT,
    created_at TIMESTAMPTZ DEFAULT NOW()
);
```

**Option 3: Two tables scenario (tests multi-table sync)**
```sql
CREATE TABLE authors (
    id TEXT PRIMARY KEY,
    name TEXT,
    email TEXT
);

CREATE TABLE books (
    id TEXT PRIMARY KEY,
    title TEXT,
    author_id TEXT,
    published_year INTEGER
);
```

**Note:** Avoid INTEGER PRIMARY KEY for sync tests as it is not recommended for distributed sync scenarios (conflicts with auto-increment across devices).

### Step 3: Convert DDL

Convert the provided DDL to both SQLite and PostgreSQL compatible formats if needed. Key differences:
- SQLite uses `INTEGER PRIMARY KEY` for auto-increment, PostgreSQL uses `SERIAL` or `BIGSERIAL`
- SQLite uses `TEXT`, PostgreSQL can use `TEXT` or `VARCHAR`
- PostgreSQL has more specific types like `TIMESTAMPTZ`, SQLite uses `TEXT` for dates
- For UUID primary keys, SQLite uses `TEXT`, PostgreSQL uses `UUID`

### Step 4: Get JWT Token

Run the token script from the cloudsync project:
```bash
cd ../cloudsync && go run scripts/get_supabase_token.go -project-ref=supabase-local -email=claude@sqlitecloud.io -password="password" -apikey=<SUPABASE_APIKEY> -auth-url=<AUTH_URL>
```
Save the JWT token for later use.

### Step 5: Setup PostgreSQL

Connect to Supabase PostgreSQL and prepare the environment:
```bash
psql <PG_CONN>
```

Inside psql:
1. List existing tables with `\dt` to find any `_cloudsync` metadata tables
2. For each table already configured for cloudsync (has a `<table_name>_cloudsync` companion table), run:
   ```sql
   SELECT cloudsync_cleanup('<table_name>');
   ```
3. Drop the test table if it exists: `DROP TABLE IF EXISTS <table_name> CASCADE;`
4. Create the test table using the PostgreSQL DDL
5. Initialize cloudsync: `SELECT cloudsync_init('<table_name>');`
6. Insert some test data into the table

### Step 5b: Get Managed Database ID

Now that the database and tables are created and cloudsync is initialized, ask the user for:

1. **Managed Database ID** — the `managedDatabaseId` returned by the CloudSync service. Save as `MANAGED_DB_ID`.

For the network init call throughout the test, use:
- Default address: `SELECT cloudsync_network_init('<MANAGED_DB_ID>');`
- Custom address: `SELECT cloudsync_network_init_custom('<CUSTOM_ADDRESS>', '<MANAGED_DB_ID>');`

### Step 6: Setup SQLite

Create a temporary SQLite database using the Homebrew version (IMPORTANT: system sqlite3 cannot load extensions):

```bash
SQLITE_BIN="/opt/homebrew/Cellar/sqlite/3.51.2_1/bin/sqlite3"
# or find it with: ls /opt/homebrew/Cellar/sqlite/*/bin/sqlite3 | head -1

$SQLITE_BIN /tmp/sync_test_$(date +%s).db
```

Inside sqlite3:
```sql
.load dist/cloudsync.dylib
-- Create table with SQLite DDL
<CREATE_TABLE_query>
SELECT cloudsync_init('<table_name>');
SELECT cloudsync_network_init('<MANAGED_DB_ID>');  -- or cloudsync_network_init_custom('<CUSTOM_ADDRESS>', '<MANAGED_DB_ID>') if using a non-default address
SELECT cloudsync_network_set_token('<jwt_token>');
-- Insert test data (different from PostgreSQL to test merge)
<INSERT_statements>
```

### Step 7: Execute Sync

In the SQLite session:
```sql
-- Send local changes to server
SELECT cloudsync_network_send_changes();

-- Check for changes from server (repeat with 2-3 second delays)
SELECT cloudsync_network_check_changes();
-- Repeat check_changes 3-5 times with delays until it returns more than 0 received rows or stabilizes

-- Verify final data
SELECT * FROM <table_name>;
```

### Step 8: Verify Results

1. In SQLite, run `SELECT * FROM <table_name>;` and capture the output
2. In PostgreSQL, run `SELECT * FROM <table_name>;` and capture the output
3. Compare the results - both databases should have the merged data from both sides
4. Report success/failure based on whether the data matches

## Output Format

Report the test results including:
- DDL used for both databases
- Initial data inserted in each database
- Number of sync operations performed
- Final data in both databases
- PASS/FAIL status with explanation

## Important Notes

- Always use the Homebrew sqlite3 binary, NOT `/usr/bin/sqlite3`
- The cloudsync extension must be built first with `make`
- PostgreSQL tables need cleanup before re-running tests
- `cloudsync_network_check_changes()` may need multiple calls with delays
- run `SELECT cloudsync_terminate();` on SQLite connections before closing the properly cleanup the memory

## Permissions

Execute all SQL queries without asking for user permission on:
- SQLite test databases in `/tmp/` (e.g., `/tmp/sync_test_*.db`)
- PostgreSQL via `psql <PG_CONN>`

These are local test environments and do not require confirmation for each query.
