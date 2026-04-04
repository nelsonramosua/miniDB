# Go use cases

## Available

- `URLShortener/`: HTTP URL shortener with stats and rate limiting.

## Prerequisites

- Go 1.18+
- miniDB on `localhost:6380`

## Run

```bash
cd scripts/useCases/go/URLShortener
go mod tidy
go run .
```

Optional:

```bash
MINIDB_ADDR=localhost:6380 go run .
```

Server runs on `http://localhost:8080`.

## Main endpoints

- `POST /shorten`
- `GET /{code}`
- `GET /stats/{code}`
- `GET /`

## Quick test

```bash
curl -s -X POST http://localhost:8080/shorten \
  -H "Content-Type: application/json" \
  -d '{"url":"https://github.com"}'
```