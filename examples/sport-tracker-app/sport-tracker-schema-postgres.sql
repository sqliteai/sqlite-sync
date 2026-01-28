-- PostgreSQL schema
-- Use this schema to create the remote database on PostgreSQL/PostgREST

CREATE TABLE IF NOT EXISTS users_sport (
    id TEXT PRIMARY KEY NOT NULL, -- UUID's HIGHLY RECOMMENDED for global uniqueness
    name TEXT UNIQUE NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS activities (
    id TEXT PRIMARY KEY NOT NULL, -- UUID's HIGHLY RECOMMENDED for global uniqueness
    type TEXT NOT NULL DEFAULT 'runnning',
    duration INTEGER,
    distance DOUBLE PRECISION,
    calories INTEGER,
    date TEXT,
    notes TEXT,
    user_id TEXT REFERENCES users_sport (id)
);

CREATE TABLE IF NOT EXISTS workouts (
    id TEXT PRIMARY KEY NOT NULL, -- UUID's HIGHLY RECOMMENDED for global uniqueness
    name TEXT,
    type TEXT,
    duration INTEGER,
    exercises TEXT,
    date TEXT,
    completed INTEGER DEFAULT 0,
    user_id TEXT
);
