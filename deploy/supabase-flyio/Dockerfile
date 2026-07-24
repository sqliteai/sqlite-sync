# Self-hosted Supabase + CloudSync, packaged as a single Fly Machine image.
#
# The image ships dockerd + docker compose and a pinned copy of the upstream
# Supabase `docker/` directory. Everything the stack needs at runtime (images,
# PGDATA, .env) lives on the Fly volume mounted at /data, so machine restarts
# and redeploys keep the data and re-run the same bootstrap.
FROM docker:28-dind

# fuse-overlayfs: Docker's overlay2 driver fails inside a Fly Machine
# ("failed to convert whiteout file: operation not permitted").
RUN apk add --no-cache bash fuse-overlayfs openssl

# Pinned upstream Supabase docker/ tree. Bump to adopt new service versions.
ARG SUPABASE_REF=312d05af4be0a404325417254d57f69378a0091c
RUN wget -qO /tmp/sb.tgz "https://codeload.github.com/supabase/supabase/tar.gz/${SUPABASE_REF}" \
 && tar -xzf /tmp/sb.tgz -C /tmp \
 && mv /tmp/supabase-*/docker /supabase \
 && rm -rf /tmp/sb.tgz /tmp/supabase-*

COPY docker-compose.cloudsync.yml cloudsync.sql entrypoint.sh /supabase/
RUN chmod +x /supabase/entrypoint.sh

ENTRYPOINT ["/supabase/entrypoint.sh"]
