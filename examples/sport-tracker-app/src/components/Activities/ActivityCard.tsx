import React from "react";
import type { Activity } from "../../db/database";

interface ActivityCardProps {
  activity: Activity;
  onDelete?: (id: string) => void;
  canDelete?: boolean;
  userName?: string;
}

const ActivityCard: React.FC<ActivityCardProps> = ({
  activity,
  onDelete,
  canDelete = false,
  userName,
}) => {
  return (
    <div className="activity-card">
      <div className="activity-header">
        <h4>
          {activity.type.charAt(0).toUpperCase() + activity.type.slice(1)}
        </h4>
        <div className="activity-header-right">
          <span className="activity-date">
            {new Date(activity.date).toLocaleDateString()}
          </span>
          {canDelete && onDelete && activity.id && (
            <button
              className="btn-delete"
              onClick={() => onDelete(activity.id!)}
              title="Delete activity"
            >
              ×
            </button>
          )}
        </div>
      </div>
      {userName && <div className="activity-user">👤 {userName}</div>}
      <div className="activity-details">
        <span className="duration">{activity.duration} min</span>
        {activity.distance && (
          <span className="distance">{activity.distance} km</span>
        )}
        {activity.calories && (
          <span className="calories">{activity.calories} cal</span>
        )}
      </div>
      {activity.notes && <p className="activity-notes">{activity.notes}</p>}
    </div>
  );
};

export default ActivityCard;
