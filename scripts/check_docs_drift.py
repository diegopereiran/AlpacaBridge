#!/usr/bin/env python3
"""Fail when the repo's own docs/config disagree with each other or with reality.

None of these facts are cross-checked anywhere else, so each has drifted
silently in the past (see AGENTS.md's own admissions and the 2026-09
harness-readiness evaluation that prompted this script). Every check below
is read-only and file-local -- no network, no build.

Run from the repo root:  python3 scripts/check_docs_drift.py

Checks:
  1. Every ALPACACORE_ENABLE_* CMake option is documented in the
     docs/development.md build-options table.
  2. The zizmor pinned version + sha256 are identical between ci.yml and
     ci_preflight.sh.
  3. The cppcheck --suppress list is identical between ci.yml and
     ci_preflight.sh (AGENTS.md: "Keep the cppcheck --suppress list
     identical between ci.yml and ci_preflight.sh").
  4. VERSION matches the version in the README's changelog badge line.
  5. Every relative path referenced in AGENTS.md's inline code spans
     (`` `AlpacaCore/...` ``, `` `scripts/...` ``, `` `docs/...` ``, etc.)
     that looks like a real repo path actually exists.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read(path):
    return (ROOT / path).read_text(encoding="utf-8", errors="replace")


# --- check 1: CMake options vs docs/development.md --------------------------

CMAKE_OPTION_RE = re.compile(r"option\((ALPACACORE_ENABLE_\w+)\b")
DOCS_OPTION_RE = re.compile(r"`(ALPACACORE_ENABLE_\w+)`")


def check_cmake_options_documented():
    failures = []
    cmake_text = read("AlpacaCore/CMakeLists.txt")
    cmake_options = set(CMAKE_OPTION_RE.findall(cmake_text))

    docs_text = read("docs/development.md")
    docs_options = set(DOCS_OPTION_RE.findall(docs_text))

    missing = sorted(cmake_options - docs_options)
    for opt in missing:
        failures.append(
            "docs/development.md build-options table is missing %s "
            "(defined in AlpacaCore/CMakeLists.txt)" % opt
        )
    return failures


# --- check 2: zizmor pin sync ------------------------------------------------

def check_zizmor_pin_sync():
    failures = []
    ci = read(".github/workflows/ci.yml")
    preflight = read("scripts/ci_preflight.sh")

    # ci.yml has more than one `ver=`/`sha256=` shell pin (cppcheck's
    # from-source build has its own); scope to the "Install zizmor" step so
    # this doesn't accidentally read cppcheck's pin instead.
    start = ci.find("- name: Install zizmor")
    if start == -1:
        failures.append("could not find the 'Install zizmor' step in ci.yml")
        return failures
    end = ci.find("- name:", start + len("- name: Install zizmor"))
    ci_block = ci[start:end if end != -1 else len(ci)]

    ci_ver = re.search(r"^\s*ver=([0-9.]+)\s*$", ci_block, re.MULTILINE)
    ci_sha = re.search(r"^\s*sha256=([0-9a-f]{64})\s*$", ci_block, re.MULTILINE)
    pf_ver = re.search(r'^ZIZMOR_VER="([0-9.]+)"', preflight, re.MULTILINE)
    pf_sha = re.search(r'^ZIZMOR_SHA256="([0-9a-f]{64})"', preflight, re.MULTILINE)

    if not (ci_ver and ci_sha and pf_ver and pf_sha):
        failures.append(
            "could not find zizmor ver/sha256 pins in both ci.yml (zizmor "
            "job) and ci_preflight.sh (ZIZMOR_VER/ZIZMOR_SHA256) -- update "
            "this check's regexes if the format changed"
        )
        return failures

    if ci_ver.group(1) != pf_ver.group(1):
        failures.append(
            "zizmor version mismatch: ci.yml has %s, ci_preflight.sh has %s"
            % (ci_ver.group(1), pf_ver.group(1))
        )
    if ci_sha.group(1) != pf_sha.group(1):
        failures.append(
            "zizmor sha256 mismatch: ci.yml has %s, ci_preflight.sh has %s"
            % (ci_sha.group(1), pf_sha.group(1))
        )
    return failures


# --- check 3: cppcheck --suppress list sync ---------------------------------

SUPPRESS_RE = re.compile(r"--suppress=(\S+)")


def check_cppcheck_suppress_sync():
    failures = []
    ci = read(".github/workflows/ci.yml")
    preflight = read("scripts/ci_preflight.sh")

    ci_suppress = SUPPRESS_RE.findall(ci)
    pf_suppress = SUPPRESS_RE.findall(preflight)

    if not ci_suppress or not pf_suppress:
        failures.append(
            "could not find --suppress= flags in both ci.yml and "
            "ci_preflight.sh -- update this check's regex if the cppcheck "
            "invocation changed"
        )
        return failures

    if set(ci_suppress) != set(pf_suppress):
        failures.append(
            "cppcheck --suppress list differs: ci.yml has %s, "
            "ci_preflight.sh has %s"
            % (sorted(set(ci_suppress)), sorted(set(pf_suppress)))
        )
    return failures


# --- check 4: VERSION vs README badge ---------------------------------------

def check_version_matches_readme():
    failures = []
    version = read("VERSION").strip()
    readme = read("README.md")

    m = re.search(r"^####\s*\[([0-9.]+)\]\s*-\s*[0-9-]+\s*&middot;\s*\[Changelog\]", readme, re.MULTILINE)
    if not m:
        failures.append("could not find the version badge line in README.md")
        return failures

    badge_version = m.group(1)
    if badge_version != version:
        failures.append(
            "VERSION (%s) does not match the README badge version (%s)"
            % (version, badge_version)
        )
    return failures


# --- check 5: AGENTS.md path references exist -------------------------------

# Backtick-quoted spans that look like a repo-relative path: start with one of
# these top-level dirs/files, contain no spaces, and are not a bare CLI flag
# or a URL.
PATH_PREFIXES = (
    "AlpacaCore/", "AlpacaHTTP/", "scripts/", "docs/", ".github/",
    ".claude/", "debian/",
)
CODE_SPAN_RE = re.compile(r"`([^`\s]+)`")
# Trailing punctuation/anchors that can ride along inside a backtick span.
TRIM_SUFFIX_RE = re.compile(r"[),.;:]+$")


def check_agents_md_paths_exist():
    failures = []
    text = read("AGENTS.md")
    seen = set()

    for m in CODE_SPAN_RE.finditer(text):
        span = m.group(1)
        if not span.startswith(PATH_PREFIXES):
            continue
        path = TRIM_SUFFIX_RE.sub("", span)
        # Markdown anchors / fragments (`docs/x.md#section`), glob patterns,
        # and template placeholders (`AlpacaCore/src/vendors/<vendor>/...`)
        # aren't real filesystem paths. Generated build output (`.../build/...`)
        # is real but never committed, so it can't be checked this way either.
        if "#" in path or "*" in path or "<" in path or ">" in path:
            continue
        if "/build/" in path or path.startswith("build/"):
            continue
        if path in seen:
            continue
        seen.add(path)
        if not (ROOT / path).exists():
            failures.append("AGENTS.md references a path that does not exist: %s" % path)
    return failures


CHECKS = [
    ("CMake options documented in docs/development.md", check_cmake_options_documented),
    ("zizmor pin sync (ci.yml vs ci_preflight.sh)", check_zizmor_pin_sync),
    ("cppcheck --suppress sync (ci.yml vs ci_preflight.sh)", check_cppcheck_suppress_sync),
    ("VERSION matches README badge", check_version_matches_readme),
    ("AGENTS.md path references exist", check_agents_md_paths_exist),
]


def main():
    all_failures = []
    for name, fn in CHECKS:
        failures = fn()
        if failures:
            print("[FAIL] %s" % name)
            for f in failures:
                print("  - %s" % f)
        else:
            print("[PASS] %s" % name)
        all_failures.extend(failures)

    if all_failures:
        print("\n%d finding(s)." % len(all_failures))
        return 1

    print("\nDocs drift check OK.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
