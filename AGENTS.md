# AGENTS.md

This file provides general technical guidance about the SQLite Sync codebase for AI agents and autonomous workflows.

## Project Overview

**SQLite Sync** is a C-based SQLite extension that implements CRDT (Conflict-free Replicated Data Type) algorithms to enable offline-first, multi-device synchronization for SQLite databases. The extension adds automatic conflict resolution and network synchronization capabilities directly into SQLite without requiring external dependencies.

## Quickstart

1. Build the extension: `make` (outputs `dist/cloudsync.*` for your platform).
2. Launch SQLite against a test DB: `sqlite3 demo.db`.
3. In the SQLite shell:
   ```sql
   .load ./dist/cloudsync          -- adjust suffix for your OS
   CREATE TABLE notes (id TEXT PRIMARY KEY NOT NULL, body TEXT DEFAULT '');
   SELECT cloudsync_init('notes', 'CLS');
   INSERT INTO notes VALUES (cloudsync_uuid(), 'hello');
   SELECT * FROM cloudsync_changes WHERE tbl='notes';  -- view pending changes
   ```

## Build Commands

### Building the Extension

```bash
# Build for current platform (auto-detected)
make

# Build with code coverage
make test COVERAGE=true

# Build for specific platforms
make PLATFORM=macos
make PLATFORM=linux
make PLATFORM=windows
make PLATFORM=android ARCH=arm64-v8a ANDROID_NDK=/path/to/ndk
make PLATFORM=ios
make PLATFORM=ios-sim

# Build Apple XCFramework
make xcframework

# Build Android AAR package
make aar
```

### Testing

```bash
# Run all tests (builds extension + unit tests, runs in SQLite)
make test

# Run only unit tests
make unittest

# Run tests with coverage report (generates coverage/ directory with HTML report)
make test COVERAGE=true

# Run with custom SQLite3 binary 
make test SQLITE3=/path/to/sqlite3
```

**macOS Testing Note:** If the default `/usr/bin/sqlite3` doesn't support loading extensions, set the SQLITE3 variable when running tests (Adjust the version path if using a specific version like /opt/homebrew/Cellar/sqlite/3.50.4/bin/sqlite3:
```
make test SQLITE3=/opt/homebrew/bin/sqlite3
make unittest SQLITE3=/opt/homebrew/bin/sqlite3
```

### Build System

The Makefile supports cross-platform compilation:
- Auto-detects host platform (Linux, macOS, Windows)
- Uses parallel builds (`-j` based on CPU cores)
- Handles platform-specific compilers, flags, and dependencies
- Downloads and builds curl statically with minimal feature set for network layer
- For Android: requires ANDROID_NDK environment variable and ARCH parameter

### Cleaning

```bash
# Remove all build artifacts
make clean
```

## Directory Structure

The codebase is organized to separate multi-platform (database-agnostic) code from database-specific implementations:

```
src/
├── cloudsync.c/h          # Multi-platform CRDT core
├── pk.c/h                 # Multi-platform payload encoding
├── network.c/h            # Multi-platform network layer
├── dbutils.c/h            # Multi-platform database utilities
├── utils.c/h              # Multi-platform utilities (UUID, hashing, etc.)
├── lz4.c/h                # Multi-platform compression
├── database.h             # Database abstraction API
│
├── sqlite/                # SQLite-specific implementations
│   ├── database_sqlite.c          # Implements database.h for SQLite
│   ├── cloudsync_sqlite.c         # Extension entry point
│   ├── cloudsync_sqlite.h
│   └── cloudsync_changes_sqlite.c/h  # Virtual table implementation
│
└── postgresql/            # PostgreSQL-specific implementations
    ├── database_postgresql.c      # Implements database.h for PostgreSQL
    ├── cloudsync_pg.c            # Extension entry point
    └── cloudsync_pg.h
```

**Key principles:**
- Files at `src/` root are multi-platform and work with any database via `database.h`
- Files in `src/sqlite/` and `src/postgresql/` contain database-specific code
- All database interaction goes through the abstraction layer defined in `database.h`

## Core Architecture

