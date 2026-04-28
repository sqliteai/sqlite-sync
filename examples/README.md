# SQLite Sync Examples

This directory contains comprehensive examples demonstrating SQLite Sync in various environments and use cases. Each example showcases offline-first data synchronization across multiple devices using SQLite AI's sync capabilities.

## Examples Overview

### [simple-todo-db/](./simple-todo-db/)
**Basic CLI Example - Getting Started**
- Command-line demonstration using SQLite CLI
- Shows fundamental SQLite Sync concepts and workflow
- Multi-device collaborative editing with conflict resolution
- Offline scenarios and network synchronization
- Perfect for understanding core sync mechanics

### [schema-migrations/](./schema-migrations/)
**Schema Migrations**
- Client-to-server and server-to-client migration examples
- Demonstrates the `cloudsync_alter_*` API, generated pending migrations, table creation, `cloudsync_init`, block-level LWW, dialect overrides, raw SQL escape hatches, and V2 rebuild payloads
- Shows how schema-capable API keys fit into migration upload/download

### [sport-tracker-app/](./sport-tracker-app/)
**Advanced Web App - Production Patterns**
- React/TypeScript web application with Vite
- Demonstrates secure authentication with Access Tokens
- Row-Level Security (RLS) implementation for data isolation
- Multi-user scenarios (athletes and coaches)
- WASM SQLite with sync extension
- Production-ready security patterns

### [swift-multiplatform-app/](./swift-multiplatform-app/)
**Swift iOS/macOS Integration**
- Cross-platform Swift app for iOS and macOS
- CloudSync XCFramework integration guide
- Native mobile and desktop sync capabilities
- Xcode project setup and security configuration

### [to-do-app/](./to-do-app/)
**React Native/Expo Mobile App**
- Cross-platform mobile app (iOS/Android)
- Expo framework with SQLite sync
- Simple task management with categories
- Offline-first mobile development patterns
- **Note**: Uses basic connection strings for demo purposes only

## Getting Started

1. **Begin with**: `simple-todo-db/` for core concepts
2. **Production web apps**: Use `sport-tracker-app/` patterns
3. **Native mobile**: Choose `swift-multiplatform-app/` or `to-do-app/`

Each example includes detailed setup instructions, code explanations, and security considerations. See the individual README files for specific implementation details.

---

**Note**: For generic extension loading guides please refer to the [SQLite Extension Guide](https://github.com/sqliteai/sqlite-extensions-guide) repository
