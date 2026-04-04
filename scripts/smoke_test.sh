#!/usr/bin/env bash
# scripts/smoke_test.sh – run-through of every miniDB command
#
# Usage:
#   ./scripts/smoke_test.sh [PORT]          # default port 6380
#   BINARY=./miniDB_debug ./scripts/smoke_test.sh 7399
#
# Requires: redis-cli (from the redis-tools package)
#   Ubuntu/Debian:  sudo apt install redis-tools
#   macOS:          brew install redis
#
# The script starts the server, exercises every command, checks expected
# responses, then stops the server.  Exit code is 0 on all-pass, 1 on any
# failure.

set -uo pipefail

# ── Config ───────────────────────────────────────────────────────────────────

PORT="${1:-6380}"
HOST="${HOST:-127.0.0.1}"
BINARY="${BINARY:-./miniDB}"
SERVER_PID=""

# ── Colour helpers ────────────────────────────────────────────────────────────

if [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
    CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; CYAN=''; BOLD=''; NC=''
fi

# ── Preconditions ─────────────────────────────────────────────────────────────

if ! command -v redis-cli >/dev/null 2>&1; then
    echo "Error: redis-cli not found."
    echo "Install with:  sudo apt install redis-tools   or   brew install redis"
    exit 1
fi

if [ ! -x "$BINARY" ]; then
    echo "Error: binary '$BINARY' not found or not executable. Run 'make' first."
    exit 1
fi

# ── Counters ──────────────────────────────────────────────────────────────────

PASS=0; FAIL=0; SECTION_FAIL=0

# ── Cleanup ───────────────────────────────────────────────────────────────────

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# ── Helpers ───────────────────────────────────────────────────────────────────

# Run one redis-cli command, return raw output (no trailing newline)
cmd() {
    redis-cli -h "$HOST" -p "$PORT" --no-auth-warning "$@" 2>/dev/null | tr -d '\r'
}

# cmd_raw: same but with --resp2 for exact wire output (for error prefix checks)
cmd_raw() {
    redis-cli -h "$HOST" -p "$PORT" --no-auth-warning --resp2 "$@" 2>/dev/null | tr -d '\r'
}

section() {
    SECTION_FAIL=0
    printf "\n${CYAN}${BOLD}── %s ${NC}\n" "$*"
}

# check DESC EXPECTED ACTUAL
check() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$actual" = "$expected" ]; then
        printf "  ${GREEN}PASS${NC}  %s\n" "$desc"
        PASS=$(( PASS + 1 ))
    else
        printf "  ${RED}FAIL${NC}  %s\n" "$desc"
        printf "        expected │ %s\n" "$expected"
        printf "        actual   │ %s\n" "$actual"
        FAIL=$(( FAIL + 1 ))
        SECTION_FAIL=$(( SECTION_FAIL + 1 ))
    fi
}

# check_contains DESC NEEDLE HAYSTACK
check_contains() {
    local desc="$1" needle="$2" haystack="$3"
    if echo "$haystack" | grep -qF "$needle"; then
        printf "  ${GREEN}PASS${NC}  %s\n" "$desc"
        PASS=$(( PASS + 1 ))
    else
        printf "  ${RED}FAIL${NC}  %s\n" "$desc"
        printf "        looking for │ %s\n" "$needle"
        printf "        in          │ %s\n" "$haystack"
        FAIL=$(( FAIL + 1 ))
        SECTION_FAIL=$(( SECTION_FAIL + 1 ))
    fi
}

# check_int DESC EXPECTED ACTUAL  (numeric comparison, tolerates whitespace)
check_int() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$actual" -eq "$expected" ] 2>/dev/null; then
        printf "  ${GREEN}PASS${NC}  %s\n" "$desc"
        PASS=$(( PASS + 1 ))
    else
        printf "  ${RED}FAIL${NC}  %s\n" "$desc"
        printf "        expected │ %s\n" "$expected"
        printf "        actual   │ %s\n" "$actual"
        FAIL=$(( FAIL + 1 ))
        SECTION_FAIL=$(( SECTION_FAIL + 1 ))
    fi
}