### Database Abstraction Layer

The codebase uses a database abstraction layer (`database.h`) that wraps database-specific APIs. Database-specific implementations are organized in subdirectories: `src/sqlite/database_sqlite.c` for SQLite, `src/postgresql/database_postgresql.c` for PostgreSQL. All database interactions go through this abstraction layer using:
- `cloudsync_context` - opaque per-database context shared across layers
- `dbvm_t` - opaque prepared statement/virtual machine handle
- `dbvalue_t` - opaque database value handle

The abstraction exposes:
- Result/status codes (`DBRES`), data types (`DBTYPE`), and flags (`DBFLAG`).
- Core query helpers (`database_exec`, `database_select_*`, `database_write`).
- Schema/metadata helpers (`database_table_exists`, `database_trigger_exists`, `database_count_*`, `database_pk_names`).
- Transaction helpers (`database_begin_savepoint`, `database_commit_savepoint`, `database_rollback_savepoint`, `database_in_transaction`).
- VM lifecycle (`databasevm_prepare/step/reset/finalize/clear_bindings`) plus bind/value/column accessors.
- Backend memory helpers (`dbmem_*`) and SQL builder helpers (`sql_build_*`).

### CRDT Implementation

The extension implements four CRDT algorithms for different use cases:

1. **CLS (Causal-Length Set)** - Default algorithm, balances add/delete operations
2. **GOS (Grow-Only Set)** - Additions only, deletions create tombstones
3. **DWS (Delete-Wins Set)** - Deletions take precedence over additions
4. **AWS (Add-Wins Set)** - Additions take precedence over deletions

Algorithm selection is per-table via `cloudsync_init(table_name, algo)`.

### Key Components

#### Core Sync Engine (`cloudsync.c/h`)

The main synchronization logic and public API. Key structures:
- `cloudsync_context` - Per-database sync context (site ID, version, sequence counters)
- `cloudsync_table_context` - Per-table sync metadata (algorithm, columns, primary keys)

Critical functions:
- `cloudsync_init_table()` - Initializes table for sync, creates metadata tables and triggers
- `cloudsync_payload_save()` - Exports changes as binary payload
- `cloudsync_payload_apply()` - Applies incoming changes with CRDT merge logic
- `cloudsync_commit_hook()` / `cloudsync_rollback_hook()` - Transaction hooks for change tracking

#### Virtual Table (`src/sqlite/cloudsync_changes_sqlite.c`)

Implements `cloudsync_changes` virtual table (SQLite-specific) that provides a SQL interface to view pending changes:
```sql
SELECT * FROM cloudsync_changes WHERE tbl='my_table';
```

#### Payload Encoding (`pk.c`)

Efficient binary serialization of database changes:
- Platform-independent (handles endianness with htonl/ntohl)
- Encodes type information + variable-length data
- Minimizes payload size for network transmission
- Supports all SQLite types (integer, float, text, blob, null)

#### Network Layer (`network.c/h`)

Built-in synchronization with SQLite Cloud:
- Uses libcurl for HTTPS communication
- Handles authentication (API keys and JWT tokens)
- Implements retry logic and state reconciliation
- Functions: `cloudsync_network_init()`, `cloudsync_network_sync()`, etc.

#### Database Utilities (`dbutils.c/h`)

Helper functions for:
- Creating/managing sync metadata tables (`cloudsync_settings`, `cloudsync_table_settings`, etc.)
- Schema validation and sanity checks
- Trigger management for change tracking
- Settings persistence (sync versions, sequences, algorithms)

#### UUID Generation (`utils.c`)

Implements UUIDv7 generation optimized for distributed systems:
- Timestamp-based with monotonic ordering
- Globally unique across devices
- Available via `cloudsync_uuid()` SQL function

### Metadata Tables

The extension creates internal tables to track sync state:

- `cloudsync_settings` - Global sync configuration and state
- `cloudsync_table_settings` - Per-table sync configuration
- `cloudsync_site_id` - Unique site identifier for this database
- `cloudsync_schema_versions` - Schema version tracking
- `{table}_cloudsync` - Per-table CRDT metadata (logical clock, site IDs)

