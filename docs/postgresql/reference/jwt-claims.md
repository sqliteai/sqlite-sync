# JWT Claims Reference

## HS256 Claims

Use this mode when CloudSync validates JWTs with `jwtSecret`.

| Claim   | Required?  | Notes                                                                                             |
| ------- | ---------- | ------------------------------------------------------------------------------------------------- |
| `sub`   | ⚠️ Depends | Not required by CloudSync itself, but commonly used by application-specific RLS policies          |
| `email` | ❌ No      | Optional app-specific claim; not validated by CloudSync                                           |
| `role`  | ✅ Yes     | Required for PostgreSQL JWT-authenticated requests because CloudSync uses it for `SET LOCAL ROLE` |
| `iss`   | ❌ No      | Optional in HS256 mode                                                                            |
| `aud`   | ⚠️ Depends | Required only when `jwtExpectedAudiences` is configured                                           |
| `iat`   | ❌ No      | Optional issued-at timestamp; not validated by CloudSync                                          |
| `exp`   | ✅ Yes     | Required and validated by CloudSync                                                               |

## JWKS Claims

Use this mode when CloudSync validates JWTs with `jwtAllowedIssuers` and optional `jwksUri`.

| Claim    | Required?  | Notes                                                                                             |
| -------- | ---------- | ------------------------------------------------------------------------------------------------- |
| `sub`    | ⚠️ Depends | Not required by CloudSync itself, but commonly used by application-specific RLS policies          |
| `email`  | ❌ No      | Optional app-specific claim; not validated by CloudSync                                           |
| `role`   | ✅ Yes     | Required for PostgreSQL JWT-authenticated requests because CloudSync uses it for `SET LOCAL ROLE` |
| `iss`    | ✅ Yes     | Required for JWKS / issuer-based validation                                                       |
| `aud`    | ⚠️ Depends | Required only when `jwtExpectedAudiences` is configured                                           |
| `iat`    | ❌ No      | Optional issued-at timestamp; not validated by CloudSync                                          |
| `exp`    | ✅ Yes     | Required and validated by CloudSync                                                               |
| Header `kid` | ✅ Yes | Required in the JWT header so CloudSync can select the verification key from the JWKS             |

## Custom Claims Examples

| Claim           | Use Case                   |
| --------------- | -------------------------- |
| `org_id`        | Multi-tenant apps          |
| `team_id`       | Team-based access          |
| `permissions`   | Fine-grained access        |
| `scope`         | OAuth scopes               |
| `department_id` | Department-based filtering |
| `is_admin`      | Admin flag                 |

---

## How RLS Works with JWT Claims

**Flow:**

```
1. Client sends JWT token to CloudSync
2. CloudSync validates JWT and extracts claims
3. CloudSync passes claims to PostgreSQL as session variables
4. PostgreSQL policies can read session variables via current_setting()
5. Policies filter data based on claims
6. Only authorized rows returned to client
```

## PostgreSQL Role Requirement

For PostgreSQL JWT authentication, the `role` claim must name a real database role that CloudSync can switch into with `SET LOCAL ROLE`.

That role should:

