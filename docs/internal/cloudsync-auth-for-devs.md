# CloudSync Authentication for Developers

## Direct Database Connections from Mobile

**NEVER have mobile/web clients connect directly to PostgreSQL, even with pgjwt.**

```
❌ DANGEROUS (Don't do this)
Mobile App → PostgreSQL (direct connection)
  - Exposes database to internet
  - Database credentials in app (can be decompiled)
  - Any client can execute SQL
  - No way to audit/control access
  - Doesn't scale

✅ CORRECT (Do this)
Mobile App → Auth Server → PostgreSQL
  - App only talks to lightweight auth service
  - Auth server controls database access
  - Database hidden behind application layer
  - Credentials secure
  - Scalable and auditable
```

**Even for quick testing:** Provide a real-world example (auth server), not a shortcut that teaches bad practices. Quick start should resemble production architecture.

---

## Understanding JWT Authentication Patterns

PostgreSQL projects use these main patterns:

### Core Patterns

1. **Database-native** — Validate tokens inside PostgreSQL
   - Best for: PostgREST, API-first architectures
   - Limitation: Requires JWKS endpoint, less flexibility
   - Database handles auth logic

2. **Application-layer** ← **What CloudSync implements**
   - Validate tokens in the microservice
   - Pass user context to PostgreSQL (as JSON claims)
   - Full control, scalable, industry standard
   - Separates auth from database

3. **Hybrid** — Both API and DB validate
   - Application validates JWT signature
   - Database enforces RLS policies using claims
   - Defense-in-depth approach
   - Recommended for sensitive data

### Common Variations

4. **API Gateway Pattern** — Validation at gateway layer
   - Gateway validates JWT, extracts claims
   - Passes claims to backend microservices as headers/context
   - Still application-layer, just centralized

5. **Token Introspection Pattern** — Dynamic validation
   - Instead of signature validation, call auth service: "is this token valid?"
   - Better for token revocation, shorter-lived tokens
   - Adds latency (requires extra call per request)

6. **Service-to-Service JWT** — Internal microservice authentication
   - Services authenticate to each other with JWTs
   - Different from client authentication
   - Uses same JWT mechanisms

**CloudSync uses pattern 2 (application-layer)** with pattern 3 (RLS).

---

## PostgreSQL JWT Extensions: Why We Don't Recommend Them

**pg_session_jwt** (Neon Extension)
- ✅ Validates JWTs at database layer with RLS integration
- ✅ Good for: PostgREST, Postgres-centric apps
- ❌ For CloudSync: Redundant (microservice already validates)
- ❌ Adds DB-layer complexity without benefit
- ❌ Not portable (Neon-specific initially)

**pgjwt** (GitHub Extension)
- ✅ Generates + verifies JWTs in SQL
- ❌ HS256-only (no key rotation, no RS256)
- ❌ **CRITICAL:** Clients cannot connect directly to PostgreSQL
- ❌ **DEPRECATED:** Removed in PostgreSQL 17+ (not forward-compatible)
- Only use for: Learning (local development only, PostgreSQL 15 or earlier)

**Custom extensions you might find:**
- Various community JWT extensions exist but have limited adoption
- Most have the same issues: HS256-only, adds DB load, not portable
- Evaluate against: production-readiness, portability, flexibility, maintenance

**Verdict:** Extensions add complexity without benefits for CloudSync. Application-layer validation (auth server) is:
- ✅ Simpler (use standard JWT libraries)
- ✅ More flexible (HS256, RS256, ES256, etc.)
- ✅ More secure (database stays hidden)
- ✅ Better separation of concerns
- ✅ Scales independently
- ✅ **Industry standard** (PostgREST uses this pattern with Auth0, custom auth servers, etc.)

**PostgREST (industry benchmark):**
PostgREST doesn't generate JWTs. Instead, it validates JWTs from **external auth services** (Auth0, custom servers) and uses the JWT's `role` claim to control database access.

---

## Recommended Authentication Methods for CloudSync

### 1. Auth Server + HS256 (Development)
**Best for:** Development and testing

```javascript
// auth-server/server.js
app.post('/token', (req, res) => {
  const token = jwt.sign(
    { sub: 'user-123', role: 'authenticated' },
    process.env.JWT_SECRET,
    { expiresIn: '24h', algorithm: 'HS256' }
  );
  res.json({ token });
});
```

**Pros:** Simple, separate from database, real-world pattern

**Cons:** Can't rotate keys without redeployment, secret must sync across services

### 2. Auth Server + RS256 (Production)
**Best for:** Production deployments

```bash
# Generate RSA keys once, persist to volume
openssl genrsa -out private.pem 2048
openssl rsa -in private.pem -pubout -out public.pem

# Auth server signs with private.pem
# CloudSync fetches public key via JWKS endpoint
# Keys can be rotated without redeployment
```

**Pros:** Industry standard, asymmetric, key rotation, production-ready

**Cons:** More setup complexity

---

## Learning: pgjwt (Backend Developers Only)

**Not recommended for CloudSync testing**, but useful for understanding token generation:

```sql
CREATE EXTENSION IF NOT EXISTS pgjwt;

-- Backend developer tests token generation
SELECT sign(
  json_build_object('sub', 'user-123', 'role', 'authenticated'),
  'secret-key',
  'HS256'
) AS token;

-- Test RLS with generated token
SET request.jwt.claims = '{"sub":"user-123"}';
SELECT * FROM todos;  -- Verify RLS filters correctly
```

**When to use:** Backend developers learning RLS, testing SQL token logic

**When NOT to use:** Testing CloudSync (still requires auth server for clients)

**Why not for CloudSync:** Even though pgjwt generates tokens, you still need an auth server because clients can't connect to PostgreSQL. Use a simple auth server instead — it's simpler, real-world, and PostgreSQL 17+ compatible.

---

## Client Apps (Android/Web/SQLite)

### How to get a JWT token

**Flow for all client types:**
```
Client app
    ↓ (HTTP POST /auth/login)
Auth Server (generates JWT)
    ↓ (returns JWT)
Client app (stores & uses JWT)
    ↓ (HTTP request with Authorization: Bearer <token> through cloudsync extension)
CloudSync Microservice
```
