-- Payload chunks and transparent large-value fragmentation

\set testid '52-chunks'
\ir helper_test_init.sql

\connect postgres
\ir helper_psql_conn_setup.sql
DROP DATABASE IF EXISTS cloudsync_test_52_chunks_src;
DROP DATABASE IF EXISTS cloudsync_test_52_chunks_dst;
DROP DATABASE IF EXISTS cloudsync_test_52_chunks_legacy;
CREATE DATABASE cloudsync_test_52_chunks_src;
CREATE DATABASE cloudsync_test_52_chunks_dst;
CREATE DATABASE cloudsync_test_52_chunks_legacy;

\connect cloudsync_test_52_chunks_src
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE payload_chunk_test (
  id TEXT PRIMARY KEY,
  note TEXT DEFAULT '',
  data BYTEA DEFAULT '\x'::bytea
);
SELECT cloudsync_init('payload_chunk_test', 'CLS', 1) AS _init_src \gset
SELECT cloudsync_set('payload_max_chunk_size', '1');

INSERT INTO payload_chunk_test(id, note, data)
SELECT
  'big',
  (SELECT string_agg(md5(i::text), '') FROM generate_series(1, 22500) AS g(i)),
  decode((SELECT string_agg(md5((i * 17)::text), '') FROM generate_series(1, 23000) AS g(i)), 'hex');

INSERT INTO payload_chunk_test(id, note, data)
VALUES
  ('same-a', 'same payload a', decode(repeat('ab', 360000), 'hex')),
  ('same-b', 'same payload b', decode(repeat('ab', 360000), 'hex'));

INSERT INTO payload_chunk_test(id, note, data)
SELECT
  format('row-%s', lpad(i::text, 3, '0')),
  format('small-%s-%s', i, repeat(md5(i::text), 24)),
  decode(repeat(md5((i * 31)::text), 16), 'hex')
FROM generate_series(1, 260) AS g(i);

SELECT
  count(*) AS chunk_count,
  count(*) FILTER (WHERE get_byte(payload, 4) = 3) AS v3_chunk_count,
  bool_and(octet_length(payload) <= 262144) AS chunks_within_limit,
  max(octet_length(payload)) AS max_chunk_len,
  sum(rows) AS chunk_rows
FROM cloudsync_payload_chunks() \gset

\if :chunks_within_limit
\echo [PASS] (:testid) Generated chunks respect the 256KB technical minimum - max: :max_chunk_len
\else
\echo [FAIL] (:testid) Generated chunk exceeds 256KB - max: :max_chunk_len
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT (:chunk_count::int >= 5 AND :v3_chunk_count::int >= 2) AS chunk_shape_ok \gset
\if :chunk_shape_ok
\echo [PASS] (:testid) Rowset and large-value fragmentation produced multiple chunks (:chunk_count total, :v3_chunk_count v3)
\else
\echo [FAIL] (:testid) Expected multiple chunks and v3 fragments, got :chunk_count total and :v3_chunk_count v3
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT count(*) AS explicit_arg_chunk_count
FROM cloudsync_payload_chunks(NULL, cloudsync_siteid(), NULL) \gset

SELECT (:explicit_arg_chunk_count::int = :chunk_count::int) AS explicit_args_ok \gset
\if :explicit_args_ok
\echo [PASS] (:testid) Optional cloudsync_payload_chunks arguments work
\else
\echo [FAIL] (:testid) Optional cloudsync_payload_chunks arguments changed result count
SELECT (:fail::int + 1) AS fail \gset
\endif

-- exclude_filter_site_id flag: every change here originates from the local
-- site, so excluding it must yield zero chunks while including it yields the
-- full set. This proves the predicate flips between "= site" and "<> site".
SELECT count(*) AS excl_local_chunks
FROM cloudsync_payload_chunks(0, cloudsync_siteid(), NULL, true) \gset

SELECT count(*) AS incl_local_chunks
FROM cloudsync_payload_chunks(0, cloudsync_siteid(), NULL, false) \gset

SELECT (:excl_local_chunks::int = 0 AND :incl_local_chunks::int > 0) AS exclude_flag_ok \gset
\if :exclude_flag_ok
\echo [PASS] (:testid) exclude_filter_site_id flips the site filter (exclude local -> 0, include -> :incl_local_chunks)
\else
\echo [FAIL] (:testid) exclude_filter_site_id did not flip the filter (exclude=:excl_local_chunks include=:incl_local_chunks)
SELECT (:fail::int + 1) AS fail \gset
\endif

-- exclude=true without a filter_site_id must raise an error
CREATE TEMP TABLE _excl_err(ok bool);
DO $$
BEGIN
  PERFORM 1 FROM cloudsync_payload_chunks(0, NULL, NULL, true);
  INSERT INTO _excl_err VALUES (false);
