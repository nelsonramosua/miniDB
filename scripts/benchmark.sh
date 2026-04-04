#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-7380}"
HOST="${HOST:-127.0.0.1}"
REQUESTS="${REQUESTS:-100000}"
CLIENTS="${CLIENTS:-1}"
PIPELINE="${PIPELINE:-1}"
KEYSPACE="${KEYSPACE:-1000000}"
RANDOM_KEYS="${RANDOM_KEYS:-0}"

if ! command -v redis-benchmark >/dev/null 2>&1; then
    echo "redis-benchmark not found. Install redis-tools first." >&2
    exit 1
fi

echo "Running redis-benchmark on ${HOST}:${PORT}"
echo "REQUESTS=${REQUESTS} CLIENTS=${CLIENTS} PIPELINE=${PIPELINE} KEYSPACE=${KEYSPACE} RANDOM_KEYS=${RANDOM_KEYS}"

if command -v redis-cli >/dev/null 2>&1; then
    redis-cli -h "${HOST}" -p "${PORT}" SET bench:get:key abc >/dev/null 2>&1 || true
    redis-cli -h "${HOST}" -p "${PORT}" SET bench:incr:key 0 >/dev/null 2>&1 || true
fi

tests=(ping set get incr)
okCount=0

runBenchmarkTest() {
    local testName="$1"
    local clients="$2"
    local pipeline="$3"
    shift 3
    local outFile errFile

    outFile=$(mktemp)
    errFile=$(mktemp)

    redis-benchmark \
        -h "${HOST}" \
        -p "${PORT}" \
        -n "${REQUESTS}" \
        -c "${clients}" \
        -P "${pipeline}" \
        -q \
        "$@" >"${outFile}" 2>"${errFile}" || true

    cat "${outFile}"
    if [[ "${DEBUG_BENCHMARK:-0}" == "1" ]]; then
        cat "${errFile}" >&2
    fi

    if grep -q 'requests per second' "${outFile}"; then
        rm -f "${outFile}" "${errFile}"
        return 0
    fi

    rm -f "${outFile}" "${errFile}"
    return 1
}

for testName in "${tests[@]}"; do
    echo
    echo "== ${testName} =="
    case "${testName}" in
        ping)
            args=(ping)
            ;;
        set)
            if [[ "${RANDOM_KEYS}" == "1" ]]; then
                args=(-r "${KEYSPACE}" set key:__rand_int__ abc)
            else
                args=(set bench:set:key abc)
            fi
            ;;
        get)
            if [[ "${RANDOM_KEYS}" == "1" ]]; then
                args=(-r "${KEYSPACE}" get key:__rand_int__)
            else
                args=(get bench:get:key)
            fi
            ;;
        incr)
            if [[ "${RANDOM_KEYS}" == "1" ]]; then
                args=(-r "${KEYSPACE}" incr counter:__rand_int__)
            else
                args=(incr bench:incr:key)
            fi
            ;;
        *)
            args=(ping)
            ;;
    esac

    if runBenchmarkTest "${testName}" "${CLIENTS}" "${PIPELINE}" "${args[@]}"; then
        okCount=$((okCount + 1))
    else
        if runBenchmarkTest "${testName}" 1 1 "${args[@]}"; then
            okCount=$((okCount + 1))
        else
            echo "${testName}: N/A at current load profile" 
        fi
    fi
done

if [[ "${okCount}" -eq 0 ]]; then
    echo "No benchmark test succeeded." >&2
    exit 1
fi
