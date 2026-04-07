#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PAYLOAD="$(printf 'x%.0s' $(seq 1 4096))"

"${ROOT_DIR}/build/mrpc_bench_client" \
  --connections 50 \
  --duration 8 \
  --method echo \
  --payload "${PAYLOAD}" \
  --expect-payload "${PAYLOAD}"