# ── Start server ──────────────────────────────────────────────────────────────

printf "${BOLD}miniDB smoke test${NC}  host=%s  port=%s\n" "$HOST" "$PORT"
printf "binary: %s\n" "$BINARY"

# Kill any leftover instance on this port
pkill -f "$BINARY.*--port $PORT" 2>/dev/null || true
sleep 0.1

"$BINARY" --port "$PORT" --no-persist >/tmp/miniDB_smoke.log 2>&1 &
SERVER_PID=$!

# Wait for the server to be ready (up to 3s)
for i in $(seq 1 30); do
    if cmd PING | grep -q PONG; then break; fi
    sleep 0.1
    if [ "$i" -eq 30 ]; then
        echo "Error: server did not start within 3s."
        cat /tmp/miniDB_smoke.log
        exit 1
    fi
done
printf "server pid=%s  ready\n" "$SERVER_PID"

# ── Flush all keys before starting ───────────────────────────────────────────
# (server started with --no-persist so it's clean, but be explicit)
cmd DEL smoke:str smoke:str2 smoke:nx smoke:app smoke:counter smoke:list \
        smoke:hash smoke:ttl smoke:px smoke:persist smoke:mset1 smoke:mset2 \
        smoke:gs smoke:type:s smoke:type:l smoke:type:h >/dev/null 2>&1 || true

# ═════════════════════════════════════════════════════════════════════════════
section "PING"
check "PING returns PONG"                   "PONG"  "$(cmd PING)"
check "PING with message echoes message"    "hello" "$(cmd PING hello)"

# ═════════════════════════════════════════════════════════════════════════════
section "SET / GET — basic"
cmd SET smoke:str "hello world" >/dev/null
check "GET returns stored value"        "hello world"  "$(cmd GET smoke:str)"
check "GET on missing key returns nil"  ""             "$(cmd GET smoke:missing)"

section "SET — with EX (seconds TTL)"
cmd SET smoke:ttl "temporary" EX 60 >/dev/null
check "GET key with TTL alive"          "temporary"    "$(cmd GET smoke:ttl)"
TTL_VAL=$(cmd TTL smoke:ttl)
check_int "TTL returns positive seconds"  1 "$([ "$TTL_VAL" -gt 0 ] && echo 1 || echo 0)"

section "SET — with PX (milliseconds TTL)"
cmd SET smoke:px "shortlived" PX 60000 >/dev/null
PTTL_VAL=$(cmd PTTL smoke:px)
check_int "PTTL returns positive ms"    1 "$([ "$PTTL_VAL" -gt 0 ] && echo 1 || echo 0)"

section "SET — with invalid option"
ERR=$(cmd SET smoke:str value BADOPT 99 2>&1)
check_contains "SET with bad option returns error"  "ERR"  "$ERR"

section "SET — NX / XX options"
cmd DEL smoke:setopt smoke:setopt2 >/dev/null
check "SET NX on missing key succeeds"      "OK" "$(cmd SET smoke:setopt first NX)"
check "SET NX on existing key returns nil"  ""   "$(cmd SET smoke:setopt second NX)"
check "SET XX on missing key returns nil"   ""   "$(cmd SET smoke:setopt2 v XX)"
check "SET XX on existing key succeeds"     "OK" "$(cmd SET smoke:setopt changed XX)"
check "SET XX updated value"                "changed" "$(cmd GET smoke:setopt)"

