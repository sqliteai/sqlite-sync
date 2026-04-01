# Supabase Installation & Testing (CloudSync PostgreSQL Extension)

This guide explains how to install and test the CloudSync PostgreSQL extension
inside Supabase, both for the Supabase CLI local stack and for self-hosted
Supabase deployments.

## Prerequisites

- Docker running
- Supabase stack running (CLI local or self-hosted)
- The Supabase Postgres image tag in use (e.g. `public.ecr.aws/supabase/postgres:17.6.1.071`)

## Option A: Supabase CLI Local Stack

1) Start the stack once so the Postgres image is present:
```bash
supabase init
supabase start
```

2) Build a new Postgres image with CloudSync installed (same tag as Supabase uses):
```bash
# From this repo root:
make postgres-supabase-build

# If auto-detect fails, set the tag explicitly:
SUPABASE_CLI_IMAGE=public.ecr.aws/supabase/postgres:<tag> make postgres-supabase-build
```
You can also set the Supabase base image tag explicitly (defaults to
`17.6.1.071`). This only affects the base image used in the Dockerfile:
```bash
SUPABASE_POSTGRES_TAG=17.6.1.071 make postgres-supabase-build
```

3) Restart Supabase:
```bash
supabase stop
supabase start
```

4) Enable the extension:
```bash
psql postgresql://supabase_admin:postgres@127.0.0.1:54322/postgres
```

```sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
SELECT cloudsync_version();
```



## Option B: Self-Hosted Supabase (Docker Compose / Kubernetes)

1) Build a custom image based on the Supabase Postgres tag in use:
```bash
# From this repo root:
docker build -f docker/postgresql/Dockerfile.supabase \
  -t myorg/supabase-postgres-cloudsync:<tag> .
```

2) Update your deployment to use `myorg/supabase-postgres-cloudsync:<tag>`
for the database image.

3) Restart the stack.

4) Enable the extension:
```sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
SELECT cloudsync_version();
```



## Quick Smoke Test

```sql
CREATE TABLE notes (
  id TEXT PRIMARY KEY,
  body TEXT DEFAULT ''
);

SELECT cloudsync_init('notes');
INSERT INTO notes VALUES (cloudsync_uuid(), 'hello');
SELECT * FROM cloudsync_changes;
```

You should see one pending change row returned.
