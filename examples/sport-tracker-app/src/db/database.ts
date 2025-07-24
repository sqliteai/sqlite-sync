export interface Activity {
  id?: string;
  type: string;
  duration: number;
  distance?: number;
  calories?: number;
  date: string;
  notes?: string;
  user_id?: string;
}

export interface Workout {
  id?: string;
  name: string;
  type: string;
  duration: number;
  date: string;
  completed?: boolean;
  user_id?: string;
}

export interface User {
  id: string;
  name: string;
}

export interface UserStats {
  id: number;
  total_activities: number;
  total_duration: number;
  total_distance: number;
  total_calories: number;
  last_updated: string;
}

export interface DatabaseCounts {
  users: number;
  activities: number;
  workouts: number;
  totalUsers: number;
  totalActivities: number;
  totalWorkouts: number;
}

export class Database {
  private worker: Worker;
  private messageId = 0;
  private pendingMessages = new Map<
    number,
    { resolve: Function; reject: Function }
  >();

  constructor() {
    this.worker = new Worker(new URL("./worker.ts", import.meta.url), {
      type: "module",
    });

    // Response message from the worker
    this.worker.onmessage = (e) => {
      const { result, error, id } = e.data;
      const pending = this.pendingMessages.get(id);

      if (pending) {
        this.pendingMessages.delete(id);
        if (error) {
          pending.reject(new Error(error));
        } else {
          pending.resolve(result);
        }
      }
    };
  }

  private sendMessage(type: string, data?: any): Promise<any> {
    const id = ++this.messageId;

    return new Promise((resolve, reject) => {
      this.pendingMessages.set(id, { resolve, reject });
      this.worker.postMessage({ type, data, id });
    });
  }

  async init(): Promise<void> {
    await this.sendMessage("init");
    await this.sendMessage("sqliteSyncVersion");
  }

  async sqliteSyncVersion(): Promise<string> {
    return this.sendMessage("sqliteSyncVersion");
  }

  async sqliteVersion(): Promise<string> {
    return this.sendMessage("sqliteVersion");
  }

  async sqliteSyncSetToken(token: string): Promise<void> {
    return this.sendMessage("sqliteSyncSetToken", token);
  }


  async sqliteSyncNetworkSync(): Promise<void> {
    return this.sendMessage("sqliteSyncNetworkSync");
  }

  async sqliteSyncSendChanges(): Promise<void> {
    return this.sendMessage("sqliteSyncSendChanges");
  }

  async sqliteSyncLogout(): Promise<void> {
    return this.sendMessage("sqliteSyncLogout");
  }

  async sqliteSyncHasUnsentChanges(): Promise<boolean> {
    return this.sendMessage("sqliteSyncHasUnsentChanges");
  }

  async addActivity(activity: Activity): Promise<any> {
    return this.sendMessage("addActivity", activity);
  }

  async getActivities(userId?: string, isCoach?: boolean): Promise<Activity[]> {
    return this.sendMessage("getActivities", {
      user_id: userId,
      is_coach: isCoach,
    });
  }

  async addWorkout(workout: Workout): Promise<any> {
    return this.sendMessage("addWorkout", workout);
  }

  async getWorkouts(userId?: string, isCoach?: boolean): Promise<Workout[]> {
    return this.sendMessage("getWorkouts", {
      user_id: userId,
      is_coach: isCoach,
    });
  }

  async completeWorkout(id: string): Promise<any> {
    return this.sendMessage("completeWorkout", id);
  }

  async deleteActivity(id: string): Promise<any> {
    return this.sendMessage("deleteActivity", { id });
  }

  async deleteWorkout(id: string): Promise<any> {
    return this.sendMessage("deleteWorkout", { id });
  }

  async getStats(userId?: string, isCoach?: boolean): Promise<UserStats> {
    return this.sendMessage("getStats", {
      user_id: userId,
      is_coach: isCoach,
    });
  }

  async createUser(user: User): Promise<User> {
    return this.sendMessage("createUser", user);
  }

  async getUsers(): Promise<User[]> {
    return this.sendMessage("getUsers");
  }

  async getUserById(id: string): Promise<User | null> {
    return this.sendMessage("getUserById", { id });
  }

  async getCounts(userId?: string, isCoach?: boolean): Promise<DatabaseCounts> {
    return this.sendMessage("getCounts", {
      user_id: userId,
      is_coach: isCoach,
    });
  }

  async close(): Promise<void> {
    // Clear any pending messages
    this.pendingMessages.clear();

    // Terminate the worker
    this.worker.terminate();
    console.log("Database connection closed");
  }
}
