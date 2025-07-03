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
- [Usage](#usage)
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

### Usage

Here's a quick example to get started with `sqlite-sync`:

```sql
-- Load the extension
.load ./cloudsync

-- Create a table you want to synchronize
CREATE TABLE IF NOT EXISTS my_data (
    id TEXT PRIMARY KEY,
    value TEXT
);

-- Initialize synchronization for the table
-- This sets up internal metadata tables and triggers for CRDT functionality.
SELECT cloudsync_init('my_data');

-- Insert some data. These changes will be tracked for synchronization.
INSERT INTO my_data (id, value) VALUES (cloudsync_uuid(), 'Hello from device A!');

-- Configure network settings to connect to your SQLite Cloud database.
-- Replace <your_connection_string> with your actual connection details.
-- Example: 'sqlitecloud://<projectid>.sqlite.cloud/<db>.sqlite'
SELECT cloudsync_network_init('<your_connection_string>');

-- Set your API key or authentication token.
-- Use cloudsync_network_set_token() if you have an access token.
SELECT cloudsync_network_set_apikey('YOUR_API_KEY');

-- Manually trigger a synchronization cycle.
-- This sends local changes to the cloud and pulls down remote changes.
SELECT cloudsync_network_sync();

-- On another device (after loading extension and initializing the same table):
-- Running SELECT cloudsync_network_sync(); would pull changes from device A.
```

## License

This project is licensed under the [Elastic License 2.0](./LICENSE.md). You can use, copy, modify, and distribute it under the terms of the license for non-production use. For production or managed service use, please [contact SQLite Cloud, Inc](mailto:info@sqlitecloud.io) for a commercial license.