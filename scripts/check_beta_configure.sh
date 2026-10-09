#!/usr/bin/env bash
# Configure AlpacaHTTP (which adds AlpacaCore as a subdirectory, so both
# CMakeLists run once) from a copy of the tree whose VERSION is a beta
# (X.Y.Z~betaN) and check that project() took the numeric part while the
# ALPACACORE_VERSION / ALPACAHTTP_VERSION defines kept the full string
# (docs/beta-channel.md). A project() that received the ~betaN string fails
# the configure itself. CI runs this in build-test and the pre-flight in gate
# 3b; nothing in the checkout is modified and nothing is compiled.
#
# Usage: scripts/check_beta_configure.sh [X.Y.Z~betaN]   (default 5.0.0~beta1)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BETA="${1:-5.0.0~beta1}"
BASE="${BETA%%~*}"
# Same form as VERSION_RE in scripts/changelog_fragments.py.
if ! [[ "${BETA}" =~ ^[0-9]+\.[0-9]+\.[0-9]+~beta[1-9][0-9]*$ ]]; then
  echo "ERROR: '${BETA}' is not X.Y.Z~betaN" >&2
  exit 2
fi

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
git -C "${ROOT_DIR}" archive --format=tar HEAD | tar -xf - -C "${tmp}"
# HEAD is what CI tests; in the local pre-flight an uncommitted edit to the
# version handling must be tested too, so overlay those files when dirty.
for f in AlpacaCore/CMakeLists.txt AlpacaHTTP/CMakeLists.txt; do
  if [ -n "$(git -C "${ROOT_DIR}" status --porcelain -- "${f}")" ]; then
    echo "note: ${f} has uncommitted changes; testing the working copy"
    cp "${ROOT_DIR}/${f}" "${tmp}/${f}"
  fi
done
printf '%s\n' "${BETA}" > "${tmp}/VERSION"

build="${tmp}/build"
if ! cmake -S "${tmp}/AlpacaHTTP" -B "${build}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
     -DALPACACORE_ENABLE_ALL_VENDORS=OFF > "${build}.log" 2>&1; then
  echo "FAIL: AlpacaHTTP (with AlpacaCore) does not configure with VERSION ${BETA}:" >&2
  tail -20 "${build}.log" >&2
  exit 1
fi

fail=0
if ! grep -q "^CMAKE_PROJECT_VERSION:STATIC=${BASE}$" "${build}/CMakeCache.txt"; then
  echo "FAIL: project() version is not ${BASE}:" >&2
  grep '^CMAKE_PROJECT_VERSION' "${build}/CMakeCache.txt" >&2 || true
  fail=1
fi
# Each define carries its own quotes, which compile_commands.json escapes
# (-DALPACACORE_VERSION=\"\\\"5.0.0~beta1\\\"\"), so match the token loosely.
for define in ALPACACORE_VERSION ALPACAHTTP_VERSION; do
  defined="$(grep -o -- "-D${define}=[^ ]*" "${build}/compile_commands.json" | sort -u || true)"
  if [ -z "${defined}" ] || [ "$(printf '%s\n' "${defined}" | wc -l)" -ne 1 ] \
     || [ "${defined#*"${BETA}"}" = "${defined}" ]; then
    echo "FAIL: ${define}=\"${BETA}\" is not defined once (found: ${defined:-nothing})" >&2
    fail=1
  fi
done
if [ "${fail}" -ne 0 ]; then
  exit 1
fi
echo "Beta VERSION configure check OK (${BETA}: project() ${BASE}, both defines keep ${BETA})."