EXCEPTION WHEN OTHERS THEN
  INSERT INTO _excl_err VALUES (true);
END $$;
SELECT ok AS exclude_no_site_errors FROM _excl_err \gset
DROP TABLE _excl_err;
\if :exclude_no_site_errors
\echo [PASS] (:testid) exclude_filter_site_id without a site_id raises an error
\else
\echo [FAIL] (:testid) exclude_filter_site_id without a site_id did not error
SELECT (:fail::int + 1) AS fail \gset
\endif

-- UUID text<->blob roundtrip, including the /check string-argument path
SELECT (
  cloudsync_uuid_blob(cloudsync_uuid_text(cloudsync_siteid())) = cloudsync_siteid()
  AND cloudsync_uuid_blob(cloudsync_uuid_text(cloudsync_siteid(), false)) = cloudsync_siteid()
  AND cloudsync_uuid_blob(upper(cloudsync_uuid_text(cloudsync_siteid()))) = cloudsync_siteid()
  AND length(cloudsync_uuid_text(cloudsync_siteid())) = 36
  AND length(cloudsync_uuid_text(cloudsync_siteid(), false)) = 32
) AS uuid_conv_ok \gset
\if :uuid_conv_ok
\echo [PASS] (:testid) cloudsync_uuid_text/cloudsync_uuid_blob roundtrip (dashed, undashed, uppercase)
\else
\echo [FAIL] (:testid) UUID conversion roundtrip mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- A string-derived site_id (as the /check endpoint would pass it) selects the
-- same chunks as the binary site_id.
SELECT count(*) AS str_arg_chunks
FROM cloudsync_payload_chunks(0, cloudsync_uuid_blob(cloudsync_uuid_text(cloudsync_siteid())), NULL, false) \gset
SELECT (:str_arg_chunks::int = :incl_local_chunks::int) AS str_arg_ok \gset
\if :str_arg_ok
\echo [PASS] (:testid) String-derived site_id via cloudsync_uuid_blob matches the binary site_id
\else
\echo [FAIL] (:testid) String-derived site_id mismatch (:str_arg_chunks vs :incl_local_chunks)
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT
  md5(string_agg(id || ':' || note || ':' || encode(data, 'hex'), '|' ORDER BY id)) AS src_hash,
  count(*) AS src_count
FROM payload_chunk_test \gset

SELECT string_agg(encode(payload, 'hex'), ',' ORDER BY chunk_index) AS chunks_hex
FROM cloudsync_payload_chunks() \gset

SELECT
  encode(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq), 'hex') AS legacy_payload_hex,
  octet_length(cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq)) AS legacy_payload_len
FROM cloudsync_changes
WHERE site_id = cloudsync_siteid() \gset

SELECT (:legacy_payload_len::int > 262144) AS legacy_payload_large_ok \gset
\if :legacy_payload_large_ok
\echo [PASS] (:testid) Legacy monolithic payload is larger than local chunk setting (:legacy_payload_len bytes)
\else
\echo [FAIL] (:testid) Legacy monolithic payload was expected to exceed the chunk setting
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect cloudsync_test_52_chunks_dst
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE payload_chunk_test (
  id TEXT PRIMARY KEY,
  note TEXT DEFAULT '',
  data BYTEA DEFAULT '\x'::bytea
);
SELECT cloudsync_init('payload_chunk_test', 'CLS', 1) AS _init_dst \gset
SELECT cloudsync_set('payload_max_chunk_size', '262144');

CREATE TEMP TABLE chunk_transport(ord INT, payload BYTEA);
INSERT INTO chunk_transport(ord, payload)
SELECT ord::int, decode(chunk_hex, 'hex')
FROM unnest(string_to_array(:'chunks_hex', ',')) WITH ORDINALITY AS t(chunk_hex, ord);

-- Stale-fragment GC: on this fresh connection no fragment has been applied yet,
-- so the first applied v3 fragment triggers cleanup of the old incomplete group.
CREATE TABLE IF NOT EXISTS cloudsync_payload_fragments (
  value_id TEXT NOT NULL, part_index BIGINT NOT NULL, part_count BIGINT NOT NULL,
  total_size BIGINT NOT NULL, checksum TEXT NOT NULL,
  created_at BIGINT NOT NULL DEFAULT (EXTRACT(EPOCH FROM now())::bigint),
  tbl TEXT NOT NULL, pk BYTEA NOT NULL, col_name TEXT NOT NULL, col_version BIGINT NOT NULL,
  db_version BIGINT NOT NULL, site_id BYTEA NOT NULL, cl BIGINT NOT NULL, seq BIGINT NOT NULL,
  fragment BYTEA NOT NULL, PRIMARY KEY(value_id, part_index)
);
INSERT INTO cloudsync_payload_fragments
(value_id, part_index, part_count, total_size, checksum, created_at, tbl, pk, col_name, col_version, db_version, site_id, cl, seq, fragment)
VALUES ('stale-incomplete', 0, 2, 10, '0000000000000000', 0, 'payload_chunk_test', '\x01', 'data', 1, 1, decode(repeat('00', 16), 'hex'), 1, 1, '\x00');

