#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
SERVER_BIN="${SERVER_BIN:-${BUILD_DIR}/simple_tcp_server}"
CLIENT_BIN="${CLIENT_BIN:-${BUILD_DIR}/mrpc_bench_client}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-8080}"
ADMIN_PORT="${ADMIN_PORT:-9090}"
IO_THREADS="${IO_THREADS:-8}"
CONNECTIONS="${CONNECTIONS:-200}"
OUTSTANDING_PER_CONNECTION="${OUTSTANDING_PER_CONNECTION:-1}"
PAYLOAD_SIZE="${PAYLOAD_SIZE:-16}"
METHOD="${METHOD:-EchoService.Echo}"
WARMUP_SECONDS="${WARMUP_SECONDS:-2}"
DURATION_SECONDS="${DURATION_SECONDS:-20}"
PERF_FREQ="${PERF_FREQ:-99}"
LABEL="${LABEL:-perf}"
RECONNECT_PER_REQUEST="${RECONNECT_PER_REQUEST:-0}"
OUT_DIR="${OUT_DIR:-${ROOT_DIR}/bench/results/$(date +%Y%m%d-%H%M%S)-${LABEL}}"

payload() {
    local size="$1"
    python3 - "$size" <<'PY'
import sys
size = int(sys.argv[1])
alphabet = "0123456789abcdefghijklmnopqrstuvwxyz"
print((alphabet * ((size // len(alphabet)) + 1))[:size], end="")
PY
}

wait_for_port() {
    local host="$1"
    local port="$2"
    local deadline=$((SECONDS + 10))
    while (( SECONDS < deadline )); do
        if python3 - "$host" "$port" <<'PY' >/dev/null 2>&1
import socket
import sys
host, port = sys.argv[1], int(sys.argv[2])
with socket.create_connection((host, port), timeout=0.25):
    pass
PY
        then
            return 0
        fi
        sleep 0.1
    done
    echo "timed out waiting for ${host}:${port}" >&2
    return 1
}

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
        kill "${SERVER_PID}" >/dev/null 2>&1 || true
        wait "${SERVER_PID}" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

if [[ ! -x "${SERVER_BIN}" ]]; then
    echo "server binary not found or not executable: ${SERVER_BIN}" >&2
    exit 1
fi
if [[ ! -x "${CLIENT_BIN}" ]]; then
    echo "client binary not found or not executable: ${CLIENT_BIN}" >&2
    exit 1
fi
if ! command -v perf >/dev/null 2>&1; then
    echo "perf is required but was not found in PATH" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"
PAYLOAD="$(payload "${PAYLOAD_SIZE}")"
SERVER_LOG="${OUT_DIR}/server.log"
PERF_DATA="${OUT_DIR}/perf.data"
PERF_REPORT="${OUT_DIR}/perf.report.txt"
PERF_SCRIPT="${OUT_DIR}/perf.script.txt"
CLIENT_JSON="${OUT_DIR}/client.json"
METRICS_PROM="${OUT_DIR}/metrics.prom"
COMMANDS_TXT="${OUT_DIR}/commands.txt"

cat >"${COMMANDS_TXT}" <<EOF
Server:
MRPC_IO_THREADS=${IO_THREADS} MRPC_ADMIN_PORT=${ADMIN_PORT} MRPC_ENABLE_REQUEST_TRACE=false MRPC_TRACE_ALL_REQUESTS=false MRPC_SLOW_REQUEST_MS=100000 ${SERVER_BIN}

Client:
${CLIENT_BIN} --host ${HOST} --port ${PORT} --method ${METHOD} --connections ${CONNECTIONS} --duration ${DURATION_SECONDS} --payload <${PAYLOAD_SIZE}B> --expect-payload <${PAYLOAD_SIZE}B> --outstanding-per-connection ${OUTSTANDING_PER_CONNECTION} --report-format json$([[ "${RECONNECT_PER_REQUEST}" == "1" ]] && echo " --reconnect-per-request")

Perf:
perf record -F ${PERF_FREQ} -g -p <server-pid> -o ${PERF_DATA} -- sleep ${DURATION_SECONDS}
EOF

(
    cd "${ROOT_DIR}"
    MRPC_IO_THREADS="${IO_THREADS}" \
    MRPC_ADMIN_PORT="${ADMIN_PORT}" \
    MRPC_ENABLE_REQUEST_TRACE=false \
    MRPC_TRACE_ALL_REQUESTS=false \
    MRPC_SLOW_REQUEST_MS=100000 \
    "${SERVER_BIN}"
) >"${SERVER_LOG}" 2>&1 &
SERVER_PID="$!"

wait_for_port "${HOST}" "${PORT}"
if [[ "${ADMIN_PORT}" != "0" ]]; then
    wait_for_port "${HOST}" "${ADMIN_PORT}"
fi

if (( $(printf "%.0f" "${WARMUP_SECONDS}") > 0 )); then
    "${CLIENT_BIN}" \
        --host "${HOST}" \
        --port "${PORT}" \
        --method "${METHOD}" \
        --connections "${CONNECTIONS}" \
        --duration "${WARMUP_SECONDS}" \
        --payload "${PAYLOAD}" \
        --expect-payload "${PAYLOAD}" \
        --outstanding-per-connection "${OUTSTANDING_PER_CONNECTION}" \
        --report-format json \
        $([[ "${RECONNECT_PER_REQUEST}" == "1" ]] && echo "--reconnect-per-request") \
        >/dev/null
fi

perf record -F "${PERF_FREQ}" -g -p "${SERVER_PID}" -o "${PERF_DATA}" -- sleep "${DURATION_SECONDS}" &
PERF_PID="$!"
sleep 0.3

"${CLIENT_BIN}" \
    --host "${HOST}" \
    --port "${PORT}" \
    --method "${METHOD}" \
    --connections "${CONNECTIONS}" \
    --duration "${DURATION_SECONDS}" \
    --payload "${PAYLOAD}" \
    --expect-payload "${PAYLOAD}" \
    --outstanding-per-connection "${OUTSTANDING_PER_CONNECTION}" \
    --report-format json \
    $([[ "${RECONNECT_PER_REQUEST}" == "1" ]] && echo "--reconnect-per-request") \
    >"${CLIENT_JSON}"

wait "${PERF_PID}"
perf report --stdio --sort comm,dso,symbol -i "${PERF_DATA}" >"${PERF_REPORT}" || true
perf script -i "${PERF_DATA}" >"${PERF_SCRIPT}" || true

if [[ "${ADMIN_PORT}" != "0" ]]; then
    python3 - "${HOST}" "${ADMIN_PORT}" >"${METRICS_PROM}" <<'PY' || true
import socket
import sys
host, port = sys.argv[1], int(sys.argv[2])
request = f"GET /metrics HTTP/1.1\r\nHost: {host}:{port}\r\nConnection: close\r\n\r\n".encode()
with socket.create_connection((host, port), timeout=2.0) as sock:
    sock.sendall(request)
    data = bytearray()
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data.extend(chunk)
_, _, body = bytes(data).partition(b"\r\n\r\n")
sys.stdout.write(body.decode("utf-8", errors="replace"))
PY
fi

if command -v stackcollapse-perf.pl >/dev/null 2>&1 && command -v flamegraph.pl >/dev/null 2>&1; then
    stackcollapse-perf.pl "${PERF_SCRIPT}" | flamegraph.pl >"${OUT_DIR}/flamegraph.svg"
elif [[ -n "${FLAMEGRAPH_DIR:-}" && -x "${FLAMEGRAPH_DIR}/stackcollapse-perf.pl" && -x "${FLAMEGRAPH_DIR}/flamegraph.pl" ]]; then
    "${FLAMEGRAPH_DIR}/stackcollapse-perf.pl" "${PERF_SCRIPT}" |
        "${FLAMEGRAPH_DIR}/flamegraph.pl" >"${OUT_DIR}/flamegraph.svg"
else
    cat >"${OUT_DIR}/flamegraph.README.txt" <<EOF
FlameGraph scripts were not found.

To generate an SVG later:
  stackcollapse-perf.pl ${PERF_SCRIPT} | flamegraph.pl > ${OUT_DIR}/flamegraph.svg

Or set FLAMEGRAPH_DIR to a checkout containing stackcollapse-perf.pl and flamegraph.pl.
EOF
fi

echo "[perf] output directory: ${OUT_DIR}"
echo "[perf] client result: ${CLIENT_JSON}"
echo "[perf] perf report: ${PERF_REPORT}"
