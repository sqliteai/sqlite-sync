import React, { useEffect, useState } from "react";
import type { DatabaseCounts } from "../db/database";

interface DatabaseStatusProps {
  onCountsLoad: () => Promise<DatabaseCounts>;
  refreshTrigger?: number; // Optional prop to trigger refresh
}

const DatabaseStatus: React.FC<DatabaseStatusProps> = ({
  onCountsLoad,
  refreshTrigger,
}) => {
  const [counts, setCounts] = useState<DatabaseCounts>({
    users: 0,
    activities: 0,
    workouts: 0,
    totalUsers: 0,
    totalActivities: 0,
    totalWorkouts: 0,
  });
  const [loading, setLoading] = useState(true);

  const loadCounts = async () => {
    try {
      setLoading(true);
      const newCounts = await onCountsLoad();
      setCounts(newCounts);
    } catch (error) {
      console.error("Failed to load database counts:", error);
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    loadCounts();
  }, [refreshTrigger]);

  if (loading) {
    return <div className="database-status">On local Database: Loading...</div>;
  }

  const hasWarnings =
    counts.totalUsers > counts.users ||
    counts.totalActivities > counts.activities ||
    counts.totalWorkouts > counts.workouts;

  return (
    <div className={`database-status ${hasWarnings ? "warning" : ""}`}>
      On local Database:&nbsp;
      {hasWarnings && <span> ⚠️ </span>}
      Users: {counts.users}
      {counts.totalUsers > counts.users && (
        <span className="warning-text"> (Total: {counts.totalUsers})</span>
      )}{" "}
      | Activities: {counts.activities}
      {counts.totalActivities > counts.activities && (
        <span className="warning-text"> (Total: {counts.totalActivities})</span>
      )}{" "}
      | Workouts: {counts.workouts}
      {counts.totalWorkouts > counts.workouts && (
        <span className="warning-text"> (Total: {counts.totalWorkouts})</span>
      )}
    </div>
  );
};

export default DatabaseStatus;
