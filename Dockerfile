# syntax=docker/dockerfile:1
#
# Two stages, so the shipped image carries no compiler and no test tooling.
#
# The builder is Debian trixie rather than the plan's gcc:14 image on purpose:
# gcc:14 is bookworm-based, while scripts/sandbox.sh builds and tests against
# trixie's libpq and hiredis. Building against one and running against the other
# links the binary to libraries that are only almost the same, which fails at
# runtime in a place far from the change. Matching the base keeps the image and
# the sandbox the same environment.
FROM debian:trixie-slim AS build

# libc6-dev is named explicitly because trixie's gcc only Recommends it, and
# --no-install-recommends would otherwise leave the compiler without stdio.h.
# The sandbox gets it as a transitive dependency of libcriterion-dev, which an
# image that only builds the server does not install.
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc make pkg-config libc6-dev \
        libpq-dev libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN make build

# The runtime has the shared libraries the binary links, the schema it applies at
# startup, and the config it falls back to when the environment says nothing.
# The image is not a build environment: no gcc, no headers, no make.
FROM debian:trixie-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        libpq5 libhiredis1.1.0 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/bin/shortener /app/shortener
COPY --from=build /src/bin/load_test /app/bin/load_test
COPY --from=build /src/config.yaml /app/config.yaml
COPY --from=build /src/src/db/schema.sql /app/src/db/schema.sql

# Non-root, with no home and no login shell. The process binds 8000 and reads two
# files; it never writes to the image. Not --system: a 10001 uid is deliberately
# outside the system range so it cannot collide with a package's user.
RUN useradd --uid 10001 --no-create-home \
        --shell /usr/sbin/nologin shortener
USER shortener

EXPOSE 8000

# The server is the default; `docker run <image> --cleanup` runs one retention
# sweep instead, which is what a cron entry wants.
ENTRYPOINT ["/app/shortener"]
