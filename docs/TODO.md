# TODO

## CI / Releases

- [x] **Compile PostgreSQL extension against both PG15 and PG17**
  - The `postgres-build` CI job now builds for both PG 15 and PG 17 via matrix
  - Artifacts are named with the PG version: `cloudsync-postgresql17-linux-x86_64`, `cloudsync-postgresql15-linux-arm64`, etc.

## With Docker

### Standard PostgreSQL (`Dockerfile.release`)

- [x] **`Dockerfile.release` downloads pre-compiled extension from GitHub releases**
  - Accepts `--build-arg POSTGRES_TAG=17` and `--build-arg CLOUDSYNC_VERSION=1.0.0`
  - Detects PG major version (`PG_MAJOR`) and architecture (`TARGETARCH`) automatically to pick the right tarball
  - User only needs the Dockerfile — no full repo clone required
  - Example: `docker build --build-arg POSTGRES_TAG=17 --build-arg CLOUDSYNC_VERSION=1.0.0 -f docker/postgresql/Dockerfile.release -t my-cloudsync-postgres .`

- [x] **Pre-built Docker images published to Docker Hub on release**
  - `sqlitecloud/sqlite-sync-postgres:17`, `:15`, `:17-<version>`, `:15-<version>`
  - Multi-arch: `linux/amd64` + `linux/arm64`
  - Users can just `docker pull` — no build needed

### Supabase

Two Dockerfiles:
- `Dockerfile.supabase` — builds from source (for development and testing, requires repo clone)
- `Dockerfile.supabase.release` — downloads pre-built extension from GitHub releases (for end-users, no repo clone needed)

- [x] **Pre-built Supabase Docker images published to Docker Hub on release**
  - `sqlitecloud/sqlite-sync-supabase:17`, `:15`, `:17-<version>`, `:15-<version>`
  - Also tagged with the exact Supabase base image version (`:17.6.1.071`, `:15.8.1.085`)
  - Multi-arch: `linux/amd64` + `linux/arm64`
  - Users can reference the image directly in their Supabase `docker-compose.yml`

- [x] **`Dockerfile.supabase.release` downloads pre-compiled extension from GitHub releases**
  - Pre-built binaries are ABI-compatible with Supabase's Nix-based PostgreSQL (same PG major version)
  - Handles Supabase's non-standard Nix paths (`NIX_PGLIBDIR`, dual share directories)
  - Accepts `--build-arg SUPABASE_POSTGRES_TAG=15.8.1.085` and `--build-arg CLOUDSYNC_VERSION=1.0.0`
  - Example: `docker build -f docker/postgresql/Dockerfile.supabase.release --build-arg SUPABASE_POSTGRES_TAG=15.8.1.085 --build-arg CLOUDSYNC_VERSION=1.0.0 -t my-cloudsync-supabase .`

## Without Docker

For users with an existing native PostgreSQL installation (bare metal or VM, not containerized).

- [x] **Release tarballs are usable for native installs**
  - User downloads the right tarball from GitHub releases (correct OS + arch + PG version)
  - Extracts 3 files: `cloudsync.so`, `cloudsync--1.0.sql`, `cloudsync.control`
  - Copies them to the PostgreSQL extension directories:
    ```bash
    cp cloudsync.so $(pg_config --pkglibdir)/
    cp cloudsync--1.0.sql cloudsync.control $(pg_config --sharedir)/extension/
    ```
  - Runs `CREATE EXTENSION cloudsync;` — done, no Docker or compiler needed