# ═════════════════════════════════════════════════════════════════════════════
section "DEL / EXISTS"
cmd SET smoke:str2 "x" >/dev/null
check "EXISTS on live key returns 1"        "1"  "$(cmd EXISTS smoke:str2)"
check "EXISTS on missing key returns 0"     "0"  "$(cmd EXISTS smoke:missing)"
check "DEL returns count of deleted keys"   "1"  "$(cmd DEL smoke:str2)"
check "GET after DEL returns nil"           ""   "$(cmd GET smoke:str2)"
check "EXISTS after DEL returns 0"          "0"  "$(cmd EXISTS smoke:str2)"

section "DEL — multiple keys"
cmd MSET smoke:d1 a smoke:d2 b smoke:d3 c >/dev/null
check "DEL multiple keys returns count"     "3"  "$(cmd DEL smoke:d1 smoke:d2 smoke:d3)"

# ═════════════════════════════════════════════════════════════════════════════
section "DBSIZE / RENAME / FLUSHALL"
cmd DEL smoke:renA smoke:renB >/dev/null
DBSIZE_BEFORE=$(cmd DBSIZE)
cmd SET smoke:renA hello >/dev/null
DBSIZE_AFTER_SET=$(cmd DBSIZE)
check_int "DBSIZE increments after SET"     1 "$((DBSIZE_AFTER_SET - DBSIZE_BEFORE))"
check "RENAME returns OK"                   "OK" "$(cmd RENAME smoke:renA smoke:renB)"
check "RENAME moved value"                  "hello" "$(cmd GET smoke:renB)"
check "RENAME removed old key"              ""   "$(cmd GET smoke:renA)"
check "FLUSHALL returns OK"                 "OK" "$(cmd FLUSHALL)"
check "DBSIZE after FLUSHALL is zero"       "0"  "$(cmd DBSIZE)"

# Recreate baseline keys after FLUSHALL
cmd SET smoke:str "hello world" >/dev/null
cmd SET smoke:ttl "temporary" EX 60 >/dev/null
cmd SET smoke:px "shortlived" PX 60000 >/dev/null

# ═════════════════════════════════════════════════════════════════════════════
section "MSET / MGET"
cmd MSET smoke:mset1 "alpha" smoke:mset2 "beta" >/dev/null
MGET=$(cmd MGET smoke:mset1 smoke:mset2 smoke:missing)
check_contains "MGET returns first value"   "alpha"  "$MGET"
check_contains "MGET returns second value"  "beta"   "$MGET"
check "MGET nil for missing key"            "" \
    "$(cmd MGET smoke:missing | head -1)"

# ═════════════════════════════════════════════════════════════════════════════
section "SETNX (set if not exists)"
cmd DEL smoke:nx >/dev/null
check "SETNX on new key returns 1"          "1"     "$(cmd SETNX smoke:nx "first")"
check "SETNX on existing key returns 0"     "0"     "$(cmd SETNX smoke:nx "second")"
check "SETNX did not overwrite value"       "first" "$(cmd GET smoke:nx)"

# ═════════════════════════════════════════════════════════════════════════════
section "APPEND"
cmd DEL smoke:app >/dev/null
check "APPEND to new key returns new length"    "5"  "$(cmd APPEND smoke:app "hello")"
check "APPEND to existing key returns length"   "11" "$(cmd APPEND smoke:app " world")"
check "GET after APPEND returns full string"    "hello world" "$(cmd GET smoke:app)"

# ═════════════════════════════════════════════════════════════════════════════
section "GETSET"
cmd SET smoke:gs "old" >/dev/null
check "GETSET returns old value"    "old" "$(cmd GETSET smoke:gs "new")"
check "GET after GETSET is new"     "new" "$(cmd GET smoke:gs)"
check "GETSET on missing key -> nil" ""    "$(cmd GETSET smoke:missing_gs "val")"
cmd DEL smoke:missing_gs >/dev/null

