import React from "react";

interface StatCardProps {
  title: string;
  value: string | number;
  className?: string;
}

const StatCard: React.FC<StatCardProps> = ({
  title,
  value,
  className = "",
}) => {
  return (
    <div className={`stat-card ${className}`}>
      <h3>{title}</h3>
      <p className="stat-value">{value}</p>
    </div>
  );
};

export default StatCard;
