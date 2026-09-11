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
  5. The QHYSDK seam's three parallel lists agree: every pure virtual on the
     interface has a LockedQHYSDK override, every override actually takes the
     mutex, and the forward sweep in test_qhy_fake_sdk.cpp drives all of them.
  6. Every relative path referenced in AGENTS.md's inline code spans
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


# --- check 5: the QHYSDK seam's three parallel lists ------------------------
#
# issue #394. The forward sweep in test_qhy_fake_sdk.cpp drives every QHYSDK
# method through LockedQHYSDK and asserts each landed on its own counterpart
# exactly once, which is a genuine (mutation-verified) guard against a
# TRANSPOSED forward. It is not a guard against an ABSENT one: the method list
# is hand-written in the test and closed with `methods.size() == N`, a literal
# compared to a literal. Add a pure virtual to QHYSDK and the compiler forces a
# LockedQHYSDK override -- the class would otherwise be abstract -- but nothing
# forces a test entry, so the sweep passes having exercised N of N+1 forwards.
#
# And the compiler only guarantees the forward EXISTS. Nothing guarantees it
# takes the mutex, which is the only reason the decorator exists: its job is to
# keep ThreadSanitizer findings pointing at driver code rather than at the
# deliberately unhardened fake, and one unlocked forward makes the fake racy
# under a storm and produces a TSan report naming the fake -- the exact
# confusion the decorator was built to prevent, arriving silently.

QHY_INTERFACE_HEADER = "AlpacaCore/include/alpacacore/vendor/qhy/qhy_sdk_wrapper.h"
QHY_LOCKED_HEADER = "AlpacaCore/tests/locked_qhy_sdk.h"
QHY_SWEEP_TEST = "AlpacaCore/tests/test_qhy_fake_sdk.cpp"

PURE_VIRTUAL_RE = re.compile(r"\bvirtual\b[^;{}]*?(\w+)\s*\([^;{}]*\)\s*=\s*0\s*;", re.S)
OVERRIDE_RE = re.compile(r"(\w+)\s*\([^;{}]*\)\s*override\s*\{", re.S)
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")


def _strip_comments(text):
    """Comments blanked (newlines kept). These headers carry long doc comments
    whose prose contains parentheses and identifiers, and both patterns above
    scan across whitespace -- without this a sentence in a comment is matched
    as a method signature. No raw string literals exist in either header."""
    text = BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return LINE_COMMENT_RE.sub("", text)


def _matching_brace(text, open_index):
    """Index just past the `}` closing the `{` at open_index."""
    depth = 0
    for i in range(open_index, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
    return len(text)


def _class_body(text, class_name):
    """The text between `class <name> ... {` and its matching brace, or None."""
    match = re.search(r"\bclass\s+%s\b[^{;]*\{" % re.escape(class_name), text)
    if not match:
        return None
    return text[match.end():_matching_brace(text, match.end() - 1) - 1]


def check_qhy_seam_lists():
    failures = []
    interface_body = _class_body(_strip_comments(read(QHY_INTERFACE_HEADER)), "QHYSDK")
    locked_body = _class_body(_strip_comments(read(QHY_LOCKED_HEADER)), "LockedQHYSDK")
    if interface_body is None or locked_body is None:
        return ["Could not locate class QHYSDK and/or class LockedQHYSDK -- this check's parser is broken."]

    interface_methods = set(PURE_VIRTUAL_RE.findall(interface_body))
    if not interface_methods:
        return ["No pure virtuals found on QHYSDK -- this check's parser is broken."]

    # Each override, with its body, so the lock can be checked too.
    locked_methods = {}
    for match in OVERRIDE_RE.finditer(locked_body):
        open_index = match.end() - 1
        locked_methods[match.group(1)] = locked_body[open_index:_matching_brace(locked_body, open_index)]

    for name in sorted(interface_methods - set(locked_methods)):
        failures.append(
            "QHYSDK::%s() has no LockedQHYSDK override. (If this fires, the parser in %s is wrong: an "
            "unimplemented pure virtual would make LockedQHYSDK abstract and fail the build.)"
            % (name, Path(__file__).name))
    for name in sorted(set(locked_methods) - interface_methods):
        failures.append(
            "LockedQHYSDK::%s() overrides nothing on QHYSDK -- stale forward, or the interface lost a "
            "method." % name)

    for name in sorted(set(locked_methods) & interface_methods):
        if "locked(" not in locked_methods[name]:
            failures.append(
                "UNLOCKED FORWARD: LockedQHYSDK::%s() does not go through locked(). The decorator exists "
                "only to take the mutex -- an unlocked forward makes the fake racy under a [stress] storm "
                "and produces a ThreadSanitizer report naming the FAKE, which is the confusion the "
                "decorator was built to prevent." % name)

    # The hand-written sweep list in the test.
    sweep = read(QHY_SWEEP_TEST)
    list_match = re.search(r"const std::vector<std::string> methods\{(.*?)\};", sweep, re.S)
    if not list_match:
        failures.append(
            "Could not find the `const std::vector<std::string> methods{...}` sweep list in %s."
            % QHY_SWEEP_TEST)
        return failures
    swept = set(re.findall(r'"([^"]+)"', list_match.group(1)))
    for name in sorted(interface_methods - swept):
        failures.append(
            "NOT SWEPT: QHYSDK::%s() is not in the forward sweep's method list in %s. The sweep is what "
            "checks the forward reaches its own counterpart; a method missing from the list is verified "
            "by inspection only." % (name, QHY_SWEEP_TEST))
    for name in sorted(swept - interface_methods):
        failures.append(
            "STALE SWEEP ENTRY: %s is in the sweep list in %s but is not a QHYSDK method."
            % (name, QHY_SWEEP_TEST))
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

    # core.quotePath=false: a tracked path with non-ASCII bytes must not
    # come back quoted, or it would never match a span.
    tracked = set(_run_git(["-c", "core.quotePath=false", "ls-files"]).stdout.splitlines())
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
    ("QHY SDK seam lists agree (interface / LockedQHYSDK / sweep)", check_qhy_seam_lists),
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