### Change Tracking

The extension uses SQLite triggers to automatically track all changes:
- INSERT triggers mark new rows for synchronization
- UPDATE triggers record which columns changed and their versions
- DELETE triggers create tombstone records (for most CRDT algorithms)
- Triggers are created/managed by `cloudsync_init()` based on the chosen algorithm

### Merge Algorithm

When applying remote changes via `cloudsync_payload_apply()`:

1. Changes are deserialized from binary payload
2. For each change, CRDT algorithm determines conflict resolution:
   - Compares vector clocks (db_version, sequence, site_id)
   - Column-by-column merge based on causal ordering
   - Handles concurrent updates deterministically
3. Local database updated with winning values
4. Metadata tables updated with merge results

## Architecture Patterns

Understanding the architectural patterns helps when modifying or extending the codebase.

### 1. SQLite Extension Pattern

The entire system is built as a **loadable SQLite extension**:
- Single entry point: `sqlite3_cloudsync_init()` in `src/sqlite/cloudsync_sqlite.c`
- Registers custom SQL functions during initialization
- Extends SQLite without modifying its core
- Loaded dynamically: `.load ./cloudsync` or `SELECT load_extension('./cloudsync')`

**Key benefit**: Users add sync to existing SQLite apps by loading the extension and calling setup functions—no application rewrite needed.

### 2. Shadow Metadata Tables Pattern

For each synced table (e.g., `users`), the extension creates parallel metadata tables:

```
users                   (user's actual data - unchanged)
users_cloudsync         (CRDT metadata: versions, site_ids, per-column logical clock)
```

**Benefits**:
- Zero schema pollution—user tables remain unchanged
- Efficient queries like "what changed since version X"
- Metadata separate from application data
- Users can drop sync by removing metadata tables

### 3. Vector Clock CRDT Pattern

Each column value carries a **vector clock** for causal ordering:

```c
// Stored in {table}_cloudsync for each column:
- col_version:  Lamport clock for a specific column, used to resolve merge conflicts when syncing databases that have taken independent writes. The primary purpose of col_version is to determine which value "wins" when two different peers update the same column of the same row offline and then merge their changes. The value with the higher col_version is selected as the most recent/authoritative one.
- db_version: Lamport clock for the entire database. This value is incremented with every transaction.
- site_id: UUID identifying which device made the change
- seq: sequence number for ordering changes within same db_version
```

**Merge algorithm** (column-by-column):
1. Compare vector clocks between local and remote values
2. Higher version wins (causally later)
3. Same version → use site_id as deterministic tiebreaker
4. No data loss, no manual conflict resolution

**Why column-level?** Allows merging concurrent updates to different columns of the same row (e.g., User A updates email, User B updates phone—both changes preserved).

### 4. Trigger-Based Change Tracking Pattern

All changes captured **declaratively** using SQLite triggers:

```sql
-- Auto-generated for each synced table
CREATE TRIGGER users_insert_trigger AFTER INSERT ON users
BEGIN
  INSERT INTO users_cloudsync (...);  -- Record CRDT metadata
END;
```

**User experience**:
```sql
-- User just does normal SQL:
INSERT INTO users (id, name) VALUES (cloudsync_uuid(), 'Alice');
UPDATE users SET email = 'alice@example.com' WHERE id = '...';
DELETE FROM users WHERE id = '...';

-- Triggers automatically capture metadata—no API calls needed
```

**Implementation**: Triggers created/destroyed by `cloudsync_init()` / `cloudsync_cleanup()` in `dbutils.c`.

### 5. Transaction Hook Pattern

Integrates with SQLite transaction lifecycle via callbacks:

```c
// Registered during extension initialization:
sqlite3_commit_hook(db, cloudsync_commit_hook, ctx);
sqlite3_rollback_hook(db, cloudsync_rollback_hook, ctx);
```

**On commit**: Increment global db_version and seq counters
**On rollback**: Discard any metadata written during failed transaction

**Why important**: Maintains consistency between user data and CRDT metadata without user intervention.

