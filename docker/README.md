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

### VS Code Dev Container Debugging (PostgreSQL)

Use this when you want breakpoints in the extension code.
The dev container uses `docker/postgresql/Dockerfile.debug` and `docker/postgresql/docker-compose.debug.yml`, which build the extension with debug symbols.
Required VS Code extensions:
- `ms-vscode-remote.remote-containers` (Dev Containers)
- `ms-vscode.cpptools` (C/C++ debugging)

1) Open the dev container  
VS Code -> Command Palette -> `Dev Containers: Reopen in Container`

2) Connect with `psql` (inside the dev container)
```bash
psql -U postgres -d cloudsync_test
```

3) Enable the extension if needed
```sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
```

4) Get the backend PID (inside `psql`)
```sql
SELECT pg_backend_pid();
```

5) Attach the debugger (VS Code dev container window)  
Run and Debug -> `Attach to Postgres (gdb)` -> pick the PID from step 4 -> Continue

6) Trigger your breakpoint  
Run the SQL that exercises the code path. If `psql` blocks, the backend is paused at a breakpoint; continue in the debugger.

## Option 2: Supabase Integration (cli)

Use this when you're running `supabase start` and want CloudSync inside the local stack.
The Supabase CLI uses a bundled PostgreSQL image (for example,
`public.ecr.aws/supabase/postgres:17.6.1.071`). Build a matching image that
includes CloudSync, then tag it with the same name so the CLI reuses it. This
keeps your local Supabase stack intact (auth, realtime, storage, etc.) while
enabling the extension in the CLI-managed Postgres container.

### Prerequisites

- Supabase CLI installed (`supabase start` works)
- Docker running

### Setup

1. Initialize a Supabase project (use a separate workdir to keep generated files
   out of the repo):
   ```bash
   mkdir -p ~/supabase-local
   supabase init --workdir ~/supabase-local
   ```

2. Start Supabase once so the CLI pulls the Postgres image:
   ```bash
   supabase start --workdir ~/supabase-local
   ```

3. Build and tag a CloudSync image using the same tag as the running CLI stack:
   ```bash
   make postgres-supabase-build
   ```
   This auto-detects the running `supabase_db` image tag and rebuilds it with
   CloudSync installed. If you need to override the full image tag, set
   `SUPABASE_CLI_IMAGE=public.ecr.aws/supabase/postgres:<tag>`.
   Example: 
   ```bash
   SUPABASE_CLI_IMAGE=public.ecr.aws/supabase/postgres:17.6.1.071 make postgres-supabase-build
   ```
   You can also set the Supabase base image tag explicitly (defaults to
   `17.6.1.071`). This only affects the base image used in the Dockerfile:
   ```bash
   SUPABASE_POSTGRES_TAG=17.6.1.071 make postgres-supabase-build
   ```

4. Restart the stack:
   ```bash
   supabase stop --workdir ~/supabase-local
   supabase start --workdir ~/supabase-local
   ```

### Using the CloudSync Extension

You can load the extension automatically from a migration, or enable it
manually.

Migration-based (notes for CLI): Supabase CLI migrations run as the `postgres`
role, which cannot create C extensions by default. Use manual enable or grant
`USAGE` on language `c` once, then migrations will work. Note: `c` is an
untrusted language, so `GRANT USAGE ON LANGUAGE c` is only allowed for
superusers. On the CLI/local stack, the simplest approach is to enable the
extension manually as `supabase_admin` after `supabase db reset`.

If you still want a migration file, add:
```bash
~/supabase-local/supabase/migrations/00000000000000_cloudsync.sql
```
Contents:
```sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
```

Then either:
- run `GRANT USAGE ON LANGUAGE c TO postgres;` once as `supabase_admin`, or
- skip the migration and enable the extension manually after `supabase db reset`.

Manual enable (no reset required):

Connect as the Supabase superuser (C extensions require superuser or language
privileges), then enable the extension:

```bash
psql postgresql://supabase_admin:postgres@127.0.0.1:54322/postgres
```

```sql
CREATE EXTENSION cloudsync;
SELECT cloudsync_version();
```

If you want to use the `postgres` role instead:

```sql
GRANT USAGE ON LANGUAGE c TO postgres;
```

### Rebuilding After Changes

If you modify the CloudSync source code, rebuild the CLI image and restart:

```bash
make postgres-supabase-rebuild SUPABASE_WORKDIR=~/supabase-local
```

### Supabase Realtime Migration Error (app_schema_version)

If Supabase Realtime fails to start with:

```
ERROR 42P01 (undefined_table) relation "app_schema_version" does not exist
```

it's caused by CloudSync's `app_schema_change` event trigger firing during
migrations while Realtime uses a restricted `search_path`. Fix it by
fully qualifying the table in the trigger function:

```sql
CREATE TABLE IF NOT EXISTS public.app_schema_version (
  version BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY
);

CREATE OR REPLACE FUNCTION bump_app_schema_version()
RETURNS event_trigger AS $$
BEGIN
  INSERT INTO public.app_schema_version DEFAULT VALUES;
END;
$$ LANGUAGE plpgsql;

DROP EVENT TRIGGER IF EXISTS app_schema_change;
CREATE EVENT TRIGGER app_schema_change
ON ddl_command_end
EXECUTE FUNCTION bump_app_schema_version();
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

- See [API.md](../API.md) for CloudSync API documentation
