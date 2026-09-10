#!/usr/bin/env python3
"""Fail a PR that adds or touches a failing ConformU report.

`/submit-pr` already refuses to open a PR whose ConformU report is unclean
(see `.claude/commands/submit-pr.md`, "ConformU report validation"), but that
is a prompt-only gate: a PR opened by hand, or from a fork not using the
skill, bypasses it entirely. This script ports the same jq/grep logic into a
plain CI gate so a merged PR can never misadvertise a driver as validated.

Run from the repo root, against a specific base ref to diff from:
    python3 scripts/check_conformu_reports.py <base-ref>

Files under BOTH AlpacaCore/conformu/ and AlpacaHTTP/conformu/ that changed
relative to <base-ref> are checked -- an unrelated PR that never touches a
report is a no-op. `/submit-pr`'s own scope is only `AlpacaCore/conformu/**`,
but AlpacaHTTP/conformu/README.md states "All HTTP endpoints must pass
ConformU protocol verification before being considered compliant" and that
directory holds a real report -- a review of this script correctly pointed
out that calling it dead and excluding it would leave exactly the kind of
unguarded gap this script exists to close, so both paths are in scope.

The two directories' reports come from different ConformU check types (a
per-device driver check vs. a protocol-level check across all endpoints) and
use different success wording -- SUCCESS_PATTERNS below covers both observed
phrasings; if ConformU ever changes its wording again, add the new phrase
here rather than looping this check.

Pass criteria (identical to `/submit-pr`):
  - JSON reports (*.json): ErrorCount, IssueCount and TimingIssuesCount must
    all be zero.
  - Text logs (*.txt): must NOT contain a line matching
    "OUTSIDE (FAST|STANDARD|EXTENDED) RESPONSE TIME TARGET" or
    "took longer than its target response time", AND MUST contain one of the
    SUCCESS_PATTERNS strings.
"""

import json
import re
import subprocess
import sys

CONFORMU_PREFIXES = ("AlpacaCore/conformu/", "AlpacaHTTP/conformu/")

TIMING_OUTSIDE_RE = re.compile(r"OUTSIDE (FAST|STANDARD|EXTENDED) RESPONSE TIME TARGET")
TIMING_LONGER_RE = re.compile(r"took longer than its target response time")
# Both are real, observed ConformU pass phrasings (see the module docstring).
SUCCESS_PATTERNS = (
    "Congratulations, no errors, warnings or issues found",
    "Congratulations there were no errors, issues or information alerts",
)


def changed_conformu_files(base_ref):
    merge_base = subprocess.run(
        ["git", "merge-base", base_ref, "HEAD"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    out = subprocess.run(
        ["git", "diff", "--name-only", "--diff-filter=d", merge_base, "HEAD"],
        check=True, capture_output=True, text=True,
    ).stdout
    return [p for p in out.splitlines() if p.startswith(CONFORMU_PREFIXES)
            and p.endswith((".json", ".txt"))]


def check_json(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, json.JSONDecodeError) as e:
        return ["%s: could not parse as JSON (%s)" % (path, e)]

    if not isinstance(data, dict):
        return ["%s: top-level JSON is a %s, not an object -- cannot check "
                "ErrorCount/IssueCount/TimingIssuesCount" % (path, type(data).__name__)]

    failures = []
    for field in ("ErrorCount", "IssueCount", "TimingIssuesCount"):
        # No default: a report missing one of these fields entirely (e.g.
        # truncated/malformed) is a hard failure, not a silent pass -- the
        # whole point of this script is to never let an unclear report
        # through.
        if field not in data:
            failures.append("%s: missing required field %r" % (path, field))
            continue
        value = data[field]
        # Explicit numeric comparison, not truthiness: a real ConformU count
        # is always an int, but `if value:` would wrongly pass a JSON `false`
        # or `null` and wrongly fail a numeric-looking string like "0".
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            failures.append("%s: %s=%r is not a number (must be the integer 0)" % (path, field, value))
        elif value != 0:
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
    if not any(p in text for p in SUCCESS_PATTERNS):
        failures.append(
            "%s: missing a required success line (looked for any of %r) -- "
            "report does not confirm a clean ASCOM validation pass" % (path, SUCCESS_PATTERNS)
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
