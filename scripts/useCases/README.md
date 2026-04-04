# miniDB useCases

This directory contains real miniDB integration examples.

Note: these use-case apps were generated with AI and then adapted for this repository.

## Current structure

- `go/URLShortener`: HTTP API for URL shortening
- `node/Analytics`: page analytics HTTP server
- `python/APIServer`: FastAPI service for users, sessions, and queue operations
- `python/Tasks`: terminal task manager CLI

## Prerequisites

- miniDB running on `localhost:6380`
- Node.js 18+
- Go 1.18+
- Python 3.10+

## CI smoke checks

CI uses `scripts/ci/usecases_smoke.sh` to discover and validate use cases by manifest:

- Node: finds `package.json`, installs dependencies, and syntax-checks `.js`
- Go: finds `go.mod` and runs `go build ./...`
- Python: finds `requirements.txt`, installs dependencies, and compiles `.py`

## Quick start

Node Analytics:

```bash
cd scripts/useCases/node/Analytics
npm install
node analytics.js
```

Go URLShortener:

```bash
cd scripts/useCases/go/URLShortener
go mod tidy
go run .
```

Python API Server:

```bash
cd scripts/useCases/python/APIServer
python3 -m pip install -r requirements.txt
uvicorn pythonApiServer:app --host 0.0.0.0 --port 8080
```

Python Tasks:

```bash
cd scripts/useCases/python/Tasks
python3 -m pip install redis
python3 tasks.py --help
```