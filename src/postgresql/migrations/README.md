# CloudSync PostgreSQL Migration Scripts

This directory holds PostgreSQL extension upgrade scripts of the form:

    cloudsync--<from>--<to>.sql

PostgreSQL uses these to execute `ALTER EXTENSION cloudsync UPDATE` by chaining
one or more files to reach the target version.

## When to add a script

For every release that bumps `CLOUDSYNC_VERSION` in `src/cloudsync.h` and
introduces SQL-level changes (new functions, changed signatures, dropped
objects, new views/triggers/casts, etc.), add:

    cloudsync--<previous_version>--<new_version>.sql

If a release has no SQL-level changes, you still need the file — an empty-ish
upgrade script (a single comment is fine) so `pg_extension_update_paths` reports
a valid path from the previous release.

## Rules

- Use `CREATE OR REPLACE` for every function — the underlying C symbol may have
  changed even when the SQL signature didn't.
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
