# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

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
