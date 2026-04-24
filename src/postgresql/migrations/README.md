# CloudSync PostgreSQL Migration Scripts

This directory holds PostgreSQL extension upgrade scripts of the form:

    cloudsync--<from>--<to>.sql

PostgreSQL uses these to execute `ALTER EXTENSION cloudsync UPDATE` by chaining
one or more files to reach the target version.

## Versioning model

The PostgreSQL extension version (`default_version` in `cloudsync.control`) is
**`MAJOR.MINOR`** only — derived from the first two components of
`CLOUDSYNC_VERSION` in `src/cloudsync.h`. The full semver of the compiled
binary is reported by the `cloudsync_version()` SQL function.

| Release kind                     | Example         | EXTVERSION moves? | Upgrade script? | User action        |
| -------------------------------- | --------------- | ----------------- | --------------- | ------------------ |
| PATCH bump (binary only)         | 1.0.16 → 1.0.17 | No (stays `1.0`)  | Not required    | Swap the `.so`.    |
| MINOR bump (SQL surface changes) | 1.0.x → 1.1.0   | Yes (`1.0` → `1.1`) | Required      | `ALTER EXTENSION cloudsync UPDATE;` |
| MAJOR bump                       | 1.x → 2.0.0     | Yes (`1.x` → `2.0`) | Required      | `ALTER EXTENSION cloudsync UPDATE;` |

CI enforces this contract via `scripts/check-postgres-migration.sh`:

- PATCH releases: the script diffs the current `cloudsync.sql.in` against the
  previous release's install script. If they differ, the build fails —
  accidental SQL surface drift in a PATCH release would silently break users
  whose `pg_extension.extversion` would otherwise stay at the old EXTVERSION.
- MINOR/MAJOR releases: the script requires a matching
  `cloudsync--<prev>--<new>.sql` in this directory.

## When to add an upgrade script

Add one **only when `EXTVERSION` changes** — i.e. when you bump MINOR or MAJOR
in `src/cloudsync.h`. The filename is literally
`cloudsync--<previous_extversion>--<new_extversion>.sql`, not the full semver.

Examples:

    1.0.17 -> 1.1.0   →  cloudsync--1.0--1.1.sql
    1.1.5  -> 2.0.0   →  cloudsync--1.1--2.0.sql

PATCH-level releases (1.0.16 → 1.0.17 → 1.0.18 → …) require **no file** — the
catalog's `installed_version` stays at the MAJOR.MINOR, and the `.so` swap is
a transparent binary upgrade.

## Rules for upgrade script content

- Use `CREATE OR REPLACE` for every function — the underlying C symbol may
  have changed even when the SQL signature didn't.
- Drop removed objects explicitly (`DROP FUNCTION IF EXISTS ...`).
- Never run `CREATE EXTENSION`-style bootstrap inside an upgrade script.
- Objects created inside an upgrade script are automatically attached to the
  extension via `pg_depend`.
- Scripts are packaged and installed by `make postgres-install` /
  `make postgres-package` via a wildcard; no need to list them anywhere.

## Verifying the chain

After rebuilding, inside the PG container:

    SELECT * FROM pg_extension_update_paths('cloudsync');

All `source -> target` rows should show a non-NULL `path`.

## I intended a PATCH but the CI check says "SQL surface drift"

Either:

1. You meant to change SQL. Bump MINOR in `src/cloudsync.h` and add
   `cloudsync--<prev>--<new>.sql`.
2. You didn't. Revert the change in `src/postgresql/cloudsync.sql.in`.

The CI error message prints a unified diff to help you decide which.
