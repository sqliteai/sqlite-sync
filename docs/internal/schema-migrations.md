# CloudSync Schema Migrations

This document describes the implemented schema migration flow for SQLite Sync.
Schema migrations can originate from an authorized SQLite client or from the
cloud database, and the same payload can be applied to SQLite and PostgreSQL.

## Goals

- Allow schema changes to originate either from a SQLite client or from the cloud database.
- Support an empty SQLite client database that creates its synchronized tables during first sync.
- Keep row CRDT payloads binary and focused on data, while using a separate schema-migration protocol.
- Support SQLite/SQLiteCloud and PostgreSQL backends.
- Keep every database coherent when a schema migration or data sync fails.
- Preserve a raw SQL escape hatch for migrations that cannot be expressed portably.

## Core Model

Schema is not a CRDT. Schema migrations are serialized in one ordered log per
`database_id`, and the backend decides whether a migration may be proposed or
applied based on the API key used for the request.

The extension stores applied migrations locally in `cloudsync_migrations`, and
client-originated migrations waiting for upload in `cloudsync_pending_migration`.
Pending alter operations are not stored in a table: they live in the current
CloudSync context until `cloudsync_alter_apply()` or `cloudsync_alter_clear()`.

The local migration applier is atomic. `cloudsync_migration_apply()` opens a
savepoint, validates the JSON payload, applies every operation, updates the
schema hash, records the migration id, and rolls everything back on failure.

## Public SQL Workflow

