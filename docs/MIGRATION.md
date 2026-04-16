# Schema Migration Architecture

This document describes the design for propagating table schema changes (DDL migrations) across all participants in a sqlite-sync deployment. It covers the problem space, the two-tier strategy, the cross-platform abstraction, initial schema bootstrapping for new clients, and the relevant extension points in the codebase.

---

## Background

sqlite-sync tracks data changes (DML) using a CRDT-based column-level mechanism. Schema changes (DDL) are a fundamentally different class of operation:

| Property | DML (data changes) | DDL (schema changes) |
|---|---|---|
| Ordering | CRDT-friendly, commutative | Sequential, order-dependent |
| Conflicts | Resolved by CRDT algorithms | Generally undefined behavior |
| Idempotency | Versioned, safe to replay | Destructive if replayed |
| Offline safety | Safe to queue and merge | May fail against a stale schema |
| Cross-platform | Identical semantics everywhere | SQLite and PostgreSQL syntax differ |

Today the extension computes a `schema_hash` from the synced tables and embeds it in every payload header. If the receiver does not recognize the hash, the sync is rejected. Schema changes must currently be applied out-of-band on every participant before DML sync can resume.

The migration architecture described here automates that coordination.

---

## Two-Tier Strategy

Not all migrations carry the same risk. The architecture handles them in two tiers:

### Tier 1 — Additive-only, automatic propagation

Operations that are **commutative and non-destructive** — adding a column, creating a table, creating an index — are safe to propagate automatically through the existing sync payload stream. Two peers independently adding different columns to the same table produces a valid merged schema. The payload format is extended with a DDL entry type that is applied before the DML rows in the same payload.

Covered operations: `ADD COLUMN` (with DEFAULT), `CREATE TABLE`, `CREATE INDEX`.

### Tier 2 — Breaking changes, server-authoritative registry

Operations that are **destructive or order-dependent** — dropping a column, renaming a column, changing a column's type — require explicit coordination. These migrations are stored in a `cloudsync_migrations` system table on the server, which is the single source of truth for schema history. Clients fetch and apply pending migrations when a schema hash mismatch is detected on reconnect.

Because the server's migration history is the canonical record of schema evolution, the same mechanism also covers **initial schema bootstrapping**: a brand-new client with an empty database fetches all migrations from version 0 and arrives at the same state as a long-running client. There is no separate "initial schema" script to maintain; the migration log is the schema definition.

The rest of this document focuses on Tier 2.

---

## Platform Abstraction

A sqlite-sync deployment always has SQLite on the client side. The server side can be SQLiteCloud (SQLite syntax) or PostgreSQL. This means the same migration descriptor must generate different DDL on different backends.

The extension already solves this problem for DML: `src/sqlite/` and `src/postgresql/` are separate source trees compiled into separate binaries, both implementing the common `database.h` interface. Each binary already knows which DDL syntax to produce. The migration system follows the same pattern: migrations are represented as **platform-neutral descriptors**, and each backend generates the appropriate DDL from them.

```
Migration Descriptor (binary, stored in cloudsync_migrations)
        │
        ├── SQLite build   →  database_migration_execute() in database_sqlite.c
        │                     generates and executes SQLite DDL
        │
        └── PostgreSQL build → database_migration_execute() in database_postgresql.c
                              generates and executes PostgreSQL DDL
```

---

## Abstract Migration Operations

The descriptor format encodes one of the following operations:

| Operation | Fields | Notes |
|---|---|---|
| `CLOUDSYNC_MIGRATION_ADD_COLUMN` | table, col_name, type, nullable, default_value | Tier 1 and Tier 2 |
| `CLOUDSYNC_MIGRATION_DROP_COLUMN` | table, col_name | Tier 2 only |
| `CLOUDSYNC_MIGRATION_RENAME_COLUMN` | table, old_name, new_name | Tier 2 only |
| `CLOUDSYNC_MIGRATION_SET_DEFAULT` | table, col_name, default_value | Tier 2 only |
| `CLOUDSYNC_MIGRATION_CREATE_TABLE` | table, columns[] | Tier 1 and Tier 2 |
| `CLOUDSYNC_MIGRATION_DROP_TABLE` | table | Tier 2 only |
| `CLOUDSYNC_MIGRATION_RENAME_TABLE` | old_name, new_name | Tier 2 only |
| `CLOUDSYNC_MIGRATION_CREATE_INDEX` | index_name, table, columns[], unique | Tier 1 and Tier 2 |
| `CLOUDSYNC_MIGRATION_DROP_INDEX` | index_name | Tier 2 only |
| `CLOUDSYNC_MIGRATION_INIT_SYNC` | table, algo, filter | Enables sync tracking on a table |
| `CLOUDSYNC_MIGRATION_CUSTOM` | sql_sqlite, sql_postgresql | Escape hatch for platform-specific SQL |

