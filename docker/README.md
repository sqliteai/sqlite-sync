# CloudSync Docker Setup

This directory contains Docker configurations for developing and testing CloudSync with PostgreSQL.

## Directory Structure

```
docker/
├── postgresql/          # Standalone PostgreSQL with CloudSync
│   ├── Dockerfile       # Custom PostgreSQL image
│   ├── docker-compose.yml
│   ├── init.sql        # CloudSync metadata tables
│   └── cloudsync.control
└── supabase/           # Supabase integration
    └── docker-compose.yml
```

## Option 1: Standalone PostgreSQL

Use this for simple PostgreSQL development and testing.

### Quick Start

```bash
# Build Docker image with CloudSync extension
make postgres-docker-build

# Start PostgreSQL container
make postgres-docker-run

# Test the extension
docker exec -it cloudsync-postgres psql -U postgres -d cloudsync_test -c "CREATE EXTENSION cloudsync; SELECT cloudsync_version();"
```

This starts:
- PostgreSQL 16 on `localhost:5432`
- CloudSync extension pre-installed
- pgAdmin on `localhost:5050` (optional, use `--profile admin`)

### Configuration

- **Database**: `cloudsync_test`
- **Username**: `postgres`
- **Password**: `postgres`

### Development Workflow

After making changes to the source code:

```bash
# Quick rebuild inside running container (fast!)
make postgres-dev-rebuild

# Then reload the extension in psql
docker exec -it cloudsync-postgres psql -U postgres -d cloudsync_test
```

```sql
DROP EXTENSION cloudsync CASCADE;
CREATE EXTENSION cloudsync;
SELECT cloudsync_version();
```

### Using pgAdmin (Optional)

Start with the admin profile:

```bash
docker-compose --profile admin up -d
```

Access pgAdmin at http://localhost:5050:
- Email: `admin@cloudsync.local`
- Password: `admin`

## Option 2: Supabase Integration

Use this for testing CloudSync with Supabase's full stack (auth, realtime, storage, etc.).

### Prerequisites

Ensure you have both repositories cloned side-by-side:

```bash
parent-directory/
├── supabase/
└── sqlite-sync/
```

### Setup

1. Clone the Supabase repository:
   ```bash
   git clone --depth 1 https://github.com/supabase/supabase
   cd supabase/docker
   ```

2. Copy CloudSync override configuration:
   ```bash
   cp ../../sqlite-sync/docker/supabase/docker-compose.yml docker-compose.override.yml
   ```

3. Copy the `.env` file and configure it:
   ```bash
   cp .env.example .env
   # Edit .env with your preferred settings
   ```

### Starting Supabase with CloudSync

The override file will automatically build the custom PostgreSQL image:

```bash
cd supabase/docker
docker-compose up -d
```

This will:
- Build the CloudSync-enabled PostgreSQL image (first time only)
- Start all Supabase services with CloudSync support
- Initialize CloudSync metadata tables alongside Supabase tables

Access Supabase Studio at http://localhost:3000

### Using the CloudSync Extension

Connect to the database and enable the extension:

```bash
psql postgresql://postgres:postgres@localhost:5432/postgres
```

```sql
CREATE EXTENSION cloudsync;

-- Verify installation
SELECT cloudsync_version();
```

### Rebuilding After Changes

If you modify the CloudSync source code, rebuild the image:

```bash
cd supabase/docker
docker-compose build db
docker-compose up -d
```

## Development Workflow

### 1. Make Changes

Edit source files in `src/postgresql/` or `src/` (shared code).

### 2. Rebuild Extension

**Fast method** (rebuild in running container):
```bash
make postgres-dev-rebuild
```

**Or manually**:
```bash
docker exec -it cloudsync-postgres bash
cd /tmp/cloudsync
make postgres-clean && make postgres-build && make postgres-install
```

### 3. Reload Extension in PostgreSQL

```bash
docker exec -it cloudsync-postgres psql -U postgres -d cloudsync_test
```

```sql
-- Reload extension
DROP EXTENSION IF EXISTS cloudsync CASCADE;
CREATE EXTENSION cloudsync;

-- Test your changes
SELECT cloudsync_version();
SELECT cloudsync_init('test_table');
```

## Troubleshooting

### Extension Not Found

If you get "could not open extension control file", the extension wasn't installed correctly:

```bash
# Check installation paths
pg_config --sharedir  # Should contain cloudsync.control
pg_config --pkglibdir # Should contain cloudsync.so

# Reinstall
cd /tmp/cloudsync
make install POSTGRES=1
```

### Build Errors

If you encounter build errors:

```bash
# Install missing dependencies
apt-get update
apt-get install -y build-essential postgresql-server-dev-16

# Clean and rebuild
make clean
make POSTGRES=1
```

### Database Connection Issues

If you can't connect to PostgreSQL:

```bash
# Check if PostgreSQL is running
docker ps | grep postgres

# Check logs
docker logs cloudsync-postgres

# Restart container
docker-compose restart
```

## Environment Variables

You can customize the setup using environment variables:

```bash
# PostgreSQL
export POSTGRES_PASSWORD=mypassword
export POSTGRES_DB=mydb

# Ports
export POSTGRES_PORT=5432
export PGADMIN_PORT=5050

docker-compose up -d
```

## Cleaning Up

```bash
# Stop containers
docker-compose down

# Remove volumes (deletes all data!)
docker-compose down -v

# Remove images
docker rmi sqliteai/sqlite-sync-pg:latest
```

## Next Steps

- Read [AGENTS.md](../AGENTS.md) for architecture details
- See [API.md](../API.md) for CloudSync API documentation
- Check [test/](../test/) for example usage
