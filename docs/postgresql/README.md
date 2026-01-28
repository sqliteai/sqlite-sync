# Architecture Overview

The **SQLite AI offline-sync solution** consists of three main components:
* **sqlite-sync**: Native client-side SQLite extension
* **cloud-sync**: Synchronization microservice
* **postgres-sync**: Native PostgreSQL extension

Together, these components provide a complete, production-grade **offline-first synchronization stack** for SQLite and PostgreSQL.

# sqlite-sync

**sqlite-sync** is a native SQLite extension that must be installed and loaded on all client devices.
We provide prebuilt binaries for:
* Desktop and mobile platforms
* WebAssembly (WASM)
* Popular frameworks including React, Expo, npm, and more

**Note:** The latest version (v0.9.96) is not yet available in the official sqlite-sync repository.  Please use our development fork instead:[https://github.com/sqliteai/sqlite-sync-dev](https://github.com/sqliteai/sqlite-sync-dev)

### Architecture Refactoring
The extension has been refactored to support both **SQLite** and **PostgreSQL** backends.
* All database-specific native calls have been isolated in database.h
* Each database engine implements its own engine-dependent layer
* The core **CRDT logic** is fully shared across engines

This modular design improves **portability**, **maintainability**, and **cross-database consistency**.
### Testing & Reliability
* Shared CRDT and SQLite components include extensive unit tests
* Code coverage exceeds **90%**
* PostgreSQL-specific code has its own dedicated test suite

Key Features
* Deep integration with SQLite — the default database for Edge applications
* Built-in network layer exposed as ordinary SQLite functions
* Cross-platform, language-agnostic payload format
* Works seamlessly in any framework or programming language

Unlike other offline-sync solutions, **sqlite-sync embeds networking directly inside SQLite**, eliminating external sync SDKs.

### Supported CRDTs
Currently implemented CRDT algorithms:
* **Last-Write-Wins (LWW)**
* **Grow-Only Set (G-Set)**

Additional CRDTs can be implemented if needed, though LWW covers most real-world use cases.



# cloud-sync

**cloudsync** is a lightweight, stateless microservice responsible for synchronizing clients with central servers.
### Responsibilities
* Synchronizes clients with:
  * **SQLiteCloud servers**
  * **PostgreSQL servers**
* Manages upload and download of CRDT payloads
* Stores payloads via **AWS S3**
* Collects operational metrics (connected devices, sync volume, traffic, etc.)
* Exposes a complete **REST API**

⠀

Technology Stack

* Written in **Go**
* Built on the high-performance **Gin Web Framework**
* Fully **multitenant**
* Connects to multiple DBMS backends
* Stateless architecture enables horizontal scaling simply by adding nodes
* Serialized job queue ensures **no job loss**, even after restarts

⠀

Observability

* Metrics dashboard available in grafana-dashboard.json

* Additional logs available via the Fly.io monitoring dashboard

  

Demo Deployment
For the current demo, a single cloudsync node is deployed in **Europe** on Fly.io.
If testing from other regions, latency will reflect this single-node deployment.  A production deployment would use **geographically distributed nodes with regional routing** for global coverage.



# postgres-sync

**postgres-sync** is a native PostgreSQL extension derived from sqlite-sync.
### Features
* Implements the same CRDT algorithms available in sqlite-sync
* Applies CRDT logic to:
  * Changes coming from synchronized clients
  * Changes made directly in PostgreSQL (CLI, Drizzle, dashboards, etc.)

This ensures **full bidirectional consistency**, regardless of where changes originate.

### Schema Handling
SQLite does not support schemas, while PostgreSQL does. To bridge this difference, postgres-sync introduces a mechanism to:
* Associate each synchronized table with a specific PostgreSQL schema
* Allow different schemas per table
This preserves PostgreSQL-native organization while maintaining SQLite compatibility.



# Current Limitations

The PostgreSQL integration is actively evolving. Current limitations include:
* **User Impersonation** : The microservice currently applies server changes using the Supabase Admin user.  In the next version, changes will be applied under the identity associated with the client’s JWT.
* **Table Creation** : Tables must currently be created manually in PostgreSQL before synchronization.  We are implementing automatic translation of SQLite CREATE TABLE statements to PostgreSQL syntax.
* **Row-Level Security**: RLS is fully implemented for SQLiteCloud servers. PostgreSQL RLS integration is in progress and will be included in the final release.
* **Beta Status** : While extensively tested, the PostgreSQL sync stack should currently be considered **beta software**. Please report any issues, we are committed to resolving them quickly.

# Next
* [CLIENT](CLIENT.md) installation and setup
* [CLOUDSYNC](CLOUDSYNC.md) microservice configuration and setup
* [SUPABASE](SUPABASE.md) configuration and setup