**`CLOUDSYNC_MIGRATION_INIT_SYNC`** is the migration-system equivalent of calling `cloudsync_init()` manually. It records the CRDT algorithm (`CLS`, `AWS`, `DWS`, or `GOS`) and an optional row filter expression for the table, then creates the `{table}_cloudsync` shadow table and the INSERT/UPDATE/DELETE tracking triggers. It is always paired with a preceding `CLOUDSYNC_MIGRATION_CREATE_TABLE` for the same table. Tables that are intentionally local-only (lookup tables, caches) do not get a `CLOUDSYNC_MIGRATION_INIT_SYNC` entry, which is what makes the separation from `CLOUDSYNC_MIGRATION_CREATE_TABLE` valuable: creation and sync enrollment are two distinct, explicit decisions in the migration log.

**`CLOUDSYNC_MIGRATION_CUSTOM`** carries two raw SQL strings and is the explicit fallback for migrations that cannot be expressed by the abstract operations above (e.g. PostgreSQL-specific constraint syntax, column type casts that differ between platforms). The correct string is selected at execution time by the backend.

---

## Abstract Type Mapping

Column types in migration descriptors use an abstract type system (`CSTYPE_*`) that maps to the correct DDL keyword on each platform. The mapping extends the existing `DBTYPE` enum already used throughout the codebase:

| Abstract type | SQLite DDL | PostgreSQL DDL |
|---|---|---|
| `CSTYPE_INTEGER` | `INTEGER` | `INTEGER` |
| `CSTYPE_REAL` | `REAL` | `DOUBLE PRECISION` |
| `CSTYPE_TEXT` | `TEXT` | `TEXT` |
| `CSTYPE_BLOB` | `BLOB` | `BYTEA` |
| `CSTYPE_BOOLEAN` | `INTEGER` | `BOOLEAN` |
| `CSTYPE_UUID` | `TEXT` | `UUID` |
| `CSTYPE_TIMESTAMP` | `INTEGER` | `BIGINT` |
| `CSTYPE_JSON` | `TEXT` | `JSONB` |

The PostgreSQL backend already performs type mapping in `src/postgresql/pgvalue.c:65–87` (OID → DBTYPE for incoming values). The migration type map is the DDL-generation direction of the same relationship.

---

## The `cloudsync_migrations` Table

Each participant maintains a local system table that records which migrations have been applied:

```sql
CREATE TABLE cloudsync_migrations (
    version    INTEGER PRIMARY KEY,  -- monotonic, server-assigned
    descriptor BLOB    NOT NULL,     -- binary migration descriptor
    applied_at INTEGER,              -- NULL until applied locally
    checksum   INTEGER NOT NULL      -- integrity check of descriptor
);
```

`version` is assigned by the server and is the total order for all migrations. Clients never assign version numbers. The server is the only writer of `version`; clients only update `applied_at` locally.

**Lazy creation.** `cloudsync_migrations` is created on demand by the first call to `cloudsync_migration_register()` or `cloudsync_migration_apply_pending()`. It is never created by `cloudsync_init()`. This matters for PostgreSQL deployments where the application role may have `CREATE` on its own schema but not on `public`: calling `cloudsync_init()` on a table that will never use the migration API should not require any additional privilege.

---

## Sync Flows

There are two entry points into the migration catch-up sequence: a cold start (new client, empty database) and a schema mismatch (existing client, stale schema). Both converge on the same fetch-and-apply loop; only the trigger and starting version differ.

### Cold Start — New Client

A new client has no `cloudsync_settings` row and therefore no `schema_hash`. Before attempting any DML sync it proactively fetches the full migration history:

```
1. Client detects schema_hash = NULL (no cloudsync_settings row)
2. Client queries server:
       SELECT version, descriptor, checksum
       FROM cloudsync_migrations
       ORDER BY version ASC
3. For each row returned:
   a. Verify descriptor checksum
   b. If descriptor.op == CLOUDSYNC_MIGRATION_CREATE_TABLE or other DDL:
        Execute DDL directly (no begin_alter needed — table does not exist yet)
   c. If descriptor.op == CLOUDSYNC_MIGRATION_INIT_SYNC:
        Call cloudsync_init(table, algo, filter)
        → creates {table}_cloudsync, installs triggers, records schema_hash
   d. Otherwise:
        Call cloudsync_begin_alter(affected_table)
        Call database_migration_execute(descriptor)
        Call cloudsync_commit_alter(affected_table)
   e. UPDATE cloudsync_migrations SET applied_at = <now> WHERE version = ?
4. DML sync begins normally
```

