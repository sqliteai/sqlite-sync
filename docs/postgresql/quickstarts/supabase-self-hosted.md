# CloudSync Quick Start: Self-Hosted Supabase

This guide helps you enable CloudSync on a **fresh or existing** self-hosted Supabase instance. CloudSync adds offline-first synchronization capabilities to your PostgreSQL database.

## Step 1: Use the CloudSync Supabase Image

When deploying or updating your Supabase instance, use the published CloudSync Supabase image instead of the standard Supabase Postgres image.

### For New Deployments

Follow [Supabase's Installing Supabase](https://supabase.com/docs/guides/self-hosting/docker#installing-supabase) guide to set up the initial files and `.env` configuration. Then, before the first `docker compose up -d`, update your `docker-compose.yml` to use the CloudSync-enabled Postgres image:

```yaml
db:
  # Supabase on PostgreSQL 15
  image: sqlitecloud/sqlite-sync-supabase:15
  # instead of: public.ecr.aws/supabase/postgres:15.8.1.135

  # OR Supabase on PostgreSQL 17
  image: sqlitecloud/sqlite-sync-supabase:17
  # instead of: public.ecr.aws/supabase/postgres:17.6.1.071

  # OR Supabase on PostgreSQL 17 with a newer Alpine-based Supabase image
  image: sqlitecloud/sqlite-sync-supabase:17-alpine
  # instead of: public.ecr.aws/supabase/postgres:17.6.1.151
```

Use the CloudSync image tag that matches your Supabase PostgreSQL major version. The published major tags `sqlitecloud/sqlite-sync-supabase:15` and `sqlitecloud/sqlite-sync-supabase:17` are the standard choice.

**Ubuntu vs Alpine base:** newer Supabase Postgres images (roughly `17.6.1.084` and later) ship an Alpine-based userland instead of the earlier Ubuntu one. The extension itself is identical on both — the PostgreSQL binary is glibc-linked in either case — so functionally they are interchangeable. If your Supabase stack pins one of these newer Alpine base images, use the matching `sqlitecloud/sqlite-sync-supabase:17-alpine` tag (or the exact base tag, e.g. `sqlitecloud/sqlite-sync-supabase:17.6.1.151`) so the CloudSync image is built from the same base. Exact Supabase base-image tags are published for both families but are optional for normal setup.

### Add the CloudSync Init Script

Create the init SQL:

```bash
mkdir -p volumes/db
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

The `100-` prefix ensures CloudSync loads after Supabase's own init scripts, which are typically numbered `98-99` in the self-hosted Docker Compose setup.

Then start Supabase:

```bash
docker compose pull
docker compose up -d
```

### For Existing Deployments

Follow [Supabase's Updating](https://supabase.com/docs/guides/self-hosting/docker#updating) guide. When updating the Postgres image, replace the default image with the matching CloudSync image:

```bash
# Update docker-compose.yml to use:
# sqlitecloud/sqlite-sync-supabase:15
# or sqlitecloud/sqlite-sync-supabase:17
# or sqlitecloud/sqlite-sync-supabase:17-alpine (newer Alpine-based Supabase bases)
docker compose pull
docker compose down && docker compose up -d
```

If Postgres has already been initialized and you are adding CloudSync afterward, the init script will not run automatically. Connect to the database and run:

```sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
```

---

## Step 2: Verify the Extension

```bash
docker compose exec db psql -U supabase_admin -d postgres -c "SELECT cloudsync_version();"
```

If the extension is installed correctly, PostgreSQL returns the CloudSync version string.

### Upgrading a later release

CloudSync uses the first two components of its semver as the PostgreSQL extension version (for example, `1.0.17` installs as extension version `1.0`). How you upgrade depends on which component changed:

- **PATCH release** (e.g. `1.0.17 → 1.0.18`): pull the matching `sqlitecloud/sqlite-sync-supabase:<tag>` image and restart the `db` service. No SQL-level upgrade is needed — `installed_version` stays at `1.0` and the new binary takes over on reconnect. `SELECT cloudsync_version();` confirms the new semver.
- **MINOR or MAJOR release** (e.g. `1.0.x → 1.1.0`): pull the new image and restart as above, then run once per database:

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

In the [CloudSync dashboard](https://dashboard.sqlitecloud.io/), create a new workspace with the **Supabase (Self-hosted)** provider, then add a project with your PostgreSQL connection string:

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
     - Enter your `JWT_SECRET` from Supabase's `.env`
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
