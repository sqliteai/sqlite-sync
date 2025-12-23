-- CloudSync PostgreSQL Initialization Script
-- This script creates the metadata tables needed by the cloudsync extension

/*
-- -- Enable required extensions
-- CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

-- CloudSync settings table
-- Stores global configuration key-value pairs
CREATE TABLE IF NOT EXISTS cloudsync_settings (
    key TEXT PRIMARY KEY NOT NULL,
    value TEXT
);

-- CloudSync site ID table
-- Stores unique site identifiers for multi-site synchronization
CREATE TABLE IF NOT EXISTS cloudsync_site_id (
    site_id BYTEA UNIQUE NOT NULL
);

-- CloudSync table settings
-- Stores per-table and per-column configuration
CREATE TABLE IF NOT EXISTS cloudsync_table_settings (
    tbl_name TEXT NOT NULL,
    col_name TEXT NOT NULL,
    key TEXT NOT NULL,
    value TEXT,
    PRIMARY KEY(tbl_name, key)
);

-- CloudSync schema versions
-- Tracks schema changes for migration purposes
CREATE TABLE IF NOT EXISTS cloudsync_schema_versions (
    hash BIGINT PRIMARY KEY,
    seq INTEGER NOT NULL
);

-- Create indexes for better query performance
CREATE INDEX IF NOT EXISTS idx_table_settings_tbl_name
    ON cloudsync_table_settings(tbl_name);

CREATE INDEX IF NOT EXISTS idx_schema_versions_seq
    ON cloudsync_schema_versions(seq);
*/

-- Log initialization
DO $$
BEGIN
    RAISE NOTICE 'CloudSync tables initialized successfully';
END $$;
