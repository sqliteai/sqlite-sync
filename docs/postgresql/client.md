# SQLite Sync

**SQLite Sync** is a multi-platform extension that brings a true **local-first experience** to your applications with minimal effort. It extends standard SQLite tables with built-in support for offline work and automatic synchronization, allowing multiple devices to operate independently — even without a network connection — while seamlessly staying in sync.

With SQLite Sync, developers can build **distributed, collaborative applications** while continuing to rely on the **simplicity, reliability, and performance of SQLite**.

Under the hood, SQLite Sync uses advanced **CRDT (Conflict-free Replicated Data Type)** algorithms and data structures designed specifically for **collaborative, distributed systems**:

- Devices can update data independently, even without a network connection.
- When they reconnect, all changes are **merged automatically and without conflicts**.
- **No data loss. No overwrites. No manual conflict resolution.**

## Conversion Between SQLite and PostgreSQL Tables

In this version, make sure to **manually create** the same tables in the PostgreSQL database as used in the SQLite client.

This guide shows how to manually convert a SQLite table definition to PostgreSQL
so CloudSync can sync between a PostgreSQL server and SQLite clients.

### 1) Primary Keys

- Use **TEXT** primary keys in SQLite.
- PostgreSQL primary keys can be **TEXT** or **UUID**. If the PK type
  isn't explicitly mapped to a DBTYPE (like UUID), it will be converted to TEXT
  in the payload so it remains compatible with the SQLite extension.
- Generate IDs with `cloudsync_uuid()` on both sides.
- Avoid INTEGER auto-increment PKs.

SQLite:
```sql
id TEXT PRIMARY KEY
```

PostgreSQL:
```sql
id TEXT PRIMARY KEY
```

PostgreSQL (UUID):
```sql
id UUID PRIMARY KEY
```

### 2) NOT NULL Columns Must Have DEFAULTs

CloudSync merges column-by-column. Any NOT NULL (non-PK) column needs a DEFAULT
to avoid constraint failures during merges.

Example:
```sql
title TEXT NOT NULL DEFAULT ''
count INTEGER NOT NULL DEFAULT 0
```

### 3) Safe Type Mapping

Use types that map cleanly to CloudSync's DBTYPEs:

- INTEGER → `INTEGER` (SQLite) / `INTEGER` (Postgres)
- FLOAT → `REAL` / `DOUBLE` (SQLite) / `DOUBLE PRECISION` (Postgres)
- TEXT → `TEXT` (both)
- BLOB → `BLOB` (SQLite) / `BYTEA` (Postgres)

Avoid: JSON/JSONB, UUID, INET, CIDR, RANGE, ARRAY unless you accept text-cast
behavior.

### 4) Defaults That Match Semantics

Use defaults that serialize the same on both sides:

- TEXT: `DEFAULT ''`
- INTEGER: `DEFAULT 0`
- FLOAT: `DEFAULT 0.0`
- BLOB: `DEFAULT X'00'` (SQLite) vs `DEFAULT E'\\x00'` (Postgres)

### 5) Foreign Keys and Triggers

- Foreign keys can cause merge conflicts; test carefully.
- Application triggers will fire during merge; keep them idempotent or disable
  in synced tables.

### 6) Example Conversion

SQLite:
```sql
CREATE TABLE notes (
  id TEXT PRIMARY KEY,
  title TEXT NOT NULL DEFAULT '',
  body TEXT DEFAULT '',
  views INTEGER NOT NULL DEFAULT 0,
  rating REAL DEFAULT 0.0,
  data BLOB
);
```

PostgreSQL:
```sql
CREATE TABLE notes (
  id TEXT PRIMARY KEY,
  title TEXT NOT NULL DEFAULT '',
  body TEXT DEFAULT '',
  views INTEGER NOT NULL DEFAULT 0,
  rating DOUBLE PRECISION DEFAULT 0.0,
  data BYTEA
);
```

### 7) Enable CloudSync

SQLite:
```sql
.load dist/cloudsync.dylib
SELECT cloudsync_init('notes');
```

PostgreSQL:
```sql
CREATE EXTENSION cloudsync;
SELECT cloudsync_init('notes');
```

### Checklist

- [ ] PKs are TEXT (or UUID in PostgreSQL)
- [ ] All NOT NULL columns have DEFAULT
- [ ] Only INTEGER/FLOAT/TEXT/BLOB-compatible types
- [ ] Same column names and order
- [ ] Same defaults (semantic match)

Please follow [these Database Schema Recommendations](https://github.com/sqliteai/sqlite-sync?tab=readme-ov-file#database-schema-recommendations)

## Pre-built Binaries

Download the appropriate pre-built binary for your platform from the official [Releases](https://github.com/sqliteai/sqlite-sync/releases) page:

- Linux: x86 and ARM
- macOS: x86 and ARM
- Windows: x86
- Android
- iOS

## Loading the Extension

```
-- In SQLite CLI
.load ./cloudsync

-- In SQL
SELECT load_extension('./cloudsync');
```

## WASM Version -> React client-side

```
npm i @sqliteai/sqlite-wasm
```

Then follow the instructions from the [README](https://www.npmjs.com/package/@sqliteai/sqlite-wasm)

## Swift Package

You can [add this repository as a package dependency to your Swift project](https://developer.apple.com/documentation/xcode/adding-package-dependencies-to-your-app#Add-a-package-dependency). After adding the package, you'll need to set up SQLite with extension loading by following steps 4 and 5 of [this guide](https://github.com/sqliteai/sqlite-extensions-guide/blob/main/platforms/ios.md#4-set-up-sqlite-with-extension-loading).

## Android Package

Add the [following](https://central.sonatype.com/artifact/ai.sqlite/sync) to your Gradle dependencies:

```
implementation 'ai.sqlite:sync:1.0.0'
```

## Expo

Install the Expo package:

```
npm install @sqliteai/sqlite-sync-expo
```

Then follow the instructions from the [README](https://www.npmjs.com/package/@sqliteai/sqlite-sync-expo)

## React Native

Install the React Native library:

```
npm install @sqliteai/sqlite-sync-react-native
```

Then follow the instructions from the [README](https://www.npmjs.com/package/@sqliteai/sqlite-sync-react-native)

## Node -> React server-side

```js
npm i better-sqlite3
npm i @sqliteai/sqlite-sync

echo "import { getExtensionPath } from '@sqliteai/sqlite-sync';
import Database from 'better-sqlite3';

const db = new Database(':memory:');
db.loadExtension(getExtensionPath());

// Ready to use
const version = db.prepare('SELECT cloudsync_version()').pluck().get();
console.log('Sync extension version:', version);" >> index.js

node index.js
```

## Naming Clarification

- **sqlite-sync** → Client-side SQLite extension  
- **cloudsync** → Synchronization server microservice  
- **postgres-sync** → PostgreSQL extension  

The sqlite-sync extension is loaded in SQLite under the extension name:  
`cloudsync`
