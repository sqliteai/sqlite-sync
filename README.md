# SQLiteSync

[![sqlite-sync coverage](https://img.shields.io/badge/dynamic/regex?url=https%3A%2F%2Fsqliteai.github.io%2Fsqlite-sync%2F&search=%3Ctd%20class%3D%22headerItem%22%3EFunctions%3A%3C%5C%2Ftd%3E%5Cs*%3Ctd%20class%3D%22headerCovTableEntryHi%22%3E(%5B%5Cd.%5D%2B)%26nbsp%3B%25%3C%5C%2Ftd%3E&replace=%241%25&label=coverage&labelColor=rgb(85%2C%2085%2C%2085)%3B&color=rgb(167%2C%20252%2C%20157)%3B&link=https%3A%2F%2Fsqliteai.github.io%2Fsqlite-sync%2F)](https://sqliteai.github.io/sqlite-sync/)

SQLiteSync is a powerful SQLite extension that provides a local-first experience using Conflict-Free Replicated Data Types (CRDTs). Designed to enable seamless data synchronization and collaboration, SQLiteSync allows developers to build distributed applications with minimal effort while leveraging the reliability and simplicity of SQLite.

## Key Features

- **Local-First Approach**: Work offline with confidence. SQLiteSync ensures data integrity and merges changes effortlessly when devices reconnect.
- **CRDT Algorithms**: Resolve conflicts automatically with proven CRDT techniques, eliminating the need for manual resolution.
- **Lightweight and Efficient**: As a SQLite extension, SQLiteSync retains the minimal overhead and speed SQLite is known for.
- **Flexible Integration**: Add synchronization capabilities to existing SQLite-based applications with minimal modifications.
- **Reliable Data Sharing**: Enable collaboration across devices and users without compromising performance or data consistency.

## Installation

To use SQLiteSync, you need SQLite version 3.x or later.

### Build from Source

1. Install dependencies:

   ```bash
   #linux
   sudo apt-get install -y uuid-dev
   ```

2. Clone the repository:

   ```bash
   git clone https://github.com/sqliteai/sqlite-sync.git
   cd sqlitesync
   ```

3. Build the extension:

   ```bash
   make
   ```
