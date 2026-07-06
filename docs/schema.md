# Database Schema Recommendations

When designing your database schema for SQLite Sync, follow these guidelines to ensure correct CRDT behavior and conflict resolution.

## Schema Consistency Across Devices

All databases participating in the same sync (every client and the cloud database) **must have the same set of synced tables with identical structure**:

- The same tables must be created on every participant.
- Each table must be initialized with `cloudsync_init()` on every participant.
- Column names, types, and constraints must match across participants.

sqlite-sync computes a **schema hash** from the synced tables and includes it in every sync payload. The server rejects payloads whose schema hash it does not recognize, failing with an error like:

```
cloudsync operation failed: Cannot apply the received payload because the schema hash is unknown <hash>
```

If you need different clients to see different subsets of data (for example, per-tenant or per-workspace isolation), do **not** give each client a different table. Instead, use a single shared schema and scope the data with a column such as `tenant_id` or `workspace_id`, then enforce isolation server-side with [RLS Overview](./rls-overview.md).

## Primary Key Requirements

- **Use globally unique identifiers**: Always use TEXT primary keys with UUIDs or ULIDs.
- **Avoid auto-incrementing integers**: Integer primary keys cause conflicts across multiple devices.
- **Use `cloudsync_uuid()`**: Generates UUIDv7 identifiers optimized for distributed systems.
- **Note:** Any write operation with a NULL primary key value will be rejected with an error.

```sql
-- Recommended: Globally unique TEXT primary key
CREATE TABLE users (
    id TEXT PRIMARY KEY,                    -- Use cloudsync_uuid()
    name TEXT NOT NULL DEFAULT '',
    email TEXT UNIQUE NOT NULL DEFAULT ''
);

-- Avoid: Auto-incrementing integer primary key
CREATE TABLE users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,  -- Causes conflicts across devices
    name TEXT NOT NULL DEFAULT '',
    email TEXT UNIQUE NOT NULL DEFAULT ''
);
```

## Column Constraint Guidelines

- All `NOT NULL` columns (except primary keys) **must** have `DEFAULT` values.
- For optional data, use nullable columns instead of empty strings.

```sql
CREATE TABLE tasks (
    id TEXT PRIMARY KEY,
    title TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL DEFAULT 'pending',
    priority INTEGER NOT NULL DEFAULT 1,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    assigned_to TEXT                       -- Nullable for optional data
);
```

## UNIQUE Constraint Considerations

In multi-tenant scenarios with Row-Level Security, UNIQUE constraints must be globally unique across all tenants in the cloud database. Use composite UNIQUE constraints for per-tenant uniqueness:

```sql
-- Multi-tenant: Composite unique constraint
CREATE TABLE users (
    id TEXT PRIMARY KEY,
    tenant_id TEXT NOT NULL DEFAULT '',
    email TEXT NOT NULL DEFAULT '',
    UNIQUE(tenant_id, email)    -- Unique email per tenant
);
```

## Foreign Key Compatibility

Foreign key constraints may conflict with the CRDT merge algorithm:

- CRDT changes are applied column-by-column during synchronization. Columns may be temporarily assigned DEFAULT values, so foreign key defaults must reference existing rows.
- RLS policies may block CASCADE DELETE/UPDATE operations on related rows.

**Recommendations:**
- Prefer application-level cascade logic over database-level CASCADE actions.
- Use nullable foreign keys to avoid DEFAULT value issues.
- Test synchronization scenarios with foreign key constraints enabled.

## Trigger Compatibility

Triggers can cause issues during synchronization:

- **Duplicate operations**: Triggers that modify synchronized tables may apply changes twice during merge.
- **Column-by-column processing**: UPDATE triggers may fire multiple times per row as each column is processed.

Avoid triggers that write to synchronized tables. Use application-level logic instead.
