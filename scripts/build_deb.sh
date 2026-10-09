#!/usr/bin/env bash
# Build the AlpacaBridge .deb.
#
# debian/changelog is a generated build artifact (gitignored), derived from
# the root CHANGELOG.md so release history is maintained in exactly one
# place. dpkg-buildpackage reads debian/changelog before debian/rules runs,
# so it must be generated here, not inside the rules file.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

PACKAGE="alpacabridge"
MAINTAINER="OpenAstro <support@openastro.net>"
VERSION="$(tr -d '[:space:]' < VERSION)"

if [ -z "${VERSION}" ]; then
    echo "ERROR: VERSION file is empty" >&2
    exit 1
fi

# A beta (X.Y.Z~betaN) has no dated CHANGELOG section; its stanza is dated
# from the README badge (the day the beta was cut) so the build is reproducible.
DATE_ARGS=()
case "${VERSION}" in
  *~beta*)
    BADGE_DATE="$(python3 -c '
import re, sys
m = re.search(r"^####\s*\[[^\]]+\]\s*-\s*(\d{4}-\d{2}-\d{2})\s*&middot;", open("README.md", encoding="utf-8").read(), re.M)
sys.exit("README.md has no dated version badge line") if not m else print(m.group(1))')"
    DATE_ARGS=(--date "${BADGE_DATE}")
    ;;
esac

echo "[STEP] Generating debian/changelog from CHANGELOG.md (version ${VERSION})..."
python3 scripts/changelog_to_deb.py \
    --changelog CHANGELOG.md \
    --out debian/changelog \
    --package "${PACKAGE}" \
    --version "${VERSION}" \
    --maintainer "${MAINTAINER}" \
    "${DATE_ARGS[@]}"

dpkg-parsechangelog -l debian/changelog --all >/dev/null
echo "[STEP] debian/changelog generated and parses cleanly."

echo "[STEP] Building .deb with dpkg-buildpackage..."
dpkg-buildpackage -us -uc -b "$@"

echo "[DONE] Package built. See ../${PACKAGE}_${VERSION}_$(dpkg --print-architecture).deb (or ../ for artifacts)."
