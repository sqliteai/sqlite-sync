// SQLite Sync specific operations
// These operations handle SQLite Sync functionality

export const getSqliteSyncOperations = (db: any) => ({
  sqliteSyncVersion() {
    let version = "";
    db.exec({
      sql: "SELECT cloudsync_version();",
      callback: (row: any) => {
        version = row[0];
        console.log("SQLite Sync - Version:", version);
      },
    });
    return version;
  },

  /** Authorize SQLite Sync with the user's Access Token. */
  sqliteSyncSetToken(token: string) {
    db.exec(`SELECT cloudsync_network_set_token('${token}')`);
    console.log("SQLite Sync - Token set", token);
  },


  /**
   * Syncs the local SQLite database with the remote SQLite Cloud database.
   * This will push changes to the cloud and check for changes from the cloud.
   * The first attempt may not find anything to apply, but subsequent attempts
   * will find changes if they exist.
  */
  sqliteSyncNetworkSync() {
    console.log("SQLite Sync - Starting sync...");
    db.exec("SELECT cloudsync_network_sync(1000, 2);");
    console.log("SQLite Sync - Sync completed");
  },

  /**
   * Sends local changes to the SQLite Cloud database.
   */
  sqliteSyncSendChanges() {
    console.log(
      "SQLite Sync - Sending changes to your the SQLite Cloud node..."
    );
    db.exec("SELECT cloudsync_network_send_changes();");
    console.log("SQLite Sync - Changes sent");
  },

  /**
   * Logs out the user from SQLite Sync.
   */
  sqliteSyncLogout() {
    db.exec("SELECT cloudsync_network_logout();");
    console.log("SQLite Sync - Logged out");
  },

  /**
   * Checks if there are unsent changes in the local SQLite database.
   */
  sqliteSyncHasUnsentChanges() {
    let unsynchedChanges;

    db.exec({
      sql: "SELECT cloudsync_network_has_unsent_changes();",
      callback: (row: any) => {
        unsynchedChanges = row[0] === 1;
      },
    });

    console.log("SQLite Sync - Unsynched changes:", unsynchedChanges);

    if (unsynchedChanges === undefined) {
      throw new Error("SQLite Sync - Failed to check unsent changes");
    }

    return unsynchedChanges;
  },
});

/**
 * Initialize SQLite Sync.
 */
export const initSQLiteSync = (db: any) => {
  if (!db) {
    throw new Error("Database not initialized");
  }

  // Initialize SQLite Sync
  db.exec(`SELECT cloudsync_init('users');`);
  db.exec(`SELECT cloudsync_init('activities');`);
  db.exec(`SELECT cloudsync_init('workouts');`);
  // ...or initialize all tables at once 
  //  db.exec('SELECT cloudsync_init("*");');

  // Initialize SQLite Sync with the SQLite Cloud Connection String.
  // On the SQLite Cloud Dashboard, enable OffSync (SQLite Sync) 
  // on the remote database and copy the Connection String.
  db.exec(
    `SELECT cloudsync_network_init('${
      import.meta.env.VITE_SQLITECLOUD_CONNECTION_STRING
    }')`
  );
};
