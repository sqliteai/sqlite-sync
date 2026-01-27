-- Test Initialization Helper
-- This script should be included at the beginning of each test file after setting testid
-- Usage: \ir helper_test_init.sql

-- Initialize fail counter if not already set
\if :{?fail}
-- fail counter already exists from previous tests
\else
\set fail 0
\endif

-- Store initial fail count to detect failures in this test
SELECT :fail::int AS initial_fail \gset
