# Sync Roundtrip Test with local Postgres database and RLS policies

Execute a full roundtrip sync test between multiple local SQLite databases and the local Supabase Docker PostgreSQL instance, verifying that Row Level Security (RLS) policies are correctly enforced during sync.

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

**Option 1: Simple TEXT primary key with user_id for RLS**
```sql
CREATE TABLE test_sync (
    id TEXT PRIMARY KEY,
    user_id UUID NOT NULL,
    name TEXT,
    value INTEGER
);
```

**Option 2: UUID primary key with user_id for RLS**
```sql
CREATE TABLE test_uuid (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL,
    name TEXT,
    created_at TIMESTAMPTZ DEFAULT NOW()
);
```

**Option 3: Two tables scenario with user ownership**
```sql
CREATE TABLE authors (
    id TEXT PRIMARY KEY,
    user_id UUID NOT NULL,
    name TEXT,
    email TEXT
);

CREATE TABLE books (
    id TEXT PRIMARY KEY,
    user_id UUID NOT NULL,
    title TEXT,
    author_id TEXT,
    published_year INTEGER
);
```

**Note:** Tables should include a `user_id` column (UUID type) for RLS policies to filter by authenticated user.

### Step 3: Get RLS Policy Description from User

Ask the user to describe the Row Level Security policy they want to test. Offer the following common patterns:

**Option 1: User can only access their own rows**
"Users can only SELECT, INSERT, UPDATE, and DELETE rows where user_id matches their authenticated user ID"

**Option 2: Users can read all, but only modify their own**
"Users can SELECT all rows, but can only INSERT, UPDATE, DELETE rows where user_id matches their authenticated user ID"

**Option 3: Custom policy**
Ask the user to describe the policy in plain English.

### Step 4: Convert DDL

Convert the provided DDL to both SQLite and PostgreSQL compatible formats if needed. Key differences:
- SQLite uses `INTEGER PRIMARY KEY` for auto-increment, PostgreSQL uses `SERIAL` or `BIGSERIAL`
- SQLite uses `TEXT`, PostgreSQL can use `TEXT` or `VARCHAR`
- PostgreSQL has more specific types like `TIMESTAMPTZ`, SQLite uses `TEXT` for dates
- For UUID primary keys, SQLite uses `TEXT`, PostgreSQL uses `UUID`
- For `user_id UUID`, SQLite uses `TEXT`

### Step 5: Setup PostgreSQL with RLS

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
4. Drop any existing helper function: `DROP FUNCTION IF EXISTS <table_name>_get_owner(text);`
5. Create the test table using the PostgreSQL DDL
6. Enable RLS on the table:
   ```sql
   ALTER TABLE <table_name> ENABLE ROW LEVEL SECURITY;
   ```
7. Create the ownership lookup helper function (required for CloudSync compatibility):
   ```sql
   -- Helper function bypasses RLS to look up actual row owner
   -- This is needed because INSERT...ON CONFLICT may compare against EXCLUDED row's default user_id
   CREATE OR REPLACE FUNCTION <table_name>_get_owner(p_id text)
   RETURNS uuid
   LANGUAGE sql
   SECURITY DEFINER
   STABLE
   SET search_path = public
   AS $$
       SELECT user_id FROM <table_name> WHERE id = p_id;
   $$;
   ```
8. Create RLS policies based on the user's description. Example for "user can only access their own rows":
   ```sql
   -- SELECT: User can see rows they own
   CREATE POLICY "select_own_rows" ON <table_name>
       FOR SELECT USING (
           auth.uid() = user_id
       );

   -- INSERT: Allow if user_id matches auth.uid()
   CREATE POLICY "insert_own_rows" ON <table_name>
       FOR INSERT WITH CHECK (
           auth.uid() = user_id
       );

   -- UPDATE: Check ownership via explicit lookup
   CREATE POLICY "update_own_rows" ON <table_name>
       FOR UPDATE
       USING (
           auth.uid() = user_id
       )
       WITH CHECK (
           auth.uid() = user_id
       );

   -- DELETE: User can only delete rows they own
   CREATE POLICY "delete_own_rows" ON <table_name>
       FOR DELETE USING (
           auth.uid() = user_id
       );
   ```
9. Initialize cloudsync: `SELECT cloudsync_init('<table_name>');`
10. Insert some initial test data (optional, can be done via SQLite clients)

