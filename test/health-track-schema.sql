-- SQL schema for initializing the health tracking database
CREATE TABLE IF NOT EXISTS users (
  id TEXT PRIMARY KEY NOT NULL,
  name TEXT UNIQUE NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS activities (
  id TEXT PRIMARY KEY NOT NULL,
  user_id TEXT,
  km REAL,
  bpm INTEGER,
  time TEXT,
  activity_type TEXT NOT NULL DEFAULT 'running',
  FOREIGN KEY(user_id) REFERENCES users(id)
);
CREATE TABLE IF NOT EXISTS workouts (
  id TEXT PRIMARY KEY NOT NULL,
  assigned_user_id TEXT,
  day_of_week TEXT,
  km REAL,
  max_time TEXT
);
