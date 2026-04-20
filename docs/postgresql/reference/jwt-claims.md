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
- have the schema, table, and sequence privileges your sync operations need
- have access to the `cloudsync_changes` view used by PostgreSQL sync operations
- be grantable by the connection-string user

If the JWT contains a `role` that does not exist, or the connection user cannot switch into it, PostgreSQL sync operations will fail even if the JWT itself is otherwise valid.

### Minimum Grants for a JWT Role

In a standard PostgreSQL setup, functions created by `CREATE EXTENSION cloudsync;` are executable by `PUBLIC` unless your cluster has been hardened with explicit `REVOKE EXECUTE` statements. In the normal case, the JWT role needs grants on:

- the schema that contains your synced tables
- the `cloudsync_changes` view
- the synced user tables
- any sequences used by those tables

Example:

```sql
GRANT USAGE ON SCHEMA public TO rls_role;

GRANT SELECT, INSERT ON cloudsync_changes TO rls_role;

GRANT SELECT, INSERT, UPDATE, DELETE ON TABLE your_table TO rls_role;

GRANT USAGE, SELECT ON SEQUENCE your_table_id_seq TO rls_role;
```

Administrative functions such as `cloudsync_init`, `cloudsync_enable`, `cloudsync_set*`, `cloudsync_terminate`, `cloudsync_cleanup`, `cloudsync_begin_alter`, and `cloudsync_commit_alter` should be run by the database owner during setup, not by client JWT roles.

If your PostgreSQL setup has revoked the default `PUBLIC` execute privileges on functions, you must also explicitly grant execute permissions on the specific CloudSync functions needed by your sync path.

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
