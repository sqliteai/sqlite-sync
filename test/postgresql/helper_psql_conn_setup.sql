\if :{?DEBUG}
\set QUIET 0
SET client_min_messages = debug1; SET log_min_messages = debug1; SET log_error_verbosity = verbose;
\pset tuples_only off
\pset format aligned
\else
\set QUIET 1
SET client_min_messages = warning; SET log_min_messages = warning;
\pset tuples_only on
\pset format unaligned
\endif
