# SQLiteSync

[![sqlite-sync coverage](https://img.shields.io/badge/dynamic/regex?url=https%3A%2F%2Fsqliteai.github.io%2Fsqlite-sync%2F&search=%3Ctd%20class%3D%22headerItem%22%3EFunctions%3A%3C%5C%2Ftd%3E%5Cs*%3Ctd%20class%3D%22headerCovTableEntryHi%22%3E(%5B%5Cd.%5D%2B)%26nbsp%3B%25%3C%5C%2Ftd%3E&replace=%241%25&label=coverage&labelColor=rgb(85%2C%2085%2C%2085)%3B&color=rgb(167%2C%20252%2C%20157)%3B&link=https%3A%2F%2Fsqliteai.github.io%2Fsqlite-sync%2F)](https://sqliteai.github.io/sqlite-sync/)

SQLiteSync is a powerful SQLite extension that provides a local-first experience using Conflict-Free Replicated Data Types (CRDTs). Designed to enable seamless data synchronization and collaboration, SQLiteSync allows developers to build distributed applications with minimal effort while leveraging the reliability and simplicity of SQLite.

## Key Features

- **Offline-First by Design**: Works seamlessly even when devices are offline. Changes are queued locally and synced automatically when connectivity is restored.
- **CRDT-Based Conflict Resolution**: Merges updates deterministically and efficiently, ensuring eventual consistency across all replicas without the need for complex merge logic.
- **Embedded Network Layer**: No external libraries or sync servers required. SQLiteSync handles connection setup, message encoding, retries, and state reconciliation internally.
- **Drop-in Simplicity**: Just load the extension into SQLite and start syncing. No need to implement custom protocols or state machines.
- **Efficient and Resilient**: Optimized binary encoding, automatic batching, and robust retry logic make synchronization fast and reliable even on flaky networks.

Whether you're building a mobile app, IoT device, or desktop tool, SQLiteSync simplifies distributed data management and unlocks the full potential of SQLite in decentralized environments.

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
.load ./js

-- In SQL
SELECT load_extension('./cloudsync');
```
