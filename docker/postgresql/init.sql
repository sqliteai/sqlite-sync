-- CloudSync PostgreSQL Initialization Script
-- This script loads the CloudSync extension during database init

CREATE EXTENSION IF NOT EXISTS cloudsync;

-- Log initialization
DO $$
BEGIN
    RAISE NOTICE 'CloudSync tables initialized successfully';
END $$;
