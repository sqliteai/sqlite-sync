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

## Step 2: Register Your Database in the CloudSync Dashboard

### 2.1 Create a Workspace

1. Log in to the CloudSync dashboard
2. Click **New Workspace**
3. Choose provider: **Supabase (Self-hosted)**
4. Enter a workspace name
5. Click **Create**

### 2.2 Create a CloudSync Project

1. In the workspace, click **New CloudSync Project**
2. **Project name:** Give it a descriptive name (e.g., `my-app-sync`)
3. **Connection string:** Enter your PostgreSQL connection:
   ```
   postgresql://user:password@host:5432/database
   ```
5. Click **Create**

---

## Step 3: Set Up Authentication

### Quick Test with API Key (Recommended for Testing)

The fastest way to test CloudSync without per-user access control — no JWT setup needed.

1. Get your PostgreSQL database credentials (username and password)
2. In your client code (SQLite), authenticate with:
   ```sql
   SELECT cloudsync_network_init('<database-id>');
   SELECT cloudsync_network_set_apikey('<username>:<password>');
   SELECT cloudsync_network_sync();
   ```

The API key is simply your PostgreSQL credentials in `username:password` format. No JWT configuration needed.

### Using JWT Tokens (For RLS and Production)

If you need role-based access control (RLS) or production security:

1. **Choose your JWT authentication method:**
   - **Default (HS256 - Shared Secret):** Supabase auto-generates `JWT_SECRET` in `.env`
   - **GoTrue/JWKS (RS256 - Asymmetric):** Use Supabase's GoTrue service with JWKS endpoint
2. **Configure JWT in the CloudSync dashboard:**
   - Go to **Configuration** tab → **Edit connection settings**
   - **For HS256:** Enter your **JWT secret** (from `.env`)
   - **For RS256/JWKS:** Enter your **JWT allowed issuers** URL
   - Click **Save**
3. **Generate JWT tokens:**
   - Use [Supabase's built-in authentication](https://supabase.com/docs/guides/auth/jwts) to generate tokens
4. **In your client code:**
   ```sql
   SELECT cloudsync_network_init('<database-id>');
   SELECT cloudsync_network_set_token('<jwt-token>');
   SELECT cloudsync_network_sync();
   ```

---

## Step 4: Enable CloudSync on Tables

1. In the CloudSync dashboard, go to the **Tables** tab
2. **Select tables** you want to sync (checkbox each table)
4. Click **Deploy Changes**

CloudSync is now active on your selected tables.
