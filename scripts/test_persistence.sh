#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-7390}"
SNAP="${2:-/tmp/minidb_persist_test.snap}"
LOG1="${3:-/tmp/minidb_persist_test_1.log}"
LOG2="${4:-/tmp/minidb_persist_test_2.log}"

if ! command -v redis-cli >/dev/null 2>&1; then
    echo "redis-cli not found. Install redis-tools first." >&2
    exit 1
fi

if [[ ! -x ./kvstore ]]; then
    echo "kvstore binary not found. Run 'make' first." >&2
    exit 1
fi

pkill -f '^./kvstore' >/dev/null 2>&1 || true
rm -f "${SNAP}" "${SNAP}.tmp" "${LOG1}" "${LOG2}"

cleanup() {
    kill "${PID1:-}" >/dev/null 2>&1 || true
    kill "${PID2:-}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

./kvstore --port "${PORT}" --snapshot "${SNAP}" --save-interval 1 >"${LOG1}" 2>&1 &
PID1=$!
sleep 0.5

redis-cli -p "${PORT}" SET p:key hello >/dev/null
redis-cli -p "${PORT}" LPUSH p:list a b c >/dev/null
redis-cli -p "${PORT}" HSET p:hash f1 v1 f2 v2 >/dev/null
redis-cli -p "${PORT}" SET p:temp bye EX 1 >/dev/null

# Wait for key expiry and periodic snapshot.
sleep 2.2
kill "${PID1}" >/dev/null 2>&1 || true
wait "${PID1}" 2>/dev/null || true

if [[ ! -f "${SNAP}" ]]; then
    echo "FAIL: snapshot was not created at ${SNAP}" >&2
    exit 1
fi

./kvstore --port "${PORT}" --snapshot "${SNAP}" --save-interval 0 >"${LOG2}" 2>&1 &
PID2=$!
sleep 0.5

GET_VAL="$(redis-cli -p "${PORT}" --raw GET p:key)"
L_LEN="$(redis-cli -p "${PORT}" --raw LLEN p:list)"
H_VAL="$(redis-cli -p "${PORT}" --raw HGET p:hash f2)"
TEMP_EXISTS="$(redis-cli -p "${PORT}" --raw EXISTS p:temp)"

kill "${PID2}" >/dev/null 2>&1 || true
wait "${PID2}" 2>/dev/null || true

if [[ "${GET_VAL}" != "hello" ]]; then
    echo "FAIL: GET p:key expected hello, got '${GET_VAL}'" >&2
    exit 1
fi
if [[ "${L_LEN}" != "3" ]]; then
    echo "FAIL: LLEN p:list expected 3, got '${L_LEN}'" >&2
    exit 1
fi
if [[ "${H_VAL}" != "v2" ]]; then
    echo "FAIL: HGET p:hash f2 expected v2, got '${H_VAL}'" >&2
    exit 1
fi
if [[ "${TEMP_EXISTS}" != "0" ]]; then
    echo "FAIL: EXISTS p:temp expected 0, got '${TEMP_EXISTS}'" >&2
    exit 1
fi

if ! grep -q "\[persist\] snapshot loaded" "${LOG2}"; then
    echo "FAIL: second start did not report snapshot load" >&2
    exit 1
fi

echo "Persistence test passed"
echo "  GET p:key => ${GET_VAL}"
echo "  LLEN p:list => ${L_LEN}"
echo "  HGET p:hash f2 => ${H_VAL}"
echo "  EXISTS p:temp => ${TEMP_EXISTS}"
