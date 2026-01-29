# Sync Roundtrip Test

Execute a full roundtrip sync test between a local SQLite database and the local Supabase Docker PostgreSQL instance.

## Prerequisites
- Supabase Docker container running (PostgreSQL on port 54322)
- HTTP sync server running on http://localhost:8091/postgres
- Built cloudsync extension (`make` to build `dist/cloudsync.dylib`)

## Test Procedure

### Step 1: Get DDL from User

Ask the user to provide a DDL query for the table(s) to test. It can be in PostgreSQL or SQLite format. Offer the following options:

**Option 1: Simple TEXT primary key**
```sql
CREATE TABLE test_sync (
    id TEXT PRIMARY KEY NOT NULL,
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
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT,
    email TEXT
);

CREATE TABLE books (
    id TEXT PRIMARY KEY NOT NULL,
    title TEXT,
    author_id TEXT,
    published_year INTEGER
);
```

**Note:** Avoid INTEGER PRIMARY KEY for sync tests as it is not recommended for distributed sync scenarios (conflicts with auto-increment across devices).

### Step 2: Convert DDL

Convert the provided DDL to both SQLite and PostgreSQL compatible formats if needed. Key differences:
- SQLite uses `INTEGER PRIMARY KEY` for auto-increment, PostgreSQL uses `SERIAL` or `BIGSERIAL`
- SQLite uses `TEXT`, PostgreSQL can use `TEXT` or `VARCHAR`
- PostgreSQL has more specific types like `TIMESTAMPTZ`, SQLite uses `TEXT` for dates
- For UUID primary keys, SQLite uses `TEXT`, PostgreSQL uses `UUID`

### Step 3: Get JWT Token

Run the token script from the cloudsync project:
```bash
cd ../cloudsync && go run scripts/get_supabase_token.go -project-ref=supabase-local -email=claude@sqlitecloud.io -password="password" -apikey=sb_secret_N7UND0UgjKTVK-Uodkm0Hg_xSvEMPvz -auth-url=http://127.0.0.1:54321
```
Save the JWT token for later use.

### Step 4: Setup PostgreSQL

Connect to Supabase PostgreSQL and prepare the environment:
```bash
psql postgresql://supabase_admin:postgres@127.0.0.1:54322/postgres
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

### Step 5: Setup SQLite

Create a temporary SQLite database using the Homebrew version (IMPORTANT: system sqlite3 cannot load extensions):

```bash
SQLITE_BIN="/opt/homebrew/Cellar/sqlite/3.50.4/bin/sqlite3"
# or find it with: ls /opt/homebrew/Cellar/sqlite/*/bin/sqlite3 | head -1

$SQLITE_BIN /tmp/sync_test_$(date +%s).db
```

Inside sqlite3:
```sql
.load dist/cloudsync.dylib
-- Create table with SQLite DDL
<CREATE_TABLE_query>
SELECT cloudsync_init('<table_name>');
SELECT cloudsync_network_init('http://localhost:8091/postgres');
SELECT cloudsync_network_set_token('<jwt_token>');
-- Insert test data (different from PostgreSQL to test merge)
<INSERT_statements>
```

### Step 6: Execute Sync

In the SQLite session:
```sql
-- Send local changes to server
SELECT cloudsync_network_send_changes();

-- Check for changes from server (repeat with 2-3 second delays)
SELECT cloudsync_network_check_changes();
-- Repeat check_changes 3-5 times with delays until it returns > 0 or stabilizes

-- Verify final data
SELECT * FROM <table_name>;
```

### Step 7: Verify Results

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
- PostgreSQL via `psql postgresql://supabase_admin:postgres@127.0.0.1:54322/postgres`

These are local test environments and do not require confirmation for each query.
