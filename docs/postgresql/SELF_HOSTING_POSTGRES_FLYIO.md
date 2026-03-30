# Self-Hosting PostgreSQL on Fly.io with CloudSync Extension

This guide deploys a standalone PostgreSQL instance with CloudSync on Fly.io, plus a minimal JWT auth server for token generation. No Supabase required.

By the end you will have:

- A Fly.io VM running PostgreSQL with the CloudSync CRDT extension
- A JWT auth server (Node.js) — choose between:
  - **HS256 (shared secret)** — simplest setup, signs tokens with a base64-encoded secret
  - **RS256 (JWKS)** — production-ready, signs with a private key and exposes a public JWKS endpoint
- A custom Postgres image published to Docker Hub

## Prerequisites

| Tool | Purpose | Install |
|------|---------|---------|
| [Docker Desktop](https://www.docker.com/products/docker-desktop/) | Build the custom Postgres image | `brew install --cask docker` |
| [Fly CLI (`flyctl`)](https://fly.io/docs/flyctl/install/) | Provision and manage Fly.io machines | `brew install flyctl` |
| [Git](https://git-scm.com/) | Clone repositories | `brew install git` |
| [Docker Hub](https://hub.docker.com/) account | Host your custom Postgres image | Free signup |

You also need a [Fly.io account](https://fly.io/app/sign-up).

### Fly.io VM requirements

Since this is just PostgreSQL + a small auth server (not a full Supabase stack), resource requirements are much lower:

| Resource | Minimum | Recommended |
|----------|---------|-------------|
| RAM | 1 GB | 2 GB |
| CPU | 1 core | 2 cores |
| Disk | 4 GB SSD | 10 GB+ |

---

## Step 1: Initialize git submodules

```bash
cd /path/to/sqlite-sync-dev
git submodule update --init --recursive
```

Without this, the build fails with `fractional_indexing.h: No such file or directory`.

---

## Step 2: Build the custom Postgres image

From the sqlite-sync-dev repo root:

```bash
make postgres-docker-build
```

This builds `postgres:17` with CloudSync pre-installed using `docker/postgresql/Dockerfile`.

Verify:

```bash
docker images | grep sqlite-sync-pg
```

---

## Step 3: Build for amd64 and push to Docker Hub

If you're on Apple Silicon, you must cross-build for Fly.io's x86 VMs:

```bash
docker build --platform linux/amd64 \
  -f docker/postgresql/Dockerfile \
  -t <your-dockerhub-username>/postgres-cloudsync:17 \
  .

docker push <your-dockerhub-username>/postgres-cloudsync:17
```

> On Intel Mac or Linux x86, the default build is already amd64.

---

## Step 4: Provision a Fly.io VM

### 4a. Log in and create the app

```bash
fly auth login
fly apps create <your-app-name>
```

### 4b. Create a persistent volume

```bash
fly volumes create pg_data --app <your-app-name> --region <region> --size 4
```

### 4c. Create a Fly Machine

```bash
fly machine run ubuntu:24.04 \
  --app <your-app-name> \
  --region <region> \
  --vm-size shared-cpu-2x \
  --vm-memory 2048 \
  --volume pg_data:/data \
  --name postgres-vm \
  -- sleep inf
```

### 4d. Allocate a public IP

```bash
fly ips allocate-v4 --shared --app <your-app-name>
fly ips allocate-v6 --app <your-app-name>
```

---

## Step 5: Set up Docker on the VM

### 5a. SSH into the machine

```bash
fly ssh console --app <your-app-name>
```

### 5b. Install Docker Engine

```bash
apt-get update
apt-get install -y ca-certificates curl gnupg

install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | gpg --dearmor -o /etc/apt/keyrings/docker.gpg
chmod a+r /etc/apt/keyrings/docker.gpg

echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
  https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo $VERSION_CODENAME) stable" \
  > /etc/apt/sources.list.d/docker.list

apt-get update
apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin
```

### 5c. Configure Docker storage driver

```bash
apt-get install -y fuse-overlayfs
mkdir -p /etc/docker
echo '{"storage-driver":"fuse-overlayfs","data-root":"/data/docker"}' > /etc/docker/daemon.json
```

### 5d. Start Docker

```bash
dockerd &
# Wait for "API listen on /var/run/docker.sock"
```

---

## Step 6: Create the Docker Compose stack

Create the project directory:

```bash
mkdir -p /data/cloudsync-postgres
cd /data/cloudsync-postgres
```

### 6a. Generate a JWT secret

```bash
JWT_SECRET=$(openssl rand -base64 32)
echo "JWT_SECRET=$JWT_SECRET" > .env
echo "POSTGRES_PASSWORD=$(openssl rand -base64 16)" >> .env
echo "Your JWT secret: $JWT_SECRET"
echo "Save this secret — you'll need it for CloudSync server configuration."
```

### 6b. Create docker-compose.yml

```bash
cat > docker-compose.yml << 'EOF'
services:
  db:
    image: <your-dockerhub-username>/postgres-cloudsync:17
    container_name: cloudsync-postgres
    environment:
      POSTGRES_USER: postgres
      POSTGRES_PASSWORD: ${POSTGRES_PASSWORD}
      POSTGRES_DB: postgres
    ports:
      - "5432:5432"
    volumes:
      - pg_data:/var/lib/postgresql/data
      - ./init.sql:/docker-entrypoint-initdb.d/init.sql:ro
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U postgres"]
      interval: 10s
      timeout: 5s
      retries: 5
    restart: unless-stopped

  auth:
    image: node:22-alpine
    container_name: cloudsync-auth
    environment:
      JWT_SECRET: ${JWT_SECRET}
      PORT: 3001
    ports:
      - "3001:3001"
    volumes:
      - ./auth-server:/app
    working_dir: /app
    command: ["node", "server.js"]
    restart: unless-stopped

volumes:
  pg_data:
EOF
```

### 6c. Create the Postgres init script

```bash
cat > init.sql << 'EOF'
CREATE EXTENSION IF NOT EXISTS cloudsync;
EOF
```

### 6d. Create the JWT auth server

```bash
mkdir -p auth-server
```

Create the package file:

```bash
cat > auth-server/package.json << 'EOF'
{
  "name": "cloudsync-auth",
  "version": "1.0.0",
  "private": true,
  "dependencies": {
    "jsonwebtoken": "^9.0.0"
  }
}
EOF
```

Create the auth server:

```bash
cat > auth-server/server.js << 'AUTHEOF'
const http = require("http");
const jwt = require("jsonwebtoken");

const PORT = process.env.PORT || 3001;
const JWT_SECRET = process.env.JWT_SECRET;

if (!JWT_SECRET) {
  console.error("JWT_SECRET environment variable is required");
  process.exit(1);
}

function parseBody(req) {
  return new Promise((resolve, reject) => {
    let data = "";
    req.on("data", (chunk) => (data += chunk));
    req.on("end", () => {
      try { resolve(JSON.parse(data)); }
      catch { reject(new Error("Invalid JSON")); }
    });
    req.on("error", reject);
  });
}

function respond(res, status, body) {
  res.writeHead(status, { "Content-Type": "application/json" });
  res.end(JSON.stringify(body));
}

const server = http.createServer(async (req, res) => {
  // Health check
  if (req.method === "GET" && req.url === "/healthz") {
    return respond(res, 200, { status: "ok" });
  }

  // Generate token
  // POST /token { "sub": "user-id", "role": "authenticated", "expiresIn": "24h" }
  if (req.method === "POST" && req.url === "/token") {
    try {
      const body = await parseBody(req);
      const sub = body.sub || "anonymous";
      const role = body.role || "authenticated";
      const expiresIn = body.expiresIn || "24h";
      const claims = body.claims || {};

      const token = jwt.sign(
        { sub, role, aud: "authenticated", ...claims },
        JWT_SECRET,
        { expiresIn, algorithm: "HS256" }
      );

      return respond(res, 200, { token, expiresIn });
    } catch (err) {
      return respond(res, 400, { error: err.message });
    }
  }

  respond(res, 404, { error: "Not found" });
});

server.listen(PORT, () => {
  console.log("Auth server listening on port " + PORT);
});
AUTHEOF
```

Install dependencies:

```bash
docker run --rm -v $(pwd)/auth-server:/app -w /app node:22-alpine npm install
```

### 6e. (Optional) Create the JWKS auth server

If you need RS256/JWKS-based authentication instead of (or in addition to) the shared secret approach, create a second auth server that generates an RSA key pair on startup and exposes a JWKS endpoint.

```bash
mkdir -p auth-server-jwks

cat > auth-server-jwks/package.json << 'EOF'
{
  "name": "cloudsync-auth-jwks",
  "version": "1.0.0",
  "private": true,
  "dependencies": {
    "jsonwebtoken": "^9.0.0",
    "jose": "^5.0.0"
  }
}
EOF

cat > auth-server-jwks/server.js << 'EOF'
const http = require("http");
const jwt = require("jsonwebtoken");
const crypto = require("crypto");
const { exportJWK } = require("jose");

const PORT = process.env.PORT || 3002;
const ISSUER = process.env.ISSUER || "cloudsync-auth-jwks";
const KID = "cloudsync-key-1";

let privateKey, publicKey, jwksResponse;

async function init() {
  const pair = crypto.generateKeyPairSync("rsa", {
    modulusLength: 2048,
    publicKeyEncoding: { type: "spki", format: "pem" },
    privateKeyEncoding: { type: "pkcs8", format: "pem" },
  });
  privateKey = pair.privateKey;
  publicKey = pair.publicKey;

  const publicKeyObject = crypto.createPublicKey(publicKey);
  const jwk = await exportJWK(publicKeyObject);
  jwk.kid = KID;
  jwk.alg = "RS256";
  jwk.use = "sig";
  jwksResponse = JSON.stringify({ keys: [jwk] });

  console.log("RSA key pair generated (kid: " + KID + ")");
}

function parseBody(req) {
  return new Promise((resolve, reject) => {
    let data = "";
    req.on("data", (chunk) => (data += chunk));
    req.on("end", () => {
      try { resolve(JSON.parse(data)); }
      catch { reject(new Error("Invalid JSON")); }
    });
    req.on("error", reject);
  });
}

function respond(res, status, body) {
  res.writeHead(status, { "Content-Type": "application/json" });
  res.end(typeof body === "string" ? body : JSON.stringify(body));
}

const server = http.createServer(async (req, res) => {
  if (req.method === "GET" && req.url === "/healthz") {
    return respond(res, 200, { status: "ok" });
  }

  // JWKS endpoint — CloudSync server fetches this to verify tokens
  if (req.method === "GET" && req.url === "/.well-known/jwks.json") {
    res.writeHead(200, { "Content-Type": "application/json" });
    return res.end(jwksResponse);
  }

  // POST /token { "sub": "user-id", "role": "authenticated", "expiresIn": "24h" }
  if (req.method === "POST" && req.url === "/token") {
    try {
      const body = await parseBody(req);
      const sub = body.sub || "anonymous";
      const role = body.role || "authenticated";
      const expiresIn = body.expiresIn || "24h";
      const claims = body.claims || {};

      const token = jwt.sign(
        { sub, role, aud: "authenticated", iss: ISSUER, ...claims },
        privateKey,
        { expiresIn, algorithm: "RS256", keyid: KID }
      );

      return respond(res, 200, { token, expiresIn });
    } catch (err) {
      return respond(res, 400, { error: err.message });
    }
  }

  respond(res, 404, { error: "Not found" });
});

init().then(() => {
  server.listen(PORT, () => {
    console.log("JWKS Auth server listening on port " + PORT);
  });
});
EOF

docker run --rm -v $(pwd)/auth-server-jwks:/app -w /app node:22-alpine npm install
```

Add the JWKS auth service to `docker-compose.yml`:

```yaml
  auth-jwks:
    image: node:22-alpine
    container_name: cloudsync-auth-jwks
    environment:
      PORT: 3002
      ISSUER: cloudsync-auth-jwks
    ports:
      - "3002:3002"
    volumes:
      - ./auth-server-jwks:/app
    working_dir: /app
    command: ["node", "server.js"]
    restart: unless-stopped
```

> **Note:** The JWKS server generates a new RSA key pair each time it starts. For production, persist the key pair to a volume so tokens remain valid across restarts.

---

## Step 7: Start the stack

```bash
cd /data/cloudsync-postgres
docker compose up -d
```

Verify:

```bash
docker compose ps

# Test Postgres
docker compose exec db psql -U postgres -c "SELECT cloudsync_version();"

# Test HS256 auth server
curl http://localhost:3001/healthz

# Test JWKS auth server (if enabled)
curl http://localhost:3002/healthz
curl http://localhost:3002/.well-known/jwks.json
```

---

## Step 8: Generate a JWT token

**HS256 (shared secret):**

```bash
curl -X POST http://localhost:3001/token \
  -H "Content-Type: application/json" \
  -d '{"sub": "user-1", "role": "authenticated"}'
```

**RS256 (JWKS):**

```bash
curl -X POST http://localhost:3002/token \
  -H "Content-Type: application/json" \
  -d '{"sub": "user-1", "role": "authenticated"}'
```

Response (both):

```json
{"token":"eyJhbG...","expiresIn":"24h"}
```

---

## Step 9: Register with CloudSync server

```bash
export CLOUDSYNC_URL="https://your-cloudsync-server.fly.dev"
export ORG_API_KEY="<your-org-api-key>"

# Get the Postgres password from .env
source /data/cloudsync-postgres/.env

# Connection string (same Fly org — use .internal network)
export CONNECTION_STRING="postgres://postgres:$POSTGRES_PASSWORD@<your-app-name>.internal:5432/postgres"

# Or via fly proxy from local machine:
# fly proxy 5432:5432 -a <your-app-name>
# export CONNECTION_STRING="postgres://postgres:$POSTGRES_PASSWORD@localhost:5432/postgres"
```

Register the database:

```bash
curl --request POST "$CLOUDSYNC_URL/v1/databases" \
  --header "Authorization: Bearer $ORG_API_KEY" \
  --header "Content-Type: application/json" \
  --data '{
    "label": "Fly.io Postgres",
    "connectionString": "'"$CONNECTION_STRING"'",
    "provider": "postgres",
    "projectId": "cloudsync-postgres-flyio",
    "databaseName": "postgres"
  }'
```

Save the returned `managedDatabaseId`:

```bash
export MANAGED_DATABASE_ID="<returned-id>"
```

---

## Step 10: Test CloudSync sync

### 10a. Create and enable a test table

```bash
# On the Fly VM
docker compose exec db psql -U postgres -c "
CREATE TABLE IF NOT EXISTS todos (
  id TEXT PRIMARY KEY DEFAULT cloudsync_uuid(),
  title TEXT NOT NULL DEFAULT '',
  done BOOLEAN DEFAULT false
);
SELECT cloudsync_init('todos');
"

# Enable sync via CloudSync API
curl --request POST "$CLOUDSYNC_URL/v1/databases/$MANAGED_DATABASE_ID/cloudsync/enable" \
  --header "Authorization: Bearer $ORG_API_KEY" \
  --header "Content-Type: application/json" \
  --data '{"tables":["todos"]}'
```

### 10b. Generate a token and sync from SQLite

```bash
# Get a JWT token from the auth server
TOKEN=$(curl -s -X POST http://localhost:3001/token \
  -H "Content-Type: application/json" \
  -d '{"sub": "user-1"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['token'])")
```

In a SQLite client:

```sql
.load path/to/cloudsync

CREATE TABLE todos (
  id TEXT PRIMARY KEY DEFAULT (cloudsync_uuid()),
  title TEXT NOT NULL DEFAULT '',
  done BOOLEAN DEFAULT false
);
SELECT cloudsync_init('todos');

SELECT cloudsync_network_init('<managed-database-id>');
SELECT cloudsync_network_set_token('<TOKEN>');

INSERT INTO todos (title) VALUES ('Test from SQLite');
SELECT cloudsync_network_sync(500, 5);
```

Verify on Postgres:

```bash
docker compose exec db psql -U postgres -c "SELECT * FROM todos;"
```

---

## Step 11: CloudSync server JWT configuration

The CloudSync server needs to validate tokens from your auth server. Configuration depends on which auth method you chose.

### Option A: HS256 (shared secret)

In the CloudSync dashboard, go to your PostgreSQL project → **Configuration** → **Edit connection settings**:
- Under **JWT secret**, enter your `JWT_SECRET` value from `.env`
- Click **Save**

Both the auth server and CloudSync must use the same raw secret string (not base64-decoded).

### Option B: RS256 (JWKS)

Configure the JWKS auth server and CloudSync to use asymmetric key verification.

**1. Update docker-compose.yml - JWKS auth server ISSUER:**

```yaml
  auth-jwks:
    environment:
      ISSUER: http://<your-app-name>.internal:3002
```

The issuer is the **base URL** (CloudSync automatically appends `/.well-known/jwks.json`).

**2. Configure CloudSync to accept this issuer:**

In the CloudSync dashboard for this PostgreSQL project:
- Go to **Configuration** tab → **Edit connection settings**
- Under **JWT allowed issuers**, enter:
  ```
  http://<your-app-name>.internal:3002
  ```

CloudSync will:
1. Receive JWT tokens with `iss: http://<your-app-name>.internal:3002`
2. Validate the issuer matches the allowed list
3. Fetch the public key from `http://<your-app-name>.internal:3002/.well-known/jwks.json`
4. Verify the token signature

This is how production auth systems (Auth0, Supabase, Firebase) work — no shared secrets needed.

---

## Access your services

| Service | URL |
|---------|-----|
| **PostgreSQL** | `postgres://postgres:<password>@<your-app-name>.internal:5432/postgres` |
| **Auth Server (HS256)** | `http://<your-app-name>.internal:3001` |
| **Auth Server (JWKS)** | `http://<your-app-name>.internal:3002` |
| **JWKS Endpoint** | `http://<your-app-name>.internal:3002/.well-known/jwks.json` |

From your local machine, use `fly proxy`:

```bash
fly proxy 5432:5432 -a <your-app-name>   # Postgres
fly proxy 3001:3001 -a <your-app-name>   # Auth server (HS256)
fly proxy 3002:3002 -a <your-app-name>   # Auth server (JWKS)
```

---

## Reference: CloudSync Configuration

After deployment, use these values to configure CloudSync dashboard:

### Database Connection

```
postgresql://postgres:<POSTGRES_PASSWORD>@<your-app-name>.internal:5432/postgres
```

Replace:
- `<POSTGRES_PASSWORD>`: from `.env` file
- `<your-app-name>`: your Fly.io app name

### JWT Secret (HS256)

For simple/development setups using shared secrets:

```env
JWT_SECRET=<your-jwt-secret>
```

Enter this in CloudSync dashboard → **Configuration** → **JWT secret**

### JWT Issuer (RS256 with JWKS)

For production setups using asymmetric keys:

```
http://<your-app-name>.internal:3002
```

Enter this in CloudSync dashboard → **Configuration** → **JWT allowed issuers**

CloudSync will automatically fetch the public key from:
```
http://<your-app-name>.internal:3002/.well-known/jwks.json
```

---

## Maintenance

### Startup script (survives VM restarts)

Fly VM root filesystem resets on stop/start — only `/data` persists. Create a startup script:

```bash
cat > /data/startup.sh << 'SCRIPT'
#!/bin/bash
set -e

echo "=== Installing Docker ==="
apt-get update && apt-get install -y ca-certificates curl gnupg fuse-overlayfs

install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | gpg --dearmor -o /etc/apt/keyrings/docker.gpg
chmod a+r /etc/apt/keyrings/docker.gpg

echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
  https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo $VERSION_CODENAME) stable" \
  > /etc/apt/sources.list.d/docker.list

apt-get update && apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin

echo "=== Configuring Docker ==="
mkdir -p /etc/docker
echo '{"storage-driver":"fuse-overlayfs","data-root":"/data/docker"}' > /etc/docker/daemon.json

echo "=== Starting Docker ==="
dockerd > /data/dockerd.log 2>&1 &
until docker info > /dev/null 2>&1; do sleep 1; done
echo "Docker is ready!"

echo "=== Starting CloudSync Postgres ==="
cd /data/cloudsync-postgres
docker compose up -d

echo "=== Done! ==="
SCRIPT
chmod +x /data/startup.sh
```

After any VM restart:

```bash
fly ssh console --app <your-app-name>
/data/startup.sh
```

### Update CloudSync extension

On your local machine:

```bash
cd /path/to/sqlite-sync-dev
git pull && git submodule update --init --recursive
docker build --platform linux/amd64 \
  -f docker/postgresql/Dockerfile \
  -t <your-dockerhub-username>/postgres-cloudsync:17 \
  .
docker push <your-dockerhub-username>/postgres-cloudsync:17
```

On the Fly VM:

```bash
cd /data/cloudsync-postgres
docker compose pull db
docker compose up -d db
```

### View logs

```bash
docker compose logs -f        # All services
docker compose logs -f db     # Postgres only
docker compose logs -f auth   # Auth server only
```

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| `fractional_indexing.h: No such file or directory` | Run `git submodule update --init --recursive` before building |
| `cloudsync_version()` not found | Init scripts only run on first start. Run `CREATE EXTENSION IF NOT EXISTS cloudsync;` manually |
| Auth server won't start | Check `docker compose logs auth`. Ensure `npm install` was run in `auth-server/` |
| Token verification fails (HS256) | Ensure `JWT_SECRET` matches exactly — CloudSync uses the raw string, not base64-decoded |
| Token verification fails (JWKS) | Ensure CloudSync can reach the JWKS endpoint and `JWT_ISSUER` matches the `ISSUER` env var |
| JWKS keys lost after restart | The JWKS server generates new keys on each start. For production, persist keys to a volume |
| Docker commands not found after VM restart | Run `/data/startup.sh` — Fly VM root filesystem resets on stop/start |
| `fuse-overlayfs` not working | Install it: `apt-get install -y fuse-overlayfs` |
| Can't connect to Postgres from outside Fly | Use `fly proxy 5432:5432 -a <your-app-name>` |
