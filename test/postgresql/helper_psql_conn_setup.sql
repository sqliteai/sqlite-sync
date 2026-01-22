\if :{?DEBUG}
SET client_min_messages = debug1; SET log_min_messages = debug1; SET log_error_verbosity = verbose;
\set QUIET 0
\pset tuples_only off
\pset format aligned
\else
SET client_min_messages = warning; SET log_min_messages = warning;
\set QUIET 1
\pset tuples_only on
\pset format unaligned
\endif
