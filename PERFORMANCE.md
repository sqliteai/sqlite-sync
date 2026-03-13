# Performance & Overhead

This document describes the computational and storage overhead introduced by the CloudSync extension, and how sync execution time relates to database size.

## TL;DR

Sync execution time scales with **the number of changes since the last sync (D)**, not with total database size (N). If you sync frequently, D stays small regardless of how large the database grows. The per-operation overhead on writes is proportional to the number of columns in the affected row, not to the table size. This is fundamentally different from sync solutions that diff or scan the full dataset.

## Breaking Down the Cost

The overhead introduced by the extension can be decomposed into four independent concerns:

### 1. Per-Operation Overhead (Write-Path Cost)

Every INSERT, UPDATE, or DELETE on a synced table fires AFTER triggers that write CRDT metadata into a companion `<table>_cloudsync` table. This happens synchronously, inline with the original write.

| Operation | Metadata Rows Written | Complexity |
|-----------|----------------------|------------|
| INSERT    | 1 sentinel + 1 per non-PK column | O(C) |
| UPDATE    | 1 per changed column (NEW != OLD) | O(C_changed) <= O(C) |
| DELETE    | 1 sentinel + cleanup of existing metadata | O(C_existing) |

Where **C** = number of non-PK columns in the table.

**Key point:** This cost is **constant per row** and independent of the total number of rows in the table (N). Writing to a 100-row table costs the same as writing to a 10-million-row table. The metadata table uses a composite primary key `(pk, col_name)` with `WITHOUT ROWID` optimization (SQLite) or a standard B-tree primary key (PostgreSQL), so the index update cost is O(log M) where M is the metadata table size -- but this is the same cost as any indexed INSERT and is negligible in practice.

### 2. Sync Operations (Push & Pull)

These are the operations that create and apply sync payloads. They are synchronous in the extension and should typically be run by the application off the main thread.

#### Push: Payload Generation

```
Cost: O(D) where D = number of column-level changes since last sync
```

The push operation queries `cloudsync_changes`, which dynamically reads from all synced `<table>_cloudsync` tables:
```sql
SELECT ... FROM cloudsync_changes WHERE db_version > <last_synced_version>
```

Each metadata table has an **index on `db_version`**, so payload generation scales primarily with the number of new changes, plus a small per-synced-table overhead to construct the `cloudsync_changes` query. It does not diff the full dataset. In SQLite, each changed column also performs a primary-key lookup in the base table to retrieve the current value.

The resulting payload is LZ4-compressed before transmission.

#### Pull: Payload Application

```
Cost: O(D) to decode + O(D_unique_pks) to merge into the database
```

Incoming changes are decoded and **batched by primary key**. All column changes for the same row are accumulated and flushed as a single UPDATE or INSERT statement. This batching reduces the number of actual database writes to one per affected row, regardless of how many columns changed.

Conflict resolution (CRDT merge) is O(1) per column: it compares version numbers and, only if tied, falls back to value comparison and site-id tiebreaking. No global state or table scan is required.

#### Summary

| Phase | Scales With | Does NOT Scale With |
|-------|-------------|-------------------|
| Payload generation | D (changes since last sync) | N (total rows) |
| Payload application | D (incoming changes) | N (total rows) |
| Conflict resolution | D (conflicting columns) | N (total rows) |

**This means sync time is driven mainly by delta size (`D`) rather than total database size (`N`)**. As long as the number of changes between syncs stays bounded, sync time remains roughly stable even as the database grows.

### 3. Sync Frequency & Network Latency

When the application runs sync off the main thread, perceived latency depends on:

- **Sync interval**: How often the app triggers a push/pull cycle. More frequent syncs mean smaller deltas (smaller D) and faster individual sync operations, at the cost of more network round-trips.
- **Network latency**: The round-trip time to the sync server. LZ4 compression reduces payload size, but latency is dominated by the network hop itself for small deltas.
- **Payload size**: Proportional to D x average column value size. Large BLOBs or TEXT values will increase transfer time linearly.

