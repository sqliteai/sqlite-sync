# Deploying CloudSync to Self-Hosted Supabase on Fly.io

## Overview

Build a custom Supabase Postgres image with CloudSync baked in, push it to a container registry, and configure your Fly.io Supabase deployment to use it.

## Step-by-step

### 1. Build the custom Supabase Postgres image

The project includes `docker/postgresql/Dockerfile.supabase` which builds CloudSync into the Supabase Postgres base image. Match the tag to the PG version your Fly.io Supabase uses:

```bash
# Build with the default Supabase Postgres tag (17.6.1.071)
make postgres-supabase-build

# Or specify the exact tag your Fly deployment uses:
make postgres-supabase-build SUPABASE_POSTGRES_TAG=17.6.1.071
```

This produces a Docker image tagged as `public.ecr.aws/supabase/postgres:<tag>` locally.

### 2. Tag and push to a container registry

You need a registry accessible from Fly.io (Docker Hub, GitHub Container Registry, or Fly's own registry):

```bash
# Tag for your registry
docker tag public.ecr.aws/supabase/postgres:17.6.1.071 \
  registry.fly.io/<your-app>/postgres-cloudsync:17.6.1.071

# Push
docker push registry.fly.io/<your-app>/postgres-cloudsync:17.6.1.071
```

Or use Docker Hub / GHCR:

```bash
docker tag public.ecr.aws/supabase/postgres:17.6.1.071 \
  ghcr.io/<your-org>/supabase-postgres-cloudsync:17.6.1.071
docker push ghcr.io/<your-org>/supabase-postgres-cloudsync:17.6.1.071
```

### 3. Update your Fly.io Supabase deployment

In your Fly.io Supabase config (`fly.toml` or however you deployed the DB service), point the Postgres image to your custom image:

```toml
[build]
  image = "ghcr.io/<your-org>/supabase-postgres-cloudsync:17.6.1.071"
```

Then redeploy:

```bash
fly deploy --app <your-supabase-db-app>
```

### 4. Enable the extension

Connect to your Fly Postgres instance and enable CloudSync:

```bash
fly postgres connect --app <your-supabase-db-app>
```

```sql
CREATE EXTENSION cloudsync;
SELECT cloudsync_version();

-- Initialize sync on a table
SELECT cloudsync_init('my_table');
```

### 5. If using supabase-docker (docker-compose)

If your Fly.io Supabase is based on the [supabase/supabase](https://github.com/supabase/supabase) docker-compose setup, update the `db` service image in `docker-compose.yml`:

```yaml
services:
  db:
    image: ghcr.io/<your-org>/supabase-postgres-cloudsync:17.6.1.071
```

## Important notes

- **Match the Postgres major version** — the Dockerfile defaults to PG 17 (`SUPABASE_POSTGRES_TAG=17.6.1.071`). Check what your Fly deployment runs with `SHOW server_version;`.
- **ARM vs x86** — if your Fly machines are ARM (`fly.toml` with `vm.size` using arm), build the image for `linux/arm64`: `docker buildx build --platform linux/arm64 ...`
- **RLS considerations** — when using Supabase Auth with Row-Level Security, use a JWT `token` (not `apikey`) when calling sync functions.
