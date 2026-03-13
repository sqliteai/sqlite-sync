# Deferred Column-Batch Merge and RLS Support

## Problem

CloudSync resolves CRDT conflicts per-column, so `cloudsync_payload_apply` processes column changes one at a time. Previously each winning column was written immediately via a single-column `INSERT ... ON CONFLICT DO UPDATE`. This caused two issues with PostgreSQL RLS:

1. **Partial-column UPSERT fails INSERT WITH CHECK**: An update to just `title` generates `INSERT INTO docs (id, title) VALUES (...)  ON CONFLICT DO UPDATE SET title=...`. PostgreSQL evaluates the INSERT `WITH CHECK` policy *before* checking for conflicts. Missing columns (e.g. `user_id`) default to NULL, so `auth.uid() = user_id` fails. The ON CONFLICT path is never reached.

2. **Premature flush in SPI**: `database_in_transaction()` always returns true inside PostgreSQL SPI. The old code only updated `last_payload_db_version` inside `if (!in_transaction && db_version_changed)`, so the variable stayed at -1, `db_version_changed` was true on every row, and batches flushed after every single column.

## Solution

### Batch merge (`merge_pending_batch`)

New structs in `cloudsync.c`:

- `merge_pending_entry` — one buffered column (col_name, col_value via `database_value_dup`, col_version, db_version, site_id, seq)
- `merge_pending_batch` — collects entries for one PK (table, pk, row_exists flag, entries array, statement cache)

`data->pending_batch` is set to `&batch` (stack-allocated) at the start of `cloudsync_payload_apply`. The INSTEAD OF trigger calls `merge_insert`, which calls `merge_pending_add` instead of `merge_insert_col`. Flush happens at PK/table/db_version boundaries and after the loop.

### UPDATE vs UPSERT (`row_exists` flag)

`merge_insert` sets `batch->row_exists = (local_cl != 0)` on the first winning column. At flush time `merge_flush_pending` selects:

- `row_exists=true` -> `sql_build_update_pk_and_multi_cols` -> `UPDATE docs SET title=? WHERE id=?`
- `row_exists=false` -> `sql_build_upsert_pk_and_multi_cols` -> `INSERT ... ON CONFLICT DO UPDATE`

Both SQLite and PostgreSQL implement `sql_build_update_pk_and_multi_cols` as a proper UPDATE statement. This is required for SQLiteCloud (which uses the SQLite extension but enforces RLS).

**Example**: DB A and DB B both have row `id='doc1'` with `user_id='alice'`, `title='Hello'`. Alice updates `title='World'` on A. The payload applied to B contains only `(id, title)`:

- **UPSERT** (wrong for RLS): `INSERT INTO docs ("id","title") VALUES (?,?) ON CONFLICT DO UPDATE SET "title"=EXCLUDED."title"` — fails INSERT `WITH CHECK` because `user_id` is NULL in the proposed row.
- **UPDATE** (correct): `UPDATE "docs" SET "title"=?2 WHERE "id"=?1` — skips INSERT `WITH CHECK` entirely; the UPDATE `USING` policy checks the existing row which has the correct `user_id`.

In plain SQLite (no RLS) both produce the same result. The distinction only matters when RLS is enforced (SQLiteCloud, PostgreSQL).

### Statement cache

`merge_pending_batch` caches the last prepared statement (`cached_vm`) along with the column combination and `row_exists` flag that produced it. On each flush, `merge_flush_pending` compares the current column names, count, and `row_exists` against the cache:

- **Cache hit**: `dbvm_reset` + rebind (skip SQL build and `databasevm_prepare`)
- **Cache miss**: finalize old cached statement, build new SQL, prepare, and update cache

This recovers the precompiled-statement advantage of the old single-column path. In a typical payload where consecutive PKs change the same columns, the cache hit rate is high.

The cached statement is finalized once at the end of `cloudsync_payload_apply`, not on every flush.

### `last_payload_db_version` fix

Moved the update outside the savepoint block so it executes unconditionally:

```c
if (db_version_changed) {
    last_payload_db_version = decoded_context.db_version;
}
```

Previously this was inside `if (!in_transaction && db_version_changed)`, which never ran in SPI.

## Savepoint Architecture

### Two-level savepoint design

`cloudsync_payload_apply` uses two layers of savepoints that serve different purposes:

| Layer | Where | Purpose |
|-------|-------|---------|
| **Outer** (per-db_version) | `cloudsync_payload_apply` loop | Transaction grouping + commit hook trigger (SQLite only) |
| **Inner** (per-PK) | `merge_flush_pending` | RLS error isolation + executor resource cleanup |

### Outer savepoints: per-db_version in `cloudsync_payload_apply`

```c
if (!in_savepoint && db_version_changed && !database_in_transaction(data)) {
    database_begin_savepoint(data, "cloudsync_payload_apply");
    in_savepoint = true;
}
```

These savepoints group rows with the same source `db_version` into one transaction. The `RELEASE` (commit) at each db_version boundary triggers `cloudsync_commit_hook`, which:
- Saves `pending_db_version` as the new `data->db_version`
- Resets `data->seq = 0`

This ensures unique `(db_version, seq)` tuples in `cloudsync_changes` across groups.

**In PostgreSQL SPI, these are dead code**: `database_in_transaction()` returns `true` (via `IsTransactionState()`), so the condition `!database_in_transaction(data)` is always false and `in_savepoint` is never set. This is correct because:
1. PostgreSQL has no equivalent commit hook on subtransaction release
2. The SPI transaction from `SPI_connect` already provides transaction context
3. The inner per-PK savepoint handles the RLS isolation PostgreSQL needs

