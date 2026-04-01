# Sport Tracker app with SQLite Sync 🚵

A Vite/React demonstration app showcasing [**SQLite Sync**](https://github.com/sqliteai/sqlite-sync) implementation for **offline-first** data synchronization across multiple devices. This example illustrates how to integrate SQLite AI's sync capabilities into modern web applications with proper authentication via [Access Token](https://docs.sqlitecloud.io/docs/access-tokens) and [Row-Level Security (RLS)](https://docs.sqlitecloud.io/docs/rls).

> This app uses the packed WASM version of SQLite with the [SQLite Sync extension enabled](https://www.npmjs.com/package/@sqliteai/sqlite-wasm).

**The source code is located in [examples/sport-tracker-app](../../examples/sport-tracker-app/)**

## Setup Instructions

### 1. Prerequisites
- Node.js 20.x or \>=22.12.0

### 2. Database Setup
1. Create database
2. Execute the schema with [sport-tracker-schema-postgres.sql](../../examples/sport-tracker-app/sport-tracker-schema-postgres.sql).  
3. Enable CloudSync for all tables on the remote database with:
    ```sql
    CREATE EXTENSION IF NOT EXISTS cloudsync;
    SELECT cloudsync_init('users_sport');
    SELECT cloudsync_init('workouts');
    SELECT cloudsync_init('activities');
    ```

### 3. Environment Configuration

Rename the `.env.example` into `.env` and fill with your values.

- `VITE_SQLITECLOUD_CONNECTION_STRING`: the url to the CloudSync server: https://cloudsync-staging.fly.dev/<database>
- `VITE_SQLITECLOUD_DATABASE`: remote database name.
- `VITE_SQLITECLOUD_API_KEY`: a valid user's JWT token. Refresh it when it expires. 
- `VITE_SQLITECLOUD_API_URL`: Supabase project API URL.

### 4. Installation & Run

```bash
npm install
npm run dev
```

### Demo

Continue reading on the official [README](https://github.com/sqliteai/sqlite-sync/blob/main/examples/sport-tracker-app/README.md#demo-use-case-multi-user-sync-scenario). 