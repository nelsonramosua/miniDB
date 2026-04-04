// main.go — URL shortener backed by miniDB
//
// Install:  go mod tidy
// Run:      go run .
//           MINIDB_ADDR=localhost:6380 go run .
//
// Endpoints
// ---------
//   POST /shorten            {"url":"https://example.com"} → {"code":"ab12cd","short":"...","url":"..."}
//   GET  /{code}             redirect to the original URL + increment hit counter
//   GET  /stats/{code}       {"code":"...","url":"...","hits":42,"created":"..."}
//   GET  /                   usage instructions (plain text)
//
// Rate limiting
// -------------
//   POST /shorten is limited to 10 requests per minute per IP.
//   Responses include X-RateLimit-Remaining.
//
// Data model in miniDB
// --------------------
//   url:<code>                HASH  {url, created, hits}
//   rate:shorten:<ip>         INCR  request count (TTL 60 s)
//   meta:urls_created         INCR  total URLs shortened
//   meta:urls_redirected      INCR  total redirects served
//
// Quick test
// ----------
//   curl -s -X POST http://localhost:8080/shorten \
//        -H "Content-Type: application/json" \
//        -d '{"url":"https://github.com"}'
//
//   curl -v http://localhost:8080/<code>
//   curl -s http://localhost:8080/stats/<code>

package main

import (
	"context"
	"crypto/rand"
	"encoding/json"
	"fmt"
	"log"
	"math/big"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/redis/go-redis/v9"
)

// ── config ────────────────────────────────────────────────────────────────────

const (
	listenAddr = ":8080"
	codeLen    = 6    // characters in a short code
	rateLimit  = 10   // POST /shorten requests per IP per 60 s
)

var alphabet = []byte("abcdefghijklmnopqrstuvwxyz0123456789")

// ── globals ───────────────────────────────────────────────────────────────────

var (
	ctx = context.Background()
	rdb *redis.Client
)

// ── entry point ───────────────────────────────────────────────────────────────

func main() {
	addr := os.Getenv("MINIDB_ADDR")
	if addr == "" {
		addr = "localhost:6380"
	}

	rdb = redis.NewClient(&redis.Options{Addr: addr})
	if err := rdb.Ping(ctx).Err(); err != nil {
		log.Fatalf("cannot connect to miniDB at %s: %v", addr, err)
	}
	log.Printf("connected to miniDB at %s", addr)

	mux := http.NewServeMux()
	mux.HandleFunc("/", route)

	log.Printf("URL shortener listening on http://localhost%s", listenAddr)
	log.Fatal(http.ListenAndServe(listenAddr, mux))
}

// ── code generation ───────────────────────────────────────────────────────────

func randomCode() (string, error) {
	b := make([]byte, codeLen)
	for i := range b {
		n, err := rand.Int(rand.Reader, big.NewInt(int64(len(alphabet))))
		if err != nil {
			return "", err
		}
		b[i] = alphabet[n.Int64()]
	}
	return string(b), nil
}

// uniqueCode generates a random code that does not yet exist in the store.
// Tries up to 5 times before giving up.
func uniqueCode() (string, error) {
	for i := 0; i < 5; i++ {
		code, err := randomCode()
		if err != nil {
			return "", err
		}
		exists, err := rdb.Exists(ctx, "url:"+code).Result()
		if err != nil {
			return "", err
		}
		if exists == 0 {
			return code, nil
		}
	}
	return "", fmt.Errorf("could not generate a unique code after 5 attempts")
}

// ── rate limiting ─────────────────────────────────────────────────────────────

// checkRate returns (remaining, ok). ok=false means the limit was exceeded.
func checkRate(ip string) (int64, bool, error) {
	key := "rate:shorten:" + ip
	count, err := rdb.Incr(ctx, key).Result()
	if err != nil {
		return 0, false, err
	}
	if count == 1 {
		rdb.Expire(ctx, key, 60*time.Second)
	}
	remaining := int64(rateLimit) - count
	if remaining < 0 {
		remaining = 0
	}
	return remaining, count <= rateLimit, nil
}

// ── JSON helpers ──────────────────────────────────────────────────────────────

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	enc := json.NewEncoder(w)
	enc.SetIndent("", "  ")
	enc.Encode(v) //nolint:errcheck
}

func clientIP(r *http.Request) string {
	if fwd := r.Header.Get("X-Forwarded-For"); fwd != "" {
		return strings.SplitN(fwd, ",", 2)[0]
	}
	// strip port
	ip := r.RemoteAddr
	if i := strings.LastIndex(ip, ":"); i != -1 {
		ip = ip[:i]
	}
	return ip
}

func route(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodPost && r.URL.Path == "/shorten" {
		handleShorten(w, r)
		return
	}

	if r.Method == http.MethodGet && r.URL.Path == "/" {
		handleRoot(w, r)
		return
	}

	if r.Method == http.MethodGet && strings.HasPrefix(r.URL.Path, "/stats/") {
		handleStats(w, r)
		return
	}

	if r.Method == http.MethodGet {
		handleRedirect(w, r)
		return
	}

	http.NotFound(w, r)
}