### Schema Mismatch — Existing Client

When an existing client reconnects after missing one or more migrations, the server rejects its DML payload:

```
1. Client sends DML payload
2. Server rejects: "unknown schema hash <hash>"
3. Client queries server:
       SELECT version, descriptor, checksum
       FROM cloudsync_migrations
       WHERE version > (SELECT COALESCE(MAX(version), 0)
                        FROM cloudsync_migrations
                        WHERE applied_at IS NOT NULL)
       ORDER BY version ASC
4. For each row returned:
   a. Verify descriptor checksum
   b. If descriptor.op == CLOUDSYNC_MIGRATION_INIT_SYNC:
        Call cloudsync_init(table, algo, filter)
        → enrolls a newly created table that was added after initial setup
   c. Otherwise:
        Call cloudsync_begin_alter(affected_table)
        Call database_migration_execute(descriptor)
        Call cloudsync_commit_alter(affected_table)
        → compacts metadata, rebuilds triggers, updates schema_hash
   d. UPDATE cloudsync_migrations SET applied_at = <now> WHERE version = ?
5. Client retries DML payload (schema_hash now matches)
```

In both flows, if any migration fails the sequence halts and the error is surfaced to the application. Partial migration is not committed. The client remains on the previous schema version until the failure is resolved.

---

## Migration Squashing

If a project accumulates many migrations, a new client must replay all of them from version 0. For long-lived projects this becomes wasteful. The standard mitigation is a **schema snapshot**: at a chosen checkpoint version N, the server replaces all migrations below N with one `CLOUDSYNC_MIGRATION_CREATE_TABLE` + `CLOUDSYNC_MIGRATION_INIT_SYNC` pair per tracked table, each reflecting the schema at that point. Incremental migrations from N+1 onward are left intact.

Clients that are already past version N skip the snapshot entries (their `applied_at` is already set). Clients starting fresh apply only the compacted snapshots, then the live migrations above N, and arrive at the same result as a client that went through every individual migration.

Squashing is a server-side administrative operation. The `cloudsync_migrations` table schema supports it without changes: the snapshot rows simply replace the old rows at the same version numbers. Each snapshot row uses `CLOUDSYNC_MIGRATION_CREATE_TABLE` for the current column set, optionally followed by `CLOUDSYNC_MIGRATION_INIT_SYNC` for tracked tables or seed-data `CLOUDSYNC_MIGRATION_CUSTOM` rows for reference tables.

---

## Integration with `cloudsync_begin_alter` / `cloudsync_commit_alter`

The existing alter lifecycle already does the right work for local DDL changes. It is reused unchanged for migration execution:

- `cloudsync_begin_alter(table)` — drops tracking triggers, marks table as mid-alter
- *(migration DDL executes here)*
- `cloudsync_commit_alter(table)` — diffs `PRAGMA table_info` before/after, compacts the `{table}_cloudsync` metadata table (removes orphaned column entries for dropped columns, garbage-collects orphaned PKs if the schema changed incompatibly), rebuilds triggers for the new schema, updates the stored schema hash

No changes to the alter lifecycle are required. The migration executor wraps each descriptor in a `begin_alter` / `commit_alter` pair, exactly as a developer would for a manual schema change today.

---

## Implementation Extension Points

Two functions are added to the `database.h` abstraction interface:

```c
// Generate platform-appropriate DDL SQL from a descriptor (for inspection/logging).
// Returns a heap-allocated string; caller must free with dbmem_free().
// Returns NULL for operations that generate no DDL (INIT_SYNC, SET_DEFAULT on SQLite).
char *database_migration_sql(const cloudsync_migration_descriptor *desc);

// Execute a migration descriptor against the open database connection.
// Must be called between cloudsync_begin_alter() and cloudsync_commit_alter().
int database_migration_execute(cloudsync_context *ctx, cloudsync_migration_descriptor *desc);
```

Each backend provides its own implementation:

- `src/sqlite/migration_sqlite.c` — generates and executes SQLite DDL
- `src/postgresql/migration_postgresql.c` — generates and executes PostgreSQL DDL

For `CLOUDSYNC_MIGRATION_CUSTOM`, each backend simply executes its corresponding SQL string (`sql_sqlite` or `sql_postgresql`) directly.