The extension does not impose a sync schedule  -- the application controls when and how often to sync. A typical pattern is to sync on a timer (e.g., every 5-30 seconds) or on specific events (app foreground, user action).

### 4. Metadata Storage Overhead

Each synced table has a companion `<table>_cloudsync` metadata table with the following schema:

```
PRIMARY KEY (pk, col_name) -- WITHOUT ROWID (SQLite)
Columns: pk, col_name, col_version, db_version, site_id, seq
Index: db_version
```

**Storage cost per row in the base table:**
- 1 sentinel row (marks the row's existence/deletion state)
- 1 metadata row per non-PK column that has ever been written

So for a table with C non-PK columns, the metadata table will contain approximately `N x (1 + C)` rows, where N is the number of rows in the base table.

**Estimated overhead per metadata row:**
- `pk`: encoded primary key (typically 8-32 bytes depending on PK type and count)
- `col_name`: column name string (shared via SQLite's string interning, typically 5-30 bytes)
- `col_version`, `db_version`, `seq`: 3 integers (8 bytes each = 24 bytes)
- `site_id`: 1 integer (8 bytes)

Rough estimate: **60-100 bytes per metadata row**, or **60-100 x (1 + C) bytes per base table row**.

| Base Table | Columns (C) | Rows (N) | Estimated Metadata Size |
|------------|-------------|----------|------------------------|
| Small      | 5           | 1,000    | ~360 KB - 600 KB       |
| Medium     | 10          | 100,000  | ~66 MB - 110 MB        |
| Large      | 10          | 1,000,000| ~660 MB - 1.1 GB       |
| Wide       | 50          | 100,000  | ~306 MB - 510 MB       |

**Mitigation strategies:**
- Only sync tables that need it  -- not every table requires CRDT tracking.
- Prefer narrow tables (fewer columns) for high-volume data.
- The `WITHOUT ROWID` optimization (SQLite) significantly reduces per-row storage overhead.
- Deleted rows have their per-column metadata cleaned up, but a tombstone sentinel row persists (see section 9 below).

### 5. Read-Path Overhead

Normal application reads are not directly instrumented by the extension. No triggers, views, or hooks intercept ordinary SELECT queries on application tables, and the CRDT metadata is stored separately. In practice, read overhead is usually negligible.

### 6. Initial Sync (First Device)

When a new device syncs for the first time (`db_version = 0`), the push payload contains the **entire dataset**: every column of every row across all synced tables. The payload size is proportional to `N * C` (total rows times columns).

The payload is built entirely in memory, starting with a 512 KB buffer (`CLOUDSYNC_PAYLOAD_MINBUF_SIZE` in `src/cloudsync.c`) and growing via `realloc` as needed. Peak memory usage is at least the full uncompressed payload size and can be higher during compression. For a database with 1 million rows and 10 columns of average 50 bytes each, the uncompressed payload could reach ~500 MB before LZ4 compression.

Subsequent syncs are incremental (proportional to D, changes since the last sync), so the first sync is the expensive one. Applications with large datasets should plan for this -- for example, by seeding new devices from a database snapshot rather than syncing from scratch.

### 7. WAL and Disk I/O Amplification

Each write to a synced table generates additional metadata writes via AFTER triggers. The amplification factor depends on the operation:

| Operation | Total Writes (base + metadata) | Amplification Factor |
|-----------|-------------------------------|---------------------|
| INSERT (C columns) | 1 + 1 sentinel + C metadata | ~C+2x |
| UPDATE (1 column)  | 1 + 1 metadata              | 2x |
| UPDATE (C columns) | 1 + C metadata              | ~C+1x |
| DELETE              | 1 + cleanup writes          | variable |

For a table with 10 non-PK columns, an INSERT generates roughly 12 logical row writes instead of 1. This increases WAL/page churn and affects:

- **Disk I/O**: More pages written per transaction, larger WAL files between checkpoints.
- **WAL checkpoint frequency**: The WAL grows faster, so checkpoints run more often (or the WAL file stays larger if checkpointing is deferred).
- **Battery on mobile**: More disk writes per user action. Batching multiple writes in a single transaction amortizes the transaction overhead but not the per-row metadata cost.

### 8. Locking During Sync Apply

Payload application (`cloudsync_payload_apply`) uses savepoints grouped by source `db_version`. On SQLite, each savepoint holds a write lock for its duration. If the application runs sync on the main thread, other work on the same connection is blocked, and reads from other connections may block outside WAL mode.

On SQLite, using WAL mode prevents readers on other connections from being blocked by writers, which is the recommended configuration for concurrent sync.

### 9. Metadata Lifecycle (Tombstones and Cleanup)

When a row is deleted, the per-column metadata rows are removed, but a **tombstone sentinel** (`__[RIP]__`) persists in the metadata table. This tombstone is necessary for propagating deletes to other devices during sync. There is no automatic garbage collection of tombstones -- they accumulate over time.

Metadata cleanup for **removed columns** (after schema migration) only runs during `cloudsync_finalize_alter()`, which is called as part of the `cloudsync_alter()` workflow. Outside of schema changes, orphaned metadata from dropped columns remains in the metadata table.

The **site ID table** (`cloudsync_site_id`) also grows monotonically -- one entry per unique device that has ever synced. This is typically small (one row per device) and not a concern in practice.

For applications with high delete rates, the tombstone accumulation may become significant over time. Consider periodic full re-syncs or application-level archival strategies if this is a concern.

### 10. Multi-Table Considerations

The `cloudsync_changes` virtual table (SQLite) or set-returning function (PostgreSQL) dynamically constructs a `UNION ALL` query across all synced tables' metadata tables. The query construction cost scales as O(T) where T is the number of synced tables.

For most applications (fewer than ~50 synced tables), this is negligible. Applications syncing a very large number of tables should be aware that payload generation involves iterating over all synced tables to check for changes.

### Platform Differences (SQLite vs PostgreSQL)

- **SQLite** uses native C triggers registered directly with the SQLite API. Metadata tables use `WITHOUT ROWID` for compact storage.
- **PostgreSQL** uses row-level PL/pgSQL trigger functions that call into C functions via the extension. This adds a small amount of overhead per trigger invocation compared to SQLite's direct C triggers. Additionally, merge operations use per-PK savepoints to handle failures such as RLS policy violations gracefully.
- **Table registration** (`cloudsync_enable()`) is a one-time operation on both platforms. It creates 1 metadata table, 1 index, and 3 triggers (INSERT, UPDATE, DELETE), plus ~15-20 prepared statements that are cached for the lifetime of the connection.

## Comparison with Full-Scan Sync Solutions

Many sync solutions must diff or hash the entire dataset to determine what changed. This leads to O(N) sync time that grows linearly with total database size  -- the exact problem described in the question.

CloudSync avoids this through its **monotonic versioning approach**: every write increments a monotonic `db_version` counter, and the sync query filters on this counter using an index. The result is that sync time depends mainly on the volume of changes (D), not on the total data size (N).

```
Full-scan sync:   sync_time ~ O(N)           -- grows with database size
CloudSync:        sync_time ~ O(D)           -- grows with changes since last sync
                  where D is independent of N when sync frequency is constant
```

## Performance Optimizations in the Implementation

1. **`WITHOUT ROWID` tables** (SQLite): Metadata tables use clustered primary keys, avoiding the overhead of a separate rowid B-tree.
2. **`db_version` index**: Enables efficient range scans for delta extraction.
3. **Deferred batch merge**: Column changes for the same primary key are accumulated and flushed as a single SQL statement.
4. **Prepared statement caching**: Merge statements are compiled once and reused across rows.
5. **LZ4 compression**: Reduces payload size for network transfer.
6. **Per-column tracking**: Only changed columns are included in the sync payload, not entire rows.
7. **Early exit on stale data**: The CLS algorithm skips rows where the incoming causal length is lower than the local one, avoiding unnecessary column-level comparisons.
