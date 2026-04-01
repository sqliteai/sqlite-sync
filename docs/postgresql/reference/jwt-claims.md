# JWT Claims Reference

## How RLS Works with JWT Claims

**Flow:**
```
1. Client sends JWT token to CloudSync
2. CloudSync validates JWT and extracts claims
3. CloudSync passes claims to PostgreSQL as session variables
4. RLS policies read session variables via current_setting()
5. Policies filter data based on claims
6. Only authorized rows returned to client
```

## How CloudSync Passes JWT Claims

**CloudSync passes ALL JWT claims as a single JSON object** in `request.jwt.claims`:

```sql
-- Access any claim from the JSON
user_id = (current_setting('request.jwt.claims')::jsonb->>'sub')::uuid
email = (current_setting('request.jwt.claims')::jsonb->>'email')
role = (current_setting('request.jwt.claims')::jsonb->>'role')
org_id = (current_setting('request.jwt.claims')::jsonb->>'org_id')::uuid
```

---

## Standard JWT Claims

| Claim | Needed? | Purpose |
|-------|---------|---------|
| `sub` | ✅ Yes | User ID |
| `email` | ✅ Yes | User email |
| `role` | ✅ Yes | Permission level |
| `iss` | ✅ Yes | Issuer (validated in app) |
| `aud` | ✅ Yes | Audience (validated in app) |
| `iat` | ⚠️ Maybe | Issued at timestamp |
| `exp` | ✅ Yes | Expiration (validated in app) |

## Custom Claims (Application-Specific)

| Claim | Use Case |
|-------|----------|
| `org_id` | Multi-tenant apps |
| `team_id` | Team-based access |
| `permissions` | Fine-grained access |
| `scope` | OAuth scopes |
| `department_id` | Department-based filtering |
| `is_admin` | Admin flag |

---

## Recommended JWT Structure for CloudSync

```javascript
const token = jwt.sign({
  // Required
  sub: user.id,                    // UUID: user_id
  email: user.email,               // String: for contact/audit
  role: user.role,                 // String: admin/user/viewer

  // Multi-tenant (if needed)
  org_id: user.org_id,             // UUID: organization
  team_id: user.team_id,           // UUID: team (optional)

  // Permissions (choose one approach)
  // Approach 1: Simple role
  // role: "admin" (handled above)

  // Approach 2: Detailed permissions
  permissions: [                   // Array: fine-grained
    "todos:read",
    "todos:write",
    "todos:delete"
  ],

  // Standard claims (handled by JWT lib)
  // iss: 'cloudsync-auth',
  // aud: 'authenticated',
  // iat: Math.floor(Date.now() / 1000),
  // exp: Math.floor(Date.now() / 1000) + (24 * 60 * 60)
}, privateKey, {
  algorithm: 'RS256',
  expiresIn: '24h'
});
```

---

## How CloudSync Passes JWT Claims to PostgreSQL

**CloudSync validates the JWT and converts all claims to JSON, then passes as a PostgreSQL session variable:**

```go
// CloudSync (internal implementation)
userData := token.Claims  // map[string]any with all JWT claims
claimJSON, _ := json.Marshal(userData)

// Pass all claims as JSON to PostgreSQL session
db.Exec(
  `SELECT set_config('role', 'authenticated', true),
          set_config('request.jwt.claims', $1, true)`,
  string(claimJSON)
)
```

**Result:** All JWT claims available in PostgreSQL as JSON in `request.jwt.claims`

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
current_setting('request.jwt.claims')
-- Returns: {"sub":"550e8400...","email":"user@example.com","role":"authenticated","org_id":"aaaaaaaa..."}
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

## Security Rules

### Rule 1: Use Immutable Claims for RLS
```javascript
// ✅ Good: System-set, immutable
role: user.role,

// ❌ Bad: User can modify
user_metadata: { role: "admin" }
```

### Rule 2: Don't Duplicate Database Lookups
```sql
-- ✅ Good: Trust the JWT claim
WHERE user_id = current_setting('request.user_id')::uuid

-- ❌ Bad: Defeats the purpose of RLS
WHERE user_id = current_setting('request.user_id')::uuid
  AND user_id IN (SELECT id FROM users WHERE active = true)
-- If RLS is on users table, this becomes circular
```

### Rule 3: Validate Claims in App Before Passing to DB
```go
// ✅ Good: Validate first
if !isValidRole(claims.Role) {
  return Unauthorized("Invalid role")
}
db.Exec("SET request.user_role = $1", claims.Role)

// ❌ Bad: Trust user input
db.Exec("SET request.user_role = $1", claims.Role)
// What if claims.Role is "superadmin"?
```

