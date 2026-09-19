# Testing against the real cloud

The repository already runs real-cloud tests in `test/integration.c`, through
`.github/workflows/main.yml`. These are separate from `network-unittest`, whose
fixtures and loopback server do not contact the cloud.

## Existing test tenants: the shortest path

The repository has all seven `INTEGRATION_TEST_*` secrets referenced below. GitHub
exposes their names, not their values; they do not need to be copied to a laptop.
Use the existing branch workflow after pushing the client change:

```sh
gh workflow run main.yml --repo sqliteai/sqlite-sync --ref pg-fixes11092026
gh run list --repo sqliteai/sqlite-sync --branch pg-fixes11092026 --limit 5
gh run watch RUN_ID --repo sqliteai/sqlite-sync --exit-status
```

A push already starts that workflow, so do not dispatch a second run for the same
commit unnecessarily. The workflow cancels older runs of the same branch. Avoid
running another branch or a local process against the shared chunked tenant at the
same time: the negative-cache test requires an idle, exclusive tenant, and the
workflow's concurrency group is per branch, not per tenant.

Inspect the **linux-x86_64 build + test** job. Only that matrix leg receives
`INTEGRATION_TEST_CHUNKED_DATABASE_ID`. A green job alone is insufficient: optional
cases can report `SKIPPED`. Require `OK` for:

- Init+Sync and Token Auth;
- Chunked Paths, Rowset, Single-Sync Drain, Capped Receive, Batched Receive,
  Failure and Negative Cache;
- Offline Error and Failure Path.

Evidence checked on 2026-09-19: [run 35424353890, Linux job 105847622981](https://github.com/sqliteai/sqlite-sync/actions/runs/35424353890/job/105847622981)
reported `OK` for all of those cases on client commit `1820a15`. That full workflow
failed in its Android x86_64 job; the Linux cloud cases did run and passed. This is
existing CI evidence, not a claim that the cleanup fix has already been deployed
and tested on the cloud server.

## What the tenant fixtures must contain

| Variable / secret | Required fixture |
| --- | --- |
| `INTEGRATION_TEST_CLOUDSYNC_ADDRESS` | Sync API base address accepted by `cloudsync_network_init_custom` |
| `INTEGRATION_TEST_APIKEY` | Credential authorized for the dedicated test tenants and gateway token minting |
| `INTEGRATION_TEST_WEBLITE_ADDRESS` | Gateway base address; the test calls `POST /v2/tokens` |
| `INTEGRATION_TEST_DATABASE_ID` | Normal sync fixture matching `db_init()` in `test/integration.c`: users, activities and workouts; initial activities present and workouts empty |
| `INTEGRATION_TEST_CHUNKED_DATABASE_ID` | Exclusive chunked fixture matching `test_chunked_schema_init()` and `test_chunked_failure_schema_init()`; server `payload_max_chunk_size=262144` |
| `INTEGRATION_TEST_OFFLINE_DATABASE_ID` | Paused database returning the expected `database_paused` / HTTP 503 error |
| `INTEGRATION_TEST_FAILURE_DATABASE_ID` | Reachable tenant whose backing node is deliberately not initialized for cloudsync, so asynchronous apply/check jobs report `cloudsync is not initialized` |

Use the established fixtures if possible. A newly created empty database is not a
substitute for all four tenants: the normal case asserts existing seed data, the
chunked tests need a server page limit, and the two negative fixtures intentionally
have different failure modes. The tests write/delete remote rows and mint an expiring
synthetic-user token; use test tenants, not an application database. Failed runs can
leave test rows behind. Retain fixture seeds when resetting a tenant.

For a local run, export the variables from a private environment/secret manager and
run `make e2e` from a disposable checkout after building the desired revision. Do not
commit a `.env` file, put credentials into shell history, or enable network tracing
on a shared log. The integration binary uses `./dist/cloudsync` and recreates
`health-track.sqlite` in its working directory. `make e2e` will load a local `.env` if
one exists. Review optional-test statuses; missing variables or a missing curl CLI
can skip coverage without failing the process.

## Testing the server-side cleanup fix

Building the SQLite client in CI does **not** deploy the PostgreSQL extension to
remote nodes. To cover this PR's cleanup regression against a real service:

1. Provision a separate staging node/tenant, install the fixed PostgreSQL extension,
   and verify `cloudsync_version()` plus the deployed build/commit identifier (the
   version alone cannot distinguish two 1.1.4 builds). Recycle backend sessions that
   had loaded the old shared library.
2. Run the real-cloud integration suite against correctly provisioned staging fixtures.
   Check row contents and final receive completion, not only HTTP success.
3. On the staging node only, use the setup and assertions from
   `test/postgresql/61_fragment_cleanup_backlog.sql`: 30,000 synthetic stale groups,
   one group held by another transaction, and a genuine two-piece payload. Do not run
   the whole SQL file on a shared node: its harness creates/drops a test database and
   assumes local administrative access. Adapt its setup to the isolated tenant.
4. Trigger a real fragment upload through the gateway, then verify bounded cleanup
   progress, preserved in-flight pieces and successful reconstruction. To observe a
   subsequent maintenance pass on a persistent worker, allow the 60-second throttle
   to expire (or use a new backend). Confirm progress again after releasing the lock.
5. Correlate client output with gateway/worker logs: no `out of shared memory`, no lost
   fragment and no receive checkpoint beyond incomplete data. Record client, server,
   gateway versions, tenant configuration and whether every optional case ran.

Without staging-node deployment/admin access we can exercise the real network and
existing server behavior, but cannot honestly certify this PostgreSQL fix on the
remote server. Local PostgreSQL regression/concurrency tests remain the deterministic
check for the fix until that deployment is available.
