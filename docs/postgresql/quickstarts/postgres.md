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

Extract the archive, then copy the extension files into PostgreSQL's extension directories. The tarball ships `cloudsync.control`, a `cloudsync--<version>.sql` install script for the current release, and — from release 1.0.17 onward — any `cloudsync--<from>--<to>.sql` upgrade scripts needed so existing installations can run `ALTER EXTENSION cloudsync UPDATE`.

```bash
cp cloudsync.so "$(pg_config --pkglibdir)/"
cp cloudsync.control cloudsync--*.sql "$(pg_config --sharedir)/extension/"
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

### Upgrading a later release

CloudSync uses the first two components of its semver as the PostgreSQL extension version (for example, `1.0.17` installs as extension version `1.0`). How you upgrade depends on which component changed:

- **PATCH release** (e.g. `1.0.17 → 1.0.18`): pull the new Docker image or replace the extension files on disk and restart PostgreSQL. No SQL-level upgrade is needed — `installed_version` stays at `1.0` and the new binary takes over on reconnect. `SELECT cloudsync_version();` confirms the new semver.
- **MINOR or MAJOR release** (e.g. `1.0.x → 1.1.0`): pull the new artifacts as above, then run once per database:

  ```sql
  ALTER EXTENSION cloudsync UPDATE;
  ```

  PostgreSQL applies any `cloudsync--<from>--<to>.sql` upgrade scripts shipped with the release and moves `installed_version` to the new value.

You can check the current state at any time:

```sql
SELECT name, default_version, installed_version
FROM pg_available_extensions
WHERE name = 'cloudsync';
```

If `installed_version` is behind `default_version` after a release, run `ALTER EXTENSION cloudsync UPDATE;` to catch up.

---

## Step 3: Register Your Database in the CloudSync Dashboard

In the [CloudSync dashboard](https://dashboard.sqlitecloud.io/), create a new workspace with the **PostgreSQL** provider, then add a project with your PostgreSQL connection string:

```
postgresql://user:password@host:5432/database
```

---

## Step 4: Enable CloudSync on Tables

In the dashboard, go to the **Database Setup** tab, select the tables you want to sync, and click **Deploy Changes**.

---

## Step 5: Set Up Authentication

On the **Client Integration** tab you'll find your **Database ID** and authentication settings.

### Quick Test with API Key (Recommended for Testing)

The fastest way to test CloudSync without per-user access control — no JWT setup needed.

With API key authentication, CloudSync uses the database role resolved from the API-key-authenticated connection when available; otherwise it falls back to the role from the connection string.

```sql
SELECT cloudsync_network_init('<database-id>');
SELECT cloudsync_network_set_apikey('<username>:<password>');
SELECT cloudsync_network_sync();
```

### Using JWT Tokens (For RLS and Production)

1. Set **Row Level Security** to **Yes, enforce RLS**
2. Under **Authentication (JWT)**, click **Configure authentication** and choose:
   - **HMAC Secret (HS256):**
     - Enter your JWT secret (or generate one: `openssl rand -base64 32`)
     - Optionally add **Expected audiences**. When configured, a token's `aud` claim must contain at least one of the configured audience values.
   - **JWKS Issuer Validation:**
     - Enter the issuer base URL from your token's `iss` claim (for example `https://your-auth-domain`)
     - By default, CloudSync uses OIDC discovery: it requests `<issuer>/.well-known/openid-configuration` and reads the returned `jwks_uri`
     - Optionally set an **Explicit JWKS URI** to bypass OIDC discovery and use a specific JWKS endpoint directly. This must be a full HTTPS URI.
     - Optionally add **Expected audiences**. When configured, a token's `aud` claim must contain at least one of the configured audience values.
3. CloudSync validates JWTs as follows:
   - **HS256:** uses the configured JWT secret
   - **JWKS:** uses the explicit `jwksUri` when provided; otherwise CloudSync requests `<issuer>/.well-known/openid-configuration` and reads `jwks_uri`
   - CloudSync does not fall back directly to `<issuer>/.well-known/jwks.json` when discovery is used
4. For claim details and RLS examples, see:
   - [JWT Claims Reference](../reference/jwt-claims.md)
   - [RLS Reference](../reference/rls.md)
5. In your client code:
   ```sql
   SELECT cloudsync_network_init('<database-id>');
   SELECT cloudsync_network_set_token('<jwt-token>');
   SELECT cloudsync_network_sync();
   ```
