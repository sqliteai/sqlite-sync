# CloudSync Quick Start: Self-Hosted Supabase

This guide helps you enable CloudSync on a **fresh or existing** self-hosted Supabase instance. CloudSync adds offline-first synchronization capabilities to your PostgreSQL database.

## Step 1: Use the CloudSync Supabase Image

When deploying or updating your Supabase instance, use the published CloudSync Supabase image instead of the standard Supabase Postgres image.

### For New Deployments

Follow [Supabase's Installing Supabase](https://supabase.com/docs/guides/self-hosting/docker#installing-supabase) guide to set up the initial files and `.env` configuration. Then, before the first `docker compose up -d`, update your `docker-compose.yml` to use the CloudSync-enabled Postgres image:

```yaml
db:
  # Supabase on PostgreSQL 15
  image: sqlitecloud/sqlite-sync-supabase:15.8.1.085
  # instead of: public.ecr.aws/supabase/postgres:15.8.1.085

  # OR Supabase on PostgreSQL 17
  image: sqlitecloud/sqlite-sync-supabase:17.6.1.071
  # instead of: public.ecr.aws/supabase/postgres:17.6.1.071
```

Use the tag that matches your Supabase Postgres base image exactly. Convenience tags `sqlitecloud/sqlite-sync-supabase:15` and `sqlitecloud/sqlite-sync-supabase:17` are also published, but the exact Supabase tag is the safest choice.

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
# sqlitecloud/sqlite-sync-supabase:15.8.1.085
# or sqlitecloud/sqlite-sync-supabase:17.6.1.071
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

```sql
SELECT cloudsync_network_init('<database-id>');
SELECT cloudsync_network_set_apikey('<username>:<password>');
SELECT cloudsync_network_sync();
```

### Using JWT Tokens (For RLS and Production)

1. Set **Row Level Security** to **Yes, enforce RLS**
2. Under **Authentication (JWT)**, click **Configure authentication** and choose:
   - **HMAC Secret (HS256):** Enter your `JWT_SECRET` from Supabase's `.env`
   - **JWKS Issuer Validation:** Enter the issuer base URL from your token's `iss` claim (e.g. `https://your-auth-domain`). CloudSync automatically fetches the JWKS document from `<issuer-url>/.well-known/jwks.json`
3. In your client code:
   ```sql
   SELECT cloudsync_network_init('<database-id>');
   SELECT cloudsync_network_set_token('<jwt-token>');
   SELECT cloudsync_network_sync();
   ```
