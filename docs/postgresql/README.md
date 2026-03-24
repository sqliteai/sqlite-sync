# Architecture Overview

The **SQLite AI offline-sync solution** consists of three main components:
* **SQLite Sync**: Native client-side SQLite extension
* **CloudSync**: Synchronization microservice
* **Postgres Sync**: Native PostgreSQL extension

Together, these components provide a complete, production-grade **offline-first synchronization stack** for SQLite and PostgreSQL.


# SQLite Sync

**SQLite Sync** is a native SQLite extension that must be installed and loaded on all client devices.
We provide prebuilt binaries for:
* Desktop and mobile platforms
* WebAssembly (WASM)
* Popular package managers and frameworks including React Native, Expo, Node, Swift PM and Android AAR

### Architecture Refactoring
The extension has been refactored to support both **SQLite** and **PostgreSQL** backends.
* All database-specific native calls have been isolated in [database.h](../../src/database.h)
* Each database engine implements its own engine-dependent layer
* The core **CRDT logic** is fully shared across engines

This modular design improves **portability**, **maintainability**, and **cross-database consistency**.
### Testing & Reliability
* Shared CRDT and SQLite components include extensive unit tests
* Code coverage exceeds **90%**
* PostgreSQL-specific code has its own dedicated test suite

### Key Features
* Deep integration with SQLite — the default database for Edge applications
* Built-in network layer exposed as ordinary SQLite functions
* Cross-platform, language-agnostic payload format
* Works seamlessly in any framework or programming language

Unlike other offline-sync solutions, **SQLite Sync embeds networking directly inside SQLite**, eliminating external sync SDKs.

### Supported CRDTs
Currently implemented CRDT algorithms:
* **Last-Write-Wins (LWW)**
* **Grow-Only Set (G-Set)**

Additional CRDTs can be implemented if needed, though LWW covers most real-world use cases.


# CloudSync

**CloudSync** is a lightweight, stateless microservice responsible for synchronizing clients with central servers.
### Responsibilities
* Synchronizes clients with:
  * **SQLite Cloud servers**
  * **PostgreSQL servers**
* Manages upload and download of CRDT payloads
* Stores payloads via **AWS S3**
* Collects operational metrics (connected devices, sync volume, traffic, etc.)
* Exposes a complete **REST API**

### Technology Stack

* Written in **Go**
* Built on the high-performance **Gin Web Framework**
* Fully **multitenant**
* Connects to multiple DBMS backends
* Stateless architecture enables horizontal scaling simply by adding nodes
* Serialized job queue ensures **no job loss**, even after restarts

### Observability

* Metrics dashboard available in [grafana-dashboard.json](grafana-dashboard.json)


# Postgres Sync

**Postgres Sync** is a native PostgreSQL extension derived from SQLite Sync.
### Features
* Implements the same CRDT algorithms available in SQLite Sync
* Applies CRDT logic to:
  * Changes coming from synchronized clients
  * Changes made directly in PostgreSQL (CLI, Drizzle, dashboards, etc.)

This ensures **full bidirectional consistency**, regardless of where changes originate.

### Schema Handling
SQLite does not support schemas, while PostgreSQL does. To bridge this difference, Postgres Sync introduces a mechanism to:

* Associate each synchronized table with a specific PostgreSQL schema
* Allow different schemas per table

This preserves PostgreSQL-native organization while maintaining SQLite compatibility.

# Current Limitations

The PostgreSQL integration is actively evolving. Current limitations include:

* **Table Creation**: Tables must currently be created manually in PostgreSQL before synchronization. We are implementing automatic translation of SQLite CREATE TABLE statements to PostgreSQL syntax.
* **Beta Status**: While extensively tested, the PostgreSQL sync stack should currently be considered **beta software**. Please report any issues; we are committed to resolving them quickly.

# Next
* [CLIENT](CLIENT.md) installation and setup
* [SUPABASE](SUPABASE.md) configuration and setup
* [SPORT-TRACKER APP](SPORT_APP_README_SUPABASE.md) demo web app based on SQLite Sync WASM