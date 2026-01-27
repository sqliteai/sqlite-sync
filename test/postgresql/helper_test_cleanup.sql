-- Test Cleanup Helper
-- This script should be included at the end of each test file
-- It determines if cleanup should happen based on DEBUG mode and test failures
-- Usage:
--   \ir helper_test_cleanup.sql
--   \if :should_cleanup
--   DROP DATABASE IF EXISTS your_test_db;
--   \endif

\connect postgres

-- Determine if we should cleanup
\if :{?DEBUG}
\set should_cleanup false
\echo [INFO] (:testid) DEBUG mode enabled - keeping test databases for inspection
\else
SELECT (:fail::int > :initial_fail::int) AS has_test_failures \gset
\if :has_test_failures
\set should_cleanup false
\echo [INFO] (:testid) Test failures detected - keeping test databases for inspection
\else
\set should_cleanup true
\endif
\endif
