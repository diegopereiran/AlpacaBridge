#!/usr/bin/env bash
# Acceptance checks for the bundled libgphoto2, run against an INSTALLED
# alpacabridge package (CI `package-deb` job, or the rig):
#
#   scripts/check_bundled_gphoto.sh [--pid N]
#
# Each check prints PASS or FAIL with the raw command output; the exit status is
# non-zero when any check failed. --pid N (rig use) additionally checks that the
# running server mapped libgphoto2 only from the bundle.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
eval "$("${REPO_ROOT}/scripts/build_bundled_gphoto.sh" --print-pins)"

SERVER=/usr/bin/alpacabridge
BUNDLE_LIB="${GPHOTO_PREFIX}/lib"
PTP2="$(find "${BUNDLE_LIB}/libgphoto2" -name ptp2.so 2>/dev/null | head -n 1)"
RULES=/usr/lib/udev/rules.d/61-alpacabridge-libgphoto2.rules
PID=""
ALLOWED_RPATH_ROOT=/usr/lib/alpacabridge

while [ $# -gt 0 ]; do
  case "$1" in
    --pid)
      [ $# -ge 2 ] && [ -n "$2" ] || { echo "--pid needs a process id" >&2; exit 2; }
      PID="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

failed=0
indent() { sed 's/^/    /'; }
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; failed=1; }

# Every libgphoto2 library `ldd` resolves for $1 must live under the bundle, and
# both sonames must be present.
check_ldd() {
  local target="$1" out bad
  if [ ! -e "${target}" ]; then fail "ldd ${target}: file is missing"; return; fi
  out="$(ldd "${target}" 2>&1)"
  echo "${out}" | grep -i 'libgphoto2' | sed 's/^/    /'
  bad="$(echo "${out}" | grep -i 'libgphoto2' | grep -vF "${BUNDLE_LIB}/" || true)"
  if echo "${out}" | grep -q 'libgphoto2.so.6 => '"${BUNDLE_LIB}"/ \
     && echo "${out}" | grep -q 'libgphoto2_port.so.12 => '"${BUNDLE_LIB}"/ \
     && [ -z "${bad}" ]; then
    pass "ldd ${target}: libgphoto2.so.6 and libgphoto2_port.so.12 resolve under ${BUNDLE_LIB}/"
  else
    fail "ldd ${target}: libgphoto2 resolves outside ${BUNDLE_LIB}/ or is missing"
  fi
}

# A library or camlib does not list libgphoto2.so.6 as its own dependency (ldd of
# libgphoto2.so.6 omits itself), so require only libgphoto2_port.
check_ldd_port() {
  local target="$1" out bad
  if [ -z "${target}" ] || [ ! -e "${target}" ]; then fail "ldd of ${target:-the camlib}: file is missing"; return; fi
  out="$(ldd "${target}" 2>&1)"
  echo "${out}" | grep -i 'libgphoto2' | sed 's/^/    /'
  bad="$(echo "${out}" | grep -i 'libgphoto2' | grep -vF "${BUNDLE_LIB}/" || true)"
  if echo "${out}" | grep -q 'libgphoto2_port.so.12 => '"${BUNDLE_LIB}"/ && [ -z "${bad}" ]; then
    pass "ldd ${target}: libgphoto2 libraries resolve under ${BUNDLE_LIB}/"
  else
    fail "ldd ${target}: libgphoto2 resolves outside ${BUNDLE_LIB}/ or is missing"
  fi
}

