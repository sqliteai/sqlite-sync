# Simple Todo Database - SQLite Sync Example

This comprehensive example demonstrates how to set up a collaborative task management system using SQLite Sync. We'll create a shared todo list that synchronizes across multiple devices in real-time.

## Prerequisites

1. **SQLite Cloud Account**: Sign up at [SQLite Cloud](https://sqlitecloud.io/)
2. **SQLite Sync Extension**: Download from [Releases](https://github.com/sqliteai/sqlite-sync/releases)

## Step 1: SQLite Cloud Dashboard Setup

Before using the local CLI, you need to set up your cloud database:

### 1.1 Open your Project
1. Log into your [SQLite Cloud Dashboard](https://dashboard.sqlitecloud.io/)
2. Open your project or create a new one

### 1.2 Create a Database
1. In your project dashboard, click **"Create Database"**
2. Name your database (e.g., "todo_app.sqlite")
3. Click **"Create"**

### 1.3 Get Connection Details
1. Copy the **Connection String** (format: `sqlitecloud://projectid.sqlite.cloud/database.sqlite`)
2. Copy an **API Key**

### 1.4 Configure Row-Level Security (Optional)
1. In your database dashboard, go to **"Security"** → **"Row-Level Security"**
2. Enable RLS for tables you want to secure
3. Create policies to control user access (e.g., users can only see their own tasks)

**Note**: RLS rules only apply to token-authenticated users. RLS rules are not enforced for API key-authenticated accesses.

## Step 2: Local Database Setup (Device A)

Open your terminal and start SQLite with the extension:

```bash
# Navigate to your project directory
cd ~/my_todo_app

# Start SQLite CLI
sqlite3 todo_local.db

# Load the SQLite Sync extension
.load ./cloudsync

# Verify extension loaded successfully
SELECT cloudsync_version();
```

## Step 3: Create and Initialize Tables

Tables must be created on both the local database and SQLite Cloud with identical schemas.

### 3.1 Create Tables on Local Database

```sql
-- Create the main tasks table
-- Note: Primary key MUST be TEXT (not INTEGER) for global uniqueness
CREATE TABLE IF NOT EXISTS tasks (
    id TEXT PRIMARY KEY NOT NULL,
    userid TEXT NOT NULL DEFAULT '',
    title TEXT NOT NULL DEFAULT '',
    description TEXT DEFAULT '',
    completed INTEGER DEFAULT 0,
    priority TEXT DEFAULT 'medium',
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT DEFAULT CURRENT_TIMESTAMP
);

-- Initialize tables for synchronization
SELECT cloudsync_init('tasks');

-- Verify tables are initialized
SELECT cloudsync_is_enabled('tasks');
```

### 3.2 Create Tables on SQLite Cloud

1. **Create tables using Studio**: 
   - Go to your SQLite Cloud dashboard
   - Click on your database name (e.g., "todo_app.sqlite") to enter the Studio page
   - Execute the same CREATE TABLE statement:
   ```sql
   CREATE TABLE IF NOT EXISTS tasks (
       id TEXT PRIMARY KEY NOT NULL,
       userid TEXT NOT NULL DEFAULT '',
       title TEXT NOT NULL DEFAULT '',
       description TEXT DEFAULT '',
       completed INTEGER DEFAULT 0,
       priority TEXT DEFAULT 'medium',
       created_at TEXT DEFAULT CURRENT_TIMESTAMP,
       updated_at TEXT DEFAULT CURRENT_TIMESTAMP
   );
   ```

2. **Initialize tables for synchronization**:
   - Return to the Databases page
   - Click the **"OffSync"** button next to your `todo_app.sqlite` database
   - Select the `tasks` table to enable synchronization

## Step 4: Configure Network Synchronization

```sql
-- Configure connection to SQLite Cloud
-- Replace with your actual connection string from Step 1.3
SELECT cloudsync_network_init('sqlitecloud://your-project-id.sqlite.cloud/todo_app.sqlite');

-- Set your API key from Step 1.3
SELECT cloudsync_network_set_apikey('your-api-key-here');

-- Optional: Test connection
SELECT cloudsync_network_sync();
```

## Step 5: Add Sample Data

```sql
-- Add some tasks for Alice (using hardcoded UUIDv7)
INSERT INTO tasks (id, userid, title, description, priority, completed) VALUES 
    (cloudsync_uuid(), '01H7VXHP7ZG8W5QV2M9X3F4Y6A', 'Set up project database', 'Initialize SQLite Sync for the todo app', 'high', 1),
    (cloudsync_uuid(), '01H7VXHP7ZG8W5QV2M9X3F4Y6A', 'Design user interface', 'Create mockups for the task management UI', 'medium', 0),
    (cloudsync_uuid(), '01H7VXHP7ZG8W5QV2M9X3F4Y6A', 'Implement authentication', 'Add user login and registration', 'high', 0),
    (cloudsync_uuid(), '01H7VXHP7ZG8W5QV2M9X3F4Y6A', 'Add task filtering', 'Allow users to filter by priority and status', 'low', 0);

-- View current tasks
SELECT id, userid, title, priority, completed FROM tasks ORDER BY created_at;

-- Sync changes to cloud
SELECT cloudsync_network_sync();
```

## Step 6: Set Up Second Device (Device B)

On another device, laptop, or terminal session:

```bash
# Create a new database file for device B
sqlite3 todo_device_b.db

# Load the extension
.load ./cloudsync
```

```sql
-- Create identical table structure
CREATE TABLE IF NOT EXISTS tasks (
    id TEXT PRIMARY KEY NOT NULL,
    userid TEXT NOT NULL DEFAULT '',
    title TEXT NOT NULL DEFAULT '',
    description TEXT DEFAULT '',
    completed INTEGER DEFAULT 0,
    priority TEXT DEFAULT 'medium',
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT DEFAULT CURRENT_TIMESTAMP
);

-- Initialize for sync
SELECT cloudsync_init('tasks');

-- Connect to the same cloud database
SELECT cloudsync_network_init('sqlitecloud://your-project-id.sqlite.cloud/todo_app.sqlite');
SELECT cloudsync_network_set_apikey('your-api-key-here');

-- Pull data from Device A - repeat until data is received
SELECT cloudsync_network_sync();
-- Keep calling until the function returns > 0 (indicating data was received)
SELECT cloudsync_network_sync();

-- Verify data was synced
SELECT COUNT(*) as task_count FROM tasks;
SELECT userid, title, priority, completed FROM tasks ORDER BY created_at;
```

## Step 7: Test Collaborative Editing

### On Device B - Add and modify tasks:

```sql
-- Add new tasks for Bob (using hardcoded UUIDv7)
INSERT INTO tasks (id, userid, title, description, priority) VALUES 
    (cloudsync_uuid(), '01H7VXHP7ZG8W5QV2M9X3F4Y6B', 'Write documentation', 'Create user guide for the app', 'medium'),
    (cloudsync_uuid(), '01H7VXHP7ZG8W5QV2M9X3F4Y6B', 'Set up CI/CD pipeline', 'Automate testing and deployment', 'high');

-- Mark a task as completed
UPDATE tasks SET completed = 1, updated_at = CURRENT_TIMESTAMP 
WHERE title = 'Design user interface';

-- Sync changes
SELECT cloudsync_network_sync();
```

### On Device A - Pull updates and make changes:

```sql
-- Get updates from Device B - repeat until data is received
SELECT cloudsync_network_sync();
-- Keep calling until the function returns > 0 (indicating data was received)
SELECT cloudsync_network_sync();

-- View all tasks (should now include Device B's additions)
SELECT userid, title, priority, completed, updated_at FROM tasks ORDER BY created_at;

-- Make concurrent changes
UPDATE tasks SET priority = 'urgent' 
WHERE title = 'Implement authentication';

INSERT INTO tasks (id, userid, title, description, priority) VALUES 
    (cloudsync_uuid(), '01H7VXHP7ZG8W5QV2M9X3F4Y6A', 'Add unit tests', 'Ensure code quality with comprehensive tests', 'high');

-- Sync changes
SELECT cloudsync_network_sync();
```

## Step 8: Handle Offline Scenarios

```sql
-- Simulate offline work (disconnect from network)
-- Make changes while offline
INSERT INTO tasks (id, userid, title, description, priority) VALUES 
    (cloudsync_uuid(), '01H7VXHP7ZG8W5QV2M9X3F4Y6A', 'Offline task', 'This was created without network', 'low');

UPDATE tasks SET completed = 1 WHERE title = 'Add task filtering';

-- Check for unsent changes
SELECT cloudsync_network_has_unsent_changes();

-- When network returns, sync automatically resolves conflicts
-- Repeat until all changes are synchronized
SELECT cloudsync_network_sync();
-- Keep calling until the function returns > 0 (indicating data was received/sent)
SELECT cloudsync_network_sync();
```

## Step 9: Cleanup

When you're finished working with the database, properly clean up the resources:

```sql
-- Before closing the database connection, always call terminate
SELECT cloudsync_terminate();

-- Close the database connection
.quit
```

**Note**: Remember to call `cloudsync_terminate()` before closing any database connection that uses SQLite Sync to properly clean up resources.

## Key Benefits Demonstrated

1. **Conflict-Free Collaboration**: Multiple devices can edit the same data simultaneously
2. **Offline-First**: Changes are queued locally and sync when connectivity returns
3. **Real-time Sync**: Changes appear on all devices within seconds
4. **Automatic Conflict Resolution**: No manual intervention needed when conflicts occur
5. **Row-Level Security**: Users can see only their authorized data
6. **Cross-Platform**: Works identically on mobile, desktop, and web applications

## Next Steps

- Integrate SQLite Sync into your application using your preferred language/framework
- Set up automated sync intervals in your application
- Implement user authentication and row-level security policies
- Add real-time notifications for collaborative features
- Scale to multiple databases and user groups

For detailed API documentation, see [API.md](../../API.md).