SELECT cloudsync_payload_apply(payload) AS stale_cleanup_apply
FROM chunk_transport WHERE get_byte(payload, 4) = 3 ORDER BY ord LIMIT 1 \gset

SELECT (COUNT(*) = 0) AS stale_cleanup_ok
FROM cloudsync_payload_fragments WHERE value_id = 'stale-incomplete' \gset
\if :stale_cleanup_ok
\echo [PASS] (:testid) Stale incomplete fragment cleanup works (first apply on a fresh connection)
\else
\echo [FAIL] (:testid) Stale incomplete fragment cleanup failed
SELECT (:fail::int + 1) AS fail \gset
\endif

SELECT coalesce(sum(cloudsync_payload_apply(payload)), 0) AS chunk_apply_rows
FROM (SELECT payload FROM chunk_transport ORDER BY ord DESC) AS ordered_chunks \gset

SELECT
  md5(string_agg(id || ':' || note || ':' || encode(data, 'hex'), '|' ORDER BY id)) AS dst_hash,
  count(*) AS dst_count
FROM payload_chunk_test \gset

SELECT (:'dst_hash' = :'src_hash' AND :dst_count::int = :src_count::int) AS chunk_apply_ok \gset
\if :chunk_apply_ok
\echo [PASS] (:testid) Chunked payloads apply correctly, including reverse-order v3 fragments and identical large values
\else
\echo [FAIL] (:testid) Chunked payload apply mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

-- Throttle check: the full apply above already ran the stale GC, so an old
-- group inserted now must NOT be removed by the next immediate fragment apply.
-- This proves the GC is not re-scanned on every applied fragment (the O(n^2) fix).
INSERT INTO cloudsync_payload_fragments
(value_id, part_index, part_count, total_size, checksum, created_at, tbl, pk, col_name, col_version, db_version, site_id, cl, seq, fragment)
VALUES ('stale-incomplete-2', 0, 2, 10, '0000000000000000', 0, 'payload_chunk_test', '\x02', 'data', 1, 1, decode(repeat('00', 16), 'hex'), 1, 1, '\x00');

SELECT cloudsync_payload_apply(payload) AS throttle_apply
FROM chunk_transport WHERE get_byte(payload, 4) = 3 ORDER BY ord LIMIT 1 \gset

SELECT (COUNT(*) = 1) AS stale_throttle_ok
FROM cloudsync_payload_fragments WHERE value_id = 'stale-incomplete-2' \gset
\if :stale_throttle_ok
\echo [PASS] (:testid) Stale GC is throttled (not re-run on every applied fragment)
\else
\echo [FAIL] (:testid) Stale GC was not throttled (removed a fresh-inserted old group on the next apply)
SELECT (:fail::int + 1) AS fail \gset
\endif

\connect cloudsync_test_52_chunks_legacy
\ir helper_psql_conn_setup.sql
CREATE EXTENSION IF NOT EXISTS cloudsync;
CREATE TABLE payload_chunk_test (
  id TEXT PRIMARY KEY,
  note TEXT DEFAULT '',
  data BYTEA DEFAULT '\x'::bytea
);
SELECT cloudsync_init('payload_chunk_test', 'CLS', 1) AS _init_legacy \gset
SELECT cloudsync_set('payload_max_chunk_size', '262144');
SELECT cloudsync_payload_apply(decode(:'legacy_payload_hex', 'hex')) AS legacy_apply_rows \gset

SELECT
  md5(string_agg(id || ':' || note || ':' || encode(data, 'hex'), '|' ORDER BY id)) AS legacy_hash,
  count(*) AS legacy_count
FROM payload_chunk_test \gset

SELECT (:'legacy_hash' = :'src_hash' AND :legacy_count::int = :src_count::int) AS legacy_apply_ok \gset
\if :legacy_apply_ok
\echo [PASS] (:testid) Legacy monolithic payload applies even when larger than local chunk setting
\else
\echo [FAIL] (:testid) Legacy monolithic payload apply mismatch
SELECT (:fail::int + 1) AS fail \gset
\endif

\ir helper_test_cleanup.sql
\if :should_cleanup
DROP DATABASE IF EXISTS cloudsync_test_52_chunks_src;
DROP DATABASE IF EXISTS cloudsync_test_52_chunks_dst;
DROP DATABASE IF EXISTS cloudsync_test_52_chunks_legacy;
\endif