### 6. Virtual Table Interface Pattern

Implements SQLite's virtual table mechanism (`src/sqlite/cloudsync_changes_sqlite.c`) for queryable sync state:

```sql
-- No actual 'cloudsync_changes' table exists—it's virtual
SELECT tbl, pk, colname, colvalue FROM cloudsync_changes
WHERE tbl='users' AND db_version > 100;
```

**Implementation**:
- `xConnect/xDisconnect` - setup/teardown
- `xBestIndex` - query optimization hints
- `xFilter` - execute query over metadata tables
- Results generated on-demand, no storage

**Benefit**: Standard SQL interface to sync internals for debugging and monitoring.

### 7. Binary Payload Serialization Pattern

Custom wire format in `pk.c` optimized for SQLite data types:

```
[num_cols:1 byte][type+len:1 byte][value:N bytes][type+len:1 byte][value:N bytes]...
```

**Features**:
- Platform-independent endianness handling (htonl/ntohl for network byte order)
- Variable-length encoding (only bytes needed)
- Type-aware (knows SQLite INTEGER/FLOAT/TEXT/BLOB/NULL)
- LZ4 compression applied to entire payload

**Why custom format?** More efficient than JSON/protobuf for SQLite's type system; minimizes network bandwidth.

### 8. Context/Handle Pattern

Encapsulated state management with opaque pointers:

```c
cloudsync_context                 // Per-database state
  ├─ site_id                      // This database's UUID
  ├─ db_version, seq              // Global counters
  ├─ insync flag                  // Transaction state
  └─ cloudsync_table_context[]    // Array of synced tables
       ├─ table_name
       ├─ algo (CLS/GOS/DWS/AWS)
       ├─ column metadata
       └─ prepared statements
```

**Benefits**:
- Multiple databases can have independent sync contexts
- Clean lifecycle: `cloudsync_context_create()` → `cloudsync_context_init()` → `cloudsync_context_free()`
- Opaque pointers (`void *`) hide implementation details
- State passed through SQLite's `sqlite3_user_data()` mechanism

### 9. Layered Architecture

Clear separation of concerns from bottom to top:

```
┌──────────────────────────────────────┐
│  SQL Functions (Public API)          │  src/sqlite/cloudsync_sqlite.c
│  - cloudsync_init()                  │  - Registers all SQL functions
│  - cloudsync_uuid()                  │  - Entry point for users
│  - cloudsync_network_sync()          │
├──────────────────────────────────────┤
│  Network Layer (Optional)            │  src/network.c/h
│  - SQLite Cloud communication        │  - Uses libcurl or native APIs
│  - Retry logic, authentication       │  - Can be omitted (CLOUDSYNC_OMIT_NETWORK)
├──────────────────────────────────────┤
│  CRDT Core / Merge Logic             │  src/cloudsync.c/h
│  - Payload generation/application    │  - Database-agnostic
│  - Vector clock comparison           │  - Core sync algorithms
│  - Conflict resolution               │
├──────────────────────────────────────┤
│  Database Utilities                  │  src/dbutils.c, src/utils.c
│  - Metadata table management         │  - Helper functions
│  - Trigger creation                  │  - UUID generation
│  - Schema validation                 │  - Hashing, encoding
├──────────────────────────────────────┤
│  Database Abstraction Layer          │  src/database.h
│  - Generic DB operations             │  src/sqlite/database_sqlite.c
│  - Prepared statements               │  src/postgresql/database_postgresql.c
│  - Memory allocation                 │
├──────────────────────────────────────┤
│  Database Engine (SQLite/PostgreSQL) │
└──────────────────────────────────────┘
```

**Key insight**: CRDT logic in `cloudsync.c` never calls SQLite directly—only uses `database.h` abstractions. This enables potential PostgreSQL support.

### 10. Platform Abstraction Pattern

Conditional compilation for platform-specific features:

```c
// Detect platform (utils.h)
#if defined(_WIN32) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    #define CLOUDSYNC_DESKTOP_OS 1
#elif defined(__APPLE__) && TARGET_OS_OSX
    #define CLOUDSYNC_DESKTOP_OS 1
#elif defined(__linux__) && !defined(__ANDROID__)
    #define CLOUDSYNC_DESKTOP_OS 1
#endif

// Enable features conditionally
#ifdef CLOUDSYNC_DESKTOP_OS
  // File I/O helpers available
  bool cloudsync_file_write(const char *path, ...);
#endif

#ifdef NATIVE_NETWORK
  // Use NSURLSession on macOS instead of libcurl
#endif
```

**Build system** (`Makefile`):
- Auto-detects platform
- Compiles only needed code (no file I/O on mobile)
- Links platform-specific libraries (Security.framework on macOS)

## Key Design Principles

1. **Non-invasive**: User tables unchanged; sync metadata stored separately
2. **Declarative**: Triggers + CRDT = automatic synchronization
3. **Self-contained**: Statically links dependencies (curl); single .so/.dylib file
4. **Extensible**: Multiple CRDT algorithms, virtual tables, custom SQL functions
5. **Efficient**: Binary payloads, column-level tracking, minimal metadata overhead
6. **Portable**: Compiles for Linux/macOS/Windows/Android/iOS/WASM with same codebase

## Performance Considerations

### Hot-Path vs. Cold-Path SQL

The extension distinguishes between performance-critical and initialization code:

**Hot-path operations** (executed on every user write or during merge):
- **MUST use pre-prepared statements** stored in the context
- Triggers fire on every INSERT/UPDATE/DELETE
- CRDT merge logic processes every incoming change
- SQL compilation overhead is unacceptable here

**Examples of hot-path code:**
- Trigger bodies that insert into `{table}_cloudsync`
- `merge_insert()` and `merge_insert_col()` in `cloudsync.c`
- Queries in `cloudsync_payload_apply()` that check/update metadata
- Any code path executed within `cloudsync_commit_hook()`

**Implementation pattern:**
```c
// Prepared statements stored in cloudsync_table_context:
typedef struct cloudsync_table_context {
    // ... other fields ...
    sqlite3_stmt *insert_meta_stmt;      // Pre-compiled
    sqlite3_stmt *update_sentinel_stmt;  // Pre-compiled
    sqlite3_stmt *check_pk_stmt;         // Pre-compiled
} cloudsync_table_context;

// Used in hot-path without recompilation:
int rc = sqlite3_bind_text(table->insert_meta_stmt, 1, pk, pklen, SQLITE_STATIC);
rc = sqlite3_step(table->insert_meta_stmt);
sqlite3_reset(table->insert_meta_stmt);
```

**Cold-path operations** (initialization, setup, infrequent operations):
- Can use runtime-compiled SQL via `sqlite3_exec()` or one-off `sqlite3_prepare_v2()`
- Executed once per table initialization or configuration change
- Performance is not critical

**Examples of cold-path code:**
- `cloudsync_init_table()` - creates metadata tables and triggers
- `dbutils_settings_init()` - sets up global configuration
- Schema validation in `dbutils_table_sanity_check()`
- `cloudsync_cleanup()` - drops metadata tables

**Implementation pattern:**
```c
// OK for initialization code:
char *sql = sqlite3_mprintf("CREATE TABLE IF NOT EXISTS %s_cloudsync (...)", table_name);
int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
sqlite3_free(sql);
```

### Why This Matters

1. **Trigger overhead**: Triggers execute on every user operation. Compiling SQL on each trigger execution would make writes unacceptably slow.

2. **Merge performance**: `cloudsync_payload_apply()` may process thousands of changes in a single sync. SQL compilation would dominate runtime.

3. **Memory efficiency**: Prepared statements are parsed once, reused many times, and cleaned up when the context is freed.

### Finding Prepared Statements in the Code

- Prepared statements initialized in `cloudsync_init_table()` and stored in `cloudsync_table_context`
- Look for `sqlite3_stmt *` fields in context structures
- Lifetime: created during table init, reset after each use, finalized when context freed
- See `cloudsync.c` and `dbutils.c` for examples

## Testing Strategy

