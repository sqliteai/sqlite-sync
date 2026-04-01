# CloudSync Quick Start: Self-Hosted PostgreSQL

This guide helps you enable CloudSync on a **self-hosted PostgreSQL database**. CloudSync adds offline-first synchronization capabilities to your PostgreSQL database.

## Step 1: Deploy PostgreSQL with CloudSync

You can enable CloudSync in one of two ways:
- Use the published Docker image if you run PostgreSQL in Docker
- Install the released extension files into an existing native PostgreSQL installation

### Option A: Docker

Use the published PostgreSQL image that already includes the CloudSync extension:
- `sqlitecloud/sqlite-sync-postgres:15`
- `sqlitecloud/sqlite-sync-postgres:17`

Example using Docker Compose:

```yaml
services:
  db:
    image: sqlitecloud/sqlite-sync-postgres:17
    container_name: cloudsync-postgres
    environment:
      POSTGRES_USER: postgres
      POSTGRES_PASSWORD: your-secure-password
      POSTGRES_DB: postgres
    ports:
      - "5432:5432"
    volumes:
      - pg_data:/var/lib/postgresql/data
      - ./init.sql:/docker-entrypoint-initdb.d/init.sql:ro

volumes:
  pg_data:
```

Create `init.sql`:
```sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
```

Run:
```bash
docker compose up -d
```

### Option B: Existing PostgreSQL Without Docker

If you already run PostgreSQL directly on a VM or bare metal, download the release tarball that matches your operating system, CPU architecture, and PostgreSQL major version.

Extract the archive, then copy the three extension files into PostgreSQL's extension directories:

```bash
cp cloudsync.so "$(pg_config --pkglibdir)/"
cp cloudsync.control cloudsync--1.0.sql "$(pg_config --sharedir)/extension/"
```

Then connect to PostgreSQL and enable the extension:

```sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
```

---

## Step 2: Verify the Extension

If you are using Docker:

```bash
docker compose exec db psql -U postgres -d postgres -c "SELECT cloudsync_version();"
```

If you are using an existing PostgreSQL installation without Docker:

```bash
psql -U postgres -d postgres -c "SELECT cloudsync_version();"
```

If the extension is installed correctly, PostgreSQL returns the CloudSync version string.

---

## Step 3: Register Your Database in the CloudSync Dashboard

### 2.1 Create a Workspace

1. Log in to the CloudSync dashboard
2. Click **New Workspace**
3. Choose provider: **PostgreSQL**
4. Enter a workspace name
5. Click **Create**

### 2.2 Create a CloudSync Project

1. In the workspace, click **New CloudSync Project**
2. **Project name:** Give it a descriptive name
3. **Connection string:** Enter your PostgreSQL connection:
   ```
   postgresql://user:password@host:5432/database
   ```
5. Click **Create**

---

## Step 4: Set Up Authentication

### Quick Test with API Key (Recommended for Testing)

The fastest way to test CloudSync without per-user access control — no JWT setup needed.

1. Get PostgreSQL credentials for a user with sufficient permissions on the tables CloudSync will sync
2. In your client code (SQLite), authenticate with:
   ```sql
   SELECT cloudsync_network_init('<database-id>');
   SELECT cloudsync_network_set_apikey('<username>:<password>');
   SELECT cloudsync_network_sync();
   ```

The API key is simply your PostgreSQL credentials in `username:password` format. No JWT configuration needed.

### Using JWT Tokens (For RLS and Production)

If you need role-based access control (RLS) or production security:

1. **Prepare your JWT authentication:**
   - **For HS256 (shared secret):** Use your existing secret, or generate one: `openssl rand -base64 32`
   - **For RS256 (asymmetric/JWKS):** Have your auth system's issuer URL and JWKS endpoint ready
2. **Update CloudSync dashboard configuration:**
   - Go to your project's **Configuration** tab
   - Click **Edit connection settings**
   - **For HS256 (shared secret):** Enter your JWT secret
   - **For RS256 (asymmetric):** Enter the same issuer base URL used in the token's `iss` claim, for example `https://your-auth-domain`
   - Do not include `/.well-known/jwks.json` in this field. CloudSync uses the issuer URL and fetches the JWKS document automatically.
   - Click **Save**
3. **Get JWT tokens** — choose one:
   - **Testing only:** [jwt.io](https://jwt.io/) — paste your JWT secret, generate a test token
   - **Existing auth system:** Use tokens from your current auth provider
4. **In your client code:**
   ```sql
   SELECT cloudsync_network_init('<database-id>');
   SELECT cloudsync_network_set_token('<jwt-token>');
   SELECT cloudsync_network_sync();
   ```

---

## Step 5: Enable CloudSync on Tables

1. In the CloudSync dashboard, go to the **Sync Tables** tab
2. **Select tables** you want to sync (checkbox each table)
3. Click **Deploy Changes**

CloudSync is now active on your selected tables.