# ═════════════════════════════════════════════════════════════════════════════
section "STRLEN / GETDEL"
cmd SET smoke:slen "hello" >/dev/null
check "STRLEN returns string length"     "5"     "$(cmd STRLEN smoke:slen)"
check "GETDEL returns old string value"  "hello" "$(cmd GETDEL smoke:slen)"
check "GETDEL deletes key"               ""      "$(cmd GET smoke:slen)"
check "STRLEN on missing key returns 0"  "0"     "$(cmd STRLEN smoke:slen)"

# ═════════════════════════════════════════════════════════════════════════════
section "INCR / DECR / INCRBY / DECRBY"
cmd DEL smoke:counter >/dev/null
check "INCR on new key starts at 1"         "1"   "$(cmd INCR smoke:counter)"
check "INCR increments"                     "2"   "$(cmd INCR smoke:counter)"
check "INCRBY adds delta"                   "12"  "$(cmd INCRBY smoke:counter 10)"
check "DECR decrements"                     "11"  "$(cmd DECR smoke:counter)"
check "DECRBY subtracts delta"              "6"   "$(cmd DECRBY smoke:counter 5)"

section "INCR — error cases"
cmd SET smoke:notnum "abc" >/dev/null
ERR=$(cmd INCR smoke:notnum 2>&1)
check_contains "INCR on non-integer returns error"  "ERR"  "$ERR"
cmd SET smoke:type:s "val" >/dev/null

# ═════════════════════════════════════════════════════════════════════════════
section "EXPIRE / TTL / PERSIST"
cmd SET smoke:persist "alive" >/dev/null
check "EXPIRE returns 1 on existing key"    "1"  "$(cmd EXPIRE smoke:persist 60)"
TTL=$(cmd TTL smoke:persist)
check_int "TTL after EXPIRE is positive"    1 "$([ "$TTL" -gt 0 ] && echo 1 || echo 0)"
check "PERSIST removes TTL, returns 1"      "1"  "$(cmd PERSIST smoke:persist)"
check "TTL after PERSIST is -1 (no expiry)" "-1" "$(cmd TTL smoke:persist)"
check "EXPIRE on missing key returns 0"     "0"  "$(cmd EXPIRE smoke:missing_e 60)"
check "TTL on missing key returns -2"       "-2" "$(cmd TTL smoke:missing_e)"
check "TTL on key without expiry is -1"     "-1" "$(cmd TTL smoke:persist)"

section "PEXPIRE / PTTL"
cmd PEXPIRE smoke:persist 30000 >/dev/null
PTTL=$(cmd PTTL smoke:persist)
check_int "PTTL after PEXPIRE is positive"  1 "$([ "$PTTL" -gt 0 ] && echo 1 || echo 0)"
check "PTTL on missing key returns -2"      "-2" "$(cmd PTTL smoke:missing_p)"

# ═════════════════════════════════════════════════════════════════════════════
section "TYPE"
cmd SET  smoke:type:s "str"                           >/dev/null
cmd RPUSH smoke:type:l "item"                         >/dev/null
cmd HSET  smoke:type:h field val                      >/dev/null
check "TYPE on string key"          "string" "$(cmd TYPE smoke:type:s)"
check "TYPE on list key"            "list"   "$(cmd TYPE smoke:type:l)"
check "TYPE on hash key"            "hash"   "$(cmd TYPE smoke:type:h)"
check "TYPE on missing key -> none"  "none"   "$(cmd TYPE smoke:missing_t)"

# ═════════════════════════════════════════════════════════════════════════════
section "KEYS"
cmd MSET keys:a 1 keys:b 2 keys:c 3 >/dev/null
KEYS_ALL=$(cmd KEYS "keys:*")
check_contains "KEYS keys:* finds keys:a"  "keys:a"  "$KEYS_ALL"
check_contains "KEYS keys:* finds keys:b"  "keys:b"  "$KEYS_ALL"
check_contains "KEYS keys:* finds keys:c"  "keys:c"  "$KEYS_ALL"
KEYS_PATTERN=$(cmd KEYS "keys:[ab]")
check_contains "KEYS [ab] finds keys:a"    "keys:a"  "$KEYS_PATTERN"
check_contains "KEYS [ab] finds keys:b"    "keys:b"  "$KEYS_PATTERN"
KEYS_Q=$(cmd KEYS "keys:?")
check_contains "KEYS keys:? matches single char"  "keys:a"  "$KEYS_Q"