Tests are in `test/unit.c`. The test framework:
- Uses in-memory SQLite databases
- Tests core CRDT operations (insert, update, delete, merge)
- Validates multi-device sync scenarios
- Checks payload serialization/deserialization
- Compiled with `-DCLOUDSYNC_UNITTEST` flag

To add tests:
1. Add test function in `test/unit.c`
2. Call from `main()`
3. Run `make test` to execute

## Important Constraints

### Primary Key Requirements

Tables must use TEXT primary keys with globally unique identifiers:
- Use `cloudsync_uuid()` for UUID generation
- Integer auto-increment PKs cause conflicts across devices
- All PK columns must be `NOT NULL`

### Column Constraints

For CRDT merge to work correctly:
- All `NOT NULL` columns (except PKs) must have `DEFAULT` values
- This ensures column-by-column merge doesn't violate constraints

### Triggers and Foreign Keys

- Foreign key constraints may conflict with CRDT merge (see README for details)
- Triggers on synced tables may execute multiple times during merge
- Test thoroughly when using FKs or triggers with synced tables

## Code Style Notes

- Error handling via return codes (SQLITE_OK, SQLITE_ERROR, etc.)
- Memory allocation through abstraction layer (`cloudsync_memory_*` macros)
- Debug macros throughout (disabled by default): `DEBUG_FUNCTION`, `DEBUG_SQL`, etc.
- Hash tables via khash.h (header-only library)
- Compression via LZ4 for payloads
- Comments and documentation must be written in English unless explicitly asked otherwise, even if the prompt is in another language.
- Table names to augment are limited to 512 characters; size buffer allocations for SQL strings accordingly.
- Prefer static buffer allocation with `sqlite3_snprintf` for SQL string construction when practical (e.g., fixed pattern plus table name with a 1024-byte buffer) instead of dynamic `sqlite3_mprintf` to reduce allocations and cleanup.
- SQL statements:
  - Parameterless SQL should live as global constants in `src/<database>/database_<engine>.c` (e.g., `const char *SQL_CREATE_SETTINGS = "CREATE TABLE ...";` in `src/sqlite/database_sqlite.c`) and be used via `extern const char *SQL_CREATE_SETTINGS;` so database backends can override as needed.
  - Parameterized SQL must be provided via functions in the database layer (as with `database_count_pk`) so each backend can build statements appropriately.
  - Put backend-specific SQL templates in `src/<database>/sql_<engine>.c`; add a `database_<engine>.c` helper (exposed in `database.h`) whenever placeholder rules, quoting/escaping, or catalog-driven SQL generation differ between backends.
- Preserve existing coding style and patterns (e.g., prepared statements with bind/step/reset, use `cloudsync_memory_*` macros, return SQLite error codes). Ask the user before significant structural changes or refactors.

## PostgreSQL Database Backend Patterns

- SPI usage: prefer `SPI_execute()` for one-shot catalog queries and `SPI_prepare` + `SPI_execute_plan` for reusable statements.
- Error handling: wrap SPI calls in `PG_TRY()/PG_CATCH()`, capture with `CopyErrorData()`, call `cloudsync_set_error(...)`, and `FlushErrorState()`; helpers should not rethrow.
- Statement lifecycle: `databasevm_prepare/step/reset/finalize` owns a `pg_stmt_t` with `stmt_mcxt`, plus `bind_mcxt` and `row_mcxt` subcontexts; reset uses `MemoryContextReset` (not free).
- Cursor strategy: use portals (`SPI_cursor_open`/`SPI_cursor_fetch`) only for cursorable plans (check `SPI_is_cursor_plan`); non-cursorable plans execute once.
- Binding: bind arrays (`values`, `nulls`, `types`) live in `bind_mcxt` and are cleared in `databasevm_clear_bindings`.
- Row access: extract values via `SPI_getbinval` with OID checks, convert to C types, and copy into cloudsync-managed buffers.
- SQL construction: prefer `snprintf` into fixed buffers, fall back to `cloudsync_memory_mprintf` for dynamic sizes.
- SPI context: helpers assume the caller has already executed `SPI_connect()`; they avoid managing SPI connection state.