---

## Authoring Migrations

For the common case, the developer creates a descriptor using the C API (or a helper function exposed as a SQLite scalar function) and registers it on the server. The extension handles the rest.

### Initial table setup

A new synced table requires two consecutive migrations: one to create the table and one to enroll it in sync. The separation is intentional — local-only tables are created with `CLOUDSYNC_MIGRATION_CREATE_TABLE` alone, without a following `CLOUDSYNC_MIGRATION_INIT_SYNC`.

```c
// Migration 1 — create the table (platform-neutral)
cloudsync_migration_descriptor *m1 = cloudsync_migration_create(CLOUDSYNC_MIGRATION_CREATE_TABLE);
cloudsync_migration_set_table(m1, "tasks");
cloudsync_migration_add_column(m1, "id",         CSTYPE_UUID,      false, NULL);  // PK
cloudsync_migration_add_column(m1, "title",      CSTYPE_TEXT,      false, "''");
cloudsync_migration_add_column(m1, "done",       CSTYPE_BOOLEAN,   false, "0");
cloudsync_migration_add_column(m1, "created_at", CSTYPE_TIMESTAMP, false, "0");
cloudsync_migration_set_primary_key(m1, "id");
cloudsync_migration_register(ctx, 1, m1);
cloudsync_migration_free(m1);

// Migration 2 — enroll in sync with CLS algorithm
cloudsync_migration_descriptor *m2 = cloudsync_migration_create(CLOUDSYNC_MIGRATION_INIT_SYNC);
cloudsync_migration_set_table(m2, "tasks");
cloudsync_migration_set_algo(m2, CSALGO_CLS);
cloudsync_migration_set_filter(m2, NULL);        // NULL = no row filter
cloudsync_migration_register(ctx, 2, m2);
cloudsync_migration_free(m2);
```

### Adding a column to an existing table

```c
cloudsync_migration_descriptor *m = cloudsync_migration_create(CLOUDSYNC_MIGRATION_ADD_COLUMN);
cloudsync_migration_set_table(m, "tasks");
cloudsync_migration_set_column(m, "priority");
cloudsync_migration_set_type(m, CSTYPE_INTEGER);
cloudsync_migration_set_nullable(m, false);
cloudsync_migration_set_default(m, "0");
cloudsync_migration_register(ctx, 3, m);
cloudsync_migration_free(m);
```

### Platform-specific SQL via `CLOUDSYNC_MIGRATION_CUSTOM`

For cases that cannot be expressed by the abstract operations — for example, adding a column with a PostgreSQL `CHECK` constraint that has no direct SQLite equivalent:

```c
cloudsync_migration_descriptor *m = cloudsync_migration_create(CLOUDSYNC_MIGRATION_CUSTOM);
cloudsync_migration_set_sql_sqlite(m,
    "ALTER TABLE orders ADD COLUMN status TEXT NOT NULL DEFAULT 'pending'");
cloudsync_migration_set_sql_postgresql(m,
    "ALTER TABLE orders ADD COLUMN status TEXT NOT NULL DEFAULT 'pending' "
    "CHECK (status IN ('pending', 'processing', 'shipped', 'cancelled'))");
cloudsync_migration_register(ctx, 4, m);
cloudsync_migration_free(m);
```

### Local-only and reference tables

A table created with `CLOUDSYNC_MIGRATION_CREATE_TABLE` and no following `CLOUDSYNC_MIGRATION_INIT_SYNC` is distributed to every client as part of the normal bootstrap or catch-up flow, but its data is never tracked or synced. This covers two patterns:

**Local state tables** hold device-specific data (UI preferences, draft state, offline queue metadata). Each client manages its own rows independently.

**Reference tables** hold static lookup data that is the same on every client. The initial rows are distributed by a companion `CLOUDSYNC_MIGRATION_CUSTOM` migration immediately after the `CLOUDSYNC_MIGRATION_CREATE_TABLE`. Because the seed SQL is the same on both platforms it is written identically into both SQL fields:

