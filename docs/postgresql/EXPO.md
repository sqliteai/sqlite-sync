# Expo CloudSync Example

A simple Expo example demonstrating SQLite synchronization with CloudSync and PostgreSQL. Build cross-platform apps that sync data seamlessly across devices.

https://github.com/user-attachments/assets/21a0332a-7f8f-468b-bd5c-004049e70763

## 🚀 Quick Start

### 1. Clone the template

Create a new project using this template:
```bash
npx create-expo-app MyApp --template @sqliteai/todoapp@dev
cd MyApp
```

### 2. Setup

1. Execute the exact schema from [`to-do-app.sql`](../../examples/to-do-app/to-do-app.sql).
2. Enable CloudSync for all tables on the remote database with:
    ```sql
    CREATE EXTENSION IF NOT EXISTS cloudsync;
    SELECT cloudsync_init('tasks');
    SELECT cloudsync_init('tags');
    SELECT cloudsync_init('tasks_tags');
    ```
3. Rename the `.env.example` into `.env` and fill with your values.
4. If you're testing with a local server define also the `ANDROID_CONNECTION_STRING` variable and use a different connection string for it, replace localhost with `10.0.2.2`.

```
CONNECTION_STRING="http://localhost:8091/postgres"
ANDROID_CONNECTION_STRING="http://10.0.2.2:8091/postgres"
API_TOKEN="token"
```

5. Fill the `API_TOKEN` variable with the access token.

> **⚠️ SECURITY WARNING**: This example puts database connection strings directly in `.env` files for demonstration purposes only. **Do not use this pattern in production.** 
>
> **Why this is unsafe:**
> - Connection strings contain sensitive credentials
> - Client-side apps expose all environment variables to users
> - Anyone can inspect your app and extract database credentials
>
> **For production apps:**
> - Use the secure [sport-tracker-app](https://github.com/sqliteai/sqlite-sync/tree/main/examples/sport-tracker-app) pattern with authentication tokens and row-level security
> - Never embed database credentials in client applications

### 4. Build and run the App

```bash
npx expo prebuild # run once
npm run ios # or android
```

## ✨ Features

- **Add Tasks** - Create new tasks with titles and optional tags.
- **Edit Task Status** - Update task status when completed.
- **Delete Tasks** - Remove tasks from your list.
- **Dropdown Menu** - Select categories for tasks from a predefined list.
- **Cross-Platform** - Works on iOS and Android
- **Offline Support** - Works offline, syncs when connection returns

