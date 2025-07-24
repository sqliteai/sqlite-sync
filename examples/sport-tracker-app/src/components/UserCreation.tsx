import React, { useEffect, useState } from "react";
import type { User } from "../db/database";

interface UserCreationProps {
  onUserCreate: (user: User) => void;
  onUserRestore: (user: User) => void;
  currentUser: User | null;
}

/**
 * !! NOTE - DEMOSTRATION PURPOSES ONLY !!
 * Never perform requests using the SQLite Cloud API Key from the client-side.
 * This is just an example to dimostrate the restore of a user and its data
 * from the synced SQLite Cloud database.
 */
const fetchRemoteUsers = async (): Promise<User[]> => {
  const response = await fetch(
    `${import.meta.env.VITE_SQLITECLOUD_API_URL}/v2/weblite/sql`,
    {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${import.meta.env.VITE_SQLITECLOUD_API_KEY}`,
      },
      body: JSON.stringify({
        sql: "SELECT id, name FROM users;",
        database: import.meta.env.VITE_SQLITECLOUD_DATABASE || "",
      }),
    }
  );

  if (!response.ok) {
    throw new Error(`Failed to fetch users: ${response.status}`);
  }

  const result = await response.json();
  return result.data;
};

const UserCreation: React.FC<UserCreationProps> = ({
  onUserCreate,
  onUserRestore,
  currentUser,
}) => {
  const [userName, setUserName] = useState("");
  const [isCreating, setIsCreating] = useState(false);
  const [remoteUsers, setRemoteUsers] = useState<User[]>([]);
  const [selectedRemoteUser, setSelectedRemoteUser] = useState<string>("");
  const [isLoadingRemoteUsers, setIsLoadingRemoteUsers] = useState(false);
  const [isRestoring, setIsRestoring] = useState(false);

  // Load remote users when component mounts
  useEffect(() => {
    const loadRemoteUsers = async () => {
      setIsLoadingRemoteUsers(true);
      try {
        const users = await fetchRemoteUsers();
        setRemoteUsers(users);
      } catch (error) {
        console.error("Failed to load remote users:", error);
      } finally {
        setIsLoadingRemoteUsers(false);
      }
    };

    loadRemoteUsers();
  }, []);

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!userName.trim()) return;

    setIsCreating(true);
    try {
      onUserCreate({ id: "", name: userName.trim() } as User);
      setUserName("");
    } catch (error) {
      console.error("Failed to create user:", error);
      alert("Failed to create user. Please try again.");
    } finally {
      setIsCreating(false);
    }
  };

  const handleRestore = async () => {
    if (!selectedRemoteUser) return;

    const userToRestore = remoteUsers.find(
      (user) => user.id === selectedRemoteUser
    );
    if (!userToRestore) return;

    setIsRestoring(true);
    try {
      // Restore user by adding to local database and logging in
      onUserRestore(userToRestore);
      setSelectedRemoteUser("");
    } catch (error) {
      console.error("Failed to restore user:", error);
      alert("Failed to restore user. Please try again.");
    } finally {
      setIsRestoring(false);
    }
  };

  if (currentUser && currentUser.name) {
    return null;
  }

  return (
    <div className="user-creation">
      <div className="user-actions">
        {/* Create User Section */}
        <div className="create-user-section">
          <form onSubmit={handleSubmit} className="user-form">
            <input
              type="text"
              value={userName}
              onChange={(e) => setUserName(e.target.value)}
              placeholder="Enter your name"
              className="user-input"
              disabled={isCreating}
              required
            />
            <button
              type="submit"
              className="btn-create-user"
              disabled={isCreating || !userName.trim()}
            >
              {isCreating ? "Creating..." : "Create User"}
            </button>
          </form>
        </div>

        {/* Restore User Section */}
        <div className="restore-user-section">
          <div className="restore-form">
            <select
              value={selectedRemoteUser}
              onChange={(e) => setSelectedRemoteUser(e.target.value)}
              className="user-selector"
              disabled={isLoadingRemoteUsers || isRestoring}
            >
              <option value="">
                {isLoadingRemoteUsers
                  ? "Loading users..."
                  : "Select user to restore"}
              </option>
              {remoteUsers.map((user) => (
                <option key={user.id} value={user.id}>
                  {user.name} [{user.id.slice(0, 6)}]
                </option>
              ))}
            </select>
            <button
              type="button"
              onClick={handleRestore}
              className="btn-restore-user"
              disabled={
                !selectedRemoteUser || isRestoring || isLoadingRemoteUsers
              }
            >
              {isRestoring ? "Restoring..." : "Restore from cloud"}
            </button>
          </div>
        </div>
      </div>
    </div>
  );
};

export default UserCreation;
