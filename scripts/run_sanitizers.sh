#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

run_build() {
  local name="$1"
  shift
  local build_dir="${root_dir}/build-${name}"
  cmake -S "${root_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Debug "$@"
  cmake --build "${build_dir}" --parallel
  ctest --test-dir "${build_dir}" --output-on-failure
}

run_build asan-ubsan -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
run_build tsan -DENABLE_TSAN=ON
