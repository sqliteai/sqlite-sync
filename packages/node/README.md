# @sqliteai/sqlite-sync

[![npm version](https://badge.fury.io/js/@sqliteai%2Fsqlite-sync.svg)](https://www.npmjs.com/package/@sqliteai/sqlite-sync)
[![License](https://img.shields.io/badge/license-Elastic%202.0-blue.svg)](LICENSE.md)
[![sqlite-sync coverage](https://img.shields.io/badge/dynamic/regex?url=https%3A%2F%2Fsqliteai.github.io%2Fsqlite-sync%2F&search=%3Ctd%20class%3D%22headerItem%22%3EFunctions%3A%3C%5C%2Ftd%3E%5Cs*%3Ctd%20class%3D%22headerCovTableEntryHi%22%3E(%5B%5Cd.%5D%2B)%26nbsp%3B%25%3C%5C%2Ftd%3E&replace=%241%25&label=coverage&labelColor=rgb(85%2C%2085%2C%2085)%3B&color=rgb(167%2C%20252%2C%20157)%3B&link=https%3A%2F%2Fsqliteai.github.io%2Fsqlite-sync%2F)](https://sqliteai.github.io/sqlite-sync/)

> SQLite Sync extension packaged for Node.js

**SQLite Sync** is a multi-platform extension that brings a true **local-first experience** to your applications with minimal effort. It extends standard SQLite tables with built-in support for offline work and automatic synchronization, allowing multiple devices to operate independently—even without a network connection—and seamlessly stay in sync. With SQLite Sync, developers can easily build **distributed, collaborative applications** while continuing to rely on the **simplicity, reliability, and performance of SQLite**.

Under the hood, SQLite Sync uses advanced **CRDT (Conflict-free Replicated Data Type)** algorithms and data structures designed specifically for **collaborative, distributed systems**. This means:

- Devices can update data independently, even without a network connection.
- When they reconnect, all changes are **merged automatically and without conflicts**.
- **No data loss. No overwrites. No manual conflict resolution.**

In simple terms, CRDTs make it possible for multiple users to **edit shared data at the same time**, from anywhere, and everything just works.

## Features

- ✅ **Bidirectional Sync** - Sync local SQLite databases with cloud storage
- ✅ **Conflict Resolution** - Intelligent conflict handling and resolution
- ✅ **Offline-First** - Full functionality without network connectivity
- ✅ **Cross-platform** - Works on macOS, Linux (glibc/musl), and Windows
- ✅ **Zero configuration** - Automatically detects and loads the correct binary for your platform
- ✅ **TypeScript native** - Full type definitions included
- ✅ **Modern ESM + CJS** - Works with both ES modules and CommonJS

## Installation

```bash
npm install @sqliteai/sqlite-sync
```

The package automatically downloads the correct native extension for your platform during installation.

### Supported Platforms

| Platform | Architecture | Package |
|----------|-------------|---------|
| macOS | ARM64 (Apple Silicon) | `@sqliteai/sqlite-sync-darwin-arm64` |
| macOS | x86_64 (Intel) | `@sqliteai/sqlite-sync-darwin-x86_64` |
| Linux | ARM64 (glibc) | `@sqliteai/sqlite-sync-linux-arm64` |
| Linux | ARM64 (musl/Alpine) | `@sqliteai/sqlite-sync-linux-arm64-musl` |
| Linux | x86_64 (glibc) | `@sqliteai/sqlite-sync-linux-x86_64` |
| Linux | x86_64 (musl/Alpine) | `@sqliteai/sqlite-sync-linux-x86_64-musl` |
| Windows | x86_64 | `@sqliteai/sqlite-sync-win32-x86_64` |

## sqlite-sync API

For detailed information on how to use the sync extension features, see the [main documentation](https://github.com/sqliteai/sqlite-sync/blob/main/API.md).

## Usage

```typescript
import { getExtensionPath } from '@sqliteai/sqlite-sync';
import Database from 'better-sqlite3';

const db = new Database(':memory:');
db.loadExtension(getExtensionPath());

// Ready to use
const version = db.prepare('SELECT cloudsync_version()').pluck().get();
console.log('Sync extension version:', version);
```

## Examples

For complete, runnable examples, see the [sqlite-extensions-guide](https://github.com/sqliteai/sqlite-extensions-guide/tree/main/examples/node).

These examples are generic and work with all SQLite extensions: `sqlite-vector`, `sqlite-sync`, `sqlite-js`, and `sqlite-ai`.

## API Reference

### `getExtensionPath(): string`

Returns the absolute path to the SQLite Sync extension binary for the current platform.

**Returns:** `string` - Absolute path to the extension file (`.so`, `.dylib`, or `.dll`)

**Throws:** `ExtensionNotFoundError` - If the extension binary cannot be found for the current platform

---

### `getExtensionInfo(): ExtensionInfo`

Returns detailed information about the extension for the current platform.

**Returns:** `ExtensionInfo` object with the following properties:
- `platform: Platform` - Current platform identifier (e.g., `'darwin-arm64'`)
- `packageName: string` - Name of the platform-specific npm package
- `binaryName: string` - Filename of the binary (e.g., `'cloudsync.dylib'`)
- `path: string` - Full path to the extension binary

**Throws:** `ExtensionNotFoundError` - If the extension binary cannot be found

**Example:**
```typescript
import { getExtensionInfo } from '@sqliteai/sqlite-sync';

const info = getExtensionInfo();
console.log(`Running on ${info.platform}`);
console.log(`Extension path: ${info.path}`);
```

---

### `getCurrentPlatform(): Platform`

Returns the current platform identifier.

**Returns:** `Platform` - One of:
- `'darwin-arm64'` - macOS ARM64
- `'darwin-x86_64'` - macOS x86_64
- `'linux-arm64'` - Linux ARM64 (glibc)
- `'linux-arm64-musl'` - Linux ARM64 (musl)
- `'linux-x86_64'` - Linux x86_64 (glibc)
- `'linux-x86_64-musl'` - Linux x86_64 (musl)
- `'win32-x86_64'` - Windows x86_64

**Throws:** `Error` - If the platform is unsupported

---

### `isMusl(): boolean`

Detects if the system uses musl libc (Alpine Linux, etc.).

**Returns:** `boolean` - `true` if musl is detected, `false` otherwise

---

### `class ExtensionNotFoundError extends Error`

Error thrown when the SQLite Sync extension cannot be found for the current platform.

## Related Projects

- **[@sqliteai/sqlite-sync-react-native](https://www.npmjs.com/package/@sqliteai/sqlite-sync-react-native)** - SQLite Sync for React Native
- **[@sqliteai/sqlite-sync-expo](https://www.npmjs.com/package/@sqliteai/sqlite-sync-expo)** - SQLite Sync for Expo
- **[@sqliteai/sqlite-vector](https://www.npmjs.com/package/@sqliteai/sqlite-vector)** - Vector search and similarity matching
- **[@sqliteai/sqlite-ai](https://www.npmjs.com/package/@sqliteai/sqlite-ai)** - On-device AI inference and embedding generation
- **[@sqliteai/sqlite-js](https://www.npmjs.com/package/@sqliteai/sqlite-js)** - Define SQLite functions in JavaScript

## License

This project is licensed under the [Elastic License 2.0](LICENSE.md).

For production or managed service use, please [contact SQLite Cloud, Inc](mailto:info@sqlitecloud.io) for a commercial license.

## Contributing

Contributions are welcome! Please see the [main repository](https://github.com/sqliteai/sqlite-sync) to open an issue.

## Support

- 📖 [Documentation](https://github.com/sqliteai/sqlite-sync/blob/main/API.md)
- 🐛 [Report Issues](https://github.com/sqliteai/sqlite-sync/issues)
