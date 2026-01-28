import { Database } from "./db/database";

export interface UserSession {
  userId: string;
  name: string;
}

export interface TokenData {
  name: string;
  token: string;
  userId: string;
  attributes?: Record<string, any> | null;
  expiresAt: string | null;
}

export class SQLiteSync {
  private db: Database;
  private static readonly TOKEN_EXPIRY_MINUTES = 10;
  private static readonly TOKEN_KEY_PREFIX = "token";

  constructor(db: Database) {
    this.db = db;
  }


  /**
   * Sets up SQLite Sync using Access Tokens authentication for a specific user.
   */
  async setupWithToken(currentSession: UserSession): Promise<void> {
    if (!this.db || !currentSession) {
      throw new Error("Database or session not available");
    }

    try {
      const { userId, name } = currentSession;

      // Get valid token (from session or fetch new one)
      const token = await this.getValidToken(userId, name);

      // Authenticate SQLite Sync with the token
      await this.db.sqliteSyncSetToken(token);
      console.log("SQLite Sync setup completed with token for user:", name);
    } catch (error) {
      console.error("Failed to setup SQLite Sync with token:", error);
      throw error;
    }
  }

  /**
   * Send and receive changes to/from the database on SQLite Cloud.
   *
   * Sync happens in these steps:
   * 1. Send local changes to the server (`cloudsync_network_send_changes()`).
   * 2. Check for changes from the server (`cloudsync_network_check_changes()`).
   * 3. Waits a moment for the server to prepare changes if any.
   * 4. Check again for changes from the server and apply them to the local database (`cloudsync_network_check_changes()`).
   */
  async sync() {
    if (!this.db) {
      throw new Error("Database not available");
    }

    await this.db.sqliteSyncNetworkSync();
  }

  /**
   * Logs out the user from SQLite Sync.
   * This operation will wipe oute the SQLite Sync data and ALL the user's synced data.
   * The method checks before that there a no changes made on the local database not yet sent to the server.
   */
  async logout(): Promise<void> {
    if (!this.db) {
      throw new Error("Database not available");
    }

    // Check for unsent changes before logout.
    const hasUnsentChanges = await this.db.sqliteSyncHasUnsentChanges();
    if (hasUnsentChanges) {
      console.warn("There are unsent changes, consider syncing before logout");
      throw new Error(
        "There are unsent changes. Please sync before logging out."
      );
    }

    await this.db.sqliteSyncLogout();

    console.log("SQLite Sync - Logout and cleanup completed");
  }

  /**
   * Gets a valid token for the user, either from localStorage or by fetching a new one
   */
  private async getValidToken(userId: string, name: string): Promise<string> {
    const storedTokenData = localStorage.getItem(SQLiteSync.TOKEN_KEY_PREFIX);

    let token = null;
    let tokenExpiry = null;

    if (storedTokenData) {
      try {
        const parsed: TokenData = JSON.parse(storedTokenData);
        token = parsed.token;
        tokenExpiry = parsed.expiresAt ? new Date(parsed.expiresAt) : null;
        console.log("SQLite Sync: Found stored token in localStorage");
      } catch (e) {
        console.error("SQLite Sync: Failed to parse stored token:", e);
      }
    } else {
      console.log("SQLite Sync: No token found in localStorage");
    }

    const now = new Date();
    if (!token) {
      console.log(
        "SQLite Sync: No token available, requesting new one from API",
      );
      const tokenData = await this.fetchNewToken(userId, name);
      // localStorage.setItem(
      //   SQLiteSync.TOKEN_KEY_PREFIX,
      //   JSON.stringify(tokenData),
      // );
      token = tokenData.token;
      console.log("SQLite Sync: New token obtained and stored in localStorage");
    } else if (tokenExpiry && tokenExpiry <= now) {
      console.warn("SQLite Sync: Token expired, requesting new one from API");
      const tokenData = await this.fetchNewToken(userId, name);
      // localStorage.setItem(
      //   SQLiteSync.TOKEN_KEY_PREFIX,
      //   JSON.stringify(tokenData),
      // );
      token = tokenData.token;
      console.log("SQLite Sync: New token obtained and stored in localStorage");
    } else {
      console.log("SQLite Sync: Using valid token from localStorage");
    }

    return token;
  }

  /**
   * Fetches a new token from the SQLite Cloud API.
   *
   * !! NOTE - DEMOSTRATION PURPOSES ONLY !!
   * This operation should only be done in trusted environments (server-side).
   */
  private async fetchNewToken(
    userId: string,
    name: string,
  ): Promise<Record<string, any>> {
    const jwt = await Promise.resolve(import.meta.env.VITE_SQLITECLOUD_API_KEY);
    return {
      token: jwt
    };
  }

  /**
   * Checks if a valid token exists in localStorage
   */
  static hasValidToken(): boolean {
    return false;
  }
}
