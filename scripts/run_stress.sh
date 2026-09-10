#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root_dir}/build-release"
socket_path="${TMPDIR:-/tmp}/local_cache_stress_$$.sock"
requests="${LCS_STRESS_REQUESTS:-100000}"

cmake -S "${root_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release -DLCS_BUILD_TESTS=OFF
cmake --build "${build_dir}" --parallel
"${build_dir}/cache_server" --socket "${socket_path}" --workers "${LCS_WORKERS:-8}" \
  --max-connections 1024 --max-entries "${requests}" --max-cache-bytes 256m &
server_pid=$!
cleanup() {
  kill -TERM "${server_pid}" 2>/dev/null || true
  wait "${server_pid}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
for _ in $(seq 1 100); do [[ -S "${socket_path}" ]] && break; sleep 0.05; done

"${build_dir}/cache_stress" --socket "${socket_path}" --connections 500 \
  --pipeline 32 --requests "${requests}"
