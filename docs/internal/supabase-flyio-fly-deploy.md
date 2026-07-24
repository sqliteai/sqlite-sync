# Self-Hosted Supabase on Fly.io — `fly deploy` Strategy

How to run a self-hosted Supabase stack with the CloudSync Postgres extension on
Fly.io. Everything lives in `deploy/supabase-flyio/` and is deployed with a single
`fly deploy`. No SSH, no `apt-get`, no editing files on the VM.

Nothing in `deploy/supabase-flyio/` names a specific deployment: the app name,
region and org are command-line arguments, and the public URLs are derived from
them at boot. The examples below use these shell variables:

```bash
FLY_APP=<your-app-name>
FLY_REGION=<your-region>
FLY_ORG=<your-org>
```

Hardware for a reference deployment is `shared-cpu-4x` / 4 GB / 50 GB volume, set
in `fly.toml`.

---

## Why not Fly's native Docker Compose support

Fly can turn a compose file into containers on one Machine
(`[build.compose]` in `fly.toml`,
[docs](https://fly.io/docs/machines/guides-examples/multi-container-machines/)).
It cannot run the Supabase compose file. The converter is
`internal/containerconfig/compose.go` in `superfly/flyctl`, and it:

| Limitation | Consequence for Supabase |
|---|---|
| Plain `yaml.Unmarshal`, no `${VAR}` interpolation and no `env_file` | Every service would receive the literal string `${POSTGRES_PASSWORD}` |
| `volumes:` entries are `os.ReadFile(hostPath)` → injected as base64 container files | File mounts (`kong.yml`, `*.sql`, `pooler.exs`) work; directory mounts (`volumes/db/data`, `volumes/storage`, `volumes/functions`) are dropped with a warning |
| Never emits `containers[].mounts` | No way to attach a Fly Volume to a container: PGDATA lands on the ephemeral overlay and is wiped on restart/redeploy |
| Named volumes ignored | `db-config:/etc/postgresql-custom` (pgsodium key) is dropped |
| One buildable service; secrets are machine-global; one container gets ingress | Manageable, but on top of the above |

So we keep real Docker Compose and put dockerd inside the Machine instead.

---

## How it works

One Fly Machine runs a Docker-in-Docker image. The image contains dockerd, the
compose plugin and a pinned copy of the upstream Supabase `docker/` directory.
A Fly Volume at `/data` holds the Docker data root, PGDATA, storage and `.env`,
so state survives restarts and redeploys.

```
deploy/supabase-flyio/
├── Dockerfile                     # docker:28-dind + fuse-overlayfs + pinned supabase/docker tree
├── entrypoint.sh                  # dockerd → compose tree → .env bootstrap → compose up → post-init
├── docker-compose.cloudsync.yml   # our overrides, layered via COMPOSE_FILE
├── cloudsync.sql                  # CREATE EXTENSION IF NOT EXISTS cloudsync
└── fly.toml                       # hardware, volume, env, HTTPS service
```

Notable decisions:

- **`fuse-overlayfs`**: Docker's `overlay2` fails inside a Fly Machine
  (`failed to convert whiteout file: operation not permitted`). `vfs` works but
  costs ~35 GB and 30–60 min for a full pull.
- **Docker data root on `/data/docker`**: images are pulled once, not on every boot.
- **Compose config re-copied from the image on every boot**, `.env` and
  `volumes/` are not — config is immutable and versioned, state is persistent.
- **Overrides layered via `COMPOSE_FILE`** (upstream's own mechanism) instead of
  `sed`-ing `docker-compose.yml`, so re-pinning upstream needs no merge work.
- **dockerd runs with IPv6 enabled** so published ports also bind `[::]`. Fly's
  private network (`<app>.internal`) is IPv6-only, and Docker publishes IPv4-only
  by default — without this, server-to-server access over `.internal` fails.
- **`docker compose logs -f` is the foreground process**, so `fly logs` shows all
  services. If it exits, the Machine restarts and the bootstrap re-runs.
- **A failed `compose up` does not kill the Machine.** It used to: with `set -e`,
  one service failing its dependency condition exited the entrypoint, Fly
  restarted, it failed again, and after 10 rounds the Machine was stopped — taking
  the healthy services with it and leaving nothing to inspect. Now the failure is
  logged and the boot continues to the log stream.
- **Containers of services dropped from the config are removed before `up`.**
  `--remove-orphans` does not cover a service that still exists but sits in a
  disabled profile, so its container keeps running *and keeps its published
  ports*. That is exactly what happens when swapping the gateway: the old
  `supabase-kong` container held port 8000 and Envoy could not bind.

### API gateway: Envoy

`COMPOSE_FILE` layers `docker-compose.envoy.yml`, which parks Kong in a disabled
profile and runs `envoyproxy/envoy` as `api-gw` / `supabase-envoy` on the same
host port 8000, keeping the network aliases `kong` and `envoy` so other services'
internal URLs still resolve. Upstream makes Envoy the default in the ~August 2026
release ([discussion 48048](https://github.com/orgs/supabase/discussions/48048));
after that the override becomes a no-op shim and Kong moves to
`docker-compose.kong.yml`.

The Envoy default has no `:8443` HTTPS listener, which does not matter here —
Fly terminates TLS at the edge and only plain 8000 is ever exposed.

Envoy is stricter than Kong about credentials. `/rest/v1/` (the PostgREST OpenAPI
root) is service-role-only and answers `RBAC: access denied` to an anon key; that
is intended hardening, not a broken route.

### Overrides applied to upstream compose

```yaml
services:
  db:
    image: ${CLOUDSYNC_IMAGE:-sqlitecloud/sqlite-sync-supabase:17-alpine}
    volumes:
      - ./cloudsync.sql:/docker-entrypoint-initdb.d/init-scripts/100-cloudsync.sql:Z
    ports:
      - "5433:5432"          # direct Postgres; 5432 belongs to Supavisor
  supavisor:
    entrypoint: ["/usr/bin/tini", "-s", "-g", "--"]   # skips /app/limits.sh
```

The `100-` prefix keeps the CloudSync init script after Supabase's own scripts
(97–99). Init scripts only run on an empty PGDATA, so `entrypoint.sh` also runs
`CREATE EXTENSION IF NOT EXISTS cloudsync;` on every boot.

---

## Deploying

```bash
fly apps create "$FLY_APP" --org "$FLY_ORG"
cd deploy/supabase-flyio
fly deploy . --config fly.toml --app "$FLY_APP" --primary-region "$FLY_REGION"
```

That is the whole setup. The volume is created from `[[mounts]] initial_size`,
IPs are allocated automatically, and the first boot generates all secrets.

`fly.toml` deliberately has no `app` or `primary_region` key, so the same file
works for any deployment and no environment-specific identifier is committed.

First boot pulls ~11 service images and takes roughly 5–10 minutes. Follow it with
`fly logs -a "$FLY_APP"`.

To run a pre-release CloudSync image instead of the tag baked into the compose
override:

```bash
fly secrets set -a "$FLY_APP" \
  CLOUDSYNC_IMAGE=sqlitecloud/sqlite-sync-supabase:17-alpine-beta-<branch>
```

### Secrets

`entrypoint.sh` bootstraps `.env` on first boot only, from `.env.example` plus
upstream's own generators:

```
sh ./utils/generate-keys.sh --update-env      # JWT_SECRET, ANON_KEY, SERVICE_ROLE_KEY, …
sh ./utils/add-new-auth-keys.sh --update-env  # ES256 JWT_KEYS/JWT_JWKS, publishable/secret keys
```

plus a random `POSTGRES_PASSWORD` and `DASHBOARD_PASSWORD`. Read them back with:

```bash
fly ssh console -a "$FLY_APP" -C \
  "grep -E '^(POSTGRES_PASSWORD|DASHBOARD_USERNAME|DASHBOARD_PASSWORD|ANON_KEY|SERVICE_ROLE_KEY|JWT_SECRET)=' /data/supabase-docker/.env"
```

To pin a value yourself, set it as a Fly secret — compose interpolation prefers
the process environment over `.env`, and Fly secrets are process environment:

```bash
fly secrets set -a "$FLY_APP" POSTGRES_PASSWORD=...
```

> Changing `POSTGRES_PASSWORD` *after* the database has been initialized leaves
> the existing roles on the old password, and services then fail with
> `FATAL: password authentication failed for user "authenticator"`. Realign them:
>
> ```bash
> cd /data/supabase-docker
> for role in authenticator supabase_auth_admin supabase_storage_admin supabase_admin; do
>     docker compose exec -T db psql -U postgres \
>         -c "ALTER USER $role WITH PASSWORD 'NEW_PASSWORD';"
> done
> docker compose restart
> ```
>
> Upstream also ships `sh ./utils/db-passwd.sh` for a guided rotation, followed by
> `docker compose up -d --force-recreate`.

---

## Accessing the stack

| What | How |
|---|---|
| Studio / API gateway | `https://$FLY_APP.fly.dev` (basic auth: `DASHBOARD_USERNAME` / `DASHBOARD_PASSWORD`) |
| REST / Auth / Storage / Realtime | `https://$FLY_APP.fly.dev/{rest,auth,storage,realtime}/v1/` |
| Postgres, direct, from another Fly app | `postgres://postgres:<pw>@$FLY_APP.internal:5433/postgres` |
| Postgres, direct, from a laptop | `fly proxy 5433 -a "$FLY_APP"` then connect to `localhost:5433` |
| Postgres via Supavisor pooler | machine port `5432` (session) / `6543` (transaction) |
| Shell | `fly ssh console -a "$FLY_APP"` then `cd /data/supabase-docker` |

Register with the CloudSync server using the direct (non-pooled) connection
string on port 5433, `"flavor": "supabase"`.

To keep everything private instead, drop `[http_service]` from `fly.toml`,
release the public IPs, and use `fly proxy 8000 -a <app>`.

---

## Maintenance

| Task | Command |
|---|---|
| Update the CloudSync image | released tag: redeploy (compose pulls `:17-alpine`); beta: `fly secrets set CLOUDSYNC_IMAGE=…` |
| Update Supabase services | bump `SUPABASE_REF` in the `Dockerfile`, `fly deploy` |
| Restart the stack | `fly machine restart -a <app>` — the entrypoint re-runs everything |
| Inspect containers | `fly ssh console -a <app> -C "docker -H unix:///var/run/docker.sock ps"` |
| Resize hardware | `fly scale vm shared-cpu-8x --vm-memory 8192 -a <app>` |
| Grow the volume | `fly volumes extend <vol-id> --size 80 -a <app>` |
| Destroy | `fly apps destroy <app>` (removes the volume too) |

Redeploys recreate the Machine but keep `/data`, so the Docker image cache and
the database survive; only the ~135 MB app image is re-pulled.

---

## Registering the database with the CloudSync server

The CloudSync server needs a Postgres connection string it can reach. If it runs
in the same Fly org, use the private network directly — no public exposure needed:

```bash
export CLOUDSYNC_URL="https://cloudsync-staging-testing.fly.dev"   # or https://cloudsync.sqlite.ai
export ORG_API_KEY="<organization-api-key>"
export CONNECTION_STRING="postgres://postgres:<POSTGRES_PASSWORD>@$FLY_APP.internal:5433/postgres"

curl "$CLOUDSYNC_URL/healthz"     # {"status":"ok"}
```

If the server runs outside Fly, tunnel instead: `fly proxy 5433 -a "$FLY_APP"` and
use `@localhost:5433`.

Register, then verify connectivity:

```bash
curl --request POST "$CLOUDSYNC_URL/v1/databases" \
  --header "Authorization: Bearer $ORG_API_KEY" \
  --header "Content-Type: application/json" \
  --data '{
    "label": "Supabase Fly.io Test",
    "connectionString": "'"$CONNECTION_STRING"'",
    "provider": "postgres",
    "flavor": "supabase",
    "projectId": "'"$FLY_APP"'",
    "databaseName": "postgres"
  }'

export MANAGED_DATABASE_ID="<returned-id>"

curl --request POST "$CLOUDSYNC_URL/v1/databases/$MANAGED_DATABASE_ID/verify" \
  --header "Authorization: Bearer $ORG_API_KEY"
```

Always register the **direct** port 5433, not the Supavisor pooler.

Create a table and enable sync on it:

```bash
fly ssh console -a "$FLY_APP"
cd /data/supabase-docker && docker compose exec db psql -U postgres
```

```sql
CREATE TABLE IF NOT EXISTS todos (
  id TEXT PRIMARY KEY DEFAULT cloudsync_uuid(),
  title TEXT NOT NULL DEFAULT '',
  done BOOLEAN DEFAULT false
);
SELECT cloudsync_init('todos');
```

```bash
curl --request POST "$CLOUDSYNC_URL/v1/databases/$MANAGED_DATABASE_ID/cloudsync/enable" \
  --header "Authorization: Bearer $ORG_API_KEY" \
  --header "Content-Type: application/json" \
  --data '{"tables":["todos"]}'

curl --request GET "$CLOUDSYNC_URL/v1/databases/$MANAGED_DATABASE_ID/cloudsync/tables" \
  --header "Authorization: Bearer $ORG_API_KEY"     # todos should show "enabled": true
```

### Client token

For a `supabase`-flavored database the client authenticates with a GoTrue JWT, not
an org key. Create the user once, then take an `access_token` per session:

```bash
ANON_KEY=$(fly ssh console -a "$FLY_APP" -C \
  "grep ^ANON_KEY= /data/supabase-docker/.env" | tail -1 | tr -d '\r' | cut -d= -f2-)

curl -X POST "https://$FLY_APP.fly.dev/auth/v1/signup" \
  -H "apikey: $ANON_KEY" -H "Content-Type: application/json" \
  -d '{"email":"sync-test@example.com","password":"..."}'

curl -X POST "https://$FLY_APP.fly.dev/auth/v1/token?grant_type=password" \
  -H "apikey: $ANON_KEY" -H "Content-Type: application/json" \
  -d '{"email":"sync-test@example.com","password":"..."}'      # → access_token
```

Tokens are HS256, signed with `JWT_SECRET` from `.env`, issuer `API_EXTERNAL_URL`,
audience `authenticated`. Validate one with
`GET /auth/v1/user` + `Authorization: Bearer <token>`. Note that
`/auth/v1/.well-known/jwks.json` returns `{"keys":[]}`: upstream ships
`GOTRUE_JWT_KEYS` commented out, so GoTrue stays HS256-only even though `.env`
carries the ES256 material (PostgREST does use it, via `PGRST_JWT_SECRET`).

### Roundtrip

```sql
-- SQLite client (Homebrew sqlite3; the system one cannot load extensions)
.load dist/cloudsync.dylib
CREATE TABLE todos (
  id TEXT PRIMARY KEY DEFAULT (cloudsync_uuid()),
  title TEXT NOT NULL DEFAULT '',
  done BOOLEAN DEFAULT false
);
SELECT cloudsync_init('todos');
SELECT cloudsync_network_init('<MANAGED_DATABASE_ID>');
SELECT cloudsync_network_set_token('<access_token>');

INSERT INTO todos (title) VALUES ('from SQLite');
SELECT cloudsync_network_sync(500, 5);
```

Then check the row landed in Postgres, insert one there, and pull it back with
`SELECT cloudsync_network_check_changes();` on the client. `cloudsync_network_init`
and `cloudsync_network_set_token` are per-session — they are not persisted.

---

## Building a custom CloudSync Postgres image

CI publishes a beta tag for every branch push (see the publish matrix in
`.github/workflows/main.yml`), which is usually all you need — point the machine at
it with `CLOUDSYNC_IMAGE`. To build one locally instead:

```bash
git submodule update --init --recursive   # else: fractional_indexing.h: No such file or directory

docker build --platform linux/amd64 \
  --build-arg SUPABASE_POSTGRES_TAG=17.6.1.151 \
  -f docker/postgresql/Dockerfile.supabase \
  -t <registry>/<image>:<tag> .

docker push <registry>/<image>:<tag>
fly secrets set -a "$FLY_APP" CLOUDSYNC_IMAGE=<registry>/<image>:<tag>
```

`--platform linux/amd64` matters: Fly Machines are x86_64, and an Apple Silicon
build produces an ARM image that will not start. The image must be pullable
without credentials, or `docker login` has to run on the Machine.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `Bind for 0.0.0.0:8000 failed: port is already allocated` | A container from a previous compose config still holds the port. The entrypoint now removes containers whose service left the config; otherwise `docker rm -f <name>`. |
| Machine stopped, `machine has reached its max restart count of 10` | Something failed during boot and crash-looped. Read `fly logs`, fix, then `fly machine start <id>`. |
| `password authentication failed for user "authenticator"` | Roles hold a different password than `.env` — see the `ALTER USER` block under [Secrets](#secrets). |
| `cloudsync_version()` does not exist | Init scripts only run on an empty PGDATA. `docker compose exec -T db psql -U postgres -c "CREATE EXTENSION IF NOT EXISTS cloudsync;"` (the entrypoint does this on every boot). |
| Auth restarts, `must be owner of function uid (SQLSTATE 42501)` | GoTrue migrates as `supabase_auth_admin` and must own `auth.uid()`/`role()`/`email()`. Automated in `entrypoint.sh`; rerun by deleting `/data/.cloudsync-post-init-done` and restarting. |
| Supavisor crashes on `ulimit: open files: cannot modify limit` | The Fly kernel refuses `/app/limits.sh`. Handled by the entrypoint override in `docker-compose.cloudsync.yml`. |
| `RBAC: access denied` on `/rest/v1/` | Envoy restricts the PostgREST OpenAPI root to the service-role key. Use a table path, or the service-role key. |
| `.internal` unreachable *from inside a container* | Containers have no route to Fly's 6PN. Use `db:5432` (compose network), `127.0.0.1:5432` (inside `supabase-db`), or the bridge gateway on `:5433`. |
| Services unhealthy right after boot | Give it ~2 minutes, then `docker compose logs <service>`. Studio and Envoy come last. |
| `no space left on device` during a pull | `fly volumes extend <vol-id> --size 80 -a "$FLY_APP"`. |
| `cannot stop container … did not receive an exit event` | Zombie container: `kill -9 $(pidof dockerd) $(pidof containerd)`, `rm -rf /data/docker/containers/<id>*`, then restart the Machine. |
| Cannot pull the CloudSync image | The tag must be public, or run `docker login` on the Machine. |

---

## Image / version notes

The Alpine CloudSync images are built from `supabase/postgres:17.6.1.151` (see the
publish matrix in `.github/workflows/main.yml`), while the pinned upstream compose
expects `17.6.1.136`. Same PG 17 line, newer patch base — fine, but re-check after
bumping either side.

The pinned compose runs 11 services: `db`, `supavisor`, `auth`, `rest`, `realtime`,
`storage`, `imgproxy`, `meta`, `functions`, `studio` and the gateway. Upstream has
dropped `analytics` (Logflare) and `vector`, so nothing here needs the old
"disable Logflare" workaround. `db` no longer publishes 5432 either — Supavisor
owns it, which is why the override maps direct Postgres to 5433.

Verified on the first deployment of this setup (`shared-cpu-4x` / 4 GB / 50 GB,
beta image `…:17-alpine-beta-<branch>`): all 11 services
healthy on first boot, `PostgreSQL 17.6`, `cloudsync` extension `1.1` /
`cloudsync_version()` = `1.1.2`, `cloudsync_init()` + insert produced a row in
`cloudsync_changes`, the gateway reachable over HTTPS (Envoy: `/auth/v1/health` and
`/storage/v1/version` 200 with the anon key, `/rest/v1/` 200 with the service-role
key), and ports 5432/5433/8000 published
on both `0.0.0.0` and `[::]` (so `<app>.internal` works). First boot took ~9
minutes, almost all of it image pulls.