section "SCAN"
SCAN_PAGE=$(cmd SCAN 0 MATCH "keys:*" COUNT 2)
SCAN_CURSOR=$(echo "$SCAN_PAGE" | head -n 1)
check_int "SCAN returns cursor line" 1 "$([ -n "$SCAN_CURSOR" ] && echo 1 || echo 0)"
check_contains "SCAN returns keyspace entries" "keys:" "$SCAN_PAGE"
SCAN_ALL=$(cmd SCAN 0 MATCH "keys:*" COUNT 100)
SCAN_ALL_CURSOR=$(echo "$SCAN_ALL" | head -n 1)
check "SCAN full pass cursor is 0" "0" "$SCAN_ALL_CURSOR"
check_contains "SCAN full pass contains keys:a" "keys:a" "$SCAN_ALL"
check_contains "SCAN full pass contains keys:b" "keys:b" "$SCAN_ALL"
check_contains "SCAN full pass contains keys:c" "keys:c" "$SCAN_ALL"

cmd DEL keys:a keys:b keys:c >/dev/null

# ═════════════════════════════════════════════════════════════════════════════
section "LPUSH / RPUSH / LLEN"
cmd DEL smoke:list >/dev/null
check "RPUSH to new list returns 1"     "1" "$(cmd RPUSH smoke:list "a")"
check "RPUSH another element returns 2" "2" "$(cmd RPUSH smoke:list "b")"
check "LPUSH to front returns 3"        "3" "$(cmd LPUSH smoke:list "z")"
check "LLEN is 3"                       "3" "$(cmd LLEN smoke:list)"
check "LLEN on missing key returns 0"   "0" "$(cmd LLEN smoke:missing_l)"

section "RPUSH — multiple values at once"
cmd DEL smoke:list2 >/dev/null
check "RPUSH multi returns count"   "3" "$(cmd RPUSH smoke:list2 x y z)"
cmd DEL smoke:list2 >/dev/null

section "LRANGE"
# smoke:list is now: z a b
check "LRANGE 0 -1 returns all"         "z
a
b" "$(cmd LRANGE smoke:list 0 -1)"
check "LRANGE 0 0 returns first"        "z"   "$(cmd LRANGE smoke:list 0 0)"
check "LRANGE -1 -1 returns last"       "b"   "$(cmd LRANGE smoke:list -1 -1)"
check "LRANGE 1 2 returns middle+last"  "a
b" "$(cmd LRANGE smoke:list 1 2)"
check "LRANGE out of bounds is empty"   ""    "$(cmd LRANGE smoke:list 10 20)"
check "LRANGE on missing key is empty"  ""    "$(cmd LRANGE smoke:missing 0 -1)"

section "LPOP / RPOP"
check "LPOP returns front element"  "z" "$(cmd LPOP smoke:list)"
check "RPOP returns back element"   "b" "$(cmd RPOP smoke:list)"
check "LLEN after 2 pops is 1"      "1" "$(cmd LLEN smoke:list)"
cmd LPOP smoke:list >/dev/null      # drain to empty
check "LPOP on empty list returns nil"  "" "$(cmd LPOP smoke:list)"
check "Key deleted after last pop"      "0" "$(cmd EXISTS smoke:list)"

# ═════════════════════════════════════════════════════════════════════════════
section "HSET / HGET / HLEN"
cmd DEL smoke:hash >/dev/null
check "HSET single field returns 1 (new)"   "1" "$(cmd HSET smoke:hash name Alice)"
check "HSET existing field returns 0"       "0" "$(cmd HSET smoke:hash name Bob)"
check "HGET returns current value"          "Bob" "$(cmd HGET smoke:hash name)"
check "HSET multiple fields returns new count" "2" \
    "$(cmd HSET smoke:hash age 30 city Lisbon)"