func codeFromRedirectPath(path string) (string, bool) {
	if !strings.HasPrefix(path, "/") {
		return "", false
	}
	code := strings.TrimPrefix(path, "/")
	if code == "" || strings.Contains(code, "/") {
		return "", false
	}
	return code, true
}

func codeFromStatsPath(path string) (string, bool) {
	const prefix = "/stats/"
	if !strings.HasPrefix(path, prefix) {
		return "", false
	}
	code := strings.TrimPrefix(path, prefix)
	if code == "" || strings.Contains(code, "/") {
		return "", false
	}
	return code, true
}

// ── handlers ──────────────────────────────────────────────────────────────────

func handleRoot(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	total, _ := rdb.Get(ctx, "meta:urls_created").Result()
	redirected, _ := rdb.Get(ctx, "meta:urls_redirected").Result()
	if total == "" {
		total = "0"
	}
	if redirected == "" {
		redirected = "0"
	}
	w.Header().Set("Content-Type", "text/plain")
	fmt.Fprintf(w,
		"miniDB URL shortener\n\n"+
			"POST /shorten          {\"url\":\"https://example.com\"}\n"+
			"GET  /{code}           redirect\n"+
			"GET  /stats/{code}     hit count and metadata\n\n"+
			"URLs created:    %s\n"+
			"Redirects served: %s\n",
		total, redirected,
	)
}

type shortenRequest struct {
	URL string `json:"url"`
}

type shortenResponse struct {
	Code  string `json:"code"`
	Short string `json:"short"`
	URL   string `json:"url"`
}

func handleShorten(w http.ResponseWriter, r *http.Request) {
	ip := clientIP(r)

	remaining, ok, err := checkRate(ip)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "internal error"})
		return
	}
	w.Header().Set("X-RateLimit-Limit", strconv.Itoa(rateLimit))
	w.Header().Set("X-RateLimit-Remaining", strconv.FormatInt(remaining, 10))
	if !ok {
		writeJSON(w, http.StatusTooManyRequests, map[string]string{"error": "rate limit exceeded"})
		return
	}

	var req shortenRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.URL == "" {
		writeJSON(w, http.StatusBadRequest,
			map[string]string{"error": `invalid body; expected {"url":"https://..."}`})
		return
	}
	if !strings.HasPrefix(req.URL, "http://") && !strings.HasPrefix(req.URL, "https://") {
		writeJSON(w, http.StatusBadRequest,
			map[string]string{"error": "url must begin with http:// or https://"})
		return
	}

	code, err := uniqueCode()
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}

	now := time.Now().UTC().Format(time.RFC3339)
	if err := rdb.HSet(ctx, "url:"+code, map[string]any{
		"url":     req.URL,
		"created": now,
		"hits":    0,
	}).Err(); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "storage error"})
		return
	}
	rdb.Incr(ctx, "meta:urls_created")

	scheme := "http"
	if r.TLS != nil {
		scheme = "https"
	}
	writeJSON(w, http.StatusCreated, shortenResponse{
		Code:  code,
		Short: fmt.Sprintf("%s://%s/%s", scheme, r.Host, code),
		URL:   req.URL,
	})
}

func handleRedirect(w http.ResponseWriter, r *http.Request) {
	code, ok := codeFromRedirectPath(r.URL.Path)
	if !ok {
		http.NotFound(w, r)
		return
	}

	target, err := rdb.HGet(ctx, "url:"+code, "url").Result()
	if err == redis.Nil {
		http.NotFound(w, r)
		return
	}
	if err != nil {
		http.Error(w, "internal error", http.StatusInternalServerError)
		return
	}

	// Count the hit asynchronously — don't block the redirect.
	go func() {
		rdb.HIncrBy(ctx, "url:"+code, "hits", 1)
		rdb.Incr(ctx, "meta:urls_redirected")
	}()

	http.Redirect(w, r, target, http.StatusFound)
}

type statsResponse struct {
	Code    string `json:"code"`
	URL     string `json:"url"`
	Hits    int    `json:"hits"`
	Created string `json:"created"`
	Short   string `json:"short"`
}

func handleStats(w http.ResponseWriter, r *http.Request) {
	code, ok := codeFromStatsPath(r.URL.Path)
	if !ok {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "not found"})
		return
	}

	data, err := rdb.HGetAll(ctx, "url:"+code).Result()
	if err != nil || len(data) == 0 {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "not found"})
		return
	}

	hits, _ := strconv.Atoi(data["hits"])
	scheme := "http"
	if r.TLS != nil {
		scheme = "https"
	}
	writeJSON(w, http.StatusOK, statsResponse{
		Code:    code,
		URL:     data["url"],
		Hits:    hits,
		Created: data["created"],
		Short:   fmt.Sprintf("%s://%s/%s", scheme, r.Host, code),
	})
}