```c
// Migration 5 — create a local-only reference table
cloudsync_migration_descriptor *m1 = cloudsync_migration_create(CLOUDSYNC_MIGRATION_CREATE_TABLE);
cloudsync_migration_set_table(m1, "currencies");
cloudsync_migration_add_column(m1, "code", CSTYPE_TEXT, false, NULL);  // PK
cloudsync_migration_add_column(m1, "name", CSTYPE_TEXT, false, "''");
cloudsync_migration_set_primary_key(m1, "code");
cloudsync_migration_register(ctx, 5, m1);
cloudsync_migration_free(m1);
// No CLOUDSYNC_MIGRATION_INIT_SYNC — data is local, not tracked

// Migration 6 — seed the reference rows (same SQL on both platforms)
const char *seed =
    "INSERT INTO currencies (code, name) VALUES "
    "('USD','US Dollar'),('EUR','Euro'),('GBP','Pound Sterling')";
cloudsync_migration_descriptor *m2 = cloudsync_migration_create(CLOUDSYNC_MIGRATION_CUSTOM);
cloudsync_migration_set_sql_sqlite(m2, seed);
cloudsync_migration_set_sql_postgresql(m2, seed);
cloudsync_migration_register(ctx, 6, m2);
cloudsync_migration_free(m2);
```

When the seed SQL differs between platforms (e.g. a PostgreSQL-specific `ON CONFLICT` clause), use separate strings as with any other `CLOUDSYNC_MIGRATION_CUSTOM` migration. Seed migrations are applied exactly once per client and recorded in `cloudsync_migrations.applied_at` like any other migration, so they are never re-executed on reconnect.

---

## Getting Started — User's Guide

This section walks through the migration API from a developer's perspective: how to wire it into an application, define an initial schema, evolve it over time, and apply pending migrations at startup or on reconnect.

### Overview of the flow

```
App startup
  │
  ├── 1. Open SQLite database
  ├── 2. Create a cloudsync_context
  ├── 3. Register migrations (if this is the server / authoritative node)
  │       cloudsync_migration_register(ctx, version, descriptor)
  ├── 4. Apply pending migrations
  │       cloudsync_migration_apply_pending(ctx)
  └── 5. Proceed with normal DML sync
```

`cloudsync_migration_register()` stores a descriptor in the local `cloudsync_migrations` table with `applied_at = NULL`. `cloudsync_migration_apply_pending()` walks those rows in version order, executes each one, and marks it applied. The two steps are separate so that a server node can populate the ledger at deploy time and client nodes can pick up and apply the same entries when they connect.

### Minimal C example

The example below creates a synced `tasks` table, applies the initial migration, then adds a column in a follow-up migration.

```c
#include <stdio.h>
#include "sqlite3.h"
#include "cloudsync.h"
#include "migration.h"

static void check(int rc, const char *label) {
    if (rc != DBRES_OK) {
        fprintf(stderr, "FAILED at %s (rc=%d)\n", label, rc);
        exit(1);
    }
}

int main(void) {
    // 1. Open the database and load the cloudsync extension.
    sqlite3 *db;
    sqlite3_open(":memory:", &db);
    sqlite3_enable_load_extension(db, 1);
    sqlite3_load_extension(db, "./dist/cloudsync", NULL, NULL);

    // 2. Create a cloudsync context.
    cloudsync_context *ctx = cloudsync_context_create(db);

    // ----------------------------------------------------------------
    // Migration 1 — create the tasks table
    // ----------------------------------------------------------------
    cloudsync_migration_descriptor *m1 =
        cloudsync_migration_create(CLOUDSYNC_MIGRATION_CREATE_TABLE);
    cloudsync_migration_set_table(m1, "tasks");
    // id TEXT PRIMARY KEY NOT NULL
    cloudsync_migration_add_column(m1, "id",    CSTYPE_UUID,    false, NULL);
    cloudsync_migration_set_primary_key(m1, "id");
    // title TEXT NOT NULL DEFAULT ''
    cloudsync_migration_add_column(m1, "title", CSTYPE_TEXT,    false, "''");
    // done INTEGER NOT NULL DEFAULT 0
    cloudsync_migration_add_column(m1, "done",  CSTYPE_BOOLEAN, false, "0");
    check(cloudsync_migration_register(ctx, 1, m1), "register m1");
    cloudsync_migration_free(m1);

    // ----------------------------------------------------------------
    // Migration 2 — enroll tasks in sync (CausalLengthSet algorithm)
    // ----------------------------------------------------------------
    cloudsync_migration_descriptor *m2 =
        cloudsync_migration_create(CLOUDSYNC_MIGRATION_INIT_SYNC);
    cloudsync_migration_set_table(m2, "tasks");
    cloudsync_migration_set_algo(m2, CSALGO_CLS);
    check(cloudsync_migration_register(ctx, 2, m2), "register m2");
    cloudsync_migration_free(m2);

    // ----------------------------------------------------------------
    // Apply everything that is pending (migrations 1 and 2 above).
    // On a client that is bootstrapping from the server's ledger, the
    // same call applies whatever rows it just fetched.
    // ----------------------------------------------------------------
    check(cloudsync_migration_apply_pending(ctx), "apply pending");

    printf("Schema v1 ready — tasks table created and enrolled in sync.\n");

    // ----------------------------------------------------------------
    // Migration 3 — add a priority column (schema evolution)
    // ----------------------------------------------------------------
    cloudsync_migration_descriptor *m3 =
        cloudsync_migration_create(CLOUDSYNC_MIGRATION_ADD_COLUMN);
    cloudsync_migration_set_table(m3, "tasks");
    cloudsync_migration_set_column(m3, "priority");
    cloudsync_migration_set_type(m3, CSTYPE_INTEGER);
    cloudsync_migration_set_nullable(m3, false);
    cloudsync_migration_set_default(m3, "0");
    check(cloudsync_migration_register(ctx, 3, m3), "register m3");
    cloudsync_migration_free(m3);

    check(cloudsync_migration_apply_pending(ctx), "apply pending v2");

    printf("Schema v2 ready — priority column added.\n");

    // ----------------------------------------------------------------
    // Inspect the generated SQL for a descriptor (optional, for logging)
    // ----------------------------------------------------------------
    cloudsync_migration_descriptor *inspect =
        cloudsync_migration_create(CLOUDSYNC_MIGRATION_DROP_COLUMN);
    cloudsync_migration_set_table(inspect, "tasks");
    cloudsync_migration_set_column(inspect, "done");
    char *sql = database_migration_sql(inspect);
    if (sql) {
        printf("Would execute: %s\n", sql);
        dbmem_free(sql);
    }
    cloudsync_migration_free(inspect);

    cloudsync_context_free(ctx);
    sqlite3_close(db);
    return 0;
}
```

