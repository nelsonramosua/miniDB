# Node use cases

## Available

- `Analytics/`: HTTP server to record page hits and expose analytics stats.

## Prerequisites

- Node.js 18+
- miniDB on `localhost:6380`

## Run

```bash
cd scripts/useCases/node/Analytics
npm install
node analytics.js
```

By default it starts on `http://localhost:3000`.

## Main endpoints

- `POST /hit?path=/home`
- `GET /stats`
- `GET /stats/pages`
- `GET /stats/recent`
- `GET /dashboard`

## Quick test

```bash
curl -X POST "http://localhost:3000/hit?path=/home"
curl -s "http://localhost:3000/stats"
```