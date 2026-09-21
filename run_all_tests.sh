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

# ctest gets its own parallelism, deliberately above the core count. The suite
# is dominated by driver cases that wait on fake hardware in real time rather
# than computing: the slowest single case (a SkyWatcher pole slew) measured
# 110.5s wall for 0.13s user + 0.32s sys, so pinning ctest to nproc leaves the
# machine idle. Measured on ubuntu-24.04-arm (4 cores, the CI runner), all
# vendors, 802 cases: -j 4 => 488.9s, -j 8 => 252.0s.
#
# Twice the core count rather than a fixed number, because what governs is the
# oversubscription RATIO, not the absolute -j: a fixed 8 would carry a 4x ratio
# onto any 2-core box, while 2x nproc does not.
#
# Oversubscription is what surfaces a test whose assertion depends on the
# scheduler rather than on behaviour. `HostClock - readers in flight survive a
# concurrent set_hooks` was one: its reader threads checked the stop flag before
# their first read, so under enough contention the writer loop finished before
# any reader was scheduled and the `reads > 0` vacuity guard went red on an
# otherwise healthy tree -- 3 runs in 10 at -j 8 under ASan+UBSan on a 4-core
# box. That case now starts each reader with one unconditional read and waits
# for all four before the writer loop, which is a property of the case and not
# of the -j it runs at. Raising this multiplier further is a separate change
# and wants its own measurement: the numbers above are all-vendors and the
# -j 8 evidence for that fix is vendors-OFF under sanitizers.
#
# CTEST_PARALLEL overrides, so a slower or busier machine can dial it back
# without a code change. The BUILD stays at nproc below: compiling is CPU-bound.
CTEST_PARALLEL="${CTEST_PARALLEL:-$((PARALLEL * 2))}"

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
