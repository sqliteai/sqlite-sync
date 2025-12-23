# PostgreSQL Extension Quick Reference

This guide covers building, installing, and testing the CloudSync PostgreSQL extension.

## Prerequisites

- Docker and Docker Compose (for containerized development)
- Or PostgreSQL 16 with development headers (`postgresql-server-dev-16`)
- Make and GCC

## Quick Start with Docker

```bash
# 1. Build Docker image with CloudSync extension pre-installed
make postgres-docker-build

# 2. Start PostgreSQL container
make postgres-docker-run

# 3. Connect and test
docker exec -it cloudsync-postgres psql -U postgres -d cloudsync_test
```

```sql
CREATE EXTENSION cloudsync;
SELECT cloudsync_version();
```

## Makefile Targets

### Build & Install

| Target | Description |
|--------|-------------|
| `make postgres-check` | Verify PostgreSQL installation |
| `make postgres-build` | Build extension (.so file) |
| `make postgres-install` | Install extension to PostgreSQL |
| `make postgres-clean` | Clean build artifacts |
| `make postgres-test` | Test extension (requires running PostgreSQL) |

### Docker Operations

| Target | Description |
|--------|-------------|
| `make postgres-docker-build` | Build Docker image with pre-installed extension |
| `make postgres-docker-run` | Start PostgreSQL container |
| `make postgres-docker-stop` | Stop PostgreSQL container |
| `make postgres-docker-rebuild` | Rebuild image and restart container |
| `make postgres-docker-shell` | Open bash shell in running container |

### Development

| Target | Description |
|--------|-------------|
| `make postgres-dev-rebuild` | Rebuild extension in running container (fast!) |
| `make postgres-help` | Show all PostgreSQL targets |

## Development Workflow

### Initial Setup

```bash
# Build and start container
make postgres-docker-build
make postgres-docker-run
```

### Making Changes

```bash
# 1. Edit source files in src/postgresql/ or src/

# 2. Rebuild extension (inside running container)
make postgres-dev-rebuild

# 3. Reload in PostgreSQL
docker exec -it cloudsync-postgres psql -U postgres -d cloudsync_test
```

```sql
DROP EXTENSION cloudsync CASCADE;
CREATE EXTENSION cloudsync;

-- Test your changes
SELECT cloudsync_version();
```

## Extension Functions

### Initialization

```sql
-- Initialize CloudSync for a table
SELECT cloudsync_init('my_table');                    -- Default algorithm
SELECT cloudsync_init('my_table', 'GOS');             -- Specify algorithm
SELECT cloudsync_init('my_table', 'GOS', false);      -- All options
```

**Algorithms**: `CLS` (Column-Level Sync), `GOS` (Greatest Order Sync), `DWS`, `AWS`

### Table Management

```sql
-- Enable/disable sync
SELECT cloudsync_enable('my_table');
SELECT cloudsync_disable('my_table');
SELECT cloudsync_is_enabled('my_table');

-- Cleanup and termination
SELECT cloudsync_cleanup('my_table');
SELECT cloudsync_terminate();
```

### Configuration

```sql
-- Global settings
SELECT cloudsync_set('key', 'value');

-- Table-level settings
SELECT cloudsync_set_table('my_table', 'key', 'value');

-- Column-level settings
SELECT cloudsync_set_column('my_table', 'my_column', 'key', 'value');
```

### Metadata

```sql
-- Get site ID (UUID)
SELECT cloudsync_siteid();

-- Get/generate UUIDs
SELECT cloudsync_uuid();

-- Database version
SELECT cloudsync_db_version();
SELECT cloudsync_db_version_next();
```

### Schema Alteration

```sql
-- Wrap ALTER TABLE statements
SELECT cloudsync_begin_alter('my_table');
ALTER TABLE my_table ADD COLUMN new_col TEXT;
SELECT cloudsync_commit_alter('my_table');
```

### Payload (Sync Operations)

```sql
-- Encode changes to payload
SELECT cloudsync_payload_encode();

-- Apply payload from another site
SELECT cloudsync_payload_decode(payload_data);
-- Or:
SELECT cloudsync_payload_apply(payload_data);
```

## Connection Details

When using `postgres-docker-run`:

- **Host**: `localhost`
- **Port**: `5432`
- **Database**: `cloudsync_test`
- **Username**: `postgres`
- **Password**: `postgres`

**Connection string**:
```
postgresql://postgres:postgres@localhost:5432/cloudsync_test
```

## Directory Structure

```
src/
├── cloudsync.c/h          # Core CRDT logic (platform-agnostic)
├── dbutils.c/h            # Database utilities
├── pk.c/h                 # Primary key encoding
├── utils.c/h              # General utilities
└── postgresql/            # PostgreSQL-specific implementation
    ├── database_postgresql.c    # Database abstraction layer
    ├── cloudsync_postgresql.c   # Extension entry point & SQL functions
    ├── pgvalue.c/h              # PostgreSQL value wrapper
    └── cloudsync--1.0.sql       # SQL installation script

docker/
├── postgresql/
│   ├── Dockerfile               # PostgreSQL + CloudSync image
│   ├── docker-compose.yml       # Container orchestration
│   ├── init.sql                # Metadata table creation
│   ├── cloudsync.control       # Extension metadata
│   └── Makefile.postgresql     # Build targets (included by root Makefile)
└── README.md
```

## Troubleshooting

### Extension not found

```bash
# Check installation
docker exec -it cloudsync-postgres bash
ls $(pg_config --pkglibdir)/cloudsync.so
ls $(pg_config --sharedir)/extension/cloudsync*

# Reinstall
cd /tmp/cloudsync
make postgres-install
```

### Build errors

```bash
# Ensure dependencies are installed
docker exec -it cloudsync-postgres bash
apt-get update
apt-get install -y build-essential postgresql-server-dev-16

# Clean and rebuild
cd /tmp/cloudsync
make postgres-clean
make postgres-build
```

### Container won't start

```bash
# Check logs
docker logs cloudsync-postgres

# Restart
make postgres-docker-stop
make postgres-docker-run
```

## Implementation Status

**21/27 functions (78%)** fully implemented:

✅ **Core Functions**: version, siteid, uuid, init, enable, disable, is_enabled, cleanup, terminate

✅ **Configuration**: set, set_table, set_column

✅ **Schema**: begin_alter, commit_alter

✅ **Versioning**: db_version, db_version_next, seq

✅ **Payload**: decode, apply, encode (partial)

✅ **Internal**: is_sync, insert, pk_encode

⚠️ **TODO**: pk_decode, update (aggregate), payload_encode (needs full variadic support)

## Next Steps

- Complete remaining aggregate functions (update, payload_encode)
- Add comprehensive test suite
- Performance benchmarking
- Integration with triggers for automatic sync

## Resources

- [AGENTS.md](./AGENTS.md) - Architecture and design patterns
- [docker/README.md](./docker/README.md) - Detailed Docker setup guide
- [plans/POSTGRESQL_IMPLEMENTATION.md](./plans/POSTGRESQL_IMPLEMENTATION.md) - Implementation roadmap
