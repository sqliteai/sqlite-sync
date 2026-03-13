# Sync Roundtrip Test with remote SQLiteCloud database and RLS policies

Execute a full roundtrip sync test between multiple local SQLite databases and the sqlitecloud, verifying that Row Level Security (RLS) policies are correctly enforced during sync.

## Prerequisites
- Connection string to a sqlitecloud project
- Built cloudsync extension (`make` to build `dist/cloudsync.dylib`)

### Step 1: Get CloudSync Parameters

Ask the user for:

1. **CloudSync server address** — propose `https://cloudsync.sqlite.ai` as default (this is the built-in default). If the user provides a different address, save it as `CUSTOM_ADDRESS` and use `cloudsync_network_init_custom` instead of `cloudsync_network_init`.

## Test Procedure

### Step 2: Get DDL from User

Ask the user to provide a DDL query for the table(s) to test. It can be in PostgreSQL or SQLite format. Offer the following options:

**Option 1: Simple TEXT primary key with user_id for RLS**
```sql
CREATE TABLE test_sync (
    id TEXT PRIMARY KEY,
    user_id TEXT NOT NULL,
    name TEXT,
    value INTEGER
);
```

**Option 2: Multi tables scenario for advanced RLS policy**

Propose a simple but multitables real world scenario

**Option 3: Custom policy**
Ask the user to describe the table/tables in plain English or DDL queries.

**Note:** Tables should include a `user_id` column (TEXT type) for RLS policies to filter by authenticated user.

### Step 3: Get RLS Policy Description from User

Ask the user to describe the Row Level Security policy they want to test. Offer the following common patterns:

**Option 1: User can only access their own rows**
"Users can only SELECT, INSERT, UPDATE, and DELETE rows where user_id matches their authenticated user ID"

**Option : Users can read all, but only modify their own**
"Users can SELECT all rows, but can only INSERT, UPDATE, DELETE rows where user_id matches their authenticated user ID"

**Option 3: Custom policy**
Ask the user to describe the policy in plain English.

### Step 4: Get sqlitecloud connection string from User

Ask the user to provide a connection string in the form of "sqlitecloud://<host>:<port>/<db_name>?apikey=<apikey>" to be later used with the sqlitecloud cli (sqlc) with `~/go/bin/sqlc "<connection_string>"`.

### Step 5: Setup SQLiteCloud with RLS

Connect to SQLiteCloud and prepare the environment:
```bash
~/go/bin/sqlc <connection_string>
```

The last command inside sqlc to exit from the cli program must be `quit`.

If the db_name doesn't exists, try again to connect without specifing the <db_name>, then inside sqlc:
1. CREATE DATABASE <db_name>
2. USE DATABASE <db_name>

Then, inside sqlc:
1. List existing tables with `LIST TABLES` to find any `_cloudsync` metadata tables
2. For each table already configured for cloudsync (has a `<table_name>_cloudsync` companion table), run:
   ```sql
   CLOUDSYNC DISABLE <table_name>
   ```
3. Drop the test table if it exists: `DROP TABLE IF EXISTS <table_name>;`
5. Create the test table using the SQLite DDL
6. Enable RLS on the table:
   ```sql
   ENABLE RLS DATABASE <db_name> TABLE <table_name>
   ```
7. Create RLS policies based on the user's description. 
Your RLS policies for INSERT, UPDATE, and DELETE operations can reference column values as they are being changed. This is done using the special OLD.column and NEW.column identifiers. Their availability and meaning depend on the operation being performed:

+-----------+--------------------------------------------+--------------------------------------------+
| Operation | OLD.column Reference                       | NEW.column Reference                       |
+-----------+--------------------------------------------+--------------------------------------------+
| INSERT    | Not available                              | The value for the new row.                 |
| UPDATE    | The value of the row before the update.    | The value of the row after the update.     |
| DELETE    | The value of the row being deleted.        | Not available                              |
+-----------+--------------------------------------------+--------------------------------------------+