# The dynamic section must carry a DT_RPATH naming the bundle (not only
# DT_RUNPATH, which the loader does not apply to transitive lookups) and no
# RPATH/RUNPATH entry outside /usr/lib/alpacabridge.
check_rpath() {
  local target="$1" out entries stray
  if [ ! -e "${target}" ]; then fail "readelf ${target}: file is missing"; return; fi
  out="$(readelf -d "${target}" 2>&1)"
  entries="$(echo "${out}" | grep -E '\((RPATH|RUNPATH)\)' || true)"
  indent <<< "${entries}"
  # Split on ':' keeping empty entries: glibc reads an empty entry as the current
  # directory. An entry with '..' could climb out of the prefix.
  stray="$(echo "${entries}" | grep -oE '\[[^]]*\]' | tr -d '[]' \
           | awk -F: -v pre="${ALLOWED_RPATH_ROOT}" '
               function inside(d) { return index(d, pre) == 1 && (length(d) == length(pre) || substr(d, length(pre) + 1, 1) == "/") }
               $0 == "" { print "<empty>"; next }
               { for (i = 1; i <= NF; i++)
                   if ($i == "") print "<empty>"
                   else if ($i ~ /\.\./ || !inside($i)) print $i }
               END { print "@@end" }')"
  # A failed awk prints no sentinel: treat that as a stray entry, never as clean.
  if [ "${stray##*$'\n'}" = "@@end" ]; then stray="${stray%@@end}"; stray="${stray%$'\n'}"
  else stray="<could not parse the dynamic section>"; fi
  if echo "${entries}" | grep '(RPATH)' | grep -qF "${BUNDLE_LIB}" && [ -z "${stray}" ]; then
    pass "readelf -d ${target}: (RPATH) names ${BUNDLE_LIB}, nothing outside /usr/lib/alpacabridge"
  else
    fail "readelf -d ${target}: no (RPATH) naming ${BUNDLE_LIB}, or a stray entry: ${stray}"
  fi
}

# 1. The server resolves libgphoto2 from the bundle.
check_ldd "${SERVER}"
# 2. The bundle's own library and camlib do too.
check_ldd_port "${BUNDLE_LIB}/libgphoto2.so.6"
check_ldd_port "${PTP2}"
# 3. The loader path is a DT_RPATH.
check_rpath "${SERVER}"
check_rpath "${BUNDLE_LIB}/libgphoto2.so.6"

# 5. udev rules generated from the bundled build.
if [ -f "${RULES}" ] \
   && grep -q '\*:060101:\*' "${RULES}" && grep -q 'GROUP="plugdev"' "${RULES}" \
   && head -n 3 "${RULES}" | grep -qF "libgphoto2 ${LIBGPHOTO2_VER}"; then
  pass "${RULES}: PTP interface-class line, GROUP=\"plugdev\", header names libgphoto2 ${LIBGPHOTO2_VER}"
else
  fail "${RULES}: missing, or no *:060101:* line, no GROUP=\"plugdev\", or no libgphoto2 ${LIBGPHOTO2_VER} header"
  head -n 3 "${RULES}" 2>&1 | sed 's/^/    /'
fi

# 6. The package no longer depends on the distro's libgphoto2.
if depends="$(dpkg-query -W -f '${Depends}' alpacabridge 2>&1)"; then dpkg_ok=1; else dpkg_ok=0; fi
echo "    Depends: ${depends}"
if [ "${dpkg_ok}" -eq 0 ]; then
  fail "dpkg-query could not read the alpacabridge package: ${depends}"
elif echo "${depends}" | grep -qE 'libgphoto2-6|libgphoto2-port12'; then
  fail "dpkg-query Depends names a distro libgphoto2 package"
else
  pass "dpkg-query Depends names neither libgphoto2-6t64 nor libgphoto2-port12t64"
fi

# 7. The bundle directory is not registered with ldconfig.
shopt -s nullglob
confs=(/etc/ld.so.conf /etc/ld.so.conf.d/*)
shopt -u nullglob
hits="$(grep -n 'libgphoto2' "${confs[@]}" 2>/dev/null || true)"
if [ -n "${hits}" ]; then
  indent <<< "${hits}"
  fail "ld.so.conf mentions libgphoto2: the bundle would shadow the distro library for every program"
else
  pass "no ld.so.conf entry mentions libgphoto2"
fi

# 8. Rig use: the running server maps libgphoto2 only from the bundle.
if [ -n "${PID}" ]; then
  maps="$(grep libgphoto2 "/proc/${PID}/maps" 2>&1 | awk '{print $6}' | sort -u)"
  indent <<< "${maps}"
  if [ -n "${maps}" ] && ! echo "${maps}" | grep -vF "${GPHOTO_PREFIX}/" | grep -q .; then
    pass "/proc/${PID}/maps: libgphoto2 mapped only from ${GPHOTO_PREFIX}/"
  else
    fail "/proc/${PID}/maps: no libgphoto2 mapping, or one outside ${GPHOTO_PREFIX}/"
  fi
fi

exit "${failed}"