### Registration and application patterns

**Server node** — registers all migrations at startup (or during deployment). It calls `cloudsync_migration_register()` for each version in order. If a version is already in the ledger, the call is a no-op for applied migrations and updates the descriptor for pending ones (see re-registration semantics below).

**Client node bootstrapping from scratch** — fetches the full migration ledger from the server, stores each row via `cloudsync_migration_register()`, then calls `cloudsync_migration_apply_pending()` once to arrive at the current schema.

**Client node catching up after reconnect** — fetches only rows with `version > MAX(applied_at IS NOT NULL version)` from the server, registers them, then calls `cloudsync_migration_apply_pending()`.

In all three cases the client-side code is the same two calls. The difference is only in which rows end up in the local `cloudsync_migrations` table before `apply_pending` is called.

### Re-registration semantics

Calling `cloudsync_migration_register()` for a version that already exists in the ledger follows a simple rule:

- **Pending** (`applied_at IS NULL`) — the existing row is overwritten with the new descriptor. This allows correcting a mistake before the migration has been applied.
- **Applied** (`applied_at IS NOT NULL`) — the existing row is left unchanged. Calling `register()` again for an already-applied version is a safe no-op.

### Schema-qualified table names

Descriptors may use schema-qualified table names such as `"sales.orders"` for PostgreSQL deployments. The SQLite backend automatically strips the schema prefix before generating DDL, since SQLite has no user-defined schemas. The bare name is also used for CloudSync metadata lookups on both platforms.

```c
cloudsync_migration_descriptor *m =
    cloudsync_migration_create(CLOUDSYNC_MIGRATION_ADD_COLUMN);
cloudsync_migration_set_table(m, "sales.orders");  // "sales." stripped on SQLite
cloudsync_migration_set_column(m, "tax_rate");
cloudsync_migration_set_type(m, CSTYPE_REAL);
cloudsync_migration_set_nullable(m, false);
cloudsync_migration_set_default(m, "0.0");
cloudsync_migration_register(ctx, 7, m);
cloudsync_migration_free(m);
```

The CloudSync cleanup for a `DROP_TABLE` or `RENAME_TABLE` only fires when the schema qualifier in the descriptor matches the schema context the table was enrolled with. A `DROP_TABLE("temp.orders")` descriptor will execute the physical DDL but will not remove sync metadata for a `"main.orders"` table that happens to share the bare name.

### Bootstrapping a local database from the cloud

A common starting point is a device that has no local database yet and needs to pull everything — schema and data — from a cloud database that is already populated.

The process has two phases:

1. **Schema phase** — create local tables and enroll them in sync. The simplest approach is to write the schema once in the application (as plain DDL or via migration descriptors) and call `cloudsync_init()` on each table. When migrations are managed on the server, a client can instead fetch the server's `cloudsync_migrations` ledger, register those rows locally, and call `cloudsync_migration_apply_pending()` to let the extension recreate the schema automatically.

