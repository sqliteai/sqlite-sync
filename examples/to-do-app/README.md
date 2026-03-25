# Expo CloudSync Example

A simple Expo example demonstrating SQLite synchronization with CloudSync. Build cross-platform apps that sync data seamlessly across devices.

<img src="https://github.com/user-attachments/assets/86db5c25-d8ff-4c31-b157-8dff178e1720" width="40%" height="40%">

## 🚀 Quick Start

### 1. Clone the template

Create a new project using this template:
```bash
npx create-expo-app MyApp --template @sqliteai/todoapp
cd MyApp
```

### 2. Database Setup

1. Create database in [SQLite Cloud Dashboard](https://dashboard.sqlitecloud.io/).
2. Execute the exact schema from `to-do-app.sql`.  
3. Enable OffSync for all tables on the remote database from the **SQLite Cloud Dashboard -> Databases**.

### 3. Environment Configuration

Rename the `.env.example` into `.env` and fill with your values.

> **⚠️ SECURITY WARNING**: This example puts database API Keys directly in `.env` files for demonstration purposes only. **Do not use this pattern in production.** 
>
> **Why this is unsafe:**
> - API Keys allow access to sensitive credentials
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

