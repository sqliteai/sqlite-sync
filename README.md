# SQLiteSync

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
