# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## General Technical Documentation

For comprehensive technical information about the SQLite Sync architecture, build system, CRDT implementation, and design patterns, see [AGENTS.md](./AGENTS.md).

This file contains:
- Project overview and architecture
- Build commands and testing
- Core components and patterns
- Performance considerations
- Design principles and constraints

## Development Workflow

### Adding New SQL Functions

1. Implement in `src/sqlite/cloudsync_sqlite.c` (e.g., `cloudsync_xyz_func`)
2. Register in `cloudsync_register()` via `sqlite3_create_function()`
3. Document in `API.md`
4. Add tests in `test/unit.c`

### Modifying CRDT Logic

The merge algorithm lives in `cloudsync.c`:
- `merge_insert()` - Handles row-level merge decisions
- `merge_insert_col()` - Handles column-level merge decisions
- Algorithm-specific logic controlled by `table->algo` enum

**Performance requirement**: Merge code is hot-path (processes every incoming change during sync). Always use prepared statements stored in `cloudsync_table_context`. Never compile SQL at runtime in merge functions. See [AGENTS.md - Performance Considerations](./AGENTS.md#performance-considerations) for details.

### Schema Migrations

The extension tracks its schema version in `cloudsync_settings.schemaversion`. When the schema changes:
1. Increment version in migration code
2. Add migration logic in `dbutils_settings_init()`
3. Handle both fresh installs and upgrades

### Platform-Specific Code

- Most code is platform-agnostic C
- Platform detection via `CLOUDSYNC_DESKTOP_OS` macro (macOS, Linux desktop, Windows)
- Network layer can use native APIs (macOS NSURLSession) with `NATIVE_NETWORK` flag
- File I/O helpers (`cloudsync_file_*`) only available on desktop platforms

## Specialized Subagents

When working on specific areas of the codebase, you can launch specialized subagents with domain expertise.

### PostgreSQL Extension Agent

**Purpose**: Implement the PostgreSQL version of SQLite Sync extension

**Context**: The codebase has a database abstraction layer (`database.h`) with database-specific implementations in subdirectories. PostgreSQL-specific code lives in `src/postgresql/`. The goal is to create a fully functional PostgreSQL extension that implements the same CRDT sync logic.

**Launch command**:
```
Use the Task tool with prompt: "Implement the PostgreSQL backend for SQLite Sync. Study the database.h abstraction layer and src/sqlite/database_sqlite.c implementation, then implement src/postgresql/database_postgresql.c with full PostgreSQL support including prepared statements, value binding, and transaction hooks."
```

**Key files to study**:
- `src/database.h` - Abstract database API
- `src/sqlite/database_sqlite.c` - SQLite implementation (reference)
- `src/postgresql/database_postgresql.c` - PostgreSQL implementation
- `src/cloudsync.c` - Uses database abstraction (must work unchanged)

**Requirements**:
- Implement all functions in `database.h` using libpq (PostgreSQL C API)
- Maintain same semantics as SQLite version
- Handle PostgreSQL-specific data types mapping
- Test with PostgreSQL backend

**Testing approach**:
- Modify Makefile to link against libpq
- Create PostgreSQL-specific test suite
- Verify CRDT operations work identically to SQLite

### Other Potential Subagents

Consider creating specialized agents for:
- **WASM/Browser Agent**: Optimize for WebAssembly builds and OPFS storage
- **Network Protocol Agent**: Enhance sync protocol or add new backends
- **CRDT Algorithm Agent**: Implement new conflict resolution algorithms
- **Performance Optimization Agent**: Profile and optimize hot-path code

## Slash Commands

Custom slash commands help automate common development tasks in this repository.

### Available Commands

Create slash commands in `.claude/commands/` directory. Each command is a markdown file executed when invoked.

### Example: `/review-sync` - Review Sync Logic

**File**: `.claude/commands/review-sync.md`

```markdown
Please review the CRDT synchronization logic for correctness and performance:

1. Read and analyze the merge algorithm in `src/cloudsync.c`:
   - `merge_insert()` function
   - `merge_insert_col()` function
   - Vector clock comparison logic

2. Check for potential issues:
   - Race conditions in concurrent merges
   - Memory leaks in error paths
   - Inefficient SQL queries (should use prepared statements)
   - Incorrect handling of tombstones

3. Verify compliance with design principles from AGENTS.md:
   - Hot-path code uses prepared statements
   - No runtime SQL compilation in merge functions
   - Proper error handling

4. Suggest improvements with specific code examples

Please provide a summary of findings and recommendations.
```

**Usage**: Type `/review-sync` in Claude Code to trigger this review workflow.

### Example: `/test-crdt` - Test CRDT Algorithm

**File**: `.claude/commands/test-crdt.md`

```markdown
Create a comprehensive test scenario for CRDT conflict resolution:

1. Design a multi-device sync test with:
   - 3 devices making concurrent changes
   - Updates to same row, different columns
   - Updates to same column (conflict)
   - Deletions with concurrent updates

2. Generate test code in `test/unit.c` format

3. Show expected outcomes based on:
   - Vector clock values (db_version, seq, site_id)
   - CRDT algorithm (CLS/GOS/DWS/AWS)
   - Deterministic conflict resolution

4. Run the test and verify results

Focus on edge cases that could expose bugs in the merge algorithm.
```

### Creating New Slash Commands

To add a new command:

1. Create `.claude/commands/<command-name>.md`
2. Write the prompt describing the task
3. Use `/command-name` to invoke it

**Useful commands to create**:
- `/add-function` - Scaffold a new SQL function with tests
- `/optimize-query` - Analyze and optimize a SQL query
- `/check-leaks` - Review code for memory leaks
- `/cross-compile` - Build for all platforms and report issues
- `/benchmark-merge` - Profile merge performance

## Branch Information

Main branch: `main`
Current working branch: `database-api` - Database abstraction layer refactoring

**macOS Testing Note:** If the default `/usr/bin/sqlite3` doesn't support loading extensions, set the SQLITE3 variable when running tests (Adjust the version path if using a specific version like /opt/homebrew/Cellar/sqlite/3.50.4/bin/sqlite3:
```
make test SQLITE3=/opt/homebrew/bin/sqlite3
make unittest SQLITE3=/opt/homebrew/bin/sqlite3
```