2. **Data phase** — call `cloudsync_network_sync()`. On a brand-new database with no local changes, this is a pure receive: all existing rows are pulled from the cloud and merged into the local tables.

The C example below uses the simpler "app-defined schema" path (no server ledger involved) and is the pattern most mobile and desktop applications use for a first launch:

```c
#include <stdio.h>
#include <stdlib.h>
#include "sqlite3.h"

#define EXT_PATH       "./dist/cloudsync"
#define DB_PATH        "myapp.db"
#define MANAGED_DB_ID  "<your-managed-database-id>"
#define API_KEY        "<your-api-key>"

/* Execute SQL and abort on failure. */
static void exec(sqlite3 *db, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "SQL error (%s): %s\n", sql, err);
        sqlite3_free(err);
        sqlite3_close(db);
        exit(1);
    }
}

int main(void) {
    sqlite3 *db = NULL;

    /* 1. Open (or create) the local database file. */
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open %s: %s\n", DB_PATH, sqlite3_errmsg(db));
        return 1;
    }

    /* 2. Load the cloudsync extension. */
    sqlite3_enable_load_extension(db, 1);
    exec(db, "SELECT load_extension('" EXT_PATH "')");

    /* 3. Create local tables.
     *    IF NOT EXISTS makes this safe to call on every app startup — it is
     *    a no-op when the database already exists on disk.
     *    Rules: PKs must be TEXT (not INTEGER autoincrement), and every NOT
     *    NULL non-PK column must carry a DEFAULT so that cloudsync can
     *    populate it when merging rows that arrived before the column existed.
     */
    exec(db,
        "CREATE TABLE IF NOT EXISTS tasks ("
        "  id       TEXT PRIMARY KEY NOT NULL,"
        "  title    TEXT NOT NULL DEFAULT '',"
        "  done     INTEGER NOT NULL DEFAULT 0,"
        "  priority INTEGER NOT NULL DEFAULT 0"
        ");"
    );
    exec(db,
        "CREATE TABLE IF NOT EXISTS tags ("
        "  id      TEXT PRIMARY KEY NOT NULL,"
        "  task_id TEXT NOT NULL DEFAULT '',"
        "  label   TEXT NOT NULL DEFAULT ''"
        ");"
    );

    /* 4. Enroll tables in sync.
     *    Also safe to call every startup — idempotent when already enrolled.
     */
    exec(db, "SELECT cloudsync_init('tasks')");
    exec(db, "SELECT cloudsync_init('tags')");

    /* 5. Connect to the cloud database.
     *    Pass the managed-database ID assigned by the CloudSync service.
     *    Use cloudsync_network_init_custom('<address>', '<id>') to target a
     *    non-default server address.
     */
    exec(db, "SELECT cloudsync_network_init('" MANAGED_DB_ID "')");

    /* 6. Authenticate.
     *    Server-to-server or backend services: use an API key.
     *    Mobile / desktop apps acting on behalf of a user: use a JWT token
     *    obtained from your auth provider instead:
     *      exec(db, "SELECT cloudsync_network_set_token('<jwt>')");
     */
    exec(db, "SELECT cloudsync_network_set_apikey('" API_KEY "')");

    /* 7. Sync — send local changes then receive remote changes.
     *    On a freshly created database there are no local changes, so this
     *    is effectively a full download of all cloud rows into the local DB.
     *    The function returns a JSON summary; use sqlite3_exec with a
     *    callback, or sqlite3_prepare_v2, to read the result if needed.
     */
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db, "SELECT cloudsync_network_sync()", -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *summary = (const char *)sqlite3_column_text(stmt, 0);
        /* summary is JSON, e.g. {"send":{"rows":0},"receive":{"rows":42}} */
        printf("Sync complete: %s\n", summary);
    }
    sqlite3_finalize(stmt);

    /* 8. Verify the data arrived. */
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM tasks", -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW)
        printf("Local tasks after sync: %d\n", sqlite3_column_int(stmt, 0));
    sqlite3_finalize(stmt);

    /* 9. Release the cloudsync context before closing the database. */
    exec(db, "SELECT cloudsync_terminate()");
    sqlite3_close(db);
    return 0;
}
```

**Using the migration ledger instead of hand-written DDL.** If the server maintains a `cloudsync_migrations` table (populated via `cloudsync_migration_register()` as shown earlier), a client can derive its schema from that ledger rather than duplicating DDL in every platform. The bootstrap sequence changes to:

