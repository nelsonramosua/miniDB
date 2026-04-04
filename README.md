# miniDB

miniDB is a Redis-compatible in-memory key-value store written in pure C.
It supports a practical subset of Redis commands, speaks the RESP wire protocol,
handles TTL expiration, and persists data to disk via periodic snapshots.

The project prioritises systems-programming clarity over feature completeness:
single-threaded event loop, explicit memory ownership, a compact binary snapshot
format, and small independently-testable modules.

---

## Quick start (60 seconds)

```bash
make
./miniDB --no-persist
```

In another terminal:

```bash
redis-cli -p 6380 PING
redis-cli -p 6380 SET hello world
redis-cli -p 6380 GET hello
```

Run tests:

```bash
make test
```

Explore integration examples:

```bash
ls scripts/useCases
```

---

## Table of contents

- [Architecture overview](#architecture-overview)
- [Command reference](#command-reference)
- [Building and running](#building-and-running)
- [Testing and diagnostics](#testing-and-diagnostics)
- [Benchmarking](#benchmarking)
- [Code style](#code-style)
- [Docker](#docker)
- [Using miniDB from application code](#using-minidb-from-application-code)
- [Real use cases in this repository](#real-use-cases-in-this-repository)
- [Persistence](#persistence)
- [Continuous integration](#continuous-integration)
- [Repository layout](#repository-layout)
- [Known limits](#known-limits)

---

## Architecture overview

```
        client (redis-cli, redis-benchmark, any RESP client)
              │  TCP / RESP
         net.c     ──  poll() event loop, non-blocking I/O, client lifecycle
              │
         command/  ──  dispatch table → per-command handlers
              │
         server.c  ──  config parsing, periodic tick (TTL purge + snapshot scheduling)
              │
         store.c   ──  top-level hash table  key → Object*
              │
         object.c  ──  tagged union: string | list | hash, TTL
              │
        persist.c  ──  snapshot save (atomic rename) + load
```

### Design choices

| Decision | Rationale |
|---|---|
| Single thread + `poll()` | No locking complexity; `poll()` is POSIX-portable and sufficient for hundreds of clients |
| Separate chaining | O(1) delete without tombstones; straightforward resize |
| FNV-1a 32-bit | Fast, good distribution for short string keys |
| Snapshot persistence | Simpler than WAL; worst case: lose writes since last snapshot |
| Lazy + active TTL expiry | Lazy handles hot keys immediately on read; active sweep (100 ms) cleans cold keys |

---

## Command reference

### String and key-space

```
SET     key value [EX seconds | PX milliseconds] [NX | XX]
GET     key
DBSIZE
FLUSHALL
DEL     key [key ...]
RENAME  key newkey
EXISTS  key [key ...]
TYPE    key
KEYS    pattern
SCAN    cursor [MATCH pattern] [COUNT count]
MSET    key value [key value ...]
MGET    key [key ...]
GETSET  key value
GETDEL  key
SETNX   key value
APPEND  key value
STRLEN  key
INCR    key
DECR    key
INCRBY  key delta
DECRBY  key delta
```

### TTL

```
EXPIRE  key seconds
PEXPIRE key milliseconds
PERSIST key                  → remove expiry (1 = removed, 0 = no expiry or missing)
TTL     key                  → remaining seconds     (-1 = no expiry, -2 = missing)
PTTL    key                  → remaining milliseconds
```

### List

```
LPUSH  key value [value ...]   → new length
RPUSH  key value [value ...]   → new length
LPOP   key                     → element or nil
RPOP   key                     → element or nil
LLEN   key                     → length (0 if absent)
LRANGE key start stop          → sublist (inclusive, negative indices supported)
```

Lists are doubly-linked. An empty list is deleted automatically (LPOP / RPOP
on the last element removes the key).

### Hash

```
HSET    key field value [field value ...]   → number of new fields added
HGET    key field                           → value or nil
HDEL    key field [field ...]               → number of fields removed
HLEN    key                                 → number of fields (0 if absent)
HGETALL key                                 → flat field/value array
HKEYS   key                                 → array of field names
HVALS   key                                 → array of values
```

Hash buckets resize at 0.75 load factor (same policy as the top-level store).

### Set

```
SADD      key member [member ...] → number of new members added
SREM      key member [member ...] → number of members removed
SISMEMBER key member              → 1 if member, 0 if missing
SMEMBERS  key                     → array of all members
SCARD     key                     → number of members
SUNION    key [key ...]           → members present in any set
SINTER    key [key ...]           → members present in all sets
SDIFF     key [key ...]           → members present in first, absent in others
```

### Server and diagnostics

```
INFO                 → server stats (version, uptime, commands processed, keyspace)
PING [message]       → PONG or echoes message
QUIT                 → closes connection
CLIENT  ...          (permissive stub — satisfies redis-cli preflight)
COMMAND ...          (returns empty array)
CONFIG  ...          (returns empty values; RESETSTAT → OK)
```

`CLIENT`, `COMMAND`, and `CONFIG` exist to satisfy the probes that `redis-cli`
and `redis-benchmark` send before issuing real commands.

### Pattern matching (KEYS)

KEYS delegates to POSIX `fnmatch(3)`. Supported patterns:

| Pattern | Meaning |
|---|---|
| `*` | Match any sequence of characters |
| `?` | Match any single character |
| `[abc]` | Match one character from the set |
| `[a-z]` | Match one character in the range |

---

## Building and running

### Requirements

- `gcc` (C11 support)
- `make`

Optional (for full dev workflow):

- `valgrind`
- `clang-format` and `clang-tidy`
- `redis-cli` and `redis-benchmark` (package: `redis-tools`)

### Build

```bash
make           # release binary → ./miniDB
make debug     # ASan + UBSan build → ./miniDB_debug
```

### Run

```bash
./miniDB
```

Defaults: port `6380`, snapshot `miniDB.snap`, save interval `300 s`.

### CLI options

```
--port PORT              TCP port to listen on        (default: 6380)
--snapshot PATH          Snapshot file path           (default: miniDB.snap)
--save-interval SECS     Seconds between snapshots, 0 = off  (default: 300)
--no-persist             Disable persistence entirely
--help
```

```bash
# No persistence (useful for testing)
./miniDB --no-persist

# Custom port, fast snapshot cadence, explicit path
./miniDB --port 7390 --snapshot /tmp/miniDB.snap --save-interval 30
```

---

## Testing and diagnostics

```bash
make test              # unit tests (testStore + testProtocol + testPersist)
make valgrind          # same tests under valgrind --leak-check=full
make test-persistence  # integration: save → restart → verify snapshot round-trip
make debug             # build with AddressSanitizer + UndefinedBehaviorSanitizer
```

### Unit tests

`tests/testStore.c` — store lifecycle, TTL expiry, lazy deletion, resize, iteration.

`tests/testProtocol.c` — inline and RESP parser, response builders,
command dispatch, error paths.

`tests/testPersist.c` — snapshot save and load round-trip for all three object
types, expired-key filtering on load, version mismatch rejection.

### Integration tests

`scripts/test_persistence.sh` starts the server, writes keys of all three types
plus one key with a 1-second TTL, waits for a snapshot and for the TTL to
elapse, restarts the server, and verifies that durable keys survived and the
expired key is absent. Requires `redis-cli`.

```bash
make test-persistence
```

`scripts/smoke_test.sh` exercises every implemented command against a live server
instance and checks expected responses. Requires `redis-cli`.

```bash
make smoke-test

./scripts/smoke_test.sh          # default port 6380
./scripts/smoke_test.sh 7399     # custom port
```

---

## Benchmarking

`scripts/benchmark.sh` wraps `redis-benchmark` for `PING`, `SET`, `GET`, and
`INCR`. Requires `redis-benchmark`.

```bash
make benchmark
```

Environment variables:

| Variable | Default | Meaning |
|---|---|---|
| `REQUESTS` | 100000 | Total requests per test |
| `CLIENTS` | 10 | Concurrent connections |
| `PIPELINE` | 1 | Pipeline depth |
| `KEYSPACE` | 10000 | Key space size |
| `RANDOM_KEYS` | 0 | `1` = random keys |
| `HOST` | 127.0.0.1 | Server address |

```bash
REQUESTS=200000 CLIENTS=20 PIPELINE=8 make benchmark
```

---

## Code style

Style is enforced by `.clang-format` and `.clang-tidy`.

```bash
make format       # apply clang-format in place
make lint-style   # dry-run clang-format + clang-tidy naming checks (used in CI)
```

---

## Docker

The Dockerfile is multi-stage:

- **builder** — compiles, runs unit tests, and runs valgrind. A broken commit
  cannot produce a runnable image.
- **runtime** — `debian:bookworm-slim`, non-root user, only the binary and a
  `/data` volume.

```bash
# Build
docker build -t minidb .

# Run with persistent snapshots
docker run -p 6380:6380 -v $(pwd)/data:/data minidb

# Custom options
docker run -p 7380:7380 -v $(pwd)/data:/data minidb \
  --port 7380 --snapshot /data/miniDB.snap --save-interval 60

# No persistence
docker run -p 6380:6380 minidb --no-persist
```

---

## Using miniDB from application code

miniDB speaks standard RESP, so any Redis client library connects to it
without modification.

### Python (redis-py)

```python
import redis

r = redis.Redis(host='localhost', port=6380)

# String with TTL — session store
r.set('session:abc123', 'user_id=42', ex=3600)
print(r.get('session:abc123'))        # b'user_id=42'

# Counter — rate limiting
r.incr('ratelimit:192.168.1.1')
r.expire('ratelimit:192.168.1.1', 60)

# Hash — structured record
r.hset('user:42', mapping={'name': 'Nelson', 'role': 'admin'})
print(r.hgetall('user:42'))

# List — simple job queue
r.rpush('jobs', 'task_1', 'task_2')
job = r.lpop('jobs')                  # b'task_1'
```

### Node.js (ioredis)

```js
import Redis from 'ioredis';
const r = new Redis({ port: 6380 });

await r.set('key', 'value', 'EX', 60);
console.log(await r.get('key'));   // 'value'
await r.quit();
```

### Connecting to Docker

```bash
docker run -d -p 6380:6380 -v $(pwd)/data:/data minidb
# then connect any client to localhost:6380
```

## Real use cases in this repository

Use cases live under `scripts/useCases/`:

- `scripts/useCases/node/Analytics` (Node.js HTTP analytics service)
- `scripts/useCases/go/URLShortener` (Go HTTP URL shortener)
- `scripts/useCases/python/APIServer` (FastAPI integration service)
- `scripts/useCases/python/Tasks` (Python terminal task manager)
- `scripts/useCases/c/Metrics` (Native C system metrics daemon and viewer)

Use-case docs:

- `scripts/useCases/README.md`
- `scripts/useCases/node/README.md`
- `scripts/useCases/go/README.md`
- `scripts/useCases/python/README.md`
- `scripts/useCases/c/README.md`

### Python FastAPI use case (APIServer)

Install dependencies:

```bash
python3 -m pip install -r scripts/useCases/python/APIServer/requirements.txt
```

Run API server:

```bash
cd scripts/useCases/python/APIServer
uvicorn pythonApiServer:app --host 0.0.0.0 --port 8080 --reload
```

Quick flow (example):

```bash
curl -s http://127.0.0.1:8080/health
curl -s -X POST http://127.0.0.1:8080/users \
  -H 'Content-Type: application/json' \
  -d '{"user_id":"42","name":"Nelson","role":"admin"}'
curl -s -X POST http://127.0.0.1:8080/login \
  -H 'Content-Type: application/json' \
  -d '{"user_id":"42","client_ip":"127.0.0.1"}'
curl -s http://127.0.0.1:8080/metrics
```

### Python CLI use case (Tasks)

```bash
cd scripts/useCases/python/Tasks
python3 -m pip install redis
python3 tasks.py --help
```

---

## Persistence

Snapshots use an atomic write: serialise to `<path>.tmp`, call `fdatasync`,
then `rename` over the final path. A crash mid-save cannot corrupt the previous
snapshot.

**Binary format** (little-endian, no external dependencies):

```
Header:     "KVDB" (4 bytes magic) | version u32 | entry_count u64
Per entry:  type u8 | key_len u32 | key | expire_ms i64 | payload
  String:   val_len u32 | val_bytes
  List:     elem_count u32 | (elem_len u32 | elem_bytes)*
  Hash:     field_count u32 | (k_len u32 | k | v_len u32 | v)*
```

Expired keys are skipped during load. `expire_ms == 0` means no expiry.

The format is versioned (`SNAPSHOT_VERSION = 1`). An incompatible version
causes load to fail cleanly rather than silently misreading data.

---

## Continuous integration

GitHub Actions (`.github/workflows/ci.yml`) runs on every push to `main`/`dev`
and on pull requests:

| Job | Steps |
|---|---|
| `style-check` | `clang-format` dry-run + `clang-tidy` naming checks |
| `build-and-test` | release build → debug build (ASan/UBSan) → unit tests → valgrind → integration smoke test |
| `docker-build` | Docker image build + container smoke test (depends on `build-and-test`) |
| `usecases-smoke` | conditional smoke checks for Node/Go/Python use cases (runs only when use-case paths change) |

---

## Repository layout

```
miniDB.c                entry point: server lifecycle

src/
  net.c                  poll loop, accept/read/write, client lifecycle
  server.c               ServerConfig parsing, server context, periodic tick
  store.c                top-level hash table (key → Object*)
  object.c               Object type system, list and hash operations
  protocol.c             RESP parser and response builders
  persist.c              snapshot save and load
  command/
    command.c            dispatch table and cmdLookup/cmdDispatch
    commandConn.c        PING, QUIT, CLIENT, COMMAND, CONFIG, INFO
    commandString.c      SET, GET, DEL, EXISTS, TTL, INCR, APPEND, SETNX, …
    commandList.c        LPUSH, RPUSH, LPOP, RPOP, LLEN, LRANGE
    commandHash.c        HSET, HGET, HDEL, HLEN, HGETALL, HKEYS, HVALS
    commandSet.c         SADD, SREM, SISMEMBER, SMEMBERS, SCARD, SUNION, …

include/
  object.h               Object, KVList, KVHash types and lifecycle
  store.h                Store type and API
  command.h              CmdFn, CmdEntry, public handler declarations
  commandInternal.h      Internal helpers (getOrCreate*, parseI64Strict, …)
  protocol.h             Request, RespBuf, proto*/resp* API
  net.h                  netRun declaration and tuning constants
  server.h               Server, ServerConfig, and API
  persist.h              persistSave / persistLoad
  hashutil.h             hashFnv1a32 — shared FNV-1a implementation

tests/
  testStore.c            store, object, TTL unit tests
  testProtocol.c         parser and dispatch unit tests
  testPersist.c          snapshot save/load round-trip unit tests

scripts/
  smoke_test.sh          live command coverage test (requires redis-cli)
  benchmark.sh           redis-benchmark wrapper
  test_persistence.sh    persistence round-trip integration test
```

---

## Known limits

- Not a production Redis replacement.
- Single-threaded: one core, no parallelism. Throughput is bounded by a single
  `poll()` loop.
- Snapshot persistence only: writes since the last snapshot are lost on a hard
  crash. No write-ahead log.
- `CLOCK_REALTIME` is used for TTL timestamps. System clock jumps (NTP
  corrections, `settimeofday`) can affect expiry behaviour. A future migration
  to `CLOCK_MONOTONIC` requires a change to the persistence format.
- No replication, clustering, transactions (MULTI/EXEC), pub/sub, or Lua
  scripting.
- `KEYS` is O(N) over the whole keyspace. On large datasets prefer a secondary
  index or implement `SCAN` for incremental iteration.