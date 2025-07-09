import sqlite3InitModule from "@sqliteai/sqlite-sync-wasm";
import schemaSQL from "../../sport-tracker-schema.sql?raw";
import { getDatabaseOperations as getDatabaseOperations } from "./databaseOperations";
import {
  getSqliteSyncOperations,
  initSQLiteSync,
} from "./sqliteSyncOperations";

let db: any = null;
let operations: any = null;

/**
 * Initializes the SQLite database and sets up SQLite Sync operations.
 */
const initDB = async () => {
  const sqlite3 = await sqlite3InitModule();
  if ("opfs" in sqlite3) {
    // Database is persisted in OPFS but performance is much lower
    // See more: https://sqlite.org/wasm/doc/trunk/persistence.md
    db = new sqlite3.oo1.OpfsDb("/sqlite-sync-db.sqlite3", "c");
    console.log(
      "OPFS is available, created persisted database at",
      db.filename
    );
  } else {
    db = new sqlite3.oo1.DB("/sqlite-sync-db.sqlite3", "c");
    console.log(
      "OPFS is not available, created transient database",
      db.filename
    );
  }

  console.log("Create db schema...");
  db.exec(schemaSQL);

  // For simplicity, tables are already initialized for SQLitte Sync
  console.log("SQLite Sync - Initializing...");
  try {
    initSQLiteSync(db);
    console.log("SQLite Sync - Initialized successfully");
  } catch (error) {
    console.error("SQLite Sync - Initialization failed:", error);
  }

  // Initialize operations with db instance
  const sqliteSyncOps = getSqliteSyncOperations(db);
  const databaseOps = getDatabaseOperations(db);
  operations = { ...sqliteSyncOps, ...databaseOps };
};

self.onmessage = async (e) => {
  const { type, data, id } = e.data;

  try {
    if (type === "init") {
      console.log("Initializing database on worker...");
      await initDB();
      self.postMessage({ type: "init", success: true, id });
      return;
    }

    if (!db) {
      throw new Error("Database not initialized");
    }

    if (!operations) {
      throw new Error("Operations not initialized");
    }

    const result = operations[type as keyof typeof operations](data);
    self.postMessage({ type, result, id });
  } catch (error) {
    console.error(`Operation ${type} failed:`, error);
    self.postMessage({ type, error: (error as Error).message, id });
  }
};
