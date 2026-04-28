-- Client-originated schema migration.
--
-- Run this on an authorized SQLite client. The extension generates the JSON
-- migration payload, applies it locally, stores it in cloudsync_pending_migration,
-- and uploads it with cloudsync_network_migration_upload().

SELECT cloudsync_alter_create_table('notes');
SELECT cloudsync_alter_add_column('notes', 'id', 'text', false);
SELECT cloudsync_alter_add_primary_key('notes', 'id');
SELECT cloudsync_alter_add_column('notes', 'title', 'text', false, '');
SELECT cloudsync_alter_add_column('notes', 'body', 'text', false, '');
SELECT cloudsync_alter_add_column('notes', 'updated_at', 'timestamp', false, '1970-01-01T00:00:00Z');
SELECT cloudsync_alter_augment_table('notes', 'CLS', 1);
SELECT cloudsync_alter_set_block_lww('notes', 'body', char(10));

-- Optional: inspect the generated payload before applying it.
SELECT cloudsync_alter_preview();

SELECT cloudsync_alter_apply();
SELECT cloudsync_network_migration_upload();
SELECT cloudsync_network_sync();
