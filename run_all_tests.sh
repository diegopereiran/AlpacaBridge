#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CORE_DIR="${ROOT_DIR}/AlpacaCore"
HTTP_DIR="${ROOT_DIR}/AlpacaHTTP"
CORE_VENDORS="${ALPACACORE_ENABLE_ALL_VENDORS:-ON}"

if [[ ! -d "${CORE_DIR}" ]]; then
  echo "AlpacaCore not found at ${CORE_DIR}"
  exit 1
fi

if [[ ! -d "${HTTP_DIR}" ]]; then
  echo "AlpacaHTTP not found at ${HTTP_DIR}"
  exit 1
fi

rm -rf "${CORE_DIR}/build" "${HTTP_DIR}/build"

if [[ "${OSTYPE:-}" == "darwin"* ]]; then
  PARALLEL="$(sysctl -n hw.ncpu)"
elif command -v nproc >/dev/null 2>&1; then
  PARALLEL="$(nproc)"
else
  PARALLEL="4"
fi

# PROBE (throwaway branch, do not merge): the SkyWatcher fake-mount cases wait
# on simulated motion in real time -- the slowest measured 110.5s wall for
# 0.13s user + 0.32s sys -- so ctest is sleep-bound and tying its -j to nproc
# leaves the runner idle. Build stays at nproc (compiling IS cpu-bound).
CTEST_PARALLEL="${CTEST_PARALLEL:-8}"

echo "== AlpacaCore =="
cmake -S "${CORE_DIR}" -B "${CORE_DIR}/build" \
  -DALPACACORE_BUILD_TESTS=ON \
  -DALPACACORE_ENABLE_ALL_VENDORS="${CORE_VENDORS}"
cmake --build "${CORE_DIR}/build" --parallel "${PARALLEL}"
# --no-tests=error: ctest exits 0 when it finds NO tests, so a suite that
# silently failed to configure (Catch2 missing => AlpacaCore/tests/
# CMakeLists.txt returns early) passed vacuously here, in CI and in
# ci_preflight.sh alike (issue #586). Requires CMake >= 3.18.
ctest --test-dir "${CORE_DIR}/build" --output-on-failure --no-tests=error -j "${CTEST_PARALLEL}"

echo "== AlpacaHTTP =="
# AlpacaHTTP adds AlpacaCore as a subdirectory (AlpacaHTTP/CMakeLists.txt),
# and ALPACACORE_BUILD_TESTS defaults ON -- so without this the whole core
# suite is configured, built and run a SECOND time here (issue #586 made
# that visible: 802 cases in the core run, then 809 = 802 + 7 again).
cmake -S "${HTTP_DIR}" -B "${HTTP_DIR}/build" \
  -DALPACAHTTP_BUILD_TESTS=ON \
  -DALPACACORE_BUILD_TESTS=OFF \
  -DALPACACORE_ENABLE_ALL_VENDORS="${CORE_VENDORS}"
cmake --build "${HTTP_DIR}/build" --parallel "${PARALLEL}"
ctest --test-dir "${HTTP_DIR}/build" --output-on-failure --no-tests=error -j "${CTEST_PARALLEL}"
