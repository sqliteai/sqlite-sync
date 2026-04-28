-- Add a column with database-specific type/default SQL.
--
-- The portable operation defines the logical schema. The override calls refine
-- the generated SQL for SQLite and PostgreSQL while keeping one migration
-- payload usable across both databases.

SELECT cloudsync_alter_add_column('notes', 'metadata', 'json', false, '{}');
SELECT cloudsync_alter_add_column_sqlite('notes', 'metadata', 'TEXT', false, '''{}''');
SELECT cloudsync_alter_add_column_postgresql('notes', 'metadata', 'JSONB', false, '''{}''::jsonb');
SELECT cloudsync_alter_apply();
SELECT cloudsync_network_migration_upload();
