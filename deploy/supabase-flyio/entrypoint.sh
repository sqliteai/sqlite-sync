#!/bin/sh
# Boots the whole stack. Idempotent: runs on every machine start, including
# after `fly deploy`, `fly machine restart` and crashes.
set -eu

DATA_DIR=/data/supabase-docker
DOCKERD_LOG=/data/dockerd.log
INIT_MARKER=/data/.cloudsync-post-init-done

echo "=== configuring dockerd ==="
mkdir -p /etc/docker /data/docker
cat > /etc/docker/daemon.json <<'JSON'
{
  "storage-driver": "fuse-overlayfs",
  "data-root": "/data/docker",
  "ipv6": true,
  "ip6tables": true,
  "fixed-cidr-v6": "fd00:d0c::/64"
}
JSON

echo "=== starting dockerd ==="
dockerd >>"$DOCKERD_LOG" 2>&1 &
until docker info >/dev/null 2>&1; do sleep 1; done

echo "=== syncing compose tree to $DATA_DIR ==="
mkdir -p "$DATA_DIR"
# Config comes from the image on every boot; state (.env, volumes/) stays.
cp -R /supabase/. "$DATA_DIR/"
cd "$DATA_DIR"

set_env() {
    grep -q "^$1=" .env && sed -i "s|^$1=.*|$1=$2|" .env || echo "$1=$2" >> .env
}

if [ ! -f .env ]; then
    echo "=== first boot: generating secrets ==="
    cp .env.example .env
    sh ./utils/generate-keys.sh --update-env
    sh ./utils/add-new-auth-keys.sh --update-env
    set_env POSTGRES_PASSWORD "$(openssl rand -hex 24)"
    set_env DASHBOARD_PASSWORD "$(openssl rand -hex 12)"
fi

# Public hostnames follow from the app name, which Fly injects, so no
# deployment-specific URL has to live in the repo. Fly secrets still win.
if [ -n "${FLY_APP_NAME:-}" ]; then
    : "${SUPABASE_PUBLIC_URL:=https://$FLY_APP_NAME.fly.dev}"
    : "${API_EXTERNAL_URL:=$SUPABASE_PUBLIC_URL/auth/v1}"
    export SUPABASE_PUBLIC_URL API_EXTERNAL_URL
fi

# Services dropped from the active config — a gateway swapped out via an override
# profile, say — keep their containers and their published ports, which blocks the
# replacement from binding. `--remove-orphans` does not cover them.
active_services=$(docker compose config --services 2>/dev/null || true)
docker compose ps -a --format '{{.Service}}|{{.Name}}' 2>/dev/null | while IFS='|' read -r svc name; do
    [ -n "$svc" ] || continue
    echo "$active_services" | grep -qx "$svc" && continue
    echo "=== removing disabled service: $svc ($name) ==="
    docker rm -f "$name" >/dev/null 2>&1 || true
done

echo "=== starting supabase ==="
# A service that fails its dependency conditions must not kill the machine:
# exiting here would crash-loop until Fly gives up, taking the healthy services
# with it and leaving nothing to debug.
docker compose up -d --remove-orphans || echo "WARNING: compose up reported a failure, continuing"

echo "=== waiting for postgres ==="
pg_ready=0
for _ in $(seq 1 90); do
    if docker compose exec -T db pg_isready -U postgres -h localhost >/dev/null 2>&1; then
        pg_ready=1
        break
    fi
    sleep 2
done

if [ "$pg_ready" = 1 ]; then
    # Init scripts only run on an empty PGDATA, so create the extension explicitly.
    docker compose exec -T db psql -U postgres -c "CREATE EXTENSION IF NOT EXISTS cloudsync;" || true
else
    echo "WARNING: postgres never became ready, skipping post-init"
fi

if [ "$pg_ready" = 1 ] && [ ! -f "$INIT_MARKER" ]; then
    # GoTrue runs its migrations as supabase_auth_admin and must own these.
    echo "=== fixing auth function ownership ==="
    for fn in "auth.uid()" "auth.role()" "auth.email()"; do
        docker compose exec -T db psql -U postgres \
            -c "ALTER FUNCTION $fn OWNER TO supabase_auth_admin;" || true
    done
    docker compose restart auth
    touch "$INIT_MARKER"
fi

echo "=== supabase is up ==="
docker compose ps

# Stream service logs to `fly logs`. If this exits the machine restarts and the
# whole bootstrap re-runs.
exec docker compose logs --no-color -f