check "HLEN returns field count"            "3" "$(cmd HLEN smoke:hash)"
check "HGET missing field returns nil"      ""  "$(cmd HGET smoke:hash missing)"
check "HLEN on missing key returns 0"       "0" "$(cmd HLEN smoke:missing_h)"

section "HDEL"
check "HDEL existing field returns 1"   "1" "$(cmd HDEL smoke:hash city)"
check "HDEL missing field returns 0"    "0" "$(cmd HDEL smoke:hash city)"
check "HLEN after HDEL is 2"            "2" "$(cmd HLEN smoke:hash)"

section "HGETALL"
HGETALL=$(cmd HGETALL smoke:hash)
check_contains "HGETALL contains field name"  "name"  "$HGETALL"
check_contains "HGETALL contains value Bob"   "Bob"   "$HGETALL"
check_contains "HGETALL contains field age"   "age"   "$HGETALL"
check_contains "HGETALL contains value 30"    "30"    "$HGETALL"
HGETALL_MISSING=$(cmd HGETALL smoke:missing_h 2>&1)
check "HGETALL on missing key returns empty array"  "" "$HGETALL_MISSING"

section "HKEYS / HVALS"
HKEYS=$(cmd HKEYS smoke:hash)
check_contains "HKEYS contains field name"  "name"  "$HKEYS"
check_contains "HKEYS contains field age"   "age"   "$HKEYS"
HVALS=$(cmd HVALS smoke:hash)
check_contains "HVALS contains value Bob"   "Bob"   "$HVALS"
check_contains "HVALS contains value 30"    "30"    "$HVALS"
HKEYS_MISSING=$(cmd HKEYS smoke:missing_h 2>&1)
check "HKEYS on missing key returns empty array" "" "$HKEYS_MISSING"

# ═════════════════════════════════════════════════════════════════════════════
section "SETS — SADD / SMEMBERS / SISMEMBER / SCARD"
cmd DEL set1 s1 s2 s3 >/dev/null
check "SADD new" "3" "$(cmd SADD set1 a b c)"
check "SADD existing" "1" "$(cmd SADD set1 c d)"
check_int "SCARD" "4" "$(cmd SCARD set1)"
check "SISMEMBER true" "1" "$(cmd SISMEMBER set1 a)"
check "SISMEMBER false" "0" "$(cmd SISMEMBER set1 z)"
check "SISMEMBER non-existent" "0" "$(cmd SISMEMBER noset a)"
check_int "SCARD non-existent" "0" "$(cmd SCARD noset)"
# Note: SMEMBERS order is non-deterministic (hash iteration), so test length only
check_int "SMEMBERS count" "4" "$(cmd SMEMBERS set1 | wc -w)"

section "SREM"
check "SREM existing" "2" "$(cmd SREM set1 a b)"
check "SREM non-existent member" "0" "$(cmd SREM set1 a)"
check_int "SCARD after srem" "2" "$(cmd SCARD set1)"
check "SREM non-existent set" "0" "$(cmd SREM noset a)"

section "SUNION"
cmd DEL s1 s2 s3 >/dev/null
cmd SADD s1 a b c >/dev/null
cmd SADD s2 c d e >/dev/null
cmd SADD s3 e f g >/dev/null
check_int "SUNION count" "7" "$(cmd SUNION s1 s2 s3 | wc -w)"
check_int "SUNION missing key" "7" "$(cmd SUNION s1 s2 s3 missing | wc -w)"