Example for "user can only access their own rows":
   ```sql
   -- SELECT: User can see rows they own
   SET RLS DATABASE <db_name> TABLE <table_name> SELECT "auth_userid() = user_id"
   
   -- INSERT: Allow if user_id matches auth_userid()
   SET RLS DATABASE <db_name> TABLE <table_name> INSERT "auth_userid() = NEW.user_id"

   -- UPDATE: Check ownership via explicit lookup
   SET RLS DATABASE <db_name> TABLE <table_name> UPDATE "auth_userid() = NEW.user_id AND auth_userid() = OLD.user_id"

   -- DELETE: User can only delete rows they own
   SET RLS DATABASE <db_name> TABLE <table_name> DELETE "auth_userid() = OLD.user_id"
   ```
8. Ask the user to enable CloudSync on the table from the SQLiteCloud dashboard

### Step 5b: Get Managed Database ID

Now that the database and tables are created and CloudSync is enabled on the dashboard, ask the user for:

1. **Managed Database ID** — the `managedDatabaseId` returned by the CloudSync service. For SQLiteCloud projects, it can be obtained from the project's OffSync page on the dashboard after enabling CloudSync on the table.

Save as `MANAGED_DB_ID`.

For the network init call throughout the test, use:
- Default address: `SELECT cloudsync_network_init('<MANAGED_DB_ID>');`
- Custom address: `SELECT cloudsync_network_init_custom('<CUSTOM_ADDRESS>', '<MANAGED_DB_ID>');`

<!-- Enable CloudSync on selected tables

### POST `/v1/orgs/:orgID/databases/:databaseID/cloudsync/enable`
Purpose: enable CloudSync on selected tables.

Fields:
- Path: `orgID`, `databaseID`
- Body: `tables` (non-empty string array)
- Response data: empty object

```bash
curl --request POST "<SYNC_SERVER_URL>/v1/orgs/<ORG_ID>/databases/<db_name>/cloudsync/enable" \
  --header "Authorization: Bearer $ORG_API_KEY" \
  --header "Content-Type: application/json" \
  --data '{"tables":["<table_name>"]}'
``` -->

9. Insert some initial test data (optional, can be done via SQLite clients)

### Step 6: Get tokens for Two Users

Get auth tokens for both test users by running the token script twice:

**User 1: claude1@sqlitecloud.io**
```bash
curl -X "POST" "https://<hostname_from_connection_string>/v2/tokens" \
   -H 'Authorization: Bearer <apikey_from_connection_string>' \
   -H 'Content-Type: application/json; charset=utf-8' \
   -d $'{
 "name": "claude1@sqlitecloud.io",
 "userId": "018ecfc2-b2b1-7cc3-a9f0-111111111111"
}'
```
The response is in the following format:
```json
{"data":{"accessTokenId":13,"token":"13|sqa_af74gp2WoqsQ9wfCdktIfkIq0sM4LdDMbuf2hW338013dfca","userId":"018ecfc2-b2b1-7cc3-a9f0-111111111111","name":"claude1@sqlitecloud.io","attributes":null,"expiresAt":null,"createdAt":"2026-03-02T23:11:38Z"},"metadata":{"connectedMs":17,"executedMs":30,"elapsedMs":47}}
```
save the userId and the token values as USER1_ID and TOKEN_USER1 to be reused later