**Why a single outer savepoint doesn't work**: We tested replacing per-db_version savepoints with a single savepoint wrapping the entire loop. This broke the `(db_version, seq)` uniqueness invariant in SQLite because the commit hook never fired mid-apply — `data->db_version` never advanced and `seq` never reset.

### Inner savepoints: per-PK in `merge_flush_pending`

```c
flush_savepoint = (database_begin_savepoint(data, "merge_flush") == DBRES_OK);
// ... database operations ...
cleanup:
    if (flush_savepoint) {
        if (rc == DBRES_OK) database_commit_savepoint(data, "merge_flush");
        else database_rollback_savepoint(data, "merge_flush");
    }
```

Wraps each PK's flush in a savepoint. On failure (e.g. RLS denial), `database_rollback_savepoint` calls `RollbackAndReleaseCurrentSubTransaction()` in PostgreSQL, which properly releases all executor resources (open relations, snapshots, plan cache) acquired during the failed statement. This eliminates the "resource was not closed" warnings that `SPI_finish` previously emitted.

In SQLite, when the outer per-db_version savepoint is active, these become harmless nested savepoints.

### Platform behavior summary

| Environment | Outer savepoint | Inner savepoint | Effect |
|---|---|---|---|
| **PostgreSQL SPI** | Dead code (`in_transaction` always true) | Active — RLS error isolation + resource cleanup | Only inner savepoint runs |
| **SQLite client** | Active — groups writes, triggers commit hook | Active — nested inside outer, harmless | Both run; outer provides transaction grouping |
| **SQLiteCloud** | Active — groups writes, triggers commit hook | Active — RLS error isolation | Both run; each serves its purpose |

## SPI and Memory Management

### Nested SPI levels

`pg_cloudsync_payload_apply` calls `SPI_connect` (level 1). Inside the loop, `databasevm_step` executes `INSERT INTO cloudsync_changes`, which fires the INSTEAD OF trigger. The trigger calls `SPI_connect` (level 2), runs `merge_insert` / `merge_pending_add`, then `SPI_finish` back to level 1. The deferred `merge_flush_pending` runs at level 1.

### `database_in_transaction()` in SPI

Always returns true in SPI context (`IsTransactionState()`). This makes the per-db_version savepoints dead code in PostgreSQL and is why `last_payload_db_version` must be updated unconditionally.

### Error handling in SPI

When RLS denies a write, PostgreSQL raises an error inside SPI. The inner per-PK savepoint in `merge_flush_pending` catches this: `RollbackAndReleaseCurrentSubTransaction()` properly releases all executor resources. Without the savepoint, `databasevm_step`'s `PG_CATCH` + `FlushErrorState()` would clear the error stack but leave executor resources orphaned, causing `SPI_finish` to emit "resource was not closed" warnings.

### Batch cleanup paths

`batch.entries` is heap-allocated via `cloudsync_memory_realloc` and reused across flushes. Each entry's `col_value` (from `database_value_dup`) is freed by `merge_pending_free_entries` on every flush. The entries array, `cached_vm`, and `cached_col_names` are freed once at the end of `cloudsync_payload_apply`. Error paths (`goto cleanup`, early returns) must free all three and call `merge_pending_free_entries` to avoid leaking `col_value` copies.

## Batch Apply: Pros and Cons

The batch path is used for all platforms (SQLite client, SQLiteCloud, PostgreSQL), not just when RLS is active.

**Pros (even without RLS)**:
- Fewer SQL executions: N winning columns per PK become 1 statement instead of N. Each `databasevm_step` involves B-tree lookup, page modification, WAL write.
- Atomicity per PK: all columns for a PK succeed or fail together.

**Cons**:
- Dynamic SQL per unique column combination (mitigated by the statement cache).
- Memory overhead: `database_value_dup` copies each column value into the buffer.
- Code complexity: batching structs, flush logic, cleanup paths.

**Why not maintain two paths**: SQLiteCloud uses the SQLite extension with RLS, so the batch path (UPDATE vs UPSERT selection, per-PK savepoints) is required there. Maintaining a separate single-column path for plain SQLite clients would double the code with marginal benefit.

## Files Changed

| File | Change |
|------|--------|
| `src/cloudsync.c` | Batch merge structs with statement cache (`cached_vm`, `cached_col_names`), `merge_pending_add`, `merge_flush_pending` (with per-PK savepoint), `merge_pending_free_entries`; `pending_batch` field on context; `row_exists` propagation in `merge_insert`; batch mode in `merge_sentinel_only_insert`; `last_payload_db_version` fix; removed `payload_apply_callback` |
| `src/cloudsync.h` | Removed `CLOUDSYNC_PAYLOAD_APPLY_STEPS` enum |
| `src/database.h` | Added `sql_build_upsert_pk_and_multi_cols`, `sql_build_update_pk_and_multi_cols`; removed callback typedefs |
| `src/sqlite/database_sqlite.c` | Implemented `sql_build_upsert_pk_and_multi_cols` (dynamic SQL); `sql_build_update_pk_and_multi_cols` (delegates to upsert); removed callback functions |
| `src/postgresql/database_postgresql.c` | Implemented `sql_build_update_pk_and_multi_cols` (meta-query against `pg_catalog` generating typed UPDATE) |
| `test/unit.c` | Removed callback code and `do_test_andrea` debug function (fixed 288048-byte memory leak) |
| `test/postgresql/27_rls_batch_merge.sql` | Tests 1-3 (superuser) + Tests 4-6 (authenticated-role RLS enforcement) |
| `docs/postgresql/RLS.md` | Documented INSERT vs UPDATE paths and partial-column RLS interaction |

## TODO

 - update documentation: RLS.md, README.md and the https://github.com/sqlitecloud/docs repo