```
1. Query server's cloudsync_migrations table (via your cloud SDK / HTTP API):
       SELECT version, descriptor, checksum FROM cloudsync_migrations
       ORDER BY version ASC

2. For each row, register it locally:
       cloudsync_migration_register(ctx, row.version, deserialized_descriptor)

3. Apply all pending migrations (creates tables + enrolls in sync):
       cloudsync_migration_apply_pending(ctx)

4. Network init + sync (same as steps 5–9 above).
```

Steps 1–3 replace the `CREATE TABLE` + `cloudsync_init` calls. The rest of the startup sequence is identical.

---

## Constraints and Limitations

**Column renames and SQLite introspection.** SQLite's `PRAGMA table_info()` does not distinguish a rename from a drop-then-add. `CLOUDSYNC_MIGRATION_RENAME_COLUMN` must be specified explicitly by the developer; it cannot be inferred automatically from a `begin_alter` / `commit_alter` diff.

**Offline clients during a breaking migration.** A client that has been offline cannot apply pending migrations until it reconnects to the server. Writes made while offline against the old schema are queued normally. On reconnect, migrations are applied first, then the queued DML payload is sent. If the queued DML references a column that was dropped in a migration, the merge will produce NULL values for those entries, consistent with the existing behavior for unknown columns.

**Server authority on version ordering.** The `version` field is assigned by the server and defines the total order of all migrations. Clients never assign version numbers. There is no mechanism for peer-to-peer migration propagation; all migrations flow through the server.

**Migration atomicity.** Each migration descriptor is applied atomically within a `begin_alter` / `commit_alter` pair. A migration that fails is not retried automatically. The client surfaces the error and remains on the previous schema version.

**Type mapping coverage.** `CSTYPE_*` covers the types used by the existing CRDT payload format (`DBTYPE` + common PostgreSQL-specific types). Highly platform-specific types (PostgreSQL arrays, custom domains, geometry types) require `CLOUDSYNC_MIGRATION_CUSTOM`. The type mapping table is extended as new abstract types are needed.

**`CLOUDSYNC_MIGRATION_SET_DEFAULT` is not supported on SQLite.** SQLite's `ALTER TABLE` has no `SET DEFAULT` clause. Applying a `SET_DEFAULT` migration against an SQLite context returns an error. Use `CLOUDSYNC_MIGRATION_CUSTOM` with separate `sql_sqlite` / `sql_postgresql` strings to handle this case (a SQLite workaround typically involves recreating the table via a `CLOUDSYNC_MIGRATION_CUSTOM` migration or accepting the limitation if the default is only needed server-side).

**Index and constraint migrations.** `CLOUDSYNC_MIGRATION_CREATE_INDEX` and `CLOUDSYNC_MIGRATION_DROP_INDEX` operate on simple B-tree indexes. Partial indexes, expression indexes, and constraint-backed indexes (e.g. `UNIQUE`) may require `CLOUDSYNC_MIGRATION_CUSTOM` for full platform compatibility.

**Local-only tables have no `CLOUDSYNC_MIGRATION_INIT_SYNC` entry.** Tables created via `CLOUDSYNC_MIGRATION_CREATE_TABLE` without a following `CLOUDSYNC_MIGRATION_INIT_SYNC` are treated as local-only and are never enrolled in sync. This is intentional and correct for lookup tables, caches, and other local state. Enrolling an existing local-only table in sync at a later migration version is supported: add a `CLOUDSYNC_MIGRATION_INIT_SYNC` entry at the desired version and it will be applied on the next catch-up.

**`CLOUDSYNC_MIGRATION_INIT_SYNC` is not idempotent by default.** Calling `cloudsync_init()` on a table that is already enrolled is a no-op in the current implementation, so replaying a `CLOUDSYNC_MIGRATION_INIT_SYNC` migration is safe. However, changing the algorithm or filter of an already-enrolled table requires a dedicated operation (not yet defined) rather than a second `CLOUDSYNC_MIGRATION_INIT_SYNC`.

**Seed data is not re-applied on schema squash.** `CLOUDSYNC_MIGRATION_CUSTOM` rows that seed reference data are applied once and recorded in `cloudsync_migrations.applied_at`. When the migration history is squashed, those rows are dropped from the snapshot because the data already exists on all current clients. New clients bootstrapping from a squashed snapshot receive the seed data via the `CLOUDSYNC_MIGRATION_CREATE_TABLE` snapshot row's companion seed migration, which must be preserved above the squash checkpoint.
