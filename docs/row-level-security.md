# Row-Level Security

SQLite Sync supports **Row-Level Security (RLS)** through the underlying [SQLite Cloud](https://sqlitecloud.io/) infrastructure. RLS allows you to use a **single shared cloud database** while each client only sees and modifies its own data. Policies are enforced on the server, so the security boundary is at the database level, not in application code.

## How It Works

- Control not just who can read or write a table, but **which specific rows** they can access.
- Each device syncs only the rows it is authorized to see: no full dataset download, no client-side filtering.

For example:

- User A can only see and edit their own data.
- User B can access a different set of rows, even within the same shared table.

## Benefits

- **Single database, multiple tenants**: one cloud database serves all users. RLS policies partition data per user or role, eliminating the need to provision separate databases.
- **Efficient sync**: each client downloads only its authorized rows, reducing bandwidth and local storage.
- **Server-enforced security**: policies are evaluated on the server during sync. A compromised or modified client cannot bypass access controls.
- **Simplified development**: no need to implement permission logic in your application. Define policies once in the database and they apply everywhere.

## Authentication

RLS requires token-based authentication. Use `cloudsync_network_set_token()` instead of `cloudsync_network_set_apikey()`:

```sql
SELECT cloudsync_network_init('your-managed-database-id');
SELECT cloudsync_network_set_token('your_auth_token');
SELECT cloudsync_network_sync();
```

For more information on access tokens, see the [Access Tokens documentation](https://docs.sqlitecloud.io/docs/access-tokens).

## Schema Considerations

When using RLS with multi-tenant schemas, UNIQUE constraints must be globally unique across all tenants in the cloud database. For columns that should only be unique within a tenant, use composite UNIQUE constraints:

```sql
CREATE TABLE users (
    id TEXT PRIMARY KEY,
    tenant_id TEXT NOT NULL DEFAULT '',
    email TEXT NOT NULL DEFAULT '',
    UNIQUE(tenant_id, email)
);
```

For more schema guidelines, see [Database Schema Recommendations](./schema.md).

For full RLS documentation, see the [SQLite Cloud RLS documentation](https://docs.sqlitecloud.io/docs/rls).
