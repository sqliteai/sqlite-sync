# Schema Migrations Example

This example shows the two supported directions for schema migrations:

- **Client to server**: an authorized SQLite client creates or changes a synchronized table and uploads the generated migration.
- **Server to client**: the backend publishes a migration, and clients download/apply it before receiving data payloads for the new schema.

Normal sync API keys should only download approved migrations. Uploading any migration requires a schema-capable API key, and V2/destructive migrations require a key with destructive schema permission on the backend.

## Client-Originated V1 Migration

The client queues declarative operations, applies them locally, and then uploads the generated pending migration. The user does not need to write JSON:

```sql
SELECT cloudsync_alter_create_table('notes');
SELECT cloudsync_alter_add_column('notes', 'id', 'text', false);
SELECT cloudsync_alter_add_primary_key('notes', 'id');
SELECT cloudsync_alter_add_column('notes', 'title', 'text', false, '');
SELECT cloudsync_alter_add_column('notes', 'body', 'text', false, '');
SELECT cloudsync_alter_add_column('notes', 'updated_at', 'timestamp', false, '1970-01-01T00:00:00Z');
SELECT cloudsync_alter_augment_table('notes', 'CLS', 1);
SELECT cloudsync_alter_set_block_lww('notes', 'body', char(10));
SELECT cloudsync_alter_apply();
SELECT cloudsync_network_migration_upload();
SELECT cloudsync_network_sync();
```

`cloudsync_alter_preview()` can be used before `cloudsync_alter_apply()` to inspect the generated payload. After apply, the payload is saved in `cloudsync_pending_migration`; `cloudsync_network_migration_upload()` uploads the next pending migration and marks it uploaded on success.

The backend should authorize the API key, apply the payload to the cloud database, append it to the schema migration log for the `database_id`, and distribute it to other clients through `schema/check` or `schema/download`.

`client-to-server.sql` contains the same flow as an executable SQLite example. `client-to-server-v1.json` is the manual JSON equivalent for backend tests or custom tooling; application code should normally let `cloudsync_alter_apply()` generate that payload.

## Server-Originated V2 Migration

The backend applies and records a payload such as `server-to-client-v2.json`, then a client can update itself before receiving data:

```sql
SELECT cloudsync_network_migration_check();
SELECT cloudsync_network_sync();
```

If a client receives a data payload first and `cloudsync_payload_apply()` reports a missing schema or unknown schema hash, the network layer runs one schema check and retries the same data payload after the migration is applied.

`server-to-client-v2.json` shows a server-originated V2 migration: it adds `summary`, renames `title` to `heading`, and creates both generic and dialect-specific indexes. It contains both `sqlite` and `postgresql` raw SQL branches because the backend may need to apply the same migration to a PostgreSQL cloud database, while every client applies the SQLite branch locally.

## Dialect Overrides

Portable logical types work for most migrations. When a column needs different SQL on SQLite and PostgreSQL, add overrides after the portable operation:

```sql
SELECT cloudsync_alter_add_column('notes', 'metadata', 'json', false, '{}');
SELECT cloudsync_alter_add_column_sqlite('notes', 'metadata', 'TEXT', false, '''{}''');
SELECT cloudsync_alter_add_column_postgresql('notes', 'metadata', 'JSONB', false, '''{}''::jsonb');
SELECT cloudsync_alter_apply();
```

The last argument of the dialect-specific functions is a SQL default expression for that database, not a plain value.

For migration steps that are not covered by portable commands, use raw SQL in
the same queue:

```sql
SELECT cloudsync_alter_sql('CREATE INDEX notes_updated_at_idx ON notes(updated_at)');
SELECT cloudsync_alter_sqlite('CREATE INDEX notes_body_sqlite_idx ON notes(body)');
SELECT cloudsync_alter_postgresql('CREATE INDEX notes_body_pg_idx ON notes(body)');
SELECT cloudsync_alter_apply();
```

`raw-sql.sql` contains the same raw SQL flow as an executable example.
