# Self-Hosted Supabase on Fly.io — `fly deploy` Strategy

This is the recommended way to run a self-hosted Supabase stack with the CloudSync
Postgres extension on Fly.io. It replaces the manual VM setup in
[`supabase-flyio.md`](supabase-flyio.md), which is kept for reference and for the
troubleshooting table.

Everything lives in `deploy/supabase-flyio/` and is deployed with a single
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
> the existing roles on the old password. Fix with the `ALTER USER` block in
> [`supabase-flyio.md`](supabase-flyio.md#fix-services-fail-with-password-authentication-failed).

---

## Accessing the stack

| What | How |
|---|---|
| Studio / Kong | `https://$FLY_APP.fly.dev` (basic auth: `DASHBOARD_USERNAME` / `DASHBOARD_PASSWORD`) |
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

## Differences vs. the old manual guide

- Upstream compose **no longer ships `analytics` (Logflare) or `vector`** — the
  Logflare workaround in the old guide is obsolete.
- **`db` no longer publishes 5432**; Supavisor does. Hence the `5433:5432` override
  for direct access.
- The **Supavisor `ulimit` crash** and the **`auth.uid()` ownership fix** are still
  needed; both are automated in `entrypoint.sh`.
- Upstream now has **asymmetric auth keys** (`JWT_KEYS`, `JWT_JWKS`,
  `SUPABASE_PUBLISHABLE_KEY`, `SUPABASE_SECRET_KEY`); `add-new-auth-keys.sh`
  generates them on first boot.
- The old guide's HTTPS/Caddy section is unnecessary — `[http_service]` gives TLS
  on `<app>.fly.dev` for free.
- If you keep a hand-built VM anyway, add `persist_rootfs = "always"` (or
  `fly machine update --rootfs-persist always`) so Docker survives stop/start and
  you can drop the "reinstall Docker on every restart" ritual.

## Image / version notes

The Alpine CloudSync images are built from `supabase/postgres:17.6.1.151` (see the
publish matrix in `.github/workflows/main.yml`), while the pinned upstream compose
expects `17.6.1.136`. Same PG 17 line, newer patch base — fine, but re-check after
bumping either side.

Verified on the first deployment of this setup (`shared-cpu-4x` / 4 GB / 50 GB,
beta image `…:17-alpine-beta-<branch>`): all 11 services
healthy on first boot, `PostgreSQL 17.6`, `cloudsync` extension `1.1` /
`cloudsync_version()` = `1.1.2`, `cloudsync_init()` + insert produced a row in
`cloudsync_changes`, Kong reachable over HTTPS, and ports 5432/5433/8000 published
on both `0.0.0.0` and `[::]` (so `<app>.internal` works). First boot took ~9
minutes, almost all of it image pulls.
