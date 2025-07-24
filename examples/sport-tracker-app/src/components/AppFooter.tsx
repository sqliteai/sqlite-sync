import React from "react";

interface AppFooterProps {
  sqliteSyncVersion: string;
  sqliteVersion: string;
}

const AppFooter: React.FC<AppFooterProps> = ({
  sqliteSyncVersion,
  sqliteVersion,
}) => {
  return (
    <footer className="app-footer">
      <div className="footer-content">
        <div className="footer-section">
          <div className="footer-brand">
            <span className="footer-icon">🚵</span>
            <span className="footer-title">Sport Tracker</span>
          </div>
          <div className="footer-tagline">Powered by SQLite AI</div>
        </div>

        <div className="footer-section">
          <div className="footer-versions">
            {sqliteSyncVersion && (
              <div className="version-item">
                <span className="version-label">SQLite Sync</span>
                <span className="version-value">v{sqliteSyncVersion}</span>
              </div>
            )}
            {sqliteVersion && (
              <div className="version-item">
                <span className="version-label">SQLite</span>
                <span className="version-value">v{sqliteVersion}</span>
              </div>
            )}
          </div>
        </div>

        <div className="footer-section">
          <div className="footer-links">
            <a
              href="https://sqlitecloud.io/ai"
              target="_blank"
              rel="noopener noreferrer"
            >
              SQLite AI
            </a>
            <span className="footer-separator">•</span>
            <a
              href="https://github.com/sqliteai/sqlite-sync"
              target="_blank"
              rel="noopener noreferrer"
            >
              GitHub
            </a>
          </div>
        </div>
      </div>
    </footer>
  );
};

export default AppFooter;
