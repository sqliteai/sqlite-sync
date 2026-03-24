# SQLite Sync

[![sqlite-sync coverage](https://img.shields.io/badge/dynamic/regex?url=https%3A%2F%2Fsqliteai.github.io%2Fsqlite-sync%2F&search=Functions%3A%3C%5C%2Ftd%3E%5Cs*%3Ctd%20class%3D%22headerCovTableEntry(?:Hi|Med|Lo)%22%3E(%5B%5Cd.%5D%2B)%26nbsp%3B%25&replace=%241%25&label=coverage&labelColor=rgb(85%2C%2085%2C%2085)%3B&color=rgb(167%2C%20252%2C%20157)%3B&link=https%3A%2F%2Fsqliteai.github.io%2Fsqlite-sync%2F)](https://sqliteai.github.io/sqlite-sync/)

**SQLite Sync** is a multi-platform extension that brings a true **local-first experience** to your applications with minimal effort. It extends standard SQLite tables with built-in support for offline work and automatic synchronization, allowing multiple devices to operate independently—even without a network connection—and seamlessly stay in sync. With SQLite Sync, developers can easily build **distributed, collaborative applications** while continuing to rely on the **simplicity, reliability, and performance of SQLite**.

Under the hood, SQLite Sync uses advanced **CRDT (Conflict-free Replicated Data Type)** algorithms and data structures designed specifically for **collaborative, distributed systems**. This means:

- Devices can update data independently, even without a network connection.
- When they reconnect, all changes are **merged automatically and without conflicts**.
- **No data loss. No overwrites. No manual conflict resolution.**

In simple terms, CRDTs make it possible for multiple users to **edit shared data at the same time**, from anywhere, and everything just works.

## Table of Contents
- [Key Features](#key-features)
- [Built-in Network Layer](#built-in-network-layer)
- [Row-Level Security](#row-level-security)
- [Block-Level LWW](#block-level-lww)
- [What Can You Build with SQLite Sync?](#what-can-you-build-with-sqlite-sync)
- [Documentation](#documentation)
- [Installation](#installation)
- [Getting Started](#getting-started)
- [Block-Level LWW Example](#block-level-lww-example)
- [Database Schema Recommendations](#database-schema-recommendations)
  - [Primary Key Requirements](#primary-key-requirements)
  - [Column Constraint Guidelines](#column-constraint-guidelines)
  - [UNIQUE Constraint Considerations](#unique-constraint-considerations)
  - [Foreign Key Compatibility](#foreign-key-compatibility)
  - [Trigger Compatibility](#trigger-compatibility)
- [License](#license)

## Key Features

- **Offline-First by Design**: Works seamlessly even when devices are offline. Changes are queued locally and synced automatically when connectivity is restored.
- **CRDT-Based Conflict Resolution**: Merges updates deterministically and efficiently, ensuring eventual consistency across all replicas without the need for complex merge logic.
- **Block-Level LWW for Text**: Fine-grained conflict resolution for text columns. Instead of overwriting the entire cell, changes are tracked and merged at the line (or paragraph) level, so concurrent edits to different parts of the same text are preserved.
- **Embedded Network Layer**: No external libraries or sync servers required. SQLiteSync handles connection setup, message encoding, retries, and state reconciliation internally.
- **Drop-in Simplicity**: Just load the extension into SQLite and start syncing. No need to implement custom protocols or state machines.
- **Efficient and Resilient**: Optimized binary encoding, automatic batching, and robust retry logic make synchronization fast and reliable even on flaky networks.

Whether you're building a mobile app, IoT device, or desktop tool, SQLite Sync simplifies distributed data management and unlocks the full potential of SQLite in decentralized environments.

## Built-in Network Layer

Unlike traditional sync systems that require you to build and maintain a complex backend, **SQLite Sync includes a built-in network layer** that works out of the box:

- Sync your database with the cloud using **a single function call**.
- Compatible with **any language or framework** that supports SQLite.
- **No backend setup required** — SQLite Sync handles networking, change tracking, and conflict resolution for you.

The sync layer is tightly integrated with [**SQLite Cloud**](https://sqlitecloud.io/), enabling seamless and secure data sharing across devices, users, and platforms. You get the power of cloud sync without the complexity.

## Row-Level Security

Thanks to the underlying SQLite Cloud infrastructure, **SQLite Sync supports Row-Level Security (RLS)**—allowing you to use a **single shared cloud database** while each client only sees and modifies its own data. RLS policies are enforced on the server, so the security boundary is at the database level, not in application code.

- Control not just who can read or write a table, but **which specific rows** they can access.
- Each device syncs only the rows it is authorized to see—no full dataset download, no client-side filtering.

For example:

- User A can only see and edit their own data.
- User B can access a different set of rows—even within the same shared table.

**Benefits**:

- **Single database, multiple tenants**: One cloud database serves all users. RLS policies partition data per user or role, eliminating the need to provision separate databases.
- **Efficient sync**: Each client downloads only its authorized rows, reducing bandwidth and local storage.
- **Server-enforced security**: Policies are evaluated on the server during sync. A compromised or modified client cannot bypass access controls.
- **Simplified development**: No need to implement permission logic in your application—define policies once in the database and they apply everywhere.

For more information, see the [SQLite Cloud RLS documentation](https://docs.sqlitecloud.io/docs/rls).

## Block-Level LWW

Standard CRDT sync resolves conflicts at the **cell level**: if two devices edit the same column of the same row, one value wins entirely. This works well for short values like names or statuses, but for longer text content — documents, notes, descriptions — it means the entire text is replaced even if the edits were in different parts.

**Block-Level LWW** (Last-Writer-Wins) solves this by splitting text columns into **blocks** (lines by default) and tracking each block independently. When two devices edit different lines of the same text, **both edits are preserved** after sync. Only when two devices edit the *same* line does LWW conflict resolution apply.

### How It Works

1. **Enable block tracking** on a text column using `cloudsync_set_column()`.
2. On INSERT or UPDATE, SQLite Sync automatically splits the text into blocks using the configured delimiter (default: newline `\n`).
3. Each block gets a unique fractional index position, enabling insertions between existing blocks without reindexing.
4. During sync, changes are merged block-by-block rather than replacing the whole cell.
5. Use `cloudsync_text_materialize()` to reconstruct the full text from blocks on demand, or read the column directly (it is updated automatically after merge).

### Key Properties

- **Non-conflicting edits are preserved**: Two users editing different lines of the same document both see their changes after sync.
- **Same-line conflicts use LWW**: If two users edit the same line, the last writer wins — consistent with standard CRDT behavior.
- **Custom delimiters**: Use paragraph separators (`\n\n`), sentence boundaries, or any string as the block delimiter.
- **Mixed columns**: A table can have both regular LWW columns and block-level LWW columns side by side.
- **Transparent reads**: The base column always contains the current full text. Block tracking is an internal mechanism; your queries work unchanged.

For setup instructions and a complete example, see [Block-Level LWW Example](#block-level-lww-example). For API details, see the [API Reference](./API.md).

### What Can You Build with SQLite Sync?

SQLite Sync is ideal for building collaborative and distributed apps across web, mobile, desktop, and edge platforms. Some example use cases include:

#### 📋 Productivity & Collaboration

- **Shared To-Do Lists**: Users independently update tasks and sync effortlessly.
- **Note-Taking Apps**: Real-time collaboration with offline editing.
- **Markdown Editors**: Work offline, sync when back online—no conflicts.

#### 📱 Mobile & Edge

- **Field Data Collection**: For remote inspections, agriculture, or surveys.
- **Point-of-Sale Systems**: Offline-first retail solutions with synced inventory.
- **Health & Fitness Apps**: Sync data across devices with strong privacy controls.

#### 🏢 Enterprise Workflows

- **CRM Systems**: Sync leads and clients per user with row-level access control.
- **Project Management Tools**: Offline-friendly planning and task management.
- **Expense Trackers**: Sync team expenses securely and automatically.

#### 🧠 Personal Apps

- **Journaling & Diaries**: Private, encrypted entries that sync across devices.
- **Bookmarks & Reading Lists**: Personal or collaborative content management.
- **Habit Trackers**: Sync progress with data security and consistency.

#### 🌍 Multi-User, Multi-Tenant Systems

- **SaaS Platforms**: Row-level access for each user or team.
- **Collaborative Design Tools**: Merge visual edits and annotations offline.
- **Educational Apps**: Shared learning content with per-student access controls.

## Documentation

For detailed information on all available functions, their parameters, and examples, refer to the [comprehensive API Reference](./API.md). The API includes:

- **Configuration Functions** — initialize, enable, and disable sync on tables
- **Block-Level LWW Functions** — configure block tracking on text columns and materialize text from blocks
- **Helper Functions** — version info, site IDs, UUID generation
- **Schema Alteration Functions** — safely alter synced tables
- **Network Functions** — connect, authenticate, send/receive changes, and monitor sync status

## Installation

### Pre-built Binaries

Download the appropriate pre-built binary for your platform from the official [Releases](https://github.com/sqliteai/sqlite-sync/releases) page:

- Linux: x86 and ARM
- macOS: x86 and ARM
- Windows: x86
- Android
- iOS

### Loading the Extension

```sql
-- In SQLite CLI
.load ./cloudsync

-- In SQL
SELECT load_extension('./cloudsync');
```

### WASM Version

You can download the WebAssembly (WASM) version of SQLite with the SQLite Sync extension enabled from: https://www.npmjs.com/package/@sqliteai/sqlite-wasm

### Swift Package

You can [add this repository as a package dependency to your Swift project](https://developer.apple.com/documentation/xcode/adding-package-dependencies-to-your-app#Add-a-package-dependency). After adding the package, you'll need to set up SQLite with extension loading by following steps 4 and 5 of [this guide](https://github.com/sqliteai/sqlite-extensions-guide/blob/main/platforms/ios.md#4-set-up-sqlite-with-extension-loading).

Here's an example of how to use the package:
```swift
import CloudSync

...

var db: OpaquePointer?
sqlite3_open(":memory:", &db)
sqlite3_enable_load_extension(db, 1)
var errMsg: UnsafeMutablePointer<Int8>? = nil
sqlite3_load_extension(db, CloudSync.path, nil, &errMsg)
var stmt: OpaquePointer?
sqlite3_prepare_v2(db, "SELECT cloudsync_version()", -1, &stmt, nil)
defer { sqlite3_finalize(stmt) }
sqlite3_step(stmt)
log("cloudsync_version(): \(String(cString: sqlite3_column_text(stmt, 0)))")
sqlite3_close(db)
```

### Android Package

Add the [following](https://central.sonatype.com/artifact/ai.sqlite/sync) to your Gradle dependencies:

```gradle
implementation 'ai.sqlite:sync:1.0.0'
```

Here's an example of how to use the package:
```java
SQLiteCustomExtension cloudsyncExtension = new SQLiteCustomExtension(getApplicationInfo().nativeLibraryDir + "/cloudsync", null);
SQLiteDatabaseConfiguration config = new SQLiteDatabaseConfiguration(
    getCacheDir().getPath() + "/cloudsync_test.db",
    SQLiteDatabase.CREATE_IF_NECESSARY | SQLiteDatabase.OPEN_READWRITE,
    Collections.emptyList(),
    Collections.emptyList(),
    Collections.singletonList(cloudsyncExtension)
);
SQLiteDatabase db = SQLiteDatabase.openDatabase(config, null, null);
```

**Note:** Additional settings and configuration are required for a complete setup. For full implementation details, see the [complete Android example](https://github.com/sqliteai/sqlite-extensions-guide/blob/main/examples/android/README.md).

### Expo

Install the Expo package:

```bash
npm install @sqliteai/sqlite-sync-expo
```

Add to your `app.json`:

```json
{
  "expo": {
    "plugins": ["@sqliteai/sqlite-sync-expo"]
  }
}
```

Run prebuild:

```bash
npx expo prebuild --clean
```

Load the extension:

```typescript
import { Platform } from 'react-native';
import { getDylibPath, open } from '@op-engineering/op-sqlite';

const db = open({ name: 'mydb.db' });

// Load SQLite Sync extension
if (Platform.OS === 'ios') {
  const path = getDylibPath('ai.sqlite.cloudsync', 'CloudSync');
  db.loadExtension(path);
} else {
  db.loadExtension('cloudsync');
}
```

### React Native

Install the React Native library:

```bash
npm install @sqliteai/sqlite-sync-react-native
```

Then follow the instructions from the [README](https://www.npmjs.com/package/@sqliteai/sqlite-sync-react-native)

### Flutter Package

Add the [sqlite_sync](https://pub.dev/packages/sqlite_sync) package to your project:

```bash
flutter pub add sqlite_sync  # Flutter projects
dart pub add sqlite_sync     # Dart projects
```

Usage with `sqlite3` package:
```dart
import 'package:sqlite3/sqlite3.dart';
import 'package:sqlite_sync/sqlite_sync.dart';

sqlite3.loadSqliteSyncExtension();
final db = sqlite3.openInMemory();
print(db.select('SELECT cloudsync_version()'));
```

For a complete example, see the [Flutter example](https://github.com/sqliteai/sqlite-extensions-guide/blob/main/examples/flutter/README.md).

## Getting Started

Here's a quick example to get started with SQLite Sync:

### Prerequisites

1. **SQLite Cloud Account**: Sign up at [SQLite Cloud](https://sqlitecloud.io/)
2. **SQLite Sync Extension**: Download from [Releases](https://github.com/sqliteai/sqlite-sync/releases)

### SQLite Cloud Setup

1. Create a new project and database in your [SQLite Cloud Dashboard](https://dashboard.sqlitecloud.io/)
2. Copy your connection string and API key from the dashboard
3. Create tables with identical schema in both local and cloud databases
4. Enable synchronization: click the **"OffSync"** button for your database and select each table you want to synchronize 

### Local Database Setup

```bash
# Start SQLite CLI
sqlite3 myapp.db
```

```sql
-- Load the extension
.load ./cloudsync

-- Create a table (primary key MUST be TEXT for global uniqueness)
CREATE TABLE IF NOT EXISTS my_data (
    id TEXT PRIMARY KEY,
    value TEXT NOT NULL DEFAULT '',
    created_at TEXT DEFAULT CURRENT_TIMESTAMP
);

-- Initialize table for synchronization
SELECT cloudsync_init('my_data');

-- Use your local database normally: read and write data using standard SQL queries
-- The CRDT system automatically tracks all changes for synchronization

-- Example: Insert data (always use cloudsync_uuid() for globally unique IDs)
INSERT INTO my_data (id, value) VALUES 
    (cloudsync_uuid(), 'Hello from device A!'),
    (cloudsync_uuid(), 'Working offline is seamless!');

-- Example: Update and delete operations work normally
UPDATE my_data SET value = 'Updated: Hello from device A!' WHERE value LIKE 'Hello from device A!';

-- View your data
SELECT * FROM my_data ORDER BY created_at;

-- Configure network connection before using the network sync functions
-- The managedDatabaseId is obtained from the OffSync page on the SQLiteCloud dashboard
SELECT cloudsync_network_init('your-managed-database-id');
SELECT cloudsync_network_set_apikey('your-api-key-here');
-- Or use token authentication (required for Row-Level Security)
-- SELECT cloudsync_network_set_token('your_auth_token');

-- Sync with cloud: send local changes, then check the remote server for new changes
-- and, if a package with changes is ready to be downloaded, applies them to the local database
SELECT cloudsync_network_sync();
-- Returns a JSON string with sync status, e.g.:
-- '{"send":{"status":"synced","localVersion":5,"serverVersion":5},"receive":{"rows":3,"tables":["my_data"]}}'
-- Keep calling periodically. In production applications, you would typically
-- call this periodically rather than manually (e.g., every few seconds)
SELECT cloudsync_network_sync();

-- Before closing the database connection
SELECT cloudsync_terminate();
-- Close the database connection
.quit
```
```sql
-- On another device (or create another database for testing: sqlite3 myapp_2.db)
-- Follow the same setup steps: load extension, create table, init sync, configure network

-- Load extension and create identical table structure
.load ./cloudsync
CREATE TABLE IF NOT EXISTS my_data (
    id TEXT PRIMARY KEY,
    value TEXT NOT NULL DEFAULT '',
    created_at TEXT DEFAULT CURRENT_TIMESTAMP
);
SELECT cloudsync_init('my_data');

-- Connect to the same cloud database
SELECT cloudsync_network_init('sqlitecloud://your-project-id.sqlite.cloud/database.sqlite');
SELECT cloudsync_network_set_apikey('your-api-key-here');

-- Sync to get data from the first device
SELECT cloudsync_network_sync();
-- Repeat — check receive.rows in the JSON result to see if data was received
SELECT cloudsync_network_sync();

-- View synchronized data
SELECT * FROM my_data ORDER BY created_at;

-- Add data from this device to test bidirectional sync
INSERT INTO my_data (id, value) VALUES 
    (cloudsync_uuid(), 'Hello from device B!');

-- Sync again to send this device's changes
SELECT cloudsync_network_sync();

-- The CRDT system ensures all devices eventually have the same data,
-- with automatic conflict resolution and no data loss

-- Before closing the database connection
SELECT cloudsync_terminate();
-- Close the database connection
.quit
```

### For a Complete Example

See the [examples](./examples/simple-todo-db/) directory for a comprehensive walkthrough including:
- Multi-device collaboration
- Offline scenarios
- Row-level security setup
- Conflict resolution demonstrations

## Block-Level LWW Example

This example shows how to enable block-level text sync on a notes table, so that concurrent edits to different lines are merged instead of overwritten.

### Setup

```sql
-- Load the extension
.load ./cloudsync

-- Create a table with a text column for long-form content
CREATE TABLE notes (
    id TEXT PRIMARY KEY NOT NULL,
    title TEXT NOT NULL DEFAULT '',
    body TEXT NOT NULL DEFAULT ''
);

-- Initialize sync on the table
SELECT cloudsync_init('notes');

-- Enable block-level LWW on the "body" column
SELECT cloudsync_set_column('notes', 'body', 'algo', 'block');
```

After this setup, every INSERT or UPDATE to the `body` column automatically splits the text into blocks (one per line) and tracks each block independently.

### Two-Device Scenario

```sql
-- Device A: create a note
INSERT INTO notes (id, title, body) VALUES (
    'note-001',
    'Meeting Notes',
    'Line 1: Welcome
Line 2: Agenda
Line 3: Action items'
);

-- Sync Device A -> Cloud -> Device B
-- (Both devices now have the same 3-line note)
```

```sql
-- Device A (offline): edit line 1
UPDATE notes SET body = 'Line 1: Welcome everyone
Line 2: Agenda
Line 3: Action items' WHERE id = 'note-001';

-- Device B (offline): edit line 3
UPDATE notes SET body = 'Line 1: Welcome
Line 2: Agenda
Line 3: Action items - DONE' WHERE id = 'note-001';
```

```sql
-- After both devices sync, the merged result is:
-- 'Line 1: Welcome everyone
--  Line 2: Agenda
--  Line 3: Action items - DONE'
--
-- Both edits are preserved because they affected different lines.
```

### Custom Delimiter

For paragraph-level tracking (useful for long-form documents), set a custom delimiter:

```sql
-- Use double newline as delimiter (paragraph separator)
SELECT cloudsync_set_column('notes', 'body', 'delimiter', '

');
```

### Materializing Text

After a merge, the `body` column contains the reconstructed text automatically. You can also manually trigger materialization:

```sql
-- Reconstruct body from blocks for a specific row
SELECT cloudsync_text_materialize('notes', 'body', 'note-001');

-- Then read normally
SELECT body FROM notes WHERE id = 'note-001';
```

### Mixed Columns

Block-level LWW can be enabled on specific columns while other columns use standard cell-level LWW:

```sql
CREATE TABLE docs (
    id TEXT PRIMARY KEY NOT NULL,
    title TEXT NOT NULL DEFAULT '',    -- standard LWW (cell-level)
    body TEXT NOT NULL DEFAULT '',     -- block LWW (line-level)
    status TEXT NOT NULL DEFAULT ''    -- standard LWW (cell-level)
);

SELECT cloudsync_init('docs');
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');

-- Now: concurrent edits to "title" or "status" use normal LWW,
-- while concurrent edits to "body" merge at the line level.
```

## 📦 Integrations

Use SQLite-AI alongside:

* **[SQLite-AI](https://github.com/sqliteai/sqlite-ai)** – on-device inference, embedding generation, and model interaction directly into your database
* **[SQLite-Vector](https://github.com/sqliteai/sqlite-vector)** – vector search from SQL
* **[SQLite-JS](https://github.com/sqliteai/sqlite-js)** – define SQLite functions in JavaScript

## Database Schema Recommendations

When designing your database schema for SQLite Sync, follow these best practices to ensure optimal CRDT performance and conflict resolution:

### Primary Key Requirements

- **Use globally unique identifiers**: Always use TEXT primary keys with UUIDs, ULIDs, or similar globally unique identifiers
- **Avoid auto-incrementing integers**: Integer primary keys can cause conflicts across multiple devices
- **Use `cloudsync_uuid()`**: The built-in function generates UUIDv7 identifiers optimized for distributed systems
- **Note:** Any write operation that includes a NULL value for a primary key column will be rejected with an error, even if SQLite would normally allow it due to a legacy behavior.

```sql
-- ✅ Recommended: Globally unique TEXT primary key
CREATE TABLE users (
    id TEXT PRIMARY KEY,                    -- Use cloudsync_uuid()
    name TEXT NOT NULL,
    email TEXT UNIQUE NOT NULL
);

-- ❌ Avoid: Auto-incrementing integer primary key
CREATE TABLE users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,  -- Causes conflicts
    name TEXT NOT NULL,
    email TEXT UNIQUE NOT NULL
);
```

### Column Constraint Guidelines

- **Provide DEFAULT values**: All `NOT NULL` columns (except primary keys) must have `DEFAULT` values
- **Consider nullable columns**: For optional data, use nullable columns instead of empty strings

```sql
-- ✅ Recommended: Proper constraints and defaults
CREATE TABLE tasks (
    id TEXT PRIMARY KEY,
    title TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL DEFAULT 'pending',
    priority INTEGER NOT NULL DEFAULT 1,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    assigned_to TEXT                       -- Nullable for optional assignment
);
```

### UNIQUE Constraint Considerations

When converting from single-tenant to multi-tenant database schemas with Row-Level Security, **UNIQUE constraints must be globally unique** across all tenants in the cloud database. For columns that should only be unique within a tenant, use composite UNIQUE constraints.

```sql
-- ❌ Single-tenant: Unique email per database
CREATE TABLE users (
    id TEXT PRIMARY KEY,
    email TEXT UNIQUE NOT NULL  -- Problem: Not unique across tenants
);

-- ✅ Multi-tenant: Composite unique constraint
CREATE TABLE users (
    id TEXT PRIMARY KEY,
    tenant_id TEXT NOT NULL,
    email TEXT NOT NULL,
    UNIQUE(tenant_id, email)    -- Unique email per tenant
);
```

### Foreign Key Compatibility

When using foreign key constraints with SQLite Sync, be aware that interactions with the CRDT merge algorithm and Row-Level Security policies may cause constraint violations. 

#### Potential Conflicts

**CRDT Merge Algorithm and DEFAULT Values**

- CRDT changes are applied column-by-column during synchronization
- Columns may be temporarily assigned DEFAULT values during the merge process
- If a foreign key column has a DEFAULT value, that value must exist in the referenced table

**Row-Level Security and CASCADE Actions**
- RLS policies may block operations required for maintaining referential integrity
- CASCADE DELETE/UPDATE operations may fail if RLS prevents access to related rows

#### Recommendations

**Database Design Patterns**
- Prefer application-level cascade logic over database-level CASCADE actions
- Design RLS policies to accommodate referential integrity operations
- Use nullable foreign keys where appropriate to avoid DEFAULT value issues
- Alternatively, ensure DEFAULT values for foreign key columns exist in their referenced tables

**Testing and Validation**
- Test synchronization scenarios with foreign key constraints enabled
- Monitor for constraint violations during sync operations in development

### Trigger Compatibility

Be aware that certain types of triggers can cause errors during synchronization due to SQLite Sync's merge logic.

**Duplicate Operations**
- If a trigger modifies a table that is also synchronized with SQLite Sync, changes performed by the trigger may be applied twice during the merge operation
- This can lead to constraint violations or unexpected data states depending on the table's constraints

**Column-by-Column Processing**
- SQLite Sync applies changes column-by-column during synchronization
- UPDATE triggers may be called multiple times for a single row as each column is processed
- This can result in unexpected trigger behavior

---

## License

This project is licensed under the [Elastic License 2.0](./LICENSE.md). You can use, copy, modify, and distribute it under the terms of the license for non-production use. For production or managed service use, please [contact SQLite Cloud, Inc](mailto:info@sqlitecloud.io) for a commercial license.

---

## Part of the SQLite AI Ecosystem

This project is part of the **SQLite AI** ecosystem, a collection of extensions that bring modern AI capabilities to the world’s most widely deployed database. The goal is to make SQLite the default data and inference engine for Edge AI applications.

Other projects in the ecosystem include:

- **[SQLite-AI](https://github.com/sqliteai/sqlite-ai)** — On-device inference and embedding generation directly inside SQLite.
- **[SQLite-Memory](https://github.com/sqliteai/sqlite-memory)** — Markdown-based AI agent memory with semantic search.
- **[SQLite-Vector](https://github.com/sqliteai/sqlite-vector)** — Ultra-efficient vector search for embeddings stored as BLOBs in standard SQLite tables.
- **[SQLite-Sync](https://github.com/sqliteai/sqlite-sync)** — Local-first CRDT-based synchronization for seamless, conflict-free data sync and real-time collaboration across devices.
- **[SQLite-Agent](https://github.com/sqliteai/sqlite-agent)** — Run autonomous AI agents directly from within SQLite databases.
- **[SQLite-MCP](https://github.com/sqliteai/sqlite-mcp)** — Connect SQLite databases to MCP servers and invoke their tools.
- **[SQLite-JS](https://github.com/sqliteai/sqlite-js)** — Create custom SQLite functions using JavaScript.
- **[Liteparser](https://github.com/sqliteai/liteparser)** — A highly efficient and fully compliant SQLite SQL parser.

Learn more at **[SQLite AI](https://sqlite.ai)**.
