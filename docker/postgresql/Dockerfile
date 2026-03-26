# PostgreSQL Docker image with CloudSync extension pre-installed
ARG POSTGRES_TAG=17
FROM postgres:${POSTGRES_TAG}

# Derive the major version from PG_MAJOR (set by the official postgres image)
# and install the matching server-dev package
RUN apt-get update && apt-get install -y \
    build-essential \
    postgresql-server-dev-${PG_MAJOR} \
    git \
    make \
    && rm -rf /var/lib/apt/lists/*

# Create directory for extension source
WORKDIR /tmp/cloudsync

# Copy entire source tree (needed for includes and makefiles)
COPY src/ ./src/
COPY modules/ ./modules/
COPY docker/ ./docker/
COPY Makefile .

# Build and install the CloudSync extension
RUN make postgres-build && \
    make postgres-install && \
    make postgres-clean

# Verify installation
RUN echo "Verifying CloudSync extension installation..." && \
    ls -la $(pg_config --pkglibdir)/cloudsync.so && \
    ls -la $(pg_config --sharedir)/extension/cloudsync* && \
    echo "CloudSync extension installed successfully"

# Set default PostgreSQL credentials
ENV POSTGRES_PASSWORD=postgres
ENV POSTGRES_DB=cloudsync_test

# Expose PostgreSQL port
EXPOSE 5432

# Copy initialization script (creates CloudSync metadata tables)
COPY docker/postgresql/init.sql /docker-entrypoint-initdb.d/

# Return to root directory
WORKDIR /

# Add label with extension version
LABEL org.sqliteai.cloudsync.version="1.0" \
      org.sqliteai.cloudsync.description="PostgreSQL with CloudSync CRDT extension"
