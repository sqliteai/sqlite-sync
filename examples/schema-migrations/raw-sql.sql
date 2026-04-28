-- Raw SQL schema migration escape hatch.
--
-- Use structured cloudsync_alter_* commands whenever possible. Raw SQL is useful
-- for indexes, constraints, or migration steps that are not portable yet.
-- Generic SQL runs on every engine; dialect-specific SQL runs only on that
-- engine and is skipped by the other one.
-- Generated raw SQL migrations require destructive-schema authorization because
-- the extension cannot infer the impact of arbitrary SQL.

SELECT cloudsync_alter_sql('CREATE INDEX notes_updated_at_idx ON notes(updated_at)');
SELECT cloudsync_alter_sqlite('CREATE INDEX notes_body_sqlite_idx ON notes(body)');
SELECT cloudsync_alter_postgresql('CREATE INDEX notes_body_pg_idx ON notes(body)');

SELECT cloudsync_alter_apply();
SELECT cloudsync_network_migration_upload();
