# API Reference

This document provides a reference for the SQLite functions provided by the `sqlite-sync` extension.

## Index

- [Configuration Functions](#configuration-functions)
  - [`cloudsync_init()`](#cloudsync_inittable_name-crdt_algo-force)
  - [`cloudsync_enable()`](#cloudsync_enabletable_name)
  - [`cloudsync_disable()`](#cloudsync_disabletable_name)
  - [`cloudsync_is_enabled()`](#cloudsync_is_enabledtable_name)
  - [`cloudsync_cleanup()`](#cloudsync_cleanuptable_name)
  - [`cloudsync_terminate()`](#cloudsync_terminate)
- [Helper Functions](#helper-functions)
  - [`cloudsync_version()`](#cloudsync_version)
  - [`cloudsync_siteid()`](#cloudsync_siteid)
  - [`cloudsync_db_version()`](#cloudsync_db_version)
  - [`cloudsync_uuid()`](#cloudsync_uuid)
- [Schema Alteration Functions](#schema-alteration-functions)
  - [`cloudsync_begin_alter()`](#cloudsync_begin_altertable_name)
  - [`cloudsync_commit_alter()`](#cloudsync_commit_altertable_name)
- [Network Functions](#network-functions)
  - [`cloudsync_network_init()`](#cloudsync_network_initconnection_string)
  - [`cloudsync_network_cleanup()`](#cloudsync_network_cleanup)
  - [`cloudsync_network_set_token()`](#cloudsync_network_set_tokentoken)
  - [`cloudsync_network_set_apikey()`](#cloudsync_network_set_apikeyapikey)
  - [`cloudsync_network_has_unsent_changes()`](#cloudsync_network_has_unsent_changes)
  - [`cloudsync_network_send_changes()`](#cloudsync_network_send_changes)
  - [`cloudsync_network_check_changes()`](#cloudsync_network_check_changes)
  - [`cloudsync_network_sync()`](#cloudsync_network_syncwait_ms-max_retries)
  - [`cloudsync_network_reset_sync_version()`](#cloudsync_network_reset_sync_version)
  - [`cloudsync_network_logout()`](#cloudsync_network_logout)

---

## Configuration Functions

### `cloudsync_init(table_name, [crdt_algo], [force])`

**Description:** Initializes a table for `sqlite-sync` synchronization. This function is idempotent and needs to be called only once per table on each site; configurations are stored in the database and automatically loaded with the extension.

Before initialization, `cloudsync_init` performs schema sanity checks to ensure compatibility with CRDT requirements and best practices. These checks include:
- Primary keys should not be auto-incrementing integers; GUIDs (UUIDs, ULIDs) are highly recommended to prevent multi-node collisions.
- All primary key columns must be `NOT NULL`.
- All non-primary key `NOT NULL` columns must have a `DEFAULT` value.

The function supports three overloads:
- `cloudsync_init(table_name)`: Uses the default 'cls' CRDT algorithm.
- `cloudsync_init(table_name, crdt_algo)`: Specifies a CRDT algorithm ('cls', 'dws', 'aws', 'gos').
- `cloudsync_init(table_name, crdt_algo, force)`: Specifies an algorithm and, if `force` is `true` (or `1`), skips the integer primary key check (use with caution, GUIDs are strongly recommended).

**Parameters:**

- `table_name` (TEXT): The name of the table to initialize. Can be set to `'*'` to initialize all tables in the database.
- `crdt_algo` (TEXT, optional): The CRDT algorithm to use. Can be "cls", "dws", "aws", "gos". Defaults to "cls".
- `force` (BOOLEAN, optional): If `true` (or `1`), it skips the check that prevents the use of a single-column INTEGER primary key. Defaults to `false`. It is strongly recommended to use globally unique primary keys instead of integers.

**Returns:** None.

**Example:**

```sql
-- Initialize a single table for synchronization with the Causal-Length Set (CLS) Algorithm (default)
SELECT cloudsync_init('my_table');

-- Initialize a single table for synchronization with a different algorithm Delete-Wins Set (DWS)
SELECT cloudsync_init('my_table', 'dws');

-- Initialize all tables for synchronization with the Causal-Length Set (CLS) Algorithm, and skip the check on single-column INTEGER primary key
SELECT cloudsync_init('*', 'cls', true);
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

### `cloudsync_cleanup(table_name)`

**Description:** Removes the `sqlite-sync` synchronization mechanism from a specified table or all tables. This operation drops the associated `_cloudsync` metadata table and removes triggers from the target table(s). Use this function when synchronization is no longer desired for a table.

**Parameters:**

- `table_name` (TEXT): The name of the table to clean up. Can be set to `'*'` to clean up all synchronized tables.

**Returns:** None.

**Example:**

```sql
-- Clean up a single table
SELECT cloudsync_cleanup('my_table');

-- Clean up all synchronized tables
SELECT cloudsync_cleanup('*');
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

### `cloudsync_network_init(connection_string)`

**Description:** Initializes the `sqlite-sync` network component. This function parses the connection string to configure change checking and upload endpoints, and initializes the cURL library.

**Parameters:**

- `connection_string` (TEXT): The connection string for the remote synchronization server. The format is `sqlitecloud://<host>:<port>/<database>?<options>`.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_network_init('<projectid>.sqlite.cloud/<db>.sqlite');
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

### `cloudsync_network_has_unsent_changes()`

**Description:** Checks if there are any local changes that have not yet been sent to the remote server.

**Parameters:** None.

**Returns:** 1 if there are unsent changes, 0 otherwise.

**Example:**

```sql
SELECT cloudsync_network_has_unsent_changes();
```

---

### `cloudsync_network_send_changes()`

**Description:** Sends all unsent local changes to the remote server.

**Parameters:** None.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_network_send_changes();
```

---

### `cloudsync_network_check_changes()`

**Description:** Checks the remote server for new changes and applies them to the local database.

If a package of new changes is already available for the local site, the server returns it immediately, and the changes are applied. If no package is ready, the server returns an empty response and starts an asynchronous process to prepare a new package. This new package can be retrieved with a subsequent call to this function.

This function is designed to be called periodically to keep the local database in sync.
To force an update and wait for changes (with a timeout), use [`cloudsync_network_sync(wait_ms, max_retries)`].

If the network is misconfigured or the remote server is unreachable, the function returns an error.
On success, it returns `SQLITE_OK`, and the return value indicates how many changes were downloaded and applied.

**Parameters:** None.

**Returns:** The number of changes downloaded. Errors are reported via the SQLite return code.

**Example:**

```sql
SELECT cloudsync_network_check_changes();
```

---

### `cloudsync_network_sync([wait_ms], [max_retries])`

**Description:** Performs a full synchronization cycle. This function has two overloads:

- `cloudsync_network_sync()`: Performs one send operation and one check operation.
- `cloudsync_network_sync(wait_ms, max_retries)`: Performs one send operation and then repeatedly tries to download remote changes until at least one change is downloaded or `max_retries` times has been reached, waiting `wait_ms` between retries.

**Parameters:**

- `wait_ms` (INTEGER, optional): The time to wait in milliseconds between retries. Defaults to 100.
- `max_retries` (INTEGER, optional): The maximum number of times to retry the synchronization. Defaults to 1.

**Returns:** The number of changes downloaded. Errors are reported via the SQLite return code.

**Example:**

```sql
-- Perform a single synchronization cycle
SELECT cloudsync_network_sync();

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

### `cloudsync_network_logout()`

**Description:** Logs out the current user and cleans up all local data from synchronized tables. This function deletes and then re-initializes synchronized tables, useful for switching users or resetting the local database. **Warning:** This function deletes all data from synchronized tables. Use with caution.

**Parameters:** None.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_network_logout();
```
