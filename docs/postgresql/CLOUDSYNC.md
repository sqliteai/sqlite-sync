# Demo Deployment

For the current demo, a single CloudSync node is deployed in **Europe** on Fly.io.

If testing from other regions, latency will reflect this single-node deployment.  
A production deployment would use **geographically distributed nodes with regional routing** for global coverage.

## Fly.io

Project Name: **cloudsync-staging**  
Fly.io App: https://fly.io/apps/cloudsync-staging  
CloudSync Server URL: https://cloudsync-staging.fly.dev/  
Logs: https://fly.io/apps/cloudsync-staging/monitoring  

> Note: This is a **demo-only environment**, not intended for production use.

## Environment Variables

Edit in the Fly.io **Secrets** section:  
https://fly.io/apps/cloudsync-staging/secrets

After editing, the machine restarts automatically.

The server is currently configured to point to a demo Supabase project (https://supabase.com/dashboard/project/ajgnsrqbwmnhytqyesyr).

Environment variables:

- `CLOUDSYNC_JOBS_DATABASE_CONNECTION_STRING` — database for jobs table  
- `CLOUDSYNC_ARTIFACT_STORE_CONNECTION_STRING` — database for sync artifacts  
- `CLOUDSYNC_METRICS_DB_CONNECTION_STRING` — database for metrics  
- `CLOUDSYNC_SERVICE_PROJECT_ID` — project ID for service user  
- `CLOUDSYNC_SERVICE_DATABASE_CONNECTION_STRING` — service user DB connection  

## Tables

- **cloudsync_jobs** — queue of asynchronous jobs  
  - `check`: generate blob of client changes  
  - `apply`: apply client changes  
  - `notify`: send Expo push notifications  

- **cloudsync_artifacts** — blobs ready for client download  
- **cloudsync_metrics** — collected metrics  
- **cloudsync_push_tokens** — Expo push tokens

## Metrics

CloudSync integrates a simple metrics collector (authenticated via user JWT).

To visualize metrics, import `grafana-dashboard.json` into Grafana and configure your PostgreSQL database as a data source.

Alternatively, call the API directly:

```bash
curl --request GET   --url 'https://cloudsync-staging.fly.dev/v2/cloudsync/metrics?from=<ISO_START>&to=<ISO_END>&projectId=<PROJECT>&database=<DB>&siteId=<SITE>&action=check'   --header 'Authorization: Bearer <JWT>'
```

Filters:

- `from`
- `to`
- `projectId`
- `database`
- `siteId`
- `action` (`check` / `apply`)
