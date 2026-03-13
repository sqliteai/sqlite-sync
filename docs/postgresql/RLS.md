# Row Level Security (RLS) with CloudSync

CloudSync is fully compatible with PostgreSQL Row Level Security. Standard RLS policies work out of the box.

## How It Works

### Column-batch merge

CloudSync resolves CRDT conflicts at the column level — a sync payload may contain individual column changes arriving one at a time. Before writing to the target table, CloudSync buffers all winning column values for the same primary key and flushes them as a single SQL statement. This ensures the database sees a complete row with all columns present.

### UPDATE vs INSERT selection

When flushing a batch, CloudSync chooses the statement type based on whether the row already exists locally:

- **New row**: `INSERT ... ON CONFLICT DO UPDATE` — all columns are present (including the ownership column), so the INSERT `WITH CHECK` policy can evaluate correctly.
- **Existing row**: `UPDATE ... SET ... WHERE pk = ...` — only the changed columns are set. The UPDATE `USING` policy checks the existing row, which already has the correct ownership column value.

### Per-PK savepoint isolation

Each primary key's flush is wrapped in its own savepoint. When RLS denies a write:

1. The database raises an error inside the savepoint
2. CloudSync rolls back that savepoint, releasing all resources acquired during the failed statement
3. Processing continues with the next primary key

This means a single payload can contain a mix of allowed and denied rows — allowed rows commit normally, denied rows are silently skipped. The caller receives the total number of column changes processed (including denied ones) rather than an error.

## Quick Setup

Given a table with an ownership column (`user_id`):

```sql
CREATE TABLE documents (
    id      TEXT PRIMARY KEY,
    user_id UUID,
    title   TEXT,
    content TEXT
);

SELECT cloudsync_init('documents');
```

Enable RLS and create standard policies:

```sql
ALTER TABLE documents ENABLE ROW LEVEL SECURITY;

CREATE POLICY "select_own" ON documents FOR SELECT
    USING (auth.uid() = user_id);

CREATE POLICY "insert_own" ON documents FOR INSERT
    WITH CHECK (auth.uid() = user_id);

CREATE POLICY "update_own" ON documents FOR UPDATE
    USING (auth.uid() = user_id)
    WITH CHECK (auth.uid() = user_id);

CREATE POLICY "delete_own" ON documents FOR DELETE
    USING (auth.uid() = user_id);
```

## Example: Two-User Sync with RLS

This example shows the complete flow of syncing data between two databases where the target enforces RLS.

### Setup

```sql
-- Source database (DB A) — no RLS, represents the sync server
CREATE TABLE documents (
    id TEXT PRIMARY KEY, user_id UUID, title TEXT, content TEXT
);
SELECT cloudsync_init('documents');

-- Target database (DB B) — RLS enforced
CREATE TABLE documents (
    id TEXT PRIMARY KEY, user_id UUID, title TEXT, content TEXT
);
SELECT cloudsync_init('documents');
ALTER TABLE documents ENABLE ROW LEVEL SECURITY;
-- (policies as above)
```

### Insert sync

User 1 creates a document on DB A:

```sql
-- On DB A
INSERT INTO documents VALUES ('doc1', 'user1-uuid', 'Hello', 'World');
```

Apply the payload on DB B as the authenticated user:

```sql
-- On DB B (running as user1)
SET app.current_user_id = 'user1-uuid';
SET ROLE authenticated;
SELECT cloudsync_payload_apply(decode(:payload_hex, 'hex'));
```

The insert succeeds because `user_id` matches `auth.uid()`.

### Insert denial

User 1 tries to sync a document owned by user 2:

```sql
-- On DB A
INSERT INTO documents VALUES ('doc2', 'user2-uuid', 'Secret', 'Data');
```

```sql
-- On DB B (running as user1)
SET app.current_user_id = 'user1-uuid';
SET ROLE authenticated;
SELECT cloudsync_payload_apply(decode(:payload_hex, 'hex'));
```

The insert is denied by RLS. The row does not appear in DB B. No error is raised to the caller — CloudSync isolates the failure via a per-PK savepoint and continues processing the remaining payload.

### Partial update sync

User 1 updates only the title of their own document:

```sql
-- On DB A
UPDATE documents SET title = 'Hello Updated' WHERE id = 'doc1';
```

The sync payload contains only the changed column (`title`). CloudSync detects that the row already exists on DB B and uses a plain `UPDATE` statement:

```sql
UPDATE documents SET title = $2 WHERE id = $1;
```

The UPDATE policy checks the existing row (which has the correct `user_id`), so it succeeds.

### Mixed payload

When a single payload contains rows for multiple users, CloudSync handles each primary key independently:

```sql
-- On DB A
INSERT INTO documents VALUES ('doc3', 'user1-uuid', 'Mine', '...');
INSERT INTO documents VALUES ('doc4', 'user2-uuid', 'Theirs', '...');
```

```sql
-- On DB B (running as user1)
SELECT cloudsync_payload_apply(decode(:payload_hex, 'hex'));
-- doc3 is inserted (allowed), doc4 is silently skipped (denied)
```

## Supabase Notes

When using Supabase:

1. **auth.uid()**: Returns the authenticated user's UUID from the JWT claims.
2. **JWT propagation**: Ensure the JWT token is set before sync operations:
   ```sql
   SELECT set_config('request.jwt.claims', '{"sub": "user-uuid", ...}', true);
   ```
3. **Service role bypass**: The Supabase service role bypasses RLS entirely. Use the `authenticated` role for user-context operations where RLS enforcement is desired.

## Troubleshooting

### "new row violates row-level security policy"

**Symptom**: Insert operations fail during sync.

**Cause**: The ownership column value doesn't match the authenticated user.

**Solution**: Verify that:
- The JWT / session variable is set correctly before calling `cloudsync_payload_apply`
- The `user_id` column in the synced data matches `auth.uid()`
- RLS policies reference the correct ownership column

### Debugging

```sql
-- Check current auth context
SELECT auth.uid();

-- Inspect a specific row's ownership
SELECT id, user_id FROM documents WHERE id = 'problematic-pk';

-- Temporarily disable RLS to inspect all data
ALTER TABLE documents DISABLE ROW LEVEL SECURITY;
-- ... inspect ...
ALTER TABLE documents ENABLE ROW LEVEL SECURITY;
```
