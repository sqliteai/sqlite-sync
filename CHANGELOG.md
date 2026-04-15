# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [1.0.15] - 2026-04-15

### Fixed

- **Silent receive failures in `cloudsync_network_sync`**: When `cloudsync_payload_apply` failed during the receive phase (for example with an unknown schema hash, invalid checksum, or decompression error), the error was stored only on the internal cloudsync context and never propagated to the SQL caller. `cloudsync_network_check_changes()` returned `NULL` with no error, and `cloudsync_network_sync()` silently dropped the failure. `network_download_changes` now surfaces `cloudsync_payload_apply` failures to the caller.

### Changed

- **`cloudsync_network_send_changes()` output** now includes a `send.lastFailure` object whenever the server reports one (raw pass-through of the server's `lastFailure` — `jobId`, `code`, `message`, `retryable`, `failedAt`, …), regardless of whether the computed `send.status` is `synced`, `syncing`, or `out-of-sync`. The field is omitted when the server does not report a failure.
- **`cloudsync_network_sync()` output** now mirrors the same `send.lastFailure` field and, if the receive phase fails (network-level or `cloudsync_payload_apply` error), returns structured JSON with a new `receive.error` string rather than raising a SQL error. The send result is always preserved so callers can tell that their local changes reached the server even when applying incoming changes failed. The receive retry loop now breaks immediately on a receive error (a schema-hash mismatch will not heal across retries).
- **`cloudsync_network_check_changes()`** keeps its error-raising behavior — a receive failure in the standalone check path is still reported as a SQL error, since there is no send result to preserve.

## [1.0.14] - 2026-04-15

### Fixed

- **Stale `cloudsync_table_settings` crash**: Reopening a database that had its base table and `<table>_cloudsync` meta-table dropped without calling `cloudsync_cleanup` crashed with a double-free on `sqlite3_close`. Two bugs were involved: (1) `cloudsync_dbversion_rebuild` returned `DBRES_NOMEM` when `cloudsync_dbversion_build_query` yielded a NULL SQL string (stale row in `cloudsync_table_settings` but no matching `*_cloudsync` table in `sqlite_master`), failing extension init; (2) on init failure `dbsync_register_functions` manually freed the context that SQLite already owned via the `cloudsync_version` destructor, causing a double-free when the connection was later closed. `cloudsync_dbversion_rebuild` now treats a NULL build query the same as `count == 0` (no prepared statement, db_version stays at the minimum and is rebuilt on the next `cloudsync_init`), and the manual free in the error path has been removed.

### Added

- Unit test `do_test_stale_table_settings_dropped_meta` (Stale Table Settings Dropped Meta) covering the drop-base-table + drop-meta-table + reopen scenario.

## [1.0.13] - 2026-04-14

### Fixed

- **Block-level LWW migration**: When `cloudsync_set_column(..., 'algo', 'block')` is called on a table that already has tracked rows, those rows are now immediately migrated into the blocks table. Previously, pre-existing column values were ignored until the next UPDATE, leaving sync state incomplete. The migration uses a two-phase collect-then-write approach to avoid SQLite cursor invalidation and `INSERT OR IGNORE` / `ON CONFLICT DO NOTHING` semantics for idempotency.

### Added

- Unit test `do_test_block_lww_existing_data` (Block LWW Existing Data) verifying block migration on `set_column`, idempotency of repeated `set_column` calls, and correct materialization after update.
- PostgreSQL test `50_block_lww_existing_data.sql` with equivalent coverage for the PostgreSQL backend.

## [1.0.12] - 2026-04-11

### Fixed

- **Settings loader**: Prevent infinite loop in `sqlite3_cloudsync_init` when reopening a database that has a persisted block-column setting. `dbutils_settings_table_load_callback` was calling `cloudsync_setup_block_column`, which `REPLACE`d the same row into `cloudsync_table_settings` while `sqlite3_exec` was still iterating it, re-feeding the rewritten row to the cursor. Added a `persist` flag to `cloudsync_setup_block_column` so the loader replays the in-memory setup without writing back.
- **PostgreSQL tests**: Updated 168 `cloudsync_init` callsites across 43 `test/postgresql/*.sql` files to pass integer flags (`0`/`1`) instead of `true`/`false`, matching the signature change in 1.0.9.
- **CI**: The `postgres-test` job now fails on SQL errors and `[FAIL]` markers. `psql` is run with `ON_ERROR_STOP=on`, `pipefail` is enabled around the `tee`, and the captured log is grepped for `[FAIL]` / `psql ERROR` as a final guard.

### Added

- Unit test `do_test_block_column_reload` (Block Column Reload) that persists a block column with a custom delimiter, closes the database, and reopens it — without the fix this hangs the test process.

## [1.0.11] - 2026-04-11

### Fixed

- **cloudsync_cleanup**: Now also drops the `{table}_cloudsync_blocks` table when the table has block LWW columns configured via `cloudsync_set_column(..., 'algo', 'block')`.

### Added

- Unit test `do_test_block_lww_cleanup` verifying that both `{table}_cloudsync` and `{table}_cloudsync_blocks` are removed after `cloudsync_cleanup`.

## [1.0.10] - 2026-04-08

### Fixed

- **PostgreSQL**: Prevent debug assertion crash on `cloudsync_init` error path (#37).
- **Row filter**: `cloudsync_set_filter` and `cloudsync_clear_filter` now reset the metatable and refill it from scratch, ensuring only rows matching the active filter are tracked for sync (#38).

### Added

- Row filter edge-case test coverage: clear/change filter lifecycle, complex expressions (AND, IS NULL), row enter/exit via UPDATE, composite PK with multi-column filters, multi-table roundtrip sync, and pre-existing data prefill tests for both SQLite and PostgreSQL.

## [1.0.9] - 2026-04-08

### Changed

- **cloudsync_init**: Replaced the `force` boolean parameter with an `init_flags` integer bitmask (`CLOUDSYNC_INIT_FLAG`), allowing fine-grained control over which schema sanity checks are skipped. Existing callers passing `0`/`false` or `1`/`true` remain compatible.
- **API**: Updated `cloudsync_init` SQL signature (PostgreSQL) to accept `integer` instead of `boolean` for the third argument, enabling flag combinations via bitwise OR.

### Added

- `CLOUDSYNC_INIT_FLAG_NONE` (0), `CLOUDSYNC_INIT_FLAG_SKIP_INT_PK_CHECK` (1), `CLOUDSYNC_INIT_FLAG_SKIP_NOT_NULL_DEFAULT_CHECK` (2), `CLOUDSYNC_INIT_FLAG_SKIP_NOT_NULL_PRIKEYS_CHECK` (4) enum values.
- Documentation for `cloudsync_set_filter` and `cloudsync_clear_filter` in API.md.

## [1.0.8] - 2026-04-03

### Changed

- **CI/CD**: Fix flutter package publish workflow not triggering on new releases.

## [1.0.7] - 2026-04-02

### Fixed

- **Harden table initialization against stale config and error cleanup.**

## [1.0.2] - 2026-03-25

### Fixed

- **Swift Package**: Use binary target and versioned macOS framework for Xcode 26 compatibility.
- **Minor bugs** in tests, docs, and examples related to the 1.0.0 major release.

## [1.0.0] - 2026-03-24

### Added

- **PostgreSQL support**: The CloudSync extension can now be built and loaded on PostgreSQL, so both SQLiteCloud and PostgreSQL are supported as the cloud backend database of the sync service. The core CRDT functions are shared by the SQLite and PostgreSQL extensions. Includes support for PostgreSQL-native types (UUID primary keys, composite PKs with mixed types, and automatic type casting).
- **Row-Level Security (RLS)**: Sync payloads are now fully compatible with SQLiteCloud and PostgreSQL Row-Level Security policies. Changes are buffered per primary key and flushed as complete rows, so RLS policies can evaluate all columns at once.
- **Block-level LWW for text conflict resolution**: Text columns can now be tracked at block level (lines by default) using Last-Writer-Wins. Concurrent edits to different parts of the same text are preserved after sync. New functions: `cloudsync_set_column()` to write individual blocks and `cloudsync_text_materialize()` to reconstruct the full text.

### Changed

- **BREAKING: `cloudsync_network_init` now accepts a `managedDatabaseId` instead of a connection string.** The `managedDatabaseId` is returned by the CloudSync service when a new database is registered for sync. For SQLiteCloud projects, it can be obtained from the project's OffSync page on the dashboard.

  Before:
  ```sql
  SELECT cloudsync_network_init('sqlitecloud://myproject.sqlite.cloud:8860/mydb.sqlite?apikey=KEY');
  ```

  After:
  ```sql
  SELECT cloudsync_network_init('your-managed-database-id');
  ```

- **BREAKING: Sync functions now return structured JSON.** `cloudsync_network_send_changes`, `cloudsync_network_check_changes`, and `cloudsync_network_sync` return a JSON object instead of a plain integer. This provides richer status information including sync state, version numbers, row counts, and affected table names.

  Before:
  ```sql
  SELECT cloudsync_network_sync();
  -- 3  (number of rows received)
  ```

  After:
  ```sql
  SELECT cloudsync_network_sync();
  -- '{"send":{"status":"synced","localVersion":5,"serverVersion":5},"receive":{"rows":3,"tables":["tasks"]}}'
  ```

- **Batch merge replaces column-by-column processing**: During sync, changes to the same row are now applied in a single SQL statement instead of one statement per column. This eliminates the previous behavior where UPDATE triggers fired multiple times per row during synchronization.
- **Network endpoints updated for the CloudSync v2 HTTP service**: Internal network layer now targets the new CloudSync service endpoints, including support for multi-organization routing.
- **NULL primary key rejection at runtime**: The extension now enforces NULL primary key rejection at runtime, so the explicit `NOT NULL` constraint on primary key columns is no longer a schema requirement.

### Fixed

- **Improved error reporting**: Sync network functions now surface the actual server error message instead of generic error codes.
- **Schema hash verification**: Normalized schema comparison now uses only column name (lowercase), type (SQLite affinity), and primary key flag, preventing false mismatches caused by formatting differences.
- **SQLite trigger safety**: Internal functions used inside triggers are now marked with `SQLITE_INNOCUOUS`, fixing `unsafe use of` errors when initializing tables that have triggers.
- **NULL column binding**: Column value parameters are now correctly bound even when NULL, preventing sync failures on rows with NULL values.
- **Stability and reliability improvements** across the SQLite and PostgreSQL codebases, including fixes to memory management, error handling, and CRDT version tracking.
