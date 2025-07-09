import React, { useCallback, useEffect, useState } from "react";
import ActivityCard from "./components/Activities/ActivityCard";
import AppFooter from "./components/AppFooter";
import DatabaseStatus from "./components/DatabaseStatus";
import StatCard from "./components/Statistics/StatCard";
import UserCreation from "./components/UserCreation";
import UserLogin from "./components/UserLogin";
import WorkoutCard from "./components/Workouts/WorkoutCard";
import { DatabaseProvider, useDatabase } from "./context/DatabaseContext";
import type {
  Activity,
  DatabaseCounts,
  User,
  UserStats,
  Workout,
} from "./db/database";
import "./style.css";

interface UserSession {
  userId: string;
  name: string;
}

const AppContent: React.FC = () => {
  const { db, isInitialized, error } = useDatabase();
  const [activities, setActivities] = useState<Activity[]>([]);
  const [workouts, setWorkouts] = useState<Workout[]>([]);
  const [stats, setStats] = useState<UserStats | null>(null);
  const [currentUser, setCurrentUser] = useState<User | null>(null);
  const [users, setUsers] = useState<User[]>([]);
  const [currentSession, setCurrentSession] = useState<UserSession | null>(
    null
  );
  const [selectedWorkoutUser, setSelectedWorkoutUser] = useState<string>("");
  const [loading, setLoading] = useState(true);
  const [refreshTrigger, setRefreshTrigger] = useState(0);
  const [sqliteSyncVersion, setSqliteSyncVersion] = useState<string>("");
  const [sqliteVersion, setSqliteVersion] = useState<string>("");

  // Coach mode - true when logged in user is named "coach"
  const isCoachMode = (session: UserSession | null): boolean => {
    return session?.name === "coach";
  };

  const coachMode = isCoachMode(currentSession);

  const getUserName = (userId?: string): string | undefined => {
    if (!userId) return undefined;
    const user = users.find((u) => u.id === userId);
    return user?.name;
  };

  useEffect(() => {
    // Reload session from localStorage
    const savedSession = localStorage.getItem("userSession");
    if (savedSession) {
      try {
        const session: UserSession = JSON.parse(savedSession);
        setCurrentSession(session);
        loadUser(session.userId);
      } catch (error) {
        console.error("Failed to parse saved session:", error);
        localStorage.removeItem("userSession");
      }
    }

    // Fetch database versions
    if (db) {
      db.sqliteSyncVersion()
        .then((version: string) => {
          setSqliteSyncVersion(version);
        })
        .catch((error: any) => {
          console.error("Failed to get SQLite Sync version:", error);
        });

      db.sqliteVersion()
        .then((version: string) => {
          setSqliteVersion(version);
        })
        .catch((error: any) => {
          console.error("Failed to get SQLite version:", error);
        });
    }
  }, [db]);

  const loadUser = async (userId: string) => {
    if (!db) return;
    try {
      const user = await db.getUserById(userId);
      if (user) {
        setCurrentUser(user);
      } else {
        // User doesn't exist anymore, clear session
        await handleLogout();
      }
    } catch (error) {
      console.error("Failed to load user:", error);
      await handleLogout();
    }
  };

  const loadUsers = useCallback(async () => {
    if (!db) return;
    try {
      const usersList = await db.getUsers();
      setUsers(usersList);
    } catch (error) {
      console.error("Failed to load users:", error);
    }
  }, [db, currentSession, selectedWorkoutUser]);

  const loadCounts = useCallback(async (): Promise<DatabaseCounts> => {
    const defaultCounts = {
      users: 0,
      activities: 0,
      workouts: 0,
      totalUsers: 0,
      totalActivities: 0,
      totalWorkouts: 0,
    };
    
    if (!db) return defaultCounts;
    
    try {
      const userId = currentSession?.userId;
      const isCoach = coachMode;
      return await db.getCounts(userId, isCoach);
    } catch (error) {
      console.error("Failed to load database counts:", error);
      return defaultCounts;
    }
  }, [db, currentSession, coachMode]);

  const triggerRefresh = () => {
    setRefreshTrigger((prev) => prev + 1);
  };

  const handleRefreshData = async () => {
    await Promise.all([loadUsers(), loadAllData()]);
    triggerRefresh();
  };

  useEffect(() => {
    if (db && isInitialized) {
      loadAllData();
      loadUsers();
    }
  }, [db, isInitialized, loadUsers]);

  // Reload data when session or coach mode changes
  useEffect(() => {
    if (db && isInitialized) {
      loadAllData();
    }
  }, [currentSession?.userId, coachMode]);

  const loadAllData = async () => {
    if (!db) return;
    try {
      if (!currentSession) {
        // No user logged in - clear all data
        setActivities([]);
        setWorkouts([]);
        setStats(null);
        setLoading(false);
        return;
      }

      const userId = currentSession.userId;
      const isCoach = coachMode;

      const [activitiesList, workoutsList, userStats] = await Promise.all([
        db.getActivities(userId, isCoach),
        db.getWorkouts(userId, isCoach),
        db.getStats(userId, isCoach),
      ]);
      setActivities(activitiesList);
      setWorkouts(workoutsList);
      setStats(userStats);
    } catch (error) {
      console.error("Failed to load data:", error);
    } finally {
      setLoading(false);
    }
  };

  const handleAddActivity = async () => {
    if (!db || !currentSession) return;
    try {
      // Generate random activity
      const activityTypes = [
        "running",
        "cycling",
        "swimming",
        "walking",
        "gym",
        "other",
      ];
      const randomActivity: Activity = {
        type: activityTypes[Math.floor(Math.random() * activityTypes.length)],
        duration: Math.floor(Math.random() * 120) + 15, // 15-135 minutes
        distance: Math.round((Math.random() * 20 + 1) * 10) / 10, // 1-21 km, 1 decimal
        calories: Math.floor(Math.random() * 800) + 100, // 100-900 calories
        date: new Date().toISOString().split("T")[0],
        notes: `Random activity #${Date.now()}`,
        user_id: currentSession.userId,
      };

      await db.addActivity(randomActivity);
      // Optionally, you can send changes to SQLite Cloud if enabled
      //   await db.sqliteSyncSendChanges();

      await loadAllData();
      triggerRefresh();
    } catch (error) {
      console.error("Failed to add activity:", error);
      alert("Failed to add activity. Please try again.");
    }
  };

  const handleAddWorkout = async () => {
    if (!db || !selectedWorkoutUser || !coachMode) return;
    try {
      // Generate random workout
      const workoutTypes = [
        "strength",
        "cardio",
        "flexibility",
        "endurance",
        "HIIT",
      ];
      const workoutNames = [
        "Morning Routine",
        "Power Session",
        "Quick Blast",
        "Intense Training",
        "Focus Session",
      ];

      const randomWorkout: Workout = {
        name: workoutNames[Math.floor(Math.random() * workoutNames.length)],
        type: workoutTypes[Math.floor(Math.random() * workoutTypes.length)],
        duration: Math.floor(Math.random() * 60) + 15, // 15-75 minutes
        date: new Date().toISOString().split("T")[0],
        user_id: selectedWorkoutUser,
      };

      await db.addWorkout(randomWorkout);
      // Optionally, you can send changes to SQLite Cloud if enabled
      //   await db.sqliteSyncSendChanges();

      await loadAllData();
      triggerRefresh();
    } catch (error) {
      console.error("Failed to add workout:", error);
      alert("Failed to add workout. Please try again.");
    }
  };

  const handleCompleteWorkout = async (id: string) => {
    if (!db) return;
    try {
      await db.completeWorkout(id);
      await loadAllData();
    } catch (error) {
      console.error("Failed to complete workout:", error);
      alert("Failed to complete workout. Please try again.");
    }
  };

  const handleDeleteActivity = async (activityId: string) => {
    if (!db) return;
    try {
      await db.deleteActivity(activityId);
      await loadAllData();
      triggerRefresh();
    } catch (error) {
      console.error("Failed to delete activity:", error);
      alert("Failed to delete activity. Please try again.");
    }
  };

  const handleDeleteWorkout = async (workoutId: string) => {
    if (!db) return;
    try {
      await db.deleteWorkout(workoutId);
      await loadAllData();
      triggerRefresh();
    } catch (error) {
      console.error("Failed to delete workout:", error);
      alert("Failed to delete workout. Please try again.");
    }
  };

  const handleUserCreate = async (userData: User) => {
    if (!db) return;
    try {
      const newUser = await db.createUser(userData);
      // Optionally, you can send changes to SQLite Cloud if enabled
      //   await db.sqliteSyncSendChanges();

      setCurrentUser(newUser);
      // Update the users list
      await loadUsers();
      triggerRefresh();
      // Auto-login the newly created user
      handleLogin({
        userId: newUser.id,
        name: newUser.name,
      });
    } catch (error) {
      console.error("Failed to create user:", error);
      if (
        error instanceof Error &&
        error.message.includes("UNIQUE constraint failed")
      ) {
        alert(
          "A user with this name already exists. Please choose a different name."
        );
      } else {
        alert("Failed to create user. Please try again.");
      }
      throw error;
    }
  };

  const handleUserRestore = async (userData: User) => {
    if (!db) return;
    try {
      // First, try to create the user in the local database
      const restoredUser = await db.createUser(userData);
      setCurrentUser(restoredUser);
      // Update the users list
      await loadUsers();
      triggerRefresh();
      // Auto-login the restored user
      handleLogin({
        userId: restoredUser.id,
        name: restoredUser.name,
      });
    } catch (error) {
      console.error("Failed to restore user:", error);
      if (
        error instanceof Error &&
        error.message.includes("UNIQUE constraint failed")
      ) {
        alert(
          "A user with this name already exists locally. Please choose a different user."
        );
      } else {
        alert("Failed to restore user. Please try again.");
      }
      throw error;
    }
  };

  const handleLogin = (session: UserSession) => {
    setCurrentSession(session);
    localStorage.setItem("userSession", JSON.stringify(session));
    loadUser(session.userId);
    loadAllData();
    // Populate workout user selector with all users when coach logs in
    if (isCoachMode(session)) {
      setSelectedWorkoutUser(users.length > 0 ? users[0].id : "");
    } else {
      setSelectedWorkoutUser("");
    }
  };

  const handleLogout = async () => {
    setCurrentSession(null);
    setCurrentUser(null);
    localStorage.removeItem("userSession");
    // Clear the data when logged out
    setActivities([]);
    setWorkouts([]);
    setStats(null);
    // Clear workout user selector
    setSelectedWorkoutUser("");
    // Update user list and trigger refresh to reflect database state
    await loadUsers();
    triggerRefresh();
  };

  if (error) {
    return (
      <div className="error-container">
        <h2>Database Error</h2>
        <p>{error}</p>
      </div>
    );
  }

  if (!isInitialized || loading) {
    return (
      <div className="loading-container">
        <h2>Initializing Database...</h2>
        <p>Please wait while we set up your sport tracking database.</p>
      </div>
    );
  }

  return (
    <div className="app">
      <header className="app-header">
        <h1>Sport Tracker</h1>
      </header>

      <div className="login-section">
        {!currentSession && (
          <div className="user-management-section">
            <UserCreation
              onUserCreate={handleUserCreate}
              onUserRestore={handleUserRestore}
              currentUser={currentUser}
            />
          </div>
        )}

        <UserLogin
          users={users}
          currentSession={currentSession}
          onLogin={handleLogin}
          onLogout={handleLogout}
          onUsersLoad={loadUsers}
          onRefresh={handleRefreshData}
        />
        <DatabaseStatus
          onCountsLoad={loadCounts}
          refreshTrigger={refreshTrigger}
        />
      </div>

      <main className="app-main">
        {/* Welcome message when no user is logged in */}
        {!currentSession && (
          <section className="section">
            <div className="welcome-message">
              <p>
                Please create a user and log in to start tracking your
                activities and workouts.
                <br />
                Alternatively, you can restore a user from the remote database
                on SQLite Cloud.
              </p>
              <p>
                💡 <strong>Tip:</strong> The "coach" user has special privileges
                to create workouts for all users and to manage users'
                activities.
              </p>
            </div>
          </section>
        )}

        {/* Statistics Section - only show when logged in */}
        {currentSession && (
          <section className="section">
            <h2>Statistics</h2>
            <div className="stats-grid">
              <StatCard
                title="Total Activities"
                value={stats?.total_activities || 0}
              />
              <StatCard
                title="Total Duration"
                value={`${Math.round((stats?.total_duration || 0) / 60)} hours`}
              />
              <StatCard
                title="Total Distance"
                value={`${(stats?.total_distance || 0).toFixed(1)} km`}
              />
              <StatCard
                title="Total Calories"
                value={stats?.total_calories || 0}
              />
            </div>
          </section>
        )}

        {/* Activities Section - only show when logged in */}
        {currentSession && (
          <section className="section">
            <div className="section-header">
              <h2>Activities</h2>
              <button
                className="btn-primary"
                onClick={handleAddActivity}
                disabled={!currentSession}
              >
                Add Random Activity
              </button>
            </div>

            <div className="items-grid">
              {activities.length === 0 ? (
                <p className="empty-state">
                  No activities yet. Add your first activity!
                </p>
              ) : (
                activities
                  .slice(0, 6)
                  .map((activity) => (
                    <ActivityCard
                      key={activity.id}
                      activity={activity}
                      onDelete={handleDeleteActivity}
                      canDelete={
                        coachMode || activity.user_id === currentSession?.userId
                      }
                      userName={getUserName(activity.user_id)}
                    />
                  ))
              )}
            </div>
          </section>
        )}

        {/* Workouts Section - only show when logged in */}
        {currentSession && (
          <section className="section">
            <div className="section-header">
              <h2>
                Workouts{" "}
                {coachMode && (
                  <span className="coach-badge">👨‍💼 Coach Mode</span>
                )}
              </h2>
              {coachMode && (
                <div className="workout-controls">
                  <select
                    className="user-selector"
                    value={selectedWorkoutUser}
                    onChange={(e) => setSelectedWorkoutUser(e.target.value)}
                    disabled={!currentSession}
                  >
                    <option value="">Select user for workout</option>
                    {users.map((user) => (
                      <option key={user.id} value={user.id}>
                        {user.name} [{user.id.slice(0, 6)}]
                      </option>
                    ))}
                  </select>
                  <button
                    className="btn-primary"
                    onClick={handleAddWorkout}
                    disabled={!currentSession || !selectedWorkoutUser}
                  >
                    Add Random Workout
                  </button>
                </div>
              )}
            </div>

            <div className="items-grid">
              {workouts.length === 0 ? (
                <p className="empty-state">
                  No workouts yet. Add your first workout!
                </p>
              ) : (
                workouts
                  .slice(0, 6)
                  .map((workout) => (
                    <WorkoutCard
                      key={workout.id}
                      workout={workout}
                      onComplete={handleCompleteWorkout}
                      onDelete={handleDeleteWorkout}
                      canDelete={coachMode}
                      userName={getUserName(workout.user_id)}
                    />
                  ))
              )}
            </div>
          </section>
        )}
      </main>
      <AppFooter
        sqliteSyncVersion={sqliteSyncVersion}
        sqliteVersion={sqliteVersion}
      />
    </div>
  );
};

const App: React.FC = () => {
  return (
    <DatabaseProvider>
      <AppContent />
    </DatabaseProvider>
  );
};

export default App;
