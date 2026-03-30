# CloudSync Quick Start: Self-Hosted PostgreSQL

This guide helps you enable CloudSync on a **self-hosted PostgreSQL database**. CloudSync adds offline-first synchronization capabilities to your PostgreSQL database.

## Step 1: Deploy PostgreSQL with CloudSync

Use the pre-built PostgreSQL Docker image that includes the CloudSync extension. This image is available for:
- PostgreSQL 15
- PostgreSQL 17

Example using Docker Compose:

```yaml
services:
  db:
    image: <cloudsync-postgres-image>
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

---

## Step 2: Register Your Database in the CloudSync Dashboard

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

1. **Prepare your JWT authentication:**
   - **For HS256 (shared secret):** Use your existing secret, or generate one: `openssl rand -base64 32`
   - **For RS256 (asymmetric/JWKS):** Have your auth system's issuer URL and JWKS endpoint ready
2. **Update CloudSync dashboard configuration:**
   - Go to your project's **Configuration** tab
   - Click **Edit connection settings**
   - **For HS256 (shared secret):** Enter your JWT secret
   - **For RS256 (asymmetric):** Enter your JWT allowed issuers URL
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

## Step 4: Enable CloudSync on Tables

1. In the CloudSync dashboard, go to the **Sync Tables** tab
2. **Select tables** you want to sync (checkbox each table)
3. Click **Deploy Changes**

CloudSync is now active on your selected tables.