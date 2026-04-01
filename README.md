# SQLite Sync

[![sqlite-sync coverage](https://img.shields.io/badge/dynamic/regex?url=https%3A%2F%2Fsqliteai.github.io%2Fsqlite-sync%2F&search=Functions%3A%3C%5C%2Ftd%3E%5Cs*%3Ctd%20class%3D%22headerCovTableEntry(?:Hi|Med|Lo)%22%3E(%5B%5Cd.%5D%2B)%26nbsp%3B%25&replace=%241%25&label=coverage&labelColor=rgb(85%2C%2085%2C%2085)%3B&color=rgb(167%2C%20252%2C%20157)%3B&link=https%3A%2F%2Fsqliteai.github.io%2Fsqlite-sync%2F)](https://sqliteai.github.io/sqlite-sync/)

**SQLite Sync** is a multi-platform extension that turns any SQLite database into a **conflict-free, offline-first replica** that syncs automatically with **[SQLite Cloud](https://sqlitecloud.io/)** nodes, **PostgreSQL** servers, and **Supabase** instances. One function call is all it takes: no backend to build, no sync protocol to implement.

Built on **CRDT** (Conflict-free Replicated Data Types), it guarantees:

- **No data loss.** Devices update independently, even offline, and all changes merge automatically.
- **No conflicts.** Deterministic merge, no manual conflict resolution, ever.
- **No extra infrastructure.** A globally distributed network of **CloudSync microservices** handles routing, packaging, and delivery of changes between SQLite and other DBMS nodes.

## Why SQLite Sync?

**For offline-first apps** (mobile, desktop, IoT, edge): devices work with a local SQLite database and sync when connectivity is available. Changes queue locally and merge seamlessly on reconnect.

**For AI agents**: agents that maintain memory, notes, or shared state in SQLite can sync across instances without coordination. **[Block-Level LWW](#block-level-lww)** was specifically designed to keep **markdown files** in sync: multiple agents editing different sections of the same document preserve all changes after sync.

## What Can You Build with SQLite Sync?

### Offline-First Apps
- **Shared To-Do Lists**: users independently update tasks and sync effortlessly.
- **Note-Taking Apps**: real-time collaboration with offline editing.
- **Field Data Collection**: for remote inspections, agriculture, or surveys.
- **Point-of-Sale Systems**: offline-first retail solutions with synced inventory.

### AI Agent Sync
- **Agent Memory**: multiple agents share and update a common SQLite database, syncing state across instances without coordination.
- **Markdown Knowledge Bases**: agents independently edit different sections of shared markdown documents, with Block-Level LWW preserving all changes.
- **Distributed Pipelines**: agents running on different nodes accumulate results locally and merge them into a single consistent dataset.

### Enterprise and Multi-Tenant
- **CRM Systems**: sync leads and clients per user with row-level access control.
- **SaaS Platforms**: row-level access for each user or team using a single shared database.
- **Project Management Tools**: offline-friendly planning and task management.

### Personal Apps
- **Journaling and Diaries**: private entries that sync across devices.
- **Habit Trackers**: sync progress with data security and consistency.
- **Bookmarks and Reading Lists**: personal or collaborative content management.

## Key Features

| Feature | Description |
|---------|-------------|
| **CRDT-based sync** | Causal-Length Set, Delete-Wins, Add-Wins, and Grow-Only Set algorithms |
| **Block-Level LWW** | Line-level merge for text/markdown columns, concurrent edits to different lines are preserved |
| **Built-in networking** | Embedded network layer (libcurl or native), single function call to sync |
| **Row-Level Security** | Server-enforced RLS: each client syncs only the rows it is authorized to see |
| **Multi-platform** | Linux, macOS, Windows, iOS, Android, WASM |

## Quick Start

### 1. Install

Download a pre-built binary from the [Releases](https://github.com/sqliteai/sqlite-sync/releases) page, or install a platform package (see [full installation guide](./docs/installation.md) for platform-specific code examples):

| Platform | Install |
|----------|---------|
| **SQLite CLI / C** | `.load ./cloudsync` or `SELECT load_extension('./cloudsync');` |
| **Swift** | [Add this repo as a Swift Package dependency](https://developer.apple.com/documentation/xcode/adding-package-dependencies-to-your-app), follow [steps 4 and 5](https://github.com/sqliteai/sqlite-extensions-guide/blob/main/platforms/ios.md#4-set-up-sqlite-with-extension-loading), and load extension with `CloudSync.path`  |
| **Android** | `implementation 'ai.sqlite:sync:1.0.0'` ([Maven Central](https://central.sonatype.com/artifact/ai.sqlite/sync)) |
| **Flutter** | `flutter pub add sqlite_sync` ([pub.dev](https://pub.dev/packages/sqlite_sync)) |
| **Expo** | `npm install @sqliteai/sqlite-sync-expo` |
| **React Native** | `npm install @sqliteai/sqlite-sync-react-native` |
| **WASM** | `npm install @sqliteai/sqlite-wasm` ([npm](https://www.npmjs.com/package/@sqliteai/sqlite-wasm)) |

### 2. Create a table and enable sync

```sql
.load ./cloudsync

CREATE TABLE tasks (
    id TEXT PRIMARY KEY,
    title TEXT NOT NULL DEFAULT '',
    done INTEGER NOT NULL DEFAULT 0
);

-- Enable CRDT sync on the table
SELECT cloudsync_init('tasks');
```

### 3. Use your database normally

```sql
INSERT INTO tasks (id, title) VALUES (cloudsync_uuid(), 'Buy groceries');
INSERT INTO tasks (id, title) VALUES (cloudsync_uuid(), 'Review PR #42');

UPDATE tasks SET done = 1 WHERE title = 'Buy groceries';

SELECT * FROM tasks;
```

### 4. Sync with the cloud

```sql
-- Connect to your SQLite Cloud managed database
-- (get the managed database ID from the OffSync page on the SQLite Cloud dashboard)
SELECT cloudsync_network_init('your-managed-database-id');
SELECT cloudsync_network_set_apikey('your-api-key');

-- Send local changes and receive remote changes
SELECT cloudsync_network_sync();
-- Returns JSON: {"send":{"status":"synced","localVersion":3,"serverVersion":3},"receive":{"rows":0,"tables":[]}}

-- Call periodically to stay in sync
SELECT cloudsync_network_sync();

-- Before closing the connection
SELECT cloudsync_terminate();
```

### 5. Sync from another device

On a second device (or a second database for testing), repeat the same setup:

```sql
-- Device B: load extension, create the same table, init sync
.load ./cloudsync

CREATE TABLE tasks (
    id TEXT PRIMARY KEY,
    title TEXT NOT NULL DEFAULT '',
    done INTEGER NOT NULL DEFAULT 0
);

SELECT cloudsync_init('tasks');

-- Connect to the same cloud database
SELECT cloudsync_network_init('your-managed-database-id');
SELECT cloudsync_network_set_apikey('your-api-key');

-- Pull changes from Device A
SELECT cloudsync_network_sync();
-- Call again: the first call triggers package preparation, the second downloads it
SELECT cloudsync_network_sync();

-- Device A's tasks are now here
SELECT * FROM tasks;

-- Add data from this device
INSERT INTO tasks (id, title) VALUES (cloudsync_uuid(), 'Call the dentist');

-- Send this device's changes to the cloud
SELECT cloudsync_network_sync();

-- Before closing the connection
SELECT cloudsync_terminate();
```

Back on Device A, calling `cloudsync_network_sync()` will pull Device B's changes. The CRDT engine ensures all devices converge to the same data, automatically, with no conflicts.

## Block-Level LWW

Standard CRDT sync replaces an entire cell when two devices edit the same column. **Block-Level LWW** splits text into lines and merges them independently, designed for keeping **markdown files and agent memory** in sync.

```sql
CREATE TABLE notes (
    id TEXT PRIMARY KEY NOT NULL,
    title TEXT NOT NULL DEFAULT '',
    body TEXT NOT NULL DEFAULT ''
);

SELECT cloudsync_init('notes');
SELECT cloudsync_set_column('notes', 'body', 'algo', 'block');
```

Now two agents (or devices) can edit different lines of the same note, and both edits are preserved after sync. See the full guide: **[Block-Level LWW Documentation](./docs/block-lww.md)**.

## Row-Level Security

With SQLite Cloud's RLS, a single shared cloud database serves all users while each client only sees and syncs its own rows. Policies are enforced server-side: a compromised client cannot bypass access controls.

- One database, multiple tenants, no per-user database provisioning.
- Each client syncs only authorized rows, minimal bandwidth and storage.

See the full guide: **[Row-Level Security Documentation](./docs/row-level-security.md)**.

## Documentation

- **[API Reference](./API.md)**: all functions, parameters, and examples
- **[Installation Guide](./docs/installation.md)**: platform-specific setup (Swift, Android, Expo, React Native, Flutter, WASM)
- **[Block-Level LWW Guide](./docs/block-lww.md)**: line-level text merge for markdown and documents
- **[Row-Level Security Guide](./docs/row-level-security.md)**: multi-tenant access control with server-enforced policies
- **[Database Schema Recommendations](./docs/schema.md)**: primary keys, constraints, foreign keys, triggers
- **[Custom Network Layer](./docs/internal/network.md)**: replace the built-in libcurl networking
- **[Examples](./examples/)**: complete walkthroughs (todo app, sport tracker, Swift multiplatform)

## SQLite Cloud Setup

1. Sign up at [SQLite Cloud](https://sqlitecloud.io/) and create a project.
2. Create a database and your tables in the [dashboard](https://dashboard.sqlitecloud.io/).
3. Enable synchronization: click **"OffSync"** for your database and select the tables to sync.
4. Copy the managed database ID and API key from the dashboard.
5. Use `cloudsync_network_init()` and `cloudsync_network_set_apikey()` locally, then call `cloudsync_network_sync()`.

For token-based authentication (required for RLS), use `cloudsync_network_set_token()` instead of `cloudsync_network_set_apikey()`.

## Integrations

Part of the **[SQLite AI](https://sqlite.ai)** ecosystem:

| Extension | Description |
|-----------|-------------|
| **[SQLite-AI](https://github.com/sqliteai/sqlite-ai)** | On-device inference and embedding generation |
| **[SQLite-Memory](https://github.com/sqliteai/sqlite-memory)** | Markdown-based AI agent memory with semantic search |
| **[SQLite-Vector](https://github.com/sqliteai/sqlite-vector)** | Vector search for embeddings stored as BLOBs |
| **[SQLite-Agent](https://github.com/sqliteai/sqlite-agent)** | Run autonomous AI agents from within SQLite |
| **[SQLite-MCP](https://github.com/sqliteai/sqlite-mcp)** | Connect SQLite databases to MCP servers |
| **[SQLite-JS](https://github.com/sqliteai/sqlite-js)** | Custom SQLite functions in JavaScript |
| **[Liteparser](https://github.com/sqliteai/liteparser)** | Fully compliant SQLite SQL parser |

## License

This project is licensed under the [Elastic License 2.0](./LICENSE.md). For production or managed service use, [contact SQLite Cloud, Inc](mailto:info@sqlitecloud.io) for a commercial license.
