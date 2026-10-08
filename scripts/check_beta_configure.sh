#!/usr/bin/env bash
# Configure AlpacaCore and AlpacaHTTP from a copy of the tree whose VERSION is
# a beta (X.Y.Z~betaN) and check that project() took the numeric part while the
# ALPACACORE_VERSION / ALPACAHTTP_VERSION defines kept the full string
# (docs/beta-channel.md). CI runs this in build-test; nothing in the checkout
# is modified and nothing is compiled.
#
# Usage: scripts/check_beta_configure.sh [X.Y.Z~betaN]   (default 5.0.0~beta1)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BETA="${1:-5.0.0~beta1}"
BASE="${BETA%%~*}"
case "${BETA}" in
  [0-9]*.[0-9]*.[0-9]*~beta[1-9]*) ;;
  *) echo "ERROR: '${BETA}' is not X.Y.Z~betaN" >&2; exit 2 ;;
esac

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
git -C "${ROOT_DIR}" archive --format=tar HEAD | tar -xf - -C "${tmp}"
printf '%s\n' "${BETA}" > "${tmp}/VERSION"

fail=0
check_tree() {
  local name="$1" define="$2" src="${tmp}/$1" build="${tmp}/build-$1"
  shift 2
  if ! cmake -S "${src}" -B "${build}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$@" > "${build}.log" 2>&1; then
    echo "FAIL: ${name} does not configure with VERSION ${BETA}:" >&2
    tail -20 "${build}.log" >&2
    fail=1
    return
  fi
  if ! grep -q "^CMAKE_PROJECT_VERSION:STATIC=${BASE}$" "${build}/CMakeCache.txt"; then
    echo "FAIL: ${name} project() version is not ${BASE}:" >&2
    grep '^CMAKE_PROJECT_VERSION' "${build}/CMakeCache.txt" >&2 || true
    fail=1
  fi
  # The define carries its own quotes, which compile_commands.json escapes
  # (-DALPACACORE_VERSION=\"\\\"5.0.0~beta1\\\"\"), so match the token loosely.
  local defined
  defined="$(grep -o -- "-D${define}=[^ ]*" "${build}/compile_commands.json" | sort -u || true)"
  if [ -z "${defined}" ] || [ "$(printf '%s\n' "${defined}" | wc -l)" -ne 1 ] \
     || [ "${defined#*"${BETA}"}" = "${defined}" ]; then
    echo "FAIL: ${name} does not define ${define}=\"${BETA}\" (found: ${defined:-nothing})" >&2
    fail=1
  fi
  echo "${name}: project() ${BASE}, ${define}=\"${BETA}\""
}

check_tree AlpacaCore ALPACACORE_VERSION -DALPACACORE_ENABLE_ALL_VENDORS=OFF
check_tree AlpacaHTTP ALPACAHTTP_VERSION -DALPACACORE_ENABLE_ALL_VENDORS=OFF
if [ "${fail}" -ne 0 ]; then
  exit 1
fi
echo "Beta VERSION configure check OK (${BETA})."
