import React, { useState, useEffect, useCallback } from "react";
import type { User } from "../db/database";
import { useDatabase } from "../context/DatabaseContext";
import { SQLiteSync } from "../SQLiteSync";

interface UserSession {
  userId: string;
  name: string;
}

interface UserLoginProps {
  users: User[];
  currentSession: UserSession | null;
  onLogin: (session: UserSession) => void;
  onLogout: () => Promise<void>;
  onUsersLoad: () => void;
  onRefresh: () => void;
}

const UserLogin: React.FC<UserLoginProps> = ({
  users,
  currentSession,
  onLogin,
  onLogout,
  onUsersLoad,
  onRefresh,
}) => {
  const { db } = useDatabase();
  const [selectedUserId, setSelectedUserId] = useState<string>("");
  const [sqliteSyncEnabled, setSqliteSyncEnabled] = useState<boolean>(false);
  const [sqliteSync, setSqliteSync] = useState<SQLiteSync | null>(null);
  const [isRefreshing, setIsRefreshing] = useState<boolean>(false);
  const [isLoggingOut, setIsLoggingOut] = useState<boolean>(false);

  useEffect(() => {
    onUsersLoad();
  }, [onUsersLoad]);

  useEffect(() => {
    if (db) {
      setSqliteSync(new SQLiteSync(db));
    }
  }, [db]);

  // Initialize sync state when user logs in
  useEffect(() => {
    // No user logged in - disable sync
    if (!currentSession) {
      setSqliteSyncEnabled(false);
      return;
    }

    // Check if there's a valid token available
    const hasValidToken = SQLiteSync.hasValidToken();
    if (hasValidToken) {
      console.log(
        "Valid token found in localStorage for user:",
        currentSession.name
      );
    } else {
      console.log(
        "No valid token found in localStorage for user:",
        currentSession.name
      );
    }

    setSqliteSyncEnabled(false);
  }, [currentSession]);

  // Handle SQLite Sync enable/disable toggle
  const handleSyncToggle = async (checked: boolean) => {
    setSqliteSyncEnabled(checked);

    if (checked) {
      console.log("SQLite Sync enabled for user:", currentSession?.name);
    } else {
      console.log("SQLite Sync disabled");
    }
  };

  const handleLogin = () => {
    const selectedUser = users.find((user) => user.id === selectedUserId);
    if (selectedUser) {
      const session: UserSession = {
        userId: selectedUser.id,
        name: selectedUser.name,
      };
      onLogin(session);
    }
  };

  const formatUserDisplay = (user: User) => {
    const shortId = user.id ? user.id.slice(0, 6) : "no-id";
    const display = `${user.name} [${shortId}]`;
    return display;
  };

  const handleRefreshClick = async () => {
    setIsRefreshing(true);

    try {
      // If SQLite Sync is enabled, sync with cloud before refreshing
      if (sqliteSyncEnabled && sqliteSync && currentSession) {
        try {
          await sqliteSync.setupWithToken(currentSession);

          console.log("SQLite Sync - Starting sync...");
          await sqliteSync.sync();
          console.log("SQLite Sync - Sync completed successfully");
        } catch (error) {
          console.error(
            "SQLite Sync - Failed to sync with SQLite Cloud:",
            error
          );
          console.warn("SQLite Sync: Falling back to local refresh only");
        }
      } else {
        console.log(
          "SQLite Sync disabled - refreshing from local database only"
        );
      }

      // Refresh data from database
      onRefresh();
    } finally {
      setIsRefreshing(false);
    }
  };

  const handleLogout = async () => {
    setIsLoggingOut(true);

    try {
      // If SQLite Sync is enabled, perform complete logout
      if (sqliteSyncEnabled && sqliteSync) {
        try {
          console.log("Performing SQLite Sync logout...");
          await sqliteSync.logout();
        } catch (error) {
          console.error("SQLite Sync logout error:", error);
          alert("Logout: " + error);
          return;
        }
      }

      // Clear tokens from localStorage
      if (currentSession) {
        localStorage.clear();
      }

      // Reset SQLite Sync state
      setSqliteSyncEnabled(false);

      await onLogout();
    } finally {
      setIsLoggingOut(false);
    }
  };

  if (currentSession) {
    return (
      <div className="user-login logged-in">
        <div className="user-header">
          <span className="logged-user">
            Logged in as: {currentSession.name} [
            {currentSession.userId
              ? currentSession.userId.slice(0, 6)
              : "no-id"}
            ]
          </span>
          <button
            className="btn-logout"
            onClick={handleLogout}
            disabled={isLoggingOut}
          >
            {isLoggingOut ? "Logging out..." : "Logout"}
          </button>
        </div>
        <div className="sync-controls">
          <label className="sync-checkbox">
            <input
              type="checkbox"
              checked={sqliteSyncEnabled}
              onChange={(e) => handleSyncToggle(e.target.checked)}
            />
            SQLite Sync
          </label>
          <button
            className="btn-secondary"
            onClick={handleRefreshClick}
            disabled={isRefreshing}
          >
            {isRefreshing
              ? "Refreshing..."
              : sqliteSyncEnabled
              ? "Sync & Refresh"
              : "Refresh"}
          </button>
        </div>
      </div>
    );
  }

  return (
    <div className="user-login">
      <select
        value={selectedUserId}
        onChange={(e) => setSelectedUserId(e.target.value)}
        className="user-selector"
      >
        <option value="">Select a user...</option>
        {users.map((user) => {
          return (
            <option key={user.id} value={user.id}>
              {formatUserDisplay(user)}
            </option>
          );
        })}
      </select>
      <button
        className="btn-login"
        onClick={handleLogin}
        disabled={!selectedUserId}
      >
        Login
      </button>
    </div>
  );
};

export default UserLogin;