### Step 5b: Get Managed Database ID

Now that the database and tables are created and cloudsync is initialized, ask the user for:

1. **Managed Database ID** — the `managedDatabaseId` returned by the CloudSync service. Save as `MANAGED_DB_ID`.

For the network init call throughout the test, use:
- Default address: `SELECT cloudsync_network_init('<MANAGED_DB_ID>');`
- Custom address: `SELECT cloudsync_network_init_custom('<CUSTOM_ADDRESS>', '<MANAGED_DB_ID>');`

### Step 6: Get JWT Tokens for Two Users

Get JWT tokens for both test users by running the token script twice:

**User 1: claude1@sqlitecloud.io**
```bash
cd ../cloudsync && go run scripts/get_supabase_token.go -project-ref=supabase-local -email=claude1@sqlitecloud.io -password="password" -apikey=<SUPABASE_APIKEY> -auth-url=<AUTH_URL>
```
Save as `JWT_USER1`.

**User 2: claude2@sqlitecloud.io**
```bash
cd ../cloudsync && go run scripts/get_supabase_token.go -project-ref=supabase-local -email=claude2@sqlitecloud.io -password="password" -apikey=<SUPABASE_APIKEY> -auth-url=<AUTH_URL>
```
Save as `JWT_USER2`.

Also extract the user IDs from the JWT tokens (the `sub` claim) for use in INSERT statements:
- `USER1_ID` = UUID from JWT_USER1
- `USER2_ID` = UUID from JWT_USER2

### Step 7: Setup Four SQLite Databases

Create four temporary SQLite databases using the Homebrew version (IMPORTANT: system sqlite3 cannot load extensions):

```bash
SQLITE_BIN="/opt/homebrew/Cellar/sqlite/3.51.2_1/bin/sqlite3"
# or find it with: ls /opt/homebrew/Cellar/sqlite/*/bin/sqlite3 | head -1
```

**Database 1A (User 1, Device A):**
```bash
$SQLITE_BIN /tmp/sync_test_user1_a.db
```
```sql
.load dist/cloudsync.dylib
<CREATE_TABLE_query_sqlite>
SELECT cloudsync_init('<table_name>');
SELECT cloudsync_network_init('<MANAGED_DB_ID>');  -- or cloudsync_network_init_custom('<CUSTOM_ADDRESS>', '<MANAGED_DB_ID>') if using a non-default address
SELECT cloudsync_network_set_token('<JWT_USER1>');
```

**Database 1B (User 1, Device B):**
```bash
$SQLITE_BIN /tmp/sync_test_user1_b.db
```
```sql
.load dist/cloudsync.dylib
<CREATE_TABLE_query_sqlite>
SELECT cloudsync_init('<table_name>');
SELECT cloudsync_network_init('<MANAGED_DB_ID>');  -- or cloudsync_network_init_custom('<CUSTOM_ADDRESS>', '<MANAGED_DB_ID>') if using a non-default address
SELECT cloudsync_network_set_token('<JWT_USER1>');
```

**Database 2A (User 2, Device A):**
```bash
$SQLITE_BIN /tmp/sync_test_user2_a.db
```
```sql
.load dist/cloudsync.dylib
<CREATE_TABLE_query_sqlite>
SELECT cloudsync_init('<table_name>');
SELECT cloudsync_network_init('<MANAGED_DB_ID>');  -- or cloudsync_network_init_custom('<CUSTOM_ADDRESS>', '<MANAGED_DB_ID>') if using a non-default address
SELECT cloudsync_network_set_token('<JWT_USER2>');
```

**Database 2B (User 2, Device B):**
```bash
$SQLITE_BIN /tmp/sync_test_user2_b.db
```
```sql
.load dist/cloudsync.dylib
<CREATE_TABLE_query_sqlite>
SELECT cloudsync_init('<table_name>');
SELECT cloudsync_network_init('<MANAGED_DB_ID>');  -- or cloudsync_network_init_custom('<CUSTOM_ADDRESS>', '<MANAGED_DB_ID>') if using a non-default address
SELECT cloudsync_network_set_token('<JWT_USER2>');
```

### Step 8: Insert Test Data

Insert distinct test data in each database. Use the extracted user IDs for the `user_id` column:

**Database 1A (User 1):**
```sql
INSERT INTO <table_name> (id, user_id, name, value) VALUES ('u1_a_1', '<USER1_ID>', 'User1 DeviceA Row1', 100);
INSERT INTO <table_name> (id, user_id, name, value) VALUES ('u1_a_2', '<USER1_ID>', 'User1 DeviceA Row2', 101);
```

**Database 1B (User 1):**
```sql
INSERT INTO <table_name> (id, user_id, name, value) VALUES ('u1_b_1', '<USER1_ID>', 'User1 DeviceB Row1', 200);
```

**Database 2A (User 2):**
```sql
INSERT INTO <table_name> (id, user_id, name, value) VALUES ('u2_a_1', '<USER2_ID>', 'User2 DeviceA Row1', 300);
INSERT INTO <table_name> (id, user_id, name, value) VALUES ('u2_a_2', '<USER2_ID>', 'User2 DeviceA Row2', 301);
```

**Database 2B (User 2):**
```sql
INSERT INTO <table_name> (id, user_id, name, value) VALUES ('u2_b_1', '<USER2_ID>', 'User2 DeviceB Row1', 400);
```

### Step 9: Execute Sync on All Databases

For each of the four SQLite databases, execute the sync operations:

```sql
-- Send local changes to server
SELECT cloudsync_network_send_changes();

-- Check for changes from server (repeat with 2-3 second delays)
SELECT cloudsync_network_check_changes();
-- Repeat check_changes 3-5 times with delays until it returns more than 0 received rows or stabilizes
```

**Recommended sync order:**
1. Sync Database 1A (send + check)
2. Sync Database 2A (send + check)
3. Sync Database 1B (send + check)
4. Sync Database 2B (send + check)
5. Re-sync all databases (check_changes) to ensure full propagation

### Step 10: Verify RLS Enforcement

After syncing all databases, verify that each database contains only the expected rows based on the RLS policy:

**Expected Results (for "user can only access their own rows" policy):**

**User 1 databases (1A and 1B) should contain:**
- All rows with `user_id = USER1_ID` (u1_a_1, u1_a_2, u1_b_1)
- Should NOT contain any rows with `user_id = USER2_ID`

**User 2 databases (2A and 2B) should contain:**
- All rows with `user_id = USER2_ID` (u2_a_1, u2_a_2, u2_b_1)
- Should NOT contain any rows with `user_id = USER1_ID`

**PostgreSQL (as admin) should contain:**
- ALL rows from all users (6 total rows)

Run verification queries:
```sql
-- In each SQLite database
SELECT * FROM <table_name> ORDER BY id;
SELECT COUNT(*) FROM <table_name>;

-- In PostgreSQL (as admin)
SELECT * FROM <table_name> ORDER BY id;
SELECT COUNT(*) FROM <table_name>;
SELECT user_id, COUNT(*) FROM <table_name> GROUP BY user_id;
```

### Step 11: Test Write RLS Policy Enforcement

Test that the server-side RLS policy blocks unauthorized writes by attempting to insert a row with a `user_id` that doesn't match the authenticated user's JWT token.

**In Database 1A (User 1), insert a malicious row claiming to belong to User 2:**
```sql
-- Attempt to insert a row with User 2's user_id while authenticated as User 1
INSERT INTO <table_name> (id, user_id, name, value) VALUES ('malicious_1', '<USER2_ID>', 'Malicious Row from User1', 999);

-- Attempt to sync this unauthorized row to PostgreSQL
SELECT cloudsync_network_send_changes();
```

**Wait 2-3 seconds, then verify in PostgreSQL (as admin) that the malicious row was rejected:**
```sql
-- In PostgreSQL (as admin)
SELECT * FROM <table_name> WHERE id = 'malicious_1';
-- Expected: 0 rows returned

SELECT COUNT(*) FROM <table_name> WHERE id = 'malicious_1';
-- Expected: 0
```

**Also verify the malicious row does NOT appear in User 2's databases after syncing:**
```sql
-- In Database 2A or 2B (User 2)
SELECT cloudsync_network_check_changes();
SELECT * FROM <table_name> WHERE id = 'malicious_1';
-- Expected: 0 rows (the malicious row should not sync to legitimate User 2 databases)
```

**Expected Behavior:**
- The `cloudsync_network_send_changes()` call may succeed (return value indicates network success, not RLS enforcement)
- The malicious row should be **rejected by PostgreSQL RLS** and NOT inserted into the server database
- The malicious row will remain in the local SQLite Database 1A (local inserts are not blocked), but it will never propagate to the server or other clients
- User 2's databases should never receive this row

