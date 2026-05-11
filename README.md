<div align="center">
  <a href="https://sqlite.ai">
    <img src="https://www.sqlite.ai/social/logo-ai.png" alt="SQLite AI" height="56">
  </a>

  <h1>SQLite-Sync</h1>
  <p><strong>Offline-first sync for SQLite, powered by CRDTs.</strong><br>
  Local writes, conflict-free merges, real-time collaboration across devices. Sync to SQLite Cloud, PostgreSQL, or Supabase — no central coordinator required.</p>

  <p>
    <a href="https://dashboard.sqlitecloud.io/auth/sign-in"><strong>Free managed instance →</strong></a> ·
    <a href="https://docs.sqlitecloud.io/docs/ai-overview">Docs</a> ·
    <a href="https://www.sqlite.ai/sync-demo">Live Demo</a> ·
    <a href="https://sqlite.ai">Website</a>
  </p>

  <p>
    <sub><strong>Data:</strong>
    <a href="https://github.com/sqliteai/sqlite-vector">Vector</a> ·
    <a href="https://github.com/sqliteai/sqlite-sync">Sync</a> ·
    <a href="https://github.com/sqliteai/sqlite-columnar">Columnar</a> ·
    <a href="https://github.com/sqliteai/sqlite-js">JS</a>
    <br>
    <strong>AI:</strong>
    <a href="https://github.com/sqliteai/sqlite-ai">AI</a> ·
    <a href="https://github.com/sqliteai/sqlite-agent">Agent</a> ·
    <a href="https://github.com/sqliteai/sqlite-memory">Memory</a> ·
    <a href="https://github.com/sqliteai/sqlite-mcp">MCP</a>
    </sub>
  </p>
</div>

<br>

> **Need a sync backend?** Plug into any PostgreSQL or Supabase instance, or use **[SQLite Cloud CloudSync](https://www.sqlite.ai/pricing)** — managed device sync with auth, ACL, and a free tier for up to 3 devices.

---

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

Download a pre-built binary from the [Releases](https://github.com/sqliteai/sqlite-sync/releases) page, or install a platform package (see [full installation guide](./docs/INSTALLATION.md) for platform-specific code examples):

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

> **Note:** every device participating in the same sync must create **the same set of tables with the same structure** and initialize each one with `cloudsync_init()`. sqlite-sync derives a schema hash from the synced tables, and the server rejects payloads whose hash it does not recognize. For multi-tenant setups where each client should see only a subset of rows, use a shared schema with a tenant/scope column and enforce isolation with [Row-Level Security](./docs/row-level-security.md) — do not give each client a different table.

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

## Versioning

This project follows [semver](https://semver.org/). The single source of truth is `CLOUDSYNC_VERSION` in `src/cloudsync.h`; all packaged artifacts (NPM, Maven, pub.dev, Swift, Docker, native tarballs) inherit this version. PATCH releases never alter the exposed API — they ship bug fixes, performance improvements, and internal changes only.

The PostgreSQL extension differs only in how it surfaces the version: its catalog version (`default_version` / `installed_version`) exposes `MAJOR.MINOR` only, so PATCH releases are transparent binary upgrades and only MINOR/MAJOR releases need `ALTER EXTENSION cloudsync UPDATE`. The `cloudsync_version()` SQL function always reports the full semver of the loaded `.so`. See the [PostgreSQL upgrade docs](docs/postgresql/quickstarts/postgres.md#upgrading-a-later-release) for the user-facing procedure.

## License

This project is licensed under the [Elastic License 2.0](./LICENSE.md). For production or managed service use, [contact SQLite Cloud, Inc](mailto:info@sqlitecloud.io) for a commercial license.

---

## ☁️ Hosted version

Don't want to run a sync server yourself? **[SQLite Cloud CloudSync](https://sqlite.ai)** is the managed backend for SQLite-Sync — works with SQLite Cloud, PostgreSQL, or Supabase as your source of truth, with auth, ACL, and observability included.

[**Start free →**](https://dashboard.sqlitecloud.io/auth/sign-in)

---

## Part of the SQLite AI stack

SQLite-Sync is one piece of a larger ecosystem that turns SQLite into a runtime for intelligent, distributed data:

**Data layer**
- [sqlite-vector](https://github.com/sqliteai/sqlite-vector) — ANN vector search inside SQLite
- [**sqlite-sync**](https://github.com/sqliteai/sqlite-sync) — Offline-first CRDT sync across devices *(you are here)*
- [sqlite-columnar](https://github.com/sqliteai/sqlite-columnar) — Column-oriented analytics for OLAP queries
- [sqlite-js](https://github.com/sqliteai/sqlite-js) — Custom SQLite functions written in JavaScript

**AI layer**
- [sqlite-ai](https://github.com/sqliteai/sqlite-ai) — On-device LLM inference and embeddings
- [sqlite-agent](https://github.com/sqliteai/sqlite-agent) — Autonomous AI agents running inside SQLite
- [sqlite-memory](https://github.com/sqliteai/sqlite-memory) — Persistent, searchable memory for agents
- [sqlite-mcp](https://github.com/sqliteai/sqlite-mcp) — Call MCP tools directly from SQL queries

**Managed platform**
- [SQLite Cloud](https://sqlite.ai) — Hosted SQLite with sync, auth, edge functions, and analytics. [Free tier →](https://dashboard.sqlitecloud.io/auth/sign-in)

Built by [SQLite AI](https://sqlite.ai). Questions? [Contact us](https://sqlite.ai/support).
