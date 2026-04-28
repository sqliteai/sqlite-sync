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
  - [`cloudsync_alter_create_table()`](#cloudsync_alter_create_tabletable_name)
  - [`cloudsync_alter_add_column()`](#cloudsync_alter_add_columntable_name-column_name-logical_type-nullable-default_value)
  - [`cloudsync_alter_add_column_sqlite()`](#cloudsync_alter_add_column_sqlitetable_name-column_name-type_sql-nullable-default_sql)
  - [`cloudsync_alter_add_column_postgresql()`](#cloudsync_alter_add_column_postgresqltable_name-column_name-type_sql-nullable-default_sql)
  - [`cloudsync_alter_sql()`](#cloudsync_alter_sqlsql)
  - [`cloudsync_alter_sqlite()`](#cloudsync_alter_sqlitesql)
  - [`cloudsync_alter_postgresql()`](#cloudsync_alter_postgresqlsql)
  - [`cloudsync_alter_add_primary_key()`](#cloudsync_alter_add_primary_keytable_name-column_name)
  - [`cloudsync_alter_augment_table()`](#cloudsync_alter_augment_tabletable_name-algo-init_flags)
  - [`cloudsync_alter_set_block_lww()`](#cloudsync_alter_set_block_lwwtable_name-column_name-delimiter)
  - [`cloudsync_alter_set_column()`](#cloudsync_alter_set_columntable_name-column_name-key-value)
  - [`cloudsync_alter_set_filter()`](#cloudsync_alter_set_filtertable_name-filter_expr)
  - [`cloudsync_alter_drop_column()`](#cloudsync_alter_drop_columntable_name-column_name)
  - [`cloudsync_alter_rename_column()`](#cloudsync_alter_rename_columntable_name-from_name-to_name)
  - [`cloudsync_alter_preview()`](#cloudsync_alter_preview)
  - [`cloudsync_alter_apply()`](#cloudsync_alter_apply)
  - [`cloudsync_alter_clear()`](#cloudsync_alter_cleartable_name)
  - [`cloudsync_migration_apply()`](#cloudsync_migration_applypayload)
- [Network Functions](#network-functions)
  - [`cloudsync_network_init()`](#cloudsync_network_initmanageddatabaseid)
  - [`cloudsync_network_cleanup()`](#cloudsync_network_cleanup)
  - [`cloudsync_network_set_token()`](#cloudsync_network_set_tokentoken)
  - [`cloudsync_network_set_apikey()`](#cloudsync_network_set_apikeyapikey)
  - [`cloudsync_network_send_changes()`](#cloudsync_network_send_changes)
  - [`cloudsync_network_check_changes()`](#cloudsync_network_check_changes)
  - [`cloudsync_network_sync()`](#cloudsync_network_syncwait_ms-max_retries)
  - [`cloudsync_network_migration_check()`](#cloudsync_network_migration_check)
  - [`cloudsync_network_migration_download()`](#cloudsync_network_migration_download)
  - [`cloudsync_network_migration_upload()`](#cloudsync_network_migration_uploadpayload)
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

Schema migrations should be created with the declarative `cloudsync_alter_*`
functions. Operations are queued in memory for the current connection until
`cloudsync_alter_apply()` applies them locally and stores a generated
payload in `cloudsync_pending_migration` for upload.

The older public SQL functions `cloudsync_begin_alter()` and
`cloudsync_commit_alter()` are no longer exposed. The extension still uses
internal C primitives with those names while replaying migrations on augmented
tables.

### `cloudsync_alter_create_table(table_name)`

**Description:** Queues creation of a table inside a schema migration. Add the
table columns with `cloudsync_alter_add_column()` and mark at least one primary
key column with `cloudsync_alter_add_primary_key()` before applying.

**Parameters:**

- `table_name` (TEXT): The table to create.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_alter_create_table('notes');
SELECT cloudsync_alter_add_column('notes', 'id', 'text', false);
SELECT cloudsync_alter_add_primary_key('notes', 'id');
```

---

### `cloudsync_alter_add_column(table_name, column_name, logical_type, nullable, [default_value])`

**Description:** Queues a portable column definition. `default_value` is
optional. A `NOT NULL` non-primary-key column must have a default value.

**Parameters:**

- `table_name` (TEXT): The table to alter.
- `column_name` (TEXT): The column to add.
- `logical_type` (TEXT): Portable type such as `text`, `integer`, `real`, `numeric`, `blob`, `boolean`, `json`, `timestamp`, or `uuid`.
- `nullable` (BOOLEAN/INTEGER): Whether the column may contain NULL values.
- `default_value` (optional): Plain default value. Serialization is inferred from `logical_type`.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_alter_add_column('notes', 'title', 'text', false, '');
SELECT cloudsync_alter_add_column('notes', 'metadata', 'json', true);
```

---

### `cloudsync_alter_add_column_sqlite(table_name, column_name, type_sql, nullable, [default_sql])`

**Description:** Adds or replaces the SQLite-specific SQL rendering for a
previously queued `cloudsync_alter_add_column()` operation.

**Parameters:**

- `table_name` (TEXT): The table being altered.
- `column_name` (TEXT): The column with the override.
- `type_sql` (TEXT): SQLite type SQL fragment.
- `nullable` (BOOLEAN/INTEGER): Whether the column may contain NULL values.
- `default_sql` (TEXT, optional): SQLite default SQL expression, not a plain value.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_alter_add_column('notes', 'metadata', 'json', false, '{}');
SELECT cloudsync_alter_add_column_sqlite('notes', 'metadata', 'TEXT', false, '''{}''');
```

---

### `cloudsync_alter_add_column_postgresql(table_name, column_name, type_sql, nullable, [default_sql])`

**Description:** Adds or replaces the PostgreSQL-specific SQL rendering for a
previously queued `cloudsync_alter_add_column()` operation.

**Parameters:**

- `table_name` (TEXT): The table being altered.
- `column_name` (TEXT): The column with the override.
- `type_sql` (TEXT): PostgreSQL type SQL fragment.
- `nullable` (BOOLEAN/INTEGER): Whether the column may contain NULL values.
- `default_sql` (TEXT, optional): PostgreSQL default SQL expression, not a plain value.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_alter_add_column('notes', 'metadata', 'json', false, '{}');
SELECT cloudsync_alter_add_column_postgresql('notes', 'metadata', 'JSONB', false, '''{}''::jsonb');
```

---

### `cloudsync_alter_sql(sql)`

**Description:** Queues raw SQL inside the current schema migration. This is an
advanced escape hatch for schema work that cannot be represented by the
portable alter APIs. The SQL runs in order with the other queued operations and
inside the same migration savepoint. Transaction-control statements such as
`BEGIN`, `COMMIT`, `ROLLBACK`, `SAVEPOINT`, and `RELEASE` are rejected. Because
the extension cannot infer the impact of arbitrary SQL, generated raw SQL
migrations use `formatVersion: 2` and require destructive-schema authorization.

**Parameters:**

- `sql` (TEXT): SQL statement to run on every database engine.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_alter_sql('CREATE INDEX notes_updated_at_idx ON notes(updated_at)');
```

---

### `cloudsync_alter_sqlite(sql)`

**Description:** Queues raw SQL that should run only when the migration is
applied to SQLite. PostgreSQL skips this operation.

**Parameters:**

- `sql` (TEXT): SQLite SQL statement.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_alter_sqlite('CREATE INDEX notes_body_sqlite_idx ON notes(body)');
```

---

### `cloudsync_alter_postgresql(sql)`

**Description:** Queues raw SQL that should run only when the migration is
applied to PostgreSQL. SQLite skips this operation.

**Parameters:**

- `sql` (TEXT): PostgreSQL SQL statement.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_alter_postgresql('CREATE INDEX notes_body_pg_idx ON notes(body)');
```

---

### `cloudsync_alter_add_primary_key(table_name, column_name)`

**Description:** Marks a queued column as part of the primary key for a queued
`cloudsync_alter_create_table()` operation.

**Parameters:**

- `table_name` (TEXT): The table being created.
- `column_name` (TEXT): The primary-key column.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_alter_add_primary_key('notes', 'id');
```

---

### `cloudsync_alter_augment_table(table_name, [algo], [init_flags])`

**Description:** Queues CloudSync augmentation for the table. This is the
migration equivalent of `cloudsync_init()`.

**Parameters:**

- `table_name` (TEXT): The table to augment.
- `algo` (TEXT, optional): CRDT algorithm. Defaults to `cls`.
- `init_flags` (INTEGER, optional): Same bitmask used by `cloudsync_init()`.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_alter_augment_table('notes', 'CLS', 1);
```

---

### `cloudsync_alter_set_block_lww(table_name, column_name, [delimiter])`

**Description:** Queues block-level LWW for a text column.

**Parameters:**

- `table_name` (TEXT): The synchronized table.
- `column_name` (TEXT): The text column to configure.
- `delimiter` (TEXT, optional): Block delimiter. For line-level blocks use `char(10)`.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_alter_set_block_lww('notes', 'body', char(10));
```

---

### `cloudsync_alter_set_column(table_name, column_name, key, value)`

**Description:** Queues a CloudSync column setting.

**Parameters:**

- `table_name` (TEXT): The synchronized table.
- `column_name` (TEXT): The target column.
- `key` (TEXT): Setting key.
- `value` (TEXT): Setting value, or NULL.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_alter_set_column('notes', 'body', 'delimiter', char(10));
```

---

### `cloudsync_alter_set_filter(table_name, filter_expr)`

**Description:** Queues a portable row filter. Use
`cloudsync_alter_set_filter_sqlite()` or
`cloudsync_alter_set_filter_postgresql()` when the expression must differ by
database.

**Parameters:**

- `table_name` (TEXT): The synchronized table.
- `filter_expr` (TEXT): SQL predicate for rows that should be tracked.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_alter_set_filter('notes', 'archived = 0');
SELECT cloudsync_alter_set_filter_postgresql('notes', 'archived IS FALSE');
```

---

### `cloudsync_alter_drop_column(table_name, column_name)`

**Description:** Queues a V2 destructive column drop. Backend uploads must
require `schema:destructive` authorization.

**Parameters:**

- `table_name` (TEXT): The table to alter.
- `column_name` (TEXT): The column to drop.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_alter_drop_column('notes', 'legacy_body');
```

---

### `cloudsync_alter_rename_column(table_name, from_name, to_name)`

**Description:** Queues a V2 column rename and updates CloudSync metadata for
the renamed column.

**Parameters:**

- `table_name` (TEXT): The table to alter.
- `from_name` (TEXT): Existing column name.
- `to_name` (TEXT): New column name.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_alter_rename_column('notes', 'text', 'body');
```

---

### `cloudsync_alter_preview()`

**Description:** Returns the generated schema migration JSON without applying it.

**Parameters:** None.

**Returns:** A JSON payload as TEXT.

**Example:**

```sql
SELECT cloudsync_alter_preview();
```

---

### `cloudsync_alter_apply()`

**Description:** Builds the queued migration, applies it locally, stores the
generated payload in `cloudsync_pending_migration`, and clears the queued
operations for the current connection.

**Parameters:** None.

**Returns:** A JSON status object with the generated `migrationId`.

**Example:**

```sql
SELECT cloudsync_alter_apply();
-- '{"status":"applied","migrationId":"...","pendingUpload":true}'
```

---

### `cloudsync_alter_clear([table_name])`

**Description:** Discards queued in-memory schema alteration operations. With no
argument, clears all queued operations for the current connection, including
global raw SQL operations.

**Parameters:**

- `table_name` (TEXT, optional): Table-specific queue to clear.

**Returns:** None.

**Example:**

```sql
SELECT cloudsync_alter_clear('notes');
SELECT cloudsync_alter_clear();
```

---

### `cloudsync_migration_apply(payload)`

**Description:** Applies an explicit schema migration JSON payload. This is
mainly for custom backends, server-downloaded migrations, and tests; application
code should normally use the declarative `cloudsync_alter_*` functions.

**Parameters:**

- `payload` (TEXT): Migration JSON.

**Returns:** A JSON status object.

**Example:**

```sql
SELECT cloudsync_migration_apply(CAST(readfile('client-to-server-v1.json') AS TEXT));
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

### Error handling

The sync functions follow a consistent error-handling contract:

| Error type | Behavior |
|---|---|
| **Endpoint/network errors** (server unreachable, auth failure, bad URL) | SQL error — the function could not execute. |
| **Apply errors** (`cloudsync_payload_apply` failures — unknown schema hash, invalid checksum, decompression error) | Structured JSON — a `receive.error` string field is included in the response. |
| **Server-reported apply job failures** (the server processed the request but its own apply job failed) | Structured JSON — a `send.lastFailure` object is included in the response. |

This means: if you get JSON back, the server was reachable and the network protocol ran. If you get a SQL error, connectivity or configuration is broken.

---

### `cloudsync_network_send_changes()`

**Description:** Sends all unsent local changes to the remote server.

**Parameters:** None.

**Returns:** A JSON string with the send result:

```json
{"send": {"status": "synced|syncing|out-of-sync|error", "localVersion": N, "serverVersion": N, "lastFailure": {...}}}
```

- `send.status`: The current sync state — `"synced"` (all changes confirmed), `"syncing"` (changes sent but not yet confirmed), `"out-of-sync"` (local changes pending or gaps detected), or `"error"`.
- `send.localVersion`: The latest local database version.
- `send.serverVersion`: The latest version confirmed by the server.
- `send.lastFailure` (optional): Present only when the server reports a failed apply job. The object is forwarded verbatim from the server and typically includes `jobId`, `code`, `message`, `retryable`, and `failedAt`. It is emitted regardless of `status` so callers can detect server-side failures during `"syncing"` or even after the state has nominally recovered.

**Example:**

```sql
SELECT cloudsync_network_send_changes();
-- '{"send":{"status":"synced","localVersion":5,"serverVersion":5}}'

-- With a server-reported failure (e.g. unknown schema hash on the server side):
-- '{"send":{"status":"out-of-sync","localVersion":1,"serverVersion":0,"lastFailure":{"jobId":44961,"code":"internal_error","message":"cloudsync operation failed: Cannot apply the received payload because the schema hash is unknown 4288148391734624266.","retryable":true,"failedAt":"2026-04-15T22:21:09.018606Z"}}}'
```

---

### `cloudsync_network_check_changes()`

**Description:** Checks the remote server for new changes and applies them to the local database.

If a package of new changes is already available for the local site, the server returns it immediately, and the changes are applied. If no package is ready, the server returns an empty response and starts an asynchronous process to prepare a new package. This new package can be retrieved with a subsequent call to this function.

This function is designed to be called periodically to keep the local database in sync.
To force an update and wait for changes (with a timeout), use [`cloudsync_network_sync(wait_ms, max_retries)`].

If the network is misconfigured or the remote server is unreachable, the function raises a SQL error. If the received payload cannot be applied locally (for example because of an unknown schema hash), the error is returned as a `receive.error` field in the JSON response.

**Parameters:** None.

**Returns:** A JSON string with the receive result:

```json
{"receive": {"rows": N, "tables": ["table1", "table2"], "error": "..."}}
```

- `receive.rows`: The number of rows received and applied to the local database. `0` when the receive phase failed.
- `receive.tables`: An array of table names that received changes. Empty (`[]`) if no changes were applied or the receive phase failed.
- `receive.error` (optional): Present when `cloudsync_payload_apply` failed. Contains a human-readable error message describing why the received payload could not be applied.

**Example:**

```sql
SELECT cloudsync_network_check_changes();
-- '{"receive":{"rows":3,"tables":["tasks"]}}'

-- With an apply error:
-- '{"receive":{"rows":0,"tables":[],"error":"Cannot apply the received payload because the schema hash is unknown 7218827471400075525."}}'
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
  "send": {"status": "synced|syncing|out-of-sync|error", "localVersion": N, "serverVersion": N, "lastFailure": {...}},
  "receive": {"rows": N, "tables": ["table1", "table2"], "error": "..."}
}
```

- `send.status`: The current sync state — `"synced"`, `"syncing"`, `"out-of-sync"`, or `"error"`.
- `send.localVersion`: The latest local database version.
- `send.serverVersion`: The latest version confirmed by the server.
- `send.lastFailure` (optional): Same semantics as in [`cloudsync_network_send_changes()`](#cloudsync_network_send_changes) — forwarded verbatim from the server whenever a failed apply job is reported, regardless of `status`.
- `receive.rows`: The number of rows received and applied during the check phase. `0` when the receive phase failed.
- `receive.tables`: An array of table names that received changes. Empty (`[]`) if no changes were applied or the receive phase failed.
- `receive.error` (optional): Present when `cloudsync_payload_apply` failed (for example `"Cannot apply the received payload because the schema hash is unknown 7218827471400075525."`). The send result is always preserved so the caller can tell that local changes reached the server even when applying incoming changes failed. The retry loop breaks immediately on apply errors, since failures like schema-hash mismatches do not heal across retries. Endpoint/network errors during the receive phase raise a SQL error instead.

**Example:**

```sql
-- Perform a single synchronization cycle
SELECT cloudsync_network_sync();
-- '{"send":{"status":"synced","localVersion":5,"serverVersion":5},"receive":{"rows":3,"tables":["tasks"]}}'

-- Perform a synchronization cycle with custom retry settings
SELECT cloudsync_network_sync(500, 3);

-- Receive phase failed but send phase completed — the error is surfaced in JSON, not as a SQL error:
-- '{"send":{"status":"synced","localVersion":5,"serverVersion":5},"receive":{"rows":0,"tables":[],"error":"Cannot apply the received payload because the schema hash is unknown 7218827471400075525."}}'
```

---

### `cloudsync_network_migration_check()`

**Description:** Checks the schema migration endpoint and applies any returned
migration before normal row payloads are processed. Responses may contain an
inline `migration`/`payload` object, a migration payload directly, a `url` to
download, or no migration.

**Parameters:** None.

**Returns:** A JSON status object such as `{"status":"applied"}` or
`{"status":"none"}`.

**Example:**

```sql
SELECT cloudsync_network_migration_check();
```

---

### `cloudsync_network_migration_download()`

**Description:** Downloads and applies the next migration from the schema
download endpoint.

**Parameters:** None.

**Returns:** A JSON status object such as `{"status":"applied"}` or
`{"status":"none"}`.

**Example:**

```sql
SELECT cloudsync_network_migration_download();
```

---

### `cloudsync_network_migration_upload([payload])`

**Description:** Uploads a schema migration to the backend. With no arguments it
uploads the next row from `cloudsync_pending_migration`, which is the normal
flow after `cloudsync_alter_apply()`. With one argument it uploads the
explicit JSON payload.

Uploading requires a schema-capable API key on the backend. V2/destructive
migrations should require a stronger destructive-schema permission.

**Parameters:**

- `payload` (TEXT, optional): Explicit migration JSON. Omit it to upload the next pending generated migration.

**Returns:** The backend JSON response.

**Example:**

```sql
SELECT cloudsync_alter_apply();
SELECT cloudsync_network_migration_upload();

-- Custom backend/test flow:
SELECT cloudsync_network_migration_upload(CAST(readfile('client-to-server-v1.json') AS TEXT));
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