- already exist in PostgreSQL
- have the schema, table, and sequence privileges your sync operations need (see [Required Grants](#required-grants))
- be grantable by the connection-string user

If the JWT contains a `role` that does not exist, or the connection user cannot switch into it, PostgreSQL sync operations will fail even if the JWT itself is otherwise valid.

### Creating the Role

A typical setup uses a `NOLOGIN` role that your connection user enters via `SET LOCAL ROLE` after JWT verification:

```sql
CREATE ROLE rls_role NOLOGIN;

-- Allow the connection-string user (e.g. `postgres`) to switch into it
GRANT rls_role TO postgres;
```

### Required Grants

`cloudsync_payload_apply` running as a non-superuser touches several internal CloudSync objects during apply — not just your user table. If any grant is missing on an internal object, the per-PK savepoint silently rolls back the write and the caller sees a non-zero column-change count with no rows landing (see [RLS Troubleshooting](./rls.md#apply-reports-a-count-but-rows-are-missing)).

There are two equivalent ways to configure this: the **recommended default-privileges pattern** (future-proof) or the **explicit minimum grant set** (tighter, for audited deployments).

#### Recommended: default-privileges pattern

Run this **before** `CREATE EXTENSION cloudsync`, as the role that will install the extension (typically `postgres`). Objects created afterwards — including all CloudSync internal tables and future `cloudsync_init` shadows — inherit the grants automatically:

```sql
GRANT USAGE ON SCHEMA public TO rls_role;
GRANT USAGE ON SCHEMA auth   TO rls_role;

ALTER DEFAULT PRIVILEGES IN SCHEMA public
    GRANT SELECT, INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER
    ON TABLES TO rls_role;

ALTER DEFAULT PRIVILEGES IN SCHEMA public
    GRANT USAGE, SELECT, UPDATE ON SEQUENCES TO rls_role;

ALTER DEFAULT PRIVILEGES IN SCHEMA public
    GRANT EXECUTE ON FUNCTIONS TO rls_role;

CREATE EXTENSION IF NOT EXISTS cloudsync;
```

**If the extension is already installed**, `ALTER DEFAULT PRIVILEGES` doesn't apply retroactively — backfill existing objects with a one-time broad grant, then still set defaults for future creations:

```sql
GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES    IN SCHEMA public TO rls_role;
GRANT USAGE, SELECT                  ON ALL SEQUENCES IN SCHEMA public TO rls_role;
-- (plus the ALTER DEFAULT PRIVILEGES block above)
```

#### Explicit minimum grant set

For audited deployments that need an explicit allowlist, the tightest set that allows `cloudsync_payload_apply` to work under a non-superuser:

```sql
GRANT USAGE ON SCHEMA public TO rls_role;
GRANT USAGE ON SCHEMA auth   TO rls_role;

-- User table (RLS policies filter rows within these grants)
GRANT SELECT, INSERT, UPDATE, DELETE ON your_table TO rls_role;

-- Per-table CRDT shadow (created by cloudsync_init)
GRANT SELECT, INSERT, UPDATE, DELETE ON your_table_cloudsync TO rls_role;

-- CloudSync metadata tables
GRANT SELECT, INSERT, UPDATE, DELETE ON
    cloudsync_settings,
    cloudsync_table_settings,
    cloudsync_site_id,
    cloudsync_schema_versions,
    cloudsync_payload_fragments,
    app_schema_version
TO rls_role;

-- cloudsync_changes view: SELECT for apply-path readback, INSERT for the
-- INSTEAD OF trigger that feeds column changes into the flush buffer
GRANT SELECT, INSERT ON cloudsync_changes TO rls_role;

-- BIGSERIAL-backed sequence on cloudsync_site_id.id (nextval needs USAGE)
GRANT USAGE ON SEQUENCE cloudsync_site_id_id_seq TO rls_role;

-- Your user table's sequence, if it uses SERIAL / IDENTITY
-- GRANT USAGE, SELECT ON SEQUENCE your_table_id_seq TO rls_role;
```

Notes on the minimum set:

- **No `EXECUTE` grants on `cloudsync_*` functions or `auth.uid()` are required**, because PostgreSQL defaults `CREATE FUNCTION` to `EXECUTE TO PUBLIC`. If your cluster has revoked PUBLIC execute, grant `EXECUTE` explicitly on `cloudsync_payload_apply`, `cloudsync_payload_encode`, `cloudsync_changes_select`, `cloudsync_changes_insert_trigger`, `cloudsync_siteid`, `cloudsync_pk_encode`, and `cloudsync_encode_value`.
- **`app_schema_version` is not `cloudsync_*`-prefixed** — easy to miss in `cloudsync_%`-pattern grants.
- **Per-table shadows follow the `<table>_cloudsync` convention** — repeat the DML grant for every table passed to `cloudsync_init`.
- **Administrative functions** such as `cloudsync_init`, `cloudsync_enable`, `cloudsync_set*`, `cloudsync_terminate`, `cloudsync_cleanup`, `cloudsync_begin_alter`, and `cloudsync_commit_alter` should be run by the database owner during setup, not by client JWT roles.
- The minimum set will need widening if a future CloudSync version adds new internal objects. The default-privileges pattern above is future-proof.

### Service Role (RLS Bypass)

For server-side workers that need to apply payloads without RLS enforcement (admin restores, cross-user sync, maintenance jobs), create a dedicated role with `BYPASSRLS`:

```sql
CREATE ROLE service_role NOLOGIN BYPASSRLS;
GRANT service_role TO postgres;
```

Apply the same grants as for `rls_role`. Use this role only from trusted server code, never from JWT-gated request paths.

---

## How CloudSync Passes JWT Claims to PostgreSQL

**For PostgreSQL JWT-authenticated requests, CloudSync validates the JWT and passes all claims to PostgreSQL as a session variable:**

```go
// CloudSync (internal implementation)
userData := token.Claims  // map[string]any with all JWT claims
claimJSON, _ := json.Marshal(userData)

// Pass all claims as JSON to PostgreSQL session
db.Exec(
  `SELECT set_config('request.jwt.claims', $1, true)`,
  string(claimJSON)
)
```

**Result:** All JWT claims are available in PostgreSQL as JSON in `request.jwt.claims`, and CloudSync also sets `SET LOCAL ROLE` from the JWT `role` claim.

**Example:** If JWT contains:

```json
{
  "sub": "550e8400-e29b-41d4-a716-446655440000",
  "email": "user@example.com",
  "role": "authenticated",
  "org_id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
}
```

Then in PostgreSQL:

```sql
-- Returns: {"sub":"550e8400...","email":"user@example.com","role":"authenticated","org_id":"aaaaaaaa..."}
current_setting('request.jwt.claims')

-- Access any claim from the JSON
user_id = (current_setting('request.jwt.claims')::jsonb->>'sub')::uuid
email = (current_setting('request.jwt.claims')::jsonb->>'email')
role = (current_setting('request.jwt.claims')::jsonb->>'role')
org_id = (current_setting('request.jwt.claims')::jsonb->>'org_id')::uuid
```

---

## Optional: Helper Functions for JWT Claims

CloudSync validates JWTs and passes all claims to PostgreSQL via `request.jwt.claims` — no PostgreSQL extension is required for JWT verification. The validation happens entirely in the CloudSync microservice.

However, writing `(current_setting('request.jwt.claims')::jsonb->>'sub')::uuid` in every RLS policy is verbose. Following the pattern used by Supabase and Neon, you can optionally create a small set of helper functions in a dedicated schema:

```sql
-- Create a schema for auth helpers (optional, but keeps things clean)
CREATE SCHEMA IF NOT EXISTS auth;

-- Returns all JWT claims as JSONB
CREATE OR REPLACE FUNCTION auth.session()
  RETURNS jsonb AS $$
    SELECT current_setting('request.jwt.claims', true)::jsonb;
$$ LANGUAGE SQL STABLE;

-- Returns the user ID (sub claim)
CREATE OR REPLACE FUNCTION auth.user_id()
  RETURNS text AS $$
    SELECT auth.session()->>'sub';
$$ LANGUAGE SQL STABLE;

-- Returns the user's role claim
CREATE OR REPLACE FUNCTION auth.role()
  RETURNS text AS $$
    SELECT auth.session()->>'role';
$$ LANGUAGE SQL STABLE;
```

> **Note:** These are just convenience wrappers — they read from the same `request.jwt.claims` session variable that CloudSync sets.

---
