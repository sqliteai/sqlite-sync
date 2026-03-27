# Self-Hosting Supabase on Fly.io with CloudSync Extension

This guide walks you through deploying a full self-hosted Supabase stack on a Fly.io VM, with the CloudSync PostgreSQL extension pre-installed. By the end you will have:

- A Fly.io VM running all 13 Supabase services via Docker Compose
- PostgreSQL with the CloudSync CRDT extension baked in
- Supabase Studio dashboard accessible over HTTPS
- A custom Postgres image published to Docker Hub

## Prerequisites

Install these on your **local machine** before starting:

| Tool | Purpose | Install |
|------|---------|---------|
| [Docker Desktop](https://www.docker.com/products/docker-desktop/) | Build the custom Postgres image | See [Installing Docker Desktop](#installing-docker-desktop) below |
| [Fly CLI (`flyctl`)](https://fly.io/docs/flyctl/install/) | Provision and manage Fly.io machines | `brew install flyctl` (macOS) or `curl -L https://fly.io/install.sh \| sh` |
| [Git](https://git-scm.com/) | Clone repositories | `brew install git` (macOS) |
| [Docker Hub](https://hub.docker.com/) account | Host your custom Postgres image | Free signup at hub.docker.com |

You also need a [Fly.io account](https://fly.io/app/sign-up). A credit card is required even for free tier.

### Installing Docker Desktop

Docker Desktop is the application that lets you build and run container images on your Mac.

1. **Download** from https://www.docker.com/products/docker-desktop/ — pick **Apple chip** (M1/M2/M3/M4) or **Intel chip** depending on your Mac.

   > Not sure which you have? Click the Apple menu () → **About This Mac**. It will say either "Apple M1/M2/M3/M4" or "Intel".

2. **Install**: Open the downloaded `.dmg` file and drag Docker into your Applications folder.

3. **Launch**: Open Docker from Applications (or Spotlight: Cmd+Space → type "Docker"). It will ask for your password to install system components — that's normal.

4. **Wait**: A whale icon appears in your menu bar. Wait until it says "Docker Desktop is running" (the whale stops animating).

5. **Verify** in Terminal:
   ```bash
   docker --version
   # Should output: Docker version 27.x.x or similar
   ```

### Setting up Docker Hub

Docker Hub is a free cloud registry where you'll upload your custom Postgres image so the Fly.io server can download it.

1. **Sign up** at https://hub.docker.com/signup — pick a username. This becomes your image prefix (e.g., `myusername/supabase-postgres-cloudsync`).

2. **Log in from Terminal**:
   ```bash
   docker login
   ```
   Enter your Docker Hub username and password. You should see "Login Succeeded".

### Fly.io VM requirements

Supabase runs 13 services simultaneously (Postgres, Auth, PostgREST, Realtime, Studio, Kong, Storage, etc.), which is why it needs more resources than a typical single app.

| Resource | Minimum | Recommended |
|----------|---------|-------------|
| RAM | 4 GB | 8 GB+ |
| CPU | 2 cores | 4 cores |
| Disk | 50 GB SSD | 80 GB+ |

---

## Step 1: Initialize git submodules

The CloudSync extension depends on the [fractional-indexing](https://github.com/sqliteai/fractional-indexing) library, which is included as a git submodule. If you haven't done this already, initialize it:

```bash
cd /path/to/sqlite-sync-dev
git submodule update --init --recursive
```

Without this, the build will fail with `fractional_indexing.h: No such file or directory`.

---

## Step 2: Build the custom Supabase Postgres image

> **Important — match the Postgres version!** Check which Postgres version the Supabase docker-compose uses by looking at the `db` service `image` tag in `docker-compose.yml` (e.g., `supabase/postgres:15.8.1.085` means PG 15). You must build your custom image with the **same tag**. Using the wrong version will cause init script failures.

The `make postgres-supabase-build` command does the following:

1. **Pulls the official Supabase Postgres base image** (e.g., `public.ecr.aws/supabase/postgres:15.8.1.085`) — this is Supabase's standard PostgreSQL image that ships with ~30 extensions pre-installed (PostGIS, pgvector, etc.)
2. **Runs a multi-stage Docker build** using `docker/postgresql/Dockerfile.supabase`:
   - **Stage 1 (builder)**: Installs C build tools (`gcc`, `make`), copies the CloudSync source code (`src/`, `modules/`), and compiles `cloudsync.so` against Supabase's `pg_config`
   - **Stage 2 (runtime)**: Starts from a clean Supabase Postgres image and copies in just three files:
     - `cloudsync.so` — the compiled extension binary
     - `cloudsync.control` — tells PostgreSQL the extension's name and version
     - `cloudsync--1.0.sql` — the SQL that defines all CloudSync functions
3. **Tags the result** with the same name as the base image, so it's a drop-in replacement

To find the correct tag, clone the Supabase repo and check:

```bash
grep 'image: supabase/postgres:' supabase/docker/docker-compose.yml
# Example output: image: supabase/postgres:15.8.1.085
# Use the version after the colon as your SUPABASE_POSTGRES_TAG
```

Run from the sqlite-sync-dev repo root:

```bash
make postgres-supabase-build SUPABASE_POSTGRES_TAG=15.8.1.085
```

Verify the image was built:

```bash
docker images | grep supabase-postgres-cloudsync
# Should show: sqlcdamlayildiz/supabase-postgres-cloudsync   15.8.1.085   ...
```

Verify CloudSync is installed inside the image:

```bash
docker run --rm sqlcdamlayildiz/supabase-postgres-cloudsync:15.8.1.085 \
  find / -name "cloudsync*" -type f 2>/dev/null
# Should list cloudsync.so, cloudsync.control, and cloudsync--1.0.sql
# in /nix/store/...-postgresql-and-plugins-15.8/ paths
```

---

## Step 3: Build for the correct architecture and push to Docker Hub

The Fly.io VM needs to pull your custom image from a container registry. We use Docker Hub (free, no extra auth needed on the VM).

> **Important — architecture mismatch**: If you're building on an Apple Silicon Mac (M1/M2/M3/M4), `make postgres-supabase-build` produces an ARM image. Fly.io VMs run x86 (amd64) by default, so the ARM image won't work. You must build for the target architecture explicitly.

First, pull the base image for amd64 (this ensures Docker has the correct platform variant cached):

```bash
docker pull --platform linux/amd64 public.ecr.aws/supabase/postgres:15.8.1.085
```

Then build for `linux/amd64` (x86, which is what Fly.io uses):

```bash
docker build --platform linux/amd64 \
  --build-arg SUPABASE_POSTGRES_TAG=15.8.1.085 \
  -f docker/postgresql/Dockerfile.supabase \
  -t <your-dockerhub-username>/supabase-postgres-cloudsync:15.8.1.085 \
  .
```

Push the image (you must be logged in: `docker login`):

```bash
docker push <your-dockerhub-username>/supabase-postgres-cloudsync:15.8.1.085
```

> **Note**: `docker buildx build ... --push` may fail with ECR registry resolution errors. The two-step approach above (build then push) is more reliable.

> If you're building on an Intel Mac or a Linux x86 machine, `make postgres-supabase-build` already produces an amd64 image, so you can simply tag and push:
> ```bash
> docker tag public.ecr.aws/supabase/postgres:15.8.1.085 \
>   <your-dockerhub-username>/supabase-postgres-cloudsync:15.8.1.085
> docker push <your-dockerhub-username>/supabase-postgres-cloudsync:15.8.1.085
> ```

---

## Step 4: Provision a Fly.io VM

We use a Fly Machine as a plain Linux VM running Docker Compose — not Fly's container orchestration.

### 4a. Log in to Fly

```bash
fly auth login
```

This opens your browser to authenticate with your Fly.io account.

### 4b. Create a Fly app

```bash
fly apps create <your-app-name>
```

### 4c. Create a persistent volume for data

```bash
fly volumes create supabase_data --app <your-app-name> --region <region> --size 50
```

Pick a [region](https://fly.io/docs/reference/regions/) close to you. You can see all available regions with `fly platform regions`. Common choices:

| Code | Location |
|------|----------|
| `fra` | Frankfurt, Germany |
| `ams` | Amsterdam, Netherlands |
| `lhr` | London, UK |
| `ord` | Chicago, US |
| `iad` | Virginia, US |
| `sin` | Singapore |

When prompted "Do you still want to use the volumes feature?", type `y` — the warning about multiple volumes is for high-availability production setups; a single volume is fine for testing.

### 4d. Create a Fly Machine

```bash
fly machine run ubuntu:24.04 \
  --app <your-app-name> \
  --region <region> \
  --vm-size shared-cpu-4x \
  --vm-memory 4096 \
  --volume supabase_data:/data \
  --name supabase-vm \
  -- sleep inf
```

The `-- sleep inf` at the end is important — it tells the VM to run an infinite sleep process so it stays alive. Without it, the Ubuntu container exits immediately and the machine stops.

This creates an Ubuntu 24.04 VM with 4 CPU cores, 4 GB RAM, and your 50 GB volume mounted at `/data`.

The VM size (`shared-cpu-4x` + 4096 MB) meets Supabase's minimum requirements. For a test/dev deployment this is fine. You can resize later with `fly machine update` if needed.

### 4e. Allocate a public IP

```bash
fly ips allocate-v4 --shared --app <your-app-name>
fly ips allocate-v6 --app <your-app-name>
```

Note the IPv4 address — you'll need it for `SUPABASE_PUBLIC_URL`.

---

## Step 5: Set up Docker and Supabase on the VM

### 5a. SSH into the machine

```bash
fly ssh console --app <your-app-name>
```

### 5b. Install Docker Engine

```bash
apt-get update
apt-get install -y ca-certificates curl gnupg

install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | gpg --dearmor -o /etc/apt/keyrings/docker.gpg
chmod a+r /etc/apt/keyrings/docker.gpg

echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
  https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo $VERSION_CODENAME) stable" \
  > /etc/apt/sources.list.d/docker.list

apt-get update
apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin

# Verify
docker --version
docker compose version
```

### 5c. Configure Docker storage driver

Docker's default `overlayfs` storage driver doesn't work inside a Fly VM (you'll get "failed to convert whiteout file: operation not permitted" errors). Use `fuse-overlayfs` instead — it works in unprivileged environments like Fly VMs and is much faster and more space-efficient than the `vfs` fallback.

Install fuse-overlayfs:

```bash
apt-get install -y fuse-overlayfs
```

Configure Docker to use it:

```bash
mkdir -p /etc/docker
echo '{"storage-driver":"fuse-overlayfs","data-root":"/data/docker"}' > /etc/docker/daemon.json
```

> **Why not `vfs`?** The `vfs` driver copies every image layer in full instead of sharing them. This means 13 Supabase images can use 35GB+ of disk (vs ~5-8GB with fuse-overlayfs), and image pulls/extraction are extremely slow (30-60 minutes vs a few minutes). Avoid `vfs` unless fuse-overlayfs doesn't work.

### 5d. Start the Docker daemon

The Fly VM doesn't auto-start Docker. You need to start it manually:

```bash
dockerd &
```

Wait for the `API listen on /var/run/docker.sock` message before running any Docker commands. This message means Docker is ready.

> **Note**: If Docker is already running, you'll see a "process is still running" error. That's fine — it means Docker is already available.

### 5e. Clone and set up Supabase

```bash
cd /data
git clone --depth 1 https://github.com/supabase/supabase
mkdir -p supabase-docker
cp -rf supabase/docker/* supabase-docker/
cp supabase/docker/.env.example supabase-docker/.env
cd supabase-docker
```

---

## Step 6: Configure secrets

### 6a. Generate keys automatically

```bash
sh ./utils/generate-keys.sh
```

Review the output. The script updates `.env` with generated `JWT_SECRET`, `ANON_KEY`, and `SERVICE_ROLE_KEY`.

### 6a.1. Get the JWT secret later

If you need the Supabase Auth JWT secret after setup, read the `JWT_SECRET` value from the same `.env` file used by Docker Compose:

```bash
cd /data/supabase-docker
grep '^JWT_SECRET=' .env
```

That value is the secret GoTrue (Supabase Auth) uses to sign and verify access tokens.

If you want to confirm what the running auth container sees, check the container environment:

```bash
docker compose exec auth printenv GOTRUE_JWT_SECRET
```

Both commands should return the same value. If they do not, restart the stack after updating `.env`:

```bash
docker compose up -d
```

### 6b. Edit `.env` manually for remaining values

```bash
# Install a text editor if needed
apt-get install -y nano
nano .env
```

Set these values:

```env
############
# Required #
############

# Database — letters and numbers only, no special characters
POSTGRES_PASSWORD=<strong-random-password>

# URLs — replace with your Fly app's public IP or domain
SUPABASE_PUBLIC_URL=http://<your-fly-ip>:8000
API_EXTERNAL_URL=http://<your-fly-ip>:8000
SITE_URL=http://localhost:3000

# Dashboard login credentials
DASHBOARD_USERNAME=supabase
DASHBOARD_PASSWORD=<your-dashboard-password>

############
# Secrets  #
############
# These should already be set by generate-keys.sh, but verify they exist:
# JWT_SECRET=<generated>
# ANON_KEY=<generated>
# SERVICE_ROLE_KEY=<generated>

# Generate the rest if not already set:
# openssl rand -base64 48   → SECRET_KEY_BASE
# openssl rand -hex 16      → VAULT_ENC_KEY (must be exactly 32 chars)
# openssl rand -base64 24   → PG_META_CRYPTO_KEY
# openssl rand -base64 24   → LOGFLARE_PUBLIC_ACCESS_TOKEN
# openssl rand -base64 24   → LOGFLARE_PRIVATE_ACCESS_TOKEN
# openssl rand -hex 16      → S3_PROTOCOL_ACCESS_KEY_ID
# openssl rand -hex 32      → S3_PROTOCOL_ACCESS_KEY_SECRET
# openssl rand -hex 16      → MINIO_ROOT_PASSWORD
```

---

## Step 7: Swap in the CloudSync Postgres image

Edit `docker-compose.yml` and find the `db` service (near the top). Replace the `image` line:

```yaml
services:
  db:
    # BEFORE: image: supabase/postgres:${POSTGRES_VERSION}
    # AFTER:
    image: <your-dockerhub-username>/supabase-postgres-cloudsync:15.8.1.085
```

Use the exact image path you pushed in Step 3.

### Add the CloudSync init script

Create the init SQL:

```bash
cat > volumes/db/cloudsync.sql << 'EOF'
CREATE EXTENSION IF NOT EXISTS cloudsync;
EOF
```

Add a volume mount to the `db` service in `docker-compose.yml`:

```yaml
services:
  db:
    volumes:
      # ... existing volume mounts ...
      - ./volumes/db/cloudsync.sql:/docker-entrypoint-initdb.d/init-scripts/100-cloudsync.sql:Z
```

The `100-` prefix ensures CloudSync loads after Supabase's own init scripts (numbered 97-99).

> **Important**: Init scripts only run when the data directory is empty (first start). If you've already started Postgres once and need to add the extension, connect and run `CREATE EXTENSION cloudsync;` manually.

---

## Step 8: Start Supabase

```bash
cd /data/supabase-docker
docker compose pull
docker compose up -d
```

Wait ~1 minute for all services to start, then verify:

```bash
docker compose ps
```

All services should show `Up (healthy)`. If any service is unhealthy:

```bash
docker compose logs <service-name>
```

### Fix: services fail with "password authentication failed"

If you see `FATAL: password authentication failed for user "authenticator"` (or `supabase_auth_admin`, `supabase_storage_admin`, `supabase_admin`) in the logs, the database users were created with a different password than what's in `.env`. This happens when `POSTGRES_PASSWORD` was changed after the first start, or when the DB data persists across reinstalls.

Fix by updating all user passwords to match your `.env`:

```bash
# Replace YOUR_PASSWORD with the value of POSTGRES_PASSWORD from your .env file
docker compose exec db psql -U postgres -c "ALTER USER authenticator WITH PASSWORD 'YOUR_PASSWORD';"
docker compose exec db psql -U postgres -c "ALTER USER supabase_auth_admin WITH PASSWORD 'YOUR_PASSWORD';"
docker compose exec db psql -U postgres -c "ALTER USER supabase_storage_admin WITH PASSWORD 'YOUR_PASSWORD';"
docker compose exec db psql -U postgres -c "ALTER USER supabase_admin WITH PASSWORD 'YOUR_PASSWORD';"
```

Then restart:

```bash
docker compose restart
```

### Fix: analytics (Logflare) keeps crashing

The analytics service (Logflare) often fails in self-hosted setups due to migration issues. Since it's **optional** (only used for log analytics, not required for CloudSync or core Supabase features), the simplest fix is to disable it.

In `docker-compose.yml`:

1. **Remove the `analytics` dependency** from the `studio` service's `depends_on` block. Delete these lines:
   ```yaml
         analytics:
           condition: service_healthy
   ```
   If `depends_on:` becomes empty after removing, delete the `depends_on:` line too.

2. **Comment out the `LOGFLARE_URL`** environment variable in the `studio` service:
   ```yaml
         # LOGFLARE_URL: http://analytics:4000
   ```

3. **Restart**:
   ```bash
   docker compose stop analytics
   docker compose up -d
   ```

### Fix: CloudSync extension not found

If `SELECT cloudsync_version();` returns "function does not exist", the init script didn't run (it only runs on first boot when the data directory is empty). Create the extension manually:

```bash
docker compose exec db psql -U postgres -c "CREATE EXTENSION IF NOT EXISTS cloudsync;"
```

### Fix: Auth service crashes with "must be owner of function uid"

When the auth (GoTrue) service fails to start and logs show errors like:

```
error executing migrations: must be owner of function uid (SQLSTATE 42501)
```

This happens because the `auth.uid()`, `auth.role()`, and `auth.email()` functions were created by `postgres` (via the init scripts or CloudSync extension), but the auth service runs migrations as `supabase_auth_admin` and expects to own those functions.

**Symptoms:**
- Auth service keeps restarting
- Supabase Studio shows "Failed to retrieve users" or "column users.banned_until does not exist" (because auth migrations didn't complete)
- `docker compose logs auth` shows the `SQLSTATE 42501` ownership error

**Fix:** Transfer ownership of the functions to `supabase_auth_admin`:

```bash
docker compose exec db psql -U postgres -c "ALTER FUNCTION auth.uid() OWNER TO supabase_auth_admin;"
docker compose exec db psql -U postgres -c "ALTER FUNCTION auth.role() OWNER TO supabase_auth_admin;"
docker compose exec db psql -U postgres -c "ALTER FUNCTION auth.email() OWNER TO supabase_auth_admin;"
docker compose restart auth
```

After restarting, wait ~30 seconds and check that auth is healthy:

```bash
docker compose ps auth
docker compose logs --tail=20 auth
```

You should see auth in a `healthy` state and the migrations completing successfully.

### Fix: Supavisor (connection pooler) keeps crashing

If `docker compose logs supavisor` shows:

```
Setting RLIMIT_NOFILE to 100000
/app/limits.sh: line 6: ulimit: open files: cannot modify limit: Operation not permitted
```

This happens because Supavisor's startup script (`/app/limits.sh`) tries to increase the open-file limit to 100,000, but the Fly VM's kernel has a lower cap (typically 10,240) and doesn't allow it. The script failure crashes the container.

**Fix:** Override the entrypoint in `docker-compose.yml` to skip the limits script. Add this line right after `container_name: supabase-pooler`:

```yaml
    entrypoint: ["/usr/bin/tini", "-s", "-g", "--"]
```

Or apply with sed:

```bash
sed -i '/container_name: supabase-pooler/a\    entrypoint: ["/usr/bin/tini", "-s", "-g", "--"]' /data/supabase-docker/docker-compose.yml
```

This keeps the same `tini` init process but skips `/app/limits.sh`. The VM's default open-file limit (10,240) is sufficient for testing.

Then restart:

```bash
docker compose up -d supavisor
```

---

## Step 9: Verify CloudSync

Connect to the database:

```bash
docker compose exec db psql -U postgres
```

```sql
-- Check the extension is installed
SELECT * FROM pg_extension WHERE extname = 'cloudsync';

-- Check version
SELECT cloudsync_version();
```

If `cloudsync_version()` returns "function does not exist", create the extension manually:

```sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
```

---

## Step 10: Test CloudSync Sync

This section walks through testing the full sync flow: registering the database with the CloudSync server, creating tables, enabling sync, and running a roundtrip test.

### Prerequisites

You need a running **CloudSync server**. This can be the staging server or a local instance.

```bash
export CLOUDSYNC_URL="https://cloudsync-staging-testing.fly.dev"  # CloudSync server URL
export ORG_API_KEY="<your-org-api-key>"                          # Organization API key
```

#### Connection string

The CloudSync server needs a PostgreSQL connection string to reach your database. There are two options depending on where your CloudSync server runs:

**Option A: CloudSync on the same Fly org (`.internal` network)**

If both the CloudSync server and the Supabase VM are in the same Fly org, they can communicate over Fly's **private internal network** — no public port exposure needed. Connect directly to the `db` container's mapped port (5432 is exposed on the host by default in docker-compose):

```bash
# Direct connection (no Supavisor) — recommended for CloudSync server-to-server
export CONNECTION_STRING="postgres://postgres:$POSTGRES_PASSWORD@<your-fly-app-name>.internal:5432/postgres"
```

**Option B: CloudSync running outside Fly (e.g., local machine, another cloud)**

Use `fly proxy` to tunnel the Postgres port to your local machine:

```bash
# In a separate terminal — keep this running
fly proxy 5432:5432 -a <your-fly-app-name>
```

This makes the remote Postgres available at `localhost:5432`. Then use:

```bash
export CONNECTION_STRING="postgres://postgres:$POSTGRES_PASSWORD@localhost:5432/postgres"
```

> **Note:** The proxy must stay running in a separate terminal for the duration of your session. If the proxy disconnects, just re-run the command.

To verify the connection works:

```bash
# Option A: SSH into the VM and test locally
fly ssh console --app <your-fly-app-name>
docker compose exec db psql -U postgres -c "SELECT 1;"

# Option B: With fly proxy running, test from your local machine
psql "postgres://postgres:$POSTGRES_PASSWORD@localhost:5432/postgres" -c "SELECT 1;"
```

### 10a. Verify CloudSync server is reachable

```bash
curl "$CLOUDSYNC_URL/healthz"
# Expected: {"status":"ok"}
```

### 10b. Register the Supabase database with CloudSync

This tells CloudSync where to find your PostgreSQL database:

```bash
curl --request POST "$CLOUDSYNC_URL/v1/databases" \
  --header "Authorization: Bearer $ORG_API_KEY" \
  --header "Content-Type: application/json" \
  --data '{
    "label": "Supabase Fly.io Test",
    "connectionString": "'"$CONNECTION_STRING"'",
    "provider": "postgres",
    "flavor": "supabase",
    "projectId": "cloudsync-supabase-test",
    "databaseName": "postgres"
  }'
```

Save the returned `managedDatabaseId` — you'll need it for all subsequent operations:

```bash
export MANAGED_DATABASE_ID="<returned-id>"
```

### 10c. Verify database connectivity

```bash
curl --request POST "$CLOUDSYNC_URL/v1/databases/$MANAGED_DATABASE_ID/verify" \
  --header "Authorization: Bearer $ORG_API_KEY"
```

Expected: status should show the database is reachable.

### 10d. Create a test table on the Supabase database

SSH into the Fly VM and create a table:

```bash
docker compose exec db psql -U postgres -c "
CREATE TABLE IF NOT EXISTS todos (
  id TEXT PRIMARY KEY DEFAULT cloudsync_uuid(),
  title TEXT NOT NULL DEFAULT '',
  done BOOLEAN DEFAULT false
);
SELECT cloudsync_init('todos');
"
```

### 10e. Enable CloudSync on the table

From your local machine, enable sync via the management API:

```bash
curl --request POST "$CLOUDSYNC_URL/v1/databases/$MANAGED_DATABASE_ID/cloudsync/enable" \
  --header "Authorization: Bearer $ORG_API_KEY" \
  --header "Content-Type: application/json" \
  --data '{"tables":["todos"]}'
```

Verify:

```bash
curl --request GET "$CLOUDSYNC_URL/v1/databases/$MANAGED_DATABASE_ID/cloudsync/tables" \
  --header "Authorization: Bearer $ORG_API_KEY"
```

The `todos` table should show `"enabled": true`.

### 10f. Test sync roundtrip from a SQLite client

On your local machine, create a SQLite database and sync:

```sql
-- Load the sqlite-sync extension
.load path/to/cloudsync

-- Create the same table schema
CREATE TABLE todos (
  id TEXT PRIMARY KEY DEFAULT (cloudsync_uuid()),
  title TEXT NOT NULL DEFAULT '',
  done BOOLEAN DEFAULT false
);
SELECT cloudsync_init('todos');

-- Configure network
SELECT cloudsync_network_init('<managed-database-id>');
SELECT cloudsync_network_set_token('<your-auth-token>');

-- Insert a row locally
INSERT INTO todos (title) VALUES ('Test from SQLite');

-- Sync: send local changes, check for remote changes
SELECT cloudsync_network_sync(500, 5);
```

Then verify the row arrived on Supabase:

```bash
docker compose exec db psql -U postgres -c "SELECT * FROM todos;"
```

### 10g. Test reverse sync (Supabase → SQLite)

Insert a row directly on Supabase:

```bash
docker compose exec db psql -U postgres -c "
INSERT INTO todos (id, title, done) VALUES (cloudsync_uuid(), 'Test from Supabase', false);
"
```

Then sync from the SQLite client:

```sql
SELECT cloudsync_network_check_changes();
SELECT * FROM todos;
```

The row from Supabase should appear in SQLite.

---

## Step 11: Access your services

| Service | URL |
|---------|-----|
| **Supabase Studio** | `http://<your-fly-ip>:8000` |
| REST API | `http://<your-fly-ip>:8000/rest/v1/` |
| Auth API | `http://<your-fly-ip>:8000/auth/v1/` |
| Storage API | `http://<your-fly-ip>:8000/storage/v1/` |
| Realtime | `http://<your-fly-ip>:8000/realtime/v1/` |

Studio dashboard requires a username and password. To find them, check your `.env` file on the VM:

```bash
grep DASHBOARD /data/supabase-docker/.env
```

The values are `DASHBOARD_USERNAME` (default: `supabase`) and `DASHBOARD_PASSWORD` (default: `this_password_is_insecure_and_should_be_updated`).

> **Note:** The Fly VM doesn't expose ports publicly by default. Use `fly proxy` to access services from your local machine:
> ```bash
> fly proxy 8000:8000 -a <your-app-name>
> ```
> Then open `http://localhost:8000` in your browser.

### Connect to Postgres directly

Use `fly proxy` to tunnel the Postgres port to your local machine:

```bash
# In a separate terminal — keep this running
fly proxy 5432:5432 -a <your-app-name>
```

Then connect from your local machine:

```bash
psql 'postgres://postgres:<POSTGRES_PASSWORD>@localhost:5432/postgres'
```

> **Tip:** You can proxy multiple ports at once by running multiple `fly proxy` commands in separate terminals (e.g., `8000` for Studio and `5432` for Postgres).

---

## Step 11: Set up HTTPS (production)

For production use, put a reverse proxy in front of Kong. The simplest option is [Caddy](https://caddyserver.com/) which handles TLS automatically.

On the Fly VM:

```bash
apt-get install -y debian-keyring debian-archive-keyring apt-transport-https
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' | tee /etc/apt/sources.list.d/caddy-stable.list
apt-get update
apt-get install -y caddy
```

Create `/etc/caddy/Caddyfile`:

```
your-domain.com {
    reverse_proxy localhost:8000
}
```

```bash
systemctl enable caddy
systemctl start caddy
```

Then update `.env`:

```env
SUPABASE_PUBLIC_URL=https://your-domain.com
API_EXTERNAL_URL=https://your-domain.com
```

Restart Supabase:

```bash
cd /data/supabase-docker
docker compose down && docker compose up -d
```

---

## Maintenance

### Update Supabase services

```bash
cd /data/supabase-docker
# Update image tags in docker-compose.yml, then:
docker compose pull
docker compose down && docker compose up -d
```

### Update CloudSync extension

On your local machine, rebuild and push the image:

```bash
cd /path/to/sqlite-sync-dev
git pull  # get latest CloudSync code
git submodule update --init --recursive  # ensure submodules are up to date
make postgres-supabase-build SUPABASE_POSTGRES_TAG=15.8.1.085
docker tag public.ecr.aws/supabase/postgres:15.8.1.085 \
  <your-dockerhub-username>/supabase-postgres-cloudsync:15.8.1.085
docker push <your-dockerhub-username>/supabase-postgres-cloudsync:15.8.1.085
```

On the Fly VM:

```bash
cd /data/supabase-docker
docker compose pull db
docker compose up -d db
```

### View logs

```bash
# All services
docker compose logs -f

# Specific service
docker compose logs -f db
```

### Change database password

```bash
cd /data/supabase-docker
sh ./utils/db-passwd.sh
docker compose up -d --force-recreate
```

### Stop and restart the Fly machine

The Fly machine's root filesystem resets on every stop/start — only the `/data` volume persists. This means Docker must be reinstalled each time. To automate this, create a startup script (run once):

```bash
cat > /data/startup.sh << 'SCRIPT'
#!/bin/bash
set -e

echo "=== Installing Docker ==="
apt-get update && apt-get install -y ca-certificates curl gnupg fuse-overlayfs

install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | gpg --dearmor -o /etc/apt/keyrings/docker.gpg
chmod a+r /etc/apt/keyrings/docker.gpg

echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
  https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo $VERSION_CODENAME) stable" \
  > /etc/apt/sources.list.d/docker.list

apt-get update && apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin

echo "=== Configuring Docker ==="
mkdir -p /etc/docker
echo '{"storage-driver":"fuse-overlayfs","data-root":"/data/docker"}' > /etc/docker/daemon.json

echo "=== Starting Docker ==="
dockerd > /data/dockerd.log 2>&1 &

echo "Waiting for Docker to be ready..."
until docker info > /dev/null 2>&1; do sleep 1; done
echo "Docker is ready!"

echo "=== Starting Supabase ==="
cd /data/supabase-docker
docker compose up -d

echo "=== Done! ==="
echo "Run 'docker compose ps' to check service status"
SCRIPT
chmod +x /data/startup.sh
```

From then on, every time you restart the machine:

```bash
# From your local machine:
fly machine stop 287920ea023108 -a <your-app-name>   # stop
fly machine start 287920ea023108 -a <your-app-name>  # start
fly ssh console --app <your-app-name>

# On the VM — one command does everything:
/data/startup.sh
```

Docker images and Supabase data are on `/data`, so they survive restarts. Only Docker itself needs reinstalling (~1-2 minutes).

### Stop Supabase (without stopping the machine)

```bash
docker compose down
```

### Destroy everything (irreversible)

```bash
docker compose down -v
rm -rf volumes/db/data volumes/storage
```

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| `fractional_indexing.h: No such file or directory` | Run `git submodule update --init --recursive` before building. The fractional-indexing library is a git submodule that must be initialized. |
| `cloudsync.so: cannot open shared object file` | The custom image wasn't used. Verify `docker compose ps` shows your image, not the default one. |
| Init script didn't run / `cloudsync_version()` not found | Init scripts only run on first start. Run `CREATE EXTENSION IF NOT EXISTS cloudsync;` manually via `docker compose exec db psql -U postgres`. |
| `password authentication failed for user "authenticator"` | Database users have a different password than `.env`. See "Fix: services fail with password authentication failed" above. |
| Analytics (Logflare) keeps crashing | Disable it — see "Fix: analytics keeps crashing" above. It's optional and not needed for CloudSync. |
| `cannot stop container ... did not receive an exit event` | Zombie container. Kill Docker: `kill -9 $(pidof dockerd) $(pidof containerd)`, remove container files: `rm -rf /data/docker/containers/<container-id>*`, restart dockerd. |
| Services unhealthy after start | Wait 2 minutes. Check `docker compose logs <service>`. Most common: password mismatch (see fix above). |
| Can't pull custom image from VM | Make sure the image is public on Docker Hub, or run `docker login` on the VM. |
| ARM build errors | Fly defaults to x86. If using ARM machines, rebuild with `--platform linux/arm64`. |
| Version mismatch | Run `SHOW server_version;` in psql and ensure your `SUPABASE_POSTGRES_TAG` matches the major version. |
| `no space left on device` during pull | The `fuse-overlayfs` driver is efficient but if disk fills up, extend the volume: `fly volumes extend <vol-id> --size 80 -a <app>`. |
| Docker commands not found after machine restart | The Fly VM root filesystem resets on stop/start. Run `/data/startup.sh` to reinstall Docker. See "Stop and restart the Fly machine" section. |
| Auth crashes with `must be owner of function uid` | Transfer function ownership: `ALTER FUNCTION auth.uid() OWNER TO supabase_auth_admin;` and same for `auth.role()` and `auth.email()`. See "Fix: Auth service crashes" above. |
| Studio shows "column users.banned_until does not exist" | Auth migrations didn't complete. Fix the auth ownership issue above and restart auth. |
| Supavisor crashes with `ulimit: cannot modify limit` | Override entrypoint to skip `/app/limits.sh`. See "Fix: Supavisor keeps crashing" above. |
