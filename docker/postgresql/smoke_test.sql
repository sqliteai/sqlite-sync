\set ON_ERROR_STOP on

-- Reset extension and install
DROP EXTENSION IF EXISTS cloudsync CASCADE;
CREATE EXTENSION cloudsync;

-- Basic visibility checks
SELECT cloudsync_version() AS version;

SELECT (length(cloudsync_uuid()) > 0) AS uuid_ok \gset
\if :uuid_ok
\else
  \quit 1
\endif

-- SELECT (cloudsync_db_version() >= 0) AS dbv_ok \gset
-- \if :dbv_ok
-- \else
--   \quit 1
-- \endif

-- Enable debug logs
SET client_min_messages = debug1; SET log_min_messages = debug1;

-- Enable debug logs
-- SET client_min_messages = debug1; SET log_min_messages = debug1;

-- Init on a simple table should succeed
SELECT cloudsync_cleanup('smoke_tbl');
DROP TABLE IF EXISTS smoke_tbl;
CREATE TABLE smoke_tbl (id TEXT PRIMARY KEY, val TEXT);
SELECT cloudsync_init('smoke_tbl', 'CLS', true);

SELECT cloudsync_siteid();

SELECT (length(encode(cloudsync_siteid()::bytea, 'hex')) > 0) AS sid_ok \gset
\if :sid_ok
\else
  \quit 1
\endif