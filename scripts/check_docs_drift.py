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
import subprocess
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
    ci_block = _scoped_block(ci, "- name: Install zizmor", ("- name:",))
    if ci_block is None:
        failures.append("could not find the 'Install zizmor' step in ci.yml")
        return failures

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


def _scoped_block(text, start_marker, end_markers):
    """text from start_marker to the first of end_markers found after it (or EOF).

    Used to scope a regex scan to one CI step/gate instead of the whole file,
    the same way check_zizmor_pin_sync does -- a file-wide scan is only safe
    while the flag being matched (--suppress=, ver=, ...) appears nowhere
    else in the file, which is an assumption worth pinning down rather than
    leaving implicit.
    """
    start = text.find(start_marker)
    if start == -1:
        return None
    end = len(text)
    for marker in end_markers:
        pos = text.find(marker, start + len(start_marker))
        if pos != -1:
            end = min(end, pos)
    return text[start:end]


def check_cppcheck_suppress_sync():
    failures = []
    ci_full = read(".github/workflows/ci.yml")
    preflight_full = read("scripts/ci_preflight.sh")

    # Both files currently have exactly one --suppress= invocation, so a
    # whole-file scan happens to be equivalent to a scoped one today -- but
    # scope explicitly anyway (mirroring check_zizmor_pin_sync) so this stays
    # correct if a second tool with its own --suppress flag is ever added to
    # either file.
    ci = _scoped_block(ci_full, "- name: Analyze changed C/C++ files", ("\n  zizmor:",))
    preflight = _scoped_block(preflight_full, 'section "cppcheck (changed files)"', ('section "',))
    if ci is None or preflight is None:
        failures.append(
            "could not find the cppcheck step in ci.yml or the cppcheck "
            "gate in ci_preflight.sh -- update this check's markers if "
            "either file's structure changed"
        )
        return failures

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
# these top-level dirs/files (spaces allowed only for a verbatim tracked path), and are not a bare CLI flag
# or a URL.
PATH_PREFIXES = (
    "AlpacaCore/", "AlpacaHTTP/", "scripts/", "docs/", ".github/",
    ".claude/", "debian/",
)
# Spans are matched only after fenced code blocks are removed (their triple
# backticks would otherwise pair across lines and invert every later match,
# which silently dropped 66 of 83 path references in the first cut of this
# fix). With fences gone, pairing is consistent even for a span that wraps
# onto the next line; such a span is skipped, since a path never wraps.
# Fences: three or more backticks or tildes, optionally indented (list items).
FENCED_BLOCK_RE = re.compile(r"^[ \t]*(`{3,}|~{3,}).*?^[ \t]*\1[ \t]*$", re.S | re.M)
# A double-backtick span shows a literal backtick (`` ` ``) and would break
# single-backtick parity; it is never a path reference, so drop it first.
DOUBLE_BACKTICK_SPAN_RE = re.compile(r"``.+?``")
CODE_SPAN_RE = re.compile(r"`([^`]+)`")
# Tripwire for the span matcher, not a rule about document size: if
# AGENTS.md is legitimately trimmed below this, lower the floor.
MIN_AGENTS_MD_PATH_REFS = 50
# Trailing punctuation/anchors that can ride along inside a backtick span.
TRIM_SUFFIX_RE = re.compile(r"[),.;:]+$")


def _run_git(args, check=True):
    return subprocess.run(
        ["git"] + args, cwd=ROOT, capture_output=True, text=True, check=check
    )


def _is_gitignored(path):
    """True if git would ignore this path (e.g. a generated file/dir).

    Used instead of a plain filesystem exists() check: a generated file like
    debian/changelog can be present in one developer's tree from a past
    local build (making exists() pass by accident there) while being absent
    from every clean checkout, including CI's. A path git ignores is
    expected to be absent and isn't a documentation error.
    """
    return _run_git(["check-ignore", "-q", path], check=False).returncode == 0


def check_agents_md_paths_exist():
    failures = []
    text = FENCED_BLOCK_RE.sub("", read("AGENTS.md"))
    text = DOUBLE_BACKTICK_SPAN_RE.sub("", text)
    # With fences gone every backtick must pair up; one stray backtick would
    # invert every span after it, and the count floor below only catches a
    # large inversion. Fail loudly on parity instead.
    if text.count("`") % 2 != 0:
        return ["AGENTS.md has an unbalanced backtick outside fenced blocks; "
                "the path-reference check cannot pair code spans reliably"]
    seen = set()

    tracked = set(_run_git(["ls-files"]).stdout.splitlines())
    tracked_dirs = set()
    for f in tracked:
        parts = f.split("/")
        for i in range(1, len(parts)):
            tracked_dirs.add("/".join(parts[:i]) + "/")

    checked = 0
    for m in CODE_SPAN_RE.finditer(text):
        span = m.group(1)
        if "\n" in span or not span.startswith(PATH_PREFIXES):
            continue
        checked += 1
        path = TRIM_SUFFIX_RE.sub("", span)
        # Markdown anchors / fragments (`docs/x.md#section`), glob patterns,
        # and template placeholders (`AlpacaCore/src/vendors/<vendor>/...`)
        # aren't real filesystem paths.
        if "#" in path or "*" in path or "<" in path or ">" in path:
            continue
        if path in seen:
            continue
        seen.add(path)

        if path in tracked or path in tracked_dirs:
            continue
        # A span with whitespace is validated only when it names a tracked
        # file or directory verbatim (e.g. `AlpacaCore/conformu/Astroasis/
        # Oasis Focuser/`, handled above). Otherwise it is SKIPPED, not
        # failed: it may be prose (`AlpacaCore/tests/ and AlpacaHTTP/`) and
        # cannot be told apart from a drifted spaced path. So a spaced path
        # that later drifts stays green here; that is the accepted trade.
        if any(ch.isspace() for ch in path):
            continue
        # Not a tracked file or the directory of one: a generated/ignored
        # path (debian/changelog, a `.../build/` output dir) is expected to
        # be absent from a clean checkout, so it isn't a documentation error.
        if _is_gitignored(path):
            continue
        failures.append("AGENTS.md references a path that does not exist: %s" % path)
    if checked < MIN_AGENTS_MD_PATH_REFS:
        failures.append(
            "only %d backticked path references found in AGENTS.md (floor %d): "
            "either the code-span matcher regressed, or AGENTS.md was trimmed "
            "and MIN_AGENTS_MD_PATH_REFS should be lowered"
            % (checked, MIN_AGENTS_MD_PATH_REFS))
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
