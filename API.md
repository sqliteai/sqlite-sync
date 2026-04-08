# API Reference

This document provides a reference for the SQLite functions provided by the `sqlite-sync` extension.

## Index

- [Configuration Functions](#configuration-functions)
  - [`cloudsync_init()`](#cloudsync_inittable_name-crdt_algo-init_flags)
  - [`cloudsync_enable()`](#cloudsync_enabletable_name)
  - [`cloudsync_disable()`](#cloudsync_disabletable_name)
  - [`cloudsync_is_enabled()`](#cloudsync_is_enabledtable_name)
  - [`cloudsync_set_filter()`](#cloudsync_set_filtertable_name-filter_expr)
  - [`cloudsync_clear_filter()`](#cloudsync_clear_filtertable_name)
  - [`cloudsync_cleanup()`](#cloudsync_cleanuptable_name)
  - [`cloudsync_terminate()`](#cloudsync_terminate)
- [Block-Level LWW Functions](#block-level-lww-functions)
  - [`cloudsync_set_column()`](#cloudsync_set_columntable_name-col_name-key-value)
  - [`cloudsync_text_materialize()`](#cloudsync_text_materializetable_name-col_name-pk_values)
- [Helper Functions](#helper-functions)
  - [`cloudsync_version()`](#cloudsync_version)
  - [`cloudsync_siteid()`](#cloudsync_siteid)
  - [`cloudsync_db_version()`](#cloudsync_db_version)
  - [`cloudsync_uuid()`](#cloudsync_uuid)
- [Schema Alteration Functions](#schema-alteration-functions)
  - [`cloudsync_begin_alter()`](#cloudsync_begin_altertable_name)
  - [`cloudsync_commit_alter()`](#cloudsync_commit_altertable_name)
- [Network Functions](#network-functions)
  - [`cloudsync_network_init()`](#cloudsync_network_initmanageddatabaseid)
  - [`cloudsync_network_cleanup()`](#cloudsync_network_cleanup)
  - [`cloudsync_network_set_token()`](#cloudsync_network_set_tokentoken)
  - [`cloudsync_network_set_apikey()`](#cloudsync_network_set_apikeyapikey)
  - [`cloudsync_network_send_changes()`](#cloudsync_network_send_changes)
  - [`cloudsync_network_check_changes()`](#cloudsync_network_check_changes)
  - [`cloudsync_network_sync()`](#cloudsync_network_syncwait_ms-max_retries)
  - [`cloudsync_network_reset_sync_version()`](#cloudsync_network_reset_sync_version)
  - [`cloudsync_network_has_unsent_changes()`](#cloudsync_network_has_unsent_changes)
  - [`cloudsync_network_logout()`](#cloudsync_network_logout)

---

## Configuration Functions

### `cloudsync_init(table_name, [crdt_algo], [init_flags])`

**Description:** Initializes a table for `sqlite-sync` synchronization. This function is idempotent and needs to be called only once per table on each site; configurations are stored in the database and automatically loaded with the extension.

Before initialization, `cloudsync_init` performs schema sanity checks to ensure compatibility with CRDT requirements and best practices. These checks include:
- Primary keys should not be auto-incrementing integers; GUIDs (UUIDs, ULIDs) are highly recommended to prevent multi-node collisions.
- All non-primary key `NOT NULL` columns must have a `DEFAULT` value.
- **Note:** Any write operation that includes a NULL value for a primary key column will be rejected with an error, even if SQLite would normally allow it due to a legacy behavior.

**Schema Design Considerations:**

When designing your database schema for SQLite Sync, follow these essential requirements:

- **Primary Keys**: Use TEXT primary keys with `cloudsync_uuid()` for globally unique identifiers. Avoid auto-incrementing integers.
- **Column Constraints**: All NOT NULL columns (except primary keys) must have DEFAULT values to prevent synchronization errors.
- **UNIQUE Constraints**: In multi-tenant scenarios, use composite UNIQUE constraints (e.g., `UNIQUE(tenant_id, email)`) instead of global uniqueness.
- **Foreign Key Compatibility**: Be aware of potential conflicts during CRDT merge operations and RLS policy interactions.
- **Trigger Compatibility**: Triggers may cause duplicate operations or be called multiple times due to column-by-column processing.

For comprehensive guidelines, see the [Database Schema Recommendations](docs/schema.md).

The function supports three overloads:
- `cloudsync_init(table_name)`: Uses the default 'cls' CRDT algorithm.
- `cloudsync_init(table_name, crdt_algo)`: Specifies a CRDT algorithm ('cls', 'dws', 'aws', 'gos').
- `cloudsync_init(table_name, crdt_algo, init_flags)`: Specifies an algorithm and a bitmask of initialization flags to control which schema sanity checks are skipped.

**Parameters:**

- `table_name` (TEXT): The name of the table to initialize.
- `crdt_algo` (TEXT, optional): The CRDT algorithm to use. Can be `"cls"`, `"dws"`, `"aws"`, `"gos"`. Defaults to `"cls"`.
- `init_flags` (INTEGER, optional): A bitmask of flags that control initialization behavior. Defaults to `0` (no flags). Available flags:
  - `0` — No flags; all sanity checks are performed (default).
  - `1` (`CLOUDSYNC_INIT_FLAG_SKIP_INT_PK_CHECK`) — Skip the check that prevents the use of a single-column INTEGER primary key. Use with caution; globally unique primary keys (UUID/ULID) are strongly recommended.
  - `2` (`CLOUDSYNC_INIT_FLAG_SKIP_NOT_NULL_DEFAULT_CHECK`) — Skip the check that requires all NOT NULL non-PK columns to have a DEFAULT value.
  - `4` (`CLOUDSYNC_INIT_FLAG_SKIP_NOT_NULL_PRIKEYS_CHECK`) — Skip the check that rejects NULL primary key values.
  - Flags can be combined with bitwise OR (e.g., `3` skips both the integer PK check and the NOT NULL default check).

**Returns:** None.

**Example:**

```sql
-- Initialize a table with the default CLS algorithm
SELECT cloudsync_init('my_table');

-- Initialize a table with the Delete-Wins Set algorithm
SELECT cloudsync_init('my_table', 'dws');

-- Initialize a table with an integer primary key (skip the integer PK check)
SELECT cloudsync_init('my_table', 'cls', 1);
```

---

### `cloudsync_enable(table_name)`

**Description:** Enables synchronization for the specified table.

**Parameters:**

- `table_name` (TEXT): The name of the table to enable.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_enable('my_table');
```

---

### `cloudsync_disable(table_name)`

**Description:** Disables synchronization for the specified table.

**Parameters:**

- `table_name` (TEXT): The name of the table to disable.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_disable('my_table');
```

---

### `cloudsync_is_enabled(table_name)`

**Description:** Checks if synchronization is enabled for the specified table.

**Parameters:**

- `table_name` (TEXT): The name of the table to check.

**Returns:** 1 if enabled, 0 otherwise.

**Example:**

```sql
SELECT cloudsync_is_enabled('my_table');
```

---

### `cloudsync_set_filter(table_name, filter_expr)`

**Description:** Sets a row-level filter expression on a synchronized table. Only rows that match the filter are tracked by the sync triggers; changes to rows that do not satisfy the expression are ignored and never replicated.

The filter expression is a standard SQL boolean expression written using bare column names (without a table or alias prefix). The extension automatically rewrites it with `NEW.` for INSERT/UPDATE triggers and `OLD.` for DELETE triggers. The expression is evaluated inside the trigger `WHEN` clause.

This function stores the filter in the table's settings and immediately recreates the sync triggers to apply it. The filter persists across database reopens. Use [`cloudsync_clear_filter()`](#cloudsync_clear_filtertable_name) to remove it.

**Parameters:**

- `table_name` (TEXT): The name of the synchronized table.
- `filter_expr` (TEXT): A SQL boolean expression referencing column names of the table. Only rows for which this expression evaluates to true are tracked for sync.

**Returns:** `1` on success.

**Example:**

```sql
-- Only sync tasks that are not marked as drafts
SELECT cloudsync_set_filter('tasks', "is_draft = 0");

-- Only sync rows belonging to a specific tenant
SELECT cloudsync_set_filter('orders', "tenant_id = 'acme'");

-- Combine conditions
SELECT cloudsync_set_filter('messages', "deleted = 0 AND type != 'ephemeral'");
```

---

### `cloudsync_clear_filter(table_name)`

**Description:** Removes the row-level filter previously set with [`cloudsync_set_filter()`](#cloudsync_set_filtertable_name-filter_expr). After clearing, all row changes in the table are tracked and replicated regardless of column values.

This function updates the stored settings and immediately recreates the sync triggers without a filter condition.

**Parameters:**

- `table_name` (TEXT): The name of the synchronized table.

**Returns:** `1` on success.

**Example:**

```sql
SELECT cloudsync_clear_filter('tasks');
```

---

### `cloudsync_cleanup(table_name)`

**Description:** Removes the `sqlite-sync` synchronization mechanism from a specified table or all tables. This operation drops the associated `_cloudsync` metadata table and removes triggers from the target table(s). Use this function when synchronization is no longer desired for a table.

**Parameters:**

- `table_name` (TEXT): The name of the table to clean up.

**Returns:** None.

**Example:**

```sql
-- Clean up a single table
SELECT cloudsync_cleanup('my_table');

```

---

### `cloudsync_terminate()`

**Description:** Releases all internal resources used by the `sqlite-sync` extension for the current database connection. This function should be called before closing the database connection to ensure that all prepared statements and allocated memory are freed. Failing to call this function can result in memory leaks or a failed `sqlite3_close` operation due to pending statements.

**Parameters:** None.

**Returns:** None.

**Example:**

```sql
-- Before closing the database connection
SELECT cloudsync_terminate();
```

---

## Block-Level LWW Functions

### `cloudsync_set_column(table_name, col_name, key, value)`

**Description:** Configures per-column settings for a synchronized table. This function is primarily used to enable **block-level LWW** on text columns, allowing fine-grained conflict resolution at the line (or paragraph) level instead of the entire cell.

When block-level LWW is enabled on a column, INSERT and UPDATE operations automatically split the text into blocks using a delimiter (default: newline `\n`) and track each block independently. During sync, changes are merged block-by-block, so concurrent edits to different parts of the same text are preserved.

**Parameters:**

- `table_name` (TEXT): The name of the synchronized table.
- `col_name` (TEXT): The name of the text column to configure.
- `key` (TEXT): The setting key. Supported keys:
  - `'algo'` — Set the column algorithm. Use value `'block'` to enable block-level LWW.
  - `'delimiter'` — Set the block delimiter string. Only applies to columns with block-level LWW enabled.
- `value` (TEXT): The setting value.

**Returns:** None.

**Example:**

```sql
-- Enable block-level LWW on a column (splits text by newline by default)
SELECT cloudsync_set_column('notes', 'body', 'algo', 'block');

-- Set a custom delimiter (e.g., double newline for paragraph-level tracking)
SELECT cloudsync_set_column('notes', 'body', 'delimiter', '

');
```

---

### `cloudsync_text_materialize(table_name, col_name, pk_values...)`

**Description:** Reconstructs the full text of a block-level LWW column from its individual blocks and writes the result back to the base table column. This is useful after a merge operation to ensure the column contains the up-to-date materialized text.

After a sync/merge, the column is updated automatically. This function is primarily useful for manual materialization or debugging.

**Parameters:**

- `table_name` (TEXT): The name of the table.
- `col_name` (TEXT): The name of the block-level LWW column.
- `pk_values...` (variadic): The primary key values identifying the row. For composite primary keys, pass each key value as a separate argument in declaration order.

**Returns:** `1` on success.

**Example:**

```sql
-- Materialize the body column for a specific row
SELECT cloudsync_text_materialize('notes', 'body', 'note-001');

-- With a composite primary key (e.g., PRIMARY KEY (tenant_id, doc_id))
SELECT cloudsync_text_materialize('docs', 'body', 'tenant-1', 'doc-001');

-- Read the materialized text
SELECT body FROM notes WHERE id = 'note-001';
```

---

## Helper Functions

### `cloudsync_version()`

**Description:** Returns the version of the `sqlite-sync` library.

**Parameters:** None.

**Returns:** The library version as a string.

**Example:**
```sql
SELECT cloudsync_version();
-- e.g., '1.0.0'
```

---

### `cloudsync_siteid()`

**Description:** Returns the unique ID of the local site.

**Parameters:** None.

**Returns:** The site ID as a BLOB.

**Example:**

```sql
SELECT cloudsync_siteid();
```

---

### `cloudsync_db_version()`

**Description:** Returns the current database version.

**Parameters:** None.

**Returns:** The database version as an INTEGER.

**Example:**

```sql
SELECT cloudsync_db_version();
```

---

### `cloudsync_uuid()`

**Description:** Generates a new universally unique identifier (UUIDv7). This is useful for creating globally unique primary keys for new records, which is a best practice for CRDTs.

**Parameters:** None.

**Returns:** A new UUID as a TEXT value.

**Example:**

```sql
INSERT INTO products (id, name) VALUES (cloudsync_uuid(), 'New Product');
```

---

## Schema Alteration Functions

### `cloudsync_begin_alter(table_name)`

**Description:** Prepares a synchronized table for schema changes. This function must be called before altering the table. Failure to use `cloudsync_begin_alter` and `cloudsync_commit_alter` can lead to synchronization errors and data divergence.

**Parameters:**

- `table_name` (TEXT): The name of the table that will be altered.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_init('my_table');
-- ... later
SELECT cloudsync_begin_alter('my_table');
ALTER TABLE my_table ADD COLUMN new_column TEXT;
SELECT cloudsync_commit_alter('my_table');
```

---

### `cloudsync_commit_alter(table_name)`

**Description:** Finalizes schema changes for a synchronized table. This function must be called after altering the table's schema, completing the process initiated by `cloudsync_begin_alter` and ensuring CRDT data consistency.

**Parameters:**

- `table_name` (TEXT): The name of the table that was altered.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_init('my_table');
-- ... later
SELECT cloudsync_begin_alter('my_type');
ALTER TABLE my_table ADD COLUMN new_column TEXT;
SELECT cloudsync_commit_alter('my_table');
```

---

## Network Functions

### `cloudsync_network_init(managedDatabaseId)`

**Description:** Initializes the `sqlite-sync` network component. This function configures the endpoints for the CloudSync service and initializes the cURL library.

**Parameters:**

- `managedDatabaseId` (TEXT): The managed database identifier returned by the CloudSync service when a new database is registered for sync. For SQLiteCloud projects, this value can be obtained from the project's OffSync page on the dashboard.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_network_init('your-managed-database-id');
```

---

### `cloudsync_network_cleanup()`

**Description:** Cleans up the `sqlite-sync` network component, releasing all resources allocated by `cloudsync_network_init` (memory, cURL handles).

**Parameters:** None.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_network_cleanup();
```

---

### `cloudsync_network_set_token(token)`

**Description:** Sets the authentication token to be used for network requests. This token will be included in the `Authorization` header of all subsequent requests. For more information, refer to the [Access Tokens documentation](https://docs.sqlitecloud.io/docs/access-tokens). 

**Parameters:**

- `token` (TEXT): The authentication token.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_network_set_token('your_auth_token');
```

---

### `cloudsync_network_set_apikey(apikey)`

**Description:** Sets the API key for network requests. This key is included in the `Authorization` header of all subsequent requests.

**Parameters:**

- `apikey` (TEXT): The API key.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_network_set_apikey('your_api_key');
```

---

### `cloudsync_network_send_changes()`

**Description:** Sends all unsent local changes to the remote server.

**Parameters:** None.

**Returns:** A JSON string with the send result:

```json
{"send": {"status": "synced|syncing|out-of-sync|error", "localVersion": N, "serverVersion": N}}
```

- `send.status`: The current sync state — `"synced"` (all changes confirmed), `"syncing"` (changes sent but not yet confirmed), `"out-of-sync"` (local changes pending or gaps detected), or `"error"`.
- `send.localVersion`: The latest local database version.
- `send.serverVersion`: The latest version confirmed by the server.

**Example:**

```sql
SELECT cloudsync_network_send_changes();
-- '{"send":{"status":"synced","localVersion":5,"serverVersion":5}}'
```

---

### `cloudsync_network_check_changes()`

**Description:** Checks the remote server for new changes and applies them to the local database.

If a package of new changes is already available for the local site, the server returns it immediately, and the changes are applied. If no package is ready, the server returns an empty response and starts an asynchronous process to prepare a new package. This new package can be retrieved with a subsequent call to this function.

This function is designed to be called periodically to keep the local database in sync.
To force an update and wait for changes (with a timeout), use [`cloudsync_network_sync(wait_ms, max_retries)`].

If the network is misconfigured or the remote server is unreachable, the function returns an error.

**Parameters:** None.

**Returns:** A JSON string with the receive result:

```json
{"receive": {"rows": N, "tables": ["table1", "table2"]}}
```

- `receive.rows`: The number of rows received and applied to the local database.
- `receive.tables`: An array of table names that received changes. Empty (`[]`) if no changes were applied.

**Example:**

```sql
SELECT cloudsync_network_check_changes();
-- '{"receive":{"rows":3,"tables":["tasks"]}}'
```

---

### `cloudsync_network_sync([wait_ms], [max_retries])`

**Description:** Performs a full synchronization cycle. This function has two overloads:

- `cloudsync_network_sync()`: Performs one send operation and one check operation.
- `cloudsync_network_sync(wait_ms, max_retries)`: Performs one send operation and then repeatedly tries to download remote changes until at least one change is downloaded or `max_retries` times has been reached, waiting `wait_ms` between retries.

**Parameters:**

- `wait_ms` (INTEGER, optional): The time to wait in milliseconds between retries. Defaults to 100.
- `max_retries` (INTEGER, optional): The maximum number of times to retry the synchronization. Defaults to 1.

**Returns:** A JSON string with the full sync result, combining send and receive:

```json
{
  "send": {"status": "synced|syncing|out-of-sync|error", "localVersion": N, "serverVersion": N},
  "receive": {"rows": N, "tables": ["table1", "table2"]}
}
```

- `send.status`: The current sync state — `"synced"`, `"syncing"`, `"out-of-sync"`, or `"error"`.
- `send.localVersion`: The latest local database version.
- `send.serverVersion`: The latest version confirmed by the server.
- `receive.rows`: The number of rows received and applied during the check phase.
- `receive.tables`: An array of table names that received changes. Empty (`[]`) if no changes were applied.

**Example:**

```sql
-- Perform a single synchronization cycle
SELECT cloudsync_network_sync();
-- '{"send":{"status":"synced","localVersion":5,"serverVersion":5},"receive":{"rows":3,"tables":["tasks"]}}'

-- Perform a synchronization cycle with custom retry settings
SELECT cloudsync_network_sync(500, 3);
```

---

### `cloudsync_network_reset_sync_version()`

**Description:** Resets local synchronization version numbers, forcing the next sync to fetch all changes from the server.

**Parameters:** None.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_network_reset_sync_version();
```

---

### `cloudsync_network_has_unsent_changes()`

**Description:** Checks if there are any local changes that have not yet been sent to the remote server.

**Parameters:** None.

**Returns:** 1 if there are unsent changes, 0 otherwise.

**Example:**

```sql
SELECT cloudsync_network_has_unsent_changes();
```

---

### `cloudsync_network_logout()`

**Description:** Logs out the current user and cleans up all local data from synchronized tables. This function deletes and then re-initializes synchronized tables, useful for switching users or resetting the local database. **Warning:** This function deletes all data from synchronized tables. Use with caution. Consider calling [`cloudsync_network_has_unsent_changes()`](#cloudsync_network_has_unsent_changes) before logout to check for unsent local changes and warn the user before data that has not been fully synchronized to the remote server is deleted.

**Parameters:** None.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_network_logout();
```