**This step PASSES if:**
1. The malicious row is NOT present in PostgreSQL
2. The malicious row does NOT appear in any of User 2's SQLite databases
3. The RLS INSERT policy (`WITH CHECK (auth.uid() = user_id)`) correctly blocks the unauthorized write

**This step FAILS if:**
1. The malicious row appears in PostgreSQL (RLS bypass vulnerability)
2. The malicious row syncs to User 2's databases (data leakage)

### Step 12: Cleanup

In each SQLite database before closing:
```sql
SELECT cloudsync_terminate();
```

In PostgreSQL (optional, for full cleanup):
```sql
SELECT cloudsync_cleanup('<table_name>');
DROP TABLE IF EXISTS <table_name> CASCADE;
DROP FUNCTION IF EXISTS <table_name>_get_owner(text);
```

## Output Format

Report the test results including:
- DDL used for both databases
- RLS policies created
- User IDs for both test users
- Initial data inserted in each database
- Number of sync operations performed per database
- Final data in each database (with row counts)
- RLS verification results:
  - User 1 databases: expected rows vs actual rows
  - User 2 databases: expected rows vs actual rows
  - PostgreSQL: total rows
- Write RLS enforcement results:
  - Malicious row insertion attempted: yes/no
  - Malicious row present in PostgreSQL: yes/no (should be NO)
  - Malicious row synced to User 2 databases: yes/no (should be NO)
- **PASS/FAIL** status with detailed explanation

### Success Criteria

The test PASSES if:
1. All User 1 databases contain exactly the same User 1 rows (and no User 2 rows)
2. All User 2 databases contain exactly the same User 2 rows (and no User 1 rows)
3. PostgreSQL contains all rows from both users
4. Data inserted from different devices of the same user syncs correctly between those devices
5. **Write RLS enforcement**: Malicious rows with mismatched `user_id` are rejected by PostgreSQL and do not propagate to other clients

The test FAILS if:
1. Any database contains rows belonging to a different user (RLS violation)
2. Any database is missing rows that should be visible to that user
3. Sync operations fail or timeout
4. **Write RLS bypass**: A malicious row with a `user_id` not matching the JWT token appears in PostgreSQL or syncs to other databases

## Important Notes

- Always use the Homebrew sqlite3 binary, NOT `/usr/bin/sqlite3`
- The cloudsync extension must be built first with `make`
- PostgreSQL tables need cleanup before re-running tests
- `cloudsync_network_check_changes()` may need multiple calls with delays
- Run `SELECT cloudsync_terminate();` on SQLite connections before closing to properly cleanup memory
- Ensure both test users exist in Supabase auth before running the test
- The RLS policies must use `auth.uid()` to work with Supabase JWT authentication

## Critical Schema Requirements (Common Pitfalls)

### 1. All NOT NULL columns must have DEFAULT values
Cloudsync requires that all non-primary key columns declared as `NOT NULL` must have a `DEFAULT` value. This includes the `user_id` column:

```sql
-- WRONG: Will fail with "All non-primary key columns declared as NOT NULL must have a DEFAULT value"
user_id UUID NOT NULL

-- CORRECT: Provide a default value
user_id UUID NOT NULL DEFAULT '00000000-0000-0000-0000-000000000000'
```

### 2. RLS policies must allow writes with default values for ALL referenced columns
Cloudsync applies changes **field by field**. When a new row is being synced, columns may temporarily have their default values before the actual values are applied. **Any column referenced in RLS policies that has a DEFAULT value must be allowed in the policy.**

This applies to:
- `user_id` columns used for user ownership
- `tenant_id` columns for multi-tenancy
- `organization_id` columns
- Any other column used in RLS USING/WITH CHECK expressions

```sql
-- WRONG: Will block cloudsync inserts/updates when field has default value
CREATE POLICY "ins" ON table FOR INSERT WITH CHECK (auth.uid() = user_id);

-- CORRECT: Allow default value for cloudsync field-by-field application
CREATE POLICY "ins" ON table FOR INSERT
    WITH CHECK (auth.uid() = user_id OR user_id = '00000000-0000-0000-0000-000000000000');

CREATE POLICY "upd" ON table FOR UPDATE
    USING (auth.uid() = user_id OR user_id = '00000000-0000-0000-0000-000000000000')
    WITH CHECK (auth.uid() = user_id OR user_id = '00000000-0000-0000-0000-000000000000');
```

