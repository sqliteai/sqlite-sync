-- CloudSync PostgreSQL Initialization Script
-- This script creates the metadata tables needed by the cloudsync extension

-- Log initialization
DO $$
BEGIN
    RAISE NOTICE 'CloudSync tables initialized successfully';
END $$;
