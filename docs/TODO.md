# TODO

## CI / Releases

- [ ] **Compile PostgreSQL extension against both PG15 and PG17**
  - The `postgres-build` CI job currently hardcodes PG17 (`postgresql-server-dev-17`)
  - Add a matrix over `['15', '17']` matching the existing `postgres-test` job
  - Name artifacts with the PG version: `cloudsync-postgresql-linux-x86_64-pg17-1.0.0.tar.gz`
  - Files compiled against one major PG version will not load on another
  - **This is a prerequisite for the standard Dockerfile improvement below**

## With Docker

Decision: do not publish to a registry. Instead provide Dockerfiles users build themselves.

### Standard PostgreSQL (`Dockerfile`)

- [ ] **Update `Dockerfile` to download pre-compiled extension from GitHub releases instead of building from source**
  - Currently copies the full source tree and compiles inside the container
  - New approach: accept `--build-arg CLOUDSYNC_VERSION=1.0.0`, fetch the matching tarball from GitHub releases at build time
  - Dockerfile detects PG major version and architecture automatically to pick the right tarball
  - User only needs the Dockerfile — no full repo clone required
  - Example: `docker build --build-arg POSTGRES_TAG=17 --build-arg CLOUDSYNC_VERSION=1.0.0 -t my-cloudsync-postgres .`
  - **Blocked by:** release artifacts having PG version in the filename (see CI/Releases above)
  - **User flow (fresh):** Build image → use in `docker-compose.yml` → `CREATE EXTENSION cloudsync`
  - **User flow (existing):** Build image → swap in `docker-compose.yml` → restart → `CREATE EXTENSION cloudsync`

### Supabase (`Dockerfile.supabase`)

- Supabase uses a non-standard Nix-based PostgreSQL build — pre-compiled `.so` from releases will not work, must always compile from source
- [ ] **Update `Dockerfile.supabase` to download source tarball from GitHub releases instead of using `COPY src/`**
  - Currently uses `COPY src/`, `COPY modules/`, `COPY Makefile` — requires the user to run `docker build` from inside the cloned repo
  - New approach: accept `--build-arg CLOUDSYNC_VERSION=1.0.0`, fetch the source tarball from GitHub releases at build time and compile inside the container
  - GitHub automatically generates source tarballs for every release (`Source code (tar.gz)`)
  - User only needs the `Dockerfile.supabase` — no repo clone required
  - Example: `docker build -f Dockerfile.supabase --build-arg SUPABASE_POSTGRES_TAG=15.8.1.085 --build-arg CLOUDSYNC_VERSION=1.0.0 -t my-cloudsync-supabase-postgres .`
  - **User flow (fresh):** Build image → replace default Supabase postgres image in `docker-compose.yml` → `CREATE EXTENSION cloudsync`
  - **User flow (existing):** Build image → follow Supabase's update guide to swap the postgres image → `CREATE EXTENSION cloudsync`

## Without Docker

For users with an existing native PostgreSQL installation (bare metal or VM, not containerized).

- [ ] **Ensure release tarballs are usable for native installs**
  - User downloads the right tarball from GitHub releases (correct OS + arch + PG version)
  - Extracts 3 files: `cloudsync.so`, `cloudsync--1.0.sql`, `cloudsync.control`
  - Copies them to the PostgreSQL extension directories:
    ```bash
    cp cloudsync.so $(pg_config --pkglibdir)/
    cp cloudsync--1.0.sql cloudsync.control $(pg_config --sharedir)/extension/
    ```
  - Runs `CREATE EXTENSION cloudsync;` — done, no Docker or compiler needed
  - **Blocked by:** release artifacts having PG version in the filename (see CI/Releases above)