### Rule 4: Keep Session Variables Consistent
```go
// ✅ Good: Set all needed claims
db.Exec("SET request.user_id = $1", claims.Sub)
db.Exec("SET request.org_id = $1", claims.OrgID)

// ❌ Bad: Partial claims, RLS breaks
db.Exec("SET request.user_id = $1", claims.Sub)
// What if RLS policy expects org_id?
```

---

## Example: Complete Flow

**1. Auth server creates JWT:**
```javascript
const token = jwt.sign({
  sub: '550e8400-e29b-41d4-a716-446655440000',
  email: 'user@example.com',
  role: 'user',
  org_id: 'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee',
  permissions: ['todos:read', 'todos:write']
}, privateKey, { algorithm: 'RS256', expiresIn: '24h' });
```

**2. CloudSync passes JWT claims to PostgreSQL:**
```go
claims := jwt.Parse(token)
userData := map[string]any{
  "sub": claims.Sub,
  "email": claims.Email,
  "role": claims.Role,
  "org_id": claims.OrgID,
}
claimJSON, _ := json.Marshal(userData)

db.Exec(
  `SELECT set_config('role', 'authenticated', true),
          set_config('request.jwt.claims', $1, true)`,
  string(claimJSON)
)
```

**3. PostgreSQL RLS policies read from JWT claims:**
```sql
CREATE POLICY "org_isolation"
  ON todos FOR ALL
  USING (org_id = (current_setting('request.jwt.claims')::jsonb->>'org_id')::uuid);

CREATE POLICY "user_ownership"
  ON todos FOR UPDATE
  USING (user_id = (current_setting('request.jwt.claims')::jsonb->>'sub')::uuid);
```

**4. CloudSync executes queries (RLS filters automatically):**
```go
rows := db.Query(ctx, "SELECT * FROM todos")
// RLS automatically returns only matching rows
```

## Common RLS Patterns

### Pattern 1: Filter by User ID (Most Common)
**Best for:** Todo apps, note-taking, personal data

```sql
-- Table with user_id column
CREATE TABLE todos (
  id UUID PRIMARY KEY,
  user_id UUID NOT NULL,
  title TEXT,
  created_at TIMESTAMP
);

-- Enable RLS
ALTER TABLE todos ENABLE ROW LEVEL SECURITY;

-- Read user_id from JWT claims (sub claim)
CREATE POLICY "users_see_own_todos"
  ON todos FOR ALL
  USING (user_id = (current_setting('request.jwt.claims')::jsonb->>'sub')::uuid);
```

**CloudSync passes:** JWT claims as JSON in `request.jwt.claims`
**RLS reads:** `current_setting('request.jwt.claims')::jsonb->>'sub'`

### Pattern 2: Filter by Organization ID (Multi-tenant)
**Best for:** SaaS apps, team collaboration

```sql
CREATE TABLE projects (
  id UUID PRIMARY KEY,
  org_id UUID NOT NULL,
  name TEXT
);

ALTER TABLE projects ENABLE ROW LEVEL SECURITY;

CREATE POLICY "orgs_see_own_projects"
  ON projects FOR ALL
  USING (org_id = (current_setting('request.jwt.claims')::jsonb->>'org_id')::uuid);
```

### Pattern 3: Filter by Role (Admin vs User)
**Best for:** Different access levels

```sql
CREATE TABLE users (
  id UUID PRIMARY KEY,
  email TEXT,
  role TEXT
);

ALTER TABLE users ENABLE ROW LEVEL SECURITY;

CREATE POLICY "role_based_access"
  ON users FOR SELECT
  USING (
    (current_setting('request.jwt.claims')::jsonb->>'role') = 'admin'
    OR id = (current_setting('request.jwt.claims')::jsonb->>'sub')::uuid
  );
```

### Pattern 4: Combine User ID + Organization
**Best for:** Team apps with shared data

```sql
CREATE TABLE team_members (
  id UUID PRIMARY KEY,
  org_id UUID NOT NULL,
  user_id UUID NOT NULL,
  role TEXT
);

ALTER TABLE team_members ENABLE ROW LEVEL SECURITY;

CREATE POLICY "see_org_members"
  ON team_members FOR ALL
  USING (
    org_id = (current_setting('request.jwt.claims')::jsonb->>'org_id')::uuid
    AND user_id = (current_setting('request.jwt.claims')::jsonb->>'sub')::uuid
  );
```

---