section "SINTER"
check_int "SINTER empty on missing" "0" "$(cmd SINTER s1 missing | wc -w)"
check_int "SINTER s1 s2 count" "1" "$(cmd SINTER s1 s2 | wc -w)"
check_contains "SINTER s1 s2 contains c" "c" "$(cmd SINTER s1 s2)"
cmd SADD s2 b >/dev/null
check_int "SINTER s1 s2 after add" "2" "$(cmd SINTER s1 s2 | wc -w)"

section "SDIFF"
cmd DEL s1 s2 s3 >/dev/null
cmd SADD s1 a b c d >/dev/null
cmd SADD s2 c d e >/dev/null
cmd SADD s3 b >/dev/null
# SDIFF s1 s2 s3 -> a
check_int "SDIFF count" "1" "$(cmd SDIFF s1 s2 s3 | wc -w)"
check_contains "SDIFF contains a" "a" "$(cmd SDIFF s1 s2 s3)"
check_int "SDIFF with missing" "3" "$(cmd SDIFF s1 missing s3 | wc -w)"

# ═════════════════════════════════════════════════════════════════════════════
section "WRONGTYPE errors"

# smoke:type:s is a string, smoke:type:l is a list, smoke:type:h is a hash

# Setup wrongtype types correctly
cmd SET smoke:type:s "a string" >/dev/null
cmd LPUSH smoke:type:l "a" "list" >/dev/null
cmd HSET smoke:type:h "a" "hash" >/dev/null

ERR_LIST_ON_STR=$(cmd LPUSH smoke:type:s item 2>&1)


check_contains "LPUSH on string key -> WRONGTYPE"    "WRONGTYPE"  "$ERR_LIST_ON_STR"
ERR_STR_ON_LIST=$(cmd GET smoke:type:l 2>&1)
check_contains "GET on list key -> WRONGTYPE"        "WRONGTYPE"  "$ERR_STR_ON_LIST"
ERR_HASH_ON_STR=$(cmd HSET smoke:type:s f v 2>&1)
check_contains "HSET on string key -> WRONGTYPE"     "WRONGTYPE"  "$ERR_HASH_ON_STR"
ERR_INCR_ON_LIST=$(cmd INCR smoke:type:l 2>&1)
check_contains "INCR on list key -> WRONGTYPE"       "WRONGTYPE"  "$ERR_INCR_ON_LIST"

# ═════════════════════════════════════════════════════════════════════════════
section "INFO"
INFO=$(cmd INFO)
check_contains "INFO contains version"           "version:"             "$INFO"
check_contains "INFO contains uptime"            "uptime_in_seconds:"   "$INFO"
check_contains "INFO contains keys"              "keys:"                "$INFO"
check_contains "INFO contains load_factor"       "load_factor:"         "$INFO"
check_contains "INFO contains total_commands"    "total_commands_processed:" "$INFO"

# ═════════════════════════════════════════════════════════════════════════════
section "PING (final connection check)"
check "Server still alive after all tests"  "PONG"  "$(cmd PING)"

# ═════════════════════════════════════════════════════════════════════════════
# Cleanup keys created by this script
cmd DEL smoke:str smoke:ttl smoke:px smoke:nx smoke:app smoke:counter \
        smoke:persist smoke:gs smoke:missing_gs smoke:notnum \
    smoke:type:s smoke:type:l smoke:type:h smoke:list smoke:hash smoke:slen \
    smoke:mset1 smoke:mset2 smoke:setopt smoke:setopt2 smoke:renA smoke:renB >/dev/null 2>&1 || true

# ═════════════════════════════════════════════════════════════════════════════
# Summary
TOTAL=$(( PASS + FAIL ))
printf "\n${BOLD}Results: %d/%d tests passed${NC}\n" "$PASS" "$TOTAL"

if [ "$FAIL" -gt 0 ]; then
    printf "${RED}%d test(s) failed.${NC}\n" "$FAIL"
    printf "Server log: /tmp/miniDB_smoke.log\n"
    exit 1
else
    printf "${GREEN}All tests passed.${NC}\n"
    exit 0
fi