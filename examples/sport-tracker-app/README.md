# Sport Tracker app with SQLite Sync 🚵

A Vite/React demonstration app showcasing [**SQLite Sync**](https://github.com/sqliteai/sqlite-sync) implementation for **offline-first** data synchronization across multiple devices. This example illustrates how to integrate SQLite AI's sync capabilities into modern web applications with proper authentication via [Access Token](https://docs.sqlitecloud.io/docs/access-tokens) and [Row-Level Security (RLS)](https://docs.sqlitecloud.io/docs/rls).


## Features

From a **user experience** perspective, this is a simple sport tracking application where users can:
- Create accounts and log activities (running, cycling, swimming, etc.)
- View personal statistics and workout history
- Access "Coach Mode" for managing multiple users' workouts

From a **developer perspective**, this app showcases:
- **Offline-first** architecture with sync to the remote database using **SQLite Sync** extension for SQLite
- **Row-Level Security (RLS)** implementation for data isolation and access control on the SQLite Cloud database
- **Access Tokens** for secure user authentication with SQLite Sync and RLS policy enforcement
- **Multi-user** data isolation and sharing patterns across different user sessions

## Setup Instructions

### 1. Prerequisites
- Node.js 18+
- [SQLite Cloud account](https://sqlitecloud.io)

### 2. Database Setup
1. Create database in [SQLite Cloud Dashboard](https://dashboard.sqlitecloud.io/).
2. Execute the exact schema from `sport-tracker-schema.sql`.  
3. Enable OffSync for all tables on the remote database from the **SQLite Cloud Dashboard -> Databases**.
4. Enable and configure RLS policies on the **SQLite Cloud Dashboard -> Databases**. See the file `rls-policies.md`.

### 3. Environment Configuration

Rename the `.env.example` into `.env` and fill with your values.

### 4. Installation & Run

```bash
npm install
npm run dev
```

> This app uses the packed WASM version of SQLite with the [SQLite Sync extension enabled](https://www.npmjs.com/package/@sqliteai/sqlite-sync-wasm).

## Demo Use Case: Multi-User Sync Scenario

This walkthrough demonstrates how SQLite Sync handles offline-first synchronization between multiple users:

### The Story: Bob the Runner & Coach Sarah

1. **Bob starts tracking offline** 📱
   - Open [localhost:5173](http://localhost:5173) in your browser
   - Create user `bob` and add some activities
   - Notice Bob's data is stored locally - no internet required!

2. **Bob goes online and syncs** 🌐
   - Click `SQLite Sync` to authenticate SQLite Sync
   - Click `Sync & Refresh` - this generates an Access Token and synchronizes Bob's local data to the cloud
   - Bob's activities are now replicated in the cloud

3. **Coach Sarah joins from another device** 👩‍💼
   - Open a new private/incognito browser window at [localhost:5173](http://localhost:5173)
   - Create user `coach` (this triggers special coach privileges via RLS)
   - Enable `SQLite Sync` and click `Sync & Refresh`. Coach can now see Bob's synced activities thanks to RLS policies

4. **Coach creates a workout for Bob** 💪
   - Coach creates a workout assigned to Bob
   - Click `Sync & Refresh` to upload the workout to the cloud

5. **Bob receives his workout** 📲
   - Go back to Bob's browser window
   - Click `Sync & Refresh` - Bob's local database downloads the new workout from Coach
   - Bob can now see his personalized workout

6. **Bob gets a new device** 📱➡️💻
   - Log out Bob, then select it and click `Restore from cloud`
   - This simulates Bob logging in from a completely new device with no local data
   - Enable `SQLite Sync` and sync - all of Bob's activities and workouts are restored from the cloud

**Key takeaway**: Users can work offline, sync when convenient, and seamlessly restore data on new devices!


## SQLite Sync Implementation

### 1. Database Initialization

```typescript
// database.ts - Initialize sync for each table
export class Database {
  async initSync() {
    await this.exec('SELECT cloudsync_init("users")');
    await this.exec('SELECT cloudsync_init("activities")');
    await this.exec('SELECT cloudsync_init("workouts")');
  }
}
```

### 2. Token Management

```typescript
// SQLiteSync.ts - Access token handling
private async getValidToken(userId: string, name: string): Promise<string> {
  const storedTokenData = localStorage.getItem('token');
  
  if (storedTokenData) {
    const parsed: TokenData = JSON.parse(storedTokenData);
    const tokenExpiry = new Date(parsed.expiresAt);
    
    if (tokenExpiry > new Date()) {
      return parsed.token; // Use cached token
    }
  }
  
  // Fetch new token from API
  const tokenData = await this.fetchNewToken(userId, name);
  localStorage.setItem('token', JSON.stringify(tokenData));
  return tokenData.token;
}
```

Then authorize SQLite Sync with the token. This operation is executed again when tokens expire and a new one is provided.

```typescript
async sqliteSyncSetToken(token: string) {
  await this.exec(`SELECT cloudsync_network_set_token('${token}')`);
}
```

### 3. Synchronization

The sync operation sends local changes to the cloud and receives remote changes:

```typescript
async sqliteSyncNetworkSync() {
  await this.exec('SELECT cloudsync_network_sync()');
}
```

## Row-Level Security (RLS)

This app demonstrates **Row-Level Security** configured in the SQLite Cloud Dashboard. RLS policies ensure:

- **Users** can only see their own activities and workouts
- **Coaches** can access all users' data and create workouts for the users
- **Data isolation** is enforced at the database level

### Example RLS Policies

```sql
-- Policy for selecting activities
auth_userid() = user_id OR json_extract(auth_json(), '$.name') = 'coach'

-- Policy for inserting into workouts table  
json_extract(auth_json(), '$.name') = 'coach'
```

> **Note**: Configure RLS policies in your SQLite Cloud Dashboard under Databases → RLS

## Security Considerations

⚠️ **Important**: This demo includes client-side API key usage for simplicity. In production:

- Never expose API keys in client code
- Use **server-side generation** for Access Tokens
- Implement a proper authentication flow

## Documentation Links

Explore the code and learn more:

- **SQLite Sync API**: [sqlite-sync](https://github.com/sqliteai/sqlite-sync/blob/main/API.md)
- **Access Tokens Guide**: [SQLite Cloud Access Tokens](https://docs.sqlitecloud.io/docs/access-tokens)
- **Row-Level Security**: [SQLite Cloud RLS](https://docs.sqlitecloud.io/docs/rls)

## Performance considerations

The database is persisted in the Origin-Private FileSystem OPFS (if available) but performance is much lower. Read more [here](https://sqlite.org/wasm/doc/trunk/persistence.md)
