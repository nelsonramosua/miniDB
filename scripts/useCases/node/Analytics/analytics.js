/**
 * analytics.js — page-view analytics server backed by miniDB
 *
 * Install:  npm install
 * Start:    node analytics.js [port]           (default 3000)
 *
 * Endpoints
 * ---------
 *   POST /hit?path=/page      record a page view (call from any frontend)
 *   GET  /stats               JSON summary (total hits, today, unique visitors)
 *   GET  /stats/pages         per-path hit counts, sorted by popularity
 *   GET  /stats/recent        last 50 requests
 *   GET  /dashboard           live HTML dashboard (auto-refreshes every 5 s)
 *
 * Data model in miniDB
 * --------------------
 *   hits:total           INCR  all-time hit counter
 *   hits:daily:<date>    INCR  hits for a given date  (YYYY-MM-DD)
 *   hits:page:<path>     INCR  hits for a specific path
 *   visitors:daily:<date> INCR  unique visitors for a given date
 *   visitor:<ip>:<date>  STRING  set on first visit per day (SETNX, TTL 24 h)
 *   rate:<ip>            INCR  request count for rate-limiting (TTL 60 s)
 *   log:recent           LIST  last N requests as JSON strings (LPUSH + LTRIM)
 *
 * Quick test
 * ----------
 *   curl -X POST "http://localhost:3000/hit?path=/home"
 *   curl -X POST "http://localhost:3000/hit?path=/about"
 *   curl "http://localhost:3000/stats"
 *   open "http://localhost:3000/dashboard"
 */

import http from "node:http";
import { createClient } from "redis";

const PORT       = parseInt(process.argv[2] ?? "3000", 10);
const MINIDB     = { socket: { host: "localhost", port: 6380 } };
const RATE_LIMIT = 120;   // max requests per IP per minute
const LOG_SIZE   = 50;    // entries kept in the recent-request log

// ── DB connection ─────────────────────────────────────────────────────────────

const db = createClient(MINIDB);
db.on("error", err => console.error("[redis]", err.message));
await db.connect();
console.log("[analytics] connected to miniDB");

// ── helpers ───────────────────────────────────────────────────────────────────

function today() {
  return new Date().toISOString().slice(0, 10);
}

function json(res, statusCode, data) {
  const body = JSON.stringify(data, null, 2);
  res.writeHead(statusCode, { "Content-Type": "application/json" });
  res.end(body);
}

/** Returns false when the IP has exceeded RATE_LIMIT req/min. */
async function withinRateLimit(ip) {
  const key   = `rate:${ip}`;
  const count = await db.incr(key);
  if (count === 1) await db.expire(key, 60);
  return count <= RATE_LIMIT;
}

/** Record one page view from a given IP address. */
async function recordHit(path, ip) {
  const d = today();

  // Fire all writes concurrently — order does not matter here.
  await Promise.all([
    db.incr("hits:total"),
    db.incr(`hits:daily:${d}`),
    db.incr(`hits:page:${path}`),

    // Unique-visitor tracking: SETNX returns true only on the *first* set.
    db.setNX(`visitor:${ip}:${d}`, "1").then(isNew => {
      if (isNew) {
        return Promise.all([
          db.expire(`visitor:${ip}:${d}`, 86400), // key lives until same time tomorrow
          db.incr(`visitors:daily:${d}`),
        ]);
      }
    }),

      // Append to rolling request log and cap its length using trimming loop.
      db.lPush("log:recent", JSON.stringify({
        ts: new Date().toISOString(),
        path,
        ip,
      })).then(() => trimRecentLog()),
  ]);
}

async function trimRecentLog() {
  let len = await db.lLen("log:recent");
  while (len > LOG_SIZE) {
    await db.rPop("log:recent");
    len -= 1;
  }
}

async function getSummary() {
  const d = today();
  const [total, dailyHits, dailyVisitors] = await Promise.all([
    db.get("hits:total"),
    db.get(`hits:daily:${d}`),
    db.get(`visitors:daily:${d}`),
  ]);
  return {
    date:             d,
    hits_total:       parseInt(total        ?? 0, 10),
    hits_today:       parseInt(dailyHits    ?? 0, 10),
    visitors_today:   parseInt(dailyVisitors ?? 0, 10),
  };
}

async function getPageStats() {
  const keys = await db.keys("hits:page:*");
  const counts = await Promise.all(keys.map(k => db.get(k)));
  return keys
    .map((k, i) => ({ path: k.replace("hits:page:", ""), hits: parseInt(counts[i] ?? 0, 10) }))
    .sort((a, b) => b.hits - a.hits);
}

async function getRecent() {
  const raw = await db.lRange("log:recent", 0, LOG_SIZE - 1);
  return raw.map(r => JSON.parse(r));
}

