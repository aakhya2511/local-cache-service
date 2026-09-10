#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root_dir}/build-release"
socket_path="${TMPDIR:-/tmp}/local_cache_benchmark_$$.sock"

cmake -S "${root_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release -DLCS_BUILD_TESTS=OFF
cmake --build "${build_dir}" --parallel

"${build_dir}/cache_server" --socket "${socket_path}" --workers "${LCS_WORKERS:-8}" \
  --max-entries "${LCS_MAX_ENTRIES:-250000}" --max-cache-bytes "${LCS_CACHE_BYTES:-256m}" &
server_pid=$!
cleanup() {
  kill -TERM "${server_pid}" 2>/dev/null || true
  wait "${server_pid}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

for _ in $(seq 1 100); do
  [[ -S "${socket_path}" ]] && break
  sleep 0.05
done

"${build_dir}/cache_benchmark" --socket "${socket_path}" --clients "${LCS_CLIENTS:-100}" \
  --pipeline "${LCS_PIPELINE:-1}" --requests "${LCS_REQUESTS:-100000}" \
  --key-space "${LCS_KEY_SPACE:-10000}" --read-percent "${LCS_READ_PERCENT:-95}" \
  --put-percent "${LCS_PUT_PERCENT:-5}"
