# SQLite Sync

**SQLite Sync** is a multi-platform extension that brings a true **local-first experience** to your applications with minimal effort. It extends standard SQLite tables with built-in support for offline work and automatic synchronization, allowing multiple devices to operate independently — even without a network connection — while seamlessly staying in sync.

With SQLite Sync, developers can build **distributed, collaborative applications** while continuing to rely on the **simplicity, reliability, and performance of SQLite**.

Under the hood, SQLite Sync uses advanced **CRDT (Conflict-free Replicated Data Type)** algorithms and data structures designed specifically for **collaborative, distributed systems**:

- Devices can update data independently, even without a network connection.
- When they reconnect, all changes are **merged automatically and without conflicts**.
- **No data loss. No overwrites. No manual conflict resolution.**

---

## IMPORTANT

- Make sure to use version **0.9.96 or newer**  
  (verify with `SELECT cloudsync_version();`)

- Until v0.9.96 is released upstream, always use the development fork:  
  https://github.com/sqliteai/sqlite-sync-dev  
  and **not** the original repository:  
  https://github.com/sqliteai/sqlite-sync

- Updated example apps are available at:  
  https://github.com/sqliteai/sqlite-sync-dev/tree/main/examples  
  - sport-tracker-app (WASM), see [SPORT_APP_README_SUPABASE.md](SPORT_APP_README_SUPABASE.md) for more details
  - to-do-app (React)
  - React-Native (Expo): https://github.com/sqliteai/sqlite-sync-react-native
  - Remaining demos will be updated in the next days  

---

## Conversion Between SQLite and PostgreSQL Tables

- In this version, make sure to **manually create** the same tables in the PostgreSQL database as used in the SQLite client.
- Follow the Database Schema Recommendations:  
  https://github.com/sqliteai/sqlite-sync-dev?tab=readme-ov-file#database-schema-recommendations

---

## Pre-built Binaries

Download the appropriate pre-built binary for your platform from the official [Releases](https://github.com/sqliteai/sqlite-sync-dev/releases) page:

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



## WASM Version

```
npm i sqlite-wasm@dev
```

Then follow the instructions available from https://www.npmjs.com/package/@sqliteai/sqlite-wasm



## Swift Package

You can [add this repository as a package dependency to your Swift project](https://developer.apple.com/documentation/xcode/adding-package-dependencies-to-your-app#Add-a-package-dependency). After adding the package, you'll need to set up SQLite with extension loading by following steps 4 and 5 of [this guide](https://github.com/sqliteai/sqlite-extensions-guide/blob/main/platforms/ios.md#4-set-up-sqlite-with-extension-loading).



## Android Package

Add the [following](https://central.sonatype.com/artifact/ai.sqlite/sync.dev) to your Gradle dependencies:

```
implementation 'ai.sqlite:sync.dev:0.9.92'
```



## Expo

Install the Expo package:

```
npm install @sqliteai/sqlite-sync-expo-dev
```

Then follow the instructions from:

https://www.npmjs.com/package/@sqliteai/sqlite-sync-expo-dev



## React/Node

```js
npm i better-sqlite3
npm i @sqliteai/sqlite-sync-dev

echo "import { getExtensionPath } from '@sqliteai/sqlite-sync-dev';
import Database from 'better-sqlite3';

const db = new Database(':memory:');
db.loadExtension(getExtensionPath());

// Ready to use
const version = db.prepare('SELECT cloudsync_version()').pluck().get();
console.log('Sync extension version:', version);" >> index.js

node index.js
```

---

## Naming Clarification

- **sqlite-sync** → Client-side SQLite extension  
- **cloudsync** → Synchronization server microservice  
- **postgres-sync** → PostgreSQL extension  

The sqlite-sync extension is loaded in SQLite under the extension name:  
`cloudsync`
