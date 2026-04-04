# ── Stage 1: build + test ────────────────────────────────────────────────────
#
# Uses the full gcc image so we have make, valgrind, and all build tools.
# Tests run here — if they fail, the image build fails. This means a
# broken commit can never produce a runnable Docker image.
FROM gcc:13-bookworm AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        valgrind \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

# 1. Release binary
RUN make

# 2. Unit tests — build failure aborts image creation
RUN make test

# 3. Valgrind clean — catches leaks and memory errors before shipping
RUN make valgrind

# ── Stage 2: minimal runtime ─────────────────────────────────────────────────
#
# debian:bookworm-slim has no gcc, no make, no shell tools beyond busybox.
# The attack surface is just the binary + libc. Total image size ~30 MB.
FROM debian:bookworm-slim

# Non-root user: the process has no privileges if it is compromised
RUN useradd --system --no-create-home --shell /usr/sbin/nologin minidb

WORKDIR /app

# Copy only the binary from the builder stage
COPY --from=builder /build/miniDB .

# Persistent data directory for snapshots
RUN mkdir -p /data && chown minidb:minidb /data
VOLUME ["/data"]

USER minidb
EXPOSE 6380

# Defaults: snapshot to the mounted volume every 60 s.
# Override at runtime: docker run minidb --port 6380 --save-interval 30
ENTRYPOINT ["./miniDB"]
CMD ["--port", "6380", "--snapshot", "/data/miniDB.snap", "--save-interval", "60"]