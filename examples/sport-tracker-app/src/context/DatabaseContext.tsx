import React, { createContext, useContext, useEffect, useState } from "react";
import { Database } from "../db/database";

interface DatabaseContextType {
  db: Database | null;
  isInitialized: boolean;
  error: string | null;
}

const DatabaseContext = createContext<DatabaseContextType | undefined>(
  undefined
);

export const DatabaseProvider: React.FC<{ children: React.ReactNode }> = ({
  children,
}) => {
  const [db, setDb] = useState<Database | null>(null);
  const [isInitialized, setIsInitialized] = useState(false);
  const [error, setError] = useState<string | null>(null);

  /** Database Initialization */
  useEffect(() => {
    const initializeDatabase = () => {
      const database = new Database();
      database
        .init()
        .then(() => {
          console.log("Database initialized successfully");
          setDb(database);
          setIsInitialized(true);
        })
        .catch((err) => {
          console.error("Database initialization failed:", err);
          setError(
            err instanceof Error ? err.message : "Failed to initialize database"
          );
        });
    };

    initializeDatabase();
  }, []);

  return (
    <DatabaseContext.Provider value={{ db, isInitialized, error }}>
      {children}
    </DatabaseContext.Provider>
  );
};

export const useDatabase = (): DatabaseContextType => {
  const context = useContext(DatabaseContext);
  if (context === undefined) {
    throw new Error("useDatabase must be used within a DatabaseProvider");
  }
  return context;
};
