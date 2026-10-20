#!/usr/bin/env bash
# Disposable PostgreSQL build and tests; no host ports or persistent volumes.
set -euo pipefail
cd "$(dirname "$0")/.."
postgres_tag=${POSTGRES_TAG:-17}
test_file=${1:-full_test.sql}
if [[ ! -f "test/postgresql/$test_file" || "$test_file" == */* ]]; then
    echo "Expected a SQL file name in test/postgresql" >&2
    exit 2
fi
image="sqlite-sync-test:${postgres_tag}"
container="sqlite-sync-test-$$"
cleanup() { docker rm -f -v "$container" >/dev/null 2>&1 || true; }
trap cleanup EXIT

docker build --build-arg "POSTGRES_TAG=$postgres_tag" -t "$image" -f docker/postgresql/Dockerfile .
docker run -d --name "$container" -e POSTGRES_PASSWORD=postgres \
    -v "$PWD/test:/tests:ro" "$image" >/dev/null
for ((i=0; i<60; i++)); do
    if docker exec "$container" pg_isready -U postgres -d postgres >/dev/null 2>&1; then
        # The image entrypoint briefly runs a temporary server during initialization.
        if docker exec "$container" psql -h 127.0.0.1 -U postgres -d postgres -c 'SELECT 1' >/dev/null 2>&1; then
            docker exec "$container" psql -U postgres -d postgres -v ON_ERROR_STOP=1 \
                -f "/tests/postgresql/$test_file"
            exit 0
        fi
    fi
    sleep 1
done
docker logs "$container" >&2
exit 1
