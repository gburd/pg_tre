# Dockerfile for pg_tre development and testing
# Usage: docker build -t pg_tre:latest .
#        docker run -it --rm pg_tre:latest psql

FROM postgres:18

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    postgresql-server-dev-18 \
    autoconf \
    automake \
    libtool \
    gettext \
    m4 \
    git \
    && rm -rf /var/lib/apt/lists/*

# Clone pg_tre with submodules
WORKDIR /tmp
RUN git clone --recurse-submodules https://codeberg.org/gregburd/pg_tre.git

# Build and install
WORKDIR /tmp/pg_tre
RUN PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config make && \
    PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config make install

# Configure PostgreSQL to preload pg_tre
RUN echo "shared_preload_libraries = 'pg_tre'" >> /usr/share/postgresql/postgresql.conf.sample

# Initialize database with pg_tre
COPY --chown=postgres:postgres docker-entrypoint-initdb.d/init-pg_tre.sql /docker-entrypoint-initdb.d/

# Cleanup build artifacts
RUN rm -rf /tmp/pg_tre

EXPOSE 5432