**User 2: claude2@sqlitecloud.io**
```bash
curl -X "POST" "https://<hostname_from_connection_string>/v2/tokens" \
   -H 'Authorization: Bearer <apikey_from_connection_string>' \
   -H 'Content-Type: application/json; charset=utf-8' \
   -d $'{
 "name": "claude2@sqlitecloud.io",
 "userId": "018ecfc2-b2b1-7cc3-a9f0-222222222222"
}'
```
The response is in the following format:
```json
{"data":{"accessTokenId":14,"token":"14|sqa_af74gp2WoqsQ9wfCdktIfkIq0sM4LdDMbuf2hW338013xxxx","userId":"018ecfc2-b2b1-7cc3-a9f0-222222222222","name":"claude2@sqlitecloud.io","attributes":null,"expiresAt":null,"createdAt":"2026-03-02T23:11:38Z"},"metadata":{"connectedMs":17,"executedMs":30,"elapsedMs":47}}
```
save the userId and the token values as USER2_ID and TOKEN_USER2 to be reused later

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
SELECT cloudsync_network_set_token('<TOKEN_USER1>');
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
SELECT cloudsync_network_set_token('<TOKEN_USER1>');
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
SELECT cloudsync_network_set_token('<TOKEN_USER2>');
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
SELECT cloudsync_network_set_token('<TOKEN_USER2>');
```

### Step 8: Insert Test Data

Ask the user for optional details about the kind of test data to insert in the tables, otherwise generate some real world data for the choosen tables.
Insert distinct test data in each database. Use the extracted user IDs for the if needed. 
For example, for the simple table scenario:

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

Test that the server-side RLS policy blocks unauthorized writes by attempting to insert a row with a `user_id` that doesn't match the authenticated user's token.

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
3. The RLS INSERT policy correctly blocks the unauthorized write

**This step FAILS if:**
1. The malicious row appears in PostgreSQL (RLS bypass vulnerability)
2. The malicious row syncs to User 2's databases (data leakage)

### Step 12: Cleanup

In each SQLite database before closing:
```sql
SELECT cloudsync_terminate();
```

In SQLiteCloud (optional, for full cleanup):
```sql
CLOUDSYNC DISABLE <table_name>);
DROP TABLE IF EXISTS <table_name>;
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
  - SQLiteCloud: total rows
- Write RLS enforcement results:
  - Malicious row insertion attempted: yes/no
  - Malicious row present in SQLiteCloud: yes/no (should be NO)
  - Malicious row synced to User 2 databases: yes/no (should be NO)
- **PASS/FAIL** status with detailed explanation

### Success Criteria

The test PASSES if:
1. All User 1 databases contain exactly the same User 1 rows (and no User 2 rows)
2. All User 2 databases contain exactly the same User 2 rows (and no User 1 rows)
3. SQLiteCloud contains all rows from both users
4. Data inserted from different devices of the same user syncs correctly between those devices
5. **Write RLS enforcement**: Malicious rows with mismatched `user_id` are rejected by SQLiteCloud and do not propagate to other clients

The test FAILS if:
1. Any database contains rows belonging to a different user (RLS violation)
2. Any database is missing rows that should be visible to that user
3. Sync operations fail or timeout
4. **Write RLS bypass**: A malicious row with a `user_id` not matching the token appears in SQLiteCloud or syncs to other databases

## Important Notes

- Always use the Homebrew sqlite3 binary, NOT `/usr/bin/sqlite3`
- The cloudsync extension must be built first with `make`
- SQLiteCloud tables need cleanup before re-running tests
- `cloudsync_network_check_changes()` may need multiple calls with delays
- Run `SELECT cloudsync_terminate();` on SQLite connections before closing to properly cleanup memory
- Ensure both test users exist in Supabase auth before running the test
- The RLS policies must use `auth_userid()` to work with SQLiteCloud token authentication

## Critical Schema Requirements (Common Pitfalls)

### 1. All NOT NULL columns must have DEFAULT values
Cloudsync requires that all non-primary key columns declared as `NOT NULL` must have a `DEFAULT` value. This includes the `user_id` column:

```sql
-- WRONG: Will fail with "All non-primary key columns declared as NOT NULL must have a DEFAULT value"
user_id UUID NOT NULL

-- CORRECT: Provide a default value
user_id UUID NOT NULL DEFAULT '00000000-0000-0000-0000-000000000000'
```

### 2. Network settings are not persisted between sessions
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

### 3. Extension must be loaded before INSERT operations
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

## Permissions

Execute all SQL queries without asking for user permission on:
- SQLite test databases in `/tmp/` (e.g., `/tmp/sync_test_*.db`)
- SQLiteCloud via `~/go/bin/sqlc "<connection_string>"`

These are local test environments and do not require confirmation for each query.
