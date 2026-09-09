#!/usr/bin/env python3
"""Fail a PR that adds or touches a failing ConformU report.

`/submit-pr` already refuses to open a PR whose ConformU report is unclean
(see `.claude/commands/submit-pr.md`, "ConformU report validation"), but that
is a prompt-only gate: a PR opened by hand, or from a fork not using the
skill, bypasses it entirely. This script ports the same jq/grep logic into a
plain CI gate so a merged PR can never misadvertise a driver as validated.

Run from the repo root, against a specific base ref to diff from:
    python3 scripts/check_conformu_reports.py <base-ref>

Only files under AlpacaCore/conformu/ that changed relative to <base-ref> are
checked -- an unrelated PR that never touches a report is a no-op. That
mirrors the scope `/submit-pr` uses (`AlpacaCore/conformu/**`); the stray
`AlpacaHTTP/conformu/` directory is dead weight left over from a historic
subtree import (unreferenced anywhere else in the repo) and out of scope
here.

Pass criteria (identical to `/submit-pr`):
  - JSON reports (*.json): ErrorCount, IssueCount and TimingIssuesCount must
    all be zero.
  - Text logs (*.txt): must NOT contain a line matching
    "OUTSIDE (FAST|STANDARD|EXTENDED) RESPONSE TIME TARGET" or
    "took longer than its target response time", AND MUST contain the line
    "Congratulations, no errors, warnings or issues found".
"""

import json
import re
import subprocess
import sys

CONFORMU_PREFIX = "AlpacaCore/conformu/"

TIMING_OUTSIDE_RE = re.compile(r"OUTSIDE (FAST|STANDARD|EXTENDED) RESPONSE TIME TARGET")
TIMING_LONGER_RE = re.compile(r"took longer than its target response time")
SUCCESS_LINE = "Congratulations, no errors, warnings or issues found"


def changed_conformu_files(base_ref):
    merge_base = subprocess.run(
        ["git", "merge-base", base_ref, "HEAD"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    out = subprocess.run(
        ["git", "diff", "--name-only", "--diff-filter=d", merge_base, "HEAD"],
        check=True, capture_output=True, text=True,
    ).stdout
    return [p for p in out.splitlines() if p.startswith(CONFORMU_PREFIX)
            and p.endswith((".json", ".txt"))]


def check_json(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, json.JSONDecodeError) as e:
        return ["%s: could not parse as JSON (%s)" % (path, e)]

    failures = []
    for field in ("ErrorCount", "IssueCount", "TimingIssuesCount"):
        value = data.get(field, 0)
        if value:
            failures.append("%s: %s=%s (must be 0)" % (path, field, value))
    return failures


def check_text(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError as e:
        return ["%s: could not read file (%s)" % (path, e)]

    failures = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        if TIMING_OUTSIDE_RE.search(line):
            failures.append("%s:%d: %s" % (path, lineno, line.strip()))
        elif TIMING_LONGER_RE.search(line):
            failures.append("%s:%d: %s" % (path, lineno, line.strip()))
    if SUCCESS_LINE not in text:
        failures.append(
            "%s: missing required line %r -- report does not confirm a "
            "clean ASCOM validation pass" % (path, SUCCESS_LINE)
        )
    return failures


def main():
    if len(sys.argv) != 2:
        print("usage: check_conformu_reports.py <base-ref>", file=sys.stderr)
        return 2
    base_ref = sys.argv[1]

    files = changed_conformu_files(base_ref)
    if not files:
        print("No ConformU report files changed relative to %s -- nothing to check." % base_ref)
        return 0

    failures = []
    for path in files:
        if path.endswith(".json"):
            failures.extend(check_json(path))
        else:
            failures.extend(check_text(path))

    if failures:
        print("ConformU report validation failed:\n")
        for f in failures:
            print("  " + f)
        print(
            "\n%d finding(s). The driver is not validated. Fix the driver, "
            "re-run ConformU until clean, and replace the report on this "
            "branch. A failing ConformU report must never merge -- see "
            "AGENTS.md and /driver-build Step 10." % len(failures)
        )
        return 1

    print("ConformU report validation OK -- %d file(s) checked: %s"
          % (len(files), ", ".join(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
