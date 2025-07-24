// Regular database operations
// These operations handle local SQLite database functionality

function generateUUID() {
  return "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx".replace(/[xy]/g, function (c) {
    const r = (Math.random() * 16) | 0;
    const v = c == "x" ? r : (r & 0x3) | 0x8;
    return v.toString(16);
  });
}

export const getDatabaseOperations = (db: any) => ({
    sqliteVersion() {
    let version = "";
    db.exec({
      sql: "SELECT sqlite_version();",
      callback: (row: any) => {
        version = row[0];
        console.log("SQLite - Version:", version);
      },
    });
    return version;
  },
  
  addActivity(data: any) {
    const { type, duration, distance, calories, date, notes, user_id } = data;
    const id = generateUUID();

    db.exec({
      sql: `INSERT INTO activities (id, type, duration, distance, calories, date, notes, user_id) 
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
      bind: [
        id,
        type,
        duration,
        distance || null,
        calories || null,
        date,
        notes || null,
        user_id,
      ],
    });

    return { id, ...data };
  },

  getActivities(data?: any) {
    const { user_id, is_coach } = data || {};
    const activities: any[] = [];

    let sql =
      "SELECT id, type, duration, distance, calories, date, notes, user_id FROM activities";
    let bind: any[] = [];

    if (user_id && !is_coach) {
      sql += " WHERE user_id = ?";
      bind.push(user_id);
    }

    sql += " ORDER BY date DESC";

    db.exec({
      sql,
      bind: bind.length > 0 ? bind : undefined,
      callback: (row: any) => {
        activities.push({
          id: row[0],
          type: row[1],
          duration: row[2],
          distance: row[3],
          calories: row[4],
          date: row[5],
          notes: row[6],
          user_id: row[7],
        });
      },
    });
    return activities;
  },

  addWorkout(data: any) {
    const { name, type, duration, exercises, date, user_id } = data;
    const id = generateUUID();

    db.exec({
      sql: `INSERT INTO workouts (id, name, type, duration, exercises, date, user_id) 
            VALUES (?, ?, ?, ?, ?, ?, ?)`,
      bind: [
        id,
        name,
        type,
        duration,
        JSON.stringify(exercises),
        date,
        user_id,
      ],
    });
    return { id, ...data };
  },

  getWorkouts(data?: any) {
    const { user_id, is_coach } = data || {};
    const workouts: any[] = [];

    let sql =
      "SELECT id, name, type, duration, exercises, date, completed, user_id FROM workouts";
    let bind: any[] = [];

    if (user_id && !is_coach) {
      sql += " WHERE user_id = ?";
      bind.push(user_id);
    }

    sql += " ORDER BY date DESC";

    db.exec({
      sql,
      bind: bind.length > 0 ? bind : undefined,
      callback: (row: any) => {
        workouts.push({
          id: row[0],
          name: row[1],
          type: row[2],
          duration: row[3],
          exercises: JSON.parse(row[4] || "[]"),
          date: row[5],
          completed: row[6],
          user_id: row[7],
        });
      },
    });
    return workouts;
  },

  completeWorkout(id: string) {
    return db.exec({
      sql: "UPDATE workouts SET completed = 1 WHERE id = ?",
      bind: [id],
    });
  },

  deleteActivity(data: any) {
    const { id } = data;
    try {
      db.exec({
        sql: "DELETE FROM activities WHERE id = ?",
        bind: [id],
      });
      return { success: true };
    } catch (error) {
      throw new Error(`Failed to delete activity: ${error}`);
    }
  },

  deleteWorkout(data: any) {
    const { id } = data;
    try {
      db.exec({
        sql: "DELETE FROM workouts WHERE id = ?",
        bind: [id],
      });
      return { success: true };
    } catch (error) {
      throw new Error(`Failed to delete workout: ${error}`);
    }
  },

  getStats(data?: any) {
    const { user_id, is_coach } = data || {};
    let stats = {
      id: 1,
      total_activities: 0,
      total_duration: 0,
      total_distance: 0,
      total_calories: 0,
      last_updated: new Date().toISOString(),
    };

    // Calculate stats from activities table
    let sql = `SELECT 
              COUNT(*) as count,
              COALESCE(SUM(duration), 0) as total_duration,
              COALESCE(SUM(distance), 0) as total_distance,
              COALESCE(SUM(calories), 0) as total_calories
            FROM activities`;

    let bind: any[] = [];

    // Filter by user if not coach and user_id is provided
    if (user_id && !is_coach) {
      sql += ` WHERE user_id = ?`;
      bind = [user_id];
    }

    db.exec({
      sql,
      bind: bind.length > 0 ? bind : undefined,
      callback: (row: any) => {
        stats = {
          id: 1,
          total_activities: row[0],
          total_duration: row[1],
          total_distance: row[2],
          total_calories: row[3],
          last_updated: new Date().toISOString(),
        };
      },
    });

    return stats;
  },

  createUser(data: any) {
    const { id, name } = data;
    const userId = id || generateUUID();

    try {
      db.exec({
        sql: "INSERT INTO users (id, name) VALUES (?, ?)",
        bind: [userId, name],
      });
      return { id: userId, name };
    } catch (error) {
      throw new Error(`Failed to create user: ${error}`);
    }
  },

  getUsers() {
    const users: any[] = [];
    db.exec({
      sql: "SELECT id, name FROM users ORDER BY name",
      callback: (row: any) => {
        users.push({
          id: row[0],
          name: row[1],
        });
      },
    });
    return users;
  },

  getUserById(data: any) {
    const { id } = data;
    let user = null;
    db.exec({
      sql: "SELECT id, name FROM users WHERE id = ?",
      bind: [id],
      callback: (row: any) => {
        user = {
          id: row[0],
          name: row[1],
        };
      },
    });
    return user;
  },

  getCounts(data?: any) {
    const { user_id, is_coach } = data || {};
    const counts = {
      users: 0,
      activities: 0,
      workouts: 0,
      totalActivities: 0,
      totalWorkouts: 0,
      totalUsers: 0,
    };

    // Get total counts (always show total for comparison)
    db.exec({
      sql: "SELECT COUNT(*) FROM users WHERE name != ?",
      bind: ["coach"],
      callback: (row: any) => (counts.totalUsers = row[0]),
    });

    db.exec({
      sql: "SELECT COUNT(*) FROM activities",
      callback: (row: any) => (counts.totalActivities = row[0]),
    });

    db.exec({
      sql: "SELECT COUNT(*) FROM workouts",
      callback: (row: any) => (counts.totalWorkouts = row[0]),
    });

    // Get user-specific or all counts depending on role
    if (user_id && !is_coach) {
      // Regular user - count only their data
      db.exec({
        sql: "SELECT COUNT(*) FROM users WHERE name != ?",
        bind: ["coach"],
        callback: (row: any) => (counts.users = row[0]),
      });

      db.exec({
        sql: "SELECT COUNT(*) FROM activities WHERE user_id = ?",
        bind: [user_id],
        callback: (row: any) => (counts.activities = row[0]),
      });

      db.exec({
        sql: "SELECT COUNT(*) FROM workouts WHERE user_id = ?",
        bind: [user_id],
        callback: (row: any) => (counts.workouts = row[0]),
      });
    } else {
      // Coach or no user - show all data
      counts.users = counts.totalUsers;
      counts.activities = counts.totalActivities;
      counts.workouts = counts.totalWorkouts;
    }

    return counts;
  },
});
