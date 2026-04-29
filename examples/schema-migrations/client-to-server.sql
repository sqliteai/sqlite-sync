-- Client-originated schema migration.
--
-- Run this on an authorized SQLite client. The extension generates the JSON
-- migration payload, applies it locally, stores it in cloudsync_pending_migration,
-- and uploads it automatically before row data on the next cloudsync_network_sync().

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

-- Optional initial data can be inserted here. cloudsync_network_sync() uploads
-- the pending schema migration first and sends row data only after the backend
-- accepts that schema.
INSERT INTO notes (id, title, body, updated_at)
VALUES (cloudsync_uuid(), 'First note', 'Created before first sync', '1970-01-01T00:00:00Z');

SELECT cloudsync_network_sync();
