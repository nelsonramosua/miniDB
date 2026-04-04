# Python use cases

## Available

- `APIServer/`: FastAPI service integrated with miniDB
- `Tasks/`: terminal task manager CLI

## Prerequisites

- Python 3.10+
- miniDB on `localhost:6380`

## APIServer

Files:

- `pythonApiServer.py`
- `requirements.txt`

Run:

```bash
cd scripts/useCases/python/APIServer
python3 -m pip install -r requirements.txt
uvicorn pythonApiServer:app --host 0.0.0.0 --port 8080
```

Common endpoints:

- `GET /health`
- `POST /users`
- `POST /login`
- `GET /metrics`

## Tasks

File:

- `tasks.py`

Run:

```bash
cd scripts/useCases/python/Tasks
python3 -m pip install redis
python3 tasks.py --help
```

Examples:

```bash
python3 tasks.py add "Write report" --priority high --tag work
python3 tasks.py list
python3 tasks.py done 1
python3 tasks.py stats
```