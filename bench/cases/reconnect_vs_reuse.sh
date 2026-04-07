#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLIENT="${ROOT_DIR}/build/mrpc_bench_client"

echo "== connection reuse =="
"${CLIENT}" --connections 20 --duration 8 --method echo --payload "hello rpc" --expect-payload "hello rpc"

echo
echo "== reconnect per request =="
"${CLIENT}" --connections 20 --duration 8 --method echo --payload "hello rpc" --expect-payload "hello rpc" --reconnect-per-request
