import React from "react";
import type { Workout } from "../../db/database";

interface WorkoutCardProps {
  workout: Workout;
  onComplete: (id: string) => void;
  onDelete?: (id: string) => void;
  canDelete?: boolean;
  userName?: string;
}

const WorkoutCard: React.FC<WorkoutCardProps> = ({
  workout,
  onComplete,
  onDelete,
  canDelete = false,
  userName,
}) => {
  return (
    <div className={`workout-card ${workout.completed ? "completed" : ""}`}>
      <div className="workout-header">
        <h4>{workout.name}</h4>
        <div className="workout-header-right">
          <span className="workout-type">{workout.type}</span>
          {canDelete && onDelete && workout.id && (
            <button
              className="btn-delete"
              onClick={() => onDelete(workout.id!)}
              title="Delete workout"
            >
              ×
            </button>
          )}
        </div>
      </div>
      {userName && <div className="workout-user">👤 {userName}</div>}
      <div className="workout-details">
        <span className="duration">{workout.duration} min</span>
        <span className="workout-date">
          {new Date(workout.date).toLocaleDateString()}
        </span>
      </div>
      {!workout.completed ? (
        <button
          className="btn-complete"
          onClick={() => workout.id && onComplete(workout.id)}
        >
          Complete
        </button>
      ) : (
        <span className="completed-badge">✓ Completed</span>
      )}
    </div>
  );
};

export default WorkoutCard;