Applications should build migrations with declarative SQL functions. They do
not need to write JSON.

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
```

`cloudsync_alter_apply()` applies the queued migration locally and stores the
generated payload in `cloudsync_pending_migration`. After that, an authorized
client uploads it with:

```sql
SELECT cloudsync_network_migration_upload();
SELECT cloudsync_network_sync();
```

The zero-argument upload form uploads the next pending local migration and marks
it uploaded only after the backend returns valid JSON. The one-argument form is
still available for custom backends or tests:

```sql
SELECT cloudsync_network_migration_upload(:json_payload);
```

While a local migration is pending upload, `cloudsync_network_send_changes()`
returns an error instead of sending row changes. This prevents data produced
with a new local schema from reaching a server that has not accepted that schema.

## Declarative API

The same SQL API is exposed by the SQLite and PostgreSQL extensions:

- `cloudsync_alter_create_table(table)`
- `cloudsync_alter_add_column(table, column, logical_type, nullable)`
- `cloudsync_alter_add_column(table, column, logical_type, nullable, default_value)`
- `cloudsync_alter_add_column_sqlite(table, column, type_sql, nullable)`
- `cloudsync_alter_add_column_sqlite(table, column, type_sql, nullable, default_sql)`
- `cloudsync_alter_add_column_postgresql(table, column, type_sql, nullable)`
- `cloudsync_alter_add_column_postgresql(table, column, type_sql, nullable, default_sql)`
- `cloudsync_alter_add_primary_key(table, column)`
- `cloudsync_alter_augment_table(table)`
- `cloudsync_alter_augment_table(table, algorithm)`
- `cloudsync_alter_augment_table(table, algorithm, init_flags)`
- `cloudsync_alter_set_block_lww(table, column)`
- `cloudsync_alter_set_block_lww(table, column, delimiter)`
- `cloudsync_alter_set_column(table, column, key, value)`
- `cloudsync_alter_set_filter(table, filter_expr)`
- `cloudsync_alter_set_filter_sqlite(table, filter_expr)`
- `cloudsync_alter_set_filter_postgresql(table, filter_expr)`
- `cloudsync_alter_drop_column(table, column)`
- `cloudsync_alter_rename_column(table, from_name, to_name)`
- `cloudsync_alter_sql(sql)`
- `cloudsync_alter_sqlite(sql)`
- `cloudsync_alter_postgresql(sql)`
- `cloudsync_alter_preview()`
- `cloudsync_alter_apply()`
- `cloudsync_alter_clear()`
- `cloudsync_alter_clear(table)`

`cloudsync_alter_preview()` returns the generated JSON without applying it.
`cloudsync_alter_clear()` discards queued in-memory operations.

The dialect override functions are optional. Use them when the portable logical
type or default is not precise enough:

```sql
SELECT cloudsync_alter_add_column('notes', 'metadata', 'json', false, '{}');
SELECT cloudsync_alter_add_column_sqlite('notes', 'metadata', 'TEXT', false, '''{}''');
SELECT cloudsync_alter_add_column_postgresql('notes', 'metadata', 'JSONB', false, '''{}''::jsonb');
```

The override default is a SQL fragment for that dialect, not a plain value. The
portable `default_value` argument is optional, and its serialization is inferred
from the logical column type.

Raw SQL functions are an escape hatch for migration steps that do not have a
portable command yet. `cloudsync_alter_sql()` runs on every engine, while
`cloudsync_alter_sqlite()` and `cloudsync_alter_postgresql()` are emitted as
dialect-specific raw SQL and skipped by the other engine. They run in queue
order with the structured operations and cannot contain transaction-control
statements.

`cloudsync_begin_alter()` and `cloudsync_commit_alter()` still exist as internal
C primitives used while replaying migrations on already-augmented tables. They
are not public SQL APIs.

## Payload Format

The network payload is JSON. This is intentional even though row sync uses a
binary encoder: schema payloads must be audited, authorized, inspected by a
backend service, and sometimes hand-produced by server tooling. User-facing APIs
generate the JSON automatically, so application code does not need to construct
it directly.

Generated client payloads omit `baseSchemaHash` and `targetSchemaHash` because
raw SQLite and PostgreSQL schema hashes are not necessarily portable across
dialects. Manual payloads may include those fields; when present,
`cloudsync_migration_apply()` enforces them and rolls back on mismatch.

Example generated V1 payload:

```json
{
  "type": "cloudsync.schema.migration",
  "formatVersion": 1,
  "migrationId": "0197097c-8b35-7c11-8ed4-4e59ddfdb928",
  "requiredCapabilities": ["schema:write"],
  "ops": [
    {
      "op": "createTable",
      "table": "notes",
      "columns": [
        {"name": "id", "type": "text", "nullable": false, "primaryKey": true},
        {"name": "body", "type": "text", "nullable": false, "default": {"type": "text", "value": ""}}
      ]
    },
    {"op": "augmentTable", "table": "notes", "algorithm": "CLS", "initFlags": 1},
    {"op": "setBlockLww", "table": "notes", "column": "body", "delimiter": "\n"}
  ]
}
```

## Version 1

Version 1 contains additive and bootstrap operations:

- `createTable`: create a table from logical column definitions.
- `addColumn`: add a nullable column or a `NOT NULL` column with a default value.
- `augmentTable`: call the same internal path as `cloudsync_init()`.
- `setBlockLww`: configure block-level LWW and materialize block metadata.
- `setColumn`: set a CloudSync column setting.
- `setFilter`: set a row filter, with optional dialect-specific filters.

Creating a synchronized table requires both `createTable` and `augmentTable`.
`setBlockLww` must run after the table is augmented and after the target column
exists.

## Version 2

Version 2 is implemented for authorized non-additive changes:

- `dropColumn`
- `renameColumn`
- `rebuildTableSync`
- `rawSql` in V2/destructive payloads

Generated payloads containing `dropColumn` or `renameColumn` use
`formatVersion: 2` and include `schema:destructive` in `requiredCapabilities`.
The backend must enforce this capability from the API key; the payload field is
for audit and policy clarity, not authentication.

The declarative raw SQL functions also emit V2/destructive payloads, even when
the SQL is intended to be additive, because the extension cannot safely infer
the behavioral impact of arbitrary SQL.

`rebuildTableSync` uses `cloudsync_cleanup(..., is_migration = true)` so the
table sync metadata is rebuilt without resetting the database-wide CloudSync
site identity or schema history. The `ddl` and `blockLww` fields are validated
before cleanup/reinit so malformed payloads fail without partially changing the
table.

Version 3 orchestration is deliberately not implemented. Rolling expand/contract
migrations, payload translation across schema epochs, and long-running backfills
belong to a future protocol layer.

## Logical Type Mapping

Portable payloads use logical types and let `migration.c` render backend SQL:

- `text` -> SQLite `TEXT`, PostgreSQL `TEXT`
- `uuid` -> SQLite `TEXT`, PostgreSQL `UUID`
- `integer` -> SQLite `INTEGER`, PostgreSQL `BIGINT`
- `real` -> SQLite `REAL`, PostgreSQL `DOUBLE PRECISION`
- `numeric` -> SQLite `NUMERIC`, PostgreSQL `NUMERIC`
- `blob` -> SQLite `BLOB`, PostgreSQL `BYTEA`
- `boolean` -> SQLite `INTEGER`, PostgreSQL `BOOLEAN`
- `json` -> SQLite `TEXT`, PostgreSQL `JSONB`
- `timestamp` -> SQLite `TEXT`, PostgreSQL `TIMESTAMPTZ`

Use dialect override functions when a migration needs exact SQL types or
database-specific default expressions.

## Backend Protocol

Schema endpoints live beside the existing data endpoints:

- `POST /v2/cloudsync/databases/{databaseId}/{siteId}/schema/check`
- `POST /v2/cloudsync/databases/{databaseId}/{siteId}/schema/upload`
- `GET /v2/cloudsync/databases/{databaseId}/{siteId}/schema/download`

Recommended backend log fields:

- `database_id`
- `schema_version` or `schema_epoch`
- `migration_id`
- `source`: `client` or `server`
- `author_site_id`
- `payload`
- `payload_hash`
- `required_capabilities`
- `authorized_by_key_id`
- `status`: `pending`, `applied`, `rejected`, `failed`
- `created_at`, `applied_at`
- `error`

API key classes:

- `sync`: send/receive data and download already-approved migrations.
- `schema:write`: propose/upload/apply V1 migrations.
- `schema:destructive`: propose/upload/apply V2 destructive migrations.

Normal application clients should use `sync` keys. A schema-capable key is
required for every schema change, including V1 additive changes.

## Sync Flow

Client-originated migration:

1. Application queues operations with `cloudsync_alter_*`.
2. Application calls `cloudsync_alter_apply()`.
3. Extension applies the migration locally and writes `cloudsync_pending_migration`.
4. Application or `cloudsync_network_sync()` uploads the pending migration.
5. Backend authorizes the API key, applies the payload to the cloud database,
   records the migration, and returns success.
6. Client sends row changes after the pending migration is uploaded.

Server-originated migration:

1. Backend applies and records a migration.
2. Client calls `cloudsync_network_sync()` or `cloudsync_network_migration_check()`.
3. The network layer downloads the migration when the local schema is missing or stale.
4. `cloudsync_migration_apply()` applies it locally.
5. Data download/retry continues on the new schema.

Empty client first sync:

1. Empty SQLite client calls `cloudsync_network_init(database_id)`.
2. Sync checks schema before returning from the empty local send phase.
3. Backend returns a schema snapshot or migration chain.
4. The client creates tables, augments them, applies block LWW, then downloads data.

## Failure Semantics

- Migrations are atomic per database connection.
- A malformed JSON payload is rejected before DDL is applied.
- A migration id is idempotent through `cloudsync_migrations`.
- Explicit hash guards are enforced when present.
- Raw SQL runs inside the same savepoint as portable operations.
- Row changes are not uploaded while `cloudsync_pending_migration` contains an unuploaded migration.
- V2 migrations should be blocked by the backend when stale/offline clients may still upload incompatible old-epoch payloads, unless the backend has an explicit rejection or translation policy.

## Tests

SQLite coverage is in `test/unit.c` and the mock network tests in
`test/integration.c`.

PostgreSQL coverage is in `test/postgresql/31_alter_table_sync.sql` and
`test/postgresql/52_schema_migrations.sql`.

Cross-dialect coverage is in `test/schema_migration_cross_dialect.sh` and can
be run with:

```sh
make cross-dialect-migration-test
```

The cross-dialect test covers SQLite-generated migrations applied to PostgreSQL,
PostgreSQL-generated migrations with dialect overrides applied to SQLite, and
generic plus dialect-specific raw SQL in both directions.