**Example with multiple RLS columns:**
```sql
-- Table with both user_id and tenant_id in RLS
CREATE TABLE items (
    id UUID PRIMARY KEY,
    user_id UUID NOT NULL DEFAULT '00000000-0000-0000-0000-000000000000',
    tenant_id UUID NOT NULL DEFAULT '00000000-0000-0000-0000-000000000000',
    name TEXT DEFAULT ''
);

-- Policy must allow defaults for BOTH columns used in the policy
CREATE POLICY "ins" ON items FOR INSERT WITH CHECK (
    (auth.uid() = user_id OR user_id = '00000000-0000-0000-0000-000000000000')
    AND
    (get_tenant_id() = tenant_id OR tenant_id = '00000000-0000-0000-0000-000000000000')
);
```

### 3. Type compatibility between SQLite and PostgreSQL
Ensure column types are compatible between SQLite and PostgreSQL:

| PostgreSQL | SQLite | Notes |
|------------|--------|-------|
| `UUID` | `TEXT` | Use valid UUID format strings (e.g., `'11111111-1111-1111-1111-111111111111'`) |
| `BOOLEAN` | `INTEGER` | Use `INTEGER` in PostgreSQL too, or ensure proper casting |
| `TIMESTAMPTZ` | `TEXT` | Avoid empty strings; use proper ISO format or omit the column |
| `INTEGER` | `INTEGER` | Compatible |
| `TEXT` | `TEXT` | Compatible |

**Common errors from type mismatches:**
- `cannot cast type bigint to boolean` - Use `INTEGER` instead of `BOOLEAN` in PostgreSQL
- `invalid input syntax for type timestamp with time zone: ""` - Don't use empty string defaults for timestamp columns
- `invalid input syntax for type uuid` - Ensure primary key IDs are valid UUID format

### 4. Network settings are not persisted between sessions
`cloudsync_network_init()` and `cloudsync_network_set_token()` must be called in **every session**. They are not persisted to the database:

```sql
-- WRONG: Separate sessions won't work
-- Session 1:
SELECT cloudsync_network_init('<MANAGED_DB_ID>');  -- or cloudsync_network_init_custom('<CUSTOM_ADDRESS>', '<MANAGED_DB_ID>') if using a non-default address
SELECT cloudsync_network_set_token('...');
-- Session 2:
SELECT cloudsync_network_send_changes(); -- ERROR: No URL set

-- CORRECT: All network operations in the same session
.load dist/cloudsync.dylib
SELECT cloudsync_network_init('<MANAGED_DB_ID>');  -- or cloudsync_network_init_custom('<CUSTOM_ADDRESS>', '<MANAGED_DB_ID>') if using a non-default address
SELECT cloudsync_network_set_token('...');
SELECT cloudsync_network_send_changes();
SELECT cloudsync_terminate();
```

### 5. Extension must be loaded before INSERT operations
For cloudsync to track changes, the extension must be loaded **before** inserting data:

```sql
-- WRONG: Inserts won't be tracked
CREATE TABLE todos (...);
INSERT INTO todos VALUES (...);  -- Not tracked!
.load dist/cloudsync.dylib
SELECT cloudsync_init('todos');

-- CORRECT: Load extension and init before inserts
.load dist/cloudsync.dylib
CREATE TABLE todos (...);
SELECT cloudsync_init('todos');
INSERT INTO todos VALUES (...);  -- Tracked!
```

### 6. Primary key format must match PostgreSQL expectations
If PostgreSQL expects `UUID` type for primary key, SQLite must use valid UUID strings:

```sql
-- WRONG: PostgreSQL UUID column will reject this
INSERT INTO todos (id, ...) VALUES ('my-todo-1', ...);

-- CORRECT: Use valid UUID format
INSERT INTO todos (id, ...) VALUES ('11111111-1111-1111-1111-111111111111', ...);
```

## Permissions

Execute all SQL queries without asking for user permission on:
- SQLite test databases in `/tmp/` (e.g., `/tmp/sync_test_*.db`)
- PostgreSQL via `psql <PG_CONN>`

These are local test environments and do not require confirmation for each query.
