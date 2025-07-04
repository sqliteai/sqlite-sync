# SQLite Sync

[![sqlite-sync coverage](https://img.shields.io/badge/dynamic/regex?url=https%3A%2F%2Fsqliteai.github.io%2Fsqlite-sync%2F&search=%3Ctd%20class%3D%22headerItem%22%3EFunctions%3A%3C%5C%2Ftd%3E%5Cs*%3Ctd%20class%3D%22headerCovTableEntryHi%22%3E(%5B%5Cd.%5D%2B)%26nbsp%3B%25%3C%5C%2Ftd%3E&replace=%241%25&label=coverage&labelColor=rgb(85%2C%2085%2C%2085)%3B&color=rgb(167%2C%20252%2C%20157)%3B&link=https%3A%2F%2Fsqliteai.github.io%2Fsqlite-sync%2F)](https://sqliteai.github.io/sqlite-sync/)

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
- [What Can You Build with SQLite Sync?](#what-can-you-build-with-sqlite-sync)
- [Documentation](#documentation)
- [Installation](#installation)
- [Getting Started](#getting-started)
- [License](#license)

## Key Features

- **Offline-First by Design**: Works seamlessly even when devices are offline. Changes are queued locally and synced automatically when connectivity is restored.
- **CRDT-Based Conflict Resolution**: Merges updates deterministically and efficiently, ensuring eventual consistency across all replicas without the need for complex merge logic.
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

Thanks to the underlying SQLite Cloud infrastructure, **SQLite Sync supports Row-Level Security (RLS)**—allowing you to define **precise access control at the row level**:

- Control not just who can read or write a table, but **which specific rows** they can access.
- Enforce security policies on the server—no need for client-side filtering.

For example:

- User A can only see and edit their own data.
- User B can access a different set of rows—even within the same shared table.

**Benefits of RLS**:

- **Data isolation**: Ensure users only access what they’re authorized to see.
- **Built-in privacy**: Security policies are enforced at the database level.
- **Simplified development**: Reduce or eliminate complex permission logic in your application code.

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

For detailed information on all available functions, their parameters, and examples, refer to the [comprehensive API Reference](./API.md).

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
    id TEXT PRIMARY KEY NOT NULL,
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
SELECT cloudsync_network_init('sqlitecloud://your-project-id.sqlite.cloud/database.sqlite');
SELECT cloudsync_network_set_apikey('your-api-key-here');
-- Or use token authentication (required for Row-Level Security)
-- SELECT cloudsync_network_set_token('your_auth_token');

-- Sync with cloud: send local changes, then check the remote server for new changes 
-- and, if a package with changes is ready to be downloaded, applies them to the local database
SELECT cloudsync_network_sync();
-- Keep calling periodically. The function returns > 0 if data was received
-- In production applications, you would typically call this periodically
-- rather than manually (e.g., every few seconds)
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
    id TEXT PRIMARY KEY NOT NULL,
    value TEXT NOT NULL DEFAULT '',
    created_at TEXT DEFAULT CURRENT_TIMESTAMP
);
SELECT cloudsync_init('my_data');

-- Connect to the same cloud database
SELECT cloudsync_network_init('sqlitecloud://your-project-id.sqlite.cloud/database.sqlite');
SELECT cloudsync_network_set_apikey('your-api-key-here');

-- Sync to get data from the first device 
SELECT cloudsync_network_sync();
-- repeat until data is received (returns > 0)
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

See the [Simple Todo Database example](./examples/simple-todo-db/) for a comprehensive walkthrough including:
- Multi-device collaboration
- Offline scenarios  
- Row-level security setup
- Conflict resolution demonstrations

## License

This project is licensed under the [Elastic License 2.0](./LICENSE.md). You can use, copy, modify, and distribute it under the terms of the license for non-production use. For production or managed service use, please [contact SQLite Cloud, Inc](mailto:info@sqlitecloud.io) for a commercial license.