// ── dashboard HTML ────────────────────────────────────────────────────────────

async function buildDashboard() {
  const [summary, pages, recent] = await Promise.all([
    getSummary(), getPageStats(), getRecent(),
  ]);

  const pageRows = pages.slice(0, 25).map(p =>
    `<tr><td>${escHtml(p.path)}</td><td>${p.hits}</td></tr>`,
  ).join("");

  const logRows = recent.slice(0, 25).map(r =>
    `<tr><td>${escHtml(r.ts)}</td><td>${escHtml(r.ip)}</td><td>${escHtml(r.path)}</td></tr>`,
  ).join("");

  return `<!DOCTYPE html>
<html lang="en"><head>
  <meta charset="utf-8">
  <meta http-equiv="refresh" content="5">
  <title>miniDB Analytics</title>
  <style>
    *{box-sizing:border-box}
    body{font-family:monospace;max-width:960px;margin:40px auto;padding:0 24px;background:#0d1117;color:#c9d1d9}
    h1{color:#58a6ff;margin-bottom:4px}
    h2{color:#8b949e;font-size:.85em;letter-spacing:.1em;text-transform:uppercase;margin:28px 0 8px}
    .cards{display:flex;gap:16px;margin:24px 0}
    .card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:20px 28px;flex:1}
    .card .n{font-size:2.4em;color:#58a6ff;font-weight:700}
    .card .l{font-size:.8em;color:#8b949e;margin-top:4px}
    table{border-collapse:collapse;width:100%}
    th,td{text-align:left;padding:6px 12px;border-bottom:1px solid #21262d;font-size:.85em}
    th{color:#8b949e}
    .note{font-size:.75em;color:#484f58;margin-top:4px}
  </style>
</head><body>
  <h1>miniDB Analytics</h1>
  <p class="note">auto-refreshes every 5 s &nbsp;·&nbsp; ${summary.date}</p>
  <div class="cards">
    <div class="card"><div class="n">${summary.hits_total}</div><div class="l">all-time hits</div></div>
    <div class="card"><div class="n">${summary.hits_today}</div><div class="l">hits today</div></div>
    <div class="card"><div class="n">${summary.visitors_today}</div><div class="l">unique visitors today</div></div>
  </div>

  <h2>Top pages</h2>
  <table><tr><th>Path</th><th>Hits</th></tr>${pageRows || "<tr><td colspan=2>no data yet</td></tr>"}</table>

  <h2>Recent requests</h2>
  <table><tr><th>Time</th><th>IP</th><th>Path</th></tr>${logRows || "<tr><td colspan=3>no data yet</td></tr>"}</table>
</body></html>`;
}

function escHtml(s) {
  return String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;");
}

// ── request router ────────────────────────────────────────────────────────────

const server = http.createServer(async (req, res) => {
  const ip  = (req.headers["x-forwarded-for"] ?? req.socket.remoteAddress ?? "").split(",")[0].trim();
  const url = new URL(req.url, "http://localhost");

  // CORS — allow any frontend to call POST /hit
  res.setHeader("Access-Control-Allow-Origin",  "*");
  res.setHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  if (req.method === "OPTIONS") { res.writeHead(204); res.end(); return; }

  // Rate limiting
  if (!(await withinRateLimit(ip))) {
    return json(res, 429, { error: "rate limit exceeded" });
  }

  try {
    // POST /hit?path=<path>  — record a page view
    if (req.method === "POST" && url.pathname === "/hit") {
      const path = (url.searchParams.get("path") ?? "/").slice(0, 200);
      await recordHit(path, ip);
      return json(res, 200, { ok: true });
    }

    if (req.method !== "GET") {
      return json(res, 405, { error: "method not allowed" });
    }

    // GET /stats
    if (url.pathname === "/stats") {
      return json(res, 200, await getSummary());
    }

    // GET /stats/pages
    if (url.pathname === "/stats/pages") {
      return json(res, 200, await getPageStats());
    }

    // GET /stats/recent
    if (url.pathname === "/stats/recent") {
      return json(res, 200, await getRecent());
    }

    // GET /dashboard
    if (url.pathname === "/dashboard") {
      const html = await buildDashboard();
      res.writeHead(200, { "Content-Type": "text/html" });
      return res.end(html);
    }

    return json(res, 404, { error: "not found" });

  } catch (err) {
    console.error(err);
    return json(res, 500, { error: "internal server error" });
  }
});

server.listen(PORT, () => {
  console.log(`[analytics] http://localhost:${PORT}`);
  console.log(`[analytics] dashboard → http://localhost:${PORT}/dashboard`);
  console.log(`[analytics] record a hit: curl -X POST "http://localhost:${PORT}/hit?path=/home"